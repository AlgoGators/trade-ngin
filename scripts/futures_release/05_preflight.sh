#!/bin/zsh
# 05_preflight.sh: read only, before 05 (part A) on production: the load's preflight (load/preflight.sql: server, TimeZone, TimescaleDB version, each table's kind and keys, hypertables and compression, what of MGC
# and SIL is stored) and, per target hypertable, its compressed chunks overlapping the dates part A writes
# (2010-01-01 to 2026-10-27). Part A must not run if any exists and the version cannot insert into compressed chunks.
#
#   zsh 05_preflight.sh production | SCRATCH_DB=<clone> zsh 05_preflight.sh scratch
#
# CHANGES: nothing (every session read only). READS: the catalog and the MGC/SIL counts. TAKES: seconds.
set -u
. "${0:A:h}/lib.zsh"
TARGET="${1:-${TARGET:-}}"
resolve_target
export PGOPTIONS='-c default_transaction_read_only=on'
mkdir -p "$RUN_DIR/data"; R="$RUN_DIR/data/05_preflight_${TAG}_$(date -u +%Y%m%dT%H%M%SZ).txt"
{ print -r -- "== 05 preflight, $TAG, $(date -u +%FT%TZ)"
  $PGBIN/psql -X -v ON_ERROR_STOP=1 -f "$PROD/load/preflight.sql"
  print -r -- "== compressed chunks overlapping 2010-01-01 .. 2026-10-27, per target hypertable"
  if [ "$(qr "select count(*) from pg_extension where extname='timescaledb'")" = 1 ]; then
    $PGBIN/psql -X -v ON_ERROR_STOP=1 -c "select h.hypertable_schema || '.' || h.hypertable_name as hypertable, h.compression_enabled,
        count(c.*) filter (where c.is_compressed) as compressed_chunks_in_range, count(c.*) as chunks_in_range
      from timescaledb_information.hypertables h
      left join timescaledb_information.chunks c on c.hypertable_schema = h.hypertable_schema and c.hypertable_name = h.hypertable_name
        and c.range_start < '2026-10-28' and c.range_end > '2010-01-01'
      where (h.hypertable_schema, h.hypertable_name) in (('futures_data','contract_ohlcv_1d'), ('metadata','contract_metadata'))
      group by 1, 2 order by 1"
    print -r -- "compressed chunks in range, all target tables: $(qr "select count(*) from timescaledb_information.chunks where hypertable_schema in ('futures_data','metadata') and hypertable_name in ('contract_ohlcv_1d','contract_metadata') and is_compressed and range_start < '2026-10-28' and range_end > '2010-01-01'")"
  else
    print -r -- "compressed chunks in range, all target tables: 0 (TimescaleDB not installed)"
  fi
} 2>&1 | tee "$R"
rc=${pipestatus[1]}; scrub "$R"
[ $rc -eq 0 ] || die "the preflight failed or refused (see $R)"
print -r -- "kept: $R"
