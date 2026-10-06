"""Migration 015 against the owned, network-none PostgreSQL fixture."""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import os
import time

import psycopg2
import pytest

from test_runtime_control_schema import connection
from test_stream_storage import streams


MIGRATION = Path(__file__).parents[2] / "migrations/015_qt_proposal_positions.sql"
ROLLBACK = Path(__file__).parents[2] / "migrations/015_qt_proposal_positions_rollback.sql"


def normalize_positions_shape(connection):
    # The shared 013 fixture intentionally has a minimal positions relation.
    # Give this task the actual post-001/007 catalog shape without changing it.
    with connection.cursor() as cur:
        cur.execute("""
          ALTER TABLE trading.positions ALTER strategy_id SET NOT NULL,
            ALTER portfolio_id SET NOT NULL, ALTER strategy_name SET NOT NULL,
            ALTER date SET NOT NULL, ALTER symbol SET NOT NULL,
            ALTER portfolio_type SET NOT NULL,
            ALTER portfolio_type SET DEFAULT 'system';
          ALTER TABLE trading.positions
            ALTER symbol TYPE varchar(20),
            ALTER strategy_id TYPE varchar(50),
            ALTER strategy_name TYPE varchar(100),
            ALTER portfolio_id TYPE varchar(100),
            ALTER quantity TYPE numeric(20,6),
            ALTER average_price TYPE numeric(20,6),
            ALTER daily_unrealized_pnl TYPE numeric(20,6),
            ALTER daily_realized_pnl TYPE numeric(20,6),
            ALTER quantity SET NOT NULL,
            ALTER average_price SET NOT NULL,
            ALTER daily_unrealized_pnl SET NOT NULL,
            ALTER daily_realized_pnl SET NOT NULL,
            ALTER last_update SET NOT NULL,
            ALTER updated_at SET DEFAULT CURRENT_TIMESTAMP;
          DO $fixture$ DECLARE old_key text; BEGIN
            SELECT conname INTO STRICT old_key FROM pg_constraint
            WHERE conrelid='trading.positions'::regclass AND contype='u';
            EXECUTE format('ALTER TABLE trading.positions DROP CONSTRAINT %I',old_key);
          END $fixture$;
          ALTER TABLE trading.positions ADD CONSTRAINT positions_pkey
            PRIMARY KEY (portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type);
          ALTER TABLE trading.positions ADD CONSTRAINT positions_portfolio_type_check
            CHECK (portfolio_type IN
              ('system','qt','benchmark','benchmark_rebench','benchmark_frozen_shadow'));
        """)


@pytest.fixture()
def predecessor(connection):
    normalize_positions_shape(connection)
    return connection


def apply(conn, path):
    with conn.cursor() as cur:
        cur.execute(path.read_text())


def rows(conn):
    with conn.cursor() as cur:
        cur.execute("SELECT to_jsonb(p)::text FROM trading.positions p ORDER BY portfolio_type,symbol")
        return cur.fetchall()


def catalog(conn):
    with conn.cursor() as cur:
        cur.execute("""SELECT
          (SELECT pg_get_constraintdef(oid) FROM pg_constraint
            WHERE conrelid='trading.positions'::regclass AND conname='positions_portfolio_type_check'),
          (SELECT pg_get_constraintdef(oid) FROM pg_constraint
            WHERE conrelid='trading.positions'::regclass AND contype='p'),
          (SELECT (l.lanname,p.pronargs,p.prorettype::regtype::text,md5(p.prosrc))
            FROM pg_proc p JOIN pg_language l ON l.oid=p.prolang
            WHERE p.oid='trading.fence_runtime_publication_row()'::regprocedure),
          (SELECT array_agg((n.nspname,r.relname,t.tgenabled,t.tgtype,t.tgqual IS NULL,
              t.tgattr::text,t.tgnargs,encode(t.tgargs,'hex'))::text
              ORDER BY n.nspname,r.relname)
            FROM pg_trigger t JOIN pg_class r ON r.oid=t.tgrelid
            JOIN pg_namespace n ON n.oid=r.relnamespace
            WHERE t.tgname='runtime_publication_fence'),
          (SELECT pg_get_expr(d.adbin,d.adrelid) FROM pg_attrdef d JOIN pg_attribute a
            ON a.attrelid=d.adrelid AND a.attnum=d.adnum
            WHERE a.attrelid='trading.positions'::regclass AND a.attname='portfolio_type'),
          (SELECT array_agg(attname||':'||format_type(atttypid,atttypmod)||':'||attnotnull
             ORDER BY attname) FROM pg_attribute WHERE attrelid='trading.positions'::regclass
             AND attnum>0 AND NOT attisdropped)""")
        return cur.fetchone(), rows(conn)


