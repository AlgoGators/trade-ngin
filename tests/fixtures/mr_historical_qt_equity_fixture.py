"""Prospective real equity desk transaction in owned, route-less PostgreSQL.

Every market/action/basis record is explicitly synthetic. Setup uses actual
native preview and immutable confirmed decision evidence from desk, never a
trusted flag or fabricated evaluated response. Root must register the governed
schema and dispatcher before the first behavioral RED gate. Staging only.
"""
from copy import deepcopy
from datetime import timedelta
from decimal import Decimal
from hashlib import sha256
import os
import json
import subprocess

import pytest
from psycopg2.extras import Json
from test_qt_desk_storage import (
    DECISION, ATTEMPT, BINARY, ROOT, MODEL,
    canonical_qt_input_bytes, exact)
from test_runtime_control_schema import connection
from mr_historical_qt_desk_fixture import desk, BOOK

INPUT = "90000000-0000-4000-8000-000000000001"
MARKET = "91000000-0000-4000-8000-000000000001"
FINAL = "qt-finalization/92000000-0000-4000-8000-000000000001"
ACTIONS = "qt-actions/93000000-0000-4000-8000-000000000001"


def digest(value):
    return sha256(canonical_qt_input_bytes(value)).hexdigest()


def output_digest(value):
    # The full output contains actual numeric consumption observations. Native
    # canonical_qt_desk_source_json matches this Python finite-float spelling;
    # canonical input bytes deliberately reject numeric floats.
    return sha256(json.dumps(value, sort_keys=True, separators=(',', ':'),
                            ensure_ascii=False, allow_nan=False).encode('utf-8')).hexdigest()


def run(attempt=ATTEMPT, input_id=INPUT):
    assert BINARY.is_file(), "actual confirmed desk probe is required"
    return subprocess.run([str(BINARY), "--accounting", DECISION, attempt, input_id],
        capture_output=True, text=True, timeout=30, env=os.environ.copy())


def state(conn):
    with conn.cursor() as cur:
        result = []
        for table in ("positions", "executions", "live_results", "equity_curve",
            "qt_execution_observations", "qt_desk_receipts", "qt_desk_results",
            "position_overrides", "desk_run_results", "qt_desk_accounting_inputs",
            "qt_desk_market_sources", "qt_desk_finalization_sources",
            "qt_equity_desk_evidence_sources"):
            cur.execute("SELECT to_jsonb(t)::text FROM trading." + table +
                        " t ORDER BY to_jsonb(t)::text")
            result.append(cur.fetchall())
        return result


def isolated_rows(conn):
    with conn.cursor() as cur:
        result = []
        for table in ("positions", "executions", "live_results", "equity_curve"):
            cur.execute("SELECT to_jsonb(t)::text FROM trading." + table +
                        " t WHERE portfolio_type='system' OR portfolio_id='OTHER'"
                        " ORDER BY to_jsonb(t)::text")
            result.append(cur.fetchall())
        return result


