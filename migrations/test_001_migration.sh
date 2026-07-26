#!/usr/bin/env bash
# Verifies migrations/001_add_portfolio_type.sql against a real PostgreSQL 16,
# using the exact schema introspected from production on 2026-07-25.
#
# The point of this test is not "does the SQL parse" -- it is to prove:
#   1. the dual-portfolio write is genuinely IMPOSSIBLE before the migration
#      (i.e. the migration is necessary, not decorative), and
#   2. it becomes possible afterwards, with existing rows untouched.
#
# Requires a running postgres reachable via PGHOST/PGPORT/PGUSER/PGPASSWORD.

set -uo pipefail

PSQL="psql -v ON_ERROR_STOP=1 -q -X"
pass=0
fail=0

ok()   { echo "  PASS  $1"; pass=$((pass + 1)); }
bad()  { echo "  FAIL  $1"; fail=$((fail + 1)); }

# --- fixture: the live schema, exactly as introspected -----------------------
$PSQL <<'SQL'
DROP SCHEMA IF EXISTS trading CASCADE;
CREATE SCHEMA trading;

CREATE TABLE trading.positions (
    symbol               VARCHAR     NOT NULL,
    quantity             NUMERIC     NOT NULL,
    average_price        NUMERIC     NOT NULL,
    daily_unrealized_pnl NUMERIC     NOT NULL,
    daily_realized_pnl   NUMERIC     NOT NULL,
    last_update          TIMESTAMPTZ NOT NULL,
    updated_at           TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
    strategy_id          VARCHAR     NOT NULL,
    strategy_name        VARCHAR     NOT NULL,
    date                 DATE        NOT NULL,
    portfolio_id         VARCHAR     NOT NULL,
    CONSTRAINT positions_pkey PRIMARY KEY (portfolio_id, strategy_id, strategy_name, date, symbol)
);

CREATE TABLE trading.equity_curve (
    id           SERIAL PRIMARY KEY,
    strategy_id  VARCHAR          NOT NULL,
    timestamp    TIMESTAMPTZ      NOT NULL,
    equity       DOUBLE PRECISION NOT NULL,
    portfolio_id VARCHAR,
    CONSTRAINT trading_equity_curve_unique UNIQUE (portfolio_id, strategy_id, "timestamp")
);

INSERT INTO trading.positions
    (symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl,
     last_update, strategy_id, strategy_name, date, portfolio_id)
VALUES
    ('ES.v.0', 3, 5000, 10, 0, now(), 'LIVE_TREND_FOLLOWING', 'TREND_FOLLOWING',
     '2026-05-03', 'CONSERVATIVE_PORTFOLIO'),
    ('NG.v.0', -2, 3.1, -5, 0, now(), 'LIVE_TREND_FOLLOWING', 'TREND_FOLLOWING',
     '2026-05-03', 'CONSERVATIVE_PORTFOLIO');

INSERT INTO trading.equity_curve (strategy_id, timestamp, equity, portfolio_id)
VALUES ('LIVE_TREND_FOLLOWING', '2026-05-03 05:00:00+00', 500000, 'CONSERVATIVE_PORTFOLIO');
SQL

echo "########## BEFORE the migration ##########"

# The whole reason the migration exists: this write must be rejected.
if $PSQL <<'SQL' > /dev/null 2>&1
INSERT INTO trading.positions
    (symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl,
     last_update, strategy_id, strategy_name, date, portfolio_id)
VALUES ('ES.v.0', 5, 5000, 10, 0, now(), 'LIVE_TREND_FOLLOWING', 'TREND_FOLLOWING',
        '2026-05-03', 'CONSERVATIVE_PORTFOLIO');
SQL
then
    bad "a second stream should NOT be writable before the migration"
else
    ok "second stream correctly rejected (primary key violation) -- migration is necessary"
fi

echo ""
echo "########## APPLY ##########"
if $PSQL -f "$(dirname "$0")/001_add_portfolio_type.sql" > /dev/null 2>&1; then
    ok "migration applied"
else
    bad "migration failed to apply"
    exit 1
fi

echo ""
echo "########## AFTER ##########"

