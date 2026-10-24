#!/usr/bin/env bash
#
# Create a scratch copy of the regime input data (issue #103, section 12).
#
#   ./scripts/make_regime_scratch_db.sh <source-dsn> <scratch-db-name>
#
# e.g.
#   ./scripts/make_regime_scratch_db.sh \
#       "postgresql://postgres@172.31.23.NNN:5432/new_algo_data" regime_scratch_20261010
#
# What it does:
#   1. Dumps ONLY the two schemas the regime pipelines read: futures_data and
#      macro_data. pg_dump is read-only, so the source is never modified.
#   2. Creates the scratch database on the same server.
#   3. Restores the dump into it.
#   4. Sets TimeZone='UTC' and DateStyle='ISO, MDY' on the scratch database, as
#      section 12 requires.
#   5. Prints the Q1 and Q4 row counts, which section 9 says to record with
#      every result, plus the copy date.
#
# Why a copy is needed even though the regime runners never write:
# the gate must reproduce byte for byte, and production data moves underneath
# you — EODHD rewrites adjusted_close on every upsert. A frozen snapshot is what
# makes the gate reproducible. The copy is for determinism, not only for safety.
#
# The password belongs in ~/.pgpass (chmod 600), not in the DSN and not in a
# shell history.
#
set -euo pipefail

SRC_DSN="${1:?usage: $0 <source-dsn> <scratch-db-name>}"
SCRATCH_DB="${2:?usage: $0 <source-dsn> <scratch-db-name>}"

PSQL=$(command -v psql || echo /opt/homebrew/opt/libpq/bin/psql)
PG_DUMP=$(command -v pg_dump || echo /opt/homebrew/opt/libpq/bin/pg_dump)

# Derive a server-only DSN (no database) so we can create the scratch database.
SERVER_DSN="${SRC_DSN%/*}"
DUMP_FILE="${TMPDIR:-/tmp}/regime_scratch_$(date +%Y%m%d_%H%M%S).dump"

echo "==> source      : ${SRC_DSN%%\?*}"
echo "==> scratch db  : $SCRATCH_DB"
echo "==> dump file   : $DUMP_FILE"
echo

echo "==> checking the source is reachable and read-only work is possible"
"$PSQL" "$SRC_DSN" -Atc "select current_database(), current_user, version();" \
  || { echo "error: cannot reach the source database" >&2; exit 1; }

echo
echo "==> dumping futures_data and macro_data (read-only on the source)"
"$PG_DUMP" "$SRC_DSN" \
  --schema=futures_data --schema=macro_data \
  --format=custom --no-owner --no-privileges \
  --file="$DUMP_FILE"
echo "    dump size: $(du -h "$DUMP_FILE" | cut -f1)"

echo
echo "==> creating $SCRATCH_DB"
if "$PSQL" "$SERVER_DSN/postgres" -Atc \
     "select 1 from pg_database where datname = '$SCRATCH_DB';" | grep -q 1; then
  echo "error: database $SCRATCH_DB already exists — pick a new name, or drop it" >&2
  echo "       (refusing to overwrite: a scratch copy is one clone per piece of work)" >&2
  exit 1
fi
"$PSQL" "$SERVER_DSN/postgres" -c "CREATE DATABASE \"$SCRATCH_DB\";"

echo
echo "==> restoring"
pg_restore --dbname="$SERVER_DSN/$SCRATCH_DB" --no-owner --no-privileges \
           --jobs=4 "$DUMP_FILE" 2>&1 | tail -5 || true

echo
echo "==> setting session defaults required by section 12"
"$PSQL" "$SERVER_DSN/postgres" -c \
  "ALTER DATABASE \"$SCRATCH_DB\" SET TimeZone = 'UTC';"
"$PSQL" "$SERVER_DSN/postgres" -c \
  "ALTER DATABASE \"$SCRATCH_DB\" SET DateStyle = 'ISO, MDY';"

echo
echo "================= record these with every result ================="
echo "copy date : $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "source    : ${SRC_DSN%%\?*}"
echo "scratch   : $SCRATCH_DB"
echo
echo "-- Q1: macro panel dates"
"$PSQL" "$SERVER_DSN/$SCRATCH_DB" -c "
WITH d AS (
  SELECT date FROM macro_data.inflation
  UNION SELECT date FROM macro_data.growth
  UNION SELECT date FROM macro_data.yield_curve
  UNION SELECT date FROM macro_data.credit_spreads
  UNION SELECT date FROM macro_data.liquidity
  UNION SELECT date FROM macro_data.market
)
SELECT COUNT(*) AS dates, MIN(date) AS first_date, MAX(date) AS last_date FROM d;"

echo "-- Q4: futures bars per regime sleeve symbol"
"$PSQL" "$SERVER_DSN/$SCRATCH_DB" -c "
SELECT symbol, COUNT(*) AS bars,
       MIN(time::date) AS first_date, MAX(time::date) AS last_date
FROM futures_data.ohlcv_1d
WHERE symbol IN ('MES.v.0','MNQ.v.0','MYM.v.0','ZN.v.0','ZF.v.0',
                 '6E.v.0','6J.v.0','6B.v.0','6A.v.0',
                 'CL.v.0','NG.v.0','GC.v.0','HG.v.0')
GROUP BY symbol ORDER BY symbol;"

echo
echo "scratch DSN for the runners:"
echo "  $SERVER_DSN/$SCRATCH_DB"
echo
echo "Run the timeline gate with:"
echo "  TZ=UTC TIMELINE_CSV=/tmp/market_timeline.csv \\"
echo "    ./bin/Release/market_regime_pipeline_runner"
echo "  diff /tmp/market_timeline.csv \\"
echo "    src/regime_detection/baselines/market_timeline_K05plus.csv | wc -l"
