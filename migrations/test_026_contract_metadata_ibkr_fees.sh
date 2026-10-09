#!/usr/bin/env bash
# Verifies migrations/026_contract_metadata_ibkr_fees.sql and its rollback against a real
# PostgreSQL, on the shape of metadata.contract_metadata as the stage-3 scratch has it (text
# columns, the 40 rows, every "Fee Per Contract" at 014's 1.50).
#
# The properties under test:
#   1. before: the 40 fees are 1.50 and an empty fee cell can be stored;
#   2. after: each of the 40 rows holds its IBKR fee (the list below, written here independently
#      of the migration file), and each equals commission + exchange + regulatory;
#   3. NOTHING ELSE MOVES: with "Fee Per Contract" masked every row is byte-identical; no row is
#      added or removed (no micro or mini row); every column and index is unchanged; the one new
#      constraint is the fee CHECK; no table is made;
#   4. the CHECK refuses an empty, zero, negative, non-numeric and blank-padded fee, and accepts
#      another positive decimal;
#   5. a second apply is a no-op;
#   6. value-guarded: a cell holding a value that is neither 1.50 nor its fee makes 026 refuse
#      and change nothing; a missing row and an extra row do the same; so does a database
#      without 014's column;
#   7. the rollback restores the 40 values, drops the CHECK and leaves the table byte-identical
#      to the fixture; a second rollback is a no-op; the rollback refuses on a foreign value;
#   8. 026 applies again after its rollback.
#
# Two modes.
#
#   The full test (default) is DESTRUCTIVE: its fixture begins with DROP SCHEMA metadata CASCADE.
#   Point it at a THROWAWAY database created for the purpose:
#
#     createdb t026_migration_test
#     PGDATABASE=t026_migration_test MIGRATION_TEST_DB=t026_migration_test ./test_026_contract_metadata_ibkr_fees.sh
#     dropdb t026_migration_test
#
#   Never production and never a stage-3 scratch or clone: those are refused by name below, and
#   any database that already has a metadata schema is refused whatever it is called.
#
#   VERIFY_ONLY=1 reads the database PGDATABASE names and writes nothing that survives (its CHECK
#   probes run inside a transaction that is rolled back). It passes on a database that has 026
#   and fails on one that does not. Use it on a clone or on production after applying 026:
#
#     PGDATABASE=<db> VERIFY_ONLY=1 ./test_026_contract_metadata_ibkr_fees.sh
#
# Requires a running postgres reachable via PGHOST/PGPORT/PGUSER/PGPASSWORD.

set -euo pipefail

PSQL="psql -v ON_ERROR_STOP=1 -q -X"
HERE="$(cd "$(dirname "$0")" && pwd)"
UP="$HERE/026_contract_metadata_ibkr_fees.sql"
DOWN="$HERE/026_contract_metadata_ibkr_fees_rollback.sql"
OUT="$(mktemp)"; trap 'rm -f "$OUT"' EXIT
pass=0
fail=0
ok()  { echo "  PASS  $1"; pass=$((pass + 1)); }
bad() { echo "  FAIL  $1"; fail=$((fail + 1)); }

# The 40 fees: symbol, commission, exchange fee, regulatory fee, the stored cell.
FEES="VALUES
 ('6A',0.85,1.600,0.011,'2.461'),('6B',0.85,1.600,0.011,'2.461'),('6C',0.85,1.600,0.011,'2.461'),
 ('6E',0.85,1.600,0.011,'2.461'),('6J',0.85,1.600,0.011,'2.461'),('6L',0.85,1.600,0.011,'2.461'),
 ('6M',0.85,1.600,0.011,'2.461'),('6N',0.85,1.600,0.011,'2.461'),('6S',0.85,1.600,0.011,'2.461'),
 ('ES',0.85,1.386,0.011,'2.247'),('NQ',0.85,1.386,0.011,'2.247'),('RTY',0.85,1.386,0.011,'2.247'),
 ('MES',0.25,0.353,0.011,'0.614'),('MNQ',0.25,0.353,0.011,'0.614'),('M2K',0.25,0.353,0.011,'0.614'),
 ('MBT',0.85,1.150,0.011,'2.011'),
 ('GF',0.85,2.100,0.011,'2.961'),('HE',0.85,2.100,0.011,'2.961'),('LE',0.85,2.100,0.011,'2.961'),
 ('YM',0.85,1.380,0.01,'2.240'),('MYM',0.25,0.350,0.01,'0.610'),
 ('ZC',0.85,2.150,0.01,'3.010'),('ZL',0.85,2.150,0.01,'3.010'),('ZM',0.85,2.150,0.01,'3.010'),
 ('ZR',0.85,2.150,0.01,'3.010'),('ZS',0.85,2.150,0.01,'3.010'),('ZW',0.85,2.150,0.01,'3.010'),
 ('KE',0.85,2.150,0.01,'3.010'),
 ('ZT',0.85,0.650,0.01,'1.510'),('ZF',0.85,0.650,0.01,'1.510'),('ZN',0.85,0.800,0.01,'1.660'),
 ('UB',0.85,0.950,0.01,'1.810'),
 ('CL',0.85,1.500,0.01,'2.360'),('HO',0.85,1.500,0.01,'2.360'),('RB',0.85,1.500,0.01,'2.360'),
 ('NG',0.85,1.600,0.01,'2.460'),('PL',0.85,1.650,0.01,'2.510'),
 ('GC',0.85,1.650,0.01,'2.510'),('HG',0.85,1.650,0.01,'2.510'),('SI',0.85,1.650,0.01,'2.510')"

