"""Real native transition with explicitly synthetic historical original records.

The original financial rows come from the real pure accounting producer. This
fixture does not claim historical HTTP processing or authentic market parity.
"""
from copy import deepcopy
from datetime import timedelta, timezone
import json
import os
import subprocess
import pytest
from psycopg2.extras import Json
from test_qt_desk_accounting import accounting, digest, INPUT, all_state
from test_qt_desk_storage import (desk, DECISION, ATTEMPT, MODEL, ROOT, BINARY,
    qt_digest_v1, internal_snapshot_digest)
from test_runtime_control_schema import connection

PROBE=BINARY.with_name('qt_desk_upstream_probe')
FINAL='f0000000-0000-4000-8000-000000000001'
MARKET='a0000000-0000-4000-8000-000000000001'
OLD='b0000000-0000-4000-8000-000000000001'
OLD_INPUT='c0000000-0000-4000-8000-000000000001'
OLD_MODEL='b4000000-0000-4000-8000-000000000001'
pytestmark=[pytest.mark.parametrize('desk',['futures'],indirect=True),
            pytest.mark.parametrize('accounting',['no_input'],indirect=True)]

def invoke(*args,payload=None):
    return subprocess.run([str(PROBE),*args],input=None if payload is None else json.dumps(payload),
        text=True,capture_output=True,timeout=30,env=os.environ.copy())

def upstream_state(conn):
    result=all_state(conn)
    with conn.cursor() as cur:
        for table in ('qt_desk_market_sources','qt_desk_finalizations','qt_desk_finalization_sources','qt_desk_accounting_inputs'):
            cur.execute('SELECT to_jsonb(t)::text FROM trading.'+table+' t ORDER BY to_jsonb(t)::text')
            result.append(cur.fetchall())
    return result

