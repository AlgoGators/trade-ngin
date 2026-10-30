#!/bin/zsh
# 07_micros_part_B.sh: the load of micro gold (MGC) and micro silver (SIL), part B (the continuous rows), by load/apply_micro_metals_on_production.sh, with the data
# fingerprints (sql/data_fingerprint.sql, every row) taken before and after it in production/data/.
#
#   [BACKFILL=<folder>] zsh 07_micros_part_B.sh production
#   [BACKFILL=<folder>] SCRATCH_DB=<clone> zsh 07_micros_part_B.sh scratch
#
# BACKFILL (the same folder as 06, when 06 ran) adds its continuous rows.
# CHANGES: futures_data.ohlcv_1d and ohlcv_1d_raw gain MGC.v.0 and SIL.v.0 (the rows to 2026-10-26: 4,979 and 4,129
#   rows in each table, plus the backfill's), one transaction. From here every futures runner's universe holds the two
#   micros: a config without their listing pairs refuses to run, so this part goes in TOGETHER with
#   AlgoGators/algogators#85, the listing pairs in every futures config, the release build and per_contract_rolls on
#   (AlgoGators/trade-ngin#270). The check after: the contract table and the metadata unchanged, both continuous
#   tables grown by the micros' rows only.
# ORDER (at the release): after 06, #84 and 04 (with #87); then 08 fills the micros' 8 holes; then #85 merges.
#   Precondition: the read of the QT books (README, the QT precondition).
# READS: the four tables. TAKES: under a minute.
# UNDO: delete from futures_data.ohlcv_1d and ohlcv_1d_raw where symbol in ('MGC.v.0','SIL.v.0').
. "${0:A:h}/micros_part.zsh" B "$@"
