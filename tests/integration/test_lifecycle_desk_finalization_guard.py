"""N5 r2 (F1): the AlgoLens "block demotion first" guard against the REAL desk relations and the ACTUAL finalizer.

The owned equity EOD fixture chain (tests/fixtures/mr_equity_eod_fixture.py) builds the migrated desk schema
(017/018/019/021 relations), runs the actual native S processor (a processed desk decision with stored
accounting in trading.desk_run_results) and stops before the actual next-day finalizer. AlgoLens's guard,
run over the book's canonical name, must name that day; after the actual finalizer
(qt_equity_finalization_probe --finalize) writes its trading.qt_desk_finalizations row, it must pass.
"""
import os
import sys
from pathlib import Path

import psycopg2
from psycopg2.extras import RealDictCursor

ENGINE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ENGINE / 'tests/fixtures'))
from mr_equity_eod_fixture import (eod, eod_clock, equity, desk, connection,  # noqa: E402,F401  (fixtures)
    finalize, BOOK, DECISION)


def pending_day_refusal():
    """The guard as start_incubation/retire_strategy run it; returns the refusal, or None when it passes."""
    from algolens.application.portfolio.ports import IncubationError
    from algolens.infrastructure.portfolio.repositories import PostgresPortfolioRepository
    conn = psycopg2.connect(os.environ['ALGOLENS_TEST_DB'], cursor_factory=RealDictCursor)
    try:
        with conn.cursor() as cursor:
            try:
                PostgresPortfolioRepository(connection_factory=None)._require_desk_days_finalized(
                    cursor, [' ' + BOOK.lower() + ' ', 'UNRELATED_BOOK'])
            except IncubationError as refusal:
                return refusal
        return None
    finally:
        conn.rollback()
        conn.close()


def test_processed_desk_day_blocks_lifecycle_change_until_the_actual_finalizer_runs(eod):
    conn, source = eod[0], eod[1]
    with conn.cursor() as cur:
        cur.execute('SELECT portfolio_id,date::text FROM trading.desk_run_results WHERE decision_id=%s', (DECISION,))
        assert cur.fetchall() == [(BOOK, source['source_day'])]
        cur.execute('SELECT count(*) FROM trading.qt_desk_finalizations WHERE decision_id=%s', (DECISION,))
        assert cur.fetchone()[0] == 0
    refusal = pending_day_refusal()
    assert refusal is not None and getattr(refusal, 'code', None) == 'desk_finalization_pending', refusal
    assert BOOK + ' ' + source['source_day'] in str(refusal)
    result = finalize()
    assert result.returncode == 0, result.stdout + result.stderr
    with conn.cursor() as cur:
        cur.execute('SELECT count(*) FROM trading.qt_desk_finalizations WHERE decision_id=%s', (DECISION,))
        assert cur.fetchone()[0] == 1
    assert pending_day_refusal() is None
