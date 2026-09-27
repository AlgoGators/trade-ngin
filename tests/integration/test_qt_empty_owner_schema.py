"""Prospective DB identity/immutability tests, not a MODEL publication proof.

All inserted documents are explicitly synthetic SQL guard operands. Only a
separate actual typed writer test can prove executed empty owner publication.
"""
from copy import deepcopy
from hashlib import sha256
import json
import os
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
from threading import Barrier

import psycopg2
from psycopg2.extras import Json
import pytest
from test_runtime_control_schema import connection
from test_proposal_storage_migration import predecessor,apply

ROOT=Path(__file__).parents[2]
PUB='f0000000-0000-4000-8000-000000000001'
ATTEMPT='e0000000-0000-4000-8000-000000000001'
OWNERS=['empty-alpha']
CONFIG={'strategies':{'empty-alpha':{'enabled_live':True}}}

def digest(value):
    return sha256(json.dumps(value,sort_keys=True,separators=(',',':')).encode()).hexdigest()

@pytest.fixture()
def empty_schema(predecessor):
    # The existing fixture proves and owns route-less disposable DB transport.
    assert os.environ['ALGOLENS_TEST_DB'].startswith('host=/tmp/algolens-repair-pg-')
    with predecessor.cursor() as cur:
        cur.execute('SELECT current_database(),inet_server_addr()')
        database,address=cur.fetchone()
        assert database.startswith('algolens_test_') and address is None
    # Declare complete financial schema prerequisites before immutable owner
    # evidence. No enabled policy, capital source, grant, MODEL or QT run here.
    for name in ('015_qt_proposal_positions.sql','016_qt_exact_precision_and_seed_provenance.sql'):
        apply(predecessor,ROOT/'migrations'/name)
    workspace=next(p for p in ROOT.parents if (p/'.review/algolens-qt/algolens-api').is_dir())
    api=workspace/'.review/algolens-qt/algolens-api'
    with predecessor.cursor() as cur:
        cur.execute("CREATE SCHEMA IF NOT EXISTS auth; CREATE TABLE IF NOT EXISTS auth.users(id bigint PRIMARY KEY,role text NOT NULL DEFAULT 'general_member')")
    for name in ('003_qt_decision_workflow.sql','004_qt_governed_sources.sql','005_qt_evaluator_bundle.sql'):
        apply(predecessor,api/'migrations'/name)
    for name in ('017_desk_accounting_inputs.sql','018_qt_desk_finalization.sql','019_qt_equity_desk_accounting.sql',
                 '020_qt_position_accounting_precision.sql','021_qt_equity_finalization.sql'):
        apply(predecessor,ROOT/'migrations'/name)
    with predecessor.cursor() as cur:
        # Explicit typed synthetic fixture DDL for the actual writer/verifier
        # operands; no financial values or completion flags are fabricated.
        cur.execute("""ALTER TABLE trading.live_results
          ADD daily_pnl numeric(22,8),ADD daily_realized_pnl numeric(22,8),ADD daily_unrealized_pnl numeric(22,8),
          ADD daily_transaction_costs numeric(22,8),ADD total_realized_pnl numeric(22,8),
          ADD total_unrealized_pnl numeric(22,8),ADD total_transaction_costs numeric(22,8);
          ALTER TABLE trading.executions ADD strategy_name varchar(100),ADD date date,ADD symbol varchar(20),
          ADD exec_id text,ADD order_id text,ADD side text,ADD quantity numeric(20,8),ADD price numeric(20,8),
          ADD execution_time timestamptz,ADD commissions_fees numeric(22,8),ADD implicit_price_impact numeric(22,8),
          ADD slippage_market_impact numeric(22,8),ADD total_transaction_costs numeric(22,8),ADD is_partial boolean NOT NULL DEFAULT false;""")
    migration=ROOT/'migrations/023_qt_empty_model_owner_publication.sql'
    assert migration.is_file(),'Missing reviewed registration is setup failure, not behavioral RED'
    apply(predecessor,migration)
    return predecessor

def insert(cur,version,*,publication=PUB,attempt=ATTEMPT,book='EQ_BOOK',ordinal=1):
    common=(publication,attempt,book,'2026-09-26',ordinal)
    if version==2:
        cur.execute("""INSERT INTO trading.qt_empty_model_owner_publications
          (publication_id,attempt_id,portfolio_id,strategy_id,source_day,publication_version,
           schema_version,registry_id,registry_revision,configured_owner_names,configuration_snapshot,configuration_digest,
           fresh_empty_batches,inspection_capture,
           system_components,seed_digest,proposal_components,proposal_manifest_digest,
           qt_components,qt_digest,producer_version)
          VALUES(%s,%s,%s,'LIVE_EQUITY_MEAN_REVERSION',%s,%s,
           'qt-empty-model-owner-publication/v2','synthetic-registry',0,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,'synthetic-db-guard-only')""",
          common+(Json(OWNERS),Json(CONFIG),digest(CONFIG),
                  Json([dict(portfolio_id=book,strategy_id='LIVE_EQUITY_MEAN_REVERSION',
                      strategy_name=OWNERS[0],source_day='2026-09-26')]),
                  Json({}), # Deliberately noncertifying: schema guard tests only.
                  Json([]),digest({'seed_rows':[]}),
                  Json([]),digest({'proposal_rows':[]}),Json([]),digest({'qt_rows':[]})))
    else:
        rows=[dict(key=dict(portfolio_id=book,strategy_id='LIVE_EQUITY_MEAN_REVERSION',
            strategy_name='empty-alpha',date='2026-09-26',symbol='SYN',portfolio_type='system'),
            quantity_exact='1',average_price_exact='100')]
        cur.execute("""INSERT INTO trading.qt_model_seed_publications
          (publication_id,attempt_id,portfolio_id,strategy_id,source_day,publication_version,
           system_components,seed_digest,proposal_components,proposal_manifest_digest,producer_version)
          VALUES(%s,%s,%s,'LIVE_EQUITY_MEAN_REVERSION',%s,%s,%s,%s,%s,%s,'synthetic-db-guard-only')""",
          common+(Json(rows),digest({'seed_rows':rows}),Json([]),digest({'proposal_rows':[]})))