def failed_migration_is_atomic(conn, path):
    before=catalog(conn)
    with pytest.raises(psycopg2.Error):
        apply(conn,path)
    with conn.cursor() as cur:
        cur.execute('ROLLBACK')
    assert catalog(conn)==before


def seed_catalog_sentinel(conn):
    with conn.cursor() as cur:
        cur.execute("""INSERT INTO trading.positions
          (strategy_id,strategy_name,portfolio_id,date,symbol,portfolio_type,
           quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
          VALUES ('LIVE_TREND','TREND','BOOK','2026-09-22','ES','system',
                  12,101,1,-2,'2026-09-22')""")


FENCED_RELATIONS = ('positions','risk_limits','live_results','equity_curve',
                    'executions','signals','live_run_metadata','run_inputs')


def replace_fence(conn, relation, *, when=None, update_of=None, arguments=''):
    assert relation in FENCED_RELATIONS
    assert update_of in (None,'quantity')
    assert when in (None,'false')
    assert arguments in ('',"'ignored'")
    update = 'UPDATE OF quantity' if update_of else 'UPDATE'
    predicate = 'WHEN (false)' if when else ''
    with conn.cursor() as cur:
        cur.execute(f"""DROP TRIGGER runtime_publication_fence ON trading.{relation};
          CREATE TRIGGER runtime_publication_fence
          BEFORE INSERT OR {update} OR DELETE ON trading.{relation}
          FOR EACH ROW {predicate}
          EXECUTE FUNCTION trading.fence_runtime_publication_row({arguments})""")


def test_upgrade_preserves_rows_and_enables_only_proposal_positions(predecessor):
    with predecessor.cursor() as cur:
        cur.execute("""INSERT INTO trading.positions
          (strategy_id,portfolio_id,strategy_name,date,symbol,quantity,portfolio_type,
           average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
          VALUES ('LIVE_TREND','BOOK','TREND','2026-09-22','ES',12,'system',1,0,0,'2026-09-22')""")
    before = rows(predecessor)
    apply(predecessor, MIGRATION)
    apply(predecessor, MIGRATION)
    assert rows(predecessor) == before
    with predecessor.cursor() as cur:
        cur.execute("""INSERT INTO trading.positions
          (strategy_id,portfolio_id,strategy_name,date,symbol,quantity,portfolio_type,
           average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
          SELECT 'LIVE_TREND','BOOK','TREND','2026-09-22','ES',12,s,1,0,0,'2026-09-22'::timestamptz
          FROM unnest(ARRAY['qt','qt_proposal','benchmark','benchmark_rebench','benchmark_frozen_shadow']) s""")
        cur.execute("SELECT portfolio_type FROM trading.positions ORDER BY portfolio_type")
        assert [row[0] for row in cur.fetchall()] == [
            'benchmark','benchmark_frozen_shadow','benchmark_rebench','qt','qt_proposal','system']
        with pytest.raises(psycopg2.Error):
            cur.execute("""INSERT INTO trading.positions
              (strategy_id,portfolio_id,strategy_name,date,symbol,portfolio_type,
               quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
              VALUES ('LIVE_TREND','BOOK','TREND','2026-09-22','ES','qt_proposal',1,1,0,0,'2026-09-22')""")
        for stream in ('unknown', None):
            with pytest.raises(psycopg2.Error):
                cur.execute("""INSERT INTO trading.positions
                  (strategy_id,portfolio_id,strategy_name,date,symbol,portfolio_type,
                   quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
                  VALUES ('LIVE_TREND','BOOK','TREND','2026-09-22','NQ',%s,1,1,0,0,'2026-09-22')""", (stream,))
        with pytest.raises(psycopg2.Error):
            cur.execute("""INSERT INTO trading.equity_curve
              (strategy_id,portfolio_id,timestamp,equity,portfolio_type)
              VALUES ('LIVE_TREND','BOOK','2026-09-22',1,'qt_proposal')""")
        with pytest.raises(psycopg2.Error):
            cur.execute("""INSERT INTO trading.live_results
              (strategy_id,portfolio_id,date,portfolio_type)
              VALUES ('LIVE_TREND','BOOK','2026-09-22','qt_proposal')""")
        with pytest.raises(psycopg2.Error):
            cur.execute("""INSERT INTO trading.executions
              (strategy_id,portfolio_id,portfolio_type)
              VALUES ('LIVE_TREND','BOOK','qt_proposal')""")