@pytest.fixture()
def upstream(accounting,request):
    conn,original=accounting
    with conn.cursor() as cur:
        cur.execute((ROOT/'migrations/018_qt_desk_finalization.sql').read_text())
        cur.execute('SELECT source_day,clock_timestamp() FROM trading.qt_decisions WHERE decision_id=%s',(DECISION,))
        today,now=cur.fetchone();prior=today-timedelta(days=2 if getattr(request,'param','')=='dated_gap' else 1);prior2=prior-timedelta(days=1)
        if getattr(request,'param','')=='dated_gap':
            cur.execute("UPDATE trading.positions SET date=%s WHERE portfolio_type='qt' AND date=%s",(prior,today-timedelta(days=1)))
            cur.execute("UPDATE trading.live_results SET date=%s WHERE portfolio_type='qt' AND date=%s",(prior,today-timedelta(days=1)))
        # Clone only immutable schema dependencies; historical authority is
        # explicitly synthetic, not a purported old HTTP transaction.
        replacements={str(today):str(prior),DECISION:OLD,INPUT:OLD_INPUT,MODEL:OLD_MODEL}
        cur.execute('SELECT to_jsonb(m) FROM trading.qt_model_seed_publications m WHERE publication_id=%s',(MODEL,));model=cur.fetchone()[0]
        raw=json.dumps(model)
        for old,new in replacements.items():raw=raw.replace(old,new)
        model=json.loads(raw);old_seed=model['seed_digest'];old_manifest=model['proposal_manifest_digest']
        model['seed_digest']=qt_digest_v1({'seed_rows':model['system_components']})
        model['proposal_manifest_digest']=internal_snapshot_digest('qt-proposal-manifest/v1',{'proposal_rows':model['proposal_components']})
        replacements.update({old_seed:model['seed_digest'],old_manifest:model['proposal_manifest_digest']})
        actual_model=deepcopy(model)
        if getattr(request,'param','')=='archive_model_seed':
            actual_model['system_components'][0]['quantity_exact']='99'
            actual_model['seed_digest']=qt_digest_v1({'seed_rows':actual_model['system_components']})
        if getattr(request,'param','')=='archive_model_metadata':actual_model['producer_version']='foreign-producer-version'
        cur.execute('INSERT INTO trading.qt_model_seed_publications SELECT * FROM jsonb_populate_record(NULL::trading.qt_model_seed_publications,%s)',(Json(actual_model),))
        for table,column,newid in (('qt_drafts','draft_id','b1000000-0000-4000-8000-000000000001'),
                                  ('qt_previews','preview_id','b2000000-0000-4000-8000-000000000001')):
            cur.execute('SELECT to_jsonb(t) FROM trading.'+table+' t LIMIT 1');row=cur.fetchone()[0]
            replacements[row[column]]=newid
            raw=json.dumps(row)
            for old,new in replacements.items():raw=raw.replace(old,new)
            if table=='qt_previews':
                row=json.loads(raw)
                old_selected=row['selected_book_digest'];old_read_set=row['read_set_digest'];old_payload=row['payload_digest']
                projected=[dict(key={**r['key'],'portfolio_type':'qt'},quantity_exact=r['quantity_exact']) for r in row['payload']['selection_rows']]
                selected=qt_digest_v1({'selection_rows':projected})
                read_set=internal_snapshot_digest('qt-read-set/v1',row['read_set_payload'])
                raw=raw.replace(old_selected,selected).replace(old_read_set,read_set)
                row=json.loads(raw);unhashed=deepcopy(row['payload']);unhashed.pop('payload_digest')
                payload_hash=qt_digest_v1(unhashed);raw=raw.replace(old_payload,payload_hash)
                replacements.update({old_selected:selected,old_read_set:read_set,old_payload:payload_hash})
            if table=='qt_previews' and getattr(request,'param','')=='archive_evaluator':
                changed=json.loads(raw);changed['evaluator_build']='foreign-recorded-build';raw=json.dumps(changed)
            cur.execute('INSERT INTO trading.'+table+' SELECT * FROM jsonb_populate_record(NULL::trading.'+table+',%s)',(raw,))
        cur.execute('SELECT to_jsonb(d) FROM trading.qt_decisions d WHERE decision_id=%s',(DECISION,));decision=cur.fetchone()[0]
        raw=json.dumps(decision)
        for old,new in replacements.items():raw=raw.replace(old,new)
        decision=json.loads(raw)
        if getattr(request,'param','')=='archive_grant':decision['submitter_grant_version']=999;raw=json.dumps(decision)
        cur.execute('INSERT INTO trading.qt_decisions SELECT * FROM jsonb_populate_record(NULL::trading.qt_decisions,%s)',(raw,))
        source=deepcopy(original);source.update(decision_id=OLD,source_day=str(prior),previous_day=str(prior2),timestamp=str(prior)+'T00:00:00Z',prior_finalization_source_id='synthetic-earlier-final')
        for row in source['previous_positions']:row['key']['date']=str(prior2)
        cur.execute('SELECT payload FROM trading.qt_previews WHERE preview_id=%s',(decision['preview_id'],));selection=cur.fetchone()[0]['selection_rows']
        r=invoke('--original',payload=dict(decision=decision,selection=selection,input=source));assert r.returncode==0,r.stdout+r.stderr
        output=json.loads(r.stdout);output['input_digest']=digest(source)
        output['observation'].update(schema_version='qt-execution/v2',accounting_input_id=OLD_INPUT)
        if getattr(request,'param','')=='archive_count':output['observation']['results']['position_count']=999
        if getattr(request,'param','')=='archive_currency':output['observation']['results']['currency_totals'][0]['actual_cash_cost_exact']='999'
        if getattr(request,'param','')=='archive_fill_source':output['observation']['fills'][0]['accounting_source_id']=' '
        anchor=dict(schema_version='qt-finalized-accounting/v1',book_id='BOOK',source_day=str(prior2),previous_totals=source['previous_totals'])
        cur.execute("INSERT INTO trading.qt_desk_finalization_sources(source_id,book_id,source_day,producer_id,policy_version,source_version,content_digest,payload) VALUES('synthetic-earlier-final','BOOK',%s,'synthetic-execution','execution-policy-v1','earlier-v1',%s,%s)",(prior2,digest(anchor),Json(anchor)))
        cur.execute("INSERT INTO trading.qt_desk_accounting_inputs(input_id,decision_id,producer_id,policy_version,source_version,as_of,valid_until,content_digest,payload) VALUES(%s,%s,'synthetic-execution','execution-policy-v1','historical-v1',%s,%s,%s,%s)",(OLD_INPUT,OLD,now-timedelta(days=2),now-timedelta(days=1),digest(source),Json(source)))
        cur.execute("INSERT INTO trading.qt_execution_observations(observation_id,decision_id,producer_id,policy_version,source_version,as_of,valid_until,content_digest,payload) VALUES(%s,%s,'synthetic-execution','execution-policy-v1','historical-v1',%s,%s,%s,%s)",(OLD_INPUT,OLD,now-timedelta(days=2),now-timedelta(days=1),digest(output['observation']),Json(output['observation'])))
        cur.execute('INSERT INTO trading.desk_run_results(decision_id,input_id,portfolio_id,date,content_digest,payload) VALUES(%s,%s,\'BOOK\',%s,%s,%s)',(OLD,OLD_INPUT,prior,digest(output),Json(output)))
        if getattr(request,'param','')!='no_receipt':
            cur.execute('SELECT to_jsonb(p) FROM trading.qt_previews p WHERE preview_id=%s',(decision['preview_id'],));preview=cur.fetchone()[0]
            after=[dict(key=f['key'],quantity_exact=f['selected_quantity_exact'],**{name:f[name] for name in ('average_price_exact','daily_realized_pnl_exact','daily_unrealized_pnl_exact','last_update')}) for f in output['observation']['fills']]
            result=dict(schema_version='qt-desk-result/v1',decision_id=OLD,observation_id=OLD_INPUT,book_id='BOOK',source_day=str(prior),selected_book_digest=decision['selected_book_digest'],results=output['observation']['results'])
            old_attempt='b3000000-0000-4000-8000-000000000001'
            publication=dict(schema_version='qt-desk-publication/v1',decision_id=OLD,attempt_id=old_attempt,observation_id=OLD_INPUT,book_id='BOOK',source_day=str(prior),model_publication_id=OLD_MODEL,preview_payload_digest=preview['payload_digest'],read_set_digest=decision['read_set_digest'],selected_book_digest=decision['selected_book_digest'],published_book_digest=decision['selected_book_digest'],observation_digest=digest(output['observation']),results_digest=digest(result),before_accounting=preview['read_set_payload']['saved_accounting'],after_accounting=after,report_scope={})
            cur.execute("INSERT INTO trading.qt_desk_receipts(decision_id,attempt_id,status,published_book_digest,processed_at,publication_payload,report_eligibility_status,report_reason_codes) VALUES(%s,%s,'processed',%s,%s,%s,'unavailable','[]')",(OLD,old_attempt,decision['selected_book_digest'],now-timedelta(days=1,seconds=1),Json(publication)))
            cur.execute('INSERT INTO trading.qt_desk_results(decision_id,attempt_id,observation_id,content_digest,payload) VALUES(%s,%s,%s,%s,%s)',(OLD,old_attempt,OLD_INPUT,digest(result),Json(result)))
        for fill in output['observation']['fills']:
            key=fill['key'];cur.execute("UPDATE trading.positions SET quantity=%s,average_price=%s,daily_realized_pnl=0,daily_unrealized_pnl=0,last_update=%s WHERE portfolio_id='BOOK' AND strategy_id=%s AND strategy_name=%s AND symbol=%s AND date=%s AND portfolio_type='qt'",(fill['selected_quantity_exact'],fill['average_price_exact'],fill['last_update'],key['strategy_id'],key['strategy_name'],key['symbol'],prior))
        for row in output['live_results']:
            cur.execute("UPDATE trading.live_results SET daily_pnl=%s,daily_realized_pnl=0,daily_unrealized_pnl=0,daily_transaction_costs=%s,total_pnl=%s,current_portfolio_value=%s WHERE portfolio_id='BOOK' AND strategy_id=%s AND date=%s AND portfolio_type='qt'",(row['daily_pnl_exact'],row['daily_transaction_costs_exact'],row['total_pnl_exact'],row['current_portfolio_value_exact'],row['strategy_id'],prior))
            cur.execute("INSERT INTO trading.equity_curve(portfolio_id,strategy_id,timestamp,portfolio_type,equity) VALUES('BOOK',%s,%s,'qt',%s)",(row['strategy_id'],str(prior)+'T00:00:00Z',row['current_portfolio_value_exact']))
        for row in output['executions']:
            key=row['key'];cur.execute("INSERT INTO trading.executions(portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type,exec_id,order_id,side,quantity,price,execution_time,commissions_fees,implicit_price_impact,slippage_market_impact,total_transaction_costs,is_partial) VALUES('BOOK',%s,%s,%s,%s,'qt',%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,false)",tuple([key[x] for x in ('strategy_id','strategy_name','date','symbol')]+[row[x] for x in ('exec_id','order_id','side','quantity_exact','price_exact','execution_time','commissions_fees_exact','implicit_price_impact_exact','slippage_market_impact_exact','total_transaction_costs_exact')]))
        market=dict(schema_version='qt-accounting-market/v1',book_id='BOOK',source_day=str(today),previous_day=str(prior),valuation_time=str(today)+'T00:00:00Z',currency='USD',model_publication_id=MODEL,dataset_source_id='synthetic-bars',dataset_digest='1'*64,cost_config_source_id='synthetic-costs',cost_config_digest='2'*64,engine_build='local-qt-controlled',cost_config=source['cost_config'],instruments=[])
        for old in source['instruments']:
            row=deepcopy(old);row['price_model_number']='102';row['adv_model_number']=row.pop('adv_exact');row['volatility_multiplier_model_number']='1.1234567890123457';row.pop('price_exact');row.pop('volatility_multiplier_exact')
            row.update(instrument_type='FUTURE',price_time=str(prior)+'T00:00:00Z',source_id='synthetic-settlement',history_source_id='synthetic-feed-history',history_digest='3'*64,history_observation_count=1,history_complete=True,asset_lookup='exact_symbol');market['instruments'].append(row)
        evidence=dict(source_id=MARKET,producer_id='synthetic-execution',policy_version='execution-policy-v1',source_version='market-v1',as_of=(now-timedelta(seconds=1)).isoformat(),valid_until=(now+timedelta(hours=1)).isoformat(),payload=market)
    return conn,evidence,output

