"""Lane N3 (equity day 2): the MODEL publisher's immutable verified-prior binding (migration 024).

Installed placement (PLAN16): tests/integration/. Migrations 023 and 024 come from this tree's migrations/
(024 pinned to its approved final bytes), and the probe is the coherent build's equity_prior_binding_probe,
the same convention as tests/test_equity_prior_publication.py. The fixture chain is the installed one
(tests/fixtures/mr_equity_eod_fixture.py and the tests/integration helpers).

Revision 2 (independent review F1/F7, N1 r3): adds an empty-owner v2 VerifiedEquity publish, a present
relation with a missing or wrong-version capability row, an end-to-end refusal raised by a database
trigger on the binding INSERT, and a variant that stays under the synthetic clock (migrations and publish).

A copy of tests/test_equity_prior_publication.py's approved_bridge flow driving the ACTUAL native
publisher through the lane's equity_prior_binding_probe. The new MODEL payload is synthetic;
the prior (S decision -> actual processing -> actual D finalization) is actual.

Differences from the original fixture chain, all in the owned route-less database:
- the fixture chain installs the synthetic SQL clock before its own first migration (desk requests
  eod_clock), so 023/024 cannot precede it. Instead, once the actual S->D prior and the approved intent
  exist, the clock is restored (the database search_path is reset and every shadow-bound default is
  rebound to pg_catalog), and only THEN are migration 023 and the staged 024 applied and the native
  publisher run: no 023/024 DDL ever runs under the clock, and the MODEL publication and its binding are
  written on real time, as in production. (Under the clock, 016's qt_model_seed_publications.created_at
  default is bound to the shadow now(), which can never equal the r2 guard's pg_catalog.now().);
- the equities_data substrate the action-frame capture reads is created empty (no rows unless a
  test adds a governed D action);
- every native binary started by these tests (fixture probes included) runs with the no-delivery
  guard preloaded.
"""
from datetime import datetime, timezone
from decimal import Decimal
from hashlib import sha256
from pathlib import Path
from tests.qt_test_artifacts import artifact
import json, os, subprocess, sys

import pytest
from psycopg2.extras import Json

HERE = Path(__file__).resolve().parent  # <engine>/tests/integration
ENGINE = HERE.parent.parent
WORKSPACE = next(p for p in HERE.parents if (p / '.review/trade-ngin-qt').is_dir())
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(WORKSPACE / 'docs/repairs/2026-09-26-hemdutt-issue-completion/equity-finalization-staging'))
sys.path.insert(0, str(ENGINE / 'tests/fixtures'))
from mr_equity_eod_fixture import (eod, eod_clock, equity, desk, connection, finalize,  # noqa: E402,F401  (fixtures)
    all_state, original_rows, BOOK, DECISION, SUCCESSOR, D_ACTIONS)
from test_proposal_storage_migration import apply  # noqa: E402
from test_qt_desk_storage import canonical_qt_input_bytes  # noqa: E402

GUARD = artifact("libqt_no_delivery_guard.so")
GUARD_SHA = 'f895b1de8a5a623446d445914038d66397248b40bf0e8a9ecbbbff76adaae4a9'
MIGRATION_023 = ENGINE / 'migrations/023_qt_empty_model_owner_publication.sql'
MIGRATION_024 = ENGINE / 'migrations/024_qt_equity_prior_continuation.sql'
MIGRATION_024_SHA256 = '85263f23417ee5982f417ce83a80a7b60e51a34a18bc8f1e24e52e92e7a12865'
PROBE = artifact("equity_prior_binding_probe")
STRATEGY = 'LIVE_EQUITY_MEAN_REVERSION'
D_SPLIT_ACTIONS = 'qt-actions/b4000000-0000-4000-8000-000000000001'
# Recreated per test: equities_data lives outside the per-test trading schema, so rows another test added
# (the governed D split bar) would otherwise leak into the next test of the same owned database.
SUBSTRATE = """DROP SCHEMA IF EXISTS equities_data CASCADE; CREATE SCHEMA equities_data;
CREATE TABLE equities_data.ohlcv_1d(symbol text NOT NULL,time timestamptz NOT NULL,
  open double precision NOT NULL,high double precision NOT NULL,low double precision NOT NULL,
  close double precision NOT NULL,volume double precision NOT NULL,div_cash double precision,
  split_factor double precision,delisting_date date,PRIMARY KEY(symbol,time));
CREATE TABLE equities_data.corporate_action(date text NOT NULL,action text NOT NULL,
  ticker text NOT NULL,value text,contraticker text,contraname text,name text);
CREATE TABLE equities_data.ticker_aliases(historical_ticker text PRIMARY KEY,
  current_symbol text NOT NULL,effective_until text,note text);"""


