#!/bin/zsh
# fill_on_production.sh: fills the missing <ROOT>.v.0 rows of futures_data.ohlcv_1d and futures_data.ohlcv_1d_raw from the
# same database's futures_data.contract_ohlcv_1d. Called by 04_fill_holes.sh.
#
#   fill_on_production.sh production                         new_algo_data (config/defaults.json)
#   fill_on_production.sh scratch <database with scratch in its name>   PG* from the environment
#   FILL_YES=1           skip the typed confirmation (tests)
#   FILL_INCLUDE_NEW=1   also insert holes dated after the tested range (the dry run lists them either way)
#
# Order:
#   0. preflight: refuses if this target's DONE marker exists (exit 4) or a STARTED marker of an unfinished run does
#      (exit 5); checks the tested ledger's sha256; read-only: each table's kind and keys, TimescaleDB, UTC, no permanent
#      fill_* relation (exit 7).
#   1. dry run, read-only on every permanent table (temp tables only, proven by pg_stat_xact_user_tables, then
#      ROLLBACK): the rule's checks; counts per class, root and branch; the ledger with junk verdicts; its diff against
#      the tested ledger, which must be identical within the tested range (exit 6). 0 holes: nothing to do, exit 0.
#   2. confirmation, then ONE transaction: both continuous tables locked against writers, the contract table against
#      writers; the holes recomputed under the lock and refused unless they equal the dry run's row for row (exit 3);
#      the rule's checks again; an md5 baseline of every pre-existing row; insert only; then every row checked against
#      its contract bar and the expected raw id, the baseline and the counts, all BEFORE COMMIT.
#   3. after COMMIT, informational only: the counts and a second dry run; then the DONE marker.
# Exit codes: 0 done or nothing to do; 1 failure (rolled back unless the log says COMMITTED); 2 usage or a changed tested
# ledger; 3 the data moved since the dry run (rolled back, nothing written); 4 already filled; 5 an unfinished earlier
# run, or not confirmed; 6 the dry run differs from the tested ledger; 7 preflight refused.
set -u
setopt pipefail
DF="${0:A:h}"; SQL="$DF/sql"; PY="$DF/scripts"
REPO="${DF:h:h:h}"
RUN_DIR="${RUN_DIR:-$REPO/futures_release_run}"; HERE="$RUN_DIR/fill"; mkdir -p "$HERE"
TESTED_LEDGER="$DF/LEDGER_fill_base.csv"
TESTED_SHA=1cd33bd63c1034561555e596cb18ef1fd283ec8d19887c9fefdc0a89ea640ef5
TESTED_END=2026-10-23   # the ledger's rows were tested on a copy of production holding every root's bars to this date
MODE="${1:-}"
case "$MODE" in
  production)
    cfg="$REPO/config/defaults.json"
    eval "$(python3 - "$cfg" <<'PY'
import json, shlex, sys
d = json.load(open(sys.argv[1]))["database"]
for k, v in (("PGHOST", d["host"]), ("PGPORT", str(d.get("port", "5432"))), ("PGUSER", d.get("username", d.get("user"))),
             ("PGDATABASE", d["name"]), ("PGPASSWORD", d["password"])):
    print("export %s=%s" % (k, shlex.quote(v)))
PY
)" || { print -r -- "no usable database block in $cfg"; exit 2 }
    [ "$PGDATABASE" = new_algo_data ] || { print -r -- "config/defaults.json names $PGDATABASE, not new_algo_data"; exit 2 }
    TARGET=production ;;
  scratch)
    db="${2:-}"; [[ "$db" == *scratch* ]] || { print -r -- "REFUSED: scratch needs a database with scratch in its name"; exit 2 }
    export PGDATABASE="$db"
    TARGET="scratch_$db" ;;
  *) print -r -- "usage: fill_on_production.sh production | scratch <scratch database>"; exit 2 ;;
