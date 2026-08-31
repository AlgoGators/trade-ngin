# Equities — Definitive Completion Plan

**Branch**: `equities_integration` @ `c8eccf24` · **Date**: 2026-08-31 · **Status**: awaiting HD approval to execute

Merged from every source: the 05-22 review notes (all sections, not just T2), the
2026-08-31 branch audit, the 2026-08-31 multi-strategy capability audit, the
corp-actions data-boundary doc, the April integration review, and items that until
now lived only in conversation. Every status below was **verified in code or DB**,
not inherited from a prior summary — five prior claims proved wrong and are flagged.

Companion docs: `EQUITIES_REVIEW_NOTES_2026-05-22.md`, `EQUITIES_BRANCH_AUDIT_2026-08-31.md`,
`MULTI_STRATEGY_CAPABILITY_2026-08-31.md`, `CORP_ACTIONS_DATA_BOUNDARY.md`,
`DATA_OWNER_ASKS_2026-08.md`.

**Before touching any equity data path, read `DATA_SOURCES_OF_TRUTH.md`.** It is
the single reference for which table and column to read, which look authoritative
but must never be read (the vendor `adj_*` columns, stale after 2026-08-05), which
are frozen-but-needed (`corporate_action`) versus genuinely superseded
(`sharadar_ohlcv_1d`), and the rule that new code must be correct at the full
852-symbol universe rather than the 10 symbols currently configured. It closes
V4-4 and V4-5 below.

---

## E1 — COMPLETE (2026-08-31, local commits, not pushed)

