#!/usr/bin/env bash
# Verifies migrations/013_executions_netting_adjustment.sql and its rollback against a real
# PostgreSQL, using the shape of trading.executions, backtest.executions and
# trading.live_results as introspected from the stage-3 scratch (a copy of production).
#
# The properties under test:
#   1. the column the engine's executions INSERTs name after 013 is genuinely ABSENT before the
#      migration (an INSERT naming it fails: the migration is necessary, not decorative);
#   2. after it: numeric in trading, double precision in backtest, nullable, default 0; every
#      row written before reads NULL ("not computed"), a later INSERT that omits the column
#      reads 0, a negative value and 8-decimal values round-trip exactly in trading;
#   3. NOTHING ELSE MOVES: every pre-existing row is byte-identical apart from the new key, and
#      every index and constraint is still there, valid, with the same definition;
#   4. idempotent (a second apply is a no-op) and type-guarded (an existing column of another
#      type makes it refuse, changing nothing);
#   5. the three column comments are set, the equity_to_margin_ratio values untouched;
#   6. the rollback refuses while a non-zero adjustment exists and destroys nothing when it
#      refuses; with the session override it drops the column, restores the old comment, and
#      the original rows are byte-identical to before the migration.
#
# Requires a running postgres reachable via PGHOST/PGPORT/PGUSER/PGPASSWORD.
#
# DESTRUCTIVE: the fixture begins with DROP SCHEMA trading/backtest CASCADE on whatever those
# env vars point at. Point it at a THROWAWAY database created for the purpose, for example
#
#   createdb t013_migration_test
#   PGDATABASE=t013_migration_test MIGRATION_TEST_DB=t013_migration_test ./test_013_executions_netting_adjustment.sh
#   dropdb t013_migration_test
#
# Never production and never a stage-3 scratch or clone: those are refused by name below,
# and any database that already has a trading or backtest schema is refused whatever it is
# called.

set -euo pipefail

if [[ "${MIGRATION_TEST_DB:-}" == "" ]]; then
    echo "REFUSING to run: this script DROPS SCHEMA trading and backtest CASCADE on the target DB." >&2
    echo "Set MIGRATION_TEST_DB=<throwaway dbname> AND PGDATABASE to the same value." >&2
    exit 2
fi
if [[ "${PGDATABASE:-}" != "${MIGRATION_TEST_DB}" ]]; then
    echo "REFUSING to run: PGDATABASE='${PGDATABASE:-}' != MIGRATION_TEST_DB='${MIGRATION_TEST_DB}'." >&2
    exit 2
fi
case "${MIGRATION_TEST_DB}" in
    new_algo_data|new_algo_data_*|*_new_algo_data)
        echo "REFUSING to run against '${MIGRATION_TEST_DB}': that is production or a stage-3" >&2
        echo "scratch or clone, and this script DROPS the trading and backtest schemas." >&2
        exit 2
        ;;