def test_actual_source_and_finalization_transition_are_atomic_and_idempotent(upstream):
    conn,evidence,original=upstream
    r=invoke('--market',payload=evidence);assert r.returncode==0,r.stdout+r.stderr
    before=upstream_state(conn)
    r=invoke('--finalize',OLD,FINAL,MARKET);assert r.returncode==0,r.stdout+r.stderr
    transition=json.loads(r.stdout)
    assert transition['after_financial']!=transition['before_financial']
    assert transition['engine_totals'][0]['gross_pnl_exact']=='30'
    after=upstream_state(conn);assert after!=before
    r=invoke('--finalize',OLD,FINAL,MARKET);assert r.returncode==0,r.stdout+r.stderr
    assert upstream_state(conn)==after
    with conn.cursor() as cur:
        cur.execute('SELECT payload FROM trading.desk_run_results WHERE decision_id=%s',(OLD,));assert cur.fetchone()[0]==original

def test_missing_mark_refuses_source_without_any_write(upstream):
    conn,evidence,_=upstream;evidence['payload']['instruments'][0].pop('price_model_number');before=upstream_state(conn)
    assert invoke('--market',payload=evidence).returncode!=0
    assert upstream_state(conn)==before

@pytest.mark.parametrize('upstream',['no_receipt'],indirect=True)
def test_detached_original_without_processed_receipt_cannot_finalize(upstream):
    conn,evidence,_=upstream;assert invoke('--market',payload=evidence).returncode==0
    before=upstream_state(conn)
    assert invoke('--finalize',OLD,FINAL,MARKET).returncode!=0
    assert upstream_state(conn)==before

