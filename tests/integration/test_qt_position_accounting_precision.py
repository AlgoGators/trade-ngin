"""Real governed catalog/precision migration against owned PostgreSQL only."""
from decimal import Decimal
from pathlib import Path

import psycopg2
import pytest

from test_runtime_control_schema import connection
from test_proposal_storage_migration import normalize_positions_shape, apply, catalog

ROOT = Path(__file__).parents[2]
MIGRATION = ROOT / 'migrations/020_qt_position_accounting_precision.sql'


@pytest.fixture()
def predecessor(connection):
    normalize_positions_shape(connection)
    apply(connection, ROOT / 'migrations/015_qt_proposal_positions.sql')
    apply(connection, ROOT / 'migrations/016_qt_exact_precision_and_seed_provenance.sql')
    return connection


def capabilities(conn):
    with conn.cursor() as cur:
        cur.execute('SELECT to_jsonb(c)::text FROM trading.qt_storage_capabilities c ORDER BY capability_name')
        return cur.fetchall()


def rejected_atomically(conn):
    before = catalog(conn), capabilities(conn)
    with pytest.raises(psycopg2.Error):
        apply(conn, MIGRATION)
    with conn.cursor() as cur:
        cur.execute('ROLLBACK')
    assert (catalog(conn), capabilities(conn)) == before


def insert_position(conn, stream, symbol, realized, unrealized):
    with conn.cursor() as cur:
        cur.execute("INSERT INTO trading.positions(strategy_id,strategy_name,portfolio_id,date,symbol,"
                    "portfolio_type,quantity,average_price,daily_realized_pnl,daily_unrealized_pnl,last_update)"
                    " VALUES('LIVE_TREND','owned-precision','BOOK','2026-09-25',%s,%s,1,25,%s,%s,'2026-09-25T00:00:00Z')",
                    (symbol, stream, realized, unrealized))


def test_catalog_widens_only_two_pnl_columns_and_preserves_exact_capability(predecessor):
    before = capabilities(predecessor)
    apply(predecessor, MIGRATION)
    with predecessor.cursor() as cur:
        cur.execute("SELECT attname,format_type(atttypid,atttypmod) FROM pg_attribute "
                    "WHERE attrelid='trading.positions'::regclass AND attname IN "
                    "('quantity','average_price','daily_realized_pnl','daily_unrealized_pnl')")
        assert dict(cur.fetchall()) == {'quantity':'numeric(20,8)','average_price':'numeric(20,8)',
            'daily_realized_pnl':'numeric(22,8)','daily_unrealized_pnl':'numeric(22,8)'}
    assert before == [row for row in capabilities(predecessor) if 'qt_exact_decimal8' in row[0]]
    assert any('qt_position_accounting_decimal8' in row[0] for row in capabilities(predecessor))


def test_negative_eighth_decimal_pnl_and_full_legacy_integer_range_persist_exactly(predecessor):
    insert_position(predecessor, 'system', 'LEGACY', '99999999999999.999999', '-99999999999999.999999')
    with predecessor.cursor() as cur:
        cur.execute("SELECT quantity,average_price,daily_realized_pnl,daily_unrealized_pnl,last_update FROM trading.positions WHERE symbol='LEGACY'")
        before = cur.fetchone()
    apply(predecessor, MIGRATION)
    with predecessor.cursor() as cur:
        cur.execute("SELECT quantity,average_price,daily_realized_pnl,daily_unrealized_pnl,last_update FROM trading.positions WHERE symbol='LEGACY'")
        assert cur.fetchone() == before
    insert_position(predecessor, 'qt', 'EXACT', '-0.00000001', '-0.04950495')
    with predecessor.cursor() as cur:
        cur.execute("SELECT daily_realized_pnl,daily_unrealized_pnl FROM trading.positions WHERE symbol='EXACT'")
        assert cur.fetchone() == (Decimal('-0.00000001'),Decimal('-0.04950495'))


@pytest.mark.parametrize('column', ['daily_realized_pnl','daily_unrealized_pnl'])
@pytest.mark.parametrize('damage', ['type','nullable'])
def test_wrong_or_nullable_predecessor_is_refused_without_partial_change(predecessor, column, damage):
    with predecessor.cursor() as cur:
        cur.execute('ALTER TABLE trading.positions ALTER COLUMN '+column+
                    (' TYPE numeric(21,7)' if damage == 'type' else ' DROP NOT NULL'))
    rejected_atomically(predecessor)


def test_missing_exact_capability_is_refused_atomically(predecessor):
    with predecessor.cursor() as cur:
        cur.execute('ALTER TABLE trading.qt_storage_capabilities DISABLE TRIGGER qt_storage_capability_immutable')
        cur.execute("DELETE FROM trading.qt_storage_capabilities WHERE capability_name='qt_exact_decimal8'")
        cur.execute('ALTER TABLE trading.qt_storage_capabilities ENABLE TRIGGER qt_storage_capability_immutable')
    rejected_atomically(predecessor)


def test_disabled_immutable_guard_is_refused_atomically(predecessor):
    with predecessor.cursor() as cur:
        cur.execute('ALTER TABLE trading.qt_storage_capabilities DISABLE TRIGGER qt_storage_capability_immutable')
    rejected_atomically(predecessor)


@pytest.mark.parametrize('statement', [
    "UPDATE trading.qt_storage_capabilities SET capability_version=2 WHERE capability_name='qt_position_accounting_decimal8'",
    "DELETE FROM trading.qt_storage_capabilities WHERE capability_name='qt_position_accounting_decimal8'",
    'TRUNCATE trading.qt_storage_capabilities'])
def test_new_accounting_capability_is_immutable(predecessor, statement):
    apply(predecessor, MIGRATION)
    before = capabilities(predecessor)
    with predecessor.cursor() as cur, pytest.raises(psycopg2.Error):
        cur.execute(statement)
    assert capabilities(predecessor) == before


def test_repeat_migration_is_refused_atomically(predecessor):
    apply(predecessor, MIGRATION)
    rejected_atomically(predecessor)
