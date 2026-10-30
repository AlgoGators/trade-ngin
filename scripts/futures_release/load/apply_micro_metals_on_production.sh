#!/bin/zsh
# apply_micro_metals_on_production.sh: loads micro gold (MGC) and micro silver (SIL) into new_algo_data, one part per run,
# in the order the release's README sets, which cannot be changed. Called by 05, 06 and 07.
#   zsh apply_micro_metals_on_production.sh A|A2|B [production]                 (production is the default)
#   zsh apply_micro_metals_on_production.sh A|A2|B scratch <scratch database>   a test copy (PG* from the environment)
#   ROWS=<folder>      part A and B's rows to 2026-10-26 (required, see PULL_REQUEST.md)
#   BACKFILL=<folder>  part A2's rows (required for A2); for B, the same folder, so its continuous rows go in too
# Every part runs the read-only preflight first (preflight.sql) and refuses (exit 7) a session that is not UTC. Each part
# is one transaction that refuses out of order, refuses any row already stored, checks counts and shapes, and commits
# only if every check passes (exit 1 rolled back). Credentials from config/defaults.json via PG* variables, never
# printed. The log is written to $RUN_DIR/load/.
set -u
setopt pipefail
cd "${0:A:h}"
REPO="${0:A:h:h:h:h}"; RUN_DIR="${RUN_DIR:-$REPO/futures_release_run}"; mkdir -p "$RUN_DIR/load"
REPO="${0:A:h:h:h:h}"; RUN_DIR="${RUN_DIR:-$REPO/futures_release_run}"; mkdir -p "$RUN_DIR/load"
PART="${1:-}"; MODE="${2:-production}"
case "$PART" in A|A2|B) ;; *) print -r -- "usage: zsh ${0:t} A|A2|B [production | scratch <scratch database>]"; exit 2;; esac
case "$MODE" in
  production)
    cfg="$REPO/config/defaults.json"
    [ -s "$cfg" ] || { print -r -- "no config/defaults.json at $cfg"; exit 2 }
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
    db="${3:-}"; [[ "$db" == *scratch* ]] || { print -r -- "REFUSED: scratch needs a database with scratch in its name"; exit 2 }
    export PGDATABASE="$db"
    TARGET="scratch_$db" ;;
  *) print -r -- "usage: zsh ${0:t} A|A2|B [production | scratch <scratch database>]"; exit 2 ;;
esac
export PGTZ=UTC
ROWS="${ROWS:-}"; BACKFILL="${BACKFILL:-}"
log="$RUN_DIR/load/apply_micro_metals_${TARGET}_part${PART}_$(date -u +%Y%m%dT%H%M%SZ).log"
say() { print -r -- "$*" | tee -a "$log" }
say "== micro metals part $PART, target $TARGET, $(date -u +%FT%TZ); script sha256 $(shasum -a 256 "$0" | cut -c1-16), sql sha256 $(shasum -a 256 load_micro_metals.sql | cut -c1-16)"
PGOPTIONS='-c default_transaction_read_only=on' psql -X -v ON_ERROR_STOP=1 -q -f preflight.sql 2>&1 | tee -a "$log"
[ ${pipestatus[1]} -eq 0 ] || { say "STOP (exit 7): preflight refused"; exit 7 }
args=()
case "$PART" in
  A|B) [ -s "$ROWS/contract_ohlcv_1d.csv" ] || { say "no rows folder $ROWS"; exit 2 }
       args=(-v rows="$ROWS")
       say "rows $ROWS: $(cd "$ROWS" && shasum -a 256 *.csv | tr '\n' ' ')"
       if [ "$PART" = B ] && [ -n "$BACKFILL" ]; then
         [ -s "$BACKFILL/ohlcv_1d.csv" ] || { say "no backfill continuous rows in $BACKFILL"; exit 2 }
         args+=(-v backfill="$BACKFILL")
         say "backfill $BACKFILL: $(cd "$BACKFILL" && shasum -a 256 *.csv | tr '\n' ' ')"
       fi ;;
  A2)  [ -n "$BACKFILL" ] && [ -s "$BACKFILL/contract_ohlcv_1d.csv" ] || { say "part A2 needs BACKFILL=<folder> with contract_ohlcv_1d.csv"; exit 2 }
       args=(-v rows="$BACKFILL")
       say "backfill $BACKFILL: $(cd "$BACKFILL" && shasum -a 256 *.csv | tr '\n' ' ')" ;;
esac
psql -X -v ON_ERROR_STOP=1 "${args[@]}" -v part="$PART" -f load_micro_metals.sql 2>&1 | tee -a "$log"
rc=${pipestatus[1]}
[ $rc -eq 0 ] || { say "STOP (exit 1): part $PART refused or failed; its transaction rolled back, nothing written"; exit 1 }
say "== done; log: $log"
