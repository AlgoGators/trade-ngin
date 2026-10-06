"""Local relational regressions against the SQL emitted by the C++ seed.

SQLite exercises precedence/scoping/upsert behavior, not PostgreSQL runtime or
C++ execution. The disposable PostgreSQL harness remains required separately.
"""
import ast
from pathlib import Path
import re
import sqlite3
import unittest

ROOT = Path(__file__).resolve().parents[1]


def seed_sql():
    source = (ROOT / 'src/data/postgres_database.cpp').read_text()
    method = source.split('PostgresDatabase::seed_qt_positions_from_system(', 1)[1]
    expression = method.split('std::string query =', 1)[1].split(';', 1)[0]
    # Resolve the production expression's C++ string literals and table argument.
    return ''.join('trading.positions' if token == 'table_name' else ast.literal_eval(token)
                   for token in re.findall(r'"(?:[^"\\]|\\.)*"|\btable_name\b', expression))


def report_sql(names):
    source = (ROOT / 'src/data/postgres_database.cpp').read_text()
    method = source.split('PostgresDatabase::load_report_positions_by_date(', 1)[1]
    expression = method.split('const std::string query =', 1)[1].split(';', 1)[0]
    quoted_names = ','.join("'" + name.replace("'", "''") + "'" for name in names)
    return ''.join(quoted_names if token == 'quoted_names' else ast.literal_eval(token)
                   for token in re.findall(r'"(?:[^"\\]|\\.)*"|\bquoted_names\b', expression))


class QtCarryForward(unittest.TestCase):
    def setUp(self):
        self.db = sqlite3.connect(':memory:')
        self.addCleanup(self.db.close)
        self.db.executescript('''
          ATTACH DATABASE ':memory:' AS trading;
          CREATE TABLE trading.positions (
            symbol TEXT, quantity NUMERIC, average_price NUMERIC DEFAULT 100,
            daily_unrealized_pnl NUMERIC DEFAULT 0, daily_realized_pnl NUMERIC DEFAULT 0,
            last_update TEXT DEFAULT CURRENT_TIMESTAMP, updated_at TEXT DEFAULT CURRENT_TIMESTAMP,
            strategy_id TEXT DEFAULT 'COMBINED', strategy_name TEXT DEFAULT 'TREND',
            date TEXT, portfolio_id TEXT DEFAULT 'BOOK', portfolio_type TEXT,
            PRIMARY KEY (portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type));
        ''')

    def row(self, symbol, quantity, day, stream, name='TREND', book='BOOK'):
        self.db.execute('''INSERT INTO trading.positions
          (symbol,quantity,date,portfolio_type,strategy_name,portfolio_id) VALUES (?,?,?,?,?,?)''',
          (symbol, quantity, day, stream, name, book))

    def seed(self, day):
        self.db.execute(seed_sql(), {'1': 'COMBINED', '2': 'TREND', '3': 'BOOK', '4': day})

    def qt(self, day):
        rows = self.db.execute(report_sql(['TREND']), {'1': 'COMBINED', '2': 'BOOK', '3': day, '4': 'qt'})
        return {row[0]: row[1] for row in rows}

    def test_report_sql_returns_all_strategies_in_one_captured_result(self):
        self.row('ES', 7, '2026-09-19', 'qt')
        self.row('ES', -2, '2026-09-19', 'qt', name='CARRY')
        self.row('ES', 88, '2026-09-19', 'system')
        self.row('ES', 99, '2026-09-19', 'qt', book='OTHER')
        self.row('ES', 100, '2026-09-20', 'qt')
        self.row('ES', 101, '2026-09-19', 'qt', name='UNREQUESTED')
        rows = self.db.execute(report_sql(['TREND', 'CARRY']),
                               {'1': 'COMBINED', '2': 'BOOK', '3': '2026-09-19', '4': 'qt'}).fetchall()
        self.db.execute("UPDATE trading.positions SET quantity=50 WHERE strategy_name='CARRY'")
        self.assertEqual({r[6]: r[1] for r in rows}, {'TREND': 7, 'CARRY': -2})

    def test_manual_quantity_survives_next_day_system_seed(self):
        self.row('ES', 12, '2026-09-18', 'system')
        self.seed('2026-09-18')
        self.db.execute("UPDATE trading.positions SET quantity=7 WHERE portfolio_type='qt'")
        self.row('ES', 20, '2026-09-19', 'system')
        self.seed('2026-09-19')
        self.assertEqual(self.qt('2026-09-19'), {'ES': 7})
        self.assertEqual(self.db.execute("SELECT quantity FROM trading.positions WHERE date='2026-09-19' AND portfolio_type='system'").fetchone()[0], 20)

    def test_zero_closure_survives_multiple_days_until_explicit_replacement(self):
        self.row('ES', 0, '2026-09-18', 'qt')
        for day in ('2026-09-19', '2026-09-20'):
            self.row('ES', 12, day, 'system')
            self.seed(day)
            self.assertEqual(self.qt(day), {'ES': 0})
        self.db.execute("UPDATE trading.positions SET quantity=4 WHERE date='2026-09-20' AND portfolio_type='qt'")
        self.row('ES', 22, '2026-09-21', 'system')
        self.seed('2026-09-21')
        self.assertEqual(self.qt('2026-09-21'), {'ES': 4})

    def test_disappearing_symbols_keep_qt_state_and_new_symbols_seed_once(self):
        self.row('OLD', 7, '2026-09-18', 'qt')
        self.row('CLOSED', 0, '2026-09-18', 'qt')
        self.row('NEW', 3, '2026-09-19', 'system')
        self.row('NEW', 88, '2026-09-18', 'qt', book='OTHER')
        self.row('NEW', 99, '2026-09-18', 'qt', name='OTHER')
        self.row('FUTURE', 8, '2026-09-20', 'qt')
        self.seed('2026-09-19')
        self.assertEqual(self.qt('2026-09-19'), {'OLD': 7, 'CLOSED': 0, 'NEW': 3})

    def test_partial_today_qt_state_is_preserved_and_missing_symbols_are_filled(self):
        self.row('ES', 12, '2026-09-19', 'system')
        self.row('NQ', 3, '2026-09-19', 'system')
        self.row('ES', 0, '2026-09-19', 'qt')
        self.seed('2026-09-19')
        self.seed('2026-09-19')
        self.assertEqual(self.qt('2026-09-19'), {'ES': 0, 'NQ': 3})


class RollbackBarrier(unittest.TestCase):
    def test_attribution_tables_are_locked_before_the_first_safety_read(self):
        sql = (ROOT / 'migrations/012_position_overrides_portfolio_scope_rollback.sql').read_text()
        # Static fallback required by final review; PostgreSQL lock inspection is
        # also performed by the disposable migration harness when available.
        first_check = sql.index('IF EXISTS (')
        for table in ('position_overrides', 'position_override_legacy_scopes'):
            lock = f'LOCK TABLE trading.{table} IN ACCESS EXCLUSIVE MODE;'
            self.assertIn(lock, sql[:first_check])
        self.assertNotIn('LOCK TABLE IF EXISTS', sql)

    def test_safety_reads_see_writers_that_committed_while_waiting_for_locks(self):
        sql = (ROOT / 'migrations/012_position_overrides_portfolio_scope_rollback.sql').read_text()
        self.assertIn('BEGIN ISOLATION LEVEL READ COMMITTED;', sql)


if __name__ == '__main__':
    unittest.main()