esac
export PGTZ=UTC
INCLUDE_NEW=false; [ "${FILL_INCLUDE_NEW:-}" = 1 ] && INCLUDE_NEW=true
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
LOG="$HERE/fill_${TARGET}_${STAMP}.log"; LEDGER="$HERE/fill_${TARGET}_${STAMP}_ledger.csv"
DONE="$HERE/fill_${TARGET}.DONE"; STARTED="$HERE/fill_${TARGET}.STARTED"
W=$(mktemp -d "$HERE/.work_${STAMP}_XXXX")
CLEAN=0
# The work folder is deleted only after a clean finish; on any failure it is kept and the log names it.
trap 'if [ $CLEAN = 1 ]; then /bin/rm -rf "$W"; else print -r -- "work folder kept: $W" | tee -a "$LOG"; fi' EXIT
say() { print -r -- "$*" | tee -a "$LOG" }
die() { local code=${2:-1}; say "STOP (exit $code): $1"; exit $code }
q() { psql -X -v ON_ERROR_STOP=1 -q "$@" }

# 0. preflight
say "== fill of the continuous futures rows, target $TARGET, $(date -u +%FT%TZ)"
say "script sha256 $(shasum -a 256 "$0" | cut -c1-16); core sql sha256 $(shasum -a 256 "$SQL/fill_core.sql" | cut -c1-16); include holes after $TESTED_END: $INCLUDE_NEW"
[ -e "$DONE" ] && die "this target was filled already ($(cat "$DONE")); refusing to run twice" 4
[ -e "$STARTED" ] && die "an earlier run started the insert and did not finish clean ($(cat "$STARTED")). Read its log: if it says COMMITTED the rows are in, if not it rolled back. Then move the STARTED file aside and run again" 5
[ "$(shasum -a 256 "$TESTED_LEDGER" | cut -d' ' -f1)" = "$TESTED_SHA" ] || die "the tested ledger $TESTED_LEDGER is not the tested file (sha256 differs)" 2
PGOPTIONS='-c default_transaction_read_only=on' q -f "$SQL/fill_preflight.sql" 2>&1 | tee -a "$LOG"; [ ${pipestatus[1]} -eq 0 ] || die "preflight refused" 7

# 1. dry run: temp tables only, rolled back
say "== 1. dry run"
{ print -r -- "begin;"; cat "$SQL/fill_core.sql" "$SQL/fill_checks.sql" "$SQL/fill_counts.sql" "$SQL/fill_dryrun.sql"
  print -r -- "\\copy (select symbol, d, ts, root, instrument_id, raw_symbol, branch, klass, open, high, low, close, volume, rtype, publisher_id, raw_instrument_id from pg_temp.fill_fill order by symbol, d) to '$W/fill_rows.csv' csv header"
  cat "$SQL/fill_dryrun_guard.sql"; } > "$W/dryrun.sql"
q -f "$W/dryrun.sql" 2>&1 | grep -v NOTICE | tee -a "$LOG"; [ ${pipestatus[1]} -eq 0 ] || die "the dry run failed (a rule check, or the read-only guard): nothing written"
[ -s "$W/fill_rows.csv" ] || die "the dry run wrote no row list"
N=$(( $(wc -l < "$W/fill_rows.csv") - 1 ))
say "holes found: $N"
if [ "$N" -eq 0 ]; then say "nothing to fill: 0 holes. No write made."; CLEAN=1; exit 0; fi

