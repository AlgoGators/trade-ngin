# QT platform master document (2026-10-08) — rulings excerpt

The rulings below are the master document's, condensed; a struck line is one a later decision replaced, and each amendment names its source. HD's decisions of 2026-10-16 and Dominick's rulings of the same day are listed at the end: where they differ from a ruling above them, they stand. `qt-contract.md` records what is built.

## Shape
- ~~Engine (trade-ngin) and dashboard (AlgoLens) never call each other; they meet only in shared Postgres tables.~~
  **Amended 2026-10-09 (Dom), approved by HD 2026-10-16 (decision 8):** the two systems call each other. AlgoLens calls the engine's desk service over gRPC (`proto/algogators/desk.proto`: RunDesk, RequestOverride, RecordDecision, Publish, GetRunStatus; served by `engine-rpc`, `docs/design/rpc.md`) on the private Docker network `qt`. Postgres stays the record: the books, the command log (`position_overrides`), the settings and the publish record all live in the shared tables, and the engine answers by writing them.
- Three books per editable portfolio per day, same tables, told apart by one column (stream/book):
  - system: model's answer, built from yesterday's FINAL (qt) book. Written by model run.
  - qt_proposal: what the desk asked for. Seeded from system each day by model run; AlgoLens writes desk changes.
  - qt: what the engine gave back; what goes out and what tomorrow's model run starts from. Written by desk run.
