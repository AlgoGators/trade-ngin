#!/usr/bin/env bash
# Verifies the qt-seeding SQL used by PostgresDatabase::seed_qt_positions_from_system
# against a real PostgreSQL 16.
#
# The property under test is the one that matters: seeding must NEVER overwrite an
# edit QT has already made. The engine re-runs; a human decision must survive it.
#
# Requires PGHOST/PGPORT/PGUSER/PGPASSWORD/PGDATABASE.

set -uo pipefail

PSQL="psql -v ON_ERROR_STOP=1 -q -X"
pass=0
fail=0
ok()  { echo "  PASS  $1"; pass=$((pass + 1)); }
bad() { echo "  FAIL  $1"; fail=$((fail + 1)); }

# The exact statement the C++ issues, with $1..$4 bound.
seed() {
    $PSQL -tAc "
    INSERT INTO trading.positions
        (symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl,
         last_update, updated_at, strategy_id, strategy_name, date, portfolio_id, portfolio_type)
    SELECT symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl,
           last_update, updated_at, strategy_id, strategy_name, date, portfolio_id, 'qt'
    FROM trading.positions
    WHERE strategy_id = 'SID' AND strategy_name = 'SNAME' AND portfolio_id = 'P'
      AND date = '2026-05-03' AND portfolio_type = 'system'
      AND NOT EXISTS (
          SELECT 1 FROM trading.positions
          WHERE strategy_id = 'SID' AND strategy_name = 'SNAME' AND portfolio_id = 'P'
            AND date = '2026-05-03' AND portfolio_type = 'qt')
    RETURNING 1" | grep -c 1 || true
}

qty() { $PSQL -tAc "SELECT quantity FROM trading.positions WHERE symbol='$1' AND portfolio_type='$2' AND date='2026-05-03'"; }
cnt() { $PSQL -tAc "SELECT count(*) FROM trading.positions WHERE portfolio_type='$1' AND date='2026-05-03'"; }

# --- fixture: post-migration schema -----------------------------------------
$PSQL <<'SQL'
DROP SCHEMA IF EXISTS trading CASCADE;
CREATE SCHEMA trading;
CREATE TABLE trading.positions (
    symbol VARCHAR NOT NULL, quantity NUMERIC NOT NULL, average_price NUMERIC NOT NULL,
    daily_unrealized_pnl NUMERIC NOT NULL, daily_realized_pnl NUMERIC NOT NULL,
    last_update TIMESTAMPTZ NOT NULL, updated_at TIMESTAMPTZ DEFAULT CURRENT_TIMESTAMP,
    strategy_id VARCHAR NOT NULL, strategy_name VARCHAR NOT NULL, date DATE NOT NULL,
    portfolio_id VARCHAR NOT NULL,
    portfolio_type TEXT NOT NULL DEFAULT 'system'
        CHECK (portfolio_type IN ('system','qt')),
    CONSTRAINT positions_pkey PRIMARY KEY
        (portfolio_id, strategy_id, strategy_name, date, symbol, portfolio_type)
);
INSERT INTO trading.positions
 (symbol,quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update,
  strategy_id,strategy_name,date,portfolio_id)
VALUES ('ES',3,5000,0,0,'2026-05-03','SID','SNAME','2026-05-03','P'),
       ('NG',-2,3.1,0,0,'2026-05-03','SID','SNAME','2026-05-03','P');
SQL

echo "########## first run seeds the qt stream ##########"
n=$(seed)
[ "$(cnt qt)" = "2" ] && ok "2 qt rows created from system" || bad "expected 2 qt rows, got $(cnt qt)"
[ "$(qty ES qt)" = "3" ] && ok "qt ES mirrors system (qty=3)" || bad "qt ES = $(qty ES qt), expected 3"

echo ""
echo "########## THE CRITICAL PROPERTY: QT edits survive a re-run ##########"
$PSQL -q -c "UPDATE trading.positions SET quantity = 99 WHERE symbol='ES' AND portfolio_type='qt'"
$PSQL -q -c "DELETE FROM trading.positions WHERE symbol='NG' AND portfolio_type='qt'"
echo "  (QT changed ES to 99 and removed NG entirely)"

seed > /dev/null   # engine re-runs
[ "$(qty ES qt)" = "99" ] && ok "QT's edit preserved (ES still 99, not reset to 3)" \
                          || bad "QT's edit CLOBBERED: ES = $(qty ES qt)"
[ "$(cnt qt)" = "1" ] && ok "QT's deletion preserved (NG not resurrected)" \
                      || bad "deleted row came back, qt count = $(cnt qt)"

echo ""
echo "########## the system stream is untouched throughout ##########"
[ "$(qty ES system)" = "3" ] && ok "system ES still 3 (pure signal preserved)" \
                             || bad "system stream was modified: $(qty ES system)"
[ "$(cnt system)" = "2" ] && ok "system still has both rows" || bad "system count = $(cnt system)"

echo ""
echo "########## idempotent: repeated runs change nothing ##########"
before=$($PSQL -tAc "SELECT count(*) FROM trading.positions")
seed > /dev/null; seed > /dev/null; seed > /dev/null
after=$($PSQL -tAc "SELECT count(*) FROM trading.positions")
[ "$before" = "$after" ] && ok "3 further runs inserted nothing (rows: $after)" \
                         || bad "row count drifted $before -> $after"

echo ""
echo "########## a fresh day seeds independently ##########"
$PSQL -q -c "INSERT INTO trading.positions
 (symbol,quantity,average_price,daily_unrealized_pnl,daily_realized_pnl,last_update,
  strategy_id,strategy_name,date,portfolio_id)
 VALUES ('ES',7,5000,0,0,'2026-05-04','SID','SNAME','2026-05-04','P')"
$PSQL -q -tAc "
  INSERT INTO trading.positions
      (symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl,
       last_update, updated_at, strategy_id, strategy_name, date, portfolio_id, portfolio_type)
  SELECT symbol, quantity, average_price, daily_unrealized_pnl, daily_realized_pnl,
         last_update, updated_at, strategy_id, strategy_name, date, portfolio_id, 'qt'
  FROM trading.positions
  WHERE strategy_id='SID' AND strategy_name='SNAME' AND portfolio_id='P'
    AND date='2026-05-04' AND portfolio_type='system'
    AND NOT EXISTS (SELECT 1 FROM trading.positions
        WHERE strategy_id='SID' AND strategy_name='SNAME' AND portfolio_id='P'
          AND date='2026-05-04' AND portfolio_type='qt')" > /dev/null
d2=$($PSQL -tAc "SELECT count(*) FROM trading.positions WHERE date='2026-05-04' AND portfolio_type='qt'")
[ "$d2" = "1" ] && ok "next day seeded despite prior day already being edited" \
                || bad "next day qt count = $d2, expected 1"

echo ""
echo "RESULT: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
