#!/usr/bin/env bash
# Verifies migrations/014_contract_metadata_fee_per_contract.sql and its rollback against a real
# PostgreSQL, using the shape of metadata.contract_metadata as introspected from the stage-3
# scratch (a copy of production): 22 text columns, NOT NULL except "Additional Notes".
#
# The properties under test:
#   1. the column the loader reads after CM1 is genuinely ABSENT before the migration (a SELECT
#      naming it fails: the migration is necessary, not decorative);
#   2. after it: text, NOT NULL, default '1.50'; every existing row reads '1.50'; a later INSERT
#      that omits it reads '1.50'; an explicit fee round-trips; a NULL fee is rejected;
#   3. NOTHING ELSE MOVES: every pre-existing row is byte-identical apart from the new key, every
#      older column keeps its ordinal position (the loader reads them by position) and the new
#      column is the last one; every index and constraint is unchanged;
#   4. idempotent (a second apply is a no-op) and type-guarded (an existing column of another
#      type makes it refuse, changing nothing);
#   5. the column comment is set;
#   6. the rollback refuses while a fee other than 1.50 exists and destroys nothing when it
#      refuses; with the session override it drops the column and the original rows are
#      byte-identical to before the migration; with every fee at 1.50 it needs no override.
#
# Requires a running postgres reachable via PGHOST/PGPORT/PGUSER/PGPASSWORD.
#
# DESTRUCTIVE: the fixture begins with DROP SCHEMA metadata CASCADE on whatever those env vars
# point at. Point it at a THROWAWAY database created for the purpose, for example
#
#   createdb t014_migration_test
#   PGDATABASE=t014_migration_test MIGRATION_TEST_DB=t014_migration_test ./test_014_contract_metadata_fee_per_contract.sh
#   dropdb t014_migration_test
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
UP="$HERE/014_contract_metadata_fee_per_contract.sql"
DOWN="$HERE/014_contract_metadata_fee_per_contract_rollback.sql"
pass=0
fail=0
ok()  { echo "  PASS  $1"; pass=$((pass + 1)); }
bad() { echo "  FAIL  $1"; fail=$((fail + 1)); }

