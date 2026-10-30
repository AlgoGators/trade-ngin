#!/bin/zsh
# 07_micros_part_B.sh: the load of micro gold (MGC) and micro silver (SIL), part B (the continuous rows), by load/apply_micro_metals_on_production.sh, with the data
# fingerprints (sql/data_fingerprint.sql, every row) taken before and after it in $RUN_DIR/data/.
#
# WARNING, THE QT RUNS. Stop the QT runs BEFORE this part, and restart them only after their configs and the merged build
#   are in. No config is valid on both sides of this part (src/data/listing_dates.cpp): a config with the MGC and SIL
#   listing pairs refuses to run while MGC.v.0 and SIL.v.0 are not stored, a config without the pairs refuses once they are,
#   and code without that refusal takes MGC and SIL in beside GC and SI. This script cannot see a server's config or
#   schedule and does not check them: stopping the runs is the operator's and the QT desk's owner's job (README, "The QT
#   runs").
#
#   ROWS=<folder>/rows BACKFILL=<folder> zsh 07_micros_part_B.sh production
#   ROWS=<folder>/rows BACKFILL=<folder> SCRATCH_DB=<clone> zsh 07_micros_part_B.sh scratch
#
# Two folders, both printed by 06_build_backfill.sh: ROWS is the history to 2026-10-26, BACKFILL the days after it (the same
#   folder as 06 part A2). With no BACKFILL the part refuses once A2 has stored later contract rows.
# REFUSES (exit 8) before the hole fill: both continuous tables must hold the filled rows to 2026-10-23, 167,087 each less
#   the micros. Filling after this part cannot work: each fill then refuses because of the other's rows.
# CHANGES: futures_data.ohlcv_1d and ohlcv_1d_raw gain MGC.v.0 and SIL.v.0 (the rows to 2026-10-26: 4,979 and 4,129
#   rows in each table, plus the backfill's), one transaction. From here every futures runner's universe holds the two
#   micros: a config without their listing pairs refuses to run, so this part goes in TOGETHER with
#   AlgoGators/algogators#85, the listing pairs in every futures config, the release build and per_contract_rolls on
#   (AlgoGators/trade-ngin#270). The check after: the contract table and the metadata unchanged, both continuous
#   tables grown by the micros' rows only.
# ORDER (at the release): after 06, #84 and 04 (with #87); then 08 fills the micros' 8 holes; then #85 merges.
# READS: the four tables. TAKES: under a minute.
# UNDO: 07_undo_part_B.sh (README, "The way back from part B").
. "${0:A:h}/micros_part.zsh" B "$@"