def state(conn):
    with conn.cursor() as cur:
        result=[]
        for table in ('qt_model_seed_publications','qt_empty_model_owner_publications'):
            cur.execute('SELECT to_jsonb(t) FROM trading.'+table+' t ORDER BY publication_id')
            result.append(deepcopy(cur.fetchall()))
        return result

def test_explicit_empty_document_is_stored_without_sentinel(empty_schema):
    with empty_schema.cursor() as cur:insert(cur,2)
    data=state(empty_schema)
    assert data[0]==[] and len(data[1])==1
    assert data[1][0][0]['system_components']==[]
    assert data[1][0][0]['configured_owner_names']==OWNERS

@pytest.mark.parametrize('first',[1,2])
@pytest.mark.parametrize('collision',['publication','attempt','version'])
def test_old_and_new_insertions_share_identity_namespace(empty_schema,first,collision):
    with empty_schema.cursor() as cur:insert(cur,first)
    before=state(empty_schema)
    operands=dict(publication='f0000000-0000-4000-8000-000000000002',
                  attempt='e0000000-0000-4000-8000-000000000002',ordinal=2)
    if collision=='publication':operands['publication']=PUB
    elif collision=='attempt':operands['attempt']=ATTEMPT
    else:operands['ordinal']=1
    with pytest.raises(psycopg2.Error,match='cross-version identity conflict'):
        with empty_schema.cursor() as cur:insert(cur,3-first,**operands)
    assert state(empty_schema)==before

@pytest.mark.parametrize('first',[1,2])
@pytest.mark.parametrize('collision',['publication','attempt'])
def test_cross_book_identity_collision_refuses(empty_schema,first,collision):
    with empty_schema.cursor() as cur:insert(cur,first)
    before=state(empty_schema)
    operands=dict(book='OTHER',publication=PUB if collision=='publication' else 'f0000000-0000-4000-8000-000000000002',
        attempt=ATTEMPT if collision=='attempt' else 'e0000000-0000-4000-8000-000000000002')
    with pytest.raises(psycopg2.Error,match='cross-version identity conflict'):
        with empty_schema.cursor() as cur:insert(cur,3-first,**operands)
    assert state(empty_schema)==before

@pytest.mark.parametrize('version',[1,2])
@pytest.mark.parametrize('operation',['UPDATE','DELETE','TRUNCATE'])
def test_both_versions_preserve_immutable_rows(empty_schema,version,operation):
    with empty_schema.cursor() as cur:insert(cur,version)
    before=state(empty_schema)
    table='qt_model_seed_publications' if version==1 else 'qt_empty_model_owner_publications'
    sql=f"UPDATE trading.{table} SET producer_version='changed'" if operation=='UPDATE' else f'{operation} '+('FROM ' if operation=='DELETE' else '')+f'trading.{table}'
    with pytest.raises(psycopg2.Error,match='immutable'):
        with empty_schema.cursor() as cur:cur.execute(sql)
    assert state(empty_schema)==before

def test_v1_empty_seed_stays_refused(empty_schema):
    before=state(empty_schema)
    with pytest.raises(psycopg2.Error,match='system_components_check'):
        with empty_schema.cursor() as cur:
            cur.execute("""INSERT INTO trading.qt_model_seed_publications
             (publication_id,portfolio_id,strategy_id,source_day,publication_version,system_components,
              seed_digest,proposal_components,proposal_manifest_digest,producer_version)
             VALUES(%s,'EQ_BOOK','LIVE_EQUITY_MEAN_REVERSION','2026-09-26',1,'[]',%s,'[]',%s,'synthetic-db-guard-only')""",
             (PUB,digest({'seed_rows':[]}),digest({'proposal_rows':[]})))
    assert state(empty_schema)==before

@pytest.mark.parametrize('collision',['publication','attempt','version'])
def test_concurrent_versions_cannot_share_global_identity(empty_schema,collision):
    barrier=Barrier(2)
    def worker(version):
        conn=psycopg2.connect(os.environ['ALGOLENS_TEST_DB'])
        try:
            with conn.cursor() as cur:
                cur.execute("SET statement_timeout='8s'; SET lock_timeout='6s'")
                barrier.wait(timeout=5)
                insert(cur,version,book='EQ_BOOK' if version==1 or collision=='version' else 'OTHER',
                    publication=PUB if collision=='publication' or version==1 else 'f0000000-0000-4000-8000-000000000002',
                    attempt=ATTEMPT if collision=='attempt' or version==1 else 'e0000000-0000-4000-8000-000000000002')
            conn.commit();return 'inserted'
        except psycopg2.Error as exc:
            conn.rollback();assert 'cross-version identity conflict' in str(exc);return 'refused'
        finally:conn.close()
    with ThreadPoolExecutor(max_workers=2) as workers:
        statuses=list(workers.map(worker,(1,2)))
    assert sorted(statuses)==['inserted','refused']
    assert sum(len(rows) for rows in state(empty_schema))==1
