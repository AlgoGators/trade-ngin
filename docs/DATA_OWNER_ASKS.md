# Data-owner ask packet — 2026-08-30

Ready-to-send list for whoever runs data-ngin / the DB (evidence for every item in
docs/DB_AND_DATA_AUDIT_2026-08-27.md and docs/FINDINGS_2026-08-27.md §9).

> **Added 2026-09-03:** the 2026-08-07 equity daily bar is missing for the whole universe (Thu 08-06 → Mon 08-10). Every replay window is pinned to end ≤ 08-06 / 08-04 because of it. Please backfill or confirm the source gap.

## Broken / stopped (highest priority)
1. **Futures ingest is dead since 2026-08-06** (6J/ZC/ZF stopped 08-04). 33 symbols' last
   bar = 08-06. Probable cause: the 08-06 data-ngin 5-PR deploy burst (incl. the
   creds-removal + equities-rename changes). Please restart Databento futures ingest.
2. **Equity re-adjustment job dead since the same deploy**: bars append fine (through
   yesterday) but adj_* columns are no longer rescaled — MNST's 08-11 2:1 split shows a
   fake −50% adjusted day; ~50 ex-divs since 08-10 unadjusted. One full-history re-fetch
   heals it. (Trade-ngin now computes adjustment per-bar and is NOT blocked by this — but
   every other adj_* consumer, e.g. AlgoLens/algosystem, reads corrupt values post-08-05.)
3. **corporate_action table frozen since 2025-08-29** (a full year). Dividends/splits now
   arrive per-bar so returns are fine, but merger/spinoff/acquisition DEAL TERMS have no
   other home. Please revive the feed and backfill 2025-08-29 → now, or state the
   replacement plan.

## Data-quality items
4. **LEN 2025-02-07 (Millrose spinoff, ~4.2%)**: unadjusted in BOTH per-bar factors and
   adj_* columns. Please attribute (Tiingo defect vs pipeline drop) and fix; the frozen
   corp-action table has the event, which is how we caught it.
5. **Symbol backfills**: ECHO, FERG, FLEX, HONA, MRVL, PARA have coverage gaps.
6. **delisting_date upkeep**: latest value 2026-04-09 — confirm "no delistings since
   April" vs "column stopped updating".
7. **macro_data stale since ~2026-04-08** — regime pipeline inputs; please refresh.

## Security
8. **Rotate the DB password** (literal, redacted): it sat in a committed script comment on a
   GitHub branch for 3 months (now stripped, commit 5f6f00e). Also: config/defaults.json
   holds plaintext creds locally — fine if intended, flagging for the secrets story.

## New data requirements (future work, no urgency — scoping wanted)
9. **MBFS (regime liquidity/stress features)**: bid-ask spreads, order-book depth /
   intraday microstructure for the futures universe. Vendor/schema discussion needed.
10. **Credit spreads**: `macro_data.credit_spreads` already exists as a macro-DFM input —
    please confirm it's populated and on a refresh schedule (it feeds planned
    funding-stress detection; the wiring on our side is roadmap work, not blocked).

## One question (blocks a migration step, not data)
11. Does the CURRENTLY DEPLOYED AlgoLens read path filter `trading.positions` by
    `portfolio_type`? Migration 002 (qt backfill) doubles the rows per stream; an
    unfiltered reader would display a doubled portfolio until AlgoLens #80 deploys.
    (Migration 001 is already applied — 2026-08-30, verified.)


## Added 2026-09-03 (B-4)
- `equities_data.ohlcv_1d` has NO bars for RAL (FTV spinoff 2025-06-30) or MRP (LEN spinoff 2025-02-07). Until the child series exist every post-2024 spinoff takes the refusal path (parent carries a pre-spinoff basis, loud WARN every run).
- `corporate_action.spinoffdividend.value` differs from the parent bar's `div_cash` (MMM 2024-04-01: 17.85141 vs 17.3875 = 0.25 × SOLV 69.55). The code uses `div_cash`, which is what the adjusted series encodes; ask which reference price the terms value uses.
- LEN 2025-02-07: terms rows exist (spinoff 0.5 MRP, spinoffdividend 11.495) but the LEN bar carries split_factor 1 / div_cash 0, so nothing routes.

- HOLD (2026-09-04): do NOT ingest PK/HGV/RAL/MRP child bars until commit for E2-F50 (B-5c) is on the branch — a refused reverse-split spinoff would retry on the post-split share count. Packet status: NOT YET SENT; send after the end gate, dated.
- UPDATE 2026-09-04: the PK/HGV/RAL/MRP hold now lifts when B-5b's BA-25 (epsilon floor on the retried spinoff) is on the branch, not at B-5c.
- DO-16 (2026-09-04): ABT 2004-05-03 → HSP: the vendor price step (1.142465) is ~2× the distribution the terms encode; which adjustment is authoritative for pre-2010 spinoffs?
