#!/usr/bin/env bash
# Verifies migrations/016_positions_instrument_id.sql and its rollback against a real PostgreSQL,
# on the shape of trading.positions and backtest.final_positions as the stage-3 scratch has them.
#
# The properties under test:
#   1. the column the engine's positions INSERTs name after 016 is genuinely ABSENT before it;
#   2. after it: text, nullable, no default, on both tables; every pre-existing row reads NULL; a
#      later INSERT that omits it reads NULL; a value round-trips;
#   3. NOTHING ELSE MOVES: every pre-existing row is byte-identical apart from the new key, and
#      every index and constraint is still there with the same definition;
#   4. idempotent (a second apply is a no-op) and type-guarded (an existing column of another type
#      makes it refuse);
#   5. the two column comments are set;
#   6. the rollback refuses while a non-NULL id exists, and with migration.force_rollback = 'yes'
#      drops the column, the original rows byte-identical to before the migration.
#
# DESTRUCTIVE: begins with DROP SCHEMA trading/backtest CASCADE on the target. Point it at a
# THROWAWAY database (MIGRATION_TEST_DB and PGDATABASE set to its name); production and every
# stage-3 scratch or clone are refused by name, and any database that already has a trading or
# backtest schema is refused.
set -euo pipefail
if [[ "${MIGRATION_TEST_DB:-}" == "" ]]; then
    echo "REFUSING: set MIGRATION_TEST_DB=<throwaway dbname> AND PGDATABASE to the same value." >&2; exit 2
fi
if [[ "${PGDATABASE:-}" != "${MIGRATION_TEST_DB}" ]]; then
    echo "REFUSING: PGDATABASE != MIGRATION_TEST_DB" >&2; exit 2
fi
case "${MIGRATION_TEST_DB}" in
    new_algo_data|new_algo_data_*|*_new_algo_data) echo "REFUSING: '${MIGRATION_TEST_DB}' is production or a stage-3 clone" >&2; exit 2;;
esac
existing=$(psql -X -tAc "SELECT count(*) FROM information_schema.schemata WHERE schema_name IN ('trading','backtest')" 2>/dev/null || echo 0)
if [[ "${existing:-0}" != "0" ]]; then echo "REFUSING: '${MIGRATION_TEST_DB}' already has a trading or backtest schema" >&2; exit 2; fi
HERE="$(cd "$(dirname "$0")" && pwd)"
MIG="$HERE/016_positions_instrument_id.sql"; RB="$HERE/016_positions_instrument_id_rollback.sql"
PSQL="psql -v ON_ERROR_STOP=1 -q -X"
q() { psql -X -tAc "$1"; }
fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }

$PSQL <<'SQL'
DROP SCHEMA IF EXISTS trading CASCADE; DROP SCHEMA IF EXISTS backtest CASCADE;
CREATE SCHEMA trading; CREATE SCHEMA backtest;
CREATE TABLE trading.positions (
    id SERIAL, symbol VARCHAR(20) NOT NULL, quantity NUMERIC NOT NULL, average_price NUMERIC NOT NULL,
    daily_unrealized_pnl NUMERIC DEFAULT 0, daily_realized_pnl NUMERIC DEFAULT 0, last_update TIMESTAMPTZ NOT NULL,
    updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP, strategy_id VARCHAR(100) NOT NULL, strategy_name VARCHAR(100) NOT NULL,
    date DATE NOT NULL, portfolio_id VARCHAR(100) NOT NULL,
    CONSTRAINT positions_pkey PRIMARY KEY (id));
