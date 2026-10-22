#!/usr/bin/env bash
# This tests 039_backtest_end_date_comment.sql, numbered 033 until 2026-10-21. The text below keeps the numbers of that time
# (033 and its neighbours 028 to 033): the SQL is unchanged and the comments it stores name them.
# Verifies migration 033 (COMMENT ON only: backtest.results.end_date and
# backtest.run_metadata.end_date) and its rollback on a real PostgreSQL.
#   1. the fixture carries the comments the two columns and their start_date neighbours had before
#      033, set by the real 028 texts (cut from the 028 file itself);
#   2. after 033: both end_date comments differ from 028's, say "last bar the run consumed", and no
#      longer say the column holds "The end of the configured window"; the start_date comments are
#      byte-identical;
#   3. NOTHING ELSE MOVES: rows byte-identical; columns and types as before;
#   4. idempotent; refuses, changing nothing, when a commented column is missing;
#   5. the rollback restores 028's two texts byte for byte; idempotent; 033 applies again after it.
# DESTRUCTIVE on the target (DROP SCHEMA backtest CASCADE): a THROWAWAY database only; production
# and every stage-3 scratch or clone refused by name, any database with the schema refused.
set -euo pipefail
[[ "${MIGRATION_TEST_DB:-}" == "" ]] && { echo "REFUSING: set MIGRATION_TEST_DB and PGDATABASE" >&2; exit 2; }
[[ "${PGDATABASE:-}" != "${MIGRATION_TEST_DB}" ]] && { echo "REFUSING: PGDATABASE != MIGRATION_TEST_DB" >&2; exit 2; }
case "${MIGRATION_TEST_DB}" in new_algo_data|new_algo_data_*|*_new_algo_data) echo "REFUSING: production or a clone" >&2; exit 2;; esac
existing=$(psql -X -tAc "SELECT count(*) FROM information_schema.schemata WHERE schema_name IN ('trading','backtest')" 2>/dev/null || echo 0)
[[ "${existing:-0}" != "0" ]] && { echo "REFUSING: the target already has a trading or backtest schema" >&2; exit 2; }
HERE="$(cd "$(dirname "$0")" && pwd)"
PSQL="psql -v ON_ERROR_STOP=1 -q -X"
q() { psql -X -tAc "$1"; }
fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }
apply() { $PSQL -f "$HERE/$1"; }
cmt() { q "select coalesce(col_description('$1.$2'::regclass, (select attnum from pg_attribute where attrelid = '$1.$2'::regclass and attname = '$3')), '')"; }
state() { q "select md5(string_agg(t, E'\n')) from (select 'r|' || run_id || '|' || start_date || '|' || end_date as t from backtest.results union all select 'm|' || run_id || '|' || start_date || '|' || end_date from backtest.run_metadata union all select 'c|' || table_name || '|' || column_name || '|' || data_type from information_schema.columns where table_schema = 'backtest' order by 1) s"; }

$PSQL <<'SQL'
DROP SCHEMA IF EXISTS backtest CASCADE;
CREATE SCHEMA backtest;
CREATE TABLE backtest.results ("run_id" text PRIMARY KEY, "start_date" timestamp with time zone, "end_date" timestamp with time zone);
CREATE TABLE backtest.run_metadata ("run_id" text PRIMARY KEY, "start_date" date, "end_date" date);
INSERT INTO backtest.results VALUES ('R1', '2023-10-07 04:00:00+00', '2026-10-07 04:00:00+00');
INSERT INTO backtest.run_metadata VALUES ('R1', '2023-10-07', '2026-10-07');
SQL
# the four comments as the real 028 file sets them
LINES028="$(grep -E '^COMMENT ON COLUMN backtest\.(results|run_metadata)\."(start_date|end_date)" IS ' "$HERE/034_column_comments.sql")"
[[ "$(printf '%s\n' "$LINES028" | wc -l | tr -d ' ')" == "4" ]] || fail "028 does not hold the four comment lines"
printf '%s\n' "$LINES028" | $PSQL