# Hash of every row WITHOUT the new key, ordered by its own text: proves no existing value moved.
fingerprint() {
    $PSQL -tAc "SELECT coalesce(md5(string_agg(x, ';' ORDER BY x)), 'EMPTY')
                  FROM (SELECT (to_jsonb(t) - 'Fee Per Contract')::text x
                          FROM metadata.contract_metadata t) s"
}
coltype() { $PSQL -tAc "SELECT coalesce(max(data_type||'|'||is_nullable||'|'||coalesce(column_default,'')),'ABSENT')
                          FROM information_schema.columns
                         WHERE table_schema='metadata' AND table_name='contract_metadata'
                           AND column_name='Fee Per Contract'"; }
# Every column but the new one, with its ordinal position (the loader's indices).
positions() { $PSQL -tAc "SELECT string_agg(ordinal_position||':'||column_name, ',' ORDER BY ordinal_position)
                            FROM information_schema.columns
                           WHERE table_schema='metadata' AND table_name='contract_metadata'
                             AND column_name <> 'Fee Per Contract'"; }
objects() {
    $PSQL -tAc "SELECT i.relname||'|'||pg_get_indexdef(i.oid)||'|'||x.indisvalid AS o
                  FROM pg_index x JOIN pg_class c ON c.oid=x.indrelid
                  JOIN pg_class i ON i.oid=x.indexrelid
                  JOIN pg_namespace n ON n.oid=c.relnamespace
                 WHERE n.nspname='metadata' AND c.relname='contract_metadata'
                 ORDER BY o"
    $PSQL -tAc "SELECT conname||'|'||contype::text||'|'||pg_get_constraintdef(oid) AS o
                  FROM pg_constraint
                 WHERE conrelid = 'metadata.contract_metadata'::regclass
                 ORDER BY o"
}
fee_of() { $PSQL -tAc "SELECT \"Fee Per Contract\" FROM metadata.contract_metadata
                        WHERE \"Databento Symbol\" = '$1'"; }

# --- fixture: the table as the stage-3 scratch has it, three of its rows -----------------------
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
    "Additional Notes"             TEXT
);
INSERT INTO metadata.contract_metadata VALUES
 ('ZT','ZT','2-Year T-Note','CBOT','1100','1000','1100','1000','Futures','Rates','2000','USD',
  '7.8125','0.00390625','Deliverable','18:00-17:00','Databento','GLBX.MDP3','','HMUZ','12:01',NULL),
 ('6E','6E','Euro FX','CME','2500','2500','2500','2500','Futures','FX','125000','EUR',
  '6.25','0.00005','Deliverable','18:00-17:00','Databento','GLBX.MDP3','','HMUZ','09:16',NULL),
 ('MES','MES','Micro E-mini S&P 500','CME','1500','1500','1500','1500','Futures','Equity','5',
  'USD','1.25','0.25','Cash','18:00-17:00','Databento','GLBX.MDP3','','HMUZ','09:30','micro');
SQL

echo "########## BEFORE ##########"
before_objects="$(objects)"
before_positions="$(positions)"
before_fp=$(fingerprint)

[ "$(coltype)" = "ABSENT" ] && ok "contract_metadata has no \"Fee Per Contract\" before 014" \
                            || bad "contract_metadata already has \"Fee Per Contract\""
if $PSQL -c 'SELECT "Fee Per Contract" FROM metadata.contract_metadata' > /dev/null 2>&1; then
    bad "a SELECT naming \"Fee Per Contract\" succeeded BEFORE the migration"
else
    ok "a SELECT naming \"Fee Per Contract\" fails before the migration -- 014 is necessary"
fi

echo ""
echo "########## APPLY ##########"
if $PSQL -f "$UP" > /dev/null 2>&1; then ok "migration applied"; else bad "migration failed"; exit 1; fi
if $PSQL -f "$UP" > /dev/null 2>&1; then ok "migration is idempotent (a second apply is a no-op)"
else bad "a second apply failed"; fi

echo ""
echo "########## AFTER ##########"
[ "$(coltype)" = "text|NO|'1.50'::text" ] && ok "\"Fee Per Contract\" is text, NOT NULL, default '1.50'" \
    || bad "column is $(coltype)"
n=$($PSQL -tAc "SELECT count(*) FROM metadata.contract_metadata WHERE \"Fee Per Contract\" = '1.50'")
[ "$n" = "3" ] && ok "every existing row reads 1.50 (3 of 3)" || bad "existing rows at 1.50: $n of 3"
[ "$(fingerprint)" = "$before_fp" ] && ok "every existing value of every other column byte-identical" \
    || bad "existing rows changed"
[ "$(positions)" = "$before_positions" ] && ok "every older column keeps its ordinal position" \
    || bad "an older column moved"
last=$($PSQL -tAc "SELECT column_name FROM information_schema.columns
                    WHERE table_schema='metadata' AND table_name='contract_metadata'
                    ORDER BY ordinal_position DESC LIMIT 1")
[ "$last" = "Fee Per Contract" ] && ok "the new column is the last one" || bad "last column is $last"
if [ "$(objects)" = "$before_objects" ]; then
    ok "every index and constraint unchanged"
else
    bad "an index or constraint changed"; diff <(echo "$before_objects") <(objects) || true
fi

$PSQL -c "INSERT INTO metadata.contract_metadata (\"Databento Symbol\",\"IB Symbol\",\"Name\",
    \"Exchange\",\"Intraday Initial Margin\",\"Intraday Maintenance Margin\",
    \"Overnight Initial Margin\",\"Overnight Maintenance Margin\",\"Asset Type\",\"Sector\",
    \"Contract Size\",\"Units\",\"Minimum Price Fluctuation\",\"Tick Size\",\"Settlement Type\",
    \"Trading Hours (EST)\",\"Data Provider\",\"Dataset\",\"Newest Month Additions\",
    \"Contract Months\",\"Time of Expiry\")
  VALUES ('ZF','ZF','5-Year T-Note','CBOT','1300','1200','1300','1200','Futures','Rates','1000',
    'USD','7.8125','0.0078125','Deliverable','18:00-17:00','Databento','GLBX.MDP3','','HMUZ',
    '12:01')" > /dev/null
[ "$(fee_of ZF)" = "1.50" ] && ok "an INSERT that omits the column reads 1.50" \
                            || bad "an omitting INSERT reads '$(fee_of ZF)'"
$PSQL -c "UPDATE metadata.contract_metadata SET \"Fee Per Contract\" = '0.62'
           WHERE \"Databento Symbol\" = 'MES'" > /dev/null
[ "$(fee_of MES)" = "0.62" ] && ok "an explicit fee round-trips (0.62)" \
                             || bad "explicit fee reads '$(fee_of MES)'"
if $PSQL -c "UPDATE metadata.contract_metadata SET \"Fee Per Contract\" = NULL
              WHERE \"Databento Symbol\" = 'ZT'" > /dev/null 2>&1; then
    bad "a NULL fee was accepted"
else
    ok "a NULL fee is rejected (NOT NULL)"
fi
c=$($PSQL -tAc "SELECT coalesce(col_description('metadata.contract_metadata'::regclass,
                  (SELECT ordinal_position FROM information_schema.columns
                    WHERE table_schema='metadata' AND table_name='contract_metadata'
                      AND column_name='Fee Per Contract')::int), '<none>')")
[[ "$c" == "Dollars per contract per side"*"migration 014"* ]] && ok "the column carries its comment" \
    || bad "comment: $c"

echo ""
echo "########## TYPE GUARD ##########"
$PSQL -c "UPDATE metadata.contract_metadata SET \"Fee Per Contract\" = '1.50'
           WHERE \"Databento Symbol\" = 'MES'" > /dev/null
$PSQL -c "ALTER TABLE metadata.contract_metadata DROP COLUMN \"Fee Per Contract\";
          ALTER TABLE metadata.contract_metadata ADD COLUMN \"Fee Per Contract\" NUMERIC" > /dev/null
guard_fp=$(fingerprint)
if $PSQL -f "$UP" > /dev/null 2>&1; then
    bad "the migration accepted a \"Fee Per Contract\" of the wrong type"
else
    ok "the migration refuses an existing \"Fee Per Contract\" of another type"
fi
[ "$(fingerprint)" = "$guard_fp" ] && [ "$(coltype)" = "numeric|YES|" ] \
    && ok "the refused apply changed nothing" || bad "the refused apply changed the table ($(coltype))"
$PSQL -c "ALTER TABLE metadata.contract_metadata DROP COLUMN \"Fee Per Contract\"" > /dev/null
$PSQL -f "$UP" > /dev/null
[ "$(coltype)" = "text|NO|'1.50'::text" ] && ok "re-applied after the guard test" \
    || bad "re-apply after the guard test: $(coltype)"

echo ""
echo "########## ROLLBACK ##########"
$PSQL -c "UPDATE metadata.contract_metadata SET \"Fee Per Contract\" = '0.62'
           WHERE \"Databento Symbol\" = 'MES'" > /dev/null
if $PSQL -f "$DOWN" > /dev/null 2>&1; then
    bad "rollback should refuse while a fee other than 1.50 exists"
else
    ok "rollback refuses while a fee other than 1.50 exists"
fi
[ "$(fee_of MES)" = "0.62" ] && ok "the refused rollback destroyed nothing" \
                             || bad "the refused rollback lost the value ($(fee_of MES))"
if $PSQL -c "SET migration.allow_fee_drop = 'yes'" -f "$DOWN" > /dev/null 2>&1; then
    ok "rollback completes with the session override"
else
    bad "rollback failed with the override"
fi
[ "$(coltype)" = "ABSENT" ] && ok "the column is gone" || bad "the column survived the rollback"
$PSQL -c "DELETE FROM metadata.contract_metadata WHERE \"Databento Symbol\" = 'ZF'" > /dev/null
[ "$(fingerprint)" = "$before_fp" ] && ok "the original rows survived the round trip byte-identical" \
    || bad "the original rows differ after the round trip"
[ "$(positions)" = "$before_positions" ] && ok "column positions survived the round trip" \
    || bad "column positions differ after the round trip"
if [ "$(objects)" = "$before_objects" ]; then
    ok "every index and constraint survived the round trip"
else
    bad "an index or constraint did not survive the round trip"; diff <(echo "$before_objects") <(objects) || true
fi

# Every fee at the default: the rollback needs no override.
$PSQL -f "$UP" > /dev/null
if $PSQL -f "$DOWN" > /dev/null 2>&1; then ok "rollback needs no override when every fee is 1.50"
else bad "rollback refused with every fee at 1.50"; fi

echo ""
echo "RESULT: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
