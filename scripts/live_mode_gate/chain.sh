#!/bin/zsh
# scripts/live_mode_gate/chain.sh: the chain driver of the live mode gate (see gate.sh for what the gate proves).
# It is the audited stage-3 live harness (T-8a tooling/live.sh) with two additions: MODE and the host date.
#
#   chain.sh <database> <ARM> <unit> [label]
#     MODE=M3   every run is a PAST-DATE run (the real host clock), as every test run is.
#     MODE=M2   every run date is presented as the HOST'S OWN DATE (12:00 local) through hostclock_shim, on the
#               unpatched binary: this is how production runs.
#     PF        conservative | base | equity_mr
#     ARM       ABSENT (portfolio.json as the config source has it) | TPL (plus the template's listing blocks)
#     unit      chain         S0 restored, the dates RANGE_START .. RANGE_END; a non-zero exit does not stop it
#               prodrestart   MODE=M2 only: the chain as production runs it (a refusal, then the missed-run guard on
#                             every later date), snapshot, then THE RESTART: the missed dates run in order, oldest
#                             first, on the host day after RANGE_END (each is then a past-date run)
#               latebars      the bars of LATE_DATE are moved aside for the two runs after it, put back, and those
#                             dates and every later one are re-run in date order (the re-run repair). The two runs
#                             made without the bars keep their own log and body, named <date>_without_late_bars
#               refused       S0 restored, ONE run dated REFUSED_DATE that must be refused; snapshots before and after
# Every run carries a date. --send-email is never passed. Only 127.0.0.1, only a database whose name holds "scratch".
# One runner at a time under GATE_RUN_LOCK; the run config is written under the lock and the runner's own database
# line is asserted. Nothing is mailed.
#
# Settings (environment; the defaults are the stage-3 layout of the machine the gate was written on):
#   GATE_WORKTREE   the built tree (build/bin/Release holds the runners)          required; a linked worktree,
#                   NEVER the main checkout: the driver writes config/ there, deletes logs/ and moves
#                   apps/strategies/results out of it
#   GATE_OUT        where runs are kept                                             required
#   GATE_S0_DIR     the directory holding S0_conservative, S0_base, S0_equity     required
#   GATE_CONFIG_SRC defaults.json.entry and portfolios/<book>/{portfolio,risk,email}.json   required
#   GATE_RUN_LOCK   the shared run lock file                                        required
#   GATE_PG_ENV     a file that sets SCRATCH_PGPASSWORD (never printed)             required
set -u
[ -z "${SD_CAFF:-}" ] && exec env SD_CAFF=1 /usr/bin/caffeinate -i /bin/zsh "$0" "$@"
HERE="${0:A:h}"
EV="${GATE_OUT:?set GATE_OUT}"
T="$HERE"
S0DIR="${GATE_S0_DIR:?set GATE_S0_DIR}"; S0="$S0DIR/S0_conservative"
PSQL="${GATE_PSQL:-/opt/homebrew/opt/postgresql@16/bin/psql}"
RL="${GATE_RUN_LOCK:?set GATE_RUN_LOCK}"
DB="$1"; ARM="$2"; UNIT="$3"; LABEL="${4:-}"; SIDE=gate
MODE="${MODE:?set MODE=M3 or M2}"; case "$MODE" in M3|M2) ;; *) echo "REFUSING mode $MODE"; exit 2;; esac
case "$DB" in *scratch*) ;; *) echo "REFUSING database $DB"; exit 2;; esac
case "$ARM" in ABSENT|RL|LD|BOTH|TPL) ;; *) echo "REFUSING arm $ARM"; exit 2;; esac
W="${GATE_WORKTREE:?set GATE_WORKTREE to the built linked worktree the gate may write its run config into}"; SHIM="$EV/hostclock_shim.dylib"
[ "$(cd "$W" && git rev-parse --absolute-git-dir 2>/dev/null)" != "$(cd "$W" && cd "$(git rev-parse --git-common-dir 2>/dev/null)" 2>/dev/null && pwd -P)" ] || { echo "REFUSING worktree $W: it is the main checkout (or not a git worktree); the gate rewrites config/ and deletes logs/ in its worktree"; exit 2 }
[ -x "$W/build/bin/Release/live_portfolio_conservative" ] || { echo "REFUSING worktree $W: no built runner under build/bin/Release"; exit 2 }
SRC="${GATE_CONFIG_SRC:?set GATE_CONFIG_SRC}"
PF="${PF:-conservative}"; case "$PF" in conservative) BIN=live_portfolio_conservative; PID=CONSERVATIVE_PORTFOLIO;; base) BIN=live_portfolio; PID=BASE_PORTFOLIO; S0="$S0DIR/S0_base";; equity_mr) BIN=live_equity_mr; PID=EQUITY_MR_PORTFOLIO; S0="$S0DIR/S0_equity";; *) echo "REFUSING portfolio $PF"; exit 2;; esac
PFT=""; [ "$PF" = base ] && PFT="base_"; [ "$PF" = equity_mr ] && PFT="equity_"
OUT="$EV/runs/live_${PFT}${SIDE}_${DB#new_algo_data_scratch_stage3_}_${ARM}_${UNIT}_${MODE}${LABEL:+_$LABEL}"
[ -e "$OUT/DONE" ] && { echo "REFUSED: $OUT/DONE exists"; exit 2 }
mkdir -p "$OUT/logs" "$EV/proofs"
set -a; . "${GATE_PG_ENV:?set GATE_PG_ENV}" > /dev/null 2>&1; set +a
export PGPASSWORD="$SCRATCH_PGPASSWORD"
q() { $PSQL -h 127.0.0.1 -p 5433 -U postgres -d "$DB" -At -v ON_ERROR_STOP=1 -c "$1" }
dfline() { echo "$(date -u +%FT%TZ) $1: $(df -h / | tail -1) | $(pmset -g batt | head -1)" >> "$EV/proofs/df_log.txt"; [ "$(df -m / | tail -1 | awk '{print $4}')" -ge 2560 ] || { echo "STOP: under 2.5 GB free before $1"; return 6 } }
lock_run() { until ( setopt noclobber; print -r -- "live_mode_gate/$MODE/$ARM/$1 $(date -u +%FT%TZ) pid=$$" > "$RL" ) 2>/dev/null; do sleep 15; done; echo "$(date -u +%FT%TZ) took for live $SIDE $ARM $1" >> "$EV/proofs/run_lock_log.txt" }
unlock_run() { local cur=$(cat "$RL" 2>/dev/null); if [[ "$cur" == "live_mode_gate/"*" pid=$$" ]]; then /bin/rm -f "$RL"; echo "$(date -u +%FT%TZ) released" >> "$EV/proofs/run_lock_log.txt"; fi }
setcfg() {
  mkdir -p "$W/config/portfolios/$PF"
  cp "$SRC/portfolios/$PF/risk.json" "$SRC/portfolios/$PF/email.json" "$W/config/portfolios/$PF/"
  python3 - "$SRC/defaults.json.entry" "$W/config/defaults.json" "$DB" "$SRC/portfolios/$PF/portfolio.json" "$W/config/portfolios/$PF/portfolio.json" "$ARM" <<'PY'
import json, sys
entry, out, dbn, pentry, pout, arm = sys.argv[1:7]
d = json.load(open(entry)); db = d["database"]
assert db["host"] == "127.0.0.1" and db["port"] == "5433" and isinstance(db["port"], str)
db["name"] = dbn
json.dump(d, open(out, "w"), indent=2)
ptxt = open(pentry).read(); pj = json.loads(ptxt)
assert "listing_dates" not in pj and "instrument_id_relabels" not in pj
note = "portfolio.json = entry copy"
if arm == "TPL":
    import os
    tpl = json.load(open(os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(pout)))), "config_template", "portfolios", os.path.basename(os.path.dirname(pout)), "portfolio.json")))
    for k in ("listing_dates", "instrument_id_relabels", "trading_rule_removals"):
        assert k in tpl, k
        pj[k] = tpl[k]; note += " + %s (the head's own template): %s" % (k, json.dumps(tpl[k]))
    open(pout, "w").write(json.dumps(pj, indent=2) + "\n")
