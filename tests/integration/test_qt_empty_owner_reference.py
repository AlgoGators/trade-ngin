"""Owned synthetic SQL namespace/reference controls; no MODEL certification.

These exercise actual migration guards with explicit synthetic row operands.
The repeatable-read case deliberately freezes its snapshot before the winning
insert commits, and proves the loser reached the shared identity lock.
"""
from concurrent.futures import ThreadPoolExecutor
import os
from time import monotonic, sleep

import psycopg2
from psycopg2.extras import Json
import pytest
from test_runtime_control_schema import connection
from test_proposal_storage_migration import predecessor
from test_qt_empty_owner_schema import empty_schema, insert, state, PUB


@pytest.mark.parametrize('first', [1, 2])
def test_preexisting_repeatable_read_snapshot_cannot_duplicate_union_identity(empty_schema, first):
    winner = psycopg2.connect(os.environ['ALGOLENS_TEST_DB'])
    loser = psycopg2.connect(os.environ['ALGOLENS_TEST_DB'])
    pool = ThreadPoolExecutor(max_workers=1)
    future = None
    try:
        with winner.cursor() as cur:
            cur.execute("SET statement_timeout='10s'; SET lock_timeout='8s'")
            insert(cur, first, book='EQ_BOOK')
        loser.set_session(isolation_level='REPEATABLE READ')
        with loser.cursor() as cur:
            cur.execute("SET statement_timeout='10s'; SET lock_timeout='8s'")
            # This snapshot cannot see winner's uncommitted publication.
            cur.execute("SELECT count(*) FROM trading.qt_model_seed_publications WHERE publication_id=%s", (PUB,))
            assert cur.fetchone()[0] == 0
            cur.execute('SELECT pg_backend_pid()')
            pid = cur.fetchone()[0]

        def competing_insert():
            try:
                with loser.cursor() as cur:
                    insert(cur, 3-first, book='OTHER',
                           attempt='e0000000-0000-4000-8000-000000000002')
                loser.commit()
                return 'inserted'
            except psycopg2.Error as exc:
                loser.rollback()
                assert ('cross-version identity conflict' in str(exc) or
                        'qt023 writer isolation unsupported' in str(exc)), str(exc)
                return 'refused'

        future = pool.submit(competing_insert)
        deadline = monotonic()+6
        blocked = False
        while monotonic() < deadline and not future.done():
            with empty_schema.cursor() as cur:
                cur.execute('SELECT wait_event_type,wait_event FROM pg_stat_activity WHERE pid=%s', (pid,))
                wait = cur.fetchone()
            if wait == ('Lock', 'advisory'):
                blocked = True
                break
            sleep(0.01)
        # Once the fixed guard refuses unsupported isolation before locking,
        # the refusal itself is a valid control. Before the fix it MUST block.
        if not blocked:
            assert future.done() and future.result(timeout=1) == 'refused'
        winner.commit()
        assert future.result(timeout=12) == 'refused', 'stale snapshot admitted duplicate immutable publication UUID'
        assert sum(len(rows) for rows in state(empty_schema)) == 1
    finally:
        winner.rollback()
        try:
            if future is not None:
                future.result(timeout=12)
        finally:
            pool.shutdown(wait=True)
            loser.close()
            winner.close()


def market(cur, *, publication=PUB, book='EQ_BOOK', day='2026-09-26', empty=True):
    payload = dict(schema_version='qt-equity-accounting-market-empty-owner/v2' if empty else 'qt-accounting-market/v1',
                   book_id=book, source_day=day, model_publication_id=publication,
                   instruments=[], day_mode='open', calculation_version='qt-equity-main08b15c/v1', currency='USD')
    cur.execute("""INSERT INTO trading.qt_desk_market_sources
       (source_id,book_id,source_day,model_publication_id,producer_id,policy_version,policy_revision,
        source_version,as_of,valid_until,content_digest,payload)
       VALUES('d0000000-0000-4000-8000-000000000001',%s,%s,%s,'synthetic-db-guard-only',
         'synthetic-policy',1,'synthetic-market', '2026-09-26T00:00:00Z','2026-09-27T00:00:00Z',%s,%s)""",
        (book, day, publication, 'a'*64, Json(payload)))