def digest(value):
    return sha256(canonical_qt_input_bytes(value)).hexdigest()


@pytest.fixture(autouse=True)
def guarded_natives():
    """Every native binary a test (or its fixtures) starts inherits the no-delivery guard."""
    assert GUARD.is_file() and not GUARD.is_symlink()
    assert sha256(GUARD.read_bytes()).hexdigest() == GUARD_SHA
    probe = PROBE
    assert probe.is_file(), 'lane N3 probe build required; a missing binary is a setup failure, not RED'
    previous = os.environ.get('LD_PRELOAD')
    os.environ['LD_PRELOAD'] = str(GUARD)
    yield probe
    if previous is None:
        os.environ.pop('LD_PRELOAD', None)
    else:
        os.environ['LD_PRELOAD'] = previous


CLOCKED = ('v1_clock',)
CAPABILITY_TAMPER = {
    'capability_missing': "DELETE FROM trading.qt_storage_capabilities WHERE capability_name='qt_equity_model_prior_binding_v1'",
    'capability_v2': "UPDATE trading.qt_storage_capabilities SET capability_version=2 "
                     "WHERE capability_name='qt_equity_model_prior_binding_v1'",
}
# Test-only, owned database: an extra BEFORE INSERT trigger that refuses every binding row, so the
# refusal is raised by PostgreSQL at the binding INSERT inside the publishing transaction.
REFUSING_TRIGGER = """CREATE FUNCTION trading.n3_test_refuse_binding() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN RAISE EXCEPTION 'n3 test: binding refused by database trigger'; END $$;
CREATE TRIGGER n3_test_refuse_binding BEFORE INSERT ON trading.qt_equity_model_prior_bindings
  FOR EACH ROW EXECUTE FUNCTION trading.n3_test_refuse_binding();"""


def migrate(conn, variant):
    """023, then the staged 024 unless the test asks for a 023-only database.

    Every variant except the clocked one runs with no clock installed."""
    with conn.cursor() as cur:
        cur.execute("SELECT to_regprocedure('trading.now()') IS NULL AND to_regprocedure('trading.clock_timestamp()') IS NULL")
        assert cur.fetchone()[0] == (variant not in CLOCKED), 'clock state does not match the variant'
    apply(conn, MIGRATION_023)
    if variant != '023-only':
        path = MIGRATION_024
        assert sha256(path.read_bytes()).hexdigest() == MIGRATION_024_SHA256
        apply(conn, path)
    with conn.cursor() as cur:
        cur.execute("SELECT to_regclass('trading.qt_equity_model_prior_bindings') IS NOT NULL,"
                    "(SELECT count(*) FROM trading.qt_storage_capabilities "
                    " WHERE capability_name='qt_equity_model_prior_binding_v1')")
        present, capability = cur.fetchone()
    assert (present, capability) == ((False, 0) if variant == '023-only' else (True, 1))
    with conn.cursor() as cur:
        if variant in CAPABILITY_TAMPER:
            # Owned test database only: bypass the capability table's immutability for this one change.
            cur.execute('ALTER TABLE trading.qt_storage_capabilities DISABLE TRIGGER qt_storage_capability_immutable')
            cur.execute(CAPABILITY_TAMPER[variant]); assert cur.rowcount == 1
            cur.execute('ALTER TABLE trading.qt_storage_capabilities ENABLE TRIGGER qt_storage_capability_immutable')
        if variant == 'trigger_refusal':
            cur.execute(REFUSING_TRIGGER)