def test_transition_insert_failure_rolls_back_all_prior_financial_updates(upstream):
    conn,evidence,_=upstream;assert invoke('--market',payload=evidence).returncode==0
    with conn.cursor() as cur:
        cur.execute("""CREATE FUNCTION trading.reject_test_finalization() RETURNS trigger LANGUAGE plpgsql AS $$
          BEGIN RAISE EXCEPTION 'owned transition failure'; END $$;
          CREATE TRIGGER reject_test_finalization BEFORE INSERT ON trading.qt_desk_finalizations
          FOR EACH ROW EXECUTE FUNCTION trading.reject_test_finalization()""")
    before=upstream_state(conn)
    assert invoke('--finalize',OLD,FINAL,MARKET).returncode!=0
    assert upstream_state(conn)==before

def test_disabled_current_policy_refuses_finalization_without_writes(upstream):
    conn,evidence,_=upstream;assert invoke('--market',payload=evidence).returncode==0
    with conn.cursor() as cur:
        cur.execute("UPDATE trading.qt_source_policies SET enabled=false,version=version+1 WHERE book_id='BOOK' AND purpose='execution'")
    before=upstream_state(conn)
    assert invoke('--finalize',OLD,FINAL,MARKET).returncode!=0
    assert upstream_state(conn)==before

