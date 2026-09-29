"""N5 r2 (F4): AlgoLens incubation performance over ENGINE-written rows of an incubating scope.

The actual native publisher (runtime_publication_probe publish_incubating: system + qt) and the actual
proposal seeder (proposal_storage_probe seed: qt_proposal) write the same symbol-day in three streams for an
incubating, active, revision-0 scope on 013 + 015 + 016 + 025. AlgoLens's fetch_incubation_performance must
return exactly one position row per symbol-day (the MODEL, the system stream) and one equity point per day.
"""
import os
import subprocess

import psycopg2
from psycopg2.extras import RealDictCursor

from test_runtime_control_schema import (connection, prepare_exact_publication_schema,  # noqa: F401
    apply_model_incubating, reincubate_at_revision_zero)

PUBLISHER = "/home/devcontainers/qt-validation-20260921/bin/Debug/runtime_publication_probe"
SEEDER = "/home/devcontainers/qt-validation-20260921/bin/Debug/proposal_storage_probe"


def _run(argv):
    result = subprocess.run(argv, capture_output=True, text=True, timeout=60, env=os.environ.copy())
    assert result.returncode == 0, result.stdout[-1500:] + result.stderr[-500:]
    return result.stdout


def test_incubation_performance_reads_one_system_row_per_symbol_day(connection):
    prepare_exact_publication_schema(connection)
    apply_model_incubating(connection)
    reincubate_at_revision_zero(connection)
    with connection.cursor() as cur:
        # AlgoLens 002's incubation start; not a runtime field, so no revision bump.
        cur.execute("ALTER TABLE trading.strategy_registry ADD COLUMN incubation_started_at timestamptz")
        cur.execute("UPDATE trading.strategy_registry SET incubation_started_at='2026-09-01T00:00:00Z' WHERE id='trend'")
        cur.execute("SELECT lifecycle,is_active,runtime_revision FROM trading.strategy_registry WHERE id='trend'")
        assert cur.fetchone() == ("incubating", True, 0)
    assert "RUNTIME_PUBLICATION_OK=publish_incubating" in _run([PUBLISHER, "publish_incubating"])
    assert "PROPOSAL_SEEDED=1\n" in _run([SEEDER, "seed"])
    with connection.cursor() as cur:
        cur.execute("SELECT portfolio_type,quantity::float8 FROM trading.positions WHERE strategy_id='LIVE_TREND' "
                    "AND portfolio_id='BOOK' AND symbol='ES' AND date='2026-09-22' ORDER BY portfolio_type")
        assert cur.fetchall() == [("qt", 12.0), ("qt_proposal", 12.0), ("system", 12.0)]
        cur.execute("SELECT portfolio_type FROM trading.equity_curve WHERE strategy_id='LIVE_TREND' AND portfolio_id='BOOK'")
        assert [row[0] for row in cur.fetchall()] == ["system"]
    from algolens.application.portfolio.use_cases import GetIncubationPerformance
    from algolens.infrastructure.portfolio.repositories import PostgresPortfolioRepository
    repository = PostgresPortfolioRepository(connection_factory=lambda: psycopg2.connect(
        os.environ["ALGOLENS_TEST_DB"], cursor_factory=RealDictCursor))
    rows = repository.fetch_incubation_performance("trend")
    assert [(row["symbol"], float(row["quantity"]), float(row["entry_price"])) for row in rows.positions] == \
        [("ES", 12.0, 100.0)]
    assert [float(point["equity"]) for point in rows.equity_curve] == [500000.0]
    performance = GetIncubationPerformance(repository).execute("trend")
    assert len(performance["positions"]) == 1 and len(performance["equity_curve"]) == 1
