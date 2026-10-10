# How rolls work

What the engine does when a futures position passes from one contract to the next: which price
series each computation reads, how a contract switch is detected, what is held, what is booked and
what is stored. It applies to both futures books (CONSERVATIVE and BASE), in the backtest and in the
live runners, which share the code cited here.

Companion documents: [DATA_SOURCES_OF_TRUTH.md](DATA_SOURCES_OF_TRUTH.md) (the futures tables),
[COST_MODEL.md](COST_MODEL.md) (how a fill is priced, netting),
[LIVE_RUN_CYCLE.md](LIVE_RUN_CYCLE.md) (the run's date frame and the stored columns),
[OPTIMIZER_AND_RISK_DESIGN.md](OPTIMIZER_AND_RISK_DESIGN.md) (the hold set inside the rebalance),
[TREND_FOLLOWING_SYSTEM.md](TREND_FOLLOWING_SYSTEM.md) (the strategy end to end).

The design comments that carry the detail are in the headers:
`include/trade_ngin/data/roll_series.hpp:13-46` (change bars and the adjusted series),
`include/trade_ngin/data/roll_series.hpp:132-143` (the legs),
`include/trade_ngin/live/live_roll_legs.hpp:19-46` (which rolls a live run books),
`include/trade_ngin/data/session_classifier.hpp:120-130` (which bars are consumed),
`include/trade_ngin/data/listing_dates.hpp:48-53` and `:108-117` (relabels and listing dates).

---

## 1. The vendor series is a splice

Every futures symbol is a continuous symbol `<root>.v.0` (for example `MBT.v.0`). `.v.0` is the
vendor's resolver: on each day it names one real contract, the one ranked first by the previous
day's volume, and the day's bar is that contract's own bar at its own raw prices. Nothing is
adjusted, in the vendor's data or in the database.

| table | what it holds | key |
|---|---|---|
| `futures_data.ohlcv_1d` | the bars the engine reads: `time, symbol, open, high, low, close, volume` | none (the loader keeps one row per symbol and date) |
| `futures_data.ohlcv_1d_raw` | the vendor record of the same bars, with `instrument_id`, the contract the bar belongs to | `(ts_event, symbol)` |

The raw table never holds two contracts of one symbol on one date. So on the day the resolver
switches contract, the previous bar is the old contract's close and the day's bar is the new
contract's close, and the old contract's close on the switch day is stored nowhere.

The step at a switch is therefore

    close_new(d) - close_old(d-1) = [close_new(d) - close_new(d-1)] + [close_new(d-1) - close_old(d-1)]

The first bracket is a one-day move of the new contract. The second is the price difference between
two contracts on the same day, which no holder earns: a position worth its old-contract price before
the roll is worth its new-contract price after it, and the account changes only by the cost of the
two fills. Neither bracket is observable here (only the sum is), so the engine treats the whole step
as the gap. Robert Carver, Advanced Futures Trading Strategies, strategy one, section
"Back-adjusting futures price" (with Appendix B), makes the same point about stitched prices: a
roll at market prices creates no profit or loss, so a series built for returns has to remove the
difference between the two contracts.

## 2. Two series: adjusted for returns, raw for levels

The loader attaches the vendor contract id to every kept bar (`Bar::instrument_id`,
`include/trade_ngin/core/types.hpp:326`), joined from the raw table on the bar's own print: symbol,
instant and all five of open, high, low, close and volume (`build_futures_bar_query`,
`src/data/market_data_utils.cpp:111`). A kept bar whose print the raw table lacks has no id.

From each symbol's consumed bars the engine builds, on every run and never stored
(`build_series`, `src/data/roll_series.cpp:109`):

- the **raw close**, as loaded;
- the **adjusted level**: the raw close plus the sum of the steps (new minus old) on every later
  change bar, so it equals the raw close on the latest bar and differs from it by a constant inside
  each contract stretch (`adjusted_levels`, `src/data/roll_series.cpp:75`). It can be zero or
  negative and is only ever differenced;
- the **adjusted return**: the adjusted change over the raw previous close,
  `r_t = (A_t - A_t-1) / P_t-1`. It is exactly 0 on a change bar and equals the raw simple return on
  every other bar (`adjusted_returns`, `src/data/roll_series.cpp:89`). The date stays in every
  window as a zero; nothing is dropped.

Rule: anything that is a price change reads the adjusted series; anything that is a price level reads
the raw close of the contract held.

| consumer | reads | where |
|---|---|---|
| the EMAs of the trend rules | adjusted level (differences only) | `TrendFollowingStrategy::on_data`, `src/strategy/trend_following.cpp:428`, `:446`; `get_ema_values`, `:1187-1191` |
| the volatility estimate, the forecast's own volatility, the attenuation | adjusted returns | `src/strategy/trend_following.cpp:447-450` |
| the optimiser's covariance | adjusted returns (the adjusted level differenced over the raw previous close) | the sleeve's last 756 levels and closes, `src/strategy/trend_following.cpp:505-512`; `optimiser_covariance`, `src/optimization/one_pass.cpp:61`, `:113` |
| the risk overlay's gate window and covariance | adjusted returns | the sleeve's `overlay_returns`, `src/strategy/trend_following.cpp:503-504`; `overlay::gate_window`, `src/optimization/one_pass.cpp:428-432` |
| the risk reporter's readings in the live snapshot | adjusted returns | `RiskManager::adjusted_levels_of`, `src/risk/risk_manager.cpp:568`; `calculate_returns`, `:694` |
| the cost model's volatility multiplier | its own log return, 0 on a change bar | `include/trade_ngin/live/futures_cost_feed.hpp:213-218`; `TransactionCostManager::record_log_return`, `src/transaction_cost/transaction_cost_manager.cpp:288-292` |
| the price a forecast and a position size divide by, notional, the risk readings' exposures | raw close | `src/strategy/trend_following.cpp:445` (`window.close = series.raw`) |
| fill prices, cost reference prices, margin | raw close | [COST_MODEL.md](COST_MODEL.md) |
| marks and daily P&L | raw close, with the change bar's move left out (section 6) | `src/backtest/backtest_coordinator.cpp:1405-1410`; `src/live/live_pnl_manager.cpp:118-121` |

A percentage, a log or a ratio is never taken of the adjusted level.

## 3. Change bars: roll, flip, and the bar with no id

A switch is read from the ids on the **consumed** bars of a symbol, in date order. Consumed means
every bar except the ones the session classifier withholds (a corrupt or locked print, a thin first
print: `SymbolDayVerdict::k01_withheld`, `include/trade_ngin/data/session_classifier.hpp:128`;
`k01_consumed_bars`, `src/data/session_classifier.cpp:472`). A withheld bar is never a change bar or
a confirming bar, books no P&L, and is never fed later; the next consumed bar is compared with the
last consumed one.

The rule (`classify_instrument_changes`, `src/data/roll_series.cpp:10`; the same rule one bar at a
time in `RollTracker::add`, `:121`):

- A **change bar** is a bar whose id differs from the last known id before it. On its own day a
  change cannot be told from a one-day visit, so it is **pending**.
- The next consumed bar with an id decides. Same id as the change bar: the change is **confirmed as
  a roll**, and the legs are booked on this confirming bar. Back on the held contract's id: it was a
  **flip**, nothing is booked, and the reverting bar is itself a change bar.
- A third id while a change is pending extends the pending sequence. A return to an older contract
  after a confirmation is a new roll.
- A bar **with no id** is not judged: it is neither a change nor a confirmation, a pending sequence
  stays pending through it, and the next bar with an id is compared with the last known id.

| consumed ids, oldest first | reading |
|---|---|
| A, A, B, B | bar 3 is a change bar (pending). Bar 4 confirms: a roll from A to B, closing leg at bar 2's close, opening leg at bar 3's close |
| A, A, B, A | bar 3 is a change bar. Bar 4 reverts: a flip. No legs. Bars 3 and 4 are both change bars: return 0, P&L 0, held |
| A, A, B, C, C | bars 3 and 4 are change bars of one pending sequence. Bar 5 confirms: a roll from A to C, closing leg at bar 2's close, opening leg at bar 4's close |
| A, A, B, (none), B | bar 4 is not judged; the symbol stays held; its move against bar 3's close is booked as usual. Bar 5 confirms the roll |
| A, A, B, B, A, A | two rolls, A to B then B to A: four legs |

The session classifier also reads the ids: an unconfirmed change on a print thinner than 0.10 of the
symbol's volume norm gets a JUNK verdict (`kIdChangeHoldFraction`,
`include/trade_ngin/data/session_classifier.hpp:54`; `holds_id_change`,
`src/data/session_classifier.cpp:186`). That bar is still consumed (`id_change_hold`,
`session_classifier.hpp:117`): it is the change bar of this section, not a withheld bar.