| Item | Commit | Outcome |
|---|---|---|
| Live equity wrote to `BASE_PORTFOLIO` | `9b0d293c` | Now uses `app_config.portfolio_id`; 10 literals gone. No equity run had happened, so no DB repair needed |
| F-1 window + F-6 durable dedup | `67fe3713`, `190dbe7b` | Window derived from the oldest held position, clamped to [14d, historical_days], with a loud guard; dedup moved to `trading.corp_action_applied` with one-time file import. Caught a real round-trip bug: `type_to_string`/`type_from_action_string` are not inverses |
| F-2 + F-3 + V2-1 | `38f9f80a` | UTC date keys; dividend denominator moved to the ex-date close so basis and marks share one frame (§B6's raw/adjusted mix preserved); sargable event window — 489 rows identical both ways, 14,301 ms → 1,764 ms |
| V2-2 / V2-3 indexes | `2c608722`, `cf31162b` | delisting 14,144 → 1,865 ms; corp events → 800 ms (17.9× end-to-end); deal terms 491 → 281 ms. Param arrays replace 5 kB IN-lists |
| `ticker_aliases` backfill | `c15f4ac9` | 16 → 389 rows, additive only, direction verified against curated ground truth |
| Staleness config key | `5c2a9c0e` | Now visible to operators in `config_template` |
| `work_mem` | `3e62d824` | Spills gone (18 MB), but **not a speedup** — 33.7 s → 33.2 s. Full-universe adjustment is inherently ~25 s; noted for E2 planning |

Suite: 1390 → 1399, all green.

---

## 0. Status claims that were WRONG (corrected here)

| Claim | Source | Reality (verified) |
|---|---|---|
| "Live write path is NOT per-strategy; two strategies stored as one aggregated row set" | MULTI_STRATEGY doc §5 | **WRONG.** `live_portfolio_conservative.cpp:1316` passes `combined_strategy_id` **and** `strategy_name` per strategy; `trading.positions` PK includes `strategy_name`; DB shows `TREND_FOLLOWING` (80 rows) and `TREND_FOLLOWING_FAST` (40) separately under the same combined id, including opposite positions in the same symbol on the same date. Live **does** attribute per strategy in `positions`, `executions`, `signals`. Only `live_results` and `equity_curve` lack a `strategy_name` column. The 05-22 doc (Phase 7a note) was right all along. |
| "Equity data 268 days stale" | 05-22 §F7 | **RESOLVED** — `equities_data.ohlcv_1d` current through 2026-08-27. |
| "Phase 4.5 migration not applied" | 05-22 §F4/§G3/T2.3 | **DONE** — `trading.live_results.total_dividend_income` present. |
| "`load_commissions_by_symbol` queries a column that doesn't exist" | 4.2 FIXME | **Misdiagnosis** — data exists as `commissions_fees`; fixed in `43dfefb7`. |
| "`STICKY_DEBUG` remains only in `backtest_coordinator.cpp:1002`" | conversation | **Incomplete** — also at INFO in `trend_following_fast.cpp:623`. Two sites, not one. |

**One conflict to handle with care, not a correction:** 05-22 §B6 documents the applier's
raw-dollar-÷-adjusted-close **frame** mix as deliberate and load-bearing. Branch-audit F-3
is about a different axis — **which close** (T-1 vs ex-date). Both can be true. Any F-3 fix
must preserve B6's intentional frame choice; a naive "make it consistent" refactor breaks it.

---

## 1. Merged item table

Status: ✅ DONE · 🟡 PARTIAL · ❌ OPEN · ⏳ REQUIRES RUN · 🚫 CLOSED (superseded/by decision)
Blocks = does it block "equities complete"?

### 1a. Original 05-22 T2 tasks

| ID | Description | Status | Evidence / what remains | Size | Deps | Blocks |
|---|---|---|---|---|---|---|
| T2.1 | `live_equity_mr` universe-load config-first | ✅ | `live_equity_mean_reversion.cpp:238-241` | — | — | — |
| T2.2 | `bt_equity_validation` annualization formulas | ✅ | `:926` `ann_return = mean_return*252.0` | — | — | — |
| T2.3 | Phase 4.5 DB migration | ✅ | column present in DB | — | — | — |
| T2.4 | Delete orphan `bpgv_rotation.cpp` | ✅ | absent from tree | — | — | — |
| T2.5 | `ON CONFLICT` mismatch | 🟡 | call site removed; **buggy SQL survives** at `postgres_database.cpp:1995` → F-5 | S | — | No |
| T2.6 | Equity runners iterate `strategies_config` | 🟡 | bt+live use `collect_enabled_equity_strategies`; **`bt_equity_validation` still hardcodes** `lookback_period=20`/`vol_lookback=20` (`:274,:279`) | S | — | Yes |
| T2.7 | Resolve 4 main↔equities conflicts | ✅ | merge `0d899f25`, suite 1390/1390 | — | — | — |
| T2.7b | Corp-actions/dividend E2E (staging) | ⏳ | **never executed**; promoted to Tier-1 acceptance criterion | M | E1 | Yes |
| T2.8 | Full pipeline + futures regression | ⏳ | — | M | E2 | Yes |
| T2.9 | Data-freshness guard | 🟡 | guard live; **`data_staleness_tolerance_days` absent from every config file** (code default 4 only) | S | — | Yes |
| T2.10 | REG_T config wireup | ❌ | `account_mode` in `equity.hpp:58`, in no config/data file | M | — | **No** (conditional) |
| T2.11 | F8 daily-PnL regression test | ✅ | `test_daily_pnl_identity.cpp`, 3 tests | — | — | — |
| T2.12 | P1 test backlog T-OR.2/3/4/5 | 🟡 | only T-OR.5 exists and is **construction-drift, not runner-parity** (`test_live_bt_signal_consistency.cpp:108-109` builds both sides via the same helper) | S×3 + M | — | Yes |
| T2.13 | REG_T distinct maintenance margin | ❌ | `maintenance_margin_pct` absent | M | T2.10 | No |
| T2.14 | Margin interest accrual | ❌ | absent | M | T2.10 | No |
| T2.15 | Margin-call detection | ❌ | absent | L | T2.13 | No |
| T2.16 | Mixed futures+equity runner | ❌ | no `bt_portfolio_mixed`; coordinator single `asset_class` | L | MS-8, MS-9 | No |
| T2.17 | T-OR.6 two equity strategies, same symbols | ❌ | needs MS-4 + MS-5 | M | MS-4/5 | No |
| T2.18 | Phase 7a per-strategy `live_results` | ❌ | `live_results`/`equity_curve` lack `strategy_name`; **positions already per-strategy** so P&L is reconstructable | M | — | **No** (HD: missed by design) |
| T2.19 | Phase 7b asset-class breakouts | ❌ | 0 matching columns | M | T2.16 | No |
| T2.20 | `get_weights()` inter-call caching | 🟡 | instance cache `trend_following.hpp:216` populate-once — perf goal met; **no invalidation hook** (doc requires one), caches a DB-derived universe forever | S | — | No |
| T2.21 | P2/P3 tests (T-OR.7–10) | ❌ | absent | M | — | No |
| T2.22 | Corp-action handler extension | 🟢 | **mostly done by 4.3** — all 19 labels classified, renames + terminations + merger rollover (`lifecycle.cpp:124-132`). **Gap: spinoff child-share receipt** → NEW-5 | M | — | No (child-share = live prereq) |
| T2.23 | `closeunadj` sanity-check | 🚫 | **CLOSED as superseded** (HD approved) — 4.2 cross-checks vendor series directly | — | — | — |
| T2.24 | Borrow-rates DB table | ❌ | conditional on broker scope | M | — | No |

### 1b. Original 05-22 F / L / V findings

| ID | Description | Status | Evidence / remains | Size | Blocks |
|---|---|---|---|---|---|
| F1 | Orphan `bpgv_rotation.cpp` | ✅ | deleted | — | — |
| F2 | `ON CONFLICT` noise | 🟡 | = T2.5; latent SQL → F-5 | S | No |
| F3(05-22) | DB-test env gate opt-out | ❌ | 1 test file; documented non-blocker | S | No |
| F4 | Phase 4.5 migration | ✅ | applied | — | — |
| F5(05-22) | `asset_type` plumbing dead-ends | ❌ | defense-in-depth only; doc says no action required | S | No |
| F6(05-22) | `instrument_data_` scaling post-L1 | ⏳ | sanity-check during E2 equity E2E | S | No |
| F7(05-22) | Data freshness 268 days | ✅ | **resolved** — current to 2026-08-27 | — | — |
| F8(05-22) | Daily-PnL formula coincidence | ✅ | = T2.11, test added | — | — |
| F9 | `get_weights()` caching | 🟡 | = T2.20 | S | No |
| F10 | Equity runner silent strategy drop | ✅ | `collect_enabled_equity_strategies` ERRORs on unknown; live ERRORs on >1 | — | — |
| L1 | Live universe-load exit 1 | ✅ | = T2.1 | — | — |
| L2 | Recursive-mutex warnings on live shutdown | ❌ | cleanup-path bug, 5× per run; noisy not fatal | S | No |
| V1 | Validation sharpe under-annualized | ✅ | = T2.2 | — | — |

### 1c. Data dependencies (05-22 §G)

| ID | Description | Status | Remains | Blocks |
|---|---|---|---|---|
| G2-a | Corp-action filter only 3 of 19 types | ✅ | all 19 classified by 4.3 | — |
| G2-b | `closeunadj` never read | 🚫 | closed superseded | — |
| G2-c | `lastupdated` never read | ❌ | could power freshness guard without a `MAX(date)` query; nice-to-have | No |
| G3 | `total_dividend_income` column | ✅ | applied | — |
| G4-a | `short_borrow_rates` table | ❌ | = T2.24, conditional | No |
| G4-b | Equity rows in `metadata.contract_metadata` (7c) | ❌ | JSON sufficient at current scale | No |
| G6-1 | OHLCV feed schedule/staleness | ✅ | resolved; futures feed separately dead (data-owner item) | — |
| G6-2 | Corp-action feed lockstep | ❌ | **restated**: class-1 no longer uses that feed; ask is now "revive deal-terms feed" (`DATA_OWNER_ASKS` item 3) | No (live prereq) |
| G6-3 | Vendor `closeadj` backward-adjusted | 🚫 | **obsolete** — we don't read it; 4.2 verified own math to 9.5e-13 | — |
| G6-4 | `corporate_action.date` is TEXT — cast may break | ❌ | add a data-layer regression test | S / No |
| G6-5 | Future-dated corp-action rows (2027) leak if `end_date` widens | ❌ | assert upper bound tight | S / No |

### 1d. Branch audit findings (2026-08-31)

| ID | Description | Sev | Status | Size | Blocks |
|---|---|---|---|---|---|
| F-1 | Applier 14-day window vs full-history price adjustment — events dropped after a run gap. **8 of 9 dividends since 2026-05-03 fall outside it**; value silently lost from P&L | P1 | ❌ | M | **Yes** |
| F-2 | UTC bars formatted with `localtime` (6 sites) — bar dates shift a day on the `TZ=America/New_York` runtime image; dividend denominator picks the post-drop close | P1 | ❌ | M | **Yes** |
| F-3 | Applier (T-1 close) vs price series (ex-date close) dividend denominators — differ at 2nd order. **F-2 currently masks this; fix together** | P2 | ❌ | S | Yes (with F-2) |
| F-4 | Rename re-keys the position but not the tradeable universe → position with no target | P2 | ❌ | S | No |
| F-5 | Latent `ON CONFLICT (run_id)` SQL for the next caller | P2 | ❌ | S | No |
| F-6 | Corp-action idempotency state is an on-disk file — losing it re-applies every event, corrupting basis | P2 | ❌ | M | **Yes** (pairs with F-1) |
| F-7 | Backtest/live adjusted-price frames differ by window anchor — absolute levels differ, returns invariant | P2 | ❌ | S | Yes (control in T-OR.5) |
| F-8 | Adjusted-frame basis vs broker raw basis — reconciliation rule needed before real money | P2 | ❌ | S | No (live prereq) |

### 1e. Multi-strategy / multi-portfolio (2026-08-31)

| ID | Description | Status | Size | Blocks |
|---|---|---|---|---|
| MS-1 | Live equity hardcodes `"BASE_PORTFOLIO"` at **10 sites** while config says `EQUITY_MR_PORTFOLIO` → equity rows land in the futures namespace | ❌ | S | **Yes** (must precede any live equity run) |
| MS-2 | Runner portfolio is a compile-time literal → CLI/env override | ❌ | S | No |
| MS-3 | `bt_equity_mean_reversion` run metadata records only the first strategy's config | ❌ | S | No |
| MS-4 | Only one equity strategy *type* exists (`MeanReversionStrategy`) | ❌ | M | No |
| MS-5 | Live equity hard-rejects >1 strategy (`:226`); ~25 hardcoded id sites | ❌ | M | No |
| MS-6 | Live per-strategy attribution | 🟡 | M | **No** — *corrected*: positions/executions/signals **already** per-strategy; only `live_results`/`equity_curve` aggregate (= T2.18) |
| MS-7 | Backtest dispatch missing `TrendFollowingSlowStrategy` (live has it, bt has 0) | ❌ | S | No |
| MS-8 | Coordinator single `asset_class` → multi-asset bar stream + calendar policy | ❌ | L | No |
| MS-9 | CASH-equity leverage guardrail portfolio-wide → per-sleeve **(policy decision first)** | ❌ | M | No |
| MS-10 | Composite `run_id + "\|" + strategy_id` string encoding → real parameter | ❌ | S | No |

### 1f. Items that lived only in conversation

| ID | Description | Status | Size | Blocks |
|---|---|---|---|---|
| NEW-1 | `STICKY_DEBUG` still at INFO — `backtest_coordinator.cpp:1002` **and** `trend_following_fast.cpp:623` | ❌ | S | No |
| NEW-2 | Combined-id comment says `&`, code joins `_` — 13 orphaned `&`-keyed rows in DB | ❌ | S | No |
| NEW-3 | Editing `CMakeLists.txt` regenerates stale `MacOSX26.sdk` link paths → `fix_build` sed needed every time | ❌ | S | No |
| NEW-4 | Equity CSV export unimplemented — commission map is fetched then only logged | ❌ | M | No |
| NEW-5 | **Spinoff child-share receipt** — spinoffs are PRICE_RESTATING only, so the parent is value-preserved but child shares are never received. Merger rollover *is* implemented (`lifecycle.cpp:124-132`); spinoff never reaches it | ❌ | M | **No** (live-deploy prereq) |
| NEW-6 | T-OR.5 is a construction-drift tripwire, not runner-path parity | ❌ | M | Yes (= T2.12) |

---

## 2. Phase plan to "equities completely complete"

Standing testing rules apply to **every** phase: unit tests must catch real regressions
(not coverage theatre); every run reconciles **inline log output against every DB table
written**, with a per-action sanity check; runners strictly **sequential**; baseline
comparison wherever a baseline exists.

### Phase E1 — Pre-run correctness fixes (code only)
**Nothing may run live until these land** — E1 exists because the Tier-1 runs would
otherwise write to the wrong namespace and silently lose dividend value.

| Item | Why it's here |
|---|---|
| MS-1 | Live equity would pollute the futures portfolio namespace validated by the integration gate |
| F-1 + F-6 | Window widened to the price window (`historical_days`) **and** dedup state moved to the DB — safe only as a pair; a wide window with a losable state file re-applies everything |
| F-2 + F-3 | UTC-correct dates **and** one dividend denominator — F-2 masks F-3, so fixing either alone is worse than fixing both |
| T2.9 residue | `data_staleness_tolerance_days` into `config_template/` (Tier-2 → Tier-1 per revised framing) |

**Testing**: unit test per fix — a >14-day-gap catch-up applies all missed events exactly
once; a dropped state file does **not** re-apply (DB-backed dedup); date keys correct
under `TZ=America/New_York`; one denominator used by both halves; config key read and
overridable. Suite green.
**Acceptance**: all four land, suite green, no live/bt run performed yet.

### Phase E2 — Tier-1 E2E runs (the revised Tier-1 bar)
Sequential, each with log↔DB reconciliation over **every** table the runner writes.

1. `bt_equity_mr` E2E — exercises the per-bar adjustment on real data (**no equity runner has executed since the data layer was rewritten**)
2. `bt_equity_validation` → exit 0
3. `live_equity_mr` historical-replay E2E
4. **T2.7b** corp-action E2E over a window containing a real split/dividend, **including a simulated >14-day gap**
5. Backtest/live adjusted-price reconciliation for a common date (**F-7 anchor difference must be controlled, not tolerated**)
6. **T2.8** futures regression (`bt_portfolio_conservative` + `live_portfolio_conservative`) vs the integration-gate baseline
7. F6(05-22) `instrument_data_` scaling sanity-check during (1)/(3)

**Testing**: every run's inline output reconciled against `backtest.{results,equity_curve,final_positions,executions,signals,run_metadata}` and `trading.{positions,live_results,equity_curve,executions,signals}` as applicable; per-action sanity checks (row deltas, timestamps, key coverage); futures compared against the known-good baseline.
**Acceptance**: all seven pass with zero unexplained diffs; corp-action path proven to apply events after a gap.

### Phase E3 — Test backlog + correctness residue
| Item | Note |
|---|---|
| T-OR.2 / T-OR.3 / T-OR.4 | penny-stock lifecycle · non-trading-day carry-forward · position-flip cost-basis reset |
| NEW-6 / T2.12 | strengthen T-OR.5 to **true runner-path parity** (needs F-7 anchor control from E2) |
| T2.6 residue | `bt_equity_validation` reads `strategies_config` |
| F-4 | rename updates the tradeable universe, not just the position |
| F-5 | delete/fix latent `ON CONFLICT (run_id)` SQL |
| G6-4 / G6-5 | `corporate_action.date` TEXT-cast regression test; assert future-dated upper bound |
| NEW-1 / NEW-2 / NEW-3 | `STICKY_DEBUG` ×2 → DEBUG; `&`/`_` comment + orphaned rows; permanent CMake SDK fix |

**Testing**: each test must fail against the pre-fix behaviour (prove the tripwire).
**Acceptance**: suite green; T-OR.5 diffs real runner paths.

### Phase E4 — Live-deploy prerequisites
| Item | Note |
|---|---|
| NEW-5 | Spinoff child-share receipt — **code half now, activation data-blocked**; synthetic-row test like `RevivedFeedActivatesTheRolloverPathWithNoCodeChange` |
| F-8 | Explicit adjusted-frame ↔ broker-basis reconciliation rule (doc + test) |
| L2 | Recursive-mutex shutdown warnings |
| Data-owner | Revive `corporate_action` deal-terms feed + backfill 2025-08-29 → present (`DATA_OWNER_ASKS` item 3); **Sharadar-subscription question first** — if live, an ingest restart closes the whole class |

**Acceptance**: no code-side gap remains for any corp-action class; every remaining gap is explicitly data-blocked and logged loudly at runtime.

### Phase E5 — Multi-strategy proving (S1 + S3)
| Item | Note |
|---|---|
| MS-3, MS-7, MS-10, MS-2 | small correctness/parity fixes |
| MS-4 | a second equity strategy type — makes S1 a *meaningful* test rather than the same strategy twice |
| MS-5 | lift the live equity single-strategy limit |
| Runs P0–P3 | baseline → S1 backtest (overlapping symbols) → S1 live → S3 portfolio isolation |

**Testing invariants**: two rows for an overlapping symbol on the same date (no clobbering); Σ per-strategy positions == portfolio positions; Σ per-strategy PnL == portfolio PnL; margin/leverage computed on the combined book; portfolio isolation across `portfolio_id`.
**Acceptance**: S1 and S3 proven in backtest and live with those invariants held.

---

## 3. Deferred — tracked, explicitly NOT blocking "equities complete"

| Item | Reason |
|---|---|
| T2.18 / MS-6 per-strategy `live_results` + `equity_curve` | **HD decision**: missed by design, low priority. Per-strategy P&L is reconstructable from `positions` (already keyed by `strategy_name`) — the 05-22 doc's own Phase-7a workaround |
| T2.19 asset-class breakouts | Only meaningful once a mixed runner exists |
| T2.16 / MS-8 / MS-9 mixed futures+equity runner | Large; needs the CASH-guardrail **policy decision** first. Separate initiative |
| T2.10 / T2.13 / T2.14 / T2.15 REG_T chain | Conditional — CASH-only is a valid production stance (05-22 recommendation: defer) |
| T2.17 / T-OR.6 | Needs MS-4 + MS-5 (E5); lands with them or after |
| T2.21 P2/P3 tests (T-OR.7–10) | Scenario coverage, independent, piecemeal |
| T2.24 borrow-rates table | Only if a real broker connection becomes scope |
| T2.20 `weight_cache_` invalidation hook | Perf goal already met; correctness fine today |
| Phase 7c equity `contract_metadata` rows | JSON adequate at current scale |
| NEW-4 equity CSV export | Feature gap, not a correctness gap |
| G2-c `lastupdated` column | Nice-to-have simplification of the freshness guard |
| F3(05-22) DB-test env gate · F5(05-22) asset-type plumbing | Documented non-issues; no action required |
| D1: RiskManager runtime CASH-leverage guardrail · live EOD borrow-fee accrual | Deliberate deferrals — startup check covers the threat model |
| Broker reconciliation · partial-fill handling | Need a real broker adapter first |
| T2.23 `closeunadj` | **Closed** as superseded |

---

## 4. Summary

- **Blocking "equities complete"**: 4 items in E1, 7 runs in E2, 10 items in E3 → **E1–E3 is the critical path**.
- **Live-deploy prerequisites** (E4): 3 code items + 1 data-owner ask.
- **Multi-strategy proving** (E5): 6 items + 4 runs.
- **Deferred/tracked**: 18 items, each with a stated reason.
- **Not a single Tier-1 item is code-blocked today except the four E1 fixes** — everything else on the critical path is verification.

---

# VALIDATION PASS — 2026-08-31 (adversarial re-audit)

Read-only re-audit of everything above against live code/DB, under HD's new
**universe-scale directive**: `equities_data.ohlcv_1d` holds **852 symbols (700 active)**;
other/future strategies may hold any of them, so every fix must be correct and performant
at full-universe scale, not tuned to the 10 configured `equity_mr` names.

## V1. Stale premises found in the plan above

| # | Plan says | Reality (verified) | Action |
|---|---|---|---|
| V1-a | F-6 dedup state loss is a P2 risk ("losing it re-applies every event") | **Worse — loss is the default.** `resolve_corp_actions_state_dir` (`live_equity_mean_reversion.cpp:48-54`) falls back to `fs::current_path()/state/<strategy_id>`; in the container that is `/app/state`, and no volume is declared for it. Every container restart loses the file. | Raise F-6 to **P1**; E1 fix must also define a migration path from any existing file so the first post-fix run neither re-applies nor silently skips |
| V1-b | E1: "Window widened to the price window (`historical_days`)" | **Superseded in conversation.** Final design is *state-derived*: window start = earliest of (last-processed date, oldest held position's entry), floored at the price window, plus a **loud guard** that ERRORs when a held position predates the computed start. A fixed window is unsafe in principle — the live futures book already spans **459 days**. | Replace E1's F-1 description with the state-derived design |
| V1-c | `corporate_action` characterised only by row count | Table has **zero indexes** (`pg_indexes` on `equities_data` shows none) and stale planner stats (`n_live_tup=0` vs 627k actual). | Add to E1/E3 index work; ANALYZE the table |

## V2. Universe-scale findings — NEW, and two are severe

Measured with `EXPLAIN (ANALYZE, BUFFERS)` using an 852-symbol IN-list built exactly as the code builds it.

| # | Finding | Measured | Sev | Fix |
|---|---|---|---|---|
| **V2-1** | `get_per_bar_corporate_actions` uses `time::date BETWEEN` — the cast is **non-sargable**, so the `(symbol,time)` PK index cannot serve the range. Parallel Seq Scan over the 936 MB table, 4.57 M rows scanned to return 89. **Runs on every live run.** | **12,342 ms** → sargable rewrite `time >= 'D'::timestamptz AND time < ('D2'::date+1)` = **107 ms** | **P0-scale (115× )** | Rewrite to a half-open timestamptz range. **Note it also changes boundary semantics — couple with F-2's UTC fix** |
| **V2-2** | `get_delisting_dates` scans the whole table (`delisting_date IS NOT NULL` + `symbol IN`, no supporting index) | **11,297 ms** at 852 symbols | **P0-scale** | Partial index `ON ohlcv_1d(symbol) WHERE delisting_date IS NOT NULL`, or source from a small maintained table |
| **V2-3** | `build_equity_adjusted_query` filters on `time` only (no symbol filter path); PK is `(symbol,time)` so a time-only predicate can't use it → Parallel Seq Scan **plus external merge sorts spilling 22 MB + ~9 MB/worker to disk** | **33,633 ms** for 852 symbols × 2 years | **P1-scale** | Add a `(time)` or `(time,symbol)` index; raise `work_mem` for this query; prefer the symbol-filtered variant where the caller knows its universe |
| **V2-4** | `get_corporate_actions` (deal terms) seq-scans 627 k rows every call | 491 ms | P2 | Index `corporate_action(ticker, action)`; the `date` column is TEXT so also cast-aware |
| **V2-5** | Corp-action queries build **string IN-lists** (852 symbols = 5,287 chars) while the adjustment query correctly uses `= ANY($3)` array binding | works, inconsistent | P2 | Convert to array params — cheaper planning, no quoting surface |
| **V2-6** | `ticker_aliases` holds **16 rows**, but the frozen table contains **618 tickerchange rows involving our 852 symbols** (25,734 market-wide) | 38× gap | **P1** | **Backfillable today** from `corporate_action` (history is intact through 2025-08-29). Without it, rename handling at universe scale is untested and mostly inert |
| V2-7 | Spinoff/merger **confirm burden at 852 symbols** — the plan reasoned from the 10-name universe | **6–18 spinoffs + 0–1 mergers per year** (2020-2025, filtered to our 852) | info | Detect-and-confirm is still viable but is a **standing ~10–20 events/yr job**, not "single digits per decade" |
| V2-8 | `instrument_data_` is per-symbol keyed (`mean_reversion.cpp:61,96,209`) | linear, 852 entries | OK | No action — old F6 concern resolved by config-first universe |

## V3. New blockers (and two cleared)

| # | Item | Status |
|---|---|---|
| V3-1 | **E1 needs to CREATE a dedup table** — is that permitted? | ✅ **CLEARED** — connected as `postgres`, superuser, `has_schema_privilege('trading','CREATE')=true` |
| V3-2 | **Does the dead futures feed block E2's futures regression?** | ✅ **CLEARED** — the integration-gate baseline window is 2024-06-25 → 2026-06-25, entirely before the 2026-08-06 feed death. Regression is runnable now |
| V3-3 | F-6 fix needs an **existing-state migration path** (file → DB) or the first post-fix run mis-handles history | ❌ NEW — add to E1 |
| V3-4 | `work_mem` too small for the adjustment query (external merge on disk) | ❌ NEW — E1/E2 environment item |
| V3-5 | The V2 query fixes **change row-boundary semantics** at date edges, interacting with F-2's timezone fix | ❌ NEW — must be fixed and tested **together**, not sequentially |

## V4. Traceability gaps — items with NO home in the plan

| # | Item | Source | Why it matters |
|---|---|---|---|
| **V4-1** | **LEN / Millrose 2025-02-07 spinoff is unadjusted in BOTH per-bar factors and the vendor's `adjusted_close`** | `DB_AND_DATA_AUDIT_2026-08-27.md` | Our own adjustment inherits this hole — a known-bad data point that no test currently catches. Needs a data-quality assertion |
| **V4-2** | Symbol backfill gaps: ECHO, FERG, FLEX, HONA, MRVL, **PARA** | DB audit / `DATA_OWNER_ASKS` | Tracked as a data-owner ask but not as an equities-complete item |
| **V4-3** | `delisting_date` upkeep confirmation (latest value 2026-04-09 — "no delistings" vs "stopped updating" unresolved) | DB audit | Termination timing depends on it entirely |
| **V4-4** | Disposition of unused tables (`sp500_membership`, `ohlcv_1d_raw`, `coverage_gaps`, `verified_absent_bars`) and phase-out of `sharadar_ohlcv_1d` | DB audit / HD directive | Now documented in `docs/DATA_SOURCES_OF_TRUTH.md` — **cross-reference it from this plan** |
| **V4-5** | `adj_*` columns corrupt after 2026-08-05 | DB audit | Covered by `DATA_SOURCES_OF_TRUTH.md` DO-NOT-READ rule; not referenced here |
| V4-6 | T-OR.1 — 05-22 says "1/10 tests added" but never identifies which; plan lists T-OR.2/3/4/5 and 7–10 | 05-22 §Tests-to-add | Small traceability hole; confirm T-OR.1's identity/status |

## V5. Sequencing changes recommended

1. **Move the V2 query fixes (V2-1, V2-2, V2-3 index/work_mem) INTO E1.** They are not optimisations to discover during E2: at 12 s + 11 s + 34 s per invocation the E2 runs become painful, and worse, **V2-1's rewrite changes which rows land at date boundaries — the same semantics F-2 is fixing.** Fix and test as one unit with F-2/F-3.
2. **Add the `ticker_aliases` backfill (V2-6) to E1, before the live E2E.** Otherwise E2 item 3 and the F-4 rename work are exercised against 16 rows and prove nothing at universe scale.
3. **Add V4-1 (LEN/Millrose) as an E3 data-quality assertion** — a test that flags symbols where a corp-action row exists but neither `split_factor` nor `div_cash` moved.
4. E1 grows from 4 items to ~8; it remains the only code-blocked phase and the critical path is unchanged in shape.
5. Everything else in E1→E5 ordering stands.
