#!/usr/bin/env python3
"""Exercise real AlgoLens writes -> PostgreSQL -> C++ -> CSV/email rendering.

This entrypoint accepts NO database URL. It creates its own uniquely named
Docker container with network=none, reaches PostgreSQL only through an owned
Unix socket, and removes that exact container on exit. It never imports the
default Flask app, reads .env, calls an email transport, or tests SMTP.
"""
import argparse
import csv
from datetime import date, datetime, time as day_time, timedelta, timezone
import hashlib
from html.parser import HTMLParser
import json
import os
from pathlib import Path
import re
import select
import shutil
import socket
import subprocess
import sys
import time
import uuid


class TableRows(HTMLParser):
    def __init__(self):
        super().__init__()
        self.rows = []
        self.row = None
        self.cell = None

    def handle_starttag(self, tag, attrs):
        if tag == 'tr':
            self.row = []
        elif tag in ('td', 'th') and self.row is not None:
            self.cell = []

    def handle_data(self, text):
        if self.cell is not None:
            self.cell.append(text)

    def handle_endtag(self, tag):
        if tag in ('td', 'th') and self.cell is not None:
            self.row.append(''.join(self.cell).strip())
            self.cell = None
        elif tag == 'tr' and self.row is not None:
            self.rows.append(self.row)
            self.row = None


class StrategyTableRows(HTMLParser):
    """Extract position rows with the strategy heading that owns the table."""

    POSITION_HEADERS = ('Symbol', 'Quantity', 'Market Price', 'Notional', '% of Total')
    SYMBOL = re.compile(r'[A-Z0-9]{1,10}\.v\.\d+')
    NUMBER = re.compile(r'-?\d[\d,]*(?:\.\d+)?')
    CURRENCY = re.compile(r'\$-?\d[\d,]*(?:\.\d+)?')
    PERCENT = re.compile(r'-?\d[\d,]*(?:\.\d+)?%')

    def __init__(self):
        super().__init__()
        self.current_strategy = None
        self.heading = None
        self.row = None
        self.cell = None
        self.row_has_td = False
        self.positions = {}
        self.headers = {}

    def handle_starttag(self, tag, attrs):
        if tag == 'h3':
            self.heading = []
        elif tag == 'tr':
            self.row = []
            self.row_has_td = False
        elif tag in ('td', 'th') and self.row is not None:
            self.cell = []
            if tag == 'td':
                self.row_has_td = True

    def handle_data(self, text):
        if self.heading is not None:
            self.heading.append(text)
        if self.cell is not None:
            self.cell.append(text)

    def handle_endtag(self, tag):
        if tag == 'h3' and self.heading is not None:
            self.current_strategy = ''.join(self.heading).strip()
            self.heading = None
        elif tag in ('td', 'th') and self.cell is not None:
            self.row.append(''.join(self.cell).strip())
            self.cell = None
        elif tag == 'tr' and self.row is not None:
            if self.row_has_td:
                if not self.current_strategy:
                    raise AssertionError('position data row has no strategy heading')
                if (len(self.row) != 5 or
                        not self.SYMBOL.fullmatch(self.row[0]) or
                        not self.NUMBER.fullmatch(self.row[1]) or
                        not self.CURRENCY.fullmatch(self.row[2]) or
                        not self.CURRENCY.fullmatch(self.row[3]) or
                        not self.PERCENT.fullmatch(self.row[4])):
                    raise AssertionError(
                        f'nonconforming position data row for {self.current_strategy!r}: '
                        f'{self.row!r}')
                key = (self.current_strategy, self.row[0])
                if key in self.positions:
                    raise AssertionError(f'duplicate HTML position row: {key!r}')
                self.positions[key] = float(self.row[1].replace(',', ''))
            elif self.current_strategy and tuple(self.row) == self.POSITION_HEADERS:
                self.headers[self.current_strategy] = tuple(self.row)
            self.row = None


class UnsafeCertificationInput(RuntimeError):
    """Raised before side effects when live/transport configuration is present."""


FORBIDDEN_ENVIRONMENT_KEYS = frozenset({
    'DATABASE_URL', 'PRODUCTION_DATABASE_URL', 'QT_PIPELINE_TEST_DSN',
    'SMTP_HOST', 'SMTP_PORT', 'SMTP_URL', 'SMTP_USERNAME', 'SMTP_PASSWORD',
    'EMAIL_RECIPIENTS', 'EMAIL_TO', 'SENDGRID_API_KEY', 'MAILGUN_API_KEY',
})