# the junk verdict of each row (the session classifier's volume limbs, scripts/junk_replica.py), read-only
PGOPTIONS='-c default_transaction_read_only=on' q -c "\\copy (select symbol, ((time at time zone 'UTC')::date)::text as date, volume, high, low from futures_data.ohlcv_1d where symbol like '%.v.0' order by 1, 2) to '$W/series.csv' csv header" || die "series read failed"
python3 -c "import csv,sys; r=list(csv.DictReader(open(sys.argv[1]))); w=csv.writer(open(sys.argv[2],'w',newline='')); w.writerow(['symbol','d','volume','high','low']); [w.writerow([x['symbol'],x['d'],x['volume'],x['high'],x['low']]) for x in r]" "$W/fill_rows.csv" "$W/holes_min.csv"
python3 "$PY/junk_replica.py" "$W/series.csv" "$W/holes_min.csv" "$W/junk.csv" | tee -a "$LOG" || die "junk replica failed"
python3 "$PY/make_ledger.py" "$W/fill_rows.csv" "$W/junk.csv" "$LEDGER" | tee -a "$LOG" || die "ledger failed"
say "== the ledger (one line per filled row per table; also in $LEDGER)"
cat "$LEDGER" >> "$LOG"
say "== the dry run's ledger against the tested ledger"
python3 "$PY/ledger_diff.py" "$LEDGER" "$TESTED_LEDGER" "$TESTED_END" "$W/ledger_diff.txt" | tee -a "$LOG"
[ ${pipestatus[1]} -eq 0 ] || die "the dry run differs from the tested ledger within the tested range (to $TESTED_END): refused, nothing written" 6
# the rows to insert: the tested range, plus the newer holes with FILL_INCLUDE_NEW=1
python3 -c "
import csv, sys
r = list(csv.DictReader(open(sys.argv[1]))); w = csv.DictWriter(open(sys.argv[2], 'w', newline=''), fieldnames=list(r[0].keys())); w.writeheader()
[w.writerow(x) for x in r if x['d'] <= sys.argv[3] or sys.argv[4] == 'true']" "$W/fill_rows.csv" "$W/expected.csv" "$TESTED_END" "$INCLUDE_NEW"
NE=$(( $(wc -l < "$W/expected.csv") - 1 ))
say "rows to insert into each table: $NE (holes after $TESTED_END not included: $(( N - NE )))"
if [ "$NE" -eq 0 ]; then say "nothing to insert. No write made."; CLEAN=1; exit 0; fi

# 2. confirmation and the insert
if [ "${FILL_YES:-}" != 1 ]; then
  print -n "Insert $NE rows into futures_data.ohlcv_1d and $NE into futures_data.ohlcv_1d_raw on $TARGET? Type $NE to go: "
  read ans; [ "$ans" = "$NE" ] || die "not confirmed; nothing written" 5
fi
say "== 2. insert (one transaction)"
print -r -- "started $STAMP, log $(basename "$LOG")" > "$STARTED"
{ cat "$SQL/fill_apply_begin.sql" "$SQL/fill_core.sql"
  print -r -- "create temp table fill_expected (symbol text, d date, ts timestamptz, root text, instrument_id text, raw_symbol text, branch text, klass text, open float8, high float8, low float8, close float8, volume bigint, rtype text, publisher_id text, raw_instrument_id text);"
  print -r -- "\\copy pg_temp.fill_expected from '$W/expected.csv' csv header"
  cat "$SQL/fill_checks.sql" "$SQL/fill_apply.sql"; } > "$W/apply.sql"
q -v tested_end="$TESTED_END" -v include_new="$INCLUDE_NEW" -f "$W/apply.sql" 2>&1 | grep -v NOTICE | tee "$W/apply.out" | tee -a "$LOG"
rc=${pipestatus[1]}
if grep -q "== COMMITTED" "$W/apply.out"; then
  :
elif [ "$rc" -eq 3 ] || grep -qi "error:" "$W/apply.out"; then
  # psql exits 3 when a statement or meta-command of the script fails (ON_ERROR_STOP): the open transaction is rolled
  # back as the session ends, so nothing was written
  /bin/rm -f "$STARTED"
  grep -q "the rows to insert differ from the dry run" "$W/apply.out" && die "the data moved between the dry run and the insert: rolled back, nothing written; run again for a fresh dry run" 3
  die "the insert failed a check and was rolled back: nothing written"
else
  die "the insert ended (psql rc $rc) without COMMITTED or an ERROR line: the state is unknown; the STARTED marker stays"
fi

# 3. after COMMIT: informational only (every check ran before COMMIT)
say "== 3. after COMMIT"
q -f "$SQL/fill_counts.sql" 2>&1 | tee -a "$LOG"
{ print -r -- "begin;"; cat "$SQL/fill_core.sql" "$SQL/fill_dryrun.sql" "$SQL/fill_dryrun_guard.sql"; } > "$W/rerun.sql"
q -f "$W/rerun.sql" 2>&1 | grep -v NOTICE > "$W/rerun.txt"
again=$(grep -A2 "^ holes" "$W/rerun.txt" | tail -1 | tr -d ' ')
say "second dry run: $again holes left (holes after $TESTED_END not inserted: $(( N - NE )))"
print -r -- "filled $NE rows per table at $STAMP (log $(basename "$LOG"))" > "$DONE"
/bin/rm -f "$STARTED"
CLEAN=1
say "== DONE: $NE rows into each table; log $LOG; ledger $LEDGER"
