"""Real first-day QT transactions on an explicitly owned synthetic PostgreSQL.

The opening System rows and market prices are synthetic; the native evaluator,
anchor, processor, rollback and receipt verification are production code.
"""
from datetime import timedelta
from copy import deepcopy
from decimal import Decimal
import json
import os
import subprocess

import pytest
from psycopg2.extras import Json

from test_runtime_control_schema import connection as base_connection
from test_qt_desk_storage import (desk, ROOT, BINARY, DECISION, ATTEMPT, MODEL,
    qt_digest_v1, internal_snapshot_digest)
from test_qt_desk_accounting import all_state

PROBE = BINARY.with_name("qt_desk_upstream_probe")
ANCHOR = "a1000000-0000-4000-8000-000000000001"
MARKET = "a2000000-0000-4000-8000-000000000001"
INPUT = "a3000000-0000-4000-8000-000000000001"


@pytest.fixture()
def connection(request):
    """A test-only clock in the already guarded, disposable database.

    Day-one rows are really processed today, then the same database moves
    to tomorrow's clock. No receipt, original input, or financial row is rewritten.
    """
    from psycopg2 import sql
    fixture = base_connection.__wrapped__()
    conn = next(fixture)
    shifted = "finalizes_on_day_two" in request.node.name
    try:
        if shifted:
            with conn.cursor() as cur:
                cur.execute("CREATE OR REPLACE FUNCTION public.clock_timestamp() RETURNS timestamptz LANGUAGE sql VOLATILE "
                    "AS $$ SELECT pg_catalog.clock_timestamp() $$")
                cur.execute("SELECT current_database()")
                database = cur.fetchone()[0]
                cur.execute(sql.SQL("ALTER DATABASE {} SET search_path=public,pg_catalog").format(sql.Identifier(database)))
                cur.execute("SET search_path=public,pg_catalog")
        yield conn
    finally:
        if shifted:
            with conn.cursor() as cur:
                cur.execute(sql.SQL("ALTER DATABASE {} RESET search_path").format(sql.Identifier(database)))
                # Defaults refer to this function; the next fixture recreates
                # trading. Leave a real-clock implementation until that point.
                cur.execute("CREATE OR REPLACE FUNCTION public.clock_timestamp() RETURNS timestamptz LANGUAGE sql VOLATILE "
                    "AS $$ SELECT pg_catalog.clock_timestamp() $$")
                cur.execute("SET search_path=pg_catalog,public")
        next(fixture, None)


def invoke(*args, payload=None):
    return subprocess.run([str(PROBE), *args],
        input=None if payload is None else json.dumps(payload),
        text=True, capture_output=True, timeout=30, env=os.environ.copy())


def state(conn):
    result = all_state(conn)
    with conn.cursor() as cur:
        for table in ("qt_desk_accounting_inputs", "qt_desk_finalizations", "qt_first_day_anchors"):
            cur.execute("SELECT to_jsonb(t)::text FROM trading." + table + " t ORDER BY to_jsonb(t)::text")
            result.append(cur.fetchall())
    return result