## 4. The hold through a change bar

A symbol whose last consumed bar is a change bar, either bar of a flip, or a bar with no id inside a
pending sequence is **held** at that rebalance: it stays at its current quantity on every sleeve and
no fill is made in it (`RollTracker::Status::holds`, `include/trade_ngin/data/roll_series.hpp:112`).

| engine | where | log line |
|---|---|---|
| backtest | the symbol is taken out of the cycle's session set, `src/backtest/backtest_coordinator.cpp:873-875` | `CHANGE_BAR_HOLD`, `FLIP_PAIR` (`:877-886`) |
| live | the symbol joins the book's hold set, `apps/strategies/live_portfolio_conservative.cpp:1796-1799` | `CHANGE_BAR_HOLD`, `FLIP_PAIR` (`:1800-1814`) |

The hold lasts exactly the rebalances on which the symbol's last consumed bar is pending: it covers
a cycle that consumes no bar of the symbol, and it ends on the confirming bar, on which the symbol
trades again. When one cycle consumes several bars of a symbol (a catch-up) the sequence is walked
bar by bar and the rebalance acts on the last bar's status (`roll_status_of`,
`src/data/roll_series.cpp:211`).

Reason for the hold: on the change bar the engine cannot yet tell a roll from a one-day visit, and
the bar's own move is unknown (section 6). The position waits one consumed bar for the answer.