def test_upgrade_on_013_014_baseline_preserves_operational_rows_and_constraints(streams):
    normalize_positions_shape(streams)
    with streams.cursor() as cur:
        before={}
        for table in ('live_results','executions','equity_curve'):
            cur.execute(f'SELECT to_jsonb(t)::text FROM trading.{table} t ORDER BY to_jsonb(t)::text')
            before[table]=cur.fetchall()
    apply(streams,MIGRATION)
    apply(streams,MIGRATION)
    with streams.cursor() as cur:
        for table in before:
            cur.execute(f'SELECT to_jsonb(t)::text FROM trading.{table} t ORDER BY to_jsonb(t)::text')
            assert cur.fetchall()==before[table]
            cur.execute("""SELECT pg_get_constraintdef(oid) FROM pg_constraint
              WHERE conrelid=%s::regclass AND conname=%s""",
              ('trading.'+table,table+'_portfolio_type_check'))
            # 015 never broadens non-position stream constraints.
            constraint=cur.fetchone()
            if constraint:
                assert 'qt_proposal' not in constraint[0]


def test_rollback_refuses_rows_without_discarding_them(predecessor):
    apply(predecessor, MIGRATION)
    with predecessor.cursor() as cur:
        cur.execute("""INSERT INTO trading.positions
          (strategy_id,portfolio_id,strategy_name,date,symbol,portfolio_type,
           quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
          VALUES ('LIVE_TREND','BOOK','TREND','2026-09-22','ES','qt_proposal',1,1,0,0,'2026-09-22')""")
    failed_migration_is_atomic(predecessor,ROLLBACK)
    with predecessor.cursor() as cur:
        cur.execute("DELETE FROM trading.positions WHERE portfolio_type='qt_proposal'")
    apply(predecessor, ROLLBACK)
    with predecessor.cursor() as cur:
        with pytest.raises(psycopg2.Error):
            cur.execute("""INSERT INTO trading.positions
              (strategy_id,portfolio_id,strategy_name,date,symbol,portfolio_type,
               quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
              VALUES ('LIVE_TREND','BOOK','TREND','2026-09-22','ES','qt_proposal',1,1,0,0,'2026-09-22')""")


def test_missing_fenced_relation_refuses_upgrade_without_touching_positions(predecessor):
    with predecessor.cursor() as cur:
        cur.execute('DROP TABLE trading.signals')
    before=catalog(predecessor)
    with pytest.raises(psycopg2.Error):
        apply(predecessor,MIGRATION)
    with predecessor.cursor() as cur:
        cur.execute('ROLLBACK')
    assert catalog(predecessor)==before