@pytest.fixture()
def first_day(desk):
    conn, _ = desk
    with conn.cursor() as cur:
        for migration in ("017_desk_accounting_inputs.sql", "018_qt_desk_finalization.sql",
                          "030_qt_first_day_bootstrap.sql"):
            cur.execute((ROOT / "migrations" / migration).read_text())
        cur.execute("ALTER TABLE trading.executions ADD strategy_name text,ADD date date,ADD symbol text,"
            "ADD exec_id text,ADD order_id text,ADD side text,ADD quantity numeric,ADD price numeric,"
            "ADD execution_time timestamptz,ADD commissions_fees numeric,ADD implicit_price_impact numeric,"
            "ADD slippage_market_impact numeric,ADD total_transaction_costs numeric,ADD is_partial boolean;"
            "ALTER TABLE trading.live_results ADD daily_pnl numeric,ADD daily_realized_pnl numeric,"
            "ADD daily_unrealized_pnl numeric,ADD daily_transaction_costs numeric,ADD total_transaction_costs numeric")
        cur.execute("SELECT source_day,clock_timestamp() FROM trading.qt_decisions WHERE decision_id=%s", (DECISION,))
        day, now = cur.fetchone()
        cur.execute("INSERT INTO trading.live_results(portfolio_id,strategy_id,date,portfolio_type,total_pnl,"
            "current_portfolio_value,daily_pnl,daily_realized_pnl,daily_unrealized_pnl,daily_transaction_costs,total_transaction_costs) "
            "VALUES('BOOK','LIVE_TREND',%s,'system',20,1000,1,-1,2.5,0.5,12)", (day,))
        cur.execute("INSERT INTO trading.equity_curve(portfolio_id,strategy_id,timestamp,portfolio_type,equity) "
            "VALUES('BOOK','LIVE_TREND',%s,'system',1000)", (day,))
        cur.execute("INSERT INTO trading.run_inputs(portfolio_id,strategy_id,date,config_snapshot) "
            "VALUES('BOOK','LIVE_TREND',%s,'{}')", (day,))
    instrument = dict(symbol="SYN", instrument_type="FUTURE", price_model_number="100",
        adv_model_number="100000", volatility_multiplier_model_number="1", source_id="synthetic-close",
        price_time=str(day-timedelta(days=1))+"T00:00:00Z", history_source_id="synthetic-history",
        history_digest="3"*64, history_observation_count=1, history_complete=True, asset_lookup="exact_symbol",
        baseline_spread_ticks="0", min_spread_ticks="0", max_spread_ticks="0", spread_cost_multiplier="0",
        max_impact_bps="0", tick_size="0.01", point_value="50", max_total_implicit_bps="0")
    market = dict(schema_version="qt-accounting-market/v1", book_id="BOOK", source_day=str(day),
        previous_day=str(day-timedelta(days=1)), valuation_time=str(day)+"T00:00:00Z", currency="USD",
        model_publication_id=MODEL, dataset_source_id="synthetic-bars", dataset_digest="1"*64,
        cost_config_source_id="synthetic-costs", cost_config_digest="2"*64, engine_build="synthetic-input",
        cost_config=dict(explicit_fee_per_contract="2", min_adv="100", min_participation="0", max_participation="0.1"),
        instruments=[instrument])
    source = dict(source_id=MARKET, producer_id="synthetic-execution", policy_version="execution-policy-v1",
        source_version="synthetic-market-v1", as_of=(now-timedelta(seconds=1)).isoformat(),
        valid_until=(now+timedelta(hours=1)).isoformat(), payload=market)
    result = invoke("--market", payload=source)
    assert result.returncode == 0, result.stdout+result.stderr
    result = invoke("--anchor", DECISION, ANCHOR)
    assert result.returncode == 0, result.stdout+result.stderr
    return conn, source


def process():
    return invoke("--first-day", DECISION, ATTEMPT, INPUT, MARKET, ANCHOR)


@pytest.mark.parametrize("desk", ["futures_quiet"], indirect=True)
def test_first_day_noop_commits_exact_system_opening_and_replays(first_day):
    conn, _ = first_day
    result = process()
    assert result.returncode == 0, result.stdout+result.stderr
    with conn.cursor() as cur:
        cur.execute("SELECT daily_pnl,total_pnl,current_portfolio_value,daily_transaction_costs,total_transaction_costs "
                    "FROM trading.live_results WHERE portfolio_type='qt'")
        assert cur.fetchone() == (Decimal(1), Decimal(20), Decimal(1000), Decimal('0.5'), Decimal(12))
        cur.execute("SELECT count(*) FROM trading.executions WHERE portfolio_type='qt'")
        assert cur.fetchone() == (0,)
    after = state(conn)
    result = process()
    assert result.returncode == 0 and "REPLAYED=1" in result.stdout, result.stdout+result.stderr
    assert state(conn) == after


@pytest.mark.parametrize("desk", ["futures"], indirect=True)
def test_first_day_changed_choice_keeps_pnl_and_charges_only_incremental_cost(first_day):
    conn, _ = first_day
    result = process()
    assert result.returncode == 0, result.stdout+result.stderr
    with conn.cursor() as cur:
        cur.execute("SELECT daily_pnl,daily_realized_pnl,daily_unrealized_pnl,total_pnl,current_portfolio_value,"
                    "daily_transaction_costs,total_transaction_costs FROM trading.live_results WHERE portfolio_type='qt'")
        assert cur.fetchone() == tuple(map(Decimal, ('-3', '-1', '2.5', '16', '996', '4.5', '16')))
        cur.execute("SELECT strategy_name,side,quantity,total_transaction_costs FROM trading.executions "
                    "WHERE portfolio_type='qt' ORDER BY strategy_name")
        assert cur.fetchall() == [('synthetic-alpha', 'BUY', Decimal(1), Decimal(2)),
                                  ('synthetic-beta', 'SELL', Decimal(1), Decimal(2))]
        cur.execute("SELECT payload->'results'->'currency_totals' FROM trading.qt_execution_observations WHERE observation_id=%s", (INPUT,))
        assert cur.fetchone()[0] == [dict(currency='USD', actual_cash_cost_exact='4',
            daily_unrealized_pnl_exact='2.5', daily_realized_pnl_exact='-1')]