def add_governed_d_split(conn, source, market):
    """A governed D split for every held symbol: raw bar + the one nonempty D actions candidate.

    Inserted after the S->D finalization (whose market keeps its empty actions source)."""
    day, prior = market['source_day'], source['source_day']
    with conn.cursor() as cur:
        cur.execute("SELECT payload->'previous_positions' FROM trading.qt_desk_finalization_sources "
                    "WHERE source_id=%s", ('qt-finalization/' + SUCCESSOR,))
        previous = cur.fetchone()[0]
        held = sorted((p for p in previous if Decimal(p['quantity_exact']) != 0), key=lambda p: p['key']['symbol'])
        assert held, 'fixture must hold a position for a D action'
        cur.execute("SELECT to_jsonb(p) FROM trading.qt_source_policies p WHERE book_id=%s AND purpose='execution'",
                    (BOOK,))
        policy = cur.fetchone()[0]
        events = []
        for p in held:
            symbol = p['key']['symbol']
            cur.execute("INSERT INTO equities_data.ohlcv_1d(symbol,time,open,high,low,close,volume,div_cash,"
                        "split_factor,delisting_date) VALUES(%s,%s,50,50,50,50,1000,0,2,NULL)",
                        (symbol, day + 'T00:00:00Z'))
            events.append(dict(key={**p['key'], 'date': day}, type='SPLIT', ex_date=day, value_model_number='2',
                basis_provenance='formed_on_or_before_ex_date',
                basis_provenance_evidence=p['basis_evidence']['source_id'],
                frame_before=p['basis_evidence']['price_frame_id'], frame_after='owned-after-d-split',
                raw_close_model_number=None, eligible_quantity_exact=None))
        payload = dict(schema_version='qt-equity-actions-source/v1', book_id=BOOK, source_day=day, previous_day=prior,
                       valuation_time=day + 'T00:00:00Z', events=events)
        # The governed row must be formed on D, after the policy's last change.
        updated = datetime.fromisoformat(policy['updated_at'])
        created = max(updated, datetime.fromisoformat(day + 'T00:00:01+00:00'))
        assert created.astimezone(timezone.utc).date().isoformat() == day, \
            'fixture cannot express a governed D action row: execution policy updated_at=' + policy['updated_at']
        cur.execute("INSERT INTO trading.qt_equity_desk_evidence_sources(source_id,purpose,book_id,source_day,"
                    "producer_id,policy_version,policy_revision,source_version,content_digest,payload,created_at) "
                    "VALUES(%s,'actions',%s,%s,%s,%s,%s,%s,%s,%s,%s)",
                    (D_SPLIT_ACTIONS, BOOK, day, policy['producer_id'], policy['policy_version'], policy['version'],
                     D_SPLIT_ACTIONS, digest(payload), Json(payload), created))
    return D_SPLIT_ACTIONS