@pytest.mark.parametrize('version', [1, 2])
def test_market_references_exact_immutable_union_member_without_dummy_seed(empty_schema, version):
    with empty_schema.cursor() as cur:
        cur.execute("SHOW transaction_isolation")
        assert cur.fetchone()[0] == 'read committed'
        insert(cur, version)
        market(cur, empty=version == 2)
        cur.execute('SELECT model_publication_id::text FROM trading.qt_desk_market_sources')
        assert cur.fetchall() == [(PUB,)]
    publications = state(empty_schema)
    assert len(publications[version-1]) == 1 and publications[2-version] == []


@pytest.mark.parametrize('case', ['missing', 'wrong_book', 'wrong_day', 'v1_as_empty', 'rewrite'])
def test_market_union_reference_refuses_incomplete_or_wrong_authority(empty_schema, case):
    with empty_schema.cursor() as cur:
        if case != 'missing':
            insert(cur, 1 if case == 'v1_as_empty' else 2)
        if case == 'rewrite':
            cur.execute("CREATE RULE synthetic_rewrite AS ON UPDATE TO trading.qt_empty_model_owner_publications DO INSTEAD NOTHING")
    before = state(empty_schema)
    with pytest.raises(psycopg2.Error, match='qt023 (market model reference unavailable|reference rewrite unsupported)'):
        with empty_schema.cursor() as cur:
            market(cur, book='OTHER' if case == 'wrong_book' else 'EQ_BOOK',
                   day='2026-09-27' if case == 'wrong_day' else '2026-09-26')
    with empty_schema.cursor() as cur:
        cur.execute('SELECT count(*) FROM trading.qt_desk_market_sources')
        assert cur.fetchone()[0] == 0
    assert state(empty_schema) == before


def test_market_guard_has_fixed_catalog_search_path(empty_schema):
    with empty_schema.cursor() as cur:
        cur.execute("SELECT proconfig FROM pg_proc WHERE oid='trading.guard_qt_market_model_reference()'::regprocedure")
        assert cur.fetchone()[0] == ['search_path=pg_catalog']


@pytest.mark.parametrize('isolation', ['REPEATABLE READ', 'SERIALIZABLE'])
@pytest.mark.parametrize('version', [1, 2])
def test_unsupported_single_writer_isolation_refuses_without_partial_publication(empty_schema, isolation, version):
    before = state(empty_schema)
    conn = psycopg2.connect(os.environ['ALGOLENS_TEST_DB'])
    try:
        conn.set_session(isolation_level=isolation)
        with pytest.raises(psycopg2.Error, match='qt023 writer isolation unsupported'):
            with conn.cursor() as cur:
                insert(cur, version)
        conn.rollback()
        assert state(empty_schema) == before
    finally:
        conn.close()


@pytest.mark.parametrize('isolation', ['REPEATABLE READ', 'SERIALIZABLE'])
def test_unsupported_market_writer_isolation_preserves_publication_and_market_scope(empty_schema, isolation):
    with empty_schema.cursor() as cur:
        insert(cur, 2)
    before = state(empty_schema)
    conn = psycopg2.connect(os.environ['ALGOLENS_TEST_DB'])
    try:
        conn.set_session(isolation_level=isolation)
        with pytest.raises(psycopg2.Error, match='qt023 writer isolation unsupported'):
            with conn.cursor() as cur:
                market(cur)
        conn.rollback()
        assert state(empty_schema) == before
        with empty_schema.cursor() as cur:
            cur.execute('SELECT count(*) FROM trading.qt_desk_market_sources')
            assert cur.fetchone()[0] == 0
    finally:
        conn.close()
