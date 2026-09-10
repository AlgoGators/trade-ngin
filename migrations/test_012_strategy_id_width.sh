#!/usr/bin/env bash
# Verifies migrations/012_strategy_id_width.sql against a real PostgreSQL, using the exact
# shape of the three tables as introspected from production.
#
# The point is not "does the SQL parse". It is to prove:
#   1. the 62-character joined id a third enabled trend strategy produces is genuinely
#      IMPOSSIBLE to store before the migration, in all three tables (the migration is
#      necessary, not decorative), and possible afterwards;
#   2. NOTHING ELSE MOVES: every existing row is byte-identical before and after, by a hash
#      of the whole row, and every index and constraint is still there, still valid, with
#      the same definition;
#   3. the rollback refuses rather than destroying a row that needs the width, and completes
#      once no row does, leaving the original rows intact.
#
# Requires a running postgres reachable via PGHOST/PGPORT/PGUSER/PGPASSWORD.
#
# DESTRUCTIVE: the fixture begins with DROP SCHEMA trading CASCADE on whatever those env
# vars point at. To run it you must name the scratch database explicitly:
# export MIGRATION_TEST_DB=<dbname> with PGDATABASE set to the same value. Anything else
# (including production) is refused.

set -euo pipefail

if [[ "${MIGRATION_TEST_DB:-}" == "" ]]; then
    echo "REFUSING to run: this script DROPS SCHEMA trading CASCADE on the target DB." >&2
    echo "Set MIGRATION_TEST_DB=<scratch dbname> AND PGDATABASE to the same value." >&2
    exit 2
fi
if [[ "${PGDATABASE:-}" != "${MIGRATION_TEST_DB}" ]]; then
    echo "REFUSING to run: PGDATABASE='${PGDATABASE:-}' != MIGRATION_TEST_DB='${MIGRATION_TEST_DB}'." >&2
    exit 2
fi
if [[ "${MIGRATION_TEST_DB}" == "new_algo_data" ]]; then
    echo "REFUSING to run against new_algo_data: that is the production database." >&2
    exit 2
fi

PSQL="psql -v ON_ERROR_STOP=1 -q -X"
HERE="$(cd "$(dirname "$0")" && pwd)"
pass=0
fail=0

ok()  { echo "  PASS  $1"; pass=$((pass + 1)); }
bad() { echo "  FAIL  $1"; fail=$((fail + 1)); }

JOINED='LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST_TREND_FOLLOWING_SLOW'   # 62 characters