@pytest.fixture()
def approved_bridge(eod, eod_clock, guarded_natives, request):
    conn, source, _, _, _, market = eod
    variant = getattr(request, 'param', 'v1')
    assert variant in ('v1', 'd_split', '023-only', 'empty_owner', 'capability_missing', 'capability_v2',
                       'trigger_refusal', 'v1_clock')
    result = finalize(); assert result.returncode == 0, result.stdout + result.stderr
    with conn.cursor() as cur:
        cur.execute(SUBSTRATE)  # no defaults: clock-neutral DDL
    if variant == 'd_split':
        add_governed_d_split(conn, source, market)
    snapshot = subprocess.run(argv(guarded_natives, source, market, 'snapshot'), capture_output=True, text=True,
                              timeout=20, env=os.environ.copy())
    assert snapshot.returncode == 0, snapshot.stdout + snapshot.stderr
    rows = [line for line in snapshot.stdout.splitlines() if line.startswith('{')]
    assert len(rows) == 1, snapshot.stdout
    with conn.cursor() as cursor:
        cursor.execute("SELECT id,runtime_revision FROM trading.strategy_registry WHERE "
            "strategy_type='LIVE_EQUITY_MEAN_REVERSION' AND portfolio_id=%s", (BOOK,))
        identity, revision = cursor.fetchone()
        cursor.execute("INSERT INTO trading.runtime_intents(registry_id,portfolio_id,engine_strategy_id,action,"
            "registry_revision,config_snapshot,status,requested_by,request_reason) VALUES"
            "(%s,%s,'LIVE_EQUITY_MEAN_REVERSION','run',%s,%s,'pending','1','synthetic owned bridge request') RETURNING id",
            (identity, BOOK, revision, Json(json.loads(rows[0]))))
        intent = cursor.fetchone()[0]
        cursor.execute("UPDATE trading.runtime_intents SET status='approved',approved_by='3',"
            "approval_reason='synthetic bridge protocol gate',approved_at=clock_timestamp() WHERE id=%s", (intent,))
    if variant not in CLOCKED:
        eod_clock.restore()  # real time from here on; the fixture's own exit is then a no-op
        with conn.cursor() as cur:
            cur.execute("SHOW search_path"); assert 'trading' not in cur.fetchone()[0]
            cur.execute("SELECT pg_get_expr(d.adbin,d.adrelid) FROM pg_attrdef d JOIN pg_attribute a ON "
                        "a.attrelid=d.adrelid AND a.attnum=d.adnum WHERE d.adrelid='trading.qt_model_seed_publications'::regclass "
                        "AND a.attname='created_at'")
            assert cur.fetchone()[0] == 'now()'
    else:
        with conn.cursor() as cur:  # still under the clock: 016's default is bound to the shadow now()
            cur.execute("SELECT clock_timestamp()=now() AND now()=trading.now()"); assert cur.fetchone()[0]
    migrate(conn, variant)
    return conn, source, market, guarded_natives


def argv(probe, source, market, mode):
    return [str(probe), mode, DECISION, SUCCESSOR, BOOK, source['source_day'], market['source_day']]


def invoke(probe, source, market, mode):
    assert os.environ.get('LD_PRELOAD') == str(GUARD)
    result = subprocess.run(argv(probe, source, market, mode), capture_output=True, text=True, timeout=60,
                            env=os.environ.copy())
    assert result.returncode == 0, result.stdout + result.stderr
    return result


def publication_state(conn):
    result = all_state(conn)
    with conn.cursor() as cursor:
        tables = ['live_run_metadata', 'run_inputs', 'risk_limits', 'signals']
        cursor.execute("SELECT to_regclass('trading.qt_equity_model_prior_bindings') IS NOT NULL")
        if cursor.fetchone()[0]: tables.append('qt_equity_model_prior_bindings')
        for table in tables:
            cursor.execute('SELECT to_jsonb(t)::text FROM trading.' + table + ' t ORDER BY to_jsonb(t)::text')
            result.append(cursor.fetchall())
    return result


def bindings(conn):
    with conn.cursor() as cursor:
        cursor.execute("SELECT to_jsonb(b) FROM trading.qt_equity_model_prior_bindings b ORDER BY publication_id")
        return [row[0] for row in cursor.fetchall()]


def published(conn, day):
    with conn.cursor() as cursor:
        cursor.execute('SELECT publication_id::text FROM (SELECT publication_id,publication_version FROM '
                       'trading.qt_model_seed_publications WHERE portfolio_id=%s AND strategy_id=%s AND source_day=%s '
                       'UNION ALL SELECT publication_id,publication_version FROM trading.qt_empty_model_owner_publications '
                       'WHERE portfolio_id=%s AND strategy_id=%s AND source_day=%s) p '
                       'ORDER BY publication_version DESC LIMIT 1', (BOOK, STRATEGY, day, BOOK, STRATEGY, day))
        publication = cursor.fetchone()[0]
        cursor.execute('SELECT engine_flags FROM trading.run_inputs WHERE portfolio_id=%s AND strategy_id=%s AND date=%s',
                       (BOOK, STRATEGY, day))
        return publication, cursor.fetchone()[0]['equity_model_prior']


