# Complete change ledger — every change made, 2026-08-27 → 2026-08-31

Every code change across the main-branch fix batches and equities phases 4.1–E1.
Columns: what changed · what it fixed · **origin** (PRE-EXISTING vs INTRODUCED-BY-US) ·
impact size · scope (which paths it can affect).

**Origin matters**: an INTRODUCED-BY-US entry means an earlier change in this same effort
created the defect. Five such entries exist; all were caught before any production run.

---

## PART A — main branch (batches 1′/2′, merged as `462be00`)

| # | Commit | What changed | What it fixed | Origin | Impact | Scope |
|---|---|---|---|---|---|---|
| A1 | `cf6c263c` | `carver_buffer_position_factor` default 0.2 → 0.0 at 4 sites; pin test asserts struct = loader = config_template | Config shipped 0.0 while code defaulted 0.2 — a run without a config file silently traded a different strategy | PRE-EXISTING | **Zero** for config-driven runs (config already 0.0) | Futures backtest + live; configless runs only |
| A2 | `56daf2d6` | Loader `ExecutionConfig` renamed `ExecutionSettingsConfig`; EE unsubscribes both MarketDataBus registrations; 25 disabled tests re-enabled | **ODR violation**: two classes with the same name — linker kept one ctor, so every engine config was built by the wrong type and destroyed reading uninitialised memory. Root cause of the long-standing test crashes | PRE-EXISTING | **Large** — removed undefined behaviour + a production callback leak | Execution engine; both asset classes |
| A3 | `1edd7992` | Renormalisation now scales only uncapped symbols | 50%-of-sector cap was re-inflated by the renormalise step that followed it — the cap was not a cap | PRE-EXISTING | **Small but real**: 24 fewer churn executions over 2y, positions otherwise identical | Futures backtest + live (the one intended behaviour change) |
| A4 | `daf81a2b` | Deleted `previous_positions_` + stale loaders; `positions_` is the single prior-position source | Three trend variants used two different first-day mechanisms; the deleted loaders were already no-ops (key mismatch) | PRE-EXISTING | **Zero measured** (proven: identical positions across 369 days) | Futures live |
| A5 | `54a706e5` | NaN-input guards, downside-vol dust floor, degeneracy WARN | Sortino could divide by dust; NaN equity points poisoned monthly returns; degenerate series reported Sharpe 0 indistinguishable from a real zero | PRE-EXISTING | Reporting only | Metrics/reporting, both asset classes |
| A6 | `138c46d5` | Exact symbol registration wins over ES/YM/NQ→micro remap | A future equity "ES" (Eversource) would silently resolve to MES futures | PRE-EXISTING | **Zero today** (no bare ES registered); arms safety for equity universes | Instrument registry, both |
| A7 | `a38ace3d` | Migration test script requires explicit scratch-DB authorisation; rollback reports clearly on un-migrated DB | The script opened with `DROP SCHEMA trading CASCADE` against ambient env vars | PRE-EXISTING | Tooling safety | Developer tooling only |
| A8 | `b96180ce` | 4 hot-path traces INFO → DEBUG | Per-symbol-per-cycle logging in production paths | PRE-EXISTING | Log volume | Both |

**Verified empirically**: head-to-head backtest, pre-batch vs post-batch, identical positions
across all 369 days; sole delta = A3's 24 churn executions, fully attributed.

---

## PART B — equities: data layer (phase 4.2)

| # | Commit | What changed | What it fixed | Origin | Impact | Scope |
|---|---|---|---|---|---|---|
| B1 | `138ed742` `fb5d8713` `b38b13a9` | #57 cherry-picked (jrile018's authorship preserved): asset-class column lists, OPTIONS early rejection | Equity backtests read unadjusted prices; OPTIONS resolved to a non-existent schema | PRE-EXISTING | **Large** — equity prices were wrong | Equity backtest |
| B2 | `4226e3b5` | Adjustment computed from raw + `div_cash`/`split_factor`; vendor `adj_*` never read | The DB reshape removed `closeadj`; equity queries errored outright. Vendor `adj_*` also stale after 2026-08-05 | PRE-EXISTING | **Large** — equity path was broken; now outage-immune | Equity backtest + live |
| B3 | `9458f17a` | `bt_equity_validation` recomputes adjustment via the shared helper; SQL literals quoted | Tool used the dead `closeadj/close` ratio; string-concat SQL | PRE-EXISTING | Medium (validation tool) | Equity validation tool |
| B4 | `43dfefb7` | Commission query column `commission` → `commissions_fees`; dividend double-count contract pinned by test | Query errored every run, caller silently degraded to an empty map | PRE-EXISTING | Medium — data existed, was never read | Equity live reporting |

**Verified**: recomputed series matches vendor `adjusted_close` to **9.489e-13** max relative
deviation over 2023-01-01 → 2026-08-05, 12 symbols, 10,800 bars, 132 corporate actions.

---

## PART C — equities: corporate actions (phase 4.3)

| # | Commit | What changed | What it fixed | Origin | Impact | Scope |
|---|---|---|---|---|---|---|
| C1 | `75a9ed29` | Corp actions reorganised by mechanical effect class; class 1 re-sourced to per-bar columns; classes 2/3 wired to `ticker_aliases` / `delisting_date`; deal-terms query parameterised for revival | **Corporate actions were never applied to live positions.** The 14-day query hit a feed frozen since 2025-08-29 — dead before the code shipped | PRE-EXISTING | **Large** — splits/dividends silently unhandled on the live book | Equity live |
| C2 | `c8eccf24` | 16 tests incl. revived-feed rollover | No coverage of the new structure | — | Test coverage | Equity live |