esac
existing=$(psql -X -tAc "SELECT count(*) FROM information_schema.schemata
                          WHERE schema_name IN ('trading','backtest')" 2>/dev/null || echo 0)
if [[ "${existing:-0}" != "0" ]]; then
    echo "REFUSING to run against '${MIGRATION_TEST_DB}': it already has a trading or backtest" >&2
    echo "schema. This script drops both. Point it at an empty throwaway database." >&2
    exit 2
fi

PSQL="psql -v ON_ERROR_STOP=1 -q -X"
HERE="$(cd "$(dirname "$0")" && pwd)"
UP="$HERE/013_executions_netting_adjustment.sql"
DOWN="$HERE/013_executions_netting_adjustment_rollback.sql"
pass=0
fail=0
ok()  { echo "  PASS  $1"; pass=$((pass + 1)); }
bad() { echo "  FAIL  $1"; fail=$((fail + 1)); }

OLD_COMMENT='Gross notional divided by total margin posted (implied margin leverage).'

# Hash of every row WITHOUT the new key, ordered by its own text: proves no existing value moved.
fingerprint() {
    $PSQL -tAc "SELECT coalesce(md5(string_agg(x, ';' ORDER BY x)), 'EMPTY')
                  FROM (SELECT (to_jsonb(t) - 'netting_adjustment')::text x FROM $1 t) s"
}
coltype() { $PSQL -tAc "SELECT coalesce(max(data_type||'|'||is_nullable||'|'||coalesce(column_default,'')),'ABSENT')
                          FROM information_schema.columns
                         WHERE table_schema='$1' AND table_name='executions'
                           AND column_name='netting_adjustment'"; }
comment_of() { $PSQL -tAc "SELECT coalesce(col_description('$1'::regclass,
                             (SELECT ordinal_position FROM information_schema.columns
                               WHERE table_schema=split_part('$1','.',1)
                                 AND table_name=split_part('$1','.',2)
                                 AND column_name='$2')::int), '<none>')"; }
objects() {
    $PSQL -tAc "SELECT c.relname||'|'||i.relname||'|'||pg_get_indexdef(i.oid)||'|'||x.indisvalid AS o
                  FROM pg_index x JOIN pg_class c ON c.oid=x.indrelid
                  JOIN pg_class i ON i.oid=x.indexrelid
                  JOIN pg_namespace n ON n.oid=c.relnamespace
                 WHERE n.nspname IN ('trading','backtest')
                   AND c.relname IN ('executions','results','live_results')
                 ORDER BY o"
    $PSQL -tAc "SELECT conrelid::regclass::text||'|'||conname||'|'||contype::text||'|'||pg_get_constraintdef(oid) AS o
                  FROM pg_constraint
                 WHERE conrelid::regclass::text IN
                       ('trading.executions','backtest.executions','backtest.results','trading.live_results')
                 ORDER BY o"
}

# --- fixture: the three tables as the stage-3 scratch has them ------------------------------
$PSQL <<'SQL'
DROP SCHEMA IF EXISTS trading CASCADE;
DROP SCHEMA IF EXISTS backtest CASCADE;
CREATE SCHEMA trading;
CREATE SCHEMA backtest;

CREATE TABLE trading.executions (
    exec_id                 VARCHAR(50)  NOT NULL,
    order_id                VARCHAR(50)  NOT NULL,
    symbol                  VARCHAR(20)  NOT NULL,
    side                    VARCHAR(4)   NOT NULL,
    quantity                NUMERIC      NOT NULL,
    price                   NUMERIC      NOT NULL,
    execution_time          TIMESTAMPTZ  NOT NULL,
    commissions_fees        NUMERIC      NOT NULL,
    is_partial              BOOLEAN      NOT NULL,
    created_at              TIMESTAMPTZ  DEFAULT CURRENT_TIMESTAMP,
    strategy_id             VARCHAR(100) NOT NULL,
    strategy_name           VARCHAR(100) NOT NULL,
    date                    DATE         NOT NULL,
    portfolio_id            VARCHAR(100) NOT NULL,
    implicit_price_impact   NUMERIC      DEFAULT 0.0,
    slippage_market_impact  NUMERIC      DEFAULT 0.0,
    total_transaction_costs NUMERIC      DEFAULT 0.0,
    CONSTRAINT executions_pkey PRIMARY KEY (portfolio_id, strategy_id, strategy_name, date, exec_id),
    CONSTRAINT chk_executions_quantity CHECK (quantity > (0)::numeric),
    CONSTRAINT chk_executions_side CHECK (side IN ('BUY','SELL'))
);
CREATE INDEX idx_executions_order_id ON trading.executions (order_id);
CREATE INDEX idx_executions_symbol ON trading.executions (symbol);
CREATE INDEX idx_executions_time ON trading.executions (execution_time);
CREATE INDEX idx_trading_executions_portfolio_id ON trading.executions (portfolio_id);
CREATE INDEX idx_trading_executions_strategy_name ON trading.executions (strategy_name);
CREATE INDEX idx_trading_executions_symbol_time ON trading.executions (symbol, execution_time);

CREATE TABLE backtest.results (run_id TEXT PRIMARY KEY);
CREATE TABLE backtest.executions (
    id                      SERIAL,
    run_id                  TEXT         NOT NULL REFERENCES backtest.results(run_id) ON DELETE CASCADE,
    execution_id            VARCHAR(255) NOT NULL,
    order_id                VARCHAR(255) NOT NULL,
    "timestamp"             TIMESTAMPTZ  NOT NULL,
    symbol                  VARCHAR(50)  NOT NULL,
    side                    VARCHAR(10)  NOT NULL,
    quantity                DOUBLE PRECISION NOT NULL,
    price                   DOUBLE PRECISION NOT NULL,
    commissions_fees        DOUBLE PRECISION NOT NULL,
    is_partial              BOOLEAN      NOT NULL DEFAULT false,
    strategy_id             VARCHAR(100) NOT NULL,
    portfolio_id            VARCHAR(100),
    implicit_price_impact   DOUBLE PRECISION DEFAULT 0.0,
    slippage_market_impact  DOUBLE PRECISION DEFAULT 0.0,
    total_transaction_costs DOUBLE PRECISION DEFAULT 0.0,
    CONSTRAINT executions_pkey PRIMARY KEY (run_id, strategy_id, execution_id)
);
CREATE INDEX idx_backtest_executions_order_id ON backtest.executions (order_id);
CREATE INDEX idx_backtest_executions_portfolio_id ON backtest.executions (portfolio_id);
CREATE INDEX idx_backtest_executions_run_id ON backtest.executions (run_id);
CREATE INDEX idx_backtest_executions_strategy_id ON backtest.executions (strategy_id);
CREATE INDEX idx_backtest_executions_symbol ON backtest.executions (symbol);
CREATE INDEX idx_backtest_executions_timestamp ON backtest.executions ("timestamp");

CREATE TABLE trading.live_results (
    id                     SERIAL PRIMARY KEY,
    strategy_id            VARCHAR(100) NOT NULL,
    portfolio_id           VARCHAR(100) NOT NULL,
    date                   DATE NOT NULL,
    equity_to_margin_ratio DOUBLE PRECISION
);
COMMENT ON COLUMN trading.live_results.equity_to_margin_ratio IS
    'Gross notional divided by total margin posted (implied margin leverage).';

-- Rows that must survive untouched: the C9h basechain 6C pair of 2026-04-24.
INSERT INTO trading.executions (exec_id, order_id, symbol, side, quantity, price, execution_time,
    commissions_fees, is_partial, created_at, strategy_id, strategy_name, date, portfolio_id,
    implicit_price_impact, slippage_market_impact, total_transaction_costs)
VALUES ('EXEC_6C.v.0_20260424','DAILY_6C.v.0_2026-04-24','6C.v.0','BUY',1,0.73155,
        '2026-04-24 05:00:00+00',1.5,false,'2026-09-25 16:02:00+00',
        'LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST','TREND_FOLLOWING','2026-04-24','BASE_PORTFOLIO',
        0.00002449,2.4491918,3.9491918),
       ('EXEC_6C.v.0_20260424','DAILY_6C.v.0_2026-04-24','6C.v.0','SELL',1,0.73155,
        '2026-04-24 05:00:00+00',1.5,false,'2026-09-25 16:02:00+00',
        'LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST','TREND_FOLLOWING_FAST','2026-04-24','BASE_PORTFOLIO',
        0.00002449,2.4491918,3.9491918);
INSERT INTO backtest.results (run_id) VALUES ('RUN_013_PROBE');
INSERT INTO backtest.executions (run_id, execution_id, order_id, "timestamp", symbol, side, quantity,
    price, commissions_fees, strategy_id, portfolio_id, total_transaction_costs)
VALUES ('RUN_013_PROBE','EX-TF-0','PM-TF-0','2025-05-06 00:00:00+00','ZT.v.0','BUY',1,103.65234375,
        1.5,'TREND_FOLLOWING','BASE_PORTFOLIO',5.98508788);
INSERT INTO trading.live_results (strategy_id, portfolio_id, date, equity_to_margin_ratio)
VALUES ('LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST','BASE_PORTFOLIO','2026-04-24',17.392992);
SQL

echo "########## BEFORE ##########"
before_objects="$(objects)"
before_fp_tr=$(fingerprint trading.executions)
before_fp_bt=$(fingerprint backtest.executions)
before_fp_lr=$(fingerprint trading.live_results)

[ "$(coltype trading)" = "ABSENT" ]  && ok "trading.executions has no netting_adjustment before 013" \
                                     || bad "trading.executions already has netting_adjustment"
[ "$(coltype backtest)" = "ABSENT" ] && ok "backtest.executions has no netting_adjustment before 013" \
                                     || bad "backtest.executions already has netting_adjustment"
[ "$(comment_of trading.live_results equity_to_margin_ratio)" = "$OLD_COMMENT" ] \
    && ok "equity_to_margin_ratio carries the pre-013 comment" \
    || bad "fixture: equity_to_margin_ratio comment is not the pre-013 text"

INS_TR="INSERT INTO trading.executions (exec_id, order_id, symbol, side, quantity, price,
    execution_time, commissions_fees, is_partial, strategy_id, strategy_name, date, portfolio_id,
    total_transaction_costs, netting_adjustment)
  VALUES ('EXEC_6C.v.0_20260429','DAILY_6C.v.0_2026-04-29','6C.v.0','SELL',1,0.7324,
    '2026-04-29 05:00:00+00',1.5,false,'LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST',
    'TREND_FOLLOWING','2026-04-29','BASE_PORTFOLIO',3.97287664,-0.50653209)"