@pytest.mark.parametrize('damage',[
    'missing_trigger','disabled_trigger','wrong_function','wrong_check','wrong_key',
    'missing_default','widened_only','function_only',
    'conditional_trigger','column_filtered_trigger','argument_trigger',
    'unsupported_function_language'])
def test_preflight_refuses_unsupported_predecessor_without_changes(predecessor,damage):
    seed_catalog_sentinel(predecessor)
    with predecessor.cursor() as cur:
        if damage=='missing_trigger':
            cur.execute('DROP TRIGGER runtime_publication_fence ON trading.positions')
        elif damage=='disabled_trigger':
            cur.execute('ALTER TABLE trading.positions DISABLE TRIGGER runtime_publication_fence')
        elif damage=='wrong_function':
            cur.execute("""CREATE OR REPLACE FUNCTION trading.fence_runtime_publication_row()
              RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RETURN NEW; END $$""")
        elif damage=='wrong_check':
            cur.execute("""ALTER TABLE trading.positions DROP CONSTRAINT positions_portfolio_type_check;
              ALTER TABLE trading.positions ADD CONSTRAINT positions_portfolio_type_check
              CHECK (portfolio_type IN ('system','qt'))""")
        elif damage=='widened_only':
            cur.execute("""ALTER TABLE trading.positions DROP CONSTRAINT positions_portfolio_type_check;
              ALTER TABLE trading.positions ADD CONSTRAINT positions_portfolio_type_check
              CHECK (portfolio_type IN ('system','qt','benchmark','benchmark_rebench',
                'benchmark_frozen_shadow','qt_proposal'))""")
        elif damage=='function_only':
            ddl=MIGRATION.read_text().split(
                'CREATE OR REPLACE FUNCTION trading.fence_runtime_publication_row()',1)[1]
            cur.execute('CREATE OR REPLACE FUNCTION trading.fence_runtime_publication_row()'+
                        ddl.split('END $$;',1)[0]+'END $$;')
        elif damage=='wrong_key':
            cur.execute("""ALTER TABLE trading.positions DROP CONSTRAINT positions_pkey;
              ALTER TABLE trading.positions ADD CONSTRAINT positions_pkey
              PRIMARY KEY (portfolio_id,strategy_id,strategy_name,date,symbol)""")
        elif damage=='missing_default':
            cur.execute('ALTER TABLE trading.positions ALTER portfolio_type DROP DEFAULT')
        elif damage=='conditional_trigger':
            replace_fence(predecessor,'positions',when='false')
        elif damage=='column_filtered_trigger':
            replace_fence(predecessor,'positions',update_of='quantity')
        elif damage=='argument_trigger':
            replace_fence(predecessor,'positions',arguments="'ignored'")
        elif damage=='unsupported_function_language':
            cur.execute("""SELECT to_regprocedure('pg_catalog.suppress_redundant_updates_trigger()')""")
            assert cur.fetchone()[0] is not None
            cur.execute("""CREATE OR REPLACE FUNCTION trading.fence_runtime_publication_row()
              RETURNS trigger LANGUAGE internal AS 'suppress_redundant_updates_trigger'""")
            cur.execute("""SELECT l.lanname FROM pg_proc p JOIN pg_language l ON l.oid=p.prolang
              WHERE p.oid='trading.fence_runtime_publication_row()'::regprocedure""")
            assert cur.fetchone()==('internal',)
    failed_migration_is_atomic(predecessor,MIGRATION)


@pytest.mark.parametrize('relation',FENCED_RELATIONS)
def test_upgrade_refuses_conditional_fence_on_any_attached_relation(predecessor,relation):
    seed_catalog_sentinel(predecessor)
    replace_fence(predecessor,relation,when='false')
    failed_migration_is_atomic(predecessor,MIGRATION)


