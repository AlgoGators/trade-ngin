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
    desk, DECISION, ATTEMPT, BINARY, ROOT, BOOK, MODEL,
    canonical_qt_input_bytes, exact)
from test_runtime_control_schema import connection

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
    action_mode = 'split' if mode in ('mixed_split', 'mixed_split_resize') else 'dividend' if mode == 'mixed_dividend' else mode
    with conn.cursor() as cur:
        cur.execute((ROOT / "migrations/017_desk_accounting_inputs.sql").read_text())
        cur.execute((ROOT / "migrations/018_qt_desk_finalization.sql").read_text())
        # Prospective root-owned migration. A missing schema is a setup failure,
        # never the intended RED acceptance signal.
        migration = ROOT / "migrations/019_qt_equity_desk_accounting.sql"
        assert migration.is_file(), "register governed equity schema before RED"
        cur.execute(migration.read_text())
        cur.execute((ROOT / 'migrations/020_qt_position_accounting_precision.sql').read_text())
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
        frame = "owned-before-action" if action_mode in ("split", "dividend") else "owned-adjusted"
        price = "50" if action_mode == "split" else "99" if action_mode == "dividend" else "100" if mode == "quiet" else "101"
        for index, row in enumerate(selected):
            key = {**row['key'], 'date': str(prior), 'portfolio_type': 'qt'}
            chosen = Decimal(row['quantity_exact'])
            quantity = (chosen / 2 if not row['editable'] and mode == 'mixed_split' else chosen if not row['editable'] else chosen / 2 if action_mode == 'split' else chosen if action_mode in
                        ('quiet', 'dividend', 'flat_zero_basis') else chosen - Decimal('0.5') if index == 0
                        else chosen + Decimal('1.5'))
            average = Decimal('25') if not row['editable'] else Decimal('0') if mode == 'flat_zero_basis' and chosen == 0 else Decimal('100')
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
                " VALUES('BOOK',%s,%s,'qt',%s,%s,30,10,%s,0,0,0,0)",
                (engine, prior_stamp, pnl, equity_value, unreal))
            cur.execute("INSERT INTO trading.equity_curve(portfolio_id,strategy_id,timestamp,"
                "portfolio_type,equity) VALUES('BOOK',%s,%s,'qt',%s)",
                (engine, prior_stamp, equity_value))
        events = []
        if action_mode in ('split', 'dividend'):
            for row in prior_rows:
                events.append(dict(key={**row['key'], 'date': str(day)},
                    type='SPLIT' if action_mode == 'split' else 'DIVIDEND', ex_date=str(prior),
                    value_model_number='2' if action_mode == 'split' else '1',
                    basis_provenance='formed_on_or_before_ex_date',
                    basis_provenance_evidence=row['basis_evidence']['source_id'],
                    frame_before=frame, frame_after='owned-after-action',
                    raw_close_model_number=None if action_mode == 'split' else '100',
                    eligible_quantity_exact=None if action_mode == 'split' else row['quantity_exact']))
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
            instrument_price = ('12.5' if action_mode == 'split' else '24.75247525' if action_mode == 'dividend' else '25') if all(not row['editable'] for row in selected
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
                "content_digest,payload) VALUES(%s,%s,'BOOK',%s,'synthetic-execution',"
                "'execution-policy-v1',1,%s,%s,%s)",
                (source_id, purpose, source_day, source_id, digest(evidence), Json(evidence)))
        cur.execute("INSERT INTO trading.qt_desk_finalization_sources(source_id,book_id,source_day,"
            "producer_id,policy_version,source_version,content_digest,payload)"
            " VALUES(%s,'BOOK',%s,'synthetic-execution','execution-policy-v1',%s,%s,%s)",
            (FINAL, prior, FINAL, digest(anchor), Json(anchor)))
        start, end = now - timedelta(seconds=1), now + timedelta(hours=1)
        cur.execute("INSERT INTO trading.qt_desk_market_sources(source_id,book_id,source_day,"
            "model_publication_id,producer_id,policy_version,policy_revision,source_version,"
            "as_of,valid_until,content_digest,payload) VALUES(%s,'BOOK',%s,%s,"
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


def test_fractional_owner_accounting_persists_gross_cost_flow_and_exact_basis(equity):
    conn, payload = equity
    preserved = isolated_rows(conn)
    result = run()
    assert result.returncode == 0, result.stdout + result.stderr
    assert isolated_rows(conn) == preserved
    output = stored_output(conn)
    assert output['schema_version'] == 'qt-equity-accounting/v1'
    assert output['calculation_version'] == 'qt-equity-main08b15c/v1'
    assert output['input_digest'] == digest(payload)
    with conn.cursor() as cur:
        cur.execute("SELECT strategy_name,quantity,average_price FROM trading.positions"
                    " WHERE portfolio_id='BOOK' AND portfolio_type='qt' AND date=%s"
                    " ORDER BY strategy_name", (payload['source_day'],))
        assert cur.fetchall() == [('synthetic-alpha', Decimal('5'), Decimal('100.1')),
                                  ('synthetic-beta', Decimal('1'), Decimal('100'))]
        cur.execute("SELECT strategy_name,side,quantity,commissions_fees,total_transaction_costs"
                    " FROM trading.executions WHERE portfolio_id='BOOK' AND portfolio_type='qt'"
                    " ORDER BY strategy_name")
        assert cur.fetchall() == [('synthetic-alpha', 'BUY', Decimal('.5'), Decimal('.505'), Decimal('.505')),
                                  ('synthetic-beta', 'SELL', Decimal('1.5'), Decimal('1'), Decimal('1'))]
        cur.execute("SELECT daily_realized_pnl,daily_unrealized_pnl,daily_transaction_costs,"
                    "daily_pnl,total_realized_pnl,total_unrealized_pnl,total_transaction_costs,"
                    "total_pnl,current_portfolio_value FROM trading.live_results WHERE"
                    " portfolio_id='BOOK' AND portfolio_type='qt' AND date=%s", (payload['timestamp'],))
        assert cur.fetchone() == tuple(map(Decimal, ['1.5', '-1.5', '1.505', '-1.505',
                                                    '31.5', '5.5', '11.505', '25.495', '1025.495']))
        cur.execute("SELECT equity FROM trading.equity_curve WHERE portfolio_id='BOOK'"
                    " AND portfolio_type='qt' AND timestamp=%s", (payload['timestamp'],))
        assert cur.fetchall() == [(Decimal('1025.495'),)]
    after = state(conn)
    replay = run()
    assert replay.returncode == 0 and 'REPLAYED=1' in replay.stdout
    assert state(conn) == after


def test_stored_trace_is_actual_nonzero_typed_cost_reads(equity):
    conn, _ = equity
    result = run()
    assert result.returncode == 0, result.stdout + result.stderr
    output = stored_output(conn)
    child = output['consumption']
    assert child['schema_version'] == 'qt-equity-cost-consumption/v1'
    assert child['authority'] == 'inspection_only'
    assert child['profile'] == 'qt_equity_accounting_costs'
    assert child['coverage'] == {'scope': 'executed_equity_cost_calls', 'status': 'complete'}
    assert child['identity']['decision_id'] == DECISION
    assert child['identity']['accounting_input_id'] == INPUT
    assert child['identity']['input_digest'] == output['input_digest']
    assert child['identity']['producer_version'] == 'local-qt-controlled'
    financial = deepcopy(output)
    financial.pop('consumption')
    assert child['identity']['financial_output_digest'] == digest(financial)
    trace = child['charges']
    assert len(trace) == len(output['executions']) == 2
    assert {row['key']['strategy_name'] for row in trace} == {'synthetic-alpha', 'synthetic-beta'}
    for row in trace:
        assert row['outcome'] == 'returned_ok'
        assert row['meta'] == {'input_source': 'explicit_values', 'asset_lookup': 'exact_symbol'}
        reads = {item['field']: item for item in row['reads']}
        assert len(reads) == len(row['reads'])
        assert reads['cost.charge.max_commission_pct']['value'] == 0.01
        assert 'cost.charge.max_commission_per_order' not in reads
        assert 'cost.charge.sec_fee_per_million' not in reads
        assert 'cost.charge.finra_taf_per_share' not in reads
        assert all(item['origin'] == 'runtime_effective' for item in reads.values())


@pytest.mark.parametrize('desk', ['carried'], indirect=True)
@pytest.mark.parametrize('equity', ['quiet', 'split', 'dividend'], indirect=True)
def test_action_restated_or_quiet_carry_has_no_execution_or_cash_income(equity):
    conn, payload = equity
    result = run()
    assert result.returncode == 0, result.stdout + result.stderr
    output = stored_output(conn)
    assert output['executions'] == [] and output['consumption']['charges'] == []
    assert all(row['observation_kind'] == 'carried' and row['execution_id'] is None
               and row['actual_cash_cost_exact'] == '0' for row in output['observation']['fills'])
    dividend = bool(payload['actions']) and payload['actions'][0]['type'] == 'DIVIDEND'
    assert output['live_results'][0]['daily_pnl_exact'] == ('-0.05940594' if dividend else '0')
    assert output['live_results'][0]['total_transaction_costs_exact'] == '10'
    assert output['live_results'][0]['current_portfolio_value_exact'] == ('1019.94059406' if dividend else '1020')
    if dividend:
        assert all(row['average_price_exact'] == '99.00990099' for row in output['observation']['fills'])
    after = state(conn)
    assert run().returncode == 0
    assert state(conn) == after
    if payload['actions']:
        assert len(output['corporate_action_adjustments']) == 2


@pytest.mark.parametrize('damage', ['market_identity', 'market_digest', 'price', 'cost',
    'basis_identity', 'basis_digest', 'capital', 'cumulative_gross', 'cumulative_cost',
    'previous_owner_omitted', 'anchor_digest'])
def test_rehashed_input_cannot_replace_governed_source_or_physical_anchors(equity, damage):
    conn, original = equity
    payload = deepcopy(original)
    if damage == 'market_identity': payload['market_source_id'] = INPUT
    elif damage == 'market_digest': payload['market_source_digest'] = '0' * 64
    elif damage == 'price': payload['instruments'][0]['reference']['price_model_number'] = '999'
    elif damage == 'cost': payload['instruments'][0]['cost_parameters']['commission_per_unit'] = '99'
    elif damage == 'basis_identity': payload['previous_positions'][0]['basis_evidence']['source_id'] = 'absent'
    elif damage == 'basis_digest': payload['previous_positions'][0]['basis_evidence']['source_digest'] = '0' * 64
    elif damage == 'capital': payload['previous_totals'][0]['initial_capital_exact'] = '999'
    elif damage == 'cumulative_gross': payload['previous_totals'][0]['total_realized_pnl_exact'] = '29'
    elif damage == 'cumulative_cost': payload['previous_totals'][0]['total_transaction_costs_exact'] = '9'
    elif damage == 'previous_owner_omitted': payload['previous_positions'].pop()
    elif damage == 'anchor_digest': payload['prior_finalization_digest'] = '0' * 64
    rehash_row(conn, 'qt_desk_accounting_inputs', 'input_id', INPUT, payload)
    before = state(conn)
    assert_refused(run())
    assert state(conn) == before


@pytest.mark.parametrize('desk', ['carried'], indirect=True)
@pytest.mark.parametrize('equity', ['split'], indirect=True)
def test_rehashed_input_cannot_omit_governed_action_bundle(equity):
    conn, payload = equity
    changed = deepcopy(payload)
    changed['actions'] = []
    rehash_row(conn, 'qt_desk_accounting_inputs', 'input_id', INPUT, changed)
    before = state(conn)
    assert_refused(run())
    assert state(conn) == before


@pytest.mark.parametrize('column', ['total_realized_pnl', 'total_transaction_costs', 'total_unrealized_pnl'])
def test_overprecision_prior_cumulative_anchor_never_rounds(equity, column):
    conn, payload = equity
    with conn.cursor() as cur:
        cur.execute('UPDATE trading.live_results SET ' + column + '=' + column +
                    "+0.000000001 WHERE portfolio_id='BOOK' AND portfolio_type='qt' AND date=%s",
                    (payload['previous_day'] + 'T00:00:00Z',))
    before = state(conn)
    assert_refused(run())
    assert state(conn) == before


@pytest.mark.parametrize('missing', ['basis', 'actions', 'market', 'finalization', 'prior_owner', 'prior_curve'])
def test_missing_explicit_sources_or_physical_rows_refuse_all_successors(equity, missing):
    conn, payload = equity
    with conn.cursor() as cur:
        if missing in ('basis', 'actions'):
            cur.execute('ALTER TABLE trading.qt_equity_desk_evidence_sources DISABLE TRIGGER immutable_row')
            cur.execute('DELETE FROM trading.qt_equity_desk_evidence_sources WHERE purpose=%s', (missing,))
            assert cur.rowcount > 0
            cur.execute('ALTER TABLE trading.qt_equity_desk_evidence_sources ENABLE TRIGGER immutable_row')
        elif missing == 'market':
            cur.execute('ALTER TABLE trading.qt_desk_market_sources RENAME TO unavailable_market')
        elif missing == 'finalization':
            cur.execute('ALTER TABLE trading.qt_desk_finalization_sources RENAME TO unavailable_anchor')
        elif missing == 'prior_owner':
            cur.execute("DELETE FROM trading.positions WHERE portfolio_id='BOOK' AND portfolio_type='qt'"
                        " AND date=%s AND strategy_name='synthetic-beta'", (payload['previous_day'],))
        else:
            cur.execute("DELETE FROM trading.equity_curve WHERE portfolio_id='BOOK' AND portfolio_type='qt'"
                        " AND timestamp=%s", (payload['previous_day'] + 'T00:00:00Z',))
    # The generic state helper needs the original table names, so compare all
    # writable successor tables directly when a source table is unavailable.
    def successors():
        with conn.cursor() as cur:
            answer = []
            for table in ('positions', 'executions', 'live_results', 'equity_curve',
                          'qt_execution_observations', 'qt_desk_receipts', 'qt_desk_results',
                          'position_overrides', 'desk_run_results'):
                cur.execute('SELECT to_jsonb(t)::text FROM trading.' + table +
                            ' t ORDER BY to_jsonb(t)::text')
                answer.append(cur.fetchall())
            return answer
    before = successors()
    assert_refused(run())
    assert successors() == before


def test_failure_after_financial_writes_rolls_back_positions_observation_and_receipt(equity):
    conn, _ = equity
    with conn.cursor() as cur:
        cur.execute("CREATE FUNCTION trading.fail_eq_result() RETURNS trigger LANGUAGE plpgsql"
                    " AS $$ BEGIN RAISE EXCEPTION 'owned late storage failure'; END $$;"
                    "CREATE TRIGGER fail_eq_result BEFORE INSERT ON trading.desk_run_results"
                    " FOR EACH ROW EXECUTE FUNCTION trading.fail_eq_result()")
    before = state(conn)
    assert_refused(run())
    assert state(conn) == before


@pytest.mark.parametrize('damage', ['trace', 'daily_flow', 'cumulative_cost',
    'authority_build', 'authority_bundle', 'authority_source', 'authority_revision',
    'authority_missing'])
def test_rehashed_result_cannot_change_actual_trace_or_financial_math(equity, damage):
    conn, _ = equity
    first = run()
    assert first.returncode == 0, first.stdout + first.stderr
    output = stored_output(conn)
    if damage == 'trace':
        observed = output['consumption']['charges'][0]['reads']
        next(item for item in observed if item['field'] == 'cost.charge.commission_per_unit')['value'] = 99
    elif damage == 'daily_flow': output['live_results'][0]['daily_unrealized_pnl_exact'] = '99'
    elif damage == 'cumulative_cost': output['live_results'][0]['total_transaction_costs_exact'] = '99'
    elif damage == 'authority_build': output['producer_authority']['evaluator_build'] = 'foreign-build'
    elif damage == 'authority_bundle': output['producer_authority']['evaluator_bundle_sha256'] = '0' * 64
    elif damage == 'authority_source': output['producer_authority']['source_version'] = 'foreign-source'
    elif damage == 'authority_revision': output['producer_authority']['policy_revision'] += 1
    else: del output['producer_authority']
    # Also repair the child's financial SHA so refusal proves the underlying
    # immutable authority/actual calculation, not just an obsolete child hash.
    financial = deepcopy(output)
    financial.pop('consumption')
    output['consumption']['identity']['financial_output_digest'] = digest(financial)
    rehash_row(conn, 'desk_run_results', 'decision_id', DECISION, output)
    before = state(conn)
    assert_refused(run())
    assert state(conn) == before


@pytest.mark.parametrize('column', ['total_realized_pnl', 'total_unrealized_pnl', 'total_transaction_costs'])
def test_replay_proves_all_actual_physical_cumulative_fields(equity, column):
    conn, payload = equity
    result = run()
    assert result.returncode == 0, result.stdout + result.stderr
    with conn.cursor() as cur:
        cur.execute('UPDATE trading.live_results SET ' + column + '=' + column +
                    "+1 WHERE portfolio_id='BOOK' AND portfolio_type='qt' AND date=%s",
                    (payload['timestamp'],))
    before = state(conn)
    assert_refused(run())
    assert state(conn) == before


@pytest.mark.parametrize('boundary', ['revoked', 'attempt', 'input'])
def test_replay_retains_current_authorization_and_explicit_identity(equity, boundary):
    conn, _ = equity
    assert run().returncode == 0
    if boundary == 'revoked':
        with conn.cursor() as cur:
            cur.execute("UPDATE trading.qt_action_grants SET active=false,version=version+1"
                        " WHERE user_id=1 AND capability='qt_submit'")
    before = state(conn)
    result = run(attempt=MARKET if boundary == 'attempt' else ATTEMPT,
                 input_id=MARKET if boundary == 'input' else INPUT)
    assert_refused(result)
    assert state(conn) == before


@pytest.mark.parametrize('desk', ['mixed'], indirect=True)
def test_noneditable_other_engine_is_preserved_with_complete_anchors(equity):
    conn, payload = equity
    result = run()
    assert result.returncode == 0, result.stdout + result.stderr
    output = stored_output(conn)
    other = next(row for row in output['observation']['fills'] if row['key']['strategy_id'] == 'IMMUTABLE')
    assert other['selected_quantity_exact'] == '8' and other['average_price_exact'] == '25'
    assert other['observation_kind'] == 'carried' and other['actual_cash_cost_exact'] == '0'
    assert {row['strategy_id'] for row in output['live_results']} == {'LIVE_TREND', 'IMMUTABLE'}
    assert all(row['key']['strategy_id'] == 'LIVE_TREND' for row in output['executions'])
    assert all(row['key']['strategy_id'] == 'LIVE_TREND' for row in output['consumption']['charges'])
    assert {row['strategy_id'] for row in payload['previous_totals']} == {'LIVE_TREND', 'IMMUTABLE'}


@pytest.mark.parametrize('desk', ['mixed'], indirect=True)
@pytest.mark.parametrize('equity', ['mixed_split', 'mixed_dividend'], indirect=True)
def test_governed_noneditable_action_restates_basis_without_resizing_choice_or_execution(equity):
    conn, payload = equity
    preserved = isolated_rows(conn)
    with conn.cursor() as cur:
        cur.execute("SELECT payload->'selection_rows' FROM trading.qt_previews WHERE preview_id="
                    "(SELECT preview_id FROM trading.qt_decisions WHERE decision_id=%s)", (DECISION,))
        original_choices = cur.fetchone()[0]
    choice = next(row for row in original_choices if row['key']['strategy_id'] == 'IMMUTABLE')
    assert choice['quantity_exact'] == '8' and choice['average_price_exact'] == '25'
    assert choice['editable'] is False
    before_payload = deepcopy(payload)
    result = run()
    assert result.returncode == 0, result.stdout + result.stderr
    output = stored_output(conn)
    other = next(row for row in output['observation']['fills'] if row['key']['strategy_id'] == 'IMMUTABLE')
    dividend = next(event for event in payload['actions'] if event['key']['strategy_id'] == 'IMMUTABLE')['type'] == 'DIVIDEND'
    expected_basis = '24.75247525' if dividend else '12.5'
    assert other['selected_quantity_exact'] == '8' and other['average_price_exact'] == expected_basis
    assert other['observation_kind'] == 'carried' and other['execution_id'] is None
    assert other['actual_cash_cost_exact'] == '0' and other['daily_realized_pnl_exact'] == '0'
    distance = next(row for row in output['distance'] if row['key']['strategy_id'] == 'IMMUTABLE')
    assert distance['restated_previous_quantity_exact'] == '8' and distance['execution_delta_exact'] == '0'
    assert all(row['key']['strategy_id'] != 'IMMUTABLE' for row in output['executions'])
    assert all(row['key']['strategy_id'] != 'IMMUTABLE' for row in output['consumption']['charges'])
    assert isolated_rows(conn) == preserved and payload == before_payload
    with conn.cursor() as cur:
        cur.execute("SELECT quantity,average_price FROM trading.positions WHERE portfolio_type='qt' "
                    "AND portfolio_id='BOOK' AND strategy_id='IMMUTABLE' AND date=%s", (payload['source_day'],))
        assert cur.fetchone() == (Decimal('8'), Decimal(expected_basis))
        cur.execute("SELECT payload->'selection_rows' FROM trading.qt_previews WHERE preview_id="
                    "(SELECT preview_id FROM trading.qt_decisions WHERE decision_id=%s)", (DECISION,))
        assert cur.fetchone()[0] == original_choices
    after = state(conn)
    assert run().returncode == 0 and state(conn) == after


@pytest.mark.parametrize('desk', ['mixed'], indirect=True)
@pytest.mark.parametrize('equity', ['mixed_split_resize'], indirect=True)
def test_noneditable_split_cannot_resize_or_trade_the_exact_chosen_holding(equity):
    conn, payload = equity
    assert next(row for row in payload['previous_positions'] if row['key']['strategy_id'] == 'IMMUTABLE')['quantity_exact'] == '8'
    before = state(conn)
    assert_refused(run())
    assert state(conn) == before


@pytest.mark.parametrize('desk', ['mixed'], indirect=True)
@pytest.mark.parametrize('equity', ['mixed_split', 'mixed_dividend'], indirect=True)
@pytest.mark.parametrize('damage', ['omit', 'frame', 'formation'])
def test_noneditable_action_requires_governed_complete_frame_and_basis_proof(equity, damage):
    conn, original = equity
    payload = deepcopy(original)
    if damage == 'omit':
        payload['actions'] = [row for row in payload['actions'] if row['key']['strategy_id'] != 'IMMUTABLE']
    elif damage == 'frame':
        next(row for row in payload['actions'] if row['key']['strategy_id'] == 'IMMUTABLE')['frame_before'] = 'foreign'
    else:
        next(row for row in payload['previous_positions'] if row['key']['strategy_id'] == 'IMMUTABLE')['basis_evidence']['formed_day'] = payload['source_day']
    rehash_row(conn, 'qt_desk_accounting_inputs', 'input_id', INPUT, payload)
    before = state(conn)
    assert_refused(run())
    assert state(conn) == before and original != payload


def test_actual_sourced_processor_assembles_explicit_input_and_receipt_atomically(equity):
    conn, original = equity
    with conn.cursor() as cur:
        cur.execute('ALTER TABLE trading.qt_desk_accounting_inputs DISABLE TRIGGER immutable_row')
        cur.execute('DELETE FROM trading.qt_desk_accounting_inputs WHERE input_id=%s', (INPUT,))
        assert cur.rowcount == 1
        cur.execute('ALTER TABLE trading.qt_desk_accounting_inputs ENABLE TRIGGER immutable_row')
    binary = BINARY.with_name('qt_desk_upstream_probe')
    assert binary.is_file()
    args = [str(binary), '--sourced', DECISION, ATTEMPT, INPUT, MARKET, FINAL]
    result = subprocess.run(args, capture_output=True, text=True, timeout=30, env=os.environ.copy())
    assert result.returncode == 0, result.stdout + result.stderr
    with conn.cursor() as cur:
        cur.execute('SELECT input_id::text,payload FROM trading.qt_desk_accounting_inputs WHERE decision_id=%s', (DECISION,))
        assert cur.fetchone() == (INPUT, original)
    after = state(conn)
    result = subprocess.run(args, capture_output=True, text=True, timeout=30, env=os.environ.copy())
    assert result.returncode == 0 and 'REPLAYED=1' in result.stdout
    assert state(conn) == after


@pytest.mark.parametrize('damage', ['lease', 'market_revision', 'execution_policy'])
def test_live_governed_metadata_binds_market_input_and_current_policy(equity, damage):
    conn, _ = equity
    with conn.cursor() as cur:
        if damage == 'execution_policy':
            cur.execute("UPDATE trading.qt_source_policies SET enabled=false,version=version+1"
                        " WHERE book_id='BOOK' AND purpose='execution'")
        else:
            table = 'qt_desk_accounting_inputs' if damage == 'lease' else 'qt_desk_market_sources'
            cur.execute('ALTER TABLE trading.' + table + ' DISABLE TRIGGER immutable_row')
            if damage == 'lease':
                cur.execute("UPDATE trading.qt_desk_accounting_inputs SET as_of=as_of-interval '1 second'")
            else:
                cur.execute('UPDATE trading.qt_desk_market_sources SET policy_revision=policy_revision+1')
            cur.execute('ALTER TABLE trading.' + table + ' ENABLE TRIGGER immutable_row')
    before = state(conn)
    assert_refused(run())
    assert state(conn) == before


def test_persisted_full_producer_authority_matches_original_preview_read_set(equity):
    conn, payload = equity
    result = run()
    assert result.returncode == 0, result.stdout + result.stderr
    output = stored_output(conn)
    authority = output['producer_authority']
    assert set(authority) == {'schema_version', 'book_id', 'source_day',
        'model_publication_id', 'snapshot_id', 'source_version', 'as_of', 'valid_until',
        'content_digest', 'producer_id', 'policy_version', 'policy_revision',
        'policy_updated_at', 'evaluator_build', 'evaluator_sha256',
        'evaluator_bundle_sha256', 'allowed_override_codes'}
    assert authority['schema_version'] == 'qt-input-authority/v1'
    assert authority['evaluator_build'] == 'local-qt-controlled'
    assert authority['book_id'] == BOOK and authority['source_day'] == payload['source_day']
    assert authority['model_publication_id'] == MODEL
    with conn.cursor() as cur:
        cur.execute("SELECT read_set_payload FROM trading.qt_previews WHERE preview_id="
                    "(SELECT preview_id FROM trading.qt_decisions WHERE decision_id=%s)", (DECISION,))
        archived = cur.fetchone()[0]
        cur.execute("SELECT content_digest FROM trading.desk_run_results WHERE decision_id=%s", (DECISION,))
        assert cur.fetchone()[0] == output_digest(output)
    assert len(archived['external_sources']) == 8
    assert all(source['digest'] == digest(authority) and source['status'] == 'available'
               for source in archived['external_sources'])
    assert authority['evaluator_sha256'] != '0' * 64 and authority['evaluator_bundle_sha256'] != '0' * 64
