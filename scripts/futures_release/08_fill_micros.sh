#!/bin/zsh
# 08_fill_micros.sh: fills the missing MGC.v.0 and SIL.v.0 rows of futures_data.ohlcv_1d and ohlcv_1d_raw from the same
# database's futures_data.contract_ohlcv_1d, after part B (07) has stored the two series. The hole fill (04) ran before
# the micros existed, so their 8 holes are filled here.
#
#   zsh 08_fill_micros.sh production                          new_algo_data (config/defaults.json); asks for the count:
#                                                             8, and more only if the list shows holes after 2026-10-23
#   zsh 08_fill_micros.sh scratch <scratch database>          a test copy (PG* from the environment)
#   FILL_YES=1           skip the typed confirmation (test copies only)
#
# It is fill/fill_on_production.sh with four changes only:
#   1. the tested ledger is fill/LEDGER_fill_micros.csv: 16 lines, 8 rows per table; within the tested range (to
#      2026-10-23) the dry run must find exactly those 8 holes and no other (any other hole refuses with exit 6, which
#      is also what happens if the hole fill (04) has not run);
#   2. it refuses (exit 7) unless MGC.v.0 and SIL.v.0 are stored in both continuous tables (part B ran);
#   3. its markers, logs and ledgers are its own, in $RUN_DIR/fill_micros/;
#   4. the count at the confirmation is typed, or piped (print 8 | zsh 08_fill_micros.sh ...), as the operator's check.
# Like the hole fill, it takes every hole found, those dated after the tested range too, listed first and by the same rule.
# The SQL (fill/sql/) and tools (fill/scripts/) are used unchanged.
# CHANGES: inserts the 8 rows, and any hole after the tested range, into each of the two continuous tables, one transaction, every check before COMMIT, no
#   existing row changed (the fill's md5 baseline). READS: the three futures tables. TAKES: a few minutes.
# UNDO: delete the (symbol, date) pairs of the run's ledger (fill_micros/fill_micros_<target>_<stamp>_ledger.csv) from
#   both tables.
# Exit codes as the fill's: 0 done or nothing to do; 1 failure (rolled back unless the log says COMMITTED); 2 usage or a
# changed tested ledger; 3 the data moved since the dry run; 4 already filled; 5 unfinished earlier run or not
# confirmed; 6 the dry run differs from the tested ledger; 7 preflight refused.
set -u
setopt pipefail
DF="${0:A:h}/fill"; SQL="$DF/sql"; PY="$DF/scripts"
REPO="${DF:h:h:h}"
RUN_DIR="${RUN_DIR:-$REPO/futures_release_run}"; HERE="$RUN_DIR/fill_micros"; mkdir -p "$HERE"
TESTED_LEDGER="$DF/LEDGER_fill_micros.csv"
TESTED_SHA=66c6e3167945a1d09daa5eaf4be7c03e87ba88f715a9c91ea464b92d45e6ebaf
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
    . "${DF:h}/scratch_guard.zsh"; scratch_guard
    export PGDATABASE="$db"
    TARGET="scratch_$db" ;;
  *) print -r -- "usage: 08_fill_micros.sh production | scratch <scratch database>"; exit 2 ;;
esac
export PGTZ=UTC
INCLUDE_NEW=true
[ "$MODE" = production ] && [ -n "${FILL_YES:-}" ] && [ "${SEED_CONFIRM:-}" != "$PGDATABASE" ] && { print -r -- "REFUSED: FILL_YES on production needs SEED_CONFIRM=new_algo_data"; exit 2 }
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
LOG="$HERE/fill_micros_${TARGET}_${STAMP}.log"; LEDGER="$HERE/fill_micros_${TARGET}_${STAMP}_ledger.csv"
DONE="$HERE/fill_micros_${TARGET}.DONE"; STARTED="$HERE/fill_micros_${TARGET}.STARTED"
W=$(mktemp -d "$HERE/.work_${STAMP}_XXXX")
CLEAN=0
# The work folder is deleted only after a clean finish; on any failure it is kept and the log names it.
trap 'sed -i "" -E "s/[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,3}/<host>/g" "$LOG" 2>/dev/null; if [ $CLEAN = 1 ]; then /bin/rm -rf "$W"; else print -r -- "work folder kept: $W" | tee -a "$LOG"; fi' EXIT
say() { print -r -- "$*" | tee -a "$LOG" }
die() { local code=${2:-1}; say "STOP (exit $code): $1"; exit $code }
q() { psql -X -v ON_ERROR_STOP=1 -q "$@" }

