"""Exact fixed-point reads through the real strict C++ PostgreSQL readers."""

import json
import os
from pathlib import Path
import subprocess

import pytest

from test_proposal_storage_migration import MIGRATION, apply, predecessor
from test_runtime_control_schema import connection


PROBE = Path('/home/devcontainers/qt-validation-20260921/bin/Debug/proposal_storage_probe')


@pytest.fixture()
def position_db(predecessor):
    apply(predecessor, MIGRATION)
    return predecessor


def probe_output(mode):
    result = subprocess.run([str(PROBE), mode], capture_output=True, text=True,
                            env=os.environ.copy(), check=False)
    assert result.returncode == 0, result.stdout + result.stderr + f' exit={result.returncode}'
    return result.stdout.strip()


def read_raw(mode):
    output = probe_output(mode)
    assert output.startswith('DECIMAL_ROWS='), output
    return json.loads(output.split('DECIMAL_ROWS=', 1)[1])


def insert_position(conn, *, symbol='ES', stream='system', strategy='LIVE_TREND',
                    component='TREND', book='BOOK', day='2026-09-22',
                    quantity='1', price='2', unrealized='3', realized='4'):
    with conn.cursor() as cur:
        cur.execute("""INSERT INTO trading.positions
          (strategy_id,strategy_name,portfolio_id,date,symbol,portfolio_type,
           quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
          VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,'2026-09-22')""",
          (strategy, component, book, day, symbol, stream,
           quantity, price, unrealized, realized))


def position_snapshot(conn):
    with conn.cursor() as cur:
        cur.execute("""SELECT to_jsonb(p)::text FROM trading.positions p
          ORDER BY portfolio_id,strategy_id,strategy_name,date,symbol,portfolio_type""")
        return cur.fetchall()


def test_report_system_preserves_six_decimal_values_lost_by_double(position_db):
    """Catches rounding through row.as<double>() in each report position field."""
    with position_db.cursor() as cur:
        cur.execute("""INSERT INTO trading.positions
          (strategy_id,strategy_name,portfolio_id,date,symbol,portfolio_type,
           quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update)
          VALUES ('LIVE_TREND','TREND','BOOK','2026-09-22','ES','system',
                  90000000000.000001,-90000000000.000001,
                  90000000000.000001,-90000000000.000001,'2026-09-22')""")
    assert read_raw('decimal_report_system') == {'TREND': {'ES': {
        'quantity': 9000000000000000100,
        'price': -9000000000000000100,
        'unrealized': 9000000000000000100,
        'realized': -9000000000000000100,
    }}}


@pytest.mark.parametrize('mode,stream', [
    ('decimal_report_system', 'system'),
    ('decimal_report_qt', 'qt'),
    ('decimal_proposal', 'qt_proposal'),
])
def test_strict_reader_keeps_boundaries_signed_fractions_and_zero(position_db, mode, stream):
    """Catches near-limit overflow and rounding or sign loss in any strict reader."""
    insert_position(position_db, symbol='ES', stream=stream,
                    quantity='92233720368.547758', price='-92233720368.547758',
                    unrealized='0.000001', realized='-0.000001')
    insert_position(position_db, symbol='NQ', stream=stream,
                    quantity='123.456789', price='7', unrealized='-12',
                    realized='0.000000')
    assert read_raw(mode) == {'TREND': {
        'ES': {'quantity': 9223372036854775800, 'price': -9223372036854775800,
               'unrealized': 100, 'realized': -100},
        'NQ': {'quantity': 12345678900, 'price': 700000000,
               'unrealized': -1200000000, 'realized': 0},
    }}


@pytest.mark.parametrize('mode,stream', [
    ('decimal_report_qt', 'qt'),
    ('decimal_proposal', 'qt_proposal'),
])
def test_qt_and_proposal_preserve_six_decimal_values_lost_by_double(position_db, mode, stream):
    """Catches a double intermediary in each field of the other strict paths."""
    insert_position(position_db, stream=stream,
                    quantity='90000000000.000001', price='-90000000000.000001',
                    unrealized='90000000000.000001', realized='-90000000000.000001')
    assert read_raw(mode) == {'TREND': {'ES': {
        'quantity': 9000000000000000100, 'price': -9000000000000000100,
        'unrealized': 9000000000000000100, 'realized': -9000000000000000100,
    }}}


