#!/usr/bin/env bash
# Verifies migrations/019_contract_metadata_fixes.sql and its rollback against a real PostgreSQL,
# on the shape of metadata.contract_metadata and metadata.symbols as the stage-3 scratch has them
# (text columns; the nine rows 019 corrects, with the values stored before it, and three rows it
# must not touch).
#
# The properties under test:
#   1. before: the twenty cells hold the values 019 replaces;
#   2. after: each of the twenty holds its corrected value, in BOTH tables;
#   3. NOTHING ELSE MOVES: with the twenty cells masked, every row of both tables is
#      byte-identical; no row is added or removed (no micro or mini row); every column, index and
#      constraint is unchanged (no DDL);
#   4. a second apply is a no-op;
#   5. value-guarded: a cell holding a value that is neither the replaced nor the corrected one
#      makes 019 refuse and change nothing; a missing row does the same;
#   6. the rollback restores every replaced value and the tables are byte-identical to the
#      fixture; a second rollback is a no-op; the rollback refuses on a foreign value;
#   7. 019 applies again after its rollback.
#
# Requires a running postgres reachable via PGHOST/PGPORT/PGUSER/PGPASSWORD.
#
# DESTRUCTIVE: the fixture begins with DROP SCHEMA metadata CASCADE on whatever those env vars
# point at. Point it at a THROWAWAY database created for the purpose, for example
#
#   createdb t019_migration_test
#   PGDATABASE=t019_migration_test MIGRATION_TEST_DB=t019_migration_test ./test_019_contract_metadata_fixes.sh
#   dropdb t019_migration_test
#
# Never production and never a stage-3 scratch or clone: those are refused by name below, and
# any database that already has a metadata schema is refused whatever it is called.

set -euo pipefail

if [[ "${MIGRATION_TEST_DB:-}" == "" ]]; then
    echo "REFUSING to run: this script DROPS SCHEMA metadata CASCADE on the target DB." >&2
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
        echo "scratch or clone, and this script DROPS the metadata schema." >&2
        exit 2
        ;;