@pytest.mark.parametrize("desk", ["futures"], indirect=True)
def test_first_day_failure_rolls_back_input_positions_financials_and_receipt(first_day):
    conn, _ = first_day
    with conn.cursor() as cur:
        cur.execute("""CREATE FUNCTION trading.reject_first_day_result() RETURNS trigger LANGUAGE plpgsql AS $$
          BEGIN RAISE EXCEPTION 'synthetic write failure'; END $$;
          CREATE TRIGGER reject_first_day_result BEFORE INSERT ON trading.qt_desk_results
          FOR EACH ROW EXECUTE FUNCTION trading.reject_first_day_result()""")
    before = state(conn)
    result = process()
    assert result.returncode != 0
    assert state(conn) == before
    with conn.cursor() as cur:
        cur.execute("DROP TRIGGER reject_first_day_result ON trading.qt_desk_results")
    result = process()
    assert result.returncode == 0, result.stdout+result.stderr


@pytest.mark.parametrize("desk", ["futures"], indirect=True)
def test_real_first_day_finalizes_on_day_two_without_invented_predecessor(first_day):
    conn, original_market = first_day
    result = process()
    assert result.returncode == 0, result.stdout+result.stderr
    with conn.cursor() as cur:
        cur.execute("SELECT payload FROM trading.desk_run_results WHERE decision_id=%s", (DECISION,))
        original_output = cur.fetchone()[0]
        cur.execute("CREATE OR REPLACE FUNCTION public.clock_timestamp() RETURNS timestamptz LANGUAGE sql VOLATILE "
                    "AS $$ SELECT pg_catalog.clock_timestamp()+interval '1 day' $$")
        cur.execute("SELECT clock_timestamp()")
        now = cur.fetchone()[0]
        today = now.date().isoformat()
        cur.execute("SELECT to_jsonb(m) FROM trading.qt_model_seed_publications m WHERE publication_id=%s", (MODEL,))
        model = cur.fetchone()[0]
        # Only tomorrow's MODEL is synthetic; yesterday's processed ledger is
        # left exactly as written by the real processor.
        old_day = model['source_day']
        next_model = "a4000000-0000-4000-8000-000000000002"
        model = json.loads(json.dumps(model).replace(old_day, today).replace(MODEL, next_model))
        model['seed_digest'] = qt_digest_v1({'seed_rows': model['system_components']})
        model['proposal_manifest_digest'] = internal_snapshot_digest('qt-proposal-manifest/v1',
            {'proposal_rows': model['proposal_components']})
        cur.execute("INSERT INTO trading.qt_model_seed_publications SELECT * FROM "
            "jsonb_populate_record(NULL::trading.qt_model_seed_publications,%s)", (Json(model),))
    market = deepcopy(original_market)
    next_market = "a2000000-0000-4000-8000-000000000002"
    final = "a5000000-0000-4000-8000-000000000002"
    market.update(source_id=next_market, source_version='synthetic-day-two',
        as_of=(now-timedelta(seconds=1)).isoformat(), valid_until=(now+timedelta(hours=1)).isoformat())
    market['payload'].update(source_day=today, previous_day=old_day,
        valuation_time=today+'T00:00:00Z', model_publication_id=next_model)
    market['payload']['instruments'][0].update(price_model_number='101', price_time=old_day+'T00:00:00Z')
    result = invoke('--market', payload=market)
    assert result.returncode == 0, result.stdout+result.stderr
    before = state(conn)
    result = invoke('--finalize', DECISION, final, next_market)
    assert result.returncode == 0, result.stdout+result.stderr
    transition = json.loads(result.stdout)
    assert transition['first_day_anchor_id'] == ANCHOR
    assert 'predecessor_finalization_source_id' not in transition
    with conn.cursor() as cur:
        cur.execute("SELECT daily_pnl,daily_realized_pnl,daily_unrealized_pnl,total_pnl,current_portfolio_value,"
                    "daily_transaction_costs,total_transaction_costs FROM trading.live_results WHERE portfolio_type='qt'")
        assert cur.fetchone() == tuple(map(Decimal, ('297', '299', '2.5', '316', '1296', '4.5', '16')))
        cur.execute("SELECT payload FROM trading.desk_run_results WHERE decision_id=%s", (DECISION,))
        assert cur.fetchone()[0] == original_output
    after = state(conn)
    assert after != before
    result = invoke('--finalize', DECISION, final, next_market)
    assert result.returncode == 0, result.stdout+result.stderr
    assert state(conn) == after
