"""014 upgrade/preflight behavior, synthetic tables and owned socket only."""
import os
from pathlib import Path
import psycopg2
import pytest

MIGRATION=Path(__file__).parents[2]/'migrations/014_execution_result_streams.sql'

@pytest.fixture()
def legacy():
    dsn=os.environ.get('ALGOLENS_TEST_DB','')
    assert dsn.startswith('host=/tmp/algolens-repair-pg-') and ' dbname=algolens_test_' in dsn
    assert 'hostaddr' not in dsn and 'service=' not in dsn
    conn=psycopg2.connect(dsn);conn.autocommit=True
    with conn.cursor() as cur:
        cur.execute('DROP SCHEMA IF EXISTS trading CASCADE; CREATE SCHEMA trading')
        cur.execute('''
            CREATE TABLE trading.executions (
              portfolio_id text NOT NULL,strategy_id text NOT NULL,strategy_name text NOT NULL,
              date date NOT NULL,exec_id varchar(50) NOT NULL,order_id varchar(50) NOT NULL,
              execution_time timestamptz,quantity numeric,
              CONSTRAINT executions_pkey PRIMARY KEY(portfolio_id,strategy_id,strategy_name,date,exec_id));
            CREATE TABLE trading.live_results (
              id bigserial PRIMARY KEY,portfolio_id text,strategy_id text NOT NULL,date date NOT NULL,
              total_pnl numeric,
              CONSTRAINT live_results_portfolio_strategy_date_key UNIQUE(portfolio_id,strategy_id,date));
            INSERT INTO trading.executions VALUES ('BOOK','ENGINE','PART','2026-09-22','fill','order','2026-09-22',7);
            INSERT INTO trading.live_results(portfolio_id,strategy_id,date,total_pnl)
              VALUES ('BOOK','ENGINE','2026-09-22',12),(NULL,'ENGINE','2026-09-22',9);
        ''')
    yield conn
    conn.close()

def apply(cur):
    assert MIGRATION.exists(),'014 migration is missing'
    cur.execute(MIGRATION.read_text())

def test_legacy_rows_preserved_and_stream_keys_coexist(legacy):
    with legacy.cursor() as cur:
        cur.execute('SELECT row_to_json(e) FROM trading.executions e'); before=cur.fetchall()
        cur.execute('SELECT row_to_json(r) FROM trading.live_results r ORDER BY id'); before_results=cur.fetchall()
        apply(cur);apply(cur)
        cur.execute("SELECT to_jsonb(e)-'portfolio_type' FROM trading.executions e")
        assert cur.fetchall()==before
        cur.execute("SELECT to_jsonb(r)-'portfolio_type' FROM trading.live_results r ORDER BY id")
        assert cur.fetchall()==before_results
        cur.execute("SELECT portfolio_type FROM trading.executions UNION SELECT portfolio_type FROM trading.live_results")
        assert cur.fetchall()==[('system',)]
        cur.execute("INSERT INTO trading.executions SELECT portfolio_id,strategy_id,strategy_name,date,exec_id,order_id,execution_time,quantity,'qt' FROM trading.executions")
        cur.execute("INSERT INTO trading.live_results(portfolio_id,strategy_id,date,total_pnl,portfolio_type) VALUES ('BOOK','ENGINE','2026-09-22',99,'qt')")
        with pytest.raises(psycopg2.Error):
            cur.execute("INSERT INTO trading.executions SELECT * FROM trading.executions WHERE portfolio_type='qt'")
        with pytest.raises(psycopg2.Error):
            cur.execute("INSERT INTO trading.live_results(portfolio_id,strategy_id,date,portfolio_type) VALUES ('BOOK','ENGINE','2026-09-22','qt')")
        for invalid in ('unknown',None):
            with pytest.raises(psycopg2.Error):
                cur.execute("INSERT INTO trading.live_results(portfolio_id,strategy_id,date,portfolio_type) VALUES ('OTHER','ENGINE','2026-09-22',%s)",(invalid,))
        for book in (None,'','   '):
            with pytest.raises(psycopg2.Error):
                cur.execute("INSERT INTO trading.live_results(portfolio_id,strategy_id,date,portfolio_type) VALUES (%s,'ENGINE','2026-09-22','qt')",(book,))
            with pytest.raises(psycopg2.Error):
                cur.execute("INSERT INTO trading.executions(portfolio_id,strategy_id,strategy_name,date,exec_id,order_id,portfolio_type) VALUES (%s,'ENGINE','PART','2026-09-22','new','order','qt')",(book,))

@pytest.mark.parametrize('damage',[
    "CREATE UNIQUE INDEX surprise ON trading.executions(exec_id)",
    "CREATE INDEX surprise_partial ON trading.executions(exec_id) WHERE quantity>0",
    "ALTER TABLE trading.live_results ALTER COLUMN date DROP NOT NULL",
    "CREATE TABLE trading.dependent(result_id bigint REFERENCES trading.live_results(id))",
    "ALTER TABLE trading.live_results ADD portfolio_type text; UPDATE trading.live_results SET portfolio_type=NULL",
    "ALTER TABLE trading.live_results ADD portfolio_type text NOT NULL DEFAULT 'wrong'",
    "ALTER TABLE trading.live_results DROP CONSTRAINT live_results_portfolio_strategy_date_key; INSERT INTO trading.live_results(portfolio_id,strategy_id,date) VALUES ('BOOK','ENGINE','2026-09-22')",
    "DROP TABLE trading.executions",
])
def test_preflight_refuses_unreviewed_shapes_without_partial_schema_change(legacy,damage):
    with legacy.cursor() as cur:
        cur.execute(damage)
        cur.execute("SELECT table_name,column_name,data_type,is_nullable,column_default FROM information_schema.columns WHERE table_schema='trading' ORDER BY table_name,ordinal_position")
        before=cur.fetchall()
        assert MIGRATION.exists(),'014 migration is missing'
        with pytest.raises(psycopg2.Error): apply(cur)
        legacy.rollback()
        cur.execute('ROLLBACK')
        cur.execute("SELECT table_name,column_name,data_type,is_nullable,column_default FROM information_schema.columns WHERE table_schema='trading' ORDER BY table_name,ordinal_position")
        assert cur.fetchall()==before