if $PSQL -c "$INS_TR" > /dev/null 2>&1; then
    bad "an INSERT naming netting_adjustment succeeded BEFORE the migration"
else
    ok "an INSERT naming netting_adjustment fails before the migration -- 013 is necessary"
fi

echo ""
echo "########## APPLY ##########"
if $PSQL -f "$UP" > /dev/null 2>&1; then ok "migration applied"; else bad "migration failed"; exit 1; fi
if $PSQL -f "$UP" > /dev/null 2>&1; then ok "migration is idempotent (a second apply is a no-op)"
else bad "a second apply failed"; fi

echo ""
echo "########## AFTER ##########"
[ "$(coltype trading)" = "numeric|YES|0" ] && ok "trading.executions.netting_adjustment is numeric, nullable, default 0" \
    || bad "trading column is $(coltype trading)"
[ "$(coltype backtest)" = "double precision|YES|0.0" ] && ok "backtest.executions.netting_adjustment is double precision, nullable, default 0.0" \
    || bad "backtest column is $(coltype backtest)"

n_null_tr=$($PSQL -tAc "SELECT count(*) FROM trading.executions WHERE netting_adjustment IS NULL")
n_null_bt=$($PSQL -tAc "SELECT count(*) FROM backtest.executions WHERE netting_adjustment IS NULL")
[ "$n_null_tr" = "2" ] && ok "both pre-013 trading rows read NULL (not computed), not 0" \
                       || bad "pre-013 trading rows: $n_null_tr NULL of 2"