rows()      { $PSQL -tAc "SELECT count(*) FROM metadata.contract_metadata"; }
at_fee()    { $PSQL -tAc "SELECT count(*) FROM metadata.contract_metadata c JOIN ($FEES) e(sym, com, exch, reg, fee) ON e.sym = c.\"Databento Symbol\" WHERE c.\"Fee Per Contract\" = e.fee"; }
at_old()    { $PSQL -tAc "SELECT count(*) FROM metadata.contract_metadata WHERE \"Fee Per Contract\" = '1.50'"; }
sums()      { $PSQL -tAc "SELECT count(*) FROM ($FEES) e(sym, com, exch, reg, fee) WHERE (com + exch + reg)::numeric = fee::numeric"; }
has_check() { $PSQL -tAc "SELECT count(*) FROM pg_constraint WHERE conrelid = 'metadata.contract_metadata'::regclass AND conname = 'contract_metadata_fee_per_contract_positive' AND contype = 'c'"; }
# A fee value offered to one row inside a transaction that is rolled back: 'stored' or 'refused'.
probe() {
    if $PSQL -c "BEGIN" \
             -c "UPDATE metadata.contract_metadata SET \"Fee Per Contract\" = '$1' WHERE \"Databento Symbol\" = 'ZN'" \
             -c "ROLLBACK" > /dev/null 2>&1; then echo stored; else echo refused; fi
}
check_refusals() {
    local label v
    for v in "" "0" "0.0" "-1.50" "abc" " 1.50" "1.50 " "1,50" "1e1" "NaN" "."; do
        label="'$v'"
        [ "$(probe "$v")" = "refused" ] && ok "the CHECK refuses the fee cell $label" || bad "the fee cell $label was stored"
    done
    [ "$(probe "1.66")" = "stored" ] && ok "the CHECK accepts another positive decimal (1.66)" || bad "a positive decimal was refused"
}

if [[ "${VERIFY_ONLY:-}" == "1" ]]; then
    [[ "${PGDATABASE:-}" != "" ]] || { echo "REFUSING to run: set PGDATABASE." >&2; exit 2; }
    echo "########## VERIFY ONLY: $($PSQL -tAc "SELECT current_database()") ##########"
    [ "$(rows)" = "40" ] && ok "the table holds 40 rows" || bad "rows: $(rows)"
    [ "$(at_fee)" = "40" ] && ok "each of the 40 rows holds its IBKR fee" || bad "rows at their IBKR fee: $(at_fee) of 40 (at 1.50: $(at_old))"
    [ "$(sums)" = "40" ] && ok "each fee = commission + exchange + regulatory" || bad "sums: $(sums)"
    [ "$(has_check)" = "1" ] && ok "the fee CHECK exists" || bad "the fee CHECK is missing"
    check_refusals
    [ "$(at_fee)" = "40" ] && ok "the probes left the table as it was" || bad "a probe changed a cell"
    echo ""
    echo "passed=$pass failed=$fail"
    [ "$fail" -eq 0 ]
    exit $?
fi

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