def test_widened_generated_input_lease_rolls_back_every_current_output(upstream):
    conn,evidence,_=upstream;assert invoke('--market',payload=evidence).returncode==0
    r=invoke('--finalize',OLD,FINAL,MARKET);assert r.returncode==0,r.stdout+r.stderr
    with conn.cursor() as cur:
        cur.execute("""CREATE FUNCTION trading.widen_test_input_lease() RETURNS trigger LANGUAGE plpgsql AS $$
          BEGIN NEW.valid_until=NEW.valid_until+interval '1 day'; RETURN NEW; END $$;
          CREATE TRIGGER widen_test_input_lease BEFORE INSERT ON trading.qt_desk_accounting_inputs
          FOR EACH ROW WHEN (NEW.payload->>'schema_version'='qt-futures-accounting-input/v2')
          EXECUTE FUNCTION trading.widen_test_input_lease()""")
    before=upstream_state(conn)
    r=invoke('--sourced',DECISION,ATTEMPT,'d0000000-0000-4000-8000-000000000002',MARKET,'qt-finalization/'+FINAL)
    assert r.returncode!=0,r.stdout+r.stderr
    assert upstream_state(conn)==before

def test_current_receipt_insert_failure_also_rolls_back_assembled_input(upstream):
    conn,evidence,_=upstream;assert invoke('--market',payload=evidence).returncode==0
    r=invoke('--finalize',OLD,FINAL,MARKET);assert r.returncode==0,r.stdout+r.stderr
    with conn.cursor() as cur:
        cur.execute("""CREATE FUNCTION trading.reject_test_current_receipt() RETURNS trigger LANGUAGE plpgsql AS $$
          BEGIN RAISE EXCEPTION 'owned current receipt failure'; END $$;
          CREATE TRIGGER reject_test_current_receipt BEFORE INSERT ON trading.qt_desk_receipts
          FOR EACH ROW EXECUTE FUNCTION trading.reject_test_current_receipt()""")
    before=upstream_state(conn)
    r=invoke('--sourced',DECISION,ATTEMPT,'d0000000-0000-4000-8000-000000000003',MARKET,'qt-finalization/'+FINAL)
    assert r.returncode!=0,r.stdout+r.stderr
    assert upstream_state(conn)==before