---

## PART D — equities E1: the pre-run fixes

| # | Commit | What changed | What it fixed | Origin | Impact | Scope |
|---|---|---|---|---|---|---|
| D1 | `9b0d293c` | 10 `BASE_PORTFOLIO` literals → `app_config.portfolio_id` | Live equity wrote into the **futures** portfolio's namespace | PRE-EXISTING | **Large if run** — would have polluted futures data. No equity run had happened, so no repair needed | Equity live |
| D2 | `67fe3713` `190dbe7b` | Migration 002 (`trading.corp_action_applied`); window derived from state; dedup moved file → DB | Dedup state was a JSON file under a container path **with no volume** — loss was the default, and loss means re-applying every event | PRE-EXISTING | **Large** — the corruption guard was not durable | Equity live |
| D3 | `38f9f80a` | UTC date keys (6 sites); denominator on the ex-date close; sargable event window | Bar keys shifted a day on the deployed TZ; denominator mismatched the price series; 12.3s query | PRE-EXISTING | Medium; **denominator net-zero** (two errors were cancelling) | Equity live |
| D4 | `2c608722` `cf31162b` `3e62d824` | Migration 003 indexes; parameter arrays; scoped `work_mem` | Full-universe queries: 14.3s → 0.8s, 14.1s → 1.9s | PRE-EXISTING | Performance only | Equity read paths |
| D5 | `c15f4ac9` | `ticker_aliases` backfilled 16 → 389 rows from the frozen table | Rename coverage was 16 curated rows | PRE-EXISTING | Data completeness | Equity live |
| D6 | `5c2a9c0e` | `data_staleness_tolerance_days` exposed in template | Guard used an invisible code default | PRE-EXISTING | Config visibility | Equity live |
| D7 | `35060664` | Window derived from **position inception**; per-symbol close top-up | **D2's window fix was a silent no-op** — `load_positions_by_date` filters `WHERE DATE(last_update)=$4`, so every row carried the requested date and the window always collapsed to its 14-day floor | **INTRODUCED-BY-US (D2)** | **Large** — the fix wasn't working | Equity live |
| D8 | `d7f95bb5` | Dedup read keyed on `strategy_name`, matching the write | Write used 6 key columns, read used 2 — strategy A's rows returned to strategy B, B skips its own adjustment | **INTRODUCED-BY-US (D2)** | Latent (single-strategy today); **large** at scenario S1 | Equity live, multi-strategy |
| D9 | `278af5c4` | Duplicate type mapping consolidated | Two byte-identical copies — the pattern that produced the original DIVIDEND/dividend bug | **INTRODUCED-BY-US (C1)** | Hygiene | Equity live |
| D10 | `942bfe32` | `bool load()` → `Result<bool>`; runner aborts on read failure | Claimed to fail closed; actually conflated read-error with empty → empty applied-set → **every event re-applied** | **INTRODUCED-BY-US (D2)** | **Large** — D7 widened the window to years, enlarging the blast radius | Equity live |
| D11 | `b2329b59` | Dedup entries mirrored under the current ticker across renames | Vendor migrates a renamed symbol's whole history to the new ticker, so old events resurface and re-apply (AA→HWM: 67 pre-rename dividends) | **INTRODUCED-BY-US** (reachable only via D7 + D5) | **Large** when a held symbol is renamed | Equity live |
| D12 | `464533f0` | `DbTransaction` unit of work; positions + dedup commit atomically; redundant `BEGIN` removed | Two separate transactions: if dedup save failed after positions committed, the next run re-applied | PRE-EXISTING (architectural: every write owned its transaction) | Medium — narrow window, loud failure, but permanent corruption | Equity live; **shared write path** |
| D13 | `53a32eb5` | Rename mirroring bounded by `effective_until` | **33 tickers have ≥2 successors** (BBT→BBT1 1998, BBT→TFC 2019); the map picked an arbitrary winner by read order | **INTRODUCED-BY-US (D11)** | Medium; real ambiguity, not hypothetical | Equity live |
| D14 | `0fff1362` | `TRADE_NGIN_REQUIRE_DB=1` converts DB-skips into failures | DB-dependent tests skip silently; a regression would pass CI green | PRE-EXISTING (test infra) | Test-safety only. **NOT enabled in CI — CI has no DB service** | Test infrastructure |
| D15 | `fd73cbc3` | Runner refuses a dedup log that cannot bridge renames; stale class doc corrected | Doc still said "No DB dependency by design" three phases after migration 002; a reader would wire the unsafe backing | PRE-EXISTING (doc) + guard for D11 | Medium — prevents a future foot-gun | Equity live |
| D16 | `d075255e` | Window log reports its derivation rule, not just the date | Only production evidence the D7 fix works was a date a human had to judge | PRE-EXISTING (observability) | Observability | Equity live |

---

## Summary

**Origin**: 5 defects were INTRODUCED-BY-US (D7, D8, D9, D10, D11, refined by D13) — all
caught before any production run, each by the next verification layer. The rest were
pre-existing.

**Impact concentration**: the large-impact items are A2 (ODR/UB), B1+B2 (equity prices
wrong/broken), C1 (corp actions never applied), D1 (namespace collision), D2/D7/D10/D11
(the dedup-correctness chain). Everything else is medium or smaller.

**Scope**: only A1–A8 and D12 touch shared or futures paths. Futures behaviour was verified
**by execution** — byte-identical live output, same date, two binaries — plus a
statement-by-statement diff showing only intended edits.

**Verification state**: unit suite 1425/1425. Backtest head-to-head identical (369 days).
Futures live replay byte-identical. Corp-action live path: **never executed** — that is E2.