# Every row as text, ordered: the whole content. masked: the same without the fee cell.
full()   { $PSQL -tAc "SELECT md5(coalesce(string_agg(to_jsonb(t)::text, ';' ORDER BY t.\"Databento Symbol\", to_jsonb(t)::text), 'EMPTY')) FROM metadata.contract_metadata t"; }
masked() { $PSQL -tAc "SELECT md5(coalesce(string_agg((to_jsonb(t) - 'Fee Per Contract')::text, ';' ORDER BY t.\"Databento Symbol\", to_jsonb(t)::text), 'EMPTY')) FROM metadata.contract_metadata t"; }
# Columns, indexes, tables and row count; the constraints apart.
shape() {
    $PSQL -tAc "SELECT string_agg(table_name||'.'||ordinal_position||':'||column_name||':'||data_type||':'||is_nullable||':'||coalesce(column_default,''), ',' ORDER BY table_name, ordinal_position)
                  FROM information_schema.columns WHERE table_schema='metadata'"
    $PSQL -tAc "SELECT coalesce(string_agg(indexdef, '|' ORDER BY indexdef),'') FROM pg_indexes WHERE schemaname='metadata'"
    $PSQL -tAc "SELECT string_agg(schemaname||'.'||tablename, ',' ORDER BY 1) FROM pg_tables WHERE schemaname NOT IN ('pg_catalog','information_schema')"
    $PSQL -tAc "SELECT count(*) FROM metadata.contract_metadata"
}
constraints() { $PSQL -tAc "SELECT coalesce(string_agg(conname, ',' ORDER BY conname),'none') FROM pg_constraint WHERE connamespace = 'metadata'::regnamespace"; }
comment() { $PSQL -tAc "SELECT col_description('metadata.contract_metadata'::regclass, 23)"; }

# --- fixture: the table as the stage-3 scratch has it, 40 rows, every fee at 014's default ---
load_fixture() {
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
COMMENT ON COLUMN metadata.contract_metadata."Fee Per Contract" IS
    'Dollars per contract per side that the transaction cost model charges for a fill of this '
    'contract (explicit fee: broker commission plus exchange and clearing, all-in). Every row '
    'starts at the configured default fee per contract, 1.50, the reference until per-row fees '
    'land (HD ruling 33, 2026-09-26). Read by the instrument registry by name; a database without '
    'this column prices every fill at the code default 1.50 (migration 014).';
INSERT INTO metadata.contract_metadata
 ("Databento Symbol","IB Symbol","Name","Exchange","Intraday Initial Margin","Intraday Maintenance Margin",
  "Overnight Initial Margin","Overnight Maintenance Margin","Asset Type","Sector","Contract Size","Units",
  "Minimum Price Fluctuation","Tick Size","Settlement Type","Trading Hours (EST)","Data Provider","Dataset",
  "Newest Month Additions","Contract Months","Time of Expiry","Additional Notes")
SELECT s.sym, s.ib, s.sym || ' futures', s.exch, '2000', '1800', '2200', '2000', 'Futures', s.sector, s.size,
       'USD', '10', '0.01', 'Deliverable', '18:00-17:00', 'Databento', 'GLBX.MDP3', '', 'Mar Jun Sep Dec',
       '09:16', NULL
  FROM (VALUES
    ('6A','6A','CME','FX','100000'),      ('6B','GBP','CME','FX','62500'),
    ('6C','CAD','CME','FX','100000'),     ('6E','EUR','CME','FX','125000'),
    ('6J','JPY','CME','FX','12500000'),   ('6L','6L','CME','FX','100000'),
    ('6M','MXP','CME','FX','500000'),     ('6N','NZD','CME','FX','100000'),
    ('6S','CHF','CME','FX','125000'),     ('CL','CL','NYMEX','Energy','1000'),
    ('ES','ES','CME','Equities','50'),    ('GC','GC','COMEX','Metals','100'),
    ('GF','GF','CME','Agriculture','500'),('HE','HE','CME','Agriculture','400'),
    ('HG','HG','COMEX','Metals','25000'), ('HO','HO','NYMEX','Energy','42000'),
    ('KE','KE','CBOT','Agriculture','50'),('LE','LE','CME','Agriculture','400'),
    ('M2K','M2K','CME','Equities','5'),   ('MBT','MBT','CME','Crypto','0.1'),
    ('MES','MES','CME','Equities','5'),   ('MNQ','MNQ','CME','Equities','2'),
    ('MYM','MYM','CME','Equities','0.5'), ('NG','NG','NYMEX','Energy','10000'),
    ('NQ','NQ','CME','Equities','20'),    ('PL','PL','NYMEX','Metals','50'),
    ('RB','RB','NYMEX','Energy','42000'), ('RTY','RTY','CME','Equities','50'),
    ('SI','SI','COMEX','Metals','5000'),  ('UB','UB','CBOT','Interest Rates','1000'),
    ('YM','YM','CME','Equities','5'),     ('ZC','ZC','CBOT','Agriculture','50'),
    ('ZF','ZF','CBOT','Interest Rates','1000'), ('ZL','ZL','CBOT','Agriculture','600'),
    ('ZM','ZM','CBOT','Agriculture','100'),     ('ZN','ZN','CBOT','Interest Rates','1000'),
    ('ZR','ZR','CBOT','Agriculture','2000'),    ('ZS','ZS','CBOT','Agriculture','50'),
    ('ZT','ZT','CBOT','Interest Rates','2000'), ('ZW','ZW','CBOT','Agriculture','50')
  ) s(sym, ib, exch, sector, size);
SQL
}

