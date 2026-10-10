#!/usr/bin/env bash
# Verifies migration 029 (settled_at timestamptz on trading.live_results, its backfill and its
# comment) and its rollback on a real PostgreSQL.
#   1. absent before (an INSERT naming the column fails: the migration is necessary);
#   2. after: timestamptz, NULL-able, no default; an omitting INSERT reads NULL;
#   3. THE BACKFILL on seeded rows, three keys in one table (two portfolios and the rows stored
#      with a NULL portfolio_id): every row with a later row of its key holds the marker
#      1970-01-01 00:00:00+00, the last row of each key stays NULL, a key of one row stays NULL;
#      two strategies of one portfolio are two keys;
#   4. NOTHING ELSE MOVES: every pre-existing row byte-identical apart from the new key; indexes
#      and constraints as before;
#   5. idempotent: a second apply stamps nothing, not even a NULL row that has gained a later row;
#      type-guarded (a settled_at of another type makes 029 refuse);
#   6. the backfill statement alone, as a harness sources it (the lines between the two markers):
#      no row without the setting, one book with 'portfolio:<id>';
#   7. the comment set: the stamp, the marker and "backfilled at migration, settlement not
#      observed";
#   8. the rollback refuses while a row carries an observed stamp and, with
#      migration.force_rollback = 'yes', drops the column with the original rows byte-identical;
#      with only markers and NULLs it drops without the setting; idempotent.
# DESTRUCTIVE on the target (DROP SCHEMA trading CASCADE): a THROWAWAY database only; production
# and every stage-3 scratch or clone refused by name, any database with the schema refused.
set -euo pipefail
[[ "${MIGRATION_TEST_DB:-}" == "" ]] && { echo "REFUSING: set MIGRATION_TEST_DB and PGDATABASE" >&2; exit 2; }
[[ "${PGDATABASE:-}" != "${MIGRATION_TEST_DB}" ]] && { echo "REFUSING: PGDATABASE != MIGRATION_TEST_DB" >&2; exit 2; }
case "${MIGRATION_TEST_DB}" in new_algo_data|new_algo_data_*|*_new_algo_data|algo_data) echo "REFUSING: production or a clone" >&2; exit 2;; esac
existing=$(psql -X -tAc "SELECT count(*) FROM information_schema.schemata WHERE schema_name IN ('trading','backtest')" 2>/dev/null || echo 0)
[[ "${existing:-0}" != "0" ]] && { echo "REFUSING: the target already has a trading or backtest schema" >&2; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
PSQL="psql -v ON_ERROR_STOP=1 -q -X"
q() { psql -X -tAc "$1"; }
fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }
apply() { $PSQL -f "$HERE/$1"; }
refuses() { if psql -X -q -v ON_ERROR_STOP=1 -c "$1" >/dev/null 2>&1; then return 1; else return 0; fi; }
MARK="1970-01-01 00:00:00+00"
stamps() { q "select string_agg(coalesce(portfolio_id,'<null>')||'/'||strategy_id||'/'||date||'='||coalesce(to_char(settled_at at time zone 'UTC','YYYY-MM-DD HH24:MI:SS')||'+00','NULL'), ' ' order by portfolio_id nulls last, strategy_id, date) from trading.live_results"; }

$PSQL <<'SQL'
SET TimeZone = 'UTC';
DROP SCHEMA IF EXISTS trading CASCADE;
CREATE SCHEMA trading;
CREATE TABLE trading.live_results (id SERIAL PRIMARY KEY, strategy_id VARCHAR(100) NOT NULL, portfolio_id VARCHAR(100),
    date DATE NOT NULL, daily_pnl NUMERIC, active_positions INTEGER, config JSONB,
    CONSTRAINT live_results_portfolio_strategy_date_key UNIQUE (portfolio_id, strategy_id, date));