@pytest.fixture()
def equity(desk, request):
    conn, _ = desk
    mode = getattr(request, "param", "executed")
    with conn.cursor() as cur:
        cur.execute((ROOT / "migrations/017_desk_accounting_inputs.sql").read_text())
        cur.execute((ROOT / "migrations/018_qt_desk_finalization.sql").read_text())
        # Prospective root-owned migration. A missing schema is a setup failure,
        # never the intended RED acceptance signal.
        migration = ROOT / "migrations/019_qt_equity_desk_accounting.sql"
        assert migration.is_file(), "register governed equity schema before RED"
        cur.execute(migration.read_text())
        cur.execute((ROOT / 'migrations/020_qt_position_accounting_precision.sql').read_text())
        cur.execute((ROOT / 'migrations/021_qt_equity_finalization.sql').read_text())
        cur.execute("ALTER TABLE trading.executions ADD strategy_name text,ADD date date,"
            "ADD symbol text,ADD exec_id text,ADD order_id text,ADD side text,"
            "ADD quantity numeric,ADD price numeric,ADD execution_time timestamptz,"
            "ADD commissions_fees numeric,ADD implicit_price_impact numeric,"
            "ADD slippage_market_impact numeric,ADD total_transaction_costs numeric,"
            "ADD is_partial boolean;"
            "ALTER TABLE trading.live_results ADD daily_pnl numeric,"
            "ADD daily_realized_pnl numeric,ADD daily_unrealized_pnl numeric,"
            "ADD daily_transaction_costs numeric,ADD total_realized_pnl numeric,"
            "ADD total_unrealized_pnl numeric,ADD total_transaction_costs numeric")
        cur.execute("SELECT source_day,clock_timestamp() FROM trading.qt_decisions"
                    " WHERE decision_id=%s", (DECISION,))
        day, now = cur.fetchone()
        prior = day - timedelta(days=1)
        stamp, prior_stamp = str(day) + "T00:00:00Z", str(prior) + "T00:00:00Z"
        cur.execute("SELECT payload->'selection_rows' FROM trading.qt_previews"
                    " WHERE preview_id=(SELECT preview_id FROM trading.qt_decisions"
                    " WHERE decision_id=%s)", (DECISION,))
        selected = cur.fetchone()[0]
        assert selected and all(row['asset_type'] == 'EQUITY' for row in selected)
        prior_rows, basis_rows = [], []
        frame = "owned-before-action" if mode in ("split", "dividend") else "owned-adjusted"
        price = "50" if mode == "split" else "99" if mode == "dividend" else "100" if mode == "quiet" else "101"
        for index, row in enumerate(selected):
            key = {**row['key'], 'date': str(prior), 'portfolio_type': 'qt'}
            chosen = Decimal(row['quantity_exact'])
            quantity = (chosen if not row['editable'] else chosen / 2 if mode == 'split' else chosen if mode in
                        ('quiet', 'dividend') else chosen - Decimal('0.5') if index == 0
                        else chosen + Decimal('1.5'))
            average = Decimal('25') if not row['editable'] else Decimal('100')
            unreal = quantity if mode == 'executed' and row['editable'] else Decimal('0')
            basis_id = "qt-basis/94000000-0000-4000-8000-" + f"{index + 1:012d}"
            basis = dict(schema_version='qt-equity-basis-source/v1', book_id=BOOK,
                source_day=str(prior), key=key, quantity_exact=exact(quantity),
                average_price_exact=exact(average), price_frame_id=frame,
                formed_day=str(prior - timedelta(days=5)))
            basis_rows.append((basis_id, basis))
            prior_rows.append(dict(key=key, quantity_exact=exact(quantity),
                average_price_exact=exact(average), daily_realized_pnl_exact='9',
                daily_unrealized_pnl_exact=exact(unreal), last_update=prior_stamp,
                basis_evidence=dict(source_id=basis_id, source_digest=digest(basis),
                                    price_frame_id=frame, formed_day=basis['formed_day'])))
            cur.execute("INSERT INTO trading.positions(portfolio_id,strategy_id,strategy_name,"
                "date,symbol,portfolio_type,quantity,average_price,daily_realized_pnl,"
                "daily_unrealized_pnl,last_update) VALUES(%s,%s,%s,%s,%s,'qt',%s,%s,9,%s,%s)",
                (BOOK, key['strategy_id'], key['strategy_name'], prior, key['symbol'],
                 quantity, average, unreal, prior_stamp))
        totals = []
        for engine in sorted({row['key']['strategy_id'] for row in selected}):
            unreal = sum(Decimal(row['daily_unrealized_pnl_exact']) for row in prior_rows
                         if row['key']['strategy_id'] == engine)
            pnl, equity_value = Decimal('20') + unreal, Decimal('1020') + unreal
            totals.append(dict(strategy_id=engine, initial_capital_exact='1000',
                equity_exact=exact(equity_value), total_pnl_exact=exact(pnl),
                total_realized_pnl_exact='30', total_transaction_costs_exact='10',
                total_unrealized_pnl_exact=exact(unreal)))
            cur.execute("INSERT INTO trading.live_results(portfolio_id,strategy_id,date,"
                "portfolio_type,total_pnl,current_portfolio_value,total_realized_pnl,"
                "total_transaction_costs,total_unrealized_pnl,daily_pnl,daily_realized_pnl,"
                "daily_unrealized_pnl,daily_transaction_costs)"
                " VALUES('EQUITY_MR_PORTFOLIO',%s,%s,'qt',%s,%s,30,10,%s,0,0,0,0)",
                (engine, prior_stamp, pnl, equity_value, unreal))
            cur.execute("INSERT INTO trading.equity_curve(portfolio_id,strategy_id,timestamp,"
                "portfolio_type,equity) VALUES('EQUITY_MR_PORTFOLIO',%s,%s,'qt',%s)",
                (engine, prior_stamp, equity_value))
        events = []
        if mode in ('split', 'dividend'):
            for row in prior_rows:
                events.append(dict(key={**row['key'], 'date': str(day)},
                    type='SPLIT' if mode == 'split' else 'DIVIDEND', ex_date=str(prior),
                    value_model_number='2' if mode == 'split' else '1',
                    basis_provenance='formed_on_or_before_ex_date',
                    basis_provenance_evidence=row['basis_evidence']['source_id'],
                    frame_before=frame, frame_after='owned-after-action',
                    raw_close_model_number=None if mode == 'split' else '100',
                    eligible_quantity_exact=None if mode == 'split' else row['quantity_exact']))
        action_payload = dict(schema_version='qt-equity-actions-source/v1', book_id=BOOK,
            source_day=str(day), previous_day=str(prior), valuation_time=stamp, events=events)
        actions = [dict(event, source_id=ACTIONS, source_digest=digest(action_payload))
                   for event in events]
        costs = dict(tick_constrained=False, commission_per_unit='0.005',
            min_commission_per_order='1', max_commission_per_order='100', max_commission_pct='0.01',
            sec_fee_per_million='20.6', finra_taf_per_share='0.000195', finra_taf_cap_per_trade='9.79',
            apply_regulatory_fees=False, baseline_spread_ticks='1', min_spread_ticks='1',
            max_spread_ticks='10', spread_cost_multiplier='0.5', max_impact_bps='100',
            tick_size='0.01', point_value='1', max_total_implicit_bps='0')
        instruments = []
        for symbol in sorted({row['key']['symbol'] for row in selected}):
            instrument_price = '25' if all(not row['editable'] for row in selected
                                          if row['key']['symbol'] == symbol) else price
            quote = dict(source_id='owned-synthetic-close/' + symbol,
                source_digest=digest(dict(symbol=symbol, date=str(prior), price=instrument_price)),
                date=str(prior), price_frame_id='owned-after-action' if events else frame,
                price_model_number=instrument_price)
            history = dict(symbol=symbol, date=str(prior), adv='1000000', volatility='1')
            instruments.append(dict(symbol=symbol, asset_type='EQUITY', reference=quote,
                mark=deepcopy(quote), cost_evidence=dict(source_id='owned-cost/' + symbol,
                source_digest=digest(history), date=str(prior), adv_model_number='1000000',
                volatility_multiplier_model_number='1'), cost_parameters=deepcopy(costs)))
        config = dict(explicit_fee_per_contract='1.5', min_adv='10000',
                      min_participation='0', max_participation='1')
        anchor = dict(schema_version='qt-equity-finalized-accounting/v1',
            calculation_version='qt-equity-main08b15c/v1', book_id=BOOK, source_day=str(prior),
            currency='USD', policy_revision=1, previous_positions=prior_rows, previous_totals=totals)
        market = dict(schema_version='qt-equity-accounting-market/v1',
            calculation_version='qt-equity-main08b15c/v1', book_id=BOOK, source_day=str(day),
            model_publication_id=MODEL, previous_day=str(prior), valuation_time=stamp,
            day_mode='open', currency='USD', cost_config=config, instruments=instruments,
            actions_source_id=ACTIONS, actions_source_digest=digest(action_payload))
        payload = dict(schema_version='qt-equity-accounting-input/v1',
            calculation_version='qt-equity-main08b15c/v1', decision_id=DECISION, book_id=BOOK,
            source_day=str(day), accounting_input_id=INPUT, market_source_id=MARKET,
            market_source_digest=digest(market), accounting_source_id=MARKET,
            prior_finalization_source_id=FINAL, prior_finalization_digest=digest(anchor),
            previous_day=str(prior), timestamp=stamp, day_mode='open', currency='USD',
            previous_positions=deepcopy(prior_rows), previous_totals=deepcopy(totals),
            instruments=deepcopy(instruments), actions=actions, cost_config=deepcopy(config))
        for source_id, purpose, source_day, evidence in [
                *( (i, 'basis', prior, b) for i, b in basis_rows),
                (ACTIONS, 'actions', day, action_payload)]:
            cur.execute("INSERT INTO trading.qt_equity_desk_evidence_sources(source_id,purpose,"
                "book_id,source_day,producer_id,policy_version,policy_revision,source_version,"
                "content_digest,payload) VALUES(%s,%s,'EQUITY_MR_PORTFOLIO',%s,'synthetic-execution',"
                "'execution-policy-v1',1,%s,%s,%s)",
                (source_id, purpose, source_day, source_id, digest(evidence), Json(evidence)))
        cur.execute("INSERT INTO trading.qt_desk_finalization_sources(source_id,book_id,source_day,"
            "producer_id,policy_version,source_version,content_digest,payload)"
            " VALUES(%s,'EQUITY_MR_PORTFOLIO',%s,'synthetic-execution','execution-policy-v1',%s,%s,%s)",
            (FINAL, prior, FINAL, digest(anchor), Json(anchor)))
        start, end = now - timedelta(seconds=1), now + timedelta(hours=1)
        cur.execute("INSERT INTO trading.qt_desk_market_sources(source_id,book_id,source_day,"
            "model_publication_id,producer_id,policy_version,policy_revision,source_version,"
            "as_of,valid_until,content_digest,payload) VALUES(%s,'EQUITY_MR_PORTFOLIO',%s,%s,"
            "'synthetic-execution','execution-policy-v1',1,%s,%s,%s,%s,%s)",
            (MARKET, day, MODEL, MARKET, start, end, digest(market), Json(market)))
        cur.execute("INSERT INTO trading.qt_desk_accounting_inputs(input_id,decision_id,producer_id,"
            "policy_version,source_version,as_of,valid_until,content_digest,payload)"
            " VALUES(%s,%s,'synthetic-execution','execution-policy-v1',%s,%s,%s,%s,%s)",
            (INPUT, DECISION, 'qt-input/' + INPUT, start, end, digest(payload), Json(payload)))
        # An independent registered engine keeps the foreign-book sentinel
        # governed without changing BOOK's already-confirmed authority read-set.
        # Adding OTHER membership to LIVE_TREND would bump its captured revision.
        cur.execute("INSERT INTO trading.strategy_registry(id,strategy_type,portfolio_id,"
            "is_active,lifecycle) VALUES('OTHER_ENGINE','OTHER_ENGINE','OTHER',true,'live')")
        cur.execute("INSERT INTO trading.strategy_book_memberships(strategy_id,portfolio_id)"
            " VALUES('OTHER_ENGINE','OTHER')")
        cur.execute("INSERT INTO trading.live_results(portfolio_id,strategy_id,date,portfolio_type,"
            "total_pnl,current_portfolio_value) VALUES('OTHER','OTHER_ENGINE',%s,'qt',99,9900)", (stamp,))
    return conn, payload