elif arm == "ABSENT":
    open(pout, "w").write(ptxt)
else:
    if arm in ("RL", "BOTH"):
        pj["instrument_id_relabels"] = [{"symbol": "MES", "date": "2026-02-22", "from": "42140878", "to": "42003800"},
                                        {"symbol": "MNQ", "date": "2026-02-22", "from": "42002475", "to": "42004946"},
                                        {"symbol": "MYM", "date": "2026-02-22", "from": "42005850", "to": "42001953"},
                                        {"symbol": "M2K", "date": "2026-02-22", "from": "42005017", "to": "42002147"}]
        note += " + instrument_id_relabels (the four entries)"
    if arm in ("LD", "BOTH"):
        pj["listing_dates"] = {"contracts": [{"symbol": m, "listed": "2019-05-06", "before": b, "ratio": 10}
                                             for m, b in (("MES", "ES"), ("MNQ", "NQ"), ("MYM", "YM"), ("M2K", "RTY"))]}
        note += " + listing_dates (the four pairs, no switch_rule key)"
    open(pout, "w").write(json.dumps(pj, indent=2) + "\n")
c = json.load(open(out))
print("config: database name=%s port=%r live=%s; %s" % (c["database"]["name"], c["database"]["port"], c.get("live"), note))
PY
}
snap_live() {   # <out dir>
  mkdir -p "$1"
  for t in ${(f)"$(q "select table_name from information_schema.columns where table_schema='trading' and column_name='portfolio_id' order by 1")"}; do
    cols=$(q "select string_agg(quote_ident(column_name), ',' order by ordinal_position) from information_schema.columns where table_schema='trading' and table_name='$t'")
    $PSQL -h 127.0.0.1 -p 5433 -U postgres -d "$DB" -q -c "\\copy (select * from trading.$t where portfolio_id='$PID' order by $cols) to '$1/trading_$t.csv' with (format csv, header)"
    echo "trading.$t: $(( $(wc -l < "$1/trading_$t.csv") - 1 )) rows"
  done
}
restore_s0() {   # <label>: the portfolio's trading rows replaced by T-2's S0 snapshot, then proven
  local L="$1"
  {
  for F in "$S0"/trading_*.csv; do
    t=${${F:t:r}#trading_}
    [ "$t" = strategy_trading_days_metadata ] && continue
    del=$(q "WITH d AS (DELETE FROM trading.$t WHERE portfolio_id='$PID' RETURNING 1) SELECT count(*) FROM d")
    COLS=$(head -1 "$F" | tr -d '\r')
    iddef=$(q "select coalesce(column_default,'') from information_schema.columns where table_schema='trading' and table_name='$t' and column_name='id'")
    if [ -n "$iddef" ]; then INS=$(python3 -c "import sys;print(','.join(c for c in sys.argv[1].split(',') if c!='id'))" "$COLS"); else INS="$COLS"; fi
    $PSQL -h 127.0.0.1 -p 5433 -U postgres -d "$DB" -q -v ON_ERROR_STOP=1 <<SQL
CREATE TEMP TABLE _r (LIKE trading.$t INCLUDING DEFAULTS);
\copy _r ($COLS) FROM '$F' WITH (FORMAT csv, HEADER)
INSERT INTO trading.$t ($INS) SELECT $INS FROM _r;
SQL
    if [[ ",$COLS," != *",netting_adjustment,"* ]] && [ "$(q "select count(*) from information_schema.columns where table_schema='trading' and table_name='$t' and column_name='netting_adjustment'")" = 1 ]; then
      q "UPDATE trading.$t SET netting_adjustment=NULL WHERE portfolio_id='$PID'" >/dev/null
    fi
    echo "trading.$t: deleted $del, restored $(q "SELECT count(*) FROM trading.$t WHERE portfolio_id='$PID'")"
  done
  # T-8a commit (12): after migration 035 (settled_at) a restored row reads settled_at NULL and the sizing read would see
  # no settled history. The restored book is put in the state the migration leaves it: the migration's backfill
  # statement, kept verbatim in the gate's own settled_at_backfill.sql, scoped to this portfolio by the session setting
  # the statement reads. s0_project.py proves the result row by row.
  if [ "$(q "select count(*) from information_schema.columns where table_schema='trading' and table_name='live_results' and column_name='settled_at'")" = 1 ]; then
    BF="$W/scripts/live_mode_gate/settled_at_backfill.sql"
    STMT=$(grep -v '^--' "$BF")
    [[ "$STMT" == UPDATE* ]] || { echo "the settled_at backfill statement was not found in $BF"; false }
    $PSQL -h 127.0.0.1 -p 5433 -U postgres -d "$DB" -q -v ON_ERROR_STOP=1 -c "BEGIN; SET LOCAL migration.settled_at_backfill = 'portfolio:$PID'; $STMT COMMIT;"
    echo "trading.live_results settled_at after the backfill statement (sha256 of $BF: $(shasum -a 256 "$BF" | cut -c1-64)): $(q "select count(*) filter (where settled_at is not null)||' stamped, '||count(*) filter (where settled_at is null)||' NULL' from trading.live_results where portfolio_id='$PID'")"
  fi
  A="$S0/trading_strategy_trading_days_metadata.csv"
  if [ -f "$A" ]; then
    python3 - "$A" > "$OUT/.anchor_$L.sql" <<'PY'
import csv, sys
for r in csv.DictReader(open(sys.argv[1])):
    print("UPDATE trading.strategy_trading_days_metadata SET live_start_date='%s' WHERE strategy_id='%s' AND portfolio_id='%s';" % (r['live_start_date'], r['strategy_id'], r['portfolio_id']))
PY
    $PSQL -h 127.0.0.1 -p 5433 -U postgres -d "$DB" -q -v ON_ERROR_STOP=1 -f "$OUT/.anchor_$L.sql"; echo "anchors restored: $(wc -l < "$OUT/.anchor_$L.sql" | tr -d ' ')"; rm -f "$OUT/.anchor_$L.sql"
  fi
  } > "$OUT/${L}_restore.txt" 2>&1 || { echo "RESTORE FAILED ($L)"; tail -5 "$OUT/${L}_restore.txt"; return 5 }
  rm -rf "$OUT/.s0check_$L" "$OUT/.s0proj_$L"; snap_live "$OUT/.s0check_$L" >/dev/null
  python3 "$T/s0_project.py" "$OUT/.s0check_$L" "$OUT/.s0proj_$L" "$S0" > "$OUT/${L}_restore_proof.txt" || { echo "RESTORE NOT PROVEN ($L): a migrated column is off its history value"; return 5 }
  python3 "$T/compare_csv_dirs.py" --tol 0 "$S0" "$OUT/.s0proj_$L" >> "$OUT/${L}_restore_proof.txt" || { echo "RESTORE NOT PROVEN ($L)"; tail -5 "$OUT/${L}_restore_proof.txt"; return 5 }
  echo "restore $L proven: $(tail -1 "$OUT/${L}_restore_proof.txt")"; rm -rf "$OUT/.s0check_$L" "$OUT/.s0proj_$L"
}
run_day() {   # <date> [host date] [log suffix]; prints the exit code, never fails the script
  local d="$1" hd="${2:-$1}" sfx="${3:-}" rc t0=$(date +%s)
  [[ "$d" =~ '^[0-9]{4}-[0-9]{2}-[0-9]{2}$' ]] || { echo "REFUSED: a live run needs a date"; return 2 }
  dfline "before live $SIDE $ARM $UNIT $d" || return 6
  mkdir -p "$OUT/series/$d"
  lock_run "$UNIT-$d"; setcfg > "$OUT/config.txt"   # the config is written under the lock, for every run
  local lg="$d"; [ "$hd" != "$d" ] && lg="${d}_on_host_${hd}"; lg="$lg$sfx"
  ( cd "$W" && if [ "$MODE" = M2 ]; then export TDATED_HOST_CLOCK_OFFSET=$(python3 -c "import datetime as t,time,sys;y,m,dd=map(int,sys.argv[1].split('-'));print(int(time.mktime(t.datetime(y,m,dd,12,0).timetuple())-time.time()))" "$hd"); export DYLD_INSERT_LIBRARIES="$SHIM"; echo "MODE M2: host clock offset $TDATED_HOST_CLOCK_OFFSET s (run date $d; the host's date presented as $hd 12:00 local)"; else echo "MODE M3: real host clock $(date +%FT%T%z)"; fi; TRADE_NGIN_SERIES_DUMP_DIR="$OUT/series/$d" "$W/build/bin/Release/$BIN" "$d" ) > "$OUT/logs/$lg.log" 2>&1 && rc=0 || rc=$?; unlock_run
  /bin/rm -rf "$W/logs/$d"; for f in "$W"/logs/*.log(N); do /bin/rm -f "$f"; done
  grep -a "Config summary: db=" "$OUT/logs/$lg.log" | head -1 | grep -q "/$DB," || { echo "STOP: the run dated $d did not run on $DB" | tee -a "$OUT/progress.txt"; rc=9 }
  grep -av "password" "$OUT/logs/$lg.log" | gzip -nc > "$OUT/logs/$lg.log.gz"; /bin/rm -f "$OUT/logs/$lg.log"
  for f in "$OUT/series/$d"/*.csv(N); do gzip -nf "$f"; done
  echo "$UNIT run-date $d host-date $hd rc=$rc $(( $(date +%s) - t0 ))s email-send lines: $(gzip -dc "$OUT/logs/$lg.log.gz" | grep -acE 'Sending email|Email sent|email sending enabled')" | tee -a "$OUT/progress.txt"
  return $rc
}
days() { python3 -c "import datetime as t,sys;s=t.date.fromisoformat(sys.argv[1]);e=t.date.fromisoformat(sys.argv[2]);print(*[(s+t.timedelta(i)).isoformat() for i in range((e-s).days+1)])" "$1" "$2" }
chain() {   # <start> <end> [log suffix]: goes on after a refusal, stops on a tooling stop
  for d in $(python3 -c "import datetime as t;s=t.date.fromisoformat('$1');e=t.date.fromisoformat('$2');print(*[(s+t.timedelta(i)).isoformat() for i in range((e-s).days+1)])"); do
    local rc=0; run_day "$d" "$d" "${3:-}" || rc=$?
    if [ $rc -eq 9 ] || [ $rc -eq 6 ] || [ $rc -eq 2 ]; then return 1; fi
    if [ $rc -ne 0 ]; then echo "CONTINUING after exit $rc on $d (as cron would)" | tee -a "$OUT/progress.txt"; snap_live "$OUT/snap_after_exit${rc}_$d" > /dev/null; fi
  done
}
trap 'unlock_run; exit 130' INT TERM
(
  set -e
  line=$(q "select current_database()||' port='||inet_server_port()||' tz='||current_setting('TimeZone')")
  [ "$line" = "$DB port=5433 tz=UTC" ] || { echo "REFUSED: database is '$line'"; exit 3 }
  setcfg > "$OUT/config.txt"
  ( cd "$W/build/bin/Release" && shasum -a 256 libtrade_ngin.dylib $BIN ) > "$OUT/binaries.txt"
  echo "worktree HEAD: $(git -C "$W" rev-parse HEAD) dirty files: $(git -C "$W" status --porcelain --untracked-files=no | wc -l | tr -d ' ') database: $line arm: $ARM unit: $UNIT" > "$OUT/head.txt"
  case "$UNIT" in
    chain)
      restore_s0 ch
      chain "${RANGE_START:?}" "${RANGE_END:?}"
      snap_live "$OUT/snap" > "$OUT/snap.txt" ;;
    prodrestart)
      [ "$MODE" = M2 ] || { echo "REFUSING prodrestart without MODE=M2"; exit 2 }
      restore_s0 pr
      missed=()
      for d in $(days "${RANGE_START:?}" "${RANGE_END:?}"); do rc=0; run_day "$d" "$d" || rc=$?; [ $rc -ne 0 ] && missed+=("$d"); done
      echo "production mode left these dates not run: ${missed:-none}" | tee -a "$OUT/progress.txt"
      snap_live "$OUT/snap_before_restart" > /dev/null
      H=$(python3 -c "import datetime as t,sys;print((t.date.fromisoformat(sys.argv[1])+t.timedelta(1)).isoformat())" "$RANGE_END")
      echo "--- the restart, on host day $H: the missed dates in order, oldest first" >> "$OUT/progress.txt"
      for d in $missed; do rc=0; run_day "$d" "$H" || rc=$?; [ $rc -ne 0 ] && { echo "RESTART FAILED on $d (exit $rc)" | tee -a "$OUT/progress.txt"; exit 1 }; done
      snap_live "$OUT/snap" > "$OUT/snap.txt" ;;
    refused)
      restore_s0 rf
      snap_live "$OUT/snap_before" > /dev/null
      rc=0; run_day "${REFUSED_DATE:?}" || rc=$?
      echo "exit code of the run dated $REFUSED_DATE: $rc" | tee "$OUT/exit_code.txt"
      snap_live "$OUT/snap" > "$OUT/snap.txt" ;;
    latebars)
      LATE="${LATE_DATE:?}"; L1=$(python3 -c "import datetime as t,sys;print((t.date.fromisoformat(sys.argv[1])+t.timedelta(1)).isoformat())" "$LATE"); L2=$(python3 -c "import datetime as t,sys;print((t.date.fromisoformat(sys.argv[1])+t.timedelta(2)).isoformat())" "$LATE")
      restore_s0 lb
      chain "${RANGE_START:?}" "$LATE"
      { q "CREATE SCHEMA IF NOT EXISTS tdated_late; DROP TABLE IF EXISTS tdated_late.b, tdated_late.r"
        q "CREATE TABLE tdated_late.b AS SELECT * FROM futures_data.ohlcv_1d WHERE time::date='$LATE'"
        q "CREATE TABLE tdated_late.r AS SELECT * FROM futures_data.ohlcv_1d_raw WHERE ts_event::date='$LATE'"
        echo "moved aside: ohlcv_1d $(q "select count(*) from tdated_late.b") rows, ohlcv_1d_raw $(q "select count(*) from tdated_late.r") rows"
        q "DELETE FROM futures_data.ohlcv_1d WHERE time::date='$LATE'"; q "DELETE FROM futures_data.ohlcv_1d_raw WHERE ts_event::date='$LATE'"
        echo "left dated $LATE: $(q "select count(*) from futures_data.ohlcv_1d where time::date='$LATE'") / $(q "select count(*) from futures_data.ohlcv_1d_raw where ts_event::date='$LATE'")"; } > "$OUT/bars_proof.txt" 2>&1
      chain "$L1" "$L2" _without_late_bars
      snap_live "$OUT/snap_late_before_repair" > /dev/null
      { q "INSERT INTO futures_data.ohlcv_1d SELECT * FROM tdated_late.b"; q "INSERT INTO futures_data.ohlcv_1d_raw SELECT * FROM tdated_late.r"
        echo "put back: now dated $LATE: $(q "select count(*) from futures_data.ohlcv_1d where time::date='$LATE'") / $(q "select count(*) from futures_data.ohlcv_1d_raw where ts_event::date='$LATE'")"
        q "DROP SCHEMA tdated_late CASCADE"; } >> "$OUT/bars_proof.txt" 2>&1
      echo "--- re-run of ONE date alone ($L1)" >> "$OUT/progress.txt"
      chain "$L1" "$L1"
      snap_live "$OUT/snap_after_rerun_of_one_date" > /dev/null
      echo "--- re-run forward in date order" >> "$OUT/progress.txt"
      chain "$L2" "${RANGE_END:?}"
      snap_live "$OUT/snap" > "$OUT/snap.txt" ;;
    *) echo "REFUSING unit $UNIT"; exit 2 ;;
  esac
  if [ -d "$W/apps/strategies/results" ]; then /bin/rm -rf "$OUT/csv_exports"; mv "$W/apps/strategies/results" "$OUT/csv_exports"; fi
)
rc=$?
unlock_run
[ $rc -eq 0 ] && print -r -- "live $SIDE $ARM $UNIT db=$DB head=$(git -C "$W" rev-parse --short=8 HEAD) finished=$(date -u +%FT%TZ)" > "$OUT/DONE"
echo "live.sh $SIDE $ARM $UNIT rc=$rc"
exit $rc