CREATE INDEX idx_live_results_date ON trading.live_results (date);
INSERT INTO trading.live_results (strategy_id, portfolio_id, date, daily_pnl, active_positions, config) VALUES
 ('S','P','2026-04-23',1245,9,'{"a": 1}'), ('S','P','2026-04-24',-201.4755,15,NULL), ('S','P','2026-04-25',0,15,NULL),
 ('S2','P','2026-04-25',7,3,NULL), ('S2','P','2026-04-26',8,3,NULL),
 ('E','EQ','2026-06-15',12.5,4,NULL), ('E','EQ','2026-06-16',-3,4,NULL),
 ('ONE','EQ','2026-06-16',0,0,NULL),
 ('S',NULL,'2025-11-11',1,2,NULL), ('S',NULL,'2025-11-14',2,2,NULL);
SQL
fp() { q "select count(*)||' '||md5(coalesce(string_agg(x, ';' order by x), '')) from (select (to_jsonb(t) - 'settled_at')::text x from trading.live_results t) s"; }
B1=$(fp)
I1=$(q "select string_agg(indexdef, ' | ' order by indexdef) from pg_indexes where schemaname = 'trading'")
C1=$(q "select count(*) from pg_constraint c join pg_namespace n on n.oid = c.connamespace where n.nspname = 'trading'")
# 1
refuses "INSERT INTO trading.live_results (strategy_id, portfolio_id, date, settled_at) VALUES ('S','P',current_date,now())" || fail "settled_at exists before 029"
pass "1. the column is absent before the migration"
# 2
apply 029_live_results_settled_at.sql
[[ "$(q "select data_type||' '||is_nullable||' '||coalesce(column_default,'-') from information_schema.columns where table_schema='trading' and table_name='live_results' and column_name='settled_at'")" == "timestamp with time zone YES -" ]] || fail "settled_at is not timestamptz NULL with no default"
pass "2. timestamptz, NULL-able, no default"
# 3
WANT="EQ/E/2026-06-15=$MARK EQ/E/2026-06-16=NULL EQ/ONE/2026-06-16=NULL P/S/2026-04-23=$MARK P/S/2026-04-24=$MARK P/S/2026-04-25=NULL P/S2/2026-04-25=$MARK P/S2/2026-04-26=NULL <null>/S/2025-11-11=$MARK <null>/S/2025-11-14=NULL"
GOT=$(stamps); echo "   after the backfill: $GOT"
[[ "$GOT" == "$WANT" ]] || fail "the backfill is not: marker on every row with a later row of its key, NULL on the last"
[[ "$(q "select count(*) filter (where settled_at is not null)||' stamped, '||count(*) filter (where settled_at is null)||' NULL' from trading.live_results")" == "5 stamped, 5 NULL" ]] || fail "counts"
pass "3. the backfill: 5 rows with a later row of their key hold the marker, the last row of each of the 5 keys is NULL (two strategies of one portfolio and the NULL-portfolio rows are their own keys)"
# 4
[[ "$(fp)" == "$B1" ]] || fail "rows moved"
[[ "$(q "select string_agg(indexdef, ' | ' order by indexdef) from pg_indexes where schemaname = 'trading'")" == "$I1" ]] || fail "indexes changed"
[[ "$(q "select count(*) from pg_constraint c join pg_namespace n on n.oid = c.connamespace where n.nspname = 'trading'")" == "$C1" ]] || fail "constraints changed"
pass "4. every pre-existing row byte-identical apart from the new key; indexes and constraints unchanged"
# 5
$PSQL -c "INSERT INTO trading.live_results (strategy_id, portfolio_id, date, daily_pnl) VALUES ('S','P','2026-04-26',-39.975496)"
[[ "$(q "select count(*) from trading.live_results where date='2026-04-26' and strategy_id='S' and settled_at is null")" == "1" ]] || fail "an omitting INSERT is not NULL"
apply 029_live_results_settled_at.sql
[[ "$(q "select coalesce(settled_at::text,'NULL') from trading.live_results where portfolio_id='P' and strategy_id='S' and date='2026-04-25'")" == "NULL" ]] || fail "a second apply stamped a row that gained a later row"
$PSQL -c "ALTER TABLE trading.live_results ALTER COLUMN settled_at TYPE text USING NULL"
if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/029_live_results_settled_at.sql" >/dev/null 2>&1; then fail "029 type guard did not refuse"; fi
$PSQL -c "ALTER TABLE trading.live_results ALTER COLUMN settled_at TYPE timestamptz USING NULL"
pass "5. an omitting INSERT reads NULL; a second apply stamps nothing; a settled_at of another type makes 029 refuse"
# 6 (the type round trip above emptied the column: every row is NULL here)
STMT=$(awk '/^-- 029 BACKFILL BEGIN/{f=1;next} /^-- 029 BACKFILL END/{f=0} f' "$HERE/029_live_results_settled_at.sql")
[[ "$STMT" == UPDATE* ]] || fail "the backfill statement is not between the two marker lines"
$PSQL -c "$STMT"
[[ "$(q "select count(*) from trading.live_results where settled_at is not null")" == "0" ]] || fail "the statement stamped rows with no setting"
$PSQL -c "BEGIN; SET LOCAL migration.settled_at_backfill = 'portfolio:P'; $STMT COMMIT;"
GOT=$(stamps); echo "   after the statement scoped to portfolio P: $GOT"
[[ "$GOT" == "EQ/E/2026-06-15=NULL EQ/E/2026-06-16=NULL EQ/ONE/2026-06-16=NULL P/S/2026-04-23=$MARK P/S/2026-04-24=$MARK P/S/2026-04-25=$MARK P/S/2026-04-26=NULL P/S2/2026-04-25=$MARK P/S2/2026-04-26=NULL <null>/S/2025-11-11=NULL <null>/S/2025-11-14=NULL" ]] || fail "the scoped statement"
pass "6. the statement between the markers: no row without the setting; with 'portfolio:P' the rows of P only"
# 7
CMT=$(q "select col_description('trading.live_results'::regclass,(select ordinal_position from information_schema.columns where table_schema='trading' and table_name='live_results' and column_name='settled_at'))")
[[ "$CMT" == *"NULL until then"*"stamps now() on every earlier row"*"a run that finds no Day T-1 row"*"The stamp is written by the run that finalises the row; a held day on which no bar printed (a Saturday) is counted by the sizing capital from the first run that loads a later bar, which can be one run before its stamp."*"1970-01-01 00:00:00+00 is not an instant"*"backfilled at migration, settlement not observed"*"the equity book included"*"migration 029." ]] || fail "the settled_at comment"
pass "7. the comment names the stamp, the run with no Day T-1 row, the held no-bar day counted one run before its stamp, the marker and 'backfilled at migration, settlement not observed'"
# 8
apply 029_live_results_settled_at_rollback.sql
[[ "$(q "select count(*) from information_schema.columns where table_schema='trading' and column_name='settled_at'")" == "0" ]] || fail "the rollback left the column (markers and NULLs only)"
apply 029_live_results_settled_at.sql
$PSQL -c "UPDATE trading.live_results SET settled_at = now() WHERE portfolio_id='P' AND strategy_id='S' AND date='2026-04-25'"
if psql -X -q -v ON_ERROR_STOP=1 -f "$HERE/029_live_results_settled_at_rollback.sql" >/dev/null 2>&1; then fail "the rollback did not refuse while an observed stamp exists"; fi
[[ "$(q "select count(*) from information_schema.columns where table_schema='trading' and column_name='settled_at'")" == "1" ]] || fail "a refused rollback dropped the column"
psql -X -q -v ON_ERROR_STOP=1 -c "SET migration.force_rollback = 'yes'" -f "$HERE/029_live_results_settled_at_rollback.sql"
[[ "$(q "select count(*) from information_schema.columns where table_schema='trading' and column_name='settled_at'")" == "0" ]] || fail "the forced rollback left the column"
$PSQL -c "DELETE FROM trading.live_results WHERE strategy_id='S' AND portfolio_id='P' AND date='2026-04-26'"
[[ "$(fp)" == "$B1" ]] || fail "rows differ after the rollback"
[[ "$(q "select string_agg(indexdef, ' | ' order by indexdef) from pg_indexes where schemaname = 'trading'")" == "$I1" ]] || fail "indexes changed in the rollback"
apply 029_live_results_settled_at_rollback.sql
pass "8. the rollback drops a column of markers and NULLs; refuses while an observed stamp exists; forced, it drops the column, rows byte-identical; idempotent"
echo "ALL OK"