def validate_invocation_environment(environ):
    """Reject caller-supplied production or email configuration by key only.

    Values are intentionally never read: they may be credentials.  The harness
    creates its disposable DSN after this gate and passes a strict allow-list of
    variables to every child process.
    """
    present = sorted(FORBIDDEN_ENVIRONMENT_KEYS.intersection(environ.keys()))
    if present:
        raise UnsafeCertificationInput(
            'Unsafe certification environment key(s): ' + ', '.join(present))


def require_isolated_network_namespace(*, interface_provider=None,
                                       ipv4_route_path=Path('/proc/net/route')):
    """Require an empty Linux network namespace before any side effect.

    Comparing against PID 1 is not reliable inside a nested user namespace:
    procfs can deny cross-namespace metadata reads.  The security property we
    need is directly observable here: exactly the loopback device and no IPv4
    routes. With no external interface, IPv6 cannot provide external egress.
    """
    if os.name != 'posix':
        raise UnsafeCertificationInput('QT certification requires a POSIX network namespace')
    try:
        current = os.readlink('/proc/self/ns/net')
        interfaces = sorted(name for _index, name in
                            (interface_provider or socket.if_nameindex)())
        route_lines = [line for line in Path(ipv4_route_path).read_text(
            encoding='ascii').splitlines()[1:] if line.strip()]
    except (OSError, ValueError) as exc:
        raise UnsafeCertificationInput('Cannot verify the isolated network namespace') from exc
    if interfaces != ['lo']:
        raise UnsafeCertificationInput(
            'QT certification network namespace must have only interface lo')
    if route_lines:
        raise UnsafeCertificationInput(
            'QT certification network namespace must have no IPv4 routes')
    return {'current': current, 'interfaces': interfaces,
            'ipv4_route_count': len(route_lines)}


def parse_csv_positions(text):
    rows = csv.DictReader(line for line in text.splitlines()
                          if line.strip() and not line.startswith('#'))
    positions = {}
    for row in rows:
        key = (row['strategy'], row['symbol'])
        if key in positions:
            raise AssertionError(f'duplicate CSV position row: {key!r}')
        positions[key] = float(row['quantity'])
    return positions


def parse_html_positions(text):
    parser = StrategyTableRows()
    parser.feed(text)
    return parser.positions


def parse_html_position_layout(text):
    parser = StrategyTableRows()
    parser.feed(text)
    return parser.headers


def assert_position_quantities(actual, expected, label):
    if actual != expected:
        raise AssertionError(f'{label} position identity/quantity mismatch: '
                             f'expected {expected!r}, got {actual!r}')


def _csv_non_position(text):
    lines = text.splitlines(keepends=True)
    for index, line in enumerate(lines):
        if line.rstrip('\r\n').startswith('strategy,symbol,quantity'):
            return ''.join(lines[:index + 1])
    raise AssertionError('CSV did not contain the position header')


def _html_static_report(text):
    start_marker = "<h2>Today's Positions</h2>"
    end_pattern = re.compile(r'<div class=["\']metrics-section["\']>')
    start = text.find(start_marker)
    match = end_pattern.search(text, start + len(start_marker)) if start >= 0 else None
    if start < 0 or match is None:
        raise AssertionError('HTML did not contain the bounded positions section')
    position_section = text[start:match.start()]
    # Validate every <td> row before removal. This fails closed on banners,
    # colspans, arbitrary tables, malformed numeric cells, rows outside a
    # strategy heading, and duplicate strategy/symbol identities.
    validator = StrategyTableRows()
    validator.feed(position_section)
    # A position row may legitimately be added, removed, or have every cell
    # changed. Remove only data rows; keep table markup, headings, column
    # headers, labels, styles, and arbitrary prose byte-for-byte comparable.
    position_section = re.sub(
        r'<tr>\s*<td\b.*?</tr>\s*', '', position_section,
        flags=re.IGNORECASE | re.DOTALL)
    # These values are derived from the dynamic position rows. Mask only their
    # text values while preserving the exact strong labels and surrounding
    # markup/delimiters. No other content inside the section is ignored.
    quantity_labels = (
        'Positions|Notional|Margin|Active Positions|Total Notional|'
        'Total Margin Posted')
    position_section = re.sub(
        rf'(<strong>(?:{quantity_labels}):</strong>\s*)'
        rf'(\$?-?\d[\d,]*(?:\.\d+)?)',
        rf'\1<!-- QUANTITY_DEPENDENT_VALUE -->', position_section,
        flags=re.IGNORECASE)
    return text[:start] + position_section + text[match.start():]