- ~~Untouched model book = a second portfolio with desk editing switched off (replaces #55 benchmark/replay/run_inputs).~~
  **Replaced 2026-10-16 (decision 1):** two books, the model book (`CONSERVATIVE_PORTFOLIO`) and the QT book (`QT_CONSERVATIVE_PORTFOLIO`); the twin is dropped.

## Day
1. Model run stores system and seeds qt_proposal. No desk save -> qt = model result as-is (same positions, executions, costs); loop not rerun.
2. Desk changes quantities + reason, saves. Engine runs its ONE pass on the desk's whole book and stores qt, with per-symbol record of asked / given back / which step moved it. Save repeatable, sends nothing.
3. Override: either VP or President approves via link in email (recorded in AlgoLens). Request then trades exactly; risk/optimisation still computed and stored as report.
4. ~~Desk publishes every day, edited or not. Publish finalises desk's book, sends email, one record per portfolio+date (on live_run_metadata). Engine sends and writes.~~
   **Amended 2026-10-09 (Dom), approved by HD 2026-10-16 (decision 8; Sunday included, decision 6), the daily cutoff (`qt-contract.md` C7):** every calendar day, weekends and holidays included, America/New_York. The desk approves the day (the Publish command, "Approve" in AlgoLens) by 09:30: the approval freezes the day; the e-mail goes at 09:30 (an approval from 09:30 to 10:00 is sent at once). A day not approved by 10:00 is reset to the model's book, published (`system:fallback-10am`) and sent at once, and the President is alerted. From 10:00 the day is frozen. Still one publish record per portfolio+date on live_run_metadata (`publish_source`, `sent_at`, migration 027); the engine sends and writes.
5. Tomorrow's model run starts from published book.

## Rulings 2026-10-08 (HD)
1 Outside design draft loses where it conflicts; desk's exact choice does not trade without override.
2 Settings lane follows #60 design: file config base, one versioned row per desk change, merged at session start, settings used published back. Five-table rebuild is NOT the design.
3 Desk may turn any knob the config files hold. A knob that is not in config needs backend work to expose it there first.
4 Settings changes live-only; backtests keep file config.
5 Request and result stored separately as qt_proposal and qt.
6 Desk positions go through everything the model target goes through: per-name cap, overlay once, holds, search from held book, buffer, rounding, trim. Strategy not rerun. (Confirmed 2026-10-16, decisions 4 and 20: the buffer applies to every desk edit.)
7 Desk run works from positions not forecasts: sign close uses sign of desk's position; forecast-based deferral band does not apply.
8 Five-lap loop gone (#155). #87 entry point = second caller of the same overlay + one-pass code.
9 Desk book stored every day. No save -> qt takes model result as-is, loop not rerun. Save -> loop on whole desk book on every save. Each day's record says which.
10 Engine only reports distance request vs result per symbol with the step that caused it. The desk decides whether to ask for an override.
   **Amended 2026-10-16 (decision 5):** publishing is blocked only when the final book (qt) breaches a cap; every other check is reported, not a gate.
11 Either VP or President approves via email link.
12 No separate results table. Asked = qt_proposal row, given back = qt row in positions; step that moved symbol = one column on positions; run readings in qt row of live_results (detail column).
13 No equity desk editing in first release.
14 Edit before day's model run has seeded proposal is refused.
15 Desk run never sends daily email; publish does. Model CSV unchanged. The desk run's CSV leaves out the model's columns; a proper desk CSV is its own ticket.
16 Desk change not a standing override. Close stored as zero-quantity row.
17 Each book finalised into its own rows. Finalisation, P&L, loop stay in engine.
18 ~~No clock cutoff.~~ Publish every day; finalises, emails, one record per portfolio+date. Engine sends and writes.
   **Amended 2026-10-09 (Dom), approved by HD 2026-10-16 (decision 8):** there is a clock cutoff, every calendar day, New York time: approve by 09:30 (the approval freezes the day; the e-mail goes at 09:30, at once from 09:30 to 10:00); not approved by 10:00 -> the fallback resets qt to the MODEL's book, publishes it (`publish_source='fallback'`, `system:fallback-10am`) and sends it; after 10:00 approving, saving or overriding the day is refused. See `qt-contract.md` C7.
19 ~~Untouched model = second portfolio, desk editing off.~~ No benchmark tables / replay tool.
   **Replaced 2026-10-16 (decision 1):** the model book is `CONSERVATIVE_PORTFOLIO` itself; the twin (`QT_CONSERVATIVE_MODEL_PORTFOLIO`) is dropped (D-1).
20 AlgoLens opens on qt; portfolio switcher; related portfolios grouped; view flips system / qt_proposal / qt.
21 Symbol with no model data: held and counted in risk until data arrives; close at latest known price.
22 Position editing not live before desk run exists. Acknowledge-and-proceed gate dropped.
23 Dominick reviews; he is handed the final database state this document defines. All work on new_algo_data; cron still on algo_data. (2026-10-16, D-4: the old cron is retired once the model book runs daily on new_algo_data.)
24 Saved settings change that cannot be applied -> run refuses and alerts. Never falls back to file values.
25 QT desk does no dev work; doc is spec handed to reviewer (Dom).
26 One new table strategy_config (versioned, one active per portfolio, reason, author). No migrations table, publish table, run-settings table, desk results table. Publish record + settings used go on live_run_metadata. Ordered migration list in migrations README.
27 ~~No history copied into qt. First desk day starts from model book.~~
   **Replaced 2026-10-16 (decision 2):** the QT book starts from the model book: on its first day S it takes the model book's S-1 final positions and equity, and the model book's rows before S are copied into it.
28 Keep position_overrides (needs more cols). Drop risk_limits, portfolios, strategy_book_memberships, portfolio_assignments. Keep AlgoLens strategy_registry (+1 grouping column) and strategy_lifecycle_log.
29 A day whose run failed or was not published is caught up the way live does it today: the missed day is run and published before the next day's run. Until then nothing is sent and an alert fires.
   **Amended 2026-10-16 (decisions 6 and 8, the approved cutoff):** every day, weekends included, is published by 10:00 at the latest (the fallback, with the alert), so "nothing is sent" means a day is sent only once published. How the engine catches up a past day missed while it was down (published with the model's book, not e-mailed, one alert) and the chain rule are recorded as built in `qt-contract.md` C7; they are not rulings.
Also (10-07): one portfolio is one book; AlgoLens finds a book by portfolio id; strategy id is description, never key (#154).
Also: engine trading logic does not change for QT platform.

## Decisions of 2026-10-16 (HD)
Where one differs from a ruling above, it stands.
1 Two books, both run to the same standard: the model book (CONSERVATIVE_PORTFOLIO) and the QT book (QT_CONSERVATIVE_PORTFOLIO). The twin is dropped. Replaces 19.
2 The QT book starts from the model book: day S takes the model book's S-1 final positions and equity; the model book's rows before S are copied into it. From S each book runs from its own previous final book. Replaces 27.
3 The QT book runs the same engine as the model book, with the desk's settings and edits on top.
4 The whole loop, buffer included, applies to the desk's edits; a change the buffer holds back trades only with an override. Confirms 6.
5 Publishing is blocked only when the final book (qt) breaches a cap; every other check is reported. Amends 10.
6 Every calendar day, weekends included, waits for approval or the 10:00 fallback, which alerts the President.
7 The desk sees every setting except backtest ones; those it may not change are greyed out and locked.
8 Approved: the 09:30 approve and send, the 10:00 fallback to the model's book (C7), and the gRPC link. Replaces 18's "no clock cutoff" and the "never call each other" line.
9 Sunday waits for approval and the 10:00 fallback like any day.
10 An approved override cannot take the book over the margin cap (100% of equity); the cap is checked when the override is requested.
11 Desk, override and publish runs use the settings version the day's model run recorded; a change saved during the day takes effect the next day.
12 HD's team brings main and main-nonQT together; Dominick reviews.
13 Each investor follows either the model book or the QT book, set per investor; the QT book by default.
14 Kept as built: rescind before 09:30 (C8); the A -> B -> C flow (C10); screen access in place of roles; the new tables outside trading.
15 An override cannot trade a symbol the engine is holding (no price yet, or a roll pending); the page says why.
16 Dominick runs the production writes (the first-day copy and the D-38 fix) after HD has read each script.
17 A model re-run on an unpublished day puts the QT book back on the new proposal; the desk's edits are stored and the desk puts them back or drops them. A published day never changes.
18 Portfolios at the top; each has a Model book and a QT book; each book holds the strategies its config enables.
19 AlgoLens shows the portfolios chosen through the registry; BASE_PORTFOLIO is turned on.
20 The buffer applies to every desk edit; when it holds an edit back the desk can ask for an override, and is shown how close the edit came to crossing it.
21 Every override needs its own approval, covering only the request it was given for. Rescinding an override-published day uses the approval up; the day returns to the model's book.
22 A price that arrives after the run raises a message and the day is re-run; the re-run restates that day's numbers.

## Rulings 2026-10-16 (Dominick), on top of HD's decisions
- Decisions 17 and 22: a day settles with that day's positions and is updated the next day; a bad day is fixed by re-running the days in order.
- The VP or the President may approve a book over the gross, net or per-name caps. The margin cap (100% of equity) can never be overridden.
- The margin check at the override request uses the margin data in the database (no engine preview).
- Rescind: anyone on QT rescinds a day the desk published; only the VP or the President rescinds a day an override published.
- Weekend alerts and e-mails go out per day (no digest).
- The main / main-nonQT merge waits until the bugs are worked out.

## Final trading schema changes
- positions: allow qt_proposal book; one column for the step that moved a symbol
- executions: book column joins key; order id carries book
- live_results: book column joins key; desk run overlay readings in qt row detail column (detail column added by stage 3)
- live_run_metadata: published_by, published_at, full settings used
- equity_curve: none (already has book column)
- position_overrides: portfolio + date; override request + VP/President decision
- strategy_config: NEW
- strategy_registry (AlgoLens): +1 grouping column

## Known defects claimed (to corroborate)
- #55: finalising T-1 writes desk quantities into system; weekend/holiday carry-forward reads qt and saves as system; passwords stored in run_inputs; one desk row suppresses seeding for whole book; five fixes from main lost in new runner; if desk closes every position, same-day rerun seeds model book back; conflicts with main; Sonar red.
- #56: net leverage published as enforced but net-short book not scaled. (Now dropped entirely.)
- #60: no runner calls it; any override widens max_leverage 2.0->4.0 and max_drawdown 0.3->0.4 (#88); non-object override aborts run; {"risk": null} resets risk limits to compiled defaults; falls back to file config on failed lookup (must change per ruling 24).
- AlgoLens #80: branch cannot write a position against live schema; gate reads keys engine never publishes so every edit passes; 6 of 7 blockers fixed on preview branch.
- Gen 3 (continuation): finalising T-1 still saves desk quantities into system; desk run books desk quantities exactly with risk as report; approval = any two of five hard-coded names in app; investor books/multi-book mixed in; keyed on (portfolio, strategy_id); refuses on failed settings lookup (matches 24); fixes password leak; 44 new tables, 5 DB roles; migrations 002-020 collide; 014/015 refuse on stage-3 DB.
- Scheduler: killed wrapper leaves lock; later cycles run nothing and exit 0.
