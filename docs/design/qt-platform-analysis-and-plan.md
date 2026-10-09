# QT platform: code audit and build plan

2026-10-08 · prepared for Dom · checked against the master document of the same date ("QT platform", the rulings).

All repos were fetched on 2026-10-08. Local `main` in both trade-ngin and AlgoLens has diverged from origin (trade-ngin 58 ahead / 444 behind, AlgoLens 104 / 167), so nothing was merged locally; everything below is read from `origin/*`. Every finding was read from code, with file:line at the branch heads listed in §1. No GitHub writes were made.

---

## 1. What exists, by branch

| Repo | Branch (PR) | Head | Size | State |
|---|---|---|---|---|
| trade-ngin | main | 08b15c0 (09-05) | migrations 001–006 | 001 is the only QT migration |
| trade-ngin | stage3/class-a-shared … stage3/audit-fixes (#94–#99, #133–#137, #155, #158) | 69e6b0c (10-08) | 162 commits, 309 files, +61.8k/−9.4k | #94 has 0 reviews; #133+ are drafts |
| trade-ngin | feat/dual-portfolio-engine (#55) | e200ec8 (08-27) | +7.1k/−6.8k | conflicts with main (8 files) and stage3 (12 files); Sonar red |
| trade-ngin | feat/publish-risk-envelope (#56) | f2a6ea5 (08-27) | +332 | stacked on #55 |
| trade-ngin | feat/config-from-database (#60) | c9357b9 (08-27) | +1.0k | conflicts with main; 3 files against stage3 |
| trade-ngin | qt-platform-preview | c7f31fc (09-06) | — | ruled never to merge |
| trade-ngin | codex/qt-exact-choice-continuation → qt-platform-config-base (#152) → config-88-round-trip (#150) → config-88-49 (#151) | a982e42 → ed92ff7 → b47f1fa → be478d0 (10-06) | gen-3 alone: 545 files, +100.6k (≈46k prod, 52k tests) | drafts; Lint and Schema Guard fail, **Build and Test skipped**, so no C++ test has run in CI |
| trade-ngin | codex/config-foundation-trade-70aa276 | 70aa276 | — | an ancestor of #152 (17 behind), not a separate line |
| AlgoLens | main | c839014 (09-05) | — | — |
| AlgoLens | port/qt-position-edit (#80) | 3f7d9ea (08-28) | +2.1k/−0.3k | CI green; mergeable |
| AlgoLens | qt-platform-preview | 9f6730d (09-17) | 58 commits over main | ruled never to merge |
| AlgoLens | codex/qt-exact-choice-continuation ⊂ qt-platform-config-base (#101) → config-94-backend (#100) | a044ab3 → ffbcba8 → 55d1dca | #100 vs main: 608 files, +114.7k | drafts; backend tests fail |
| AlgoLens | feat/config-dashboard(-ui) (closed #33/#34) | 76d2a26 / db7fa508 | 11 files, +2.9k (half tests) | old pre-#70 layout; history was rewritten, so it ports by hand |

---

## 2. The master document, checked against the code

### Confirmed as written
- #55: finalising T-1 writes desk quantities into system (`live_portfolio_runner.cpp:1115` reads qt, `:1178` stores with the default `"system"`). The weekend/holiday carry-forward does the same (`:647-652` → `:1597`). Passwords go into run_inputs (`:1658`, no redaction). One desk row suppresses seeding for the whole book (`postgres_database.cpp:2276`, keyed per strategy and date, not per symbol). Five fixes from main are lost (hard-coded holidays path, 2-arg `get_trading_days`, unscoped `delete_stale_executions`, …).
- #56: a net-short book is never scaled (`risk_manager.cpp:694`, compares a signed value), yet the limit is published as "enforced".
- #60:
  - No runner calls the database loader. Every `ConfigLoader::load` call site on **every** branch is the 2-arg form; only the tests use the 3-arg one.
  - Any override widens leverage 2.0→4.0 and drawdown 0.3→0.4: `to_json` writes the limits at top level, `extract_config` reads them from `risk`, and the struct defaults win.
  - A non-object override throws inside `merge_json`.
  - `{"risk": null}` resets risk to the compiled defaults.
  - There are four silent fall-backs to file config, against ruling 24.
- AlgoLens #80:
  - It cannot write a row: four columns are NOT NULL with no default.
  - Its gate reads `max_symbol_notional`, `max_gross_notional` and `max_position_count`, which the engine never publishes, so every edit passes.
- Gen-3:
  - It still writes desk quantities into system at T-1 (`book_tail.cpp:289-292` reads qt; `:362-366` stores with no stream, so the default applies). The weekend carry-forward has the same flaw.
  - It books the desk's quantities exactly. The risk run is a report only, and any quantity change is *rejected* (`qt_evaluation.cpp:256-262`).
  - It is keyed on (portfolio, strategy_id).
  - It refuses when the settings lookup fails, which matches ruling 24.
  - It fixes the password leak.
  - It defines five database roles.
  - Its migrations 014 and 015 refuse on a database with stage 3 applied.

### Corrections to the master document
| The document says | The code shows |
|---|---|
| Six of #80's seven blockers are fixed on preview | **Five.** Item 5 (`json.dumps` without `default=str`, `repositories.py:859`) is not fixed. Item 7 is half fixed: `last_update` is set, but the date is still `_date.today()`. Preview also still accepts a typed average price and keeps acknowledge-risk. |
| ~60 commits above #56 | True of the continuation branch. #152's head is **106** above #56: the continuation plus 46 more commits. |
| Approval is any two of five names | **Two of three** hard-coded approvers (`qt_desk_processor.cpp:130,143`; AlgoLens `capabilities.py:50`). AlgoLens migration 003 names four people in a CHECK constraint. |
| 44 new tables | We count **47**: 27 in engine migrations 009–031 and 20 in AlgoLens migrations 003–011 plus the deployment SQL. |
| First number free everywhere is 032 | That only matters if gen-3 is kept. Main uses 001–006 and stage 3 uses 012–020, so **021** is free on stage 3. Stage 3 has no migrations README yet. |
| #60's caller: "a local patch exists, never committed" | Not on any branch. It might exist in someone's working copy. |
| If the desk closes everything, a same-day rerun reseeds the model book | **Partial.** It only happens if a close deletes rows. Under ruling 16 a close is a zero-quantity row, and that row blocks the reseed. |

### New findings, not in the master document
1. **Stage 3's positions write is book-blind.** `store_positions_in` deletes by strategy, portfolio and date with no `portfolio_type` (`postgres_database.cpp:603-607@69e6b0c`). The insert never names the book (`:693`), and `load_positions_by_date` has no book filter (`:1001-1027`). **On stage 3, any write to a second book wipes the other one.** This is the first thing to fix.
2. **Executions would collide between books.** Order ids are `PM-<sid>-<n>` (`portfolio_manager.cpp:2546`), and stale rows are deleted by order id.
3. **live_results has no book column.** Its key is (portfolio_id, strategy_id, date). Stage 3's `risk_detail` pins nine keys in a test, so adding desk readings means extending that test.
4. **#60's limit asymmetry also exists on stage 3** (`config_loader.hpp:406-407`, `cpp:394-398@69e6b0c`). It has to be fixed on stage 3 whatever else happens.
5. **The CHECK on `portfolio_type` allows only `system` and `qt`.** `qt_proposal` needs a migration before anything can write it.
6. **#55's `position_overrides` has an `UPDATE … DO INSTEAD NOTHING` rule**, so an approval cannot be recorded by an update. Either make every decision an insert or drop the rule.
7. **The AlgoLens positions read on main applies `quantity != 0` before `DISTINCT ON (symbol)`** (`repositories.py:130-147`). A zero-quantity close row (ruling 16) is skipped and the older non-zero row shows instead, so a flatten looks like it did nothing. (Separate from #83, the missing book filter.)
8. **Two one-pass catches for the desk caller:**
   - A symbol is "free" only if `signalling[i]` is set, so a desk-only symbol would become a closeout row unless the desk caller sets signalling for every row.
   - `require_loop_keys` demands `sign_close_band > 0` (`config_loader.cpp:598`), so the deferral band must be switched off in memory for the desk run, not in config.
9. **AlgoLens gen-3 migration 009 rewrites real accounts.** It looks people up by e-mail (yours included), sets your account's role to `exec_board`, and retires your personal account into `auth.account_retirements`. **It must never be applied.**
10. **Hygiene:** gen-3 fixtures (`__fixtures__/equityActionActual/case-index.json`) commit one developer's local `/mnt/c/Users/...` paths.

### Questions the code raises that the rulings do not answer
The document says nothing is open. These five came out of the code:

| # | Question | Why it matters | Suggested answer |
|---|---|---|---|
| Q1 | **How does a save reach the engine?** The two never call each other, and ruling 26 allows no job table. | Without this, a save never runs the desk loop, and a publish or an approval is never seen. | Use `position_overrides` as both the log and the queue: one row per event (save, override request, decision, publish) with a status. A desk worker on the engine host polls it. No new table. |
| Q2 | **How is the desk book split back to sleeves?** Positions are keyed per sleeve (`strategy_name`), but the desk edits a portfolio total. One-pass splits by the sleeves' contributions, and a desk total has none. | Per-sleeve rows and P&L for qt. | Split by the model's sleeve proportions for that symbol that day; fall back to held proportions; give a new desk-only symbol to a "desk" sleeve. |
| Q3 | **When is qt written on a day with no save?** | Ruling 9 says qt takes the model's result. | The model run writes qt as a copy of system, marked `source=model`. A save overwrites it (`source=desk`) and an approved override overwrites it (`source=override`). Publish only finalises. |
| Q4 | **Base branch while stage 3 is unmerged.** Fourteen stacked PRs; the bottom one has no review. | Every QT PR built on the stage-3 tip gets rebased as the stack moves. | Build on `stage3/audit-fixes`. Start with the parts that touch stage-3 code least (storage, migrations, config), and hold the runner-tail extraction until #155 is reviewed. |
| Q5 | **Who builds it.** The 10-06 ruling says John Riley cuts reviewable PRs after stage 3, and ruling 25 assigns no builder. | You plan to build it with AI. | Agree with HD and John before cutting code, and close #55, #56, #152, #150, #151, #101 and #100 as superseded, with John's sign-off. |

---

## 3. Start from scratch or build on what exists?

**Engine: start fresh on stage 3, porting about 1k lines by hand. AlgoLens: build on #80.** In both cases the rulings, the reviews and the reproductions are worth more than the code.

| Body of work | Worth keeping | Why the rest goes |
|---|---|---|
| #55 (~7k lines) | ~450 lines, re-typed: the book-scoped DELETE/INSERT/load predicates and the `column_exists` pattern, the race-free seed statement (re-keyed per symbol, onto qt_proposal), and the shape of `position_overrides` | Its runner is forked from pre-equities code. Stage 3 grew each live app by ~1,340 lines, so a rebase is a rewrite. Benchmark, replay and run_inputs (~2.3k lines) are dropped by ruling 19. Both of its book-writing paths write the wrong book. |
| #56 | nothing | Ruled out. |
| #60 (~1k lines) | the `strategy_config` DDL as-is (~25 lines), ~120 lines of overlay, ~250 lines of tests with the fallback assertions reversed | Drop `config_manifest`: settings used go on `live_run_metadata`. |
| Gen-3 engine (~100k lines) | ~150 lines: the credential-free settings snapshot and secret scan (`live_config_override.cpp:23-33, 253-276`), the parse–rebuild–compare guard (`:291-292`), the refuse-on-lookup pattern (`postgres_live_config.cpp:61-112`), and `test_live_config_override.cpp:35-72` as regression specs | Its core design is the opposite of rulings 1, 6, 8, 11, 12 and 26. It still corrupts system at T-1, and it has never built in CI. One commit, 49bd6be4, mixes +81k lines across 397 files, so nothing cherry-picks. |
| AlgoLens #80 + preview | ~1.5k lines: the write path, symbol normalisation, the market-data price lookup, the Tailwind fix, and the Postgres integration test fixture (the one that catches every blocker) | Its gate, acknowledge-risk and typed price all go. |
| AlgoLens gen-3 (~115k lines) | UI pieces to adapt: `PortfolioGrouping.tsx` (point it at `strategy_registry`, not `portfolios`), `OverrideHistory.tsx`, `QtSelectionTable.tsx`, gen-3 `EditPositionModal.tsx` and its tests, `capability_guard.py` | Decision tables, approvals, investor books, governed settings and migration 009 all go. |
| Closed #33/#34 settings editor (~1.2k production lines) | nearly all of it: it is already a versioned `strategy_config`, one active row, reason and author, a new version on revert, Running vs Pending | Point it at `live_run_metadata` instead of `config_manifest`. |

So the engine is a rebuild with a borrowed specification, test cases and about 1k lines, and AlgoLens is a rebase and re-point. Starting from a blank page in AlgoLens would throw away a write path and test fixture that already work.

---

## 3b. Decisions taken 2026-10-08 (Dom)

The master document remains the source of truth. These decisions fill its gaps, and one of them amends it.

- **Builders.** Dom and Claude only. JR and HD are not involved for now. Our own PRs are merged with admin rights.
- **Stage 3 is on main.** #94–#99, #133–#137, #155 and #158 were admin-merged in order on 2026-10-08, and main is now byte-identical to `stage3/audit-fixes`. The Dockerfile still patched a test file that stage 3 deleted, which broke Image Generation; #159 fixes it. trade-ngin deploys only from `prod`, so the live runner is unaffected. **This answers Q4.**
- **Co-location.** AlgoLens moves onto the trade-ngin host (ubuntu@ec2-18-118-225-224) and runs in Docker beside the engine. nginx on the old host keeps TLS for `algolens.algogators.com` and proxies to the new host's private IP, the same way it already serves airflow. Vault, Dex and monitoring stay on the old host.
- **gRPC command channel (amends "never call each other").** This answers Q1.
  - Postgres stays the record: all three books, the audit trail, settings and run metadata.
  - gRPC carries commands only. AlgoLens calls the engine to run the desk loop, request an override e-mail, publish, and read run status.
  - Every command is idempotent and keyed on its `position_overrides` row. AlgoLens writes the row first, then calls. On startup the engine re-drives any row still pending, so a dropped call loses nothing.
  - The contract is `trade-ngin/proto/qt/v1/desk.proto`, owned by the engine. AlgoLens vendors the generated Python stubs at a pinned commit.
  - The server is `desk-agent`, a small Python `grpcio` service shipped **inside the trade-ngin image**. It shells out to the C++ binary in `--desk` and `--publish` modes, so there is one image with two entrypoints: the cron runner and the agent.
  - Transport is plaintext on a private Docker network (`qt`) shared by the `desk-agent` and `algolens-backend` containers. No host port is published.
  - The services:
    ```
    service DeskService {
      rpc RunDesk(RunDeskRequest) returns (RunDeskReply);           // save -> one pass -> qt; per-symbol asked/given/step
      rpc RequestOverride(OverrideRequest) returns (CommandReply);  // e-mails VP + President a signed one-time link
      rpc RecordDecision(DecisionRequest) returns (CommandReply);   // link click recorded by AlgoLens -> engine books override
      rpc Publish(PublishRequest) returns (CommandReply);           // finalise, e-mail, CSV, published_by/at
      rpc GetRunStatus(RunStatusRequest) returns (RunStatus);       // model run state for the desk pages (AlgoLens #97)
    }
    ```
- **Q2, the sleeve split.** Split the desk's total for a symbol by the model's (system) sleeve proportions for that symbol that day.
  - If system holds none, use yesterday's qt proportions.
  - If neither exists, give it to the first sleeve, alphabetically, whose configured universe contains the symbol.
  - If none does, refuse the save and name the symbol.
- **Q3, the qt book on a day with no save.** The model run writes qt as a copy of system, marked `source=model`. A save overwrites it with `source=desk`, an approved override with `source=override`. Publish finalises whichever is there.
- **Q5, who builds.** Answered by the first bullet.

### Hosts (inspected 2026-10-08)
- **Old host:** ec2-user@ec2-18-226-98-126, Amazon Linux 2023, 1 vCPU, 949 MB RAM, disk 73% full.
  - It runs nginx and TLS for algolens, vault, monitoring and airflow, plus Vault, Dex, Grafana, Prometheus, Loki and AlgoLens.
  - **The live AlgoLens API is not the container.** nginx sends `/auth` and `/portfolio` to a bare `python3 backend/app.py`, started 09-05 from an old pre-#70 checkout whose local main has diverged from origin. The backend container on port 5000 is not routed at all.
  - The frontend container dates from 2026-02-04.
  - **Deploys always fail:**
    - The GitHub deploy workflow and a cron job (`* * * * * deploy-algolens.sh`) both run `git pull`, which fails on the divergence.
    - The cron log is 130 MB.
    - The last GitHub deploy run was 09-05, and it failed.
- **Target host:** ubuntu@ec2-18-118-225-224. trade-ngin is deployed there by `ssh-action` with `docker compose up -d` in `/home/ubuntu/docker-compose/trade-ngin`, from the `prod` branch only. It needs `Patrick.pem`.
- **Airflow:** a third instance (172.31.23.190 private), proxied by the old host's nginx with Dex sign-in (data-ngin #124). Its port 8080 is also open on the public IP over plain HTTP, and should be closed.

### What AlgoLens takes from data-ngin and trade-ngin CI/CD
- Build images in CI and push them to GHCR.
- Deploy by `ssh-action`: `docker compose pull && up -d` on the host. No `git pull` on the server, ever.
- Run integration tests against a Postgres service container in CI.
- A schema-ownership guard: AlgoLens owns `strategy_registry` and the lifecycle log; trade-ngin owns the trading tables.
- Dex OIDC sign-in, as Airflow does, for the sign-in work that has no issue yet.

## 4. The plan

### How each PR gets built and reviewed
Each step below is one PR, under ~800 lines of diff, stacked on a `qt/base` branch cut from `stage3/audit-fixes` (engine) or `main` (AlgoLens). Each one goes through the same loop, with you approving every gate:

1. **Spec slice.** Claude writes a half-page spec: the rulings it implements, the files it touches, and what it refuses to do. *You approve it.*
2. **Failing tests first.** Claude writes the tests, using the reproductions in `qt-review-codex/repro` and the reviews where they exist. *You review the tests.* This is where the intent lives, and it is the most valuable review you do.
3. **Implementation.** Claude implements until the tests pass, in an isolated worktree.
4. **Machine review.** `/code-review high` on the diff. Claude fixes or rebuts each finding in writing.
5. **Your review.** You read the diff and run the build. For anything that touches the runner, also run a **byte-identical A/B**: the same model day before and after the change produces identical system rows. Stage 3 already does this in #94. That run is the proof that "the engine's trading logic does not change".
6. **Ledger.** One line per PR in `docs/design/qt-ledger.md`: the rulings covered, the tests, and who approved.

Anything that changes what the engine computes stops and goes to HD.

### Phase 0: agreements, no code
- Get answers to Q1–Q5 (above) from HD.
- Agree with John Riley which PRs close.
- Cut `qt/base` in both repos.
- Add `migrations/README.md` on stage 3 with the ordered list (ruling 26).

### Phase 1: foundations, which can run in parallel
| PR | Repo | What | Depends on |
|---|---|---|---|
| E1 | trade-ngin | **Migration 021: book columns.**<br>• Widen the CHECK to `qt_proposal`.<br>• Add a book column to executions and live_results and put it in their keys.<br>• Put the book in the order id.<br>• Add a step column to positions.<br>• Make `store_positions_in`, `load_positions_by_date`, executions and live_results reads and writes book-scoped, refusing when the column is missing.<br>Tests: writing one book leaves the other byte-identical. | stage 3 |
| E2 | trade-ngin | **Migration 022 `strategy_config` and the overlay (the #60 port):**<br>• Fix the `to_json` limit asymmetry on stage 3.<br>• Overlay in `ConfigLoader::load` before `extract_config`.<br>• Reject null and non-object overrides.<br>• Run validation on the merged config.<br>• Refuse and alert on every failure (ruling 24).<br>• Live runners only (ruling 4).<br>• Write the credential-free settings used to `live_run_metadata` (gen-3 snapshot code). | stage 3 |
| A1 | AlgoLens | Positions read filtered by book and date, with the zero-quantity row bug fixed (#83 and finding 7). Can merge to main on its own. | — |
| A2 | AlgoLens | Reads keyed by portfolio id (#102, Pranav's; coordinate), one grouping column on `strategy_registry`, a portfolio switcher, a book toggle (system / qt_proposal / qt), opening on qt. | A1 |

### Phase 2: the model run writes three books
| PR | What |
|---|---|
| E3 | **The model run starts from yesterday's qt.**<br>• Each book is finalised into its own rows (ruling 17). This fixes the T-1 bug in both #55 and gen-3.<br>• The weekend carry-forward stays within the book it read.<br>• On the first desk day, start from system (ruling 27). After that, a missing qt for yesterday refuses the run.<br>• Seed qt_proposal per symbol.<br>• Write qt as a copy of system (positions, executions, costs, live_results, equity), marked `source=model` (Q3).<br>The A/B check: system must not change. |
| E4 | **Extract the runner tail** (`live_portfolio.cpp:1825-4206` and its conservative twin) into one function taking (book, target positions, previous book, prices). It is a pure refactor, so the A/B check must be byte-identical. **Hold this until #155 is reviewed**: it is the PR most exposed to stage-3 churn. |

### Phase 3: the desk run
| PR | What |
|---|---|
| E5 | **The second caller of one-pass.**<br>• Factor the input builder out of `rebalance_one_pass` (`portfolio_manager.cpp:2120-2307`).<br>• Add `rebalance_desk(desk_targets, …)`: set `target` to the desk book, set signalling for every row, set `sign_band = 0` in memory.<br>• Split to sleeves per Q2.<br>• Add a pure `one_pass::attribute(in, out)` that names the step that moved each symbol, built from `cap_bound`, `sign_closed`, `clipped`, `trimmed`, `by_hold`, etc.<br>• Extend `risk_detail` with the desk readings. |
| E6 | **The desk worker** (Q1). It polls `position_overrides` for save events and, for each one:<br>• runs E5 on the whole qt_proposal,<br>• passes the result through the E4 tail with book = qt, writing qt and the step column, `source=desk`.<br>It refuses when no proposal has been seeded (ruling 14) and never e-mails (ruling 15). An approved override books qt_proposal exactly, with the risk run stored as a report. |
| A3 | **#80 rebased and re-pointed.**<br>• Lift the five preview fixes and the Postgres fixture.<br>• Write qt_proposal, with a reason.<br>• Refuse with 409 before the proposal is seeded.<br>• Quantities only; the price comes from market data; zero means flatten; futures books only (ruling 13).<br>• Symbol picker from `contract_metadata`.<br>• Delete the gate and acknowledge-risk.<br>• Each save writes an audit and queue row.<br>Behind a flag until E6 is live (ruling 22). |
| A4 | The asked-vs-returned view: qt_proposal against qt, per symbol, with the step column. |

### Phase 4: override, publish, settings
| PR | What |
|---|---|
| A5 / E7 | **Override.**<br>• The desk requests it in AlgoLens.<br>• The engine e-mails the VP and the President a signed, single-use link. The e-mail shows the model's book next to the desk's request.<br>• Either one approves, and AlgoLens records the decision as an insert.<br>• The requester cannot approve their own request.<br>• The worker then books the override. |
| A6 / E8 | **Publish.**<br>• The desk presses publish, and a row is queued.<br>• The engine finalises the book, sends the e-mail and the CSV (moved out of the model run), and writes `published_by` and `published_at` on `live_run_metadata`.<br>• A missed day is caught up first, and an alert fires until it is (ruling 29). |
| A7 | **Settings editor** ported from #33/#34 onto `strategy_config`, reading the settings used from `live_run_metadata`. Gated by roles (#94, John's). |
| D1 | **Database clean-up** on new_algo_data (ruling 28).<br>• Drop `risk_limits`, `portfolios`, `strategy_book_memberships` and `portfolio_assignments`.<br>• Order: stage-3 migrations, then 021 and 022, then the clean-up.<br>• Record each step in the README. |

### Rough size
- Engine: ~4–6k lines including tests. E4 and E5 are the hard ones.
- AlgoLens: ~5–6k lines including tests.
- With AI writing and you reviewing every step, the limit is your review time and stage 3's merge, not typing.

### What to do this week
1. Send HD Q1–Q5, and the warning about migration 009.
2. Agree with John which PRs close.
3. Start E2 and A1. Neither depends on the open questions; both touch little stage-3 code; and A1 can merge to main right away.