esac
existing=$(psql -X -tAc "SELECT count(*) FROM information_schema.schemata
                          WHERE schema_name = 'metadata'" 2>/dev/null || echo 0)
if [[ "${existing:-0}" != "0" ]]; then
    echo "REFUSING to run against '${MIGRATION_TEST_DB}': it already has a metadata schema." >&2
    echo "This script drops it. Point it at an empty throwaway database." >&2
    exit 2
fi

PSQL="psql -v ON_ERROR_STOP=1 -q -X"
HERE="$(cd "$(dirname "$0")" && pwd)"
UP="$HERE/019_contract_metadata_fixes.sql"
DOWN="$HERE/019_contract_metadata_fixes_rollback.sql"
pass=0
fail=0
ok()  { echo "  PASS  $1"; pass=$((pass + 1)); }
bad() { echo "  FAIL  $1"; fail=$((fail + 1)); }

# Every row of both tables as text, ordered: the whole content.
full() {
    $PSQL -tAc "SELECT md5(coalesce(string_agg(x, ';' ORDER BY x), 'EMPTY'))
                  FROM (SELECT 'cm:'||to_jsonb(t)::text x FROM metadata.contract_metadata t
                        UNION ALL
                        SELECT 'sy:'||to_jsonb(t)::text FROM metadata.symbols t) s"
}
# The same with the twenty cells 019 owns removed: proves no other cell moved.
masked() {
    $PSQL -tAc "
      SELECT md5(coalesce(string_agg(x, ';' ORDER BY x), 'EMPTY')) FROM (
        SELECT 'cm:'||(to_jsonb(t)
                 - CASE WHEN \"Databento Symbol\" IN ('6A','6L') THEN ARRAY['Tick Size','Minimum Price Fluctuation'] ELSE ARRAY[]::text[] END
                 - CASE WHEN \"Databento Symbol\" IN ('6B','6E','6S','ZS','ZW') THEN ARRAY['IB Symbol'] ELSE ARRAY[]::text[] END
                 - CASE WHEN \"Databento Symbol\" IN ('HO','NG','6L') THEN ARRAY['Contract Months'] ELSE ARRAY[]::text[] END)::text x
          FROM metadata.contract_metadata t
        UNION ALL
        SELECT 'sy:'||(to_jsonb(t)
                 - CASE WHEN \"Databento Symbol\" IN ('6B','6E','6S','ZS','ZW') THEN ARRAY['IB Symbol'] ELSE ARRAY[]::text[] END
                 - CASE WHEN \"Databento Symbol\" IN ('HO','NG','6L') THEN ARRAY['Contract Months'] ELSE ARRAY[]::text[] END)::text
          FROM metadata.symbols t) s"
}
shape() {
    $PSQL -tAc "SELECT string_agg(table_name||'.'||ordinal_position||':'||column_name||':'||data_type||':'||is_nullable||':'||coalesce(column_default,''), ',' ORDER BY table_name, ordinal_position)
                  FROM information_schema.columns WHERE table_schema='metadata'"
    $PSQL -tAc "SELECT coalesce(string_agg(indexdef, '|' ORDER BY indexdef),'') FROM pg_indexes WHERE schemaname='metadata'"
    $PSQL -tAc "SELECT coalesce(string_agg(conname||pg_get_constraintdef(oid), '|' ORDER BY conname),'') FROM pg_constraint
                 WHERE connamespace = 'metadata'::regnamespace"
    $PSQL -tAc "SELECT (SELECT count(*) FROM metadata.contract_metadata)||'/'||(SELECT count(*) FROM metadata.symbols)"
}
# The twenty cells, one line, in a fixed order.
cells() {
    $PSQL -tAc "
      SELECT string_agg(v, ' ' ORDER BY k) FROM (
        SELECT 'a'||\"Databento Symbol\" k, \"Databento Symbol\"||'.tick='||\"Tick Size\"||'/'||\"Minimum Price Fluctuation\" v
          FROM metadata.contract_metadata WHERE \"Databento Symbol\" IN ('6A','6L')
        UNION ALL SELECT 'b'||\"Databento Symbol\", \"Databento Symbol\"||'.ib='||\"IB Symbol\"
          FROM metadata.contract_metadata WHERE \"Databento Symbol\" IN ('6B','6E','6S','ZS','ZW')
        UNION ALL SELECT 'c'||\"Databento Symbol\", \"Databento Symbol\"||'.sib='||\"IB Symbol\"
          FROM metadata.symbols WHERE \"Databento Symbol\" IN ('6B','6E','6S','ZS','ZW')
        UNION ALL SELECT 'd'||\"Databento Symbol\", \"Databento Symbol\"||'.months='||\"Contract Months\"
          FROM metadata.contract_metadata WHERE \"Databento Symbol\" IN ('HO','NG','6L')
        UNION ALL SELECT 'e'||\"Databento Symbol\", \"Databento Symbol\"||'.smonths='||\"Contract Months\"
          FROM metadata.symbols WHERE \"Databento Symbol\" IN ('HO','NG','6L')) s"
}
OLD_CELLS="6A.tick=0.0001/10 6L.tick=0.0001/10 6B.ib=M6B 6E.ib=M6E 6S.ib=MSF ZS.ib=YK ZW.ib=YW 6B.sib=M6B 6E.sib=M6E 6S.sib=MSF ZS.sib=YK ZW.sib=YW 6L.months=Mar Jun Sep Dec HO.months=Mar Jun Sep Dec NG.months=Mar Jun Sep Dec 6L.smonths=Mar Jun Sep Dec HO.smonths=Mar Jun Sep Dec NG.smonths=Mar Jun Sep Dec"
NEW_CELLS="6A.tick=0.00005/5 6L.tick=0.00005/5 6B.ib=GBP 6E.ib=EUR 6S.ib=CHF ZS.ib=ZS ZW.ib=ZW 6B.sib=GBP 6E.sib=EUR 6S.sib=CHF ZS.sib=ZS ZW.sib=ZW 6L.months=All Months HO.months=All Months NG.months=All Months 6L.smonths=All Months HO.smonths=All Months NG.smonths=All Months"

# --- fixture: both tables as the stage-3 scratch has them; the nine rows and three bystanders ---
$PSQL <<'SQL'
DROP SCHEMA IF EXISTS metadata CASCADE;
CREATE SCHEMA metadata;
CREATE TABLE metadata.contract_metadata (
    "Databento Symbol"             TEXT NOT NULL,
    "IB Symbol"                    TEXT NOT NULL,
    "Name"                         TEXT NOT NULL,
    "Exchange"                     TEXT NOT NULL,
    "Intraday Initial Margin"      TEXT NOT NULL,
    "Intraday Maintenance Margin"  TEXT NOT NULL,
    "Overnight Initial Margin"     TEXT NOT NULL,
    "Overnight Maintenance Margin" TEXT NOT NULL,
    "Asset Type"                   TEXT NOT NULL,
    "Sector"                       TEXT NOT NULL,
    "Contract Size"                TEXT NOT NULL,
    "Units"                        TEXT NOT NULL,
    "Minimum Price Fluctuation"    TEXT NOT NULL,
    "Tick Size"                    TEXT NOT NULL,
    "Settlement Type"              TEXT NOT NULL,
    "Trading Hours (EST)"          TEXT NOT NULL,
    "Data Provider"                TEXT NOT NULL,
    "Dataset"                      TEXT NOT NULL,
    "Newest Month Additions"       TEXT NOT NULL,
    "Contract Months"              TEXT NOT NULL,
    "Time of Expiry"               TEXT NOT NULL,
    "Additional Notes"             TEXT,
    "Fee Per Contract"             TEXT NOT NULL DEFAULT '1.50'
);
CREATE TABLE metadata.symbols (
    "Databento Symbol" TEXT,
    "IB Symbol"        TEXT,
    "Name"             TEXT,
    "Contract Months"  TEXT
);
INSERT INTO metadata.contract_metadata
 ("Databento Symbol","IB Symbol","Name","Exchange","Intraday Initial Margin","Intraday Maintenance Margin",
  "Overnight Initial Margin","Overnight Maintenance Margin","Asset Type","Sector","Contract Size","Units",
  "Minimum Price Fluctuation","Tick Size","Settlement Type","Trading Hours (EST)","Data Provider","Dataset",
  "Newest Month Additions","Contract Months","Time of Expiry","Additional Notes") VALUES
 ('6A','6A','Australian Dollar Futures','CME','2000','1800','2200','2000','Futures','FX','100000','AUD','10','0.0001','Deliverable','18:00-17:00','Databento','GLBX.MDP3','','Mar Jun Sep Dec','09:16',NULL),
 ('6L','6L','Brazilian Real Futures','CME','1500','1400','1650','1500','Futures','FX','100000','BRL','10','0.0001','Cash','18:00-17:00','Databento','GLBX.MDP3','','Mar Jun Sep Dec','09:15',NULL),
 ('6B','M6B','GBP/USD','CME','2400','2200','2600','2400','Futures','FX','62500','GBP','6.25','0.0001','Deliverable','18:00-17:00','Databento','GLBX.MDP3','','MAR, JUN, SEP, DEC','09:16',NULL),
 ('6E','M6E','EUR/USD','CME','2500','2300','2800','2500','Futures','FX','125000','EUR','6.25','0.00005','Deliverable','18:00-17:00','Databento','GLBX.MDP3','','MAR, JUN, SEP, DEC','09:16',NULL),
 ('6S','MSF','CHF/USD','CME','4000','3600','4400','4000','Futures','FX','125000','CHF','6.25','0.00005','Deliverable','18:00-17:00','Databento','GLBX.MDP3','','MAR, JUN, SEP, DEC','09:16',NULL),
 ('ZS','YK','Soybean','CBOT','2000','1800','2200','2000','Futures','Ags','50','cents','12.5','0.25','Deliverable','20:00-14:20','Databento','GLBX.MDP3','','JAN, MAR, MAY, JULY, AUG, SEP, NOV','12:01',NULL),
 ('ZW','YW','Chicago Soft Red Winter Wheat  Mini ','CBOT','1800','1600','2000','1800','Futures','Ags','50','cents','12.5','0.25','Deliverable','20:00-14:20','Databento','GLBX.MDP3','','MAR, MAY, JULY, SEP, DEC','12:01',NULL),
 ('HO','HO','Heating Oil Futures','NYMEX','7000','6400','7700','7000','Futures','Energy','42000','gallons','4.20','0.0001','Deliverable','18:00-17:00','Databento','GLBX.MDP3','','Mar Jun Sep Dec','14:30',NULL),
 ('NG','NG','Natural Gas Futures','NYMEX','5000','4500','5500','5000','Futures','Energy','10000','MMBtu','10','0.001','Deliverable','18:00-17:00','Databento','GLBX.MDP3','','Mar Jun Sep Dec','14:30',NULL),
 ('CL','CL','Crude Oil Futures','NYMEX','6000','5500','6600','6000','Futures','Energy','1000','barrels','10','0.01','Deliverable','18:00-17:00','Databento','GLBX.MDP3','','All Months','14:30',NULL),
 ('6C','CAD','CAD/USD','CME','1300','1200','1430','1300','Futures','FX','100000','CAD','5','0.00005','Deliverable','18:00-17:00','Databento','GLBX.MDP3','','MAR, JUN, SEP, DEC','09:16',NULL),
 ('ZT','ZT','2-Year US Treasury Note Futures','CBOT','1100','1000','1210','1100','Futures','Rates','2000','USD','7.8125','0.00390625','Deliverable','18:00-17:00','Databento','GLBX.MDP3','','Mar Jun Sep Dec','12:01',NULL);
INSERT INTO metadata.symbols
 SELECT "Databento Symbol","IB Symbol","Name","Contract Months" FROM metadata.contract_metadata;
SQL

echo "########## BEFORE ##########"
fx_full=$(full); fx_masked=$(masked); fx_shape="$(shape)"
[ "$(cells)" = "$OLD_CELLS" ] && ok "the twenty cells hold the values 019 replaces" || bad "fixture cells: $(cells)"

echo ""
echo "########## APPLY ##########"
if $PSQL -f "$UP" > /dev/null 2>&1; then ok "migration applied"; else bad "migration failed"; exit 1; fi
[ "$(cells)" = "$NEW_CELLS" ] && ok "each of the twenty cells holds its corrected value, in both tables" || bad "after: $(cells)"
[ "$(masked)" = "$fx_masked" ] && ok "every other cell of both tables byte-identical" || bad "another cell moved"
[ "$(full)" != "$fx_full" ] && ok "the tables' content did change (the mask is not hiding everything)" || bad "nothing changed"
[ "$(shape)" = "$fx_shape" ] && ok "no DDL: columns, indexes, constraints and row counts unchanged (no micro or mini row)" || bad "shape changed"
up_full=$(full)
if $PSQL -f "$UP" > /dev/null 2>&1 && [ "$(full)" = "$up_full" ]; then ok "a second apply is a no-op"; else bad "a second apply failed or moved a cell"; fi

echo ""
echo "########## VALUE GUARD ##########"
$PSQL -c "UPDATE metadata.contract_metadata SET \"Tick Size\"='0.0002' WHERE \"Databento Symbol\"='6A'"
$PSQL -c "UPDATE metadata.symbols SET \"IB Symbol\"='M6B' WHERE \"Databento Symbol\"='6B'"
g_full=$(full)
if $PSQL -f "$UP" > /dev/null 2>&1; then bad "019 applied over a foreign tick value"
else ok "019 refuses when a cell holds a value that is neither the replaced nor the corrected one"; fi
[ "$(full)" = "$g_full" ] && ok "the refusal changed nothing (the M6B cell it could have fixed is still M6B)" || bad "a refused apply moved a cell"
if $PSQL -f "$DOWN" > /dev/null 2>&1; then bad "the rollback ran over a foreign tick value"
else ok "the rollback refuses on the same foreign value"; fi
[ "$(full)" = "$g_full" ] && ok "the refused rollback changed nothing" || bad "a refused rollback moved a cell"
$PSQL -c "UPDATE metadata.contract_metadata SET \"Tick Size\"='0.00005' WHERE \"Databento Symbol\"='6A'"
if $PSQL -f "$UP" > /dev/null 2>&1 && [ "$(full)" = "$up_full" ]; then ok "with the foreign value gone, 019 completes the partly corrected tables"
else bad "019 did not complete a partly corrected state"; fi
$PSQL -c "CREATE TEMP TABLE keep AS SELECT * FROM metadata.symbols WHERE \"Databento Symbol\"='NG'" \
      -c "DELETE FROM metadata.symbols WHERE \"Databento Symbol\"='NG'" > /dev/null
m_full=$(full)
if $PSQL -f "$UP" > /dev/null 2>&1; then bad "019 applied with a row missing"
else ok "019 refuses when one of its rows is missing"; fi
[ "$(full)" = "$m_full" ] && ok "that refusal changed nothing" || bad "a refused apply moved a cell"
$PSQL -c "INSERT INTO metadata.symbols VALUES ('NG','NG','Natural Gas Futures','All Months')"
[ "$(full)" = "$up_full" ] && ok "fixture row put back" || bad "fixture row not restored"

echo ""
echo "########## ROLLBACK ##########"
if $PSQL -f "$DOWN" > /dev/null 2>&1; then ok "rollback applied"; else bad "rollback failed"; fi
[ "$(cells)" = "$OLD_CELLS" ] && ok "every replaced value restored" || bad "after rollback: $(cells)"
[ "$(full)" = "$fx_full" ] && ok "both tables byte-identical to before 019" || bad "tables differ from the fixture"
[ "$(shape)" = "$fx_shape" ] && ok "shape unchanged by the rollback" || bad "shape changed"
if $PSQL -f "$DOWN" > /dev/null 2>&1 && [ "$(full)" = "$fx_full" ]; then ok "a second rollback is a no-op"; else bad "a second rollback failed or moved a cell"; fi
if $PSQL -f "$UP" > /dev/null 2>&1 && [ "$(full)" = "$up_full" ]; then ok "019 applies again after its rollback"; else bad "re-apply after rollback failed"; fi

$PSQL -c "DROP SCHEMA metadata CASCADE" > /dev/null
echo ""
echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