# Hash of every row of a table, ordered by its own text so the hash does not depend on
# physical order. This is what proves a widening did not touch the data.
fingerprint() {
    $PSQL -tAc "SELECT coalesce(md5(string_agg(x::text, ';' ORDER BY x::text)), 'EMPTY')
                  FROM (SELECT to_jsonb(t) x FROM trading.$1 t) s"
}
rowcount() { $PSQL -tAc "SELECT count(*) FROM trading.$1"; }
width()    { $PSQL -tAc "SELECT coalesce(character_maximum_length, -1)
                           FROM information_schema.columns
                          WHERE table_schema='trading' AND table_name='$1'
                            AND column_name='strategy_id'"; }
objects() {
    $PSQL -tAc "SELECT c.relname||'|'||i.relname||'|'||pg_get_indexdef(i.oid)||'|'||x.indisvalid AS o
                  FROM pg_index x JOIN pg_class c ON c.oid=x.indrelid
                  JOIN pg_class i ON i.oid=x.indexrelid
                  JOIN pg_namespace n ON n.oid=c.relnamespace
                 WHERE n.nspname='trading'
                   AND c.relname IN ('positions','live_results','signals')
                 ORDER BY o"
    $PSQL -tAc "SELECT conrelid::regclass::text||'|'||conname||'|'||contype::text||'|'||pg_get_constraintdef(oid) AS o
                  FROM pg_constraint
                 WHERE conrelid::regclass::text IN
                       ('trading.positions','trading.live_results','trading.signals')
                 ORDER BY o"
}

# --- fixture: the three tables as production has them ------------------------
$PSQL <<'SQL'
DROP SCHEMA IF EXISTS trading CASCADE;
CREATE SCHEMA trading;

CREATE TABLE trading.positions (
    symbol               VARCHAR(20)  NOT NULL,
    quantity             NUMERIC      NOT NULL,
    average_price        NUMERIC      NOT NULL,
    daily_unrealized_pnl NUMERIC      NOT NULL,
    daily_realized_pnl   NUMERIC      NOT NULL,
    last_update          TIMESTAMPTZ  NOT NULL,
    updated_at           TIMESTAMPTZ  DEFAULT CURRENT_TIMESTAMP,
    strategy_id          VARCHAR(50)  NOT NULL,
    strategy_name        VARCHAR(100) NOT NULL,
    date                 DATE         NOT NULL,
    portfolio_id         VARCHAR(100) NOT NULL,
    portfolio_type       TEXT         NOT NULL DEFAULT 'system',
    CONSTRAINT positions_pkey PRIMARY KEY
        (portfolio_id, strategy_id, strategy_name, date, symbol, portfolio_type),
    CONSTRAINT chk_positions_average_price CHECK (average_price >= 0),
    CONSTRAINT positions_portfolio_type_check CHECK (portfolio_type IN ('system','qt'))
);
CREATE INDEX idx_positions_strategy_id ON trading.positions (strategy_id);
CREATE INDEX idx_trading_positions_portfolio_id ON trading.positions (portfolio_id);
CREATE INDEX idx_trading_positions_strategy_name ON trading.positions (strategy_name);

CREATE TABLE trading.live_results (
    id           SERIAL PRIMARY KEY,
    strategy_id  VARCHAR(50)  NOT NULL,
    portfolio_id VARCHAR(100) NOT NULL,
    date         DATE         NOT NULL,
    daily_pnl    NUMERIC,
    CONSTRAINT live_results_portfolio_strategy_date_key
        UNIQUE (portfolio_id, strategy_id, date)
);
CREATE INDEX idx_trading_live_results_portfolio_id ON trading.live_results (portfolio_id);

CREATE TABLE trading.signals (
    id            SERIAL PRIMARY KEY,
    strategy_id   VARCHAR(50)  NOT NULL,
    strategy_name VARCHAR(100) NOT NULL,
    symbol        VARCHAR(20)  NOT NULL,
    signal_value  DOUBLE PRECISION,
    timestamp     TIMESTAMPTZ  NOT NULL,
    portfolio_id  VARCHAR(100) NOT NULL,
    CONSTRAINT signals_portfolio_strategy_name_symbol_timestamp_key
        UNIQUE (portfolio_id, strategy_id, strategy_name, symbol, "timestamp")
);
CREATE INDEX idx_signals_strategy ON trading.signals (strategy_id);
CREATE INDEX idx_trading_signals_strategy_symbol_time
    ON trading.signals (strategy_id, symbol, "timestamp");

-- executions is already varchar(100): the asymmetry the migration removes.
CREATE TABLE trading.executions (
    id          SERIAL PRIMARY KEY,
    strategy_id VARCHAR(100) NOT NULL
);

-- Rows that must survive untouched: the two-strategy joined id in use today.
INSERT INTO trading.positions
    (symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl,
     last_update, strategy_id, strategy_name, date, portfolio_id)
VALUES ('ES.v.0', 3, 5000, 10, 0, '2026-05-03 05:00:00+00',
        'LIVE_TREND_FOLLOWING_TREND_FOLLOWING_FAST', 'TREND_FOLLOWING', '2026-05-03',
        'BASE_PORTFOLIO'),
       ('NG.v.0', -2, 3.1, -5, 0, '2026-05-03 05:00:00+00',
        'LIVE_TREND_FOLLOWING', 'TREND_FOLLOWING', '2026-05-03', 'CONSERVATIVE_PORTFOLIO');

INSERT INTO trading.live_results (strategy_id, portfolio_id, date, daily_pnl)
VALUES ('LIVE_TREND_FOLLOWING', 'CONSERVATIVE_PORTFOLIO', '2026-05-03', 1234.5),
       ('LIVE_EQUITY_MEAN_REVERSION', 'EQUITY_MR_PORTFOLIO', '2026-05-03', -12.25);

INSERT INTO trading.signals (strategy_id, strategy_name, symbol, signal_value, timestamp, portfolio_id)
VALUES ('LIVE_TREND_FOLLOWING', 'TREND_FOLLOWING', 'ES.v.0', 0.42,
        '2026-05-03 05:00:00+00', 'CONSERVATIVE_PORTFOLIO');
SQL

echo "########## BEFORE ##########"

before_objects="$(objects)"
for t in positions live_results signals; do
    eval "before_fp_$t=$(fingerprint $t)"
    eval "before_n_$t=$(rowcount $t)"
    w=$(width $t)
    [ "$w" = "50" ] && ok "trading.$t.strategy_id starts at varchar(50)" \
                    || bad "trading.$t.strategy_id starts at varchar($w), expected 50"
done
[ "$(width executions)" = "100" ] && ok "trading.executions.strategy_id is already varchar(100)" \
                                  || bad "fixture wrong: executions is not varchar(100)"

# The whole reason the migration exists.
for t in positions live_results signals; do
    case $t in
      positions)    SQL_INS="INSERT INTO trading.positions (symbol,quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update,strategy_id,strategy_name,date,portfolio_id) VALUES ('CL.v.0',1,60,0,0,'2026-05-04 05:00:00+00','$JOINED','TREND_FOLLOWING','2026-05-04','BASE_PORTFOLIO')";;
      live_results) SQL_INS="INSERT INTO trading.live_results (strategy_id,portfolio_id,date,daily_pnl) VALUES ('$JOINED','BASE_PORTFOLIO','2026-05-04',1)";;
      signals)      SQL_INS="INSERT INTO trading.signals (strategy_id,strategy_name,symbol,signal_value,timestamp,portfolio_id) VALUES ('$JOINED','TREND_FOLLOWING','CL.v.0',0.1,'2026-05-04 05:00:00+00','BASE_PORTFOLIO')";;
    esac
    eval "INS_$t=\$SQL_INS"
    if $PSQL -c "$SQL_INS" > /dev/null 2>&1; then
        bad "trading.$t accepted the 62-character joined id BEFORE the migration"
    else
        ok "trading.$t rejects the 62-character joined id before the migration -- migration is necessary"
    fi