def assert_binding(conn, market, *, schema, actions):
    day = market['source_day']
    publication, reference = published(conn, day)
    rows = bindings(conn)
    assert len(rows) == 1, rows
    b = rows[0]
    assert b['publication_id'] == publication
    assert (b['book_id'], b['source_day'], b['strategy_id']) == (BOOK, day, STRATEGY)
    # B.replay_reference == run_inputs.engine_flags.equity_model_prior, digested canonically.
    assert b['replay_reference'] == reference
    assert reference['schema_version'] == schema and reference['mode'] == 'verified_desk_prior'
    assert b['replay_reference_digest'] == digest(reference) == digest(b['replay_reference'])
    assert (b['decision_id'], b['finalization_id']) == (DECISION, SUCCESSOR) == \
        (reference['decision_id'], reference['finalization_id'])
    with conn.cursor() as cursor:
        cursor.execute('SELECT content_digest FROM trading.qt_desk_finalizations WHERE finalization_id=%s', (SUCCESSOR,))
        assert b['finalization_digest'] == reference['finalization_digest'] == cursor.fetchone()[0]
        cursor.execute('SELECT source_id,content_digest FROM trading.qt_desk_finalization_sources WHERE source_id=%s',
                       ('qt-finalization/' + SUCCESSOR,))
        assert (b['finalization_source_id'], b['finalization_source_digest']) == cursor.fetchone() == \
            (reference['finalization_source_id'], reference['finalization_source_digest'])
        if actions is None:
            assert b['actions_source_id'] is None and b['actions_source_digest'] is None
            assert 'action_frame' not in reference
        else:
            frame_source = reference['action_frame']['actions_source']
            cursor.execute("SELECT content_digest,purpose,book_id,source_day::text FROM "
                           "trading.qt_equity_desk_evidence_sources WHERE source_id=%s", (actions,))
            stored = cursor.fetchone()
            assert b['actions_source_id'] == frame_source['source_id'] == actions
            assert b['actions_source_digest'] == frame_source['content_digest'] == stored[0]
            assert stored[1:] == ('actions', BOOK, day)
    return b


def test_verified_v1_publication_records_its_exact_prior_binding(approved_bridge):
    conn, source, market, probe = approved_bridge; before = original_rows(conn)
    assert bindings(conn) == []
    result = invoke(probe, source, market, 'publish')
    assert 'EQ_ALL_PARTS_QUEUED=1' in result.stdout and 'EQ_PUBLICATION_COMMITTED=1' in result.stdout
    assert_binding(conn, market, schema='qt-equity-model-prior/v1', actions=None)
    assert original_rows(conn) == before


@pytest.mark.parametrize('approved_bridge', ['d_split'], indirect=True)
def test_verified_v2_publication_binds_the_action_frames_actions_source(approved_bridge):
    conn, source, market, probe = approved_bridge; before = original_rows(conn)
    result = invoke(probe, source, market, 'publish')
    assert 'EQ_ALL_PARTS_QUEUED=1' in result.stdout and 'EQ_PUBLICATION_COMMITTED=1' in result.stdout
    b = assert_binding(conn, market, schema='qt-equity-model-prior/v2', actions=D_SPLIT_ACTIONS)
    assert b['replay_reference']['action_admission'] == 'proved_action_adjusted_prior'
    assert b['replay_reference']['action_frame_digest'] == digest(b['replay_reference']['action_frame'])
    assert original_rows(conn) == before


def test_system_reference_publication_writes_no_binding(approved_bridge):
    conn, source, market, probe = approved_bridge
    result = invoke(probe, source, market, 'system_reference_publish')
    assert 'EQ_PUBLICATION_COMMITTED=1' in result.stdout
    with conn.cursor() as cursor:
        cursor.execute('SELECT count(*) FROM trading.qt_model_seed_publications WHERE portfolio_id=%s AND '
                       'strategy_id=%s AND source_day=%s', (BOOK, STRATEGY, market['source_day']))
        assert cursor.fetchone()[0] >= 1
        cursor.execute('SELECT engine_flags FROM trading.run_inputs WHERE portfolio_id=%s AND strategy_id=%s AND date=%s',
                       (BOOK, STRATEGY, market['source_day']))
        assert 'equity_model_prior' not in cursor.fetchone()[0]
    assert bindings(conn) == []