CREATE INDEX idx_positions_symbol ON trading.positions (symbol);
CREATE INDEX idx_positions_strategy_date ON trading.positions (strategy_id, date);
CREATE TABLE backtest.results (run_id TEXT PRIMARY KEY);
CREATE TABLE backtest.final_positions (
    run_id TEXT NOT NULL REFERENCES backtest.results(run_id) ON DELETE CASCADE, portfolio_id VARCHAR(100),
    strategy_id VARCHAR(100) NOT NULL, date DATE NOT NULL, symbol VARCHAR(50) NOT NULL, quantity DOUBLE PRECISION NOT NULL,
    average_price DOUBLE PRECISION NOT NULL, unrealized_pnl DOUBLE PRECISION DEFAULT 0, realized_pnl DOUBLE PRECISION DEFAULT 0,
    last_update TIMESTAMPTZ, updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
    CONSTRAINT final_positions_pkey PRIMARY KEY (run_id, strategy_id, date, symbol));
CREATE INDEX idx_final_positions_symbol ON backtest.final_positions (symbol);
INSERT INTO trading.positions (symbol, quantity, average_price, daily_realized_pnl, last_update, strategy_id, strategy_name, date, portfolio_id)
VALUES ('NG.v.0', 2, 3.376, 11.5, '2025-10-27 00:00:00+00', 'S', 'TREND', '2025-10-27', 'P'),
       ('ES.v.0', -1, 5000, -2.25, '2025-10-27 00:00:00+00', 'S', 'TREND', '2025-10-27', 'P');
INSERT INTO backtest.results VALUES ('R1');
INSERT INTO backtest.final_positions (run_id, portfolio_id, strategy_id, date, symbol, quantity, average_price, realized_pnl, last_update)
VALUES ('R1', 'P', 'S', '2025-10-27', 'NG.v.0', 2, 3.376, 11.5, '2025-10-27 00:00:00+00');
SQL
fp() { q "select count(*)||' '||md5(coalesce(string_agg(x, ';' order by x), '')) from (select (to_jsonb(t) - 'instrument_id')::text x from $1 t) s"; }
idx() { q "select string_agg(indexdef, ' | ' order by indexdef) from pg_indexes where schemaname||'.'||tablename = '$1'"; }
B1=$(fp trading.positions); B2=$(fp backtest.final_positions); I1=$(idx trading.positions); I2=$(idx backtest.final_positions)