R028="$(cmt backtest results end_date)"; M028="$(cmt backtest run_metadata end_date)"
RS028="$(cmt backtest results start_date)"; MS028="$(cmt backtest run_metadata start_date)"
[[ "$R028" == "The end of the configured window of the run, stored as the host-local midnight of the configured date." ]] || fail "fixture: results.end_date is not 028's text"
[[ "$M028" == "The end of the configured window of the run (a date)." ]] || fail "fixture: run_metadata.end_date is not 028's text"
S0="$(state)"
pass "fixture carries 028's four comments"

apply 039_backtest_end_date_comment.sql
R033="$(cmt backtest results end_date)"; M033="$(cmt backtest run_metadata end_date)"
[[ "$R033" != "$R028" && "$M033" != "$M028" ]] || fail "an end_date comment did not change"
[[ "$R033" == *"last bar the run consumed"* && "$M033" == *"last bar the run consumed"* ]] || fail "the new comments do not name the last bar"
[[ "$R033" != "The end of the configured window"* && "$M033" != "The end of the configured window"* ]] || fail "a comment still opens with 028's claim"
[[ "$R033" == *"backtest.equity_curve"* && "$R033" == *"host's time zone"* ]] || fail "results.end_date does not say where the stamp comes from"
[[ "$(cmt backtest results start_date)" == "$RS028" && "$(cmt backtest run_metadata start_date)" == "$MS028" ]] || fail "a start_date comment moved"
[[ "$(state)" == "$S0" ]] || fail "rows or columns moved"
pass "033 rewrites the two end_date comments and nothing else"

apply 039_backtest_end_date_comment.sql
[[ "$(cmt backtest results end_date)" == "$R033" && "$(cmt backtest run_metadata end_date)" == "$M033" && "$(state)" == "$S0" ]] || fail "033 is not idempotent"
pass "033 is idempotent"

$PSQL -c 'ALTER TABLE backtest.run_metadata RENAME COLUMN "end_date" TO "end_date_x"'
$PSQL -f "$HERE/039_backtest_end_date_comment_rollback.sql" 2>/dev/null && fail "the rollback ran with a column missing"
$PSQL -c "COMMENT ON COLUMN backtest.results.\"end_date\" IS 'probe'"
$PSQL -f "$HERE/039_backtest_end_date_comment.sql" 2>/dev/null && fail "033 ran with a column missing"
[[ "$(cmt backtest results end_date)" == "probe" ]] || fail "a refused 033 changed a comment"
$PSQL -c 'ALTER TABLE backtest.run_metadata RENAME COLUMN "end_date_x" TO "end_date"'
apply 039_backtest_end_date_comment.sql
[[ "$(cmt backtest results end_date)" == "$R033" ]] || fail "033 did not apply after the column came back"
pass "033 and its rollback refuse, changing nothing, when a column is missing"

apply 039_backtest_end_date_comment_rollback.sql
[[ "$(cmt backtest results end_date)" == "$R028" && "$(cmt backtest run_metadata end_date)" == "$M028" ]] || fail "the rollback did not restore 028's texts byte for byte"
[[ "$(cmt backtest results start_date)" == "$RS028" && "$(cmt backtest run_metadata start_date)" == "$MS028" && "$(state)" == "$S0" ]] || fail "the rollback moved something else"
apply 039_backtest_end_date_comment_rollback.sql
[[ "$(cmt backtest results end_date)" == "$R028" && "$(cmt backtest run_metadata end_date)" == "$M028" ]] || fail "the rollback is not idempotent"
pass "the rollback restores 028's two texts byte for byte and is idempotent"

apply 039_backtest_end_date_comment.sql
[[ "$(cmt backtest results end_date)" == "$R033" && "$(cmt backtest run_metadata end_date)" == "$M033" ]] || fail "033 did not apply again after the rollback"
pass "033 applies again after the rollback"
$PSQL -c 'DROP SCHEMA backtest CASCADE'
echo "ALL PASS: migration 033"
