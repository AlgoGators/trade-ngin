#!/bin/zsh
# 01_readonly_checks.sh: the read-only checks on production, before the data steps and after any step. Writes nothing to
# the database: every session is forced read only (default_transaction_read_only) and the SQL holds SELECTs only.
#
#   zsh 01_readonly_checks.sh production [label]                 new_algo_data (config/defaults.json)
#   SCRATCH_DB=<clone> zsh 01_readonly_checks.sh scratch [label]  a rehearsal clone
#
# PRINTS: sql/readonly_checks.sql (server, universe, micros, last bars, the books, the held GC/SI, the backtests, 040),
#   the schema fingerprint (sql/schema_fingerprint.sql) and the data fingerprint to the cut 2026-10-23
#   (sql/data_fingerprint.sql). The expected answers per step: EXPECTED.md. The output is kept in
#   $RUN_DIR/checks/<target>_<label>_<stamp>.txt.
# CHANGES: nothing. READS: the tables named. TAKES: about a minute (the fingerprints read every bar row).
# UNDO: nothing to undo.
set -u
. "${0:A:h}/lib.zsh"
TARGET="${TARGET:-${1:-}}"; LABEL="${2:-check}"
resolve_target
export PGOPTIONS='-c default_transaction_read_only=on'
mkdir -p "$RUN_DIR/checks"; R="$RUN_DIR/checks/${TAG}_${LABEL}_$(date -u +%Y%m%dT%H%M%SZ).txt"
{ print -r -- "== read-only checks, $TAG, $LABEL, $(date -u +%FT%TZ)"
  $PGBIN/psql -X -At -F' | ' -v ON_ERROR_STOP=1 -f "$PROD/sql/readonly_checks.sql"
  print -r -- "== schema fingerprint: $($PGBIN/psql -X -At -v ON_ERROR_STOP=1 -f "$PROD/sql/schema_fingerprint.sql" | tee "$R.schema" | wc -l | tr -d ' ') lines, md5 $(md5 -q "$R.schema")"
  print -r -- "== data fingerprint to the cut 2026-10-23"
  $PGBIN/psql -X -At -v ON_ERROR_STOP=1 -v cut=2026-10-23 -f "$PROD/sql/data_fingerprint.sql" | cut -c1-200
} 2>&1 | tee "$R"
[ ${pipestatus[1]} -eq 0 ] || die "a check failed to run"
print -r -- "kept: $R"