load_fixture
echo "########## BEFORE ##########"
fx_full=$(full); fx_masked=$(masked); fx_shape="$(shape)"; fx_comment="$(comment)"
[ "$(rows)" = "40" ] && [ "$(at_old)" = "40" ] && ok "the 40 fees hold 1.50, the value 026 replaces" || bad "fixture: $(rows) rows, $(at_old) at 1.50"
[ "$(at_fee)" = "0" ] && ok "no row holds its IBKR fee yet" || bad "rows already at their fee: $(at_fee)"
[ "$(constraints)" = "none" ] && ok "no constraint on the table" || bad "constraints: $(constraints)"
[ "$(probe "")" = "stored" ] && ok "before 026 an empty fee cell can be stored" || bad "an empty fee cell was already refused"

echo ""
echo "########## APPLY ##########"
if $PSQL -f "$UP" > /dev/null 2>&1; then ok "migration applied"; else bad "migration failed"; exit 1; fi
[ "$(at_fee)" = "40" ] && ok "each of the 40 rows holds its IBKR fee" || bad "rows at their fee: $(at_fee)"
[ "$(sums)" = "40" ] && ok "each fee = commission + exchange + regulatory" || bad "sums: $(sums)"
[ "$(masked)" = "$fx_masked" ] && ok "every other cell byte-identical" || bad "another cell moved"
[ "$(full)" != "$fx_full" ] && ok "the table's content did change (the mask is not hiding everything)" || bad "nothing changed"
[ "$(shape)" = "$fx_shape" ] && ok "columns, indexes, tables and the row count unchanged (no new table, no micro or mini row)" || bad "shape changed"
[ "$(constraints)" = "contract_metadata_fee_per_contract_positive" ] && ok "the one new constraint is the fee CHECK" || bad "constraints: $(constraints)"
[ "$(comment)" != "$fx_comment" ] && comment | grep -q "migration 026" && ok "the column comment names 026" || bad "comment: $(comment)"
check_refusals
up_full=$(full)
[ "$(at_fee)" = "40" ] && ok "the probes left the table as it was" || bad "a probe changed a cell"
if $PSQL -f "$UP" > /dev/null 2>&1 && [ "$(full)" = "$up_full" ] && [ "$(has_check)" = "1" ]; then ok "a second apply is a no-op"; else bad "a second apply failed or moved a cell"; fi

echo ""
echo "########## VALUE GUARD ##########"
$PSQL -c "UPDATE metadata.contract_metadata SET \"Fee Per Contract\"='1.75' WHERE \"Databento Symbol\"='GC'"
$PSQL -c "UPDATE metadata.contract_metadata SET \"Fee Per Contract\"='1.50' WHERE \"Databento Symbol\"='ZN'"
g_full=$(full)
if $PSQL -f "$UP" > "$OUT" 2>&1; then bad "026 applied over a foreign fee"
else grep -q "GC holds '1.75'" "$OUT" && ok "026 refuses a cell that is neither 1.50 nor its fee, naming GC" || bad "026 refused without naming GC"; fi
[ "$(full)" = "$g_full" ] && ok "the refusal changed nothing (the ZN cell it could have priced is still 1.50)" || bad "a refused apply moved a cell"
if $PSQL -f "$DOWN" > /dev/null 2>&1; then bad "the rollback ran over a foreign fee"
else ok "the rollback refuses on the same foreign value"; fi
[ "$(full)" = "$g_full" ] && [ "$(has_check)" = "1" ] && ok "the refused rollback changed nothing and kept the CHECK" || bad "a refused rollback moved a cell or dropped the CHECK"
$PSQL -c "UPDATE metadata.contract_metadata SET \"Fee Per Contract\"='2.510' WHERE \"Databento Symbol\"='GC'"
if $PSQL -f "$UP" > /dev/null 2>&1 && [ "$(full)" = "$up_full" ]; then ok "with the foreign value gone, 026 completes the partly priced table"
else bad "026 did not complete a partly priced state"; fi
$PSQL -c "CREATE TABLE public.t026_keep AS SELECT * FROM metadata.contract_metadata WHERE \"Databento Symbol\"='NG'" \
      -c "DELETE FROM metadata.contract_metadata WHERE \"Databento Symbol\"='NG'" > /dev/null