def assert_non_position_unchanged(before_csv, before_html, after_csv, after_html):
    if _csv_non_position(before_csv) != _csv_non_position(after_csv):
        raise AssertionError('non-position CSV metadata/header changed')
    if _html_static_report(before_html) != _html_static_report(after_html):
        raise AssertionError('static HTML changed outside position rows/quantity values')


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def wait_for_fifo_signal(process, fifo_fd, expected, *, timeout):
    """Wait for a FIFO message or process exit, whichever happens first."""
    if not hasattr(os, 'pidfd_open'):
        raise RuntimeError('pidfd_open is required for bounded snapshot synchronization')
    pid_fd = os.pidfd_open(process.pid)
    try:
        readable, _, _ = select.select([fifo_fd, pid_fd], [], [], timeout)
        if fifo_fd in readable:
            message = os.read(fifo_fd, 4096).decode('utf-8').strip()
            if message != expected:
                raise RuntimeError(
                    f'invalid FIFO signal: expected {expected!r}, got {message!r}')
            return
        if pid_fd in readable:
            _stdout, stderr = process.communicate(timeout=1)
            raise RuntimeError(
                f'probe exited before FIFO signal (status {process.returncode}): '
                f'{stderr.strip()}')
        raise TimeoutError(f'timed out waiting {timeout}s for FIFO signal')
    finally:
        os.close(pid_fd)


def write_evidence_manifest(path, *, scope, report_date, checks, artifacts):
    """Write conservative machine-readable evidence without send claims."""
    manifest = {
        'schema_version': 1,
        'scope': scope,
        'report_date': str(report_date),
        'checks': checks,
        'artifacts': [
            {'path': str(Path(item).resolve()), 'sha256': sha256_file(item)}
            for item in artifacts
        ],
        'email_send_attempts': {
            'status': 'unavailable',
            'count': None,
            'reason': (
                'The certification executes only the HTML renderer and has no '
                'reliable transport-attempt telemetry; send_email is never called.'
            ),
        },
        'smtp_tested': False,
        'delivery_verified': False,
    }
    Path(path).write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n',
                          encoding='utf-8')


def command(args, **kwargs):
    # Always use the local Docker daemon, even if the shell selected a remote
    # context. No caller credentials or service endpoints go into the harness.
    if args[0] == 'docker':
        args = ['docker', '--host', 'unix:///var/run/docker.sock', *args[1:]]
        kwargs['env'] = {key: os.environ[key] for key in ('PATH', 'HOME', 'DOCKER_CONFIG')
                         if key in os.environ}
    kwargs.setdefault('check', True)
    return subprocess.run(args, capture_output=True, text=True, **kwargs)