n=$($PSQL -tAc "SELECT count(*) FROM trading.positions WHERE portfolio_type = 'system'")
[ "$n" = "2" ] && ok "existing rows defaulted to 'system' (n=$n)" \
                || bad "expected 2 system rows, got $n"

# The write that was impossible before must now succeed.
if $PSQL <<'SQL' > /dev/null 2>&1
INSERT INTO trading.positions
    (symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl,
     last_update, strategy_id, strategy_name, date, portfolio_id, portfolio_type)
VALUES ('ES.v.0', 5, 5000, 10, 0, now(), 'LIVE_TREND_FOLLOWING', 'TREND_FOLLOWING',
        '2026-05-03', 'CONSERVATIVE_PORTFOLIO', 'qt');
SQL
then
    ok "qt stream now writable alongside system for the same key"
else
    bad "qt stream still rejected after migration"
fi

# ... but a true duplicate within one stream must still be rejected.
if $PSQL <<'SQL' > /dev/null 2>&1
INSERT INTO trading.positions
    (symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl,
     last_update, strategy_id, strategy_name, date, portfolio_id, portfolio_type)
VALUES ('ES.v.0', 9, 5000, 10, 0, now(), 'LIVE_TREND_FOLLOWING', 'TREND_FOLLOWING',
        '2026-05-03', 'CONSERVATIVE_PORTFOLIO', 'qt');
SQL
then
    bad "duplicate within the same stream should still be rejected"
else
    ok "duplicate within a stream still correctly rejected"
fi

if $PSQL -c "INSERT INTO trading.positions
    (symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl,
     last_update, strategy_id, strategy_name, date, portfolio_id, portfolio_type)
    VALUES ('X', 1, 1, 0, 0, now(), 's', 'n', '2026-05-03', 'p', 'nonsense')" > /dev/null 2>&1
then
    bad "CHECK should reject an unknown portfolio_type"
else
    ok "CHECK rejects an unknown portfolio_type"
fi

# equity curve: same key, two streams
if $PSQL <<'SQL' > /dev/null 2>&1
INSERT INTO trading.equity_curve (strategy_id, timestamp, equity, portfolio_id, portfolio_type)
VALUES ('LIVE_TREND_FOLLOWING', '2026-05-03 05:00:00+00', 499000, 'CONSERVATIVE_PORTFOLIO', 'qt');
SQL
then
    ok "equity_curve accepts both streams at the same timestamp"
else
    bad "equity_curve still rejects the second stream"
fi

echo ""
echo "########## IDEMPOTENCY ##########"
if $PSQL -f "$(dirname "$0")/001_add_portfolio_type.sql" > /dev/null 2>&1; then
    ok "re-running the migration is safe"
else
    bad "migration is not idempotent"
fi

echo ""
echo "########## ROLLBACK ##########"
# Must refuse while qt rows exist, rather than destroying them.
if $PSQL -f "$(dirname "$0")/001_add_portfolio_type_rollback.sql" > /dev/null 2>&1; then
    bad "rollback should refuse while qt rows exist"
else
    ok "rollback refuses to destroy existing qt rows"
fi

$PSQL -c "DELETE FROM trading.positions WHERE portfolio_type='qt'" > /dev/null 2>&1
$PSQL -c "DELETE FROM trading.equity_curve WHERE portfolio_type='qt'" > /dev/null 2>&1

if $PSQL -f "$(dirname "$0")/001_add_portfolio_type_rollback.sql" > /dev/null 2>&1; then
    ok "rollback succeeds once qt rows are gone"
else
    bad "rollback failed even with no qt rows"
fi

cols=$($PSQL -tAc "SELECT count(*) FROM information_schema.columns
                   WHERE table_schema='trading' AND table_name='positions'
                     AND column_name='portfolio_type'")
[ "$cols" = "0" ] && ok "portfolio_type removed by rollback" \
                  || bad "portfolio_type still present after rollback"

n=$($PSQL -tAc "SELECT count(*) FROM trading.positions")
[ "$n" = "2" ] && ok "original rows survived the round trip (n=$n)" \
               || bad "expected 2 rows after rollback, got $n"

echo ""
echo "RESULT: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
