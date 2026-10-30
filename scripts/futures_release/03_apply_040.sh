#!/bin/zsh
# 03_apply_040.sh: applies migration 040 (the roll split) to the target, from the release's own file, unchanged.
#
#   zsh 03_apply_040.sh check production                    read only: is 040 there, and the schema fingerprint
#   zsh 03_apply_040.sh apply production new_algo_data      apply 040 (the third word confirms the database)
#   Rehearsal: SCRATCH_DB=<clone> zsh 03_apply_040.sh apply scratch <clone>
#   DONE on production 2026-10-30; kept for the record and for a fresh copy. The file is this repository's own
#   migrations/040_roll_split.sql.
#
# CHANGES: migrations/040_roll_split.sql as the release holds it, run with psql in one transaction: three NULL-able
#   columns on trading.executions (roll_bar_move, roll_gap_points, roll_gap_value, numeric), the same three on
#   backtest.executions (double precision), two on backtest.results (roll_bar_moves, roll_gap_values), two CHECKs
#   (chk_executions_roll_split, chk_backtest_executions_roll_split) and the eight column comments. Every existing row
#   reads NULL; no value, key or index changes. Nothing else.
# THE FILE: migrations/040_roll_split.sql of this checkout, never a copy. The script refuses unless the file on disk equals
#   HEAD's blob.
# BEFORE AND AFTER: the schema fingerprint (sql/schema_fingerprint.sql: every column and constraint of the engine's
#   schemas). Before: none of the eight columns or two CHECKs present. After: the fingerprint differs from the one
#   before by exactly those ten lines, or the script reports FAIL (the migration's own transaction has committed by
#   then; undo below).
# READS: information_schema, pg_constraint; the login from config/defaults.json (production).
# TAKES: seconds (each CHECK is validated by one scan of its executions table under the ALTER's lock).
# UNDO: migrations/040_roll_split_rollback.sql from the same worktree (it refuses while any cell holds a roll split
#   value, which no row does until a per_contract_rolls run writes one).
set -u
. "${0:A:h}/lib.zsh"
TARGET="${2:-${TARGET:-}}"; CONFIRM="${3:-}"
resolve_target
WT="$REPO"
OUT="$RUN_DIR/migration_040"; mkdir -p "$OUT"; LOG="$OUT/apply_040_${TAG}.log"
MIG="$WT/migrations/040_roll_split.sql"
EXPECTED=(
  "column backtest.executions.roll_bar_move double precision YES"
  "column backtest.executions.roll_gap_points double precision YES"
  "column backtest.executions.roll_gap_value double precision YES"
  "column backtest.results.roll_bar_moves double precision YES"
  "column backtest.results.roll_gap_values double precision YES"
  "column trading.executions.roll_bar_move numeric YES"
  "column trading.executions.roll_gap_points numeric YES"
  "column trading.executions.roll_gap_value numeric YES"
)

present() { qr "select (select count(*) from information_schema.columns where (table_schema, table_name, column_name) in (('trading','executions','roll_bar_move'),('trading','executions','roll_gap_points'),('trading','executions','roll_gap_value'),('backtest','executions','roll_bar_move'),('backtest','executions','roll_gap_points'),('backtest','executions','roll_gap_value'),('backtest','results','roll_bar_moves'),('backtest','results','roll_gap_values')))||' of 8 columns, '||(select count(*) from pg_constraint where conname in ('chk_executions_roll_split','chk_backtest_executions_roll_split'))||' of 2 CHECKs'" }
fingerprint() { PGOPTIONS='-c default_transaction_read_only=on' $PGBIN/psql -X -At -v ON_ERROR_STOP=1 -f "$PROD/sql/schema_fingerprint.sql" > "$1" || die "schema fingerprint failed"; }

check_file() {
  [ -s "$MIG" ] || die "no $MIG (is the worktree at the release?)"
  local blob disk
  blob=$(git -C "$WT" rev-parse "HEAD:migrations/040_roll_split.sql") || die "040 is not in the worktree's commit"
  disk=$(git -C "$WT" hash-object "$MIG")
  [ "$disk" = "$blob" ] || die "040_roll_split.sql differs: commit $blob, disk $disk"
  say "040 file: $MIG, blob $blob (the file on disk equals it); sha256 $(shasum -a 256 "$MIG" | cut -c1-64)"
}

cmd="${1:-}"
case "$cmd" in
check)
  check_db; check_file
  say "040 on $DBNAME: $(present)"
  fingerprint "$OUT/schema_${TAG}_check_$(date -u +%Y%m%dT%H%M%SZ).txt"; say "schema fingerprint: $(wc -l < "$OUT"/schema_${TAG}_check_*.txt(om[1]) | tr -d ' ') lines" ;;

apply)
  check_db; check_file
  p=$(present); say "before: $p"
  [ "$p" = "0 of 8 columns, 0 of 2 CHECKs" ] || die "040 is not cleanly absent ($p); nothing applied"
  B="$OUT/schema_${TAG}_before.txt"; A="$OUT/schema_${TAG}_after.txt"
  fingerprint "$B"; say "schema fingerprint before: $(wc -l < "$B" | tr -d ' ') lines, md5 $(md5 -q "$B")"
  [ "$CONFIRM" = "$DBNAME" ] || die "not confirmed: give the database name as the third word; nothing applied" 5
  $PGBIN/psql -X -v ON_ERROR_STOP=1 -f "$MIG" 2>&1 | tee -a "$LOG"; [ ${pipestatus[1]} -eq 0 ] || die "040 failed; its transaction rolled back, nothing applied"
  p=$(present); say "after: $p"
  fingerprint "$A"; say "schema fingerprint after: $(wc -l < "$A" | tr -d ' ') lines, md5 $(md5 -q "$A")"
  comm -13 "$B" "$A" > "$OUT/schema_${TAG}_added.txt"; comm -23 "$B" "$A" > "$OUT/schema_${TAG}_removed.txt"
  python3 - "$OUT/schema_${TAG}_added.txt" "$OUT/schema_${TAG}_removed.txt" "${EXPECTED[@]}" <<'PY' | tee -a "$LOG"
import sys
added = [l.rstrip("\n") for l in open(sys.argv[1]) if l.strip()]
removed = [l.rstrip("\n") for l in open(sys.argv[2]) if l.strip()]
exp = sys.argv[3:]
cols = [l for l in added if l.startswith("column ")]
chks = [l for l in added if l.startswith("constraint ")]
ok = sorted(cols) == sorted(exp) and len(chks) == 2 \
     and all((".chk_executions_roll_split " in c) or (".chk_backtest_executions_roll_split " in c) for c in chks) and not removed
for l in added: print("  added: " + l)
for l in removed: print("  removed: " + l)
print("fingerprint change: %s (%d added, %d removed; expected the 8 columns and the 2 CHECKs, nothing removed)"
      % ("PASS" if ok else "FAIL", len(added), len(removed)))
sys.exit(0 if ok else 1)
PY
  [ ${pipestatus[1]} -eq 0 ] || die "the schema changed by more or less than 040 (see $OUT/schema_${TAG}_added.txt, _removed.txt). Undo: psql -f $WT/migrations/040_roll_split_rollback.sql"
  [ "$p" = "8 of 8 columns, 2 of 2 CHECKs" ] || die "040 is not complete after the apply ($p)"
  say "040 APPLIED to $DBNAME; undo: migrations/040_roll_split_rollback.sql from $WT" ;;

*) sed -n 2,26p "$0"; exit 2 ;;
esac