## 5. The two ROLL legs

On the rebalance that consumes the confirming bar, each sleeve holding the symbol books two fills of
type `ROLL` at its held quantity (`make_roll_legs`, `src/data/roll_series.cpp:171`):

| leg | side | price | contract written on the row |
|---|---|---|---|
| closing | against the position | the close of the last consumed bar before the first pending change bar (the old contract's last known close) | the outgoing id |
| opening | with the position | the close of the change bar into the kept id (the new contract's first known close) | the incoming id |

- The quantity is the book held at the start of the bar: the filled positions in the backtest
  (`src/backtest/backtest_coordinator.cpp:975-977`, `:1057-1061`), the stored Day T-1 book in live
  (`apps/strategies/live_portfolio_conservative.cpp:2401-2405`). A symbol not held has no legs.
- Neither leg carries realised P&L. The old contract's life is already in the daily P&L rows.
- Both prices must be positive closes; a leg without one stops the run (`ROLL_LEG STOP`:
  `backtest_coordinator.cpp:1077-1083`, live `:2420-2424`). No default price is used.
- Order within the day: closing leg, opening leg, then the day's `STRATEGY` fills
  (`backtest_coordinator.cpp:441-446`, `:1089`; live `:2443-2445`).
- Ids: live `EXEC_<symbol>_<YYYYMMDD>_RC` and `_RO`, with order ids `ROLL_..._RC` and `_RO`, the date
  being the **confirming bar's** (live `:2406-2412`); backtest `RL-<sleeve>-<n>`, n even for the
  closing leg (`backtest_coordinator.cpp:1062-1064`). The row's own `date` is the rebalance's: in
  live the run date, which is the day after the confirming bar in the ordinary case and later when
  the confirming bar arrives late (below).
- A strategy never sees a leg as a trade: `on_execution` returns on any row that is not `STRATEGY`
  (`src/strategy/base_strategy.cpp:200`, `src/strategy/trend_following.cpp:228`).

**Which rolls a live run books.** A live run books every roll confirmed since the contract recorded
on the symbol's stored Day T-1 positions row, not the rolls of a calendar span
(`live_rolls_by_state`, `include/trade_ngin/live/live_roll_legs.hpp:155`). So a confirming bar that
arrives late is legged by the first run that consumes it, with ids dated the confirming bar, and
that run's Day T-1 row books the consumed moves no earlier run booked (`LateRollSettlement`,
`live_roll_legs.hpp:73`; `late_booked_points`, `:100`). A re-run reads the contract its own stored
legs rolled out of (`PostgresDatabase::get_stored_roll_contracts`,
`src/data/postgres_database_extensions.cpp:147`). A recorded contract that is on no consumed bar of
the window refuses the run (live `:1345-1359`). The legs are stored before the Day T-1 rows are
re-written, in one transaction with the sweep of the day's earlier ROLL rows
(`replace_roll_day_executions`, live `:2632`).

**Costs: two outright legs, by choice.** Each leg is priced by the same cost model as any fill of
that size at that price: the contract's fee plus the model's spread and impact
(backtest `backtest_coordinator.cpp:1070-1075`; live `:2413-2418`; see [COST_MODEL.md](COST_MODEL.md)).
A roll traded as one calendar-spread order pays less than two separate outright trades. The engine
has no spread prices, so it books the two-outright figure, which is an upper bound on the cost of a
roll done as a spread. That is a deliberate choice of the conservative side, and every place the
figure appears says so (the `ROLL_LEG` log line, `backtest_coordinator.cpp:1102`; the column comments
of migrations 017 and 018). The book's own cost arithmetic makes the same assumption, two sets of
trading costs for each roll (Carver, strategy three, the holding cost calculation, restated under
strategy nine), and its Tactic one, "Contract selection and rolling", notes that a spread order
would be expected to pay about half the spread cost of two separate trades.

## 6. No P&L on change and flip bars

Daily futures P&L is `quantity x (close_t - close_t-1) x multiplier` between consecutive consumed
bars. On a change bar it is 0, because the step is the splice's gap (section 1):

| engine | where |
|---|---|
| backtest | the bar is marked a change bar without moving the tracker (`src/backtest/backtest_coordinator.cpp:1223-1228`) and its P&L is set to 0 (`:1408-1410`) |
| live | `consumed_t1_settlement` puts the symbol in `zero_pnl_symbols` (`include/trade_ngin/live/session_book_gate.hpp:205`, `:224`); `LivePnLManager` books 0 for it (`src/live/live_pnl_manager.cpp:118-121`) |

The confirming bar is not a change bar (its id equals the previous bar's), so it books its move
against the change bar's close: both are closes of the new contract.

What this leaves out, stated plainly:

- **On a roll, one bar.** The held contract's own move over the change bar is not booked and is not
  in the return series. It cannot be separated from the gap without both contracts' prices on the
  same day.
- **On a flip, two bars.** The position never left its contract, but both the change bar and the
  reverting bar book 0, so the held contract's move across those two days is not booked.
- **A zero in the return series** on each of those dates, for every consumer of section 2.

What it keeps: within each contract stretch the daily rows sum exactly to
`quantity x (last close - first close) x multiplier` of that contract, so the drift of each contract
toward expiry (the carry) stays in the P&L and in the trend signal.

## 7. What is stored

| migration | column | meaning |
|---|---|---|
| 015 | `trading.executions.execution_type`, `backtest.executions.execution_type` | `STRATEGY` (default), `ROLL` or `BORROW`; NOT NULL with a CHECK |
| 015 | `executions.instrument_id` (both schemas) | on a ROLL leg, the contract it traded: outgoing on the closing leg, incoming on the opening leg. NULL on every other row |
| 016 | `trading.positions.instrument_id`, `backtest.final_positions.instrument_id` | the contract the position is held in after that date's bar: still the outgoing contract on a pending change bar, the new one from the confirming bar on. NULL when unknown and on every equity row |
| 017 | `trading.live_results.daily_roll_costs`, `total_roll_costs` | the part of `daily_transaction_costs` and `total_transaction_costs` paid on ROLL legs |
| 018 | `backtest.results.transaction_costs`, `roll_costs`, `total_roll_fills` | the run's cost total, its ROLL part, and the count of ROLL rows (two rows per roll per sleeve holding the symbol) |

The contract on a positions row is written from the roll status: backtest
`backtest_coordinator.cpp:1222-1228`; live, the Day T-1 row is re-written with the contract held
after the T-1 bar (`apps/strategies/live_portfolio_conservative.cpp:2274-2277`).

**Cost totals.** The cost columns are a superset pair: the transaction cost total holds every fill
(STRATEGY, ROLL and BORROW) and the roll column is the part of it paid on ROLL legs; the difference is
what the STRATEGY fills cost (plus the BORROW rows on an equity book, which has no ROLL rows).

- Every total charges a fill its cost after netting, `total_transaction_costs - netting_adjustment`
  (`transaction_cost::net_cost`, `include/trade_ngin/transaction_cost/netting.hpp:97`;
  `add_net_costs`, `src/transaction_cost/netting.cpp:183`). The day's total is taken this way in live
  (`live_portfolio_conservative.cpp:2575-2577`) and per cycle in the backtest
  (`backtest_coordinator.cpp:2006-2007`).
- A ROLL leg is never netted. The legs stay out of the sleeve netting
  (`live_portfolio_conservative.cpp:2554-2556`) and are created with an adjustment of exactly 0
  (`src/data/roll_series.cpp:202`), so a leg's net cost is its own cost.
- The roll totals read the leg's own cost through `transaction_cost::unnetted_cost`
  (`src/transaction_cost/netting.cpp:177`): `backtest.results.roll_costs` in `run_cost_totals`
  (`netting.cpp:197-198`, called at `backtest_coordinator.cpp:460`) and the trade statistics' roll
  cost (`src/backtest/backtest_metrics_calculator.cpp:452`). The live `daily_roll_costs` adds each
  leg's `total_transaction_costs` as the leg is made (`live_portfolio_conservative.cpp:2439`);
  `total_roll_costs` is the previous row's total plus the day's (`:3825-3833`).
- A ROLL (or BORROW) row that carries a non-zero netting adjustment is **refused**, never corrected:
  `unnetted_row_refusal` (`netting.cpp:166`) produces the text and `unnetted_cost`, `add_net_costs`
  and `run_cost_totals` throw `NettingRefused` (`netting.hpp:108`; `netting.cpp:179`, `:186`,
  `:195`). The backtest fails the run on the day it fires (`backtest_coordinator.cpp:1649-1656`,
  `:367-373`) or at the end-of-run total (`:459-467`) and returns no result: no
  `backtest.results`, `backtest.equity_curve` or `backtest.executions` row is written. The
  `backtest.final_positions` rows of the cycles already run stay, because they are written cycle by
  cycle after the warm-up (`:407-416`). In live the exception reaches `main`, which exits 1
  (`live_portfolio_conservative.cpp:4779`); the day's `live_run_metadata` row and its signals are
  already stored by then (`:1554`, `:1940`) and none of the day's executions is (`:2632`).

The email shows the roll figures as its own lines (`src/core/email_sender.cpp:1469`, `:3720`).

## 8. Trade statistics across a roll

A roll is not a round trip. In the backtest's trade statistics
(`BacktestMetricsCalculator::calculate_trade_statistics`,
`src/backtest/backtest_metrics_calculator.cpp:414`):

- a ROLL leg never moves the tracked position, scores no trade and leaves the trade's open time
  (`:448-454`); it is counted in `roll_fills` and its cost goes to `roll_costs`, never into a trade;
- the open trade's entry price is carried across the roll by the leg gap,
  `entry := entry + (opening leg price - closing leg price)`, once per roll, longs and shorts alike
  (`RollEntryCarry`, `:283`, design comment at `:263-282`; applied at `:436-438`).

So a trade held through a roll scores the move of the contracts actually held: entry to the closing
leg in the old contract plus the opening leg to the exit in the new one. In price points that is
the move the trade's daily P&L rows book. The daily rows carry the contract multiplier and the trade
statistic does not, so the two agree only after the statistic's price move is multiplied by it
(section 10 shows it). Legs that cannot be paired stop the calculation
(`:318-323`). `calculate_symbol_pnl` applies the same carry and charges each leg's cost to the symbol
(`:562-566`).

## 9. What is not a roll

**A vendor relabel.** On 2026-02-22 the vendor began labelling the same contract of each of the four
equity index micros with a new id (on the raw table: MES 42140878 to 42003800, MNQ 42002475 to
42004946, MYM 42005850 to 42001953, M2K 42005017 to 42002147, the old id through 2026-02-20 and the
new one from 2026-02-22). The contract did not change, so the day is not a roll. Each such change is
declared in the portfolio's `instrument_id_relabels` list
(`config_template/portfolios/conservative/portfolio.json:36-41`):

    {"symbol": "MES", "date": "2026-02-22", "from": "42140878", "to": "42003800"}

From `date` on, a bar of the symbol carrying `to` is read as carrying `from`
(`ListingDates::read_id`, `src/data/listing_dates.cpp:354`), at the loader, before any consumer:
the backtest loader (`src/backtest/backtest_data_loader.cpp:74-75`), the live window and the
classifier's history (`apps/strategies/live_portfolio_conservative.cpp:958-959`, `:1184-1185`), the
id rows the classifier is fed (`ListingDates::read_ids`, `listing_dates.cpp:420`), and the contract
recorded on stored rows (live `:1324-1325`, `:1340`). No change bar, no hold, no legs, no roll cost;
the day's move is booked and its return kept; stored `instrument_id` values stay the old id until the
next real roll. An entry that rewrites nothing, or whose new id already appears before its date, is
warned of (`relabel_findings`, `listing_dates.cpp:368`; `RELABEL_UNMATCHED`, `RELABEL_LATE`).
Without the entries the rule of section 3 reads the day as a roll: a sixteen-year backtest of the
conservative book (2010-10-07 to 2026-10-07) without the listing-date, relabel and rule-removal
blocks books RL legs for the micros it holds (MNQ, MYM and M2K) on the cycle of 2026-02-24. The template of the BASE book carries neither the relabel list nor the `listing_dates`
block below.

**The listing-date switch.** The micros MES, MNQ, MYM and M2K list on 2019-05-06. With the
`listing_dates` block (`portfolio.json:26-34`) a backtest whose window trades before that date holds
the E-mini (ES, NQ, YM, RTY; ten times the contract size, with its own fee and margin) on the price
history stored under the micro's symbol, and the micro from its listing date
(`ListingDates::tradeable`, `src/data/listing_dates.cpp:104`). On the switch rebalance the held
E-mini is exited and the micro entered by the configured rule, `open_at_target` by default
(`plan_listing_switch`, `listing_dates.cpp:251`; `src/portfolio/portfolio_manager.cpp:2310-2321`).
A live run trades no predecessor and refuses one in its universe or its stored book
(`live_listing_refusals`, `include/trade_ngin/live/live_listing_guard.hpp:30`).

| | a roll | the listing-date switch |
|---|---|---|
| what changes | the contract month of the same instrument | the instrument: another contract size, fee and margin |
| trigger | the vendor id on the consumed bars | a configured date |
| quantity | unchanged | re-sized: the E-mini is closed and the micro opened at its own target |
| fills | two `ROLL` rows, ids `RL-` (backtest) or `_RC` / `_RO` (live) | `STRATEGY` rows, ids `LC-` (the E-mini's close) and `LO-` (the micro's entry) (`make_listing_switch_fills`, `listing_dates.cpp:279`; `portfolio_manager.cpp:2516`) |
| prices | two different closes of two contracts | each fill at its own symbol's signal close, which is one shared price history |
| P&L | 0 on the change bar | booked as on any day: there is no price gap |
| trade statistics | not a trade; the entry is carried | a trade closes and another opens |
| netting | excluded | the book's switch is split to the sleeves and netted by the rebalance's own rules |
| engines | backtest and live | backtest only |

## 10. One roll, day by day

An example of the arithmetic, on stored rows. Book: CONSERVATIVE (portfolio
`CONSERVATIVE_PORTFOLIO`, one sleeve, `TREND_FOLLOWING`, stored under `strategy_id` `LIVE_TREND_FOLLOWING`). Source: a replay of dated live runs
of the conservative book for run dates 2026-04-24 to 2026-05-03, one run a day in date order, on its
template configuration, which carries the listing-date, relabel and rule-removal blocks. It is the
run `docs/TREND_FOLLOWING_SYSTEM.md` uses. Symbol `MBT.v.0`, long 1 contract, multiplier 0.1, fee
2.011 a contract. The position is bought on the run of 2026-04-24 at 78,225. The run of
2026-04-30 closes it at 76,035 (fill `EXEC_MBT.v.0_20260430_SC`) and opens a short of 1 at the same
price (`EXEC_MBT.v.0_20260430`), which is outside this example.

In the live frame a dated run of date D trades at the close of the bar dated D-1, and the positions row
dated D carries the move of bar D, written final by the run of D+1.

**Bars and the series** (adjusted level anchored on the 2026-04-29 bar; step on the change bar =
79,070 - 77,960 = +1,110):

| bar date | instrument id | raw close | status | adjusted level | adjusted return |
|---|---|---:|---|---:|---:|
| 2026-04-23 (Thu) | 42185193 | 78,225 | | 79,335 | |
| 2026-04-24 (Fri) | 42185193 | 77,960 | | 79,070 | (79,070 - 79,335) / 78,225 = -0.003388 |
| 2026-04-25 (Sat) | no bar | | | | |
| 2026-04-26 (Sun) | 42013708 | 79,070 | change bar, pending | 79,070 | 0 |
| 2026-04-27 (Mon) | 42013708 | 77,740 | confirms the roll | 77,740 | (77,740 - 79,070) / 79,070 = -0.016821 |
| 2026-04-28 (Tue) | 42013708 | 76,680 | | 76,680 | (76,680 - 77,740) / 77,740 = -0.013635 |
| 2026-04-29 (Wed) | 42013708 | 76,035 | | 76,035 | (76,035 - 76,680) / 76,680 = -0.008412 |

**Stored `trading.positions` rows** (`daily_realized_pnl` = 1 x close change x 0.1):

| row date | quantity | daily_realized_pnl | check | instrument_id | what the runs do |
|---|---:|---:|---|---|---|
| 2026-04-24 | 1 | -26.50 | (77,960 - 78,225) x 0.1 | 42185193 | the run of 04-24 buys 1 at 78,225 |
| 2026-04-25 | 1 | 0.00 | no bar | 42185193 | |
| 2026-04-26 | 1 | 0.00 | change bar: (79,070 - 77,960) x 0.1 = +111.00 is not booked | 42185193 | the run of 04-27 consumes the change bar: MBT is held, no fill; the row keeps the outgoing contract |
| 2026-04-27 | 1 | -133.00 | (77,740 - 79,070) x 0.1 | 42013708 | the run of 04-28 consumes the confirming bar: books the two legs and re-writes this row with the incoming contract |
| 2026-04-28 | 1 | -106.00 | (76,680 - 77,740) x 0.1 | 42013708 | |
| 2026-04-29 | 1 | -64.50 | (76,035 - 76,680) x 0.1 | 42013708 | the run of 04-30 closes the long at 76,035 (and opens a short of 1 at the same price) |

Old contract stretch: -26.50 = 1 x (77,960 - 78,225) x 0.1. New contract stretch:
-133.00 - 106.00 - 64.50 = -303.50 = 1 x (76,035 - 79,070) x 0.1. Total booked: -330.00.

**The two stored `trading.executions` rows** (both `date` 2026-04-28, `execution_type` ROLL,
`netting_adjustment` 0):

| exec_id | side | quantity | price | instrument_id | commissions_fees | implicit_price_impact (points) | slippage_market_impact | total_transaction_costs |
|---|---|---:|---:|---|---:|---:|---:|---:|
| `EXEC_MBT.v.0_20260427_RC` | SELL | 1 | 77,960 | 42185193 | 2.011 | 4.35734140 | 0.43573414 | 2.44673414 |
| `EXEC_MBT.v.0_20260427_RO` | BUY | 1 | 79,070 | 42013708 | 2.011 | 4.37310781 | 0.43731078 | 2.44831078 |

Checks: the closing price is the 04-24 close (the last consumed bar before the change bar) and the
opening price the 04-26 close (the change bar); the id date 20260427 is the confirming bar's; the
impact in dollars is the impact in points x 0.1 x 1 (4.35734140 x 0.1 = 0.43573414;
4.37310781 x 0.1 = 0.43731078); the total is fee plus impact (2.011 + 0.43573414 = 2.44673414;
2.011 + 0.43731078 = 2.44831078). The roll costs this position 4.89504492.

**Stored `trading.live_results` rows.** The same run also legs ZL, ZM and ZW, and the run of
2026-04-29 legs ZS (each long 1; every row has `netting_adjustment` 0):

| ROLL legs on the 2026-04-28 run | closing leg cost | opening leg cost | pair |
|---|---:|---:|---:|
| MBT | 2.44673414 | 2.44831078 | 4.89504492 |
| ZL | 11.92669501 | 11.96364081 | 23.89033582 |
| ZM | 9.88031048 | 9.86149371 | 19.74180419 |
| ZW | 19.22327771 | 19.25329658 | 38.47657429 |
| total, 8 legs | | | 87.00375922 |

| row date | daily_transaction_costs | daily_roll_costs | total_transaction_costs | total_roll_costs | check |
|---|---:|---:|---:|---:|---|
| 2026-04-27 | 3.933885 | 0.000000 | 1,060.848518 | 0.000000 | the change-bar run: MBT held, nothing booked on it; the day's one fill is a STRATEGY fill in ZT (3.933885) |
| 2026-04-28 | 88.096891 | 87.003759 | 1,148.945409 | 87.003759 | 8 ROLL legs 87.003759 + 1 STRATEGY fill (M2K) 1.093132 = 88.096891; 1,060.848518 + 88.096891 = 1,148.945409 |
| 2026-04-29 | 33.843523 | 30.280416 | 1,182.788932 | 117.284175 | ZS legs 15.13138406 + 15.14903229 = 30.28041635, plus 1 STRATEGY fill (ZF) 3.56310679 = 33.84352314; 87.003759 + 30.280416 = 117.284175 |

**The trade-statistics rule on this trade.** The rule is the backtest's; a live run has no trade
statistics, and this run is used here only for its prices. Entry 78,225; the roll's leg gap is
79,070 - 77,960 = +1,110, so the carried entry is 79,335; exit 76,035. The statistic scores
1 x (76,035 - 79,335) = -3,300 before costs. That figure is in price points: the calculation applies
no contract multiplier (`src/backtest/backtest_metrics_calculator.cpp:476-477`). Multiplied by the
contract's 0.1 it is -330.00, the sum of the daily rows above, so the carry makes the trade's price
move the move the days booked. Without the carry it would score 76,035 - 78,225 = -2,190 points
(-219.00 with the multiplier), which includes the +1,110 step (+111.00) that no day booked. The cost
the statistic takes off a trade is the closing fill's own cost after netting, in currency (`:428`,
`:459`); here 2.45667148.

## 11. Known limits

- **No per-contract bars.** The database holds one contract per symbol per date. The gap between two
  contracts on the roll day is not observable, so the whole one-bar step is treated as the gap: the
  change bar's own move is not booked and its return is 0 (section 6). The closing leg is priced at
  the old contract's last consumed close and the opening leg at the new contract's first, which are
  closes of different days.
- **The roll date is the vendor's.** The engine rolls when the vendor's volume ranking switches, one
  consumed bar later. It has no roll calendar of its own, no expiry date in the contract metadata,
  and no rule about which contract month to hold.
- **Vendor whipsaws are booked as rolls.** The ranking has no minimum tenure. A switch that lasts
  one bar is a flip (held, nothing booked). A switch that lasts two bars or more and then returns is
  two rolls, with four legs for a held symbol. Reading every bar of the raw table from 2010-06-07 to
  2026-10-07 through the rule of section 3 with nothing withheld (the four relabels applied) gives
  3,586 confirmed rolls, 937 flips, and 288 rolls back into the contract the series had just left,
  every one within 48 days (the most by symbol: 6L 49, ZR 32, GF 24, GC 19, HE 16, ZM 15). A run's own counts
  differ: it reads only its consumed bars (a withheld thin print never reaches the rule), and a roll
  costs something only when the symbol is held.
- **A switch back is booked as a roll.** The engine has no rule that refuses a switch back into
  the contract just left: it books the switch back as a roll, with its two legs. Such a rule cannot
  tell a whipsaw from the opposite case, where the forward switch was the short visit and the return
  is the series going back to the contract it then stays on, and in that case it would hold the
  symbol until the old id appears again. `ZM.v.0` in 2025 is
  the case on the raw table: three bars on contract 446324 (2025-07-28 to 2025-07-30), then back on
  203692 from 2025-07-31 until 2025-11-19 (79 bars, one of them, 2025-09-03, a one-bar visit to
  446324 again). The rule of section 3 books a roll and a roll
  back there; a rule that refused the switch back would hold ZM for those bars.
- **Roll costs are an upper bound** (section 5): two outright fills where a spread order is the usual
  way to roll.
- **A bar with no id is not judged.** On the table from 2010-06-07 to 2026-10-07 three kept
  bars have none (6A on 2025-11-05, ZM and ZR on 2025-10-10). A stored
  positions row then keeps the last confirmed contract.

What per-contract data (each contract's own daily bars, with its month and expiry) would change: the
gap on the roll day becomes a known number, so the change bar's move can be booked and its return
kept, and both legs can be priced on the same day; the roll date can be set by a rule of the
engine's instead of the vendor's ranking, which ends the whipsaws, the flips and the need for the
relabel list; and the roll can be priced as a spread.