# 0. preflight
say "== fill of the MGC.v.0 and SIL.v.0 continuous rows, target $TARGET, $(date -u +%FT%TZ)"
say "script sha256 $(shasum -a 256 "$0" | cut -c1-16); core sql sha256 $(shasum -a 256 "$SQL/fill_core.sql" | cut -c1-16); include holes after $TESTED_END: $INCLUDE_NEW"
[ -e "$DONE" ] && die "this target was filled already ($(cat "$DONE")); refusing to run twice" 4
[ -e "$STARTED" ] && die "an earlier run started the insert and did not finish clean ($(cat "$STARTED")). Read its log: if it says COMMITTED the rows are in, if not it rolled back. Then move the STARTED file aside and run again" 5
[ "$(shasum -a 256 "$TESTED_LEDGER" | cut -d' ' -f1)" = "$TESTED_SHA" ] || die "the tested ledger $TESTED_LEDGER is not the tested file (sha256 differs)" 2
PGOPTIONS='-c default_transaction_read_only=on' q -f "$SQL/fill_preflight.sql" 2>&1 | tee -a "$LOG"; [ ${pipestatus[1]} -eq 0 ] || die "preflight refused" 7

n_micro=$(PGOPTIONS='-c default_transaction_read_only=on' psql -X -At -v ON_ERROR_STOP=1 -c "select (select count(distinct symbol) from futures_data.ohlcv_1d where symbol in ('MGC.v.0','SIL.v.0'))||' '||(select count(distinct symbol) from futures_data.ohlcv_1d_raw where symbol in ('MGC.v.0','SIL.v.0'))") || die "the micros check failed" 7
[ "$n_micro" = "2 2" ] || die "MGC.v.0 and SIL.v.0 are not both stored in both continuous tables ($n_micro): run part B (07) first" 7
say "MGC.v.0 and SIL.v.0 stored in both continuous tables"

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

# the junk verdict of each row (the session classifier's volume limbs, fill/scripts/junk_replica.py), read-only
PGOPTIONS='-c default_transaction_read_only=on' q -c "\\copy (select symbol, ((time at time zone 'UTC')::date)::text as date, volume, high, low from futures_data.ohlcv_1d where symbol like '%.v.0' order by 1, 2) to '$W/series.csv' csv header" || die "series read failed"
python3 -c "import csv,sys; r=list(csv.DictReader(open(sys.argv[1]))); w=csv.writer(open(sys.argv[2],'w',newline='')); w.writerow(['symbol','d','volume','high','low']); [w.writerow([x['symbol'],x['d'],x['volume'],x['high'],x['low']]) for x in r]" "$W/fill_rows.csv" "$W/holes_min.csv"
python3 "$PY/junk_replica.py" "$W/series.csv" "$W/holes_min.csv" "$W/junk.csv" | tee -a "$LOG" || die "junk replica failed"
python3 "$PY/make_ledger.py" "$W/fill_rows.csv" "$W/junk.csv" "$LEDGER" | tee -a "$LOG" || die "ledger failed"
say "== the ledger (one line per filled row per table; also in $LEDGER)"
cat "$LEDGER" >> "$LOG"
say "== the dry run's ledger against the tested ledger"
python3 "$PY/ledger_diff.py" "$LEDGER" "$TESTED_LEDGER" "$TESTED_END" "$W/ledger_diff.txt" | tee -a "$LOG"
[ ${pipestatus[1]} -eq 0 ] || die "the dry run differs from the tested ledger within the tested range (to $TESTED_END): refused, nothing written" 6
# the rows to insert: every hole, the tested range and the newer ones
python3 -c "
import csv, sys
r = list(csv.DictReader(open(sys.argv[1]))); w = csv.DictWriter(open(sys.argv[2], 'w', newline=''), fieldnames=list(r[0].keys())); w.writeheader()
[w.writerow(x) for x in r if x['d'] <= sys.argv[3] or sys.argv[4] == 'true']" "$W/fill_rows.csv" "$W/expected.csv" "$TESTED_END" "$INCLUDE_NEW"
NE=$(( $(wc -l < "$W/expected.csv") - 1 ))
NEW=$(python3 -c "import csv,sys; print(sum(1 for x in csv.DictReader(open(sys.argv[1])) if x['d'] > sys.argv[2]))" "$W/expected.csv" "$TESTED_END")
say "rows to insert into each table: $NE (of them dated after $TESTED_END, with no tested ledger behind them: $NEW)"
if [ "$NE" -eq 0 ]; then say "nothing to insert. No write made."; CLEAN=1; exit 0; fi

# 2. confirmation and the insert
if [ "${FILL_YES:-}" != 1 ]; then
  print -n "Insert $NE rows into futures_data.ohlcv_1d and $NE into futures_data.ohlcv_1d_raw on $TARGET? Type $NE to go: "
  read ans; [ "$ans" = "$NE" ] || die "not confirmed: the count given is '$ans' and the holes are $NE, $NEW of them dated after $TESTED_END (listed above); nothing written" 5
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
say "second dry run: $again holes left"
print -r -- "filled $NE rows per table at $STAMP (log $(basename "$LOG"))" > "$DONE"
/bin/rm -f "$STARTED"
CLEAN=1
say "== DONE: $NE rows into each table; log $LOG; ledger $LEDGER"
