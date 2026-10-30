#!/bin/zsh
# 06_micros_part_A2.sh: the load of micro gold (MGC) and micro silver (SIL), part A2 (the contract bars after 2026-10-26),
# by load/apply_micro_metals_on_production.sh, with the
# data fingerprints (sql/data_fingerprint.sql, every row) taken before and after it in $RUN_DIR/data/.
#
#   BACKFILL=<folder> zsh 06_micros_part_A2.sh production
#   BACKFILL=<folder> SCRATCH_DB=<clone> zsh 06_micros_part_A2.sh scratch
#
# BACKFILL is required: the MGC and SIL contract bars of the days after 2026-10-26 up to the day before the seed, built
#   by 06_build_backfill.sh (load/build_rows.py, START=2026-10-27) from the pull of PULL_REQUEST.md.
# CHANGES: futures_data.contract_ohlcv_1d gains those rows; insert only. The part refuses before part A, after part B,
#   on any (instrument_id, date) already stored, and on any day not after the stored rows' last day. Inert, like A.
# ORDER (at the release): the one pull (2010-06-06 to the day before the merge), then this, then #84 merges (its daily pull
#   then writes the later days).
# READS: the four tables. TAKES: seconds. UNDO: delete those rows (root MGC or SIL, date after 2026-10-26).
. "${0:A:h}/micros_part.zsh" A2 "$@"