def test_finalized_prior_flows_into_actual_confirmed_current_day_producer(upstream):
    conn,evidence,_=upstream
    assert invoke('--market',payload=evidence).returncode==0
    r=invoke('--finalize',OLD,FINAL,MARKET);assert r.returncode==0,r.stdout+r.stderr
    # Existing fixture's detached v1 input is deliberately not used here.
    sourced_input='d0000000-0000-4000-8000-000000000001'
    with conn.cursor() as cur:
        cur.execute('SELECT payload FROM trading.qt_desk_finalizations WHERE finalization_id=%s',(FINAL,));transition=cur.fetchone()[0]
    r=invoke('--sourced',DECISION,ATTEMPT,sourced_input,MARKET,'qt-finalization/'+FINAL)
    assert r.returncode==0,r.stdout+r.stderr
    with conn.cursor() as cur:
        cur.execute('SELECT payload FROM trading.qt_desk_accounting_inputs WHERE input_id=%s',(sourced_input,));source=cur.fetchone()[0]
        assert source['schema_version']=='qt-futures-accounting-input/v2'
        assert source['previous_totals'][0]['equity_exact']==transition['engine_totals'][0]['equity_exact']
        cur.execute('SELECT payload FROM trading.desk_run_results WHERE decision_id=%s',(DECISION,));output=cur.fetchone()[0]
        assert output['executions']==[]
        assert output['observation']['schema_version']=='qt-execution/v2'
    after=upstream_state(conn)
    r=invoke('--sourced',DECISION,ATTEMPT,sourced_input,MARKET,'qt-finalization/'+FINAL)
    assert r.returncode==0 and 'REPLAYED=1' in r.stdout,r.stdout+r.stderr
    assert upstream_state(conn)==after

@pytest.mark.parametrize('upstream',['archive_model_seed','archive_model_metadata','archive_evaluator',
    'archive_grant','archive_count','archive_currency','archive_fill_source'],indirect=True)
def test_archived_contradictions_cannot_be_blessed_by_prior_finalization(upstream):
    conn,evidence,_=upstream
    # Current-day market authority remains valid; only the archived original
    # contradiction changes, with all directly dependent hashes recomputed.
    assert invoke('--market',payload=evidence).returncode==0
    before=upstream_state(conn)
    r=invoke('--finalize',OLD,FINAL,MARKET)
    assert r.returncode!=0,r.stdout+r.stderr
    assert upstream_state(conn)==before
@pytest.mark.parametrize('column', ['as_of', 'valid_until', 'source_version'])
def test_archived_input_metadata_must_equal_original_execution_metadata(upstream, column):
    conn, evidence, _ = upstream
    assert invoke('--market', payload=evidence).returncode == 0
    # Deliberately corrupt an archived fixture as the owned test administrator.
    # Payload/digest and original observation remain untouched: the producer's
    # INSERT ... SELECT copied these row columns, so disagreement is impossible
    # for a legitimate original processing result.
    with conn.cursor() as cur:
        cur.execute('ALTER TABLE trading.qt_desk_accounting_inputs DISABLE TRIGGER USER')
        if column == 'source_version':
            cur.execute("UPDATE trading.qt_desk_accounting_inputs SET source_version='contradictory-original-version' WHERE input_id=%s", (OLD_INPUT,))
        else:
            assert column in {'as_of', 'valid_until'}
            cur.execute('UPDATE trading.qt_desk_accounting_inputs SET ' + column + '=' + column + " + interval '1 second' WHERE input_id=%s", (OLD_INPUT,))
        cur.execute('ALTER TABLE trading.qt_desk_accounting_inputs ENABLE TRIGGER USER')
    before = upstream_state(conn)
    result = invoke('--finalize', OLD, FINAL, MARKET)
    assert result.returncode != 0, result.stdout + result.stderr
    assert upstream_state(conn) == before
