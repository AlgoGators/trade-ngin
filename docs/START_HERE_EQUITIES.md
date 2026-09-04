# START HERE — equities work: state, evidence, and where everything lives

**Purpose**: cold-start briefing. If you are auditing, reviewing, or continuing this work,
read this first. Everything below is committed; nothing important lives only in a chat log.

Last updated: 2026-08-31.

---

## 1. Current state in one screen

| | |
|---|---|
| Branch | `equities_integration` (30 local commits ahead of origin, **nothing pushed** — deliberate) |
| Unit suite | **1425/1425 green** |
| Phase | **E1 complete**; E2 (end-to-end runs) not started |
| Working tree | clean |
| Host | EDT (−0400). Relevant: two `mktime` sites shift dates only on POSITIVE-offset hosts, so a date anomaly here is a real finding, not a timezone artefact |

**What is proven:** unit suite; backtest head-to-head identical across 369 days; futures live
behaviour byte-identical (real run, two binaries, same date); equity adjustment matches the
vendor to 9.489e-13.

**What is NOT proven:** the equity live path has **never executed**. Corporate actions have
never been applied to a live position, by this code or its predecessor. That is E2's job.

---

## 2. Read in this order

| # | Document | What it gives you | Read when |
|---|---|---|---|
| 1 | `CHANGE_LEDGER_2026-08-31.md` | **Every change made**, what it fixed, whether the defect was pre-existing or introduced by our own earlier work, impact size, affected paths | Always. This is the spine |
| 2 | `DATA_SOURCES_OF_TRUTH.md` | Which table/column to read for what, which look authoritative but are poison, the corp-action picture per effect class, required DB objects | Before touching any data code |
| 3 | `EQUITIES_COMPLETION_PLAN_2026-08-31.md` | The E1–E5 plan, every open item with status, the deferred list with reasons, E1's closing record | For scope questions |
| 4 | `EQUITIES_REVIEW_NOTES_2026-05-22.md` | The original 3-tier definition of done + T2.1–T2.24. **Historical**: several sections are now stale (see §5 below) | For original intent |
| 5 | `TIMEZONE_AND_DEDUP_SWEEP_2026-08-31.md` | Complete 164-site timezone ledger + the dedup chain verified link by link | For timezone or dedup questions |
| 6 | `EQUITIES_BRANCH_AUDIT_2026-08-31.md` | F-1…F-8 findings and the revised tier framing | For "why was this done" |
| 7 | `EQUITIES_ASSUMPTION_AUDIT_2026-08-31.md` | The consumer-assumes-what-producer-doesn't-guarantee sweep | Same |
| 8 | `MULTI_STRATEGY_CAPABILITY_2026-08-31.md` | S1–S4 scenarios, what actually works today, the proving plan | For multi-strategy questions |
| 9 | `CORP_ACTIONS_DATA_BOUNDARY.md` | What is trustworthy over which date ranges | For corp-action questions |
| 10 | `DB_AND_DATA_AUDIT_2026-08-27.md` | The 5-round DB audit with raw SQL evidence | For data-state questions |
| 11 | `INTEGRATION_VALIDATION_2026-08-30.md` | The main-branch before/after run comparison and its method | For regression methodology |
| 12 | `DATA_OWNER_ASKS_2026-08.md` | What is blocked on the data team | For blocked items |

---

## 3. Facts that repeatedly surprised people

1. **`equities_data.corporate_action` is FROZEN at 2025-08-29** — needed, not deprecated.
   Deal terms (merger/spinoff ratios) have no other source. Code must work with zero rows
   returned and light up unchanged when rows appear.
2. **The vendor's `adj_*` columns are stale after 2026-08-05.** We never read them; we
   compute adjustment from raw + `div_cash`/`split_factor`. Do not "fix" code by switching
   back to them.
3. **A renamed symbol's ENTIRE history migrates to the new ticker** (AA→HWM carries 67
   pre-rename dividends). This is why dedup must bridge renames.
4. **33 tickers have two or more successors** over time (BBT→BBT1 1998, BBT→TFC 2019, and
   BBT is in use by a third company today). Rename resolution must be per event date.
5. **Live DOES attribute per strategy** in `positions`/`executions`/`signals` (via
   `strategy_name`, which is in the PK). Only `live_results`/`equity_curve` aggregate.
   An earlier audit got this wrong.
6. **The futures feed is dead since 2026-08-06**; equities is current through 2026-08-27.

---

## 4. Verification practice that this effort learned the hard way

Five defects were introduced by our own earlier fixes. Each was caught by the next layer.
The patterns worth carrying forward:

- **A green test proves nothing if the producing path never yields that input.** The
  `last_update` window fix passed its tests while being a total no-op in production,
  because the real query made that column constant by construction.
- **Mocks can mask the very bug they were written to catch.** Happened twice here.
- **"No file was edited" is not "no behaviour changed"** when a shared function's body
  moved. Prove it by execution.
- **DB-dependent tests that skip are not tests.** They report green having run nothing.
- **Verify against the database, not against the test fixture.**

---

## 5. Where `EQUITIES_REVIEW_NOTES_2026-05-22.md` is now WRONG

Its §B (how corporate actions work) is premised on `closeadj` and the 14-day
`corporate_action` query — both replaced. F4 (migration not run) → applied. F7 (268 days
stale) → data current. L1 → resolved. §G2/G3 predate the schema reshape. Several line-number
citations have drifted. Treat it as intent, not as current fact.

---

## 6. Open items going into E2

**Known-unproven (this is what E2 tests):**
- Corp-action live path has never executed. **The first equity run exercises none of it** —
  the book is empty, so `previous_positions` is empty and the block is skipped by its guard.
  **Two sequential runs are required.**
- The close top-up path needs a holding older than 730 days; no fresh run reaches it. Seed
  synthetic old positions to exercise it.
- Window derivation is production-observable only via its log line, which now reports its
  rule ("derived from inception of X" vs "14-day floor").

**Deferred, with reasons (do not fold into equities):**
- `order_id` localtime → futures track. It is a cross-run match key for
  `delete_stale_executions`; a naive UTC swap breaks futures stale-execution cleanup.
- Two `mktime` sites → E3. Correct on this host and all negative-offset zones.
- CI has no postgres service, so DB-dependent tests skip there → **MAIN-branch batch-3
  track**, not equities.
- Position/dedup atomicity is fixed for equities; the futures live path has the same latent
  exposure between its position and results writes.

---

## 7. Rules in force

- **Local commits only. Never push without explicit approval.**
- Runners strictly sequential — they share DB tables and singleton state.
- Every run verification reconciles inline logs against EVERY table written, with per-action
  sanity checks. Unit tests alone are not acceptable evidence.
- Snapshot before any replay: replays upsert over the rows you are comparing against.
- Do not disturb the working futures pipeline to tidy up equities work.