[ "$n_null_bt" = "1" ] && ok "the pre-013 backtest row reads NULL" || bad "pre-013 backtest rows: $n_null_bt NULL of 1"

[ "$(fingerprint trading.executions)" = "$before_fp_tr" ] && ok "trading.executions: every existing value byte-identical" \
    || bad "trading.executions rows changed"
[ "$(fingerprint backtest.executions)" = "$before_fp_bt" ] && ok "backtest.executions: every existing value byte-identical" \
    || bad "backtest.executions rows changed"
[ "$(fingerprint trading.live_results)" = "$before_fp_lr" ] && ok "trading.live_results: every value byte-identical" \
    || bad "trading.live_results rows changed"
if [ "$(objects)" = "$before_objects" ]; then
    ok "every index and constraint unchanged and still valid"
else
    bad "an index or constraint changed"; diff <(echo "$before_objects") <(objects) || true
fi

if $PSQL -c "$INS_TR" > /dev/null 2>&1; then ok "an INSERT naming netting_adjustment succeeds after 013"
else bad "an INSERT naming netting_adjustment still fails"; fi
v=$($PSQL -tAc "SELECT netting_adjustment::text FROM trading.executions WHERE exec_id='EXEC_6C.v.0_20260429'")
[ "$v" = "-0.50653209" ] && ok "a negative 8-decimal adjustment round-trips exactly (-0.50653209)" \
                         || bad "stored adjustment reads '$v'"
$PSQL -c "INSERT INTO trading.executions (exec_id, order_id, symbol, side, quantity, price,
    execution_time, commissions_fees, is_partial, strategy_id, strategy_name, date, portfolio_id,
    total_transaction_costs)
  VALUES ('EXEC_MBT.v.0_20260425','DAILY_MBT.v.0_2026-04-25','MBT.v.0','BUY',1,77960,
    '2026-04-25 05:00:00+00',1.5,false,'LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST',
    'TREND_FOLLOWING_FAST','2026-04-25','BASE_PORTFOLIO',2.87279481)" > /dev/null
v=$($PSQL -tAc "SELECT netting_adjustment::text FROM trading.executions WHERE exec_id='EXEC_MBT.v.0_20260425'")
[ "$v" = "0" ] && ok "an INSERT that omits the column reads 0 (the default applies after 013)" \
               || bad "an omitting INSERT reads '$v'"
$PSQL -c "INSERT INTO backtest.executions (run_id, execution_id, order_id, \"timestamp\", symbol, side,
    quantity, price, commissions_fees, strategy_id)
  VALUES ('RUN_013_PROBE','EX-TF-1','PM-TF-1','2025-05-07 00:00:00+00','ZT.v.0','SELL',1,103.6,
    1.5,'TREND_FOLLOWING')" > /dev/null
v=$($PSQL -tAc "SELECT netting_adjustment FROM backtest.executions WHERE execution_id='EX-TF-1'")
[ "$v" = "0" ] && ok "a backtest INSERT that omits the column reads 0" || bad "backtest omitting INSERT reads '$v'"

