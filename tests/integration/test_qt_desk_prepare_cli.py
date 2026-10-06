"""Explicit operator preparation through actual market and finalization code."""
import json
import os
from pathlib import Path
from tests.qt_test_artifacts import artifact
import subprocess
import tempfile

import pytest
from test_qt_desk_market_capture import capture
from test_qt_desk_upstream import upstream, upstream_state, OLD, DECISION, MARKET, FINAL
from test_qt_desk_accounting import accounting
from test_qt_desk_storage import desk
from test_runtime_control_schema import connection
import test_qt_desk_run_cli as process_cli

BINARY = artifact("qt_desk_prepare_sources")
GUARD = BINARY.with_name('libqt_no_delivery_guard.so')


def args(day='2026-09-26', **patch):
    options = {'--desk': day, '--decision': DECISION, '--prior-decision': OLD,
        '--market-source': MARKET, '--finalization': FINAL,
        '--as-of': '2026-09-26T00:00:00Z', '--valid-until': '2026-09-26T01:00:00Z'}
    options.update(patch)
    return [value for pair in options.items() for value in pair]


def invoke(arguments, fd=None, environment=None):
    assert BINARY.is_file(), 'Supported QT source-preparation executable is missing'
    command = [str(BINARY), *arguments]
    if fd is not None: command += ['--connection-fd', str(fd)]
    return subprocess.run(command, capture_output=True, text=True, timeout=20,
        pass_fds=() if fd is None else (fd,), env=environment or {
            'PATH': '/usr/local/bin:/usr/bin:/bin', 'LD_PRELOAD': str(GUARD)})


def response(result, code, status):
    assert result.returncode == code, result.stdout + result.stderr
    assert result.stderr == ''
    value = json.loads(result.stdout)
    assert value['schema'] == 'qt-desk-prepare/v1' and value['status'] == status
    return value


def test_help_describes_explicit_source_preparation_without_database():
    result = invoke(['--help'])
    assert result.returncode == 0
    for field in ('--desk', '--prior-decision', '--market-source', '--finalization', '--as-of', '--valid-until', '--connection-fd'):
        assert field in result.stdout


@pytest.mark.parametrize('arguments', [
    [], args(), args() + ['--connection-fd'], args()[2:] + ['--connection-fd', '999'],
    args() + ['--unknown', 'never-echo', '--connection-fd', '999'],
    args() + ['--desk', '2026-09-26', '--connection-fd', '999'],
    args('2026-02-30') + ['--connection-fd', '999'],
    args(**{'--prior-decision': 'bad-uuid'}) + ['--connection-fd', '999'],
    args(**{'--market-source': MARKET.upper()}) + ['--connection-fd', '999'],
    args(**{'--finalization': 'qt-finalization/' + FINAL}) + ['--connection-fd', '999'],
    args(**{'--as-of': '2026-09-26T00:00:00+00:00'}) + ['--connection-fd', '999'],
    args(**{'--as-of': '2026-09-26T24:00:00Z'}) + ['--connection-fd', '999'],
    args(**{'--as-of': '2026-02-30T00:00:00Z'}) + ['--connection-fd', '999'],
    args(**{'--valid-until': '2026-09-26T00:00:00.1234567Z'}) + ['--connection-fd', '999'],
    args() + ['--connection-fd', '0'], args() + ['--connection-fd', '03'],
    args() + ['--connection-fd', '2147483648'],
])
def test_bad_arguments_are_refused_before_connection_access(arguments):
    result = invoke(arguments)
    response(result, 2, 'invalid_arguments')
    assert 'never-echo' not in result.stdout + result.stderr


def test_valid_explicit_source_grammar_reaches_descriptor_admission():
    response(invoke(args() + ['--connection-fd', '999']), 3, 'connection_input_unavailable')


@pytest.mark.parametrize('material', [b'', b'x' * 4097,
    b'host=127.0.0.1 dbname=unused user=unused password=never-echo',
    b'host=/tmp/unused dbname=unused user=unused password=never-echo service=forbidden'])
def test_shared_connection_boundary_is_bounded_and_redacted(material):
    with tempfile.TemporaryFile() as file:
        file.write(material); file.flush()
        fd = os.open(f'/proc/self/fd/{file.fileno()}', os.O_RDONLY)
        try: result = invoke(args(), fd)
        finally: os.close(fd)
    response(result, 3, 'connection_input_unavailable')
    assert 'never-echo' not in result.stdout + result.stderr


def owned(day, request, *, timezone='UTC', **patch):
    dsn = os.environ['ALGOLENS_TEST_DB']
    assert dsn.startswith('host=/tmp/algolens-repair-pg-') and 'dbname=algolens_test_' in dsn
    supplied = {'--decision': request['decision_id'], '--prior-decision': request['prior_decision_id'],
        '--market-source': request['source_id'], '--as-of': request['as_of'].replace('+00:00', 'Z'),
        '--valid-until': request['valid_until'].replace('+00:00', 'Z'), **patch}
    with tempfile.TemporaryFile() as material:
        material.write(dsn.encode()); material.flush()
        fd = os.open(f'/proc/self/fd/{material.fileno()}', os.O_RDONLY)
        environment = dict(os.environ, TZ=timezone, LD_PRELOAD=str(GUARD), PGHOST='192.0.2.1',
            PGSERVICE='must-not-be-read', PGPASSFILE='/unavailable/secret',
            DATABASE_URL='postgres://unavailable:never-echo@192.0.2.1/forbidden')
        try: return invoke(args(day, **supplied), fd, environment)
        finally: os.close(fd)