@pytest.mark.parametrize('approved_bridge', ['023-only'], indirect=True)
def test_verified_publication_without_the_binding_capability_is_refused_atomically(approved_bridge):
    conn, source, market, probe = approved_bridge; before = publication_state(conn)
    result = invoke(probe, source, market, 'publish_any')
    assert 'EQ_ALL_PARTS_QUEUED=1' in result.stdout
    assert 'EQ_PUBLICATION_REFUSED=1' in result.stdout and 'EQ_PUBLICATION_COMMITTED=1' not in result.stdout
    assert publication_state(conn) == before


@pytest.mark.parametrize('approved_bridge', ['v1_clock'], indirect=True)
def test_verified_v1_publication_binds_under_the_synthetic_clock(approved_bridge):
    """r3 pins the publication by transaction identity, so the clocked harness path needs no restore."""
    conn, source, market, probe = approved_bridge; before = original_rows(conn)
    result = invoke(probe, source, market, 'publish')
    assert 'EQ_PUBLICATION_COMMITTED=1' in result.stdout
    assert_binding(conn, market, schema='qt-equity-model-prior/v1', actions=None)
    assert original_rows(conn) == before


@pytest.mark.parametrize('approved_bridge', ['empty_owner'], indirect=True)
def test_verified_empty_owner_publication_records_its_binding(approved_bridge):
    """The D MODEL run ends flat: the publication is an empty-owner v2 row stamped clock_timestamp()."""
    conn, source, market, probe = approved_bridge; before = original_rows(conn)
    result = invoke(probe, source, market, 'publish_empty_owner')
    assert 'EQ_EMPTY_OWNER_BATCH=1' in result.stdout
    assert 'EQ_PUBLICATION_COMMITTED=1' in result.stdout, result.stdout
    b = assert_binding(conn, market, schema='qt-equity-model-prior/v1', actions=None)
    with conn.cursor() as cursor:
        cursor.execute('SELECT count(*) FROM trading.qt_model_seed_publications WHERE publication_id=%s',
                       (b['publication_id'],))
        assert cursor.fetchone()[0] == 0
        cursor.execute("SELECT system_components='[]'::jsonb FROM trading.qt_empty_model_owner_publications "
                       "WHERE publication_id=%s", (b['publication_id'],))
        assert cursor.fetchone() == (True,)
        cursor.execute("SELECT count(*) FROM trading.positions WHERE portfolio_id=%s AND date=%s AND portfolio_type='system'",
                       (BOOK, market['source_day']))
        assert cursor.fetchone()[0] == 0
    assert original_rows(conn) == before


@pytest.mark.parametrize('approved_bridge', ['capability_missing', 'capability_v2'], indirect=True)
def test_present_relation_without_the_v1_capability_row_is_refused_atomically(approved_bridge):
    conn, source, market, probe = approved_bridge; before = publication_state(conn)
    result = invoke(probe, source, market, 'publish_any')
    assert 'EQ_ALL_PARTS_QUEUED=1' in result.stdout
    assert 'EQ_PUBLICATION_REFUSED=1' in result.stdout and 'EQ_PUBLICATION_COMMITTED=1' not in result.stdout
    assert publication_state(conn) == before and bindings(conn) == []


@pytest.mark.parametrize('approved_bridge', ['trigger_refusal'], indirect=True)
def test_a_database_refusal_of_the_binding_rolls_back_the_whole_publication(approved_bridge):
    conn, source, market, probe = approved_bridge; before = publication_state(conn)
    result = invoke(probe, source, market, 'publish_any')
    assert 'EQ_ALL_PARTS_QUEUED=1' in result.stdout
    assert 'EQ_PUBLICATION_REFUSED=1' in result.stdout and 'EQ_PUBLICATION_COMMITTED=1' not in result.stdout
    assert publication_state(conn) == before and bindings(conn) == []
    with conn.cursor() as cursor:  # the seed publication INSERTed before the binding was rolled back too
        cursor.execute('SELECT count(*) FROM trading.qt_model_seed_publications WHERE portfolio_id=%s AND source_day=%s '
                       'AND producer_version=%s', (BOOK, market['source_day'], 'synthetic-mr-bridge-test'))
        assert cursor.fetchone()[0] == 0