done

echo ""
echo "########## APPLY ##########"
if $PSQL -f "$HERE/012_strategy_id_width.sql" > /dev/null 2>&1; then
    ok "migration applied"
else
    bad "migration failed to apply"
    exit 1
fi
# Idempotent: applying it twice must be a no-op, not an error.
if $PSQL -f "$HERE/012_strategy_id_width.sql" > /dev/null 2>&1; then
    ok "migration is idempotent (a second apply is a no-op)"
else
    bad "a second apply failed"
fi

echo ""
echo "########## AFTER ##########"
for t in positions live_results signals; do
    w=$(width $t)
    [ "$w" = "100" ] && ok "trading.$t.strategy_id is now varchar(100)" \
                     || bad "trading.$t.strategy_id is varchar($w), expected 100"
done

# NOTHING ELSE MOVED.
for t in positions live_results signals; do
    eval "b_fp=\$before_fp_$t"; eval "b_n=\$before_n_$t"
    a_fp=$(fingerprint $t); a_n=$(rowcount $t)
    [ "$a_n" = "$b_n" ] && ok "trading.$t row count unchanged ($a_n)" \
                        || bad "trading.$t row count $b_n -> $a_n"
    [ "$a_fp" = "$b_fp" ] && ok "trading.$t every row byte-identical (md5 $a_fp)" \
                          || bad "trading.$t rows changed: $b_fp -> $a_fp"
done
if [ "$(objects)" = "$before_objects" ]; then
    ok "every index and constraint on the three tables is unchanged and still valid"
else
    bad "an index or constraint changed"
    diff <(echo "$before_objects") <(objects) || true
fi

# And the id that could not be stored now can be.
for t in positions live_results signals; do
    eval "SQL_INS=\$INS_$t"
    if $PSQL -c "$SQL_INS" > /dev/null 2>&1; then
        ok "trading.$t stores the 62-character joined id after the migration"
    else
        bad "trading.$t still rejects the 62-character joined id"
    fi
done

echo ""
echo "########## ROLLBACK ##########"
# Must refuse while a row needs the width, rather than truncating it.
if $PSQL -f "$HERE/012_strategy_id_width_rollback.sql" > /dev/null 2>&1; then
    bad "rollback should refuse while 62-character ids exist"
else
    ok "rollback refuses to destroy rows that need the width"
fi
for t in positions live_results signals; do
    n=$($PSQL -tAc "SELECT count(*) FROM trading.$t WHERE length(strategy_id) > 50")
    [ "$n" = "1" ] && ok "trading.$t still holds its long-id row after the refused rollback" \
                   || bad "trading.$t lost the long-id row to a refused rollback (n=$n)"
done

$PSQL -c "DELETE FROM trading.positions    WHERE length(strategy_id) > 50" > /dev/null
$PSQL -c "DELETE FROM trading.live_results WHERE length(strategy_id) > 50" > /dev/null
$PSQL -c "DELETE FROM trading.signals      WHERE length(strategy_id) > 50" > /dev/null

if $PSQL -f "$HERE/012_strategy_id_width_rollback.sql" > /dev/null 2>&1; then
    ok "rollback succeeds once no row needs the width"
else
    bad "rollback failed with no long ids present"
fi
for t in positions live_results signals; do
    w=$(width $t)
    [ "$w" = "50" ] && ok "trading.$t.strategy_id is back to varchar(50)" \
                    || bad "trading.$t.strategy_id is varchar($w) after rollback"
    eval "b_fp=\$before_fp_$t"
    a_fp=$(fingerprint $t)
    [ "$a_fp" = "$b_fp" ] && ok "trading.$t original rows survived the round trip" \
                          || bad "trading.$t rows differ after the round trip: $b_fp -> $a_fp"
done
if [ "$(objects)" = "$before_objects" ]; then
    ok "every index and constraint survived the round trip"
else
    bad "an index or constraint did not survive the round trip"
    diff <(echo "$before_objects") <(objects) || true
fi

echo ""
echo "RESULT: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