def day(conn):
    with conn.cursor() as cursor:
        cursor.execute('SELECT source_day::text FROM trading.qt_decisions WHERE decision_id=%s', (DECISION,))
        return cursor.fetchone()[0]


@pytest.mark.parametrize('desk', ['futures_mes'], indirect=True)
@pytest.mark.parametrize('accounting', ['no_input'], indirect=True)
@pytest.mark.parametrize('timezone', ['UTC', 'America/New_York'])
def test_operator_prepares_then_existing_command_accounts_exact_decision(capture, timezone):
    conn, request = capture
    date = day(conn)
    prepared = response(owned(date, request, timezone=timezone), 0, 'sources_prepared')
    assert prepared == {'schema': 'qt-desk-prepare/v1', 'status': 'sources_prepared',
        'decision_id': DECISION, 'prior_decision_id': OLD, 'source_day': date,
        'market_source_id': MARKET, 'finalization_source_id': 'qt-finalization/' + FINAL}
    with conn.cursor() as cursor:
        cursor.execute('SELECT count(*) FROM trading.qt_desk_receipts WHERE decision_id=%s', (DECISION,))
        assert cursor.fetchone()[0] == 0  # Preparing sources is not current processing.
    before = upstream_state(conn)
    assert response(owned(date, request, timezone=timezone), 0, 'sources_prepared') == prepared
    assert upstream_state(conn) == before
    receipt = process_cli.response(process_cli.run_owned(date,
        input_id='d0000000-0000-4000-8000-000000000029', market_source=MARKET,
        finalization_source='qt-finalization/' + FINAL), 0, 'accounted')
    assert receipt['decision_id'] == DECISION and receipt['replayed'] is False


@pytest.mark.parametrize('desk', ['futures_mes'], indirect=True)
@pytest.mark.parametrize('accounting', ['no_input'], indirect=True)
@pytest.mark.parametrize('damage', ['missing_bar', 'disabled_policy', 'wrong_day', 'missing_prior', 'expired_lease'])
def test_preparation_refusals_never_claim_ready_or_process_current_decision(capture, damage):
    conn, request = capture
    date = day(conn); patch = {}
    with conn.cursor() as cursor:
        if damage == 'missing_bar': cursor.execute('DELETE FROM futures_data.ohlcv_1d')
        elif damage == 'disabled_policy':
            cursor.execute("UPDATE trading.qt_source_policies SET enabled=false,version=version+1 WHERE book_id='BOOK' AND purpose='execution'")
    if damage == 'wrong_day': date = '2000-01-01'
    elif damage == 'missing_prior': patch['--prior-decision'] = 'b0000000-0000-4000-8000-000000000099'
    elif damage == 'expired_lease': patch['--valid-until'] = '2000-01-01T00:00:00Z'
    before = upstream_state(conn)
    response(owned(date, request, **patch), 4 if damage == 'wrong_day' else 5,
             'decision_day_unavailable' if damage == 'wrong_day' else 'source_capture_refused')
    assert upstream_state(conn) == before


@pytest.mark.parametrize('desk', ['futures_mes'], indirect=True)
@pytest.mark.parametrize('accounting', ['no_input'], indirect=True)
def test_failed_prior_finalization_keeps_only_explicit_market_source_and_never_ready(capture):
    conn, request = capture
    with conn.cursor() as cursor:
        cursor.execute('ALTER TABLE trading.qt_desk_receipts DISABLE TRIGGER USER')
        cursor.execute("UPDATE trading.qt_desk_receipts SET publication_payload=jsonb_set(publication_payload,'{selected_book_digest}',to_jsonb(repeat('0',64))) WHERE decision_id=%s", (OLD,))
        cursor.execute('ALTER TABLE trading.qt_desk_receipts ENABLE TRIGGER USER')
    before = upstream_state(conn)
    response(owned(day(conn), request), 6, 'prior_finalization_refused')
    after = upstream_state(conn)
    assert before[-4] == [] and len(after[-4]) == 1
    assert after[:-4] + after[-3:] == before[:-4] + before[-3:]
    with conn.cursor() as cursor:
        cursor.execute('SELECT count(*) FROM trading.qt_desk_receipts WHERE decision_id=%s', (DECISION,))
        assert cursor.fetchone() == (0,)
        cursor.execute('SELECT count(*) FROM trading.qt_desk_finalizations WHERE finalization_id=%s', (FINAL,))
        assert cursor.fetchone() == (0,)
    response(owned(day(conn), request), 6, 'prior_finalization_refused')
    assert upstream_state(conn) == after