m_full=$(full)
if $PSQL -f "$UP" > "$OUT" 2>&1; then bad "026 applied with a row missing"
else grep -q "NG" "$OUT" && ok "026 refuses when one of its rows is missing, naming NG" || bad "026 refused without naming NG"; fi
[ "$(full)" = "$m_full" ] && ok "that refusal changed nothing" || bad "a refused apply moved a cell"
$PSQL -c "INSERT INTO metadata.contract_metadata SELECT * FROM public.t026_keep" -c "DROP TABLE public.t026_keep" > /dev/null
[ "$(full)" = "$up_full" ] && ok "fixture row put back" || bad "fixture row not restored"
$PSQL -c "INSERT INTO metadata.contract_metadata SELECT (jsonb_populate_record(NULL::metadata.contract_metadata, to_jsonb(t) || '{\"Databento Symbol\": \"MZN\"}')).* FROM metadata.contract_metadata t WHERE t.\"Databento Symbol\"='ZN'" > /dev/null
x_full=$(full)
if $PSQL -f "$UP" > "$OUT" 2>&1; then bad "026 applied with a row its list does not name"
else grep -q "MZN" "$OUT" && ok "026 refuses a row its list does not name (MZN)" || bad "026 refused without naming MZN"; fi
if $PSQL -f "$DOWN" > /dev/null 2>&1; then bad "the rollback ran with a row its list does not name"
else ok "the rollback refuses the same extra row"; fi
[ "$(full)" = "$x_full" ] && ok "those refusals changed nothing" || bad "a refused run moved a cell"
$PSQL -c "DELETE FROM metadata.contract_metadata WHERE \"Databento Symbol\"='MZN'" > /dev/null
[ "$(full)" = "$up_full" ] && ok "extra row removed" || bad "extra row not removed"

echo ""
echo "########## ROLLBACK ##########"
if $PSQL -f "$DOWN" > /dev/null 2>&1; then ok "rollback applied"; else bad "rollback failed"; fi
[ "$(at_old)" = "40" ] && ok "the 40 earlier values restored" || bad "rows at 1.50 after rollback: $(at_old)"
[ "$(full)" = "$fx_full" ] && ok "the table byte-identical to before 026" || bad "the table differs from the fixture"
[ "$(shape)" = "$fx_shape" ] && [ "$(constraints)" = "none" ] && ok "shape unchanged and the CHECK dropped" || bad "shape or constraints: $(constraints)"
[ "$(comment)" = "$fx_comment" ] && ok "the column comment restored" || bad "comment not restored"
[ "$(probe "")" = "stored" ] && ok "after the rollback an empty fee cell can be stored again" || bad "an empty cell is still refused"
if $PSQL -f "$DOWN" > /dev/null 2>&1 && [ "$(full)" = "$fx_full" ]; then ok "a second rollback is a no-op"; else bad "a second rollback failed or moved a cell"; fi
if $PSQL -f "$UP" > /dev/null 2>&1 && [ "$(full)" = "$up_full" ] && [ "$(has_check)" = "1" ]; then ok "026 applies again after its rollback"; else bad "re-apply after rollback failed"; fi

echo ""
echo "########## WITHOUT 014 ##########"
load_fixture
$PSQL -c "ALTER TABLE metadata.contract_metadata DROP COLUMN \"Fee Per Contract\"" > /dev/null
n_shape="$(shape)"
if $PSQL -f "$UP" > "$OUT" 2>&1; then bad "026 applied without 014's column"
else grep -q "014" "$OUT" && ok "026 refuses without 014's column, naming 014" || bad "026 refused without naming 014"; fi
[ "$(shape)" = "$n_shape" ] && [ "$(constraints)" = "none" ] && ok "that refusal changed nothing" || bad "a refused apply changed the table"

$PSQL -c "DROP SCHEMA metadata CASCADE" > /dev/null
echo ""
echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