@pytest.mark.parametrize('mode,stream,field,value', [
    ('decimal_report_system', 'system', 'quantity', '92233720368.547759'),
    ('decimal_report_system', 'system', 'price', '-92233720368.547759'),
    ('decimal_report_qt', 'qt', 'unrealized', '92233720368.547759'),
    ('decimal_report_qt', 'qt', 'realized', '-92233720368.547759'),
    ('decimal_proposal', 'qt_proposal', 'quantity', '-92233720368.547759'),
    ('decimal_proposal', 'qt_proposal', 'price', '92233720368.547759'),
    ('decimal_proposal', 'qt_proposal', 'unrealized', '-92233720368.547759'),
    ('decimal_proposal', 'qt_proposal', 'realized', '92233720368.547759'),
])
def test_out_of_range_field_fails_whole_read_without_mutation(
        position_db, mode, stream, field, value):
    """Catches clamping, wrapping, or publishing partial rows after overflow."""
    insert_position(position_db, symbol='VALID', stream=stream)
    invalid = {'quantity': '1', 'price': '2', 'unrealized': '3', 'realized': '4'}
    invalid[field] = value
    insert_position(position_db, symbol='INVALID', stream=stream, **invalid)
    before = position_snapshot(position_db)
    assert probe_output(mode) == 'DECIMAL_ERROR=1'
    assert position_snapshot(position_db) == before


@pytest.mark.parametrize('mode,stream', [
    ('decimal_report_system', 'system'),
    ('decimal_report_qt', 'qt'),
    ('decimal_proposal', 'qt_proposal'),
])
def test_strict_reader_keeps_selection_and_valid_empty_result(position_db, mode, stream):
    """Catches accidental broadening of date, book, component, or stream scope."""
    assert read_raw(mode) == {}
    with position_db.cursor() as cur:
        cur.execute("""INSERT INTO trading.strategy_book_memberships(strategy_id,portfolio_id)
          VALUES ('trend','OTHER')""")
    other_stream = 'system' if stream != 'system' else 'qt'
    insert_position(position_db, symbol='DATE', stream=stream, day='2026-09-23',
                    quantity='92233720368.547759')
    insert_position(position_db, symbol='BOOK', stream=stream, book='OTHER',
                    quantity='92233720368.547759')
    insert_position(position_db, symbol='COMPONENT', stream=stream, component='OTHER',
                    quantity='92233720368.547759')
    insert_position(position_db, symbol='STREAM', stream=other_stream,
                    quantity='92233720368.547759')
    before = position_snapshot(position_db)
    assert read_raw(mode) == {}
    insert_position(position_db, symbol='SELECTED', stream=stream,
                    quantity='0.000001', price='-0.000001',
                    unrealized='123.456789', realized='0')
    assert read_raw(mode) == {'TREND': {'SELECTED': {
        'quantity': 100, 'price': -100, 'unrealized': 12345678900, 'realized': 0,
    }}}
    after = position_snapshot(position_db)
    assert len(after) == len(before) + 1
    assert all(row in after for row in before)


@pytest.mark.parametrize('mode,stream', [
    ('decimal_report_system', 'system'),
    ('decimal_report_qt', 'qt'),
    ('decimal_proposal', 'qt_proposal'),
])
def test_nan_field_fails_whole_read_without_mutation(position_db, mode, stream):
    """Catches accepting PostgreSQL NaN as a zero or partial result."""
    insert_position(position_db, symbol='VALID', stream=stream)
    insert_position(position_db, symbol='NAN', stream=stream, realized='NaN')
    before = position_snapshot(position_db)
    assert probe_output(mode) == 'DECIMAL_ERROR=1'
    assert position_snapshot(position_db) == before


def test_report_accepts_redundant_fractional_zeroes(position_db):
    """Catches rejecting exact extra zero digits in an adjusted numeric column."""
    with position_db.cursor() as cur:
        cur.execute('ALTER TABLE trading.positions ALTER COLUMN daily_realized_pnl TYPE numeric')
    insert_position(position_db, realized='1.230000000')
    assert read_raw('decimal_report_system') == {'TREND': {'ES': {
        'quantity': 100000000, 'price': 200000000,
        'unrealized': 300000000, 'realized': 123000000,
    }}}


def test_report_accepts_both_signed_int64_raw_endpoints(position_db):
    """Catches dropping the legal positive or negative eight-place endpoint."""
    with position_db.cursor() as cur:
        cur.execute('ALTER TABLE trading.positions ALTER COLUMN quantity TYPE numeric')
    insert_position(position_db, symbol='MAX', quantity='92233720368.54775807')
    insert_position(position_db, symbol='MIN', quantity='-92233720368.54775808')
    assert read_raw('decimal_report_system') == {'TREND': {
        'MAX': {'quantity': 9223372036854775807, 'price': 200000000,
                'unrealized': 300000000, 'realized': 400000000},
        'MIN': {'quantity': -9223372036854775808, 'price': 200000000,
                'unrealized': 300000000, 'realized': 400000000},
    }}


def test_report_rejects_nonzero_ninth_decimal_digit_without_mutation(position_db):
    """Catches rounding a selected numeric value with excess precision."""
    with position_db.cursor() as cur:
        cur.execute('ALTER TABLE trading.positions ALTER COLUMN daily_realized_pnl TYPE numeric')
    insert_position(position_db, symbol='VALID')
    insert_position(position_db, symbol='INVALID', realized='0.000000001')
    before = position_snapshot(position_db)
    assert probe_output('decimal_report_system') == 'DECIMAL_ERROR=1'
    assert position_snapshot(position_db) == before