# 1. absent before
if psql -X -q -c "INSERT INTO trading.positions (symbol, quantity, average_price, last_update, strategy_id, strategy_name, date, portfolio_id, instrument_id) VALUES ('X',1,1,now(),'S','T',current_date,'P','1')" 2>/dev/null; then fail "instrument_id exists before 016"; fi
pass "1. the column is absent before the migration"
# 2. apply
$PSQL -f "$MIG"
[[ "$(q "select data_type||' '||is_nullable||' '||coalesce(column_default,'-') from information_schema.columns where table_schema='trading' and table_name='positions' and column_name='instrument_id'")" == "text YES -" ]] || fail "trading.positions.instrument_id type"
[[ "$(q "select data_type||' '||is_nullable||' '||coalesce(column_default,'-') from information_schema.columns where table_schema='backtest' and table_name='final_positions' and column_name='instrument_id'")" == "text YES -" ]] || fail "final_positions.instrument_id type"
[[ "$(q "select count(*) from trading.positions where instrument_id is null")" == "2" ]] || fail "pre-existing rows not NULL"
$PSQL -c "INSERT INTO trading.positions (symbol, quantity, average_price, last_update, strategy_id, strategy_name, date, portfolio_id) VALUES ('ZN.v.0',1,110,'2025-10-28 00:00:00+00','S','TREND','2025-10-28','P')"
[[ "$(q "select instrument_id is null from trading.positions where symbol='ZN.v.0'")" == "t" ]] || fail "omitted column not NULL"
$PSQL -c "INSERT INTO trading.positions (symbol, quantity, average_price, last_update, strategy_id, strategy_name, date, portfolio_id, instrument_id) VALUES ('NG.v.0',2,3.965,'2025-10-28 00:00:00+00','S','TREND','2025-10-28','P','863')"
[[ "$(q "select instrument_id from trading.positions where symbol='NG.v.0' and date='2025-10-28'")" == "863" ]] || fail "value round trip"
$PSQL -c "INSERT INTO backtest.final_positions (run_id, portfolio_id, strategy_id, date, symbol, quantity, average_price, instrument_id) VALUES ('R1','P','S','2025-10-28','NG.v.0',2,3.965,'863')"
[[ "$(q "select instrument_id from backtest.final_positions where date='2025-10-28'")" == "863" ]] || fail "final_positions round trip"
pass "2. text, nullable, no default; NULL on history and on an omitting INSERT; values round-trip"
# 3. nothing else moves (the original rows)
$PSQL -c "DELETE FROM trading.positions WHERE date='2025-10-28'; DELETE FROM backtest.final_positions WHERE date='2025-10-28'"
[[ "$(fp trading.positions)" == "$B1" ]] || fail "trading.positions rows moved"
[[ "$(fp backtest.final_positions)" == "$B2" ]] || fail "final_positions rows moved"
[[ "$(idx trading.positions)" == "$I1" && "$(idx backtest.final_positions)" == "$I2" ]] || fail "indexes changed"
[[ "$(q "select count(*) from pg_constraint where conrelid in ('trading.positions'::regclass,'backtest.final_positions'::regclass)")" == "3" ]] || fail "constraints changed"
pass "3. every pre-existing row byte-identical apart from the new key; indexes and constraints unchanged"
# 4. idempotent, type-guarded
$PSQL -f "$MIG"
[[ "$(q "select count(*) from information_schema.columns where column_name='instrument_id' and table_schema in ('trading','backtest')")" == "2" ]] || fail "second apply changed the column count"
$PSQL -c "ALTER TABLE trading.positions ALTER COLUMN instrument_id TYPE bigint USING NULL"
if psql -X -q -v ON_ERROR_STOP=1 -f "$MIG" >/dev/null 2>&1; then fail "type guard did not refuse"; fi
$PSQL -c "ALTER TABLE trading.positions ALTER COLUMN instrument_id TYPE text USING NULL"
pass "4. idempotent; a column of another type makes it refuse"
# 5. comments
[[ "$(q "select col_description('trading.positions'::regclass, (select ordinal_position from information_schema.columns where table_schema='trading' and table_name='positions' and column_name='instrument_id')::int)")" == *"migration 016"* ]] || fail "comment missing on trading.positions"
[[ "$(q "select col_description('backtest.final_positions'::regclass, (select ordinal_position from information_schema.columns where table_schema='backtest' and table_name='final_positions' and column_name='instrument_id')::int)")" == *"migration 016"* ]] || fail "comment missing on final_positions"
pass "5. both column comments set"
# 6. rollback refuses with a value, drops with the override, rows byte-identical to before
$PSQL -c "UPDATE trading.positions SET instrument_id='864' WHERE symbol='NG.v.0'"
if psql -X -q -v ON_ERROR_STOP=1 -f "$RB" 2>/dev/null; then fail "rollback did not refuse"; fi
[[ "$(q "select count(*) from information_schema.columns where column_name='instrument_id' and table_schema in ('trading','backtest')")" == "2" ]] || fail "a refused rollback dropped something"
psql -X -q -v ON_ERROR_STOP=1 -c "SET migration.force_rollback = 'yes'" -f "$RB" >/dev/null 2>&1 || psql -X -q -v ON_ERROR_STOP=1 <<SQL
SET migration.force_rollback = 'yes';
$(sed -e '/^BEGIN;/d' -e '/^COMMIT;/d' "$RB")
SQL
[[ "$(q "select count(*) from information_schema.columns where column_name='instrument_id' and table_schema in ('trading','backtest')")" == "0" ]] || fail "rollback left a column"
[[ "$(fp trading.positions)" == "$B1" && "$(fp backtest.final_positions)" == "$B2" ]] || fail "rows differ after the rollback"
pass "6. rollback refuses with a value, drops with the override, rows byte-identical"
echo "ALL OK"