@pytest.mark.parametrize('damage',['wrong_function','disabled_trigger','wrong_check',
                                  'conditional_trigger','column_filtered_trigger',
                                  'argument_trigger','unsupported_function_language'])
def test_rollback_refuses_unknown_post015_shape_without_changes(predecessor,damage):
    apply(predecessor,MIGRATION)
    seed_catalog_sentinel(predecessor)
    with predecessor.cursor() as cur:
        if damage=='wrong_function':
            cur.execute("""CREATE OR REPLACE FUNCTION trading.fence_runtime_publication_row()
              RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN RETURN NEW; END $$""")
        elif damage=='disabled_trigger':
            cur.execute('ALTER TABLE trading.positions DISABLE TRIGGER runtime_publication_fence')
        elif damage=='conditional_trigger':
            replace_fence(predecessor,'positions',when='false')
        elif damage=='column_filtered_trigger':
            replace_fence(predecessor,'positions',update_of='quantity')
        elif damage=='argument_trigger':
            replace_fence(predecessor,'positions',arguments="'ignored'")
        elif damage=='unsupported_function_language':
            cur.execute("""SELECT to_regprocedure('pg_catalog.suppress_redundant_updates_trigger()')""")
            assert cur.fetchone()[0] is not None
            cur.execute("""CREATE OR REPLACE FUNCTION trading.fence_runtime_publication_row()
              RETURNS trigger LANGUAGE internal AS 'suppress_redundant_updates_trigger'""")
            cur.execute("""SELECT l.lanname FROM pg_proc p JOIN pg_language l ON l.oid=p.prolang
              WHERE p.oid='trading.fence_runtime_publication_row()'::regprocedure""")
            assert cur.fetchone()==('internal',)
        else:
            cur.execute("""ALTER TABLE trading.positions DROP CONSTRAINT positions_portfolio_type_check;
              ALTER TABLE trading.positions ADD CONSTRAINT positions_portfolio_type_check
              CHECK (portfolio_type IN ('system','qt','qt_proposal'))""")
    failed_migration_is_atomic(predecessor,ROLLBACK)


@pytest.mark.parametrize('relation',FENCED_RELATIONS)
def test_rollback_refuses_conditional_fence_on_any_attached_relation(predecessor,relation):
    apply(predecessor,MIGRATION)
    seed_catalog_sentinel(predecessor)
    replace_fence(predecessor,relation,when='false')
    failed_migration_is_atomic(predecessor,ROLLBACK)


def test_rollback_waits_for_racing_insert_then_preserves_proposal(predecessor):
    apply(predecessor,MIGRATION)
    writer=psycopg2.connect(os.environ['ALGOLENS_TEST_DB'])
    reader=psycopg2.connect(os.environ['ALGOLENS_TEST_DB'])
    reader.autocommit=True
    try:
        with writer.cursor() as cur:
            cur.execute("""INSERT INTO trading.positions
              (strategy_id,portfolio_id,strategy_name,date,symbol,portfolio_type,
               quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
              VALUES ('LIVE_TREND','BOOK','TREND','2026-09-22','ES','qt_proposal',
                      7,101,1,2,'2026-09-22')""")
        def rollback_attempt():
            try:
                apply(reader,ROLLBACK)
                return False
            except psycopg2.Error:
                with reader.cursor() as cur:
                    cur.execute('ROLLBACK')
                return True
        with ThreadPoolExecutor(max_workers=1) as pool:
            future=pool.submit(rollback_attempt)
            time.sleep(.2)
            assert not future.done(), 'rollback did not wait for the proposal insert'
            writer.commit()
            assert future.result(timeout=15)
        with predecessor.cursor() as cur:
            cur.execute("SELECT quantity FROM trading.positions WHERE portfolio_type='qt_proposal'")
            assert cur.fetchall()==[(7,)]
    finally:
        writer.rollback()
        writer.close()
        reader.close()
