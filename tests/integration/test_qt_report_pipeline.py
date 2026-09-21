#!/usr/bin/env python3
"""Exercise real AlgoLens writes -> PostgreSQL -> C++ -> CSV/email rendering.

This entrypoint accepts NO database URL. It creates its own uniquely named
Docker container, exposes it only on loopback, and removes that exact container
on exit. It never imports the Flask app, reads .env, or sends an email.
"""
import argparse
import csv
from datetime import date, datetime, time as day_time, timedelta, timezone
from html.parser import HTMLParser
import json
import os
from pathlib import Path
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


def command(args, **kwargs):
    # Always use the local Docker daemon, even if the shell selected a remote
    # context. No caller credentials or service endpoints go into the harness.
    if args[0] == 'docker':
        args = ['docker', '--host', 'unix:///var/run/docker.sock', *args[1:]]
        kwargs['env'] = {key: os.environ[key] for key in ('PATH', 'HOME') if key in os.environ}
    return subprocess.run(args, check=True, capture_output=True, text=True, **kwargs)


def main():
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
    root = Path(__file__).resolve().parents[2]
    os.environ['TZ'] = 'UTC'
    os.environ.update(PYTHON_DOTENV_DISABLED='1', DB_HOST='127.0.0.1', DB_PORT='1',
                      DB_NAME='qt_test_unused', DB_USER='qt_test', DB_PASSWORD='qt_test')
    time.tzset()
    sys.path.insert(0, str(api))
    import psycopg2
    import psycopg2.extras
    from algolens.application.portfolio.use_cases import UpsertQtPosition
    from algolens.infrastructure.portfolio.repositories import PostgresPortfolioRepository
    from algolens.infrastructure.portfolio.strategy_registry import PostgresStrategyRegistry

    # Application dependencies are imported without app startup. All DB calls
    # below receive this explicit, newly created container's factory.
    name = 'qt-pipeline-test-' + test_id
    database = 'qt_pipeline_test_' + test_id
    password = 'disposable-' + test_id
    container = None
    checks = 0

    def check(condition, message):
        nonlocal checks
        if not condition:
            raise AssertionError(message)
        checks += 1
        print('PASS ' + message, flush=True)

    try:
        container = command([
            'docker', 'run', '--rm', '-d', '--name', name,
            '--label', 'purpose=qt-pipeline-disposable-test',
            '-e', 'POSTGRES_DB=' + database, '-e', 'POSTGRES_PASSWORD=' + password,
            '-p', '127.0.0.1::5432', 'postgres:16-alpine',
        ]).stdout.strip()
        port = command(['docker', 'port', container, '5432/tcp']).stdout.strip()
        if not port.startswith('127.0.0.1:') or '\n' in port:
            raise RuntimeError('Disposable database was not bound only to loopback')
        dsn = ('host=127.0.0.1 port=' + port.rsplit(':', 1)[1] +
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
                total_annualized_return NUMERIC, volatility NUMERIC, total_cumulative_return NUMERIC);
        ''')
        for migration in ('004_position_overrides_audit.sql', '005_risk_limits.sql',
                          '012_position_overrides_portfolio_scope.sql'):
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
                   (%s,%s::jsonb,%s,500000,0,0,0);
                   INSERT INTO trading.risk_limits(strategy_id,portfolio_id,limits)
                   VALUES (%s,%s,'{"max_symbol_position_contracts":{"ES.v.0":100}}');''',
                (book, json.dumps({'strategy_type': combined_id}), today, combined_id, book))

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

        def run_probe(mode, *, book='BOOK_A', on_date=today, label=None, expect_ok=True):
            destination = output / (label or mode)
            destination.mkdir(parents=True, exist_ok=True)
            epoch = int(datetime.combine(on_date, day_time(0, 30), timezone.utc).timestamp())
            probe_env = {key: os.environ[key] for key in
                         ('PATH', 'LD_LIBRARY_PATH', 'DYLD_LIBRARY_PATH', 'LANG', 'LC_ALL')
                         if key in os.environ}
            # At 00:30 UTC this zone is still on the prior date. The DB lookup
            # must nevertheless use the explicit UTC report date.
            probe_env.update(QT_PIPELINE_TEST_DSN=dsn, TZ='America/New_York')
            result = subprocess.run(
                [str(probe), mode, book, combined_id, str(epoch), str(destination), *names],
                env=probe_env, cwd=destination, capture_output=True, text=True, timeout=60)
            (destination / 'probe.log').write_text(result.stdout + result.stderr)
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
            return json.loads(lines[0])

        run_probe('seed')
        run_probe('seed', book='BOOK_B', label='seed-book-b')
        repository = PostgresPortfolioRepository(connection_factory=factory)
        registry = PostgresStrategyRegistry(connection_factory=factory)
        edit = UpsertQtPosition(registry, repository)
        for symbol, quantity in (('ES.v.0', 7), ('NG.v.0', 0)):
            result = edit.execute({'strategy_id': 'pipeline', 'portfolio_id': 'BOOK_A',
                'strategy_name': names[0], 'symbol': symbol, 'quantity': quantity,
                'reason': 'Synthetic pipeline integration test'}, user_id='1')
            check(result['position']['quantity'] == quantity, 'AlgoLens commits ' + symbol + ' edit')
        history = repository.fetch_overrides(combined_id, 'BOOK_A')
        check(len(history) == 2 and repository.fetch_overrides(combined_id, 'BOOK_B') == [],
              'override history is scoped to the edited book')
        check(sql("SELECT quantity FROM trading.positions WHERE portfolio_id='BOOK_B' AND portfolio_type='qt'",
                  fetch=True)[0]['quantity'] == 77, 'other book remains unchanged')
        check(sql("SELECT quantity FROM trading.positions WHERE portfolio_id='BOOK_A' AND portfolio_type='system' AND symbol='ES.v.0' AND strategy_name=%s",
                  (names[0],), fetch=True)[0]['quantity'] == 12, 'system proposal remains unchanged')
        run_probe('seed', label='reseed')
        report = run_probe('snapshot', label='edited-report')
        expected = {names[0]: {'ES.v.0': 7}, names[1]: {'ES.v.0': -3}}
        check(report['by_strategy'] == expected, 'real C++ reads exact manual quantities and excludes closure')
        check(report['combined'] == {'ES.v.0': 4}, 'combined quantity sums both strategies')
        csv_path = Path(report['csv_path'])
        rows = list(csv.DictReader(line for line in csv_path.read_text().splitlines()
                                  if line.strip() and not line.startswith('#')))
        check(sorted(float(row['quantity']) for row in rows if row['symbol']=='ES.v.0') == [-3, 7],
              'CSV contains the two manually effective strategy quantities')
        check(not any(row['symbol']=='NG.v.0' for row in rows), 'CSV omits the closed position')
        html = TableRows()
        html.feed(Path(report['email_path']).read_text())
        email_quantities = [float(row[1].replace(',', '')) for row in html.rows
                            if len(row)>1 and row[0]=='ES.v.0']
        check(7 in email_quantities and -3 in email_quantities,
              'unchanged email renderer shows the same per-strategy QT quantities')
        check(not any(row and row[0]=='NG.v.0' for row in html.rows), 'email omits the closed position')
        check(sql("SELECT quantity FROM trading.positions WHERE portfolio_id='BOOK_A' AND portfolio_type='qt' AND symbol='NG.v.0'",
                  fetch=True)[0]['quantity'] == 0, 'database retains explicit zero closure evidence')
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
        print('QT_PIPELINE_CHECKS=' + str(checks), flush=True)
    finally:
        if container:
            command(['docker', 'rm', '-f', container])
            print('DISPOSABLE_CONTAINER_REMOVED', flush=True)


if __name__ == '__main__':
    main()