def main():
    # This gate precedes argument path resolution, output creation, Docker, and
    # application imports. It checks names only and never reads secret values.
    validate_invocation_environment(os.environ)
    network_namespace = require_isolated_network_namespace()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--algolens-root', required=True, type=Path)
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--output-dir', required=True, type=Path)
    args = parser.parse_args()
    probe = args.probe.resolve(strict=True)
    api = (args.algolens_root / 'algolens-api').resolve(strict=True)
    test_id = uuid.uuid4().hex[:12]
    output = args.output_dir.resolve() / ('qt-pipeline-' + test_id)
    output.mkdir(parents=True, exist_ok=False)
    docker_config = output / 'empty-docker-config'
    docker_config.mkdir()
    os.environ['DOCKER_CONFIG'] = str(docker_config)
    root = Path(__file__).resolve().parents[2]
    os.environ['TZ'] = 'UTC'
    os.environ.update(PYTHON_DOTENV_DISABLED='1', DB_HOST='127.0.0.1', DB_PORT='1',
                      DB_NAME='qt_test_unused', DB_USER='qt_test', DB_PASSWORD='qt_test')
    time.tzset()
    sys.path.insert(0, str(api))
    import psycopg2
    import psycopg2.extras
    from flask import Flask
    from flask_jwt_extended import JWTManager, create_access_token, get_csrf_token
    import algolens.adapters.http.portfolio as portfolio_http
    from algolens.domain.identity.models import User
    from algolens.infrastructure.portfolio.repositories import PostgresPortfolioRepository
    from algolens.infrastructure.portfolio.strategy_registry import PostgresStrategyRegistry

    # Application dependencies are imported without app startup. All DB calls
    # below receive this explicit, newly created container's factory.
    name = 'qt-pipeline-test-' + test_id
    database = 'qt_pipeline_test_' + test_id
    password = 'disposable-' + test_id
    container = None
    disposable_root = Path('/tmp') / database
    disposable_root.mkdir(mode=0o700)
    socket_dir = disposable_root / 'socket'
    socket_dir.mkdir(mode=0o777)
    checks = 0
    check_results = []

    def check(condition, message):
        nonlocal checks
        if not condition:
            raise AssertionError(message)
        checks += 1
        check_results.append({'name': message, 'passed': True})
        print('PASS ' + message, flush=True)

    try:
        check(network_namespace['interfaces'] == ['lo'] and
              network_namespace['ipv4_route_count'] == 0,
              'process namespace has only loopback and no IPv4 routes')
        container = command([
            'docker', 'run', '-d', '--name', name,
            '--label', 'purpose=qt-pipeline-disposable-test',
            '--network', 'none', '--pull', 'never',
            '-e', 'POSTGRES_DB=' + database, '-e', 'POSTGRES_PASSWORD=' + password,
            '--mount', 'type=bind,source=' + str(socket_dir) + ',target=/var/run/postgresql',
            'postgres:16-alpine', '-c', 'listen_addresses=',
            '-c', 'unix_socket_directories=/var/run/postgresql',
        ]).stdout.strip()
        dsn = ('host=' + str(socket_dir) + ' port=5432' +
               ' dbname=' + database + ' user=postgres password=' + password +
               ' connect_timeout=5')
        deadline = time.monotonic() + 30
        while True:
            try:
                with psycopg2.connect(dsn) as connection:
                    break
            except psycopg2.OperationalError:
                if time.monotonic() >= deadline:
                    raise RuntimeError('Disposable PostgreSQL did not become ready') from None
                time.sleep(0.2)
        connection.close()

        def factory():
            return psycopg2.connect(dsn, cursor_factory=psycopg2.extras.RealDictCursor)

        def sql(statement, parameters=None, fetch=False):
            connection = factory()
            try:
                with connection:
                    with connection.cursor() as cursor:
                        cursor.execute(statement, parameters)
                        return cursor.fetchall() if fetch else None
            finally:
                connection.close()

        sql('''
            CREATE SCHEMA trading;
            CREATE TABLE trading.positions (
                symbol VARCHAR NOT NULL, quantity NUMERIC NOT NULL,
                average_price NUMERIC NOT NULL CHECK (average_price >= 0),
                daily_unrealized_pnl NUMERIC NOT NULL, daily_realized_pnl NUMERIC NOT NULL,
                last_update TIMESTAMPTZ NOT NULL, updated_at TIMESTAMPTZ DEFAULT now(),
                strategy_id VARCHAR NOT NULL, strategy_name VARCHAR NOT NULL,
                date DATE NOT NULL, portfolio_id VARCHAR NOT NULL,
                portfolio_type TEXT NOT NULL DEFAULT 'system' CHECK (portfolio_type IN ('system','qt')),
                PRIMARY KEY (portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type));
            CREATE TABLE trading.strategy_registry (
                id TEXT PRIMARY KEY, strategy_type TEXT NOT NULL, portfolio_id TEXT NOT NULL,
                name TEXT NOT NULL, description TEXT, initial_equity NUMERIC, managers TEXT[],
                is_active BOOLEAN DEFAULT TRUE, lifecycle TEXT DEFAULT 'live', sort_order INT DEFAULT 0,
                mock_capital NUMERIC, incubation_started_at TIMESTAMPTZ, updated_at TIMESTAMPTZ DEFAULT now());
            CREATE TABLE trading.strategy_book_memberships (
                strategy_id TEXT NOT NULL, portfolio_id TEXT NOT NULL, added_by TEXT,
                added_at TIMESTAMPTZ DEFAULT now(), PRIMARY KEY(strategy_id,portfolio_id));
            CREATE TABLE trading.live_results (
                portfolio_id TEXT, config JSONB, date DATE, current_portfolio_value NUMERIC,
                total_annualized_return NUMERIC, volatility NUMERIC, total_cumulative_return NUMERIC,
                strategy_id TEXT);
            CREATE TABLE trading.equity_curve (strategy_id TEXT,portfolio_id TEXT);
            CREATE TABLE trading.executions (strategy_id TEXT,portfolio_id TEXT);
            CREATE TABLE trading.signals (strategy_id TEXT,portfolio_id TEXT);
            CREATE TABLE trading.live_run_metadata (strategy_id TEXT,portfolio_id TEXT);
            CREATE TABLE trading.run_inputs (strategy_id TEXT,portfolio_id TEXT);
        ''')
        for migration in ('004_position_overrides_audit.sql', '005_risk_limits.sql',
                          '012_position_overrides_portfolio_scope.sql', '013_runtime_control.sql'):
            # These are the actual shipped migrations, not a hand-copied audit schema.
            sql((root / 'migrations' / migration).read_text())
        combined_id = 'LIVE_QT_PIPELINE'
        names = ('TREND_FOLLOWING', 'TREND_FOLLOWING_FAST')
        today = date.today()
        sql('''INSERT INTO trading.strategy_registry
               (id,strategy_type,portfolio_id,name,initial_equity)
               VALUES ('pipeline',%s,'BOOK_A','Pipeline integration',500000);
               INSERT INTO trading.strategy_book_memberships(strategy_id,portfolio_id)
               VALUES ('pipeline','BOOK_A'),('pipeline','BOOK_B');''', (combined_id,))
        for book in ('BOOK_A', 'BOOK_B'):
            sql('''INSERT INTO trading.live_results VALUES
                   (%s,%s::jsonb,%s,500000,0,0,0,%s);
                   INSERT INTO trading.risk_limits(strategy_id,portfolio_id,limits)
                   VALUES (%s,%s,'{"max_symbol_position_contracts":{"ES.v.0":100}}');''',
                (book, json.dumps({'strategy_type': combined_id}), today, combined_id, combined_id, book))

        def add_system(book, name, symbol, quantity, on_date=today, strategy=combined_id):
            sql('''INSERT INTO trading.positions
                (portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,
                 quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
                VALUES (%s,%s,%s,%s,%s,'system',%s,100,0,0,%s);''',
                (book, strategy, name, on_date, symbol, quantity, on_date))

        add_system('BOOK_A', names[0], 'ES.v.0', 12)
        add_system('BOOK_A', names[0], 'NG.v.0', -2)
        add_system('BOOK_A', names[1], 'ES.v.0', -3)
        add_system('BOOK_B', names[0], 'ES.v.0', 77)

        evidence_artifacts = []

        def probe_environment(probe_dsn):
            child = {key: os.environ[key] for key in
                     ('PATH', 'LD_LIBRARY_PATH', 'DYLD_LIBRARY_PATH', 'LANG', 'LC_ALL')
                     if key in os.environ}
            # At 00:30 UTC this zone is still on the prior date. The DB lookup
            # must nevertheless use the explicit UTC report date. No mail,
            # provider, HOME, PG*, or application configuration is inherited.
            child.update(QT_PIPELINE_TEST_DSN=probe_dsn, TZ='America/New_York')
            return child

        def parse_probe_result(result, destination, label, expect_ok):
            (destination / 'probe.log').write_text(result.stdout + result.stderr,
                                                    encoding='utf-8')
            if not expect_ok:
                check(result.returncode != 0, label + ' blocks report output')
                check(not list(destination.glob('*.csv')) and not list(destination.glob('*.html')),
                      label + ' creates neither report artifact')
                return None
            if result.returncode:
                raise AssertionError('C++ probe failed; see ' + str(destination / 'probe.log'))
            lines = [line.partition('=')[2] for line in result.stdout.splitlines()
                     if line.startswith('QT_PIPELINE_RESULT=')]
            if len(lines) != 1:
                raise AssertionError('C++ probe did not return one result')
            parsed = json.loads(lines[0])
            for key in ('csv_path', 'email_path'):
                if key in parsed:
                    artifact = Path(parsed[key]).resolve()
                    if artifact.parent != destination.resolve() or not artifact.is_file():
                        raise AssertionError(key + ' escaped the owned output directory')
                    evidence_artifacts.append(artifact)
            return parsed

        def run_probe(mode, *, book='BOOK_A', on_date=today, label=None, expect_ok=True,
                      probe_dsn=dsn):
            destination = output / (label or mode)
            destination.mkdir(parents=True, exist_ok=True)
            epoch = int(datetime.combine(on_date, day_time(0, 30), timezone.utc).timestamp())
            result = subprocess.run(
                [str(probe), mode, book, combined_id, str(epoch), str(destination), *names],
                env=probe_environment(probe_dsn), cwd=destination,
                capture_output=True, text=True, timeout=60)
            return parse_probe_result(result, destination, label or mode, expect_ok)

        # The validator must reject arbitrary Unix sockets and mixed
        # socket/hostaddr inputs before libpq can attempt a connection.
        for label, unsafe_dsn in (
            ('reject-arbitrary-socket',
             'host=/tmp/not-owned/socket port=5432 dbname=qt_pipeline_test_bad '
             'user=postgres password=x connect_timeout=1'),
            ('reject-mixed-socket-hostaddr', dsn + ' hostaddr=127.0.0.1'),
        ):
            run_probe('snapshot', label=label, expect_ok=False, probe_dsn=unsafe_dsn)

        run_probe('seed')
        run_probe('seed', book='BOOK_B', label='seed-book-b')
        baseline_report = run_probe('snapshot', label='baseline-report')
        repository = PostgresPortfolioRepository(connection_factory=factory)
        registry = PostgresStrategyRegistry(connection_factory=factory)

        # Register only the real portfolio blueprint. This intentionally does
        # not import app.py or call the default application factory (which reads
        # .env and composes unrelated runtime dependencies).
        portfolio_http.create_portfolio_dependencies = lambda: (registry, repository)
        portfolio_http.create_market_data = lambda: None

        class SyntheticCurrentUsers:
            role = None

            def find_by_id(self, user_id):
                if str(user_id) != '7001':
                    return None
                return User(
                    id=7001,
                    email='qt-pipeline@synthetic.invalid',
                    role=self.role,
                )

        synthetic_users = SyntheticCurrentUsers()
        portfolio_http.create_identity_dependencies = lambda: (
            synthetic_users, object(), object())
        app = Flask('qt-no-send-certification')
        app.config.update(
            TESTING=True,
            JWT_SECRET_KEY='synthetic-qt-certification-key-not-for-production',
            JWT_TOKEN_LOCATION=['cookies'],
            JWT_COOKIE_CSRF_PROTECT=True,
            JWT_COOKIE_SECURE=False,
            JWT_ACCESS_COOKIE_PATH='/',
        )
        JWTManager(app)
        app.register_blueprint(portfolio_http.portfolio_bp, url_prefix='/portfolio')
        client = app.test_client()

        def authenticate(role):
            synthetic_users.role = role
            with app.app_context():
                # position_overrides.user_id is an integer FK contract. JWT
                # identities are strings at the HTTP boundary, so use a
                # synthetic numeric string exactly as the real route expects.
                token = create_access_token(identity='7001',
                                            additional_claims={'role': role})
                csrf = get_csrf_token(token)
            client.set_cookie('access_token_cookie', token)
            return csrf

        def post_position(symbol, quantity, *, csrf):
            return client.post('/portfolio/positions', json={
                'strategy_id': 'pipeline', 'portfolio_id': 'BOOK_A',
                'strategy_name': names[0], 'symbol': symbol, 'quantity': quantity,
                'reason': 'Synthetic no-send certification',
            }, headers={'X-CSRF-TOKEN': csrf} if csrf else {})

        unauthenticated = post_position('ES.v.0', 999, csrf=None)
        check(unauthenticated.status_code == 401 and not repository.fetch_overrides(
            combined_id, 'BOOK_A'), 'position route requires an authenticated JWT cookie')
        subscriber_csrf = authenticate('subscriber_professional')
        forbidden = post_position('ES.v.0', 999, csrf=subscriber_csrf)
        check(forbidden.status_code == 403 and not repository.fetch_overrides(
            combined_id, 'BOOK_A'), 'position route enforces the internal role gate')
        admin_csrf = authenticate('admin')
        missing_csrf = post_position('ES.v.0', 999, csrf=None)
        check(missing_csrf.status_code in (401, 422) and not repository.fetch_overrides(
            combined_id, 'BOOK_A'), 'position route enforces cookie CSRF protection')

        for symbol, quantity in (('ES.v.0', 7), ('NG.v.0', 0)):
            response = post_position(symbol, quantity, csrf=admin_csrf)
            check(response.status_code == 201 and
                  float(response.get_json()['position']['quantity']) == quantity,
                  'authenticated AlgoLens POST commits ' + symbol + ' edit')
        history = repository.fetch_overrides(combined_id, 'BOOK_A')
        check(len(history) == 2 and repository.fetch_overrides(combined_id, 'BOOK_B') == [],
              'override history is scoped to the edited book')

        identity_rows = sql('''SELECT portfolio_id,strategy_id,strategy_name,date,symbol,
                                      portfolio_type,quantity
                               FROM trading.positions
                               WHERE portfolio_id='BOOK_A' AND date=%s AND portfolio_type='qt'
                               ORDER BY strategy_name,symbol''', (today,), fetch=True)
        actual_identity = [{**dict(row), 'date': str(row['date']),
                            'quantity': float(row['quantity'])} for row in identity_rows]
        expected_identity = [
            {'portfolio_id': 'BOOK_A', 'strategy_id': combined_id,
             'strategy_name': names[0], 'date': str(today), 'symbol': 'ES.v.0',
             'portfolio_type': 'qt', 'quantity': 7.0},
            {'portfolio_id': 'BOOK_A', 'strategy_id': combined_id,
             'strategy_name': names[0], 'date': str(today), 'symbol': 'NG.v.0',
             'portfolio_type': 'qt', 'quantity': 0.0},
            {'portfolio_id': 'BOOK_A', 'strategy_id': combined_id,
             'strategy_name': names[1], 'date': str(today), 'symbol': 'ES.v.0',
             'portfolio_type': 'qt', 'quantity': -3.0},
        ]
        check(actual_identity == expected_identity,
              'QT database rows match full book/strategy/name/date/symbol/stream identity')
        check(sql("SELECT quantity FROM trading.positions WHERE portfolio_id='BOOK_B' AND portfolio_type='qt'",
                  fetch=True)[0]['quantity'] == 77, 'other book remains unchanged')
        check(sql("SELECT quantity FROM trading.positions WHERE portfolio_id='BOOK_A' AND portfolio_type='system' AND symbol='ES.v.0' AND strategy_name=%s",
                  (names[0],), fetch=True)[0]['quantity'] == 12, 'system proposal remains unchanged')
        run_probe('seed', label='reseed')
        report = run_probe('snapshot', label='edited-report')
        expected = {names[0]: {'ES.v.0': 7}, names[1]: {'ES.v.0': -3}}
        check({key: report[key] for key in
               ('portfolio_id', 'strategy_id', 'strategy_names', 'report_date', 'portfolio_type')} == {
                   'portfolio_id': 'BOOK_A', 'strategy_id': combined_id,
                   'strategy_names': list(names), 'report_date': str(today),
                   'portfolio_type': 'qt'},
              'C++ result records full report scope and QT stream identity')
        check(report['by_strategy'] == expected, 'real C++ reads exact manual quantities and excludes closure')
        check(report['combined'] == {'ES.v.0': 4}, 'combined quantity sums both strategies')
        csv_path = Path(report['csv_path'])
        html_path = Path(report['email_path'])
        csv_text = csv_path.read_text(encoding='utf-8')
        html_text = html_path.read_text(encoding='utf-8')
        rendered_expected = {
            ('Trend Following', 'ES.v.0'): 7.0,
            ('Trend Following Fast', 'ES.v.0'): -3.0,
        }
        assert_position_quantities(parse_csv_positions(csv_text), rendered_expected, 'CSV')
        check(True, 'CSV keys quantities by strategy and symbol and omits the zero closure')
        assert_position_quantities(parse_html_positions(html_text), rendered_expected, 'HTML')
        check(True, 'HTML keys quantities by strategy and symbol and omits the zero closure')
        baseline_csv = Path(baseline_report['csv_path']).read_text(encoding='utf-8')
        baseline_html = Path(baseline_report['email_path']).read_text(encoding='utf-8')
        assert_non_position_unchanged(baseline_csv, baseline_html, csv_text, html_text)
        check(True, 'HTML static content and CSV metadata/header remain byte-stable')
        expected_headers = {
            'Trend Following': ('Symbol', 'Quantity', 'Market Price', 'Notional', '% of Total'),
            'Trend Following Fast': ('Symbol', 'Quantity', 'Market Price', 'Notional', '% of Total'),
        }
        check(parse_html_position_layout(baseline_html) == expected_headers and
              parse_html_position_layout(html_text) == expected_headers,
              'shared strategy table headings and column layout remain unchanged')

        swapped = dict(rendered_expected)
        swapped[('Trend Following', 'ES.v.0')] = -3.0
        swapped[('Trend Following Fast', 'ES.v.0')] = 7.0
        try:
            assert_position_quantities(swapped, rendered_expected, 'CSV mutation')
        except AssertionError:
            check(True, 'mutation check rejects cross-strategy quantity swapping')
        else:
            raise AssertionError('cross-strategy mutation was accepted')
        try:
            assert_non_position_unchanged(
                baseline_csv, baseline_html, csv_text,
                html_text.replace('Generated by AlgoGator', 'Unrelated report mutation'))
        except AssertionError:
            check(True, 'mutation check rejects unrelated report text changes')
        else:
            raise AssertionError('unrelated HTML mutation was accepted')
        check(sql("SELECT quantity FROM trading.positions WHERE portfolio_id='BOOK_A' AND portfolio_type='qt' AND symbol='NG.v.0'",
                  fetch=True)[0]['quantity'] == 0, 'database retains explicit zero closure evidence')

        def run_barrier_probe():
            destination = output / 'snapshot-barrier'
            destination.mkdir()
            ready = destination / 'ready.fifo'
            release = destination / 'release.fifo'
            os.mkfifo(ready, 0o600)
            os.mkfifo(release, 0o600)
            ready_fd = os.open(ready, os.O_RDWR | os.O_NONBLOCK)
            release_fd = os.open(release, os.O_RDWR | os.O_NONBLOCK)
            epoch = int(datetime.combine(today, day_time(0, 30), timezone.utc).timestamp())
            barrier_env = probe_environment(dsn)
            barrier_env.update(QT_PIPELINE_READY_FIFO=str(ready),
                               QT_PIPELINE_RELEASE_FIFO=str(release))
            process = subprocess.Popen(
                [str(probe), 'snapshot-barrier', 'BOOK_A', combined_id, str(epoch),
                 str(destination), *names], env=barrier_env, cwd=destination,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                wait_for_fifo_signal(process, ready_fd, 'snapshot-loaded', timeout=60)
                check(True, 'C++ confirms snapshot load through a bounded FIFO handshake')
                response = post_position('ES.v.0', 9, csrf=admin_csrf)
                check(response.status_code == 201,
                      'harness commits QT=9 only after the snapshot-loaded signal')
                os.write(release_fd, b'render\n')
                stdout, stderr = process.communicate(timeout=60)
            except Exception:
                process.kill()
                process.communicate()
                raise
            finally:
                os.close(ready_fd)
                os.close(release_fd)
            return parse_probe_result(
                subprocess.CompletedProcess(process.args, process.returncode, stdout, stderr),
                destination, 'snapshot-barrier', True)

        barrier_report = run_barrier_probe()
        check(barrier_report['by_strategy'] == expected,
              'captured snapshot keeps QT=7 after concurrent QT=9 commit')
        assert_position_quantities(
            parse_csv_positions(Path(barrier_report['csv_path']).read_text(encoding='utf-8')),
            rendered_expected, 'barrier CSV')
        assert_position_quantities(
            parse_html_positions(Path(barrier_report['email_path']).read_text(encoding='utf-8')),
            rendered_expected, 'barrier HTML')
        check(True, 'barrier CSV and HTML both render QT=7 from one captured snapshot')
        next_snapshot = run_probe('snapshot', label='post-barrier-snapshot')
        expected_nine = {names[0]: {'ES.v.0': 9}, names[1]: {'ES.v.0': -3}}
        check(next_snapshot['by_strategy'] == expected_nine,
              'the next independent snapshot observes committed QT=9')
        check(post_position('ES.v.0', 7, csrf=admin_csrf).status_code == 201,
              'route restores QT=7 before next-day carry-forward checks')
        # A later system day carries the human edit and closure, while exact
        # report-day reads do not leak the rows from another date.
        tomorrow = today + timedelta(days=1)
        add_system('BOOK_A', names[0], 'ES.v.0', 99, tomorrow)
        add_system('BOOK_A', names[1], 'ES.v.0', -10, tomorrow)
        run_probe('seed', on_date=tomorrow, label='seed-next-day')
        next_report = run_probe('snapshot', on_date=tomorrow, label='next-day-report')
        check(next_report['by_strategy'] == expected, 'next day carries manual values and closure')
        check(run_probe('snapshot', label='original-day-report')['by_strategy'] == expected,
              'UTC date lookup stays scoped across a local-midnight boundary')
        add_system('BOOK_A', names[0], 'NQ.v.0', 1)
        run_probe('snapshot', label='missing-qt-evidence', expect_ok=False)
        sql('ALTER TABLE trading.positions DROP COLUMN portfolio_type CASCADE')
        run_probe('snapshot', label='missing-stream-schema', expect_ok=False)
        manifest_path = output / 'evidence-manifest.json'
        write_evidence_manifest(
            manifest_path,
            scope={
                'synthetic_only': True,
                'portfolio_id': 'BOOK_A',
                'strategy_id': combined_id,
                'strategy_names': list(names),
                'report_date': str(today),
                'portfolio_type': 'qt',
                'http_path': 'Flask test client POST /portfolio/positions',
                'browser_ui_executed': False,
                'default_application_started': False,
                'database': database,
                'database_transport': 'owned Unix socket; Docker network=none',
                'network_namespace': network_namespace,
                'html_comparison_scope': (
                    'position data rows removed and only known quantity-derived text values '
                    'masked; every remaining HTML byte, including position-section static '
                    'markup/text, compared for equality'
                ),
            },
            report_date=today,
            checks=check_results,
            artifacts=list(dict.fromkeys(evidence_artifacts)),
        )
        print('QT_PIPELINE_MANIFEST=' + str(manifest_path), flush=True)
        print('QT_PIPELINE_CHECKS=' + str(checks), flush=True)
    finally:
        container_removed = container is None
        if container:
            command(['docker', 'stop', '--time', '10', container], check=False)
            removed = command(['docker', 'rm', '-f', '-v', container], check=False)
            container_removed = removed.returncode == 0
            if container_removed:
                print('DISPOSABLE_CONTAINER_REMOVED', flush=True)
        # Remove only the exact UUID-owned path after verifying its fixed
        # parent/name relationship. Never enumerate or clean containers/dirs by
        # label or prefix.
        if (container_removed and disposable_root.parent == Path('/tmp') and
                disposable_root.name == database and
                disposable_root.name.startswith('qt_pipeline_test_') and
                socket_dir == disposable_root / 'socket'):
            shutil.rmtree(disposable_root)
            print('DISPOSABLE_SOCKET_DIRECTORY_REMOVED', flush=True)
        if not container_removed:
            raise RuntimeError('Exact disposable container could not be removed; socket path retained')


if __name__ == '__main__':
    main()
