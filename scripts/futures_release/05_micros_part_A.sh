#!/bin/zsh
# 05_micros_part_A.sh: the load of micro gold (MGC) and micro silver (SIL), part A, by load/apply_micro_metals_on_production.sh, with the data fingerprints
# (sql/data_fingerprint.sql, every row) taken before and after it in $RUN_DIR/data/.
#
#   ROWS=<folder> zsh 05_micros_part_A.sh production                  new_algo_data (config/defaults.json)
#   ROWS=<folder> SCRATCH_DB=<clone> zsh 05_micros_part_A.sh scratch   a rehearsal clone
#
# ROWS is required: the folder of rows to 2026-10-26, printed by 06_build_backfill.sh as ROWS=.
#
# CHANGES: futures_data.contract_ohlcv_1d gains the MGC and SIL contract bars to 2026-10-26 (the rows to 2026-10-26:
#   29,172 MGC and 18,618 SIL rows) and metadata.contract_metadata their two rows; insert only, one transaction. Inert:
#   no runner reads a root with no .v.0 row. The check after: the continuous tables unchanged, the contract table grown
#   by the micros' rows only, the metadata by 2.
# ORDER: DONE on production 2026-10-30.
#   It must come before AlgoGators/algogators#84's first pull (part A refuses any MGC or SIL row stored).
# READS: the four tables. TAKES: under a minute.
# UNDO: delete from futures_data.contract_ohlcv_1d where root in ('MGC','SIL'), and from metadata.contract_metadata
#   where "Databento Symbol" in ('MGC','SIL') (nothing else holds those roots before this part).
. "${0:A:h}/micros_part.zsh" A "$@"