c=$(comment_of trading.executions netting_adjustment)
[[ "$c" == Dollars.*"NULL on rows written before migration 013"* ]] \
    && ok "trading.executions.netting_adjustment carries its comment" || bad "trading comment: $c"
c=$(comment_of backtest.executions netting_adjustment)
[[ "$c" == "As trading.executions.netting_adjustment"* ]] \
    && ok "backtest.executions.netting_adjustment carries its comment" || bad "backtest comment: $c"
c=$(comment_of trading.live_results equity_to_margin_ratio)
[[ "$c" == "Current portfolio value divided by total posted margin"* ]] \
    && ok "equity_to_margin_ratio comment states HD's definition" || bad "equity_to_margin_ratio comment: $c"
v=$($PSQL -tAc "SELECT equity_to_margin_ratio FROM trading.live_results")
[ "$v" = "17.392992" ] && ok "equity_to_margin_ratio values untouched" || bad "equity_to_margin_ratio reads $v"

echo ""
echo "########## TYPE GUARD ##########"
# Swap the backtest column for a text one and prove the migration refuses and changes nothing.
$PSQL -c "ALTER TABLE backtest.executions DROP COLUMN netting_adjustment;
          ALTER TABLE backtest.executions ADD COLUMN netting_adjustment TEXT" > /dev/null
guard_fp=$(fingerprint trading.executions)
if $PSQL -f "$UP" > /dev/null 2>&1; then
    bad "the migration accepted a netting_adjustment of the wrong type"
else
    ok "the migration refuses an existing netting_adjustment of another type"
fi
[ "$(fingerprint trading.executions)" = "$guard_fp" ] && ok "the refused apply changed nothing" \
    || bad "the refused apply changed trading.executions"
$PSQL -c "ALTER TABLE backtest.executions DROP COLUMN netting_adjustment" > /dev/null
$PSQL -f "$UP" > /dev/null
[ "$(coltype backtest)" = "double precision|YES|0.0" ] && ok "re-applied after the guard test" \
    || bad "re-apply after the guard test: $(coltype backtest)"
# The re-added backtest column is NULL on the two backtest rows (they predate this ADD).

echo ""
echo "########## ROLLBACK ##########"
if $PSQL -f "$DOWN" > /dev/null 2>&1; then
    bad "rollback should refuse while a non-zero netting_adjustment exists"
else
    ok "rollback refuses while a non-zero adjustment exists"
fi
[ "$(coltype trading)" = "numeric|YES|0" ] && ok "the refused rollback left the column in place" \
    || bad "the refused rollback dropped the column"
v=$($PSQL -tAc "SELECT netting_adjustment::text FROM trading.executions WHERE exec_id='EXEC_6C.v.0_20260429'")
[ "$v" = "-0.50653209" ] && ok "the refused rollback destroyed nothing" || bad "the refused rollback lost the value ($v)"

if $PSQL -c "SET migration.allow_netting_drop = 'yes'" -f "$DOWN" > /dev/null 2>&1; then
    ok "rollback completes with the session override"
else
    bad "rollback failed with the override"
fi
[ "$(coltype trading)" = "ABSENT" ] && [ "$(coltype backtest)" = "ABSENT" ] \
    && ok "both columns are gone" || bad "a column survived the rollback"
[ "$(comment_of trading.live_results equity_to_margin_ratio)" = "$OLD_COMMENT" ] \
    && ok "equity_to_margin_ratio comment restored verbatim" || bad "comment not restored"

$PSQL -c "DELETE FROM trading.executions WHERE exec_id IN ('EXEC_6C.v.0_20260429','EXEC_MBT.v.0_20260425');
          DELETE FROM backtest.executions WHERE execution_id = 'EX-TF-1'" > /dev/null
[ "$(fingerprint trading.executions)" = "$before_fp_tr" ] && ok "trading.executions original rows survived the round trip" \
    || bad "trading.executions rows differ after the round trip"
[ "$(fingerprint backtest.executions)" = "$before_fp_bt" ] && ok "backtest.executions original rows survived the round trip" \
    || bad "backtest.executions rows differ after the round trip"
if [ "$(objects)" = "$before_objects" ]; then
    ok "every index and constraint survived the round trip"
else
    bad "an index or constraint did not survive the round trip"; diff <(echo "$before_objects") <(objects) || true
fi

# A clean database (no non-zero rows) rolls back without the override.
$PSQL -f "$UP" > /dev/null
if $PSQL -f "$DOWN" > /dev/null 2>&1; then ok "rollback needs no override when every adjustment is 0 or NULL"
else bad "rollback refused with no non-zero rows"; fi

echo ""
echo "RESULT: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