def assert_refused(result):
    # The actual owned desk probe returns 10 for a handled processor refusal.
    # Signals/crashes or the probe's connection exception code do not establish
    # an admission refusal, even if PostgreSQL itself rolls the transaction back.
    assert result.returncode == 10, result.stdout + result.stderr
    assert 'DESK_REFUSED=1' in result.stdout, result.stdout + result.stderr


def rehash_row(conn, table, identity_column, identity, payload):
    """Owned hostile-storage corruption, restoring immutability before processing."""
    assert table in ('qt_desk_accounting_inputs', 'desk_run_results')
    with conn.cursor() as cur:
        cur.execute('ALTER TABLE trading.' + table + ' DISABLE TRIGGER immutable_row')
        cur.execute('UPDATE trading.' + table + ' SET payload=%s,content_digest=%s WHERE ' +
                    identity_column + '=%s', (Json(payload), output_digest(payload)
                    if table == 'desk_run_results' else digest(payload), identity))
        assert cur.rowcount == 1
        cur.execute('ALTER TABLE trading.' + table + ' ENABLE TRIGGER immutable_row')


def stored_output(conn):
    with conn.cursor() as cur:
        cur.execute('SELECT payload FROM trading.desk_run_results WHERE decision_id=%s', (DECISION,))
        return cur.fetchone()[0]

