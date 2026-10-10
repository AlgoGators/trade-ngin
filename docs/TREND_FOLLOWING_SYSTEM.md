# The trend following system

This is the one document that explains the futures trend following strategy end to end: what each stage does, its
formula, every setting in force and where it lives, a worked example with real numbers, where the code is, and where
each piece comes from. The strategy is the trend following of Robert Carver's *Advanced Futures Trading Strategies*
(Harriman House, 2023), long and short, on 36 futures, sized to a 20 percent annual risk target on a 500,000 book.
Section 1 walks the method in the order the engine runs it. Section 2 lists every place we depart from the book.
Section 3 lists what was tried and not adopted. Section 4 says how to read a result.

Deeper mechanics live in the companion documents: `OPTIMIZER_AND_RISK_DESIGN.md` (the daily rebalance for an
engineer), `RISK_MODULES.md` (the overlay as a module), `COST_MODEL.md` (costs and netting), `FUTURES_ROLLS.md`
(rolls), `LIVE_RUN_CYCLE.md` (the live day and the stored columns), `DATA_SOURCES_OF_TRUTH.md` (the tables read),
`CONFIG_GUIDE.md` (the config files). This document stands alone as the strategy's explanation.

## How to read this document

**The book.** CONSERVATIVE is the futures book: one trend sleeve (`TREND_FOLLOWING`) on 36 contracts with 500,000
of capital. BASE is a placeholder test book with a second, faster sleeve; it runs the same engine and is named only
where it behaves differently.

**The Source line.** Every stage ends with a Source line. Each part of a stage carries one of exactly three labels (a stage
with two parts names both):

- CARVER AS WRITTEN: the engine does what the book says. The book is cited by strategy number, section title and
  table number. No page numbers are given.
- CARVER, ADAPTED: the book's method with a stated difference. The line says what differs and why.
- OURS: not in the book. The line says so and gives the reason.

**The worked examples.** Each table is an example of the arithmetic of its stage. All but the two illustrations
named below use real numbers and say which of three sources the numbers come from:

| Name used below | What it is |
|---|---|
| the chain | a replay of dated live runs of the conservative book, one run a day in date order, for run dates 2026-04-24 to 2026-05-03, on its template configuration (`config_template/portfolios/conservative/` plus `config_template/defaults.json`), which carries the listing-date, relabel and rule-removal blocks. A run dated T reads the bars up to T-1 (the signal bar) and stores its rows under date T |
| the backtest | a sixteen-year backtest of the conservative book (2010-10-07 to 2026-10-07) on its template configuration, which carries the listing-date, relabel and rule-removal blocks |
| the stored bars | the daily bars of `futures_data.ohlcv_1d` with the vendor ids of `futures_data.ohlcv_1d_raw`. No run is involved |

Two tables come from none of the three and say so: the removal table of section 1.6 is the recipe tool's output
on the sixteen-year run of the conservative book the list is computed from, which is made without the rule-removal block, and
the weights of section 1.8 are computed from the metadata. Two examples of section 1.11, the cut and the cap, are
illustrations with invented inputs and say so: they show the arithmetic and quote no stored row.

A number that a run stores is quoted from that run's row (`trading.positions`, `trading.executions`,
`trading.signals`, `trading.live_results`; `backtest.executions`, `backtest.final_positions`,
`backtest.equity_curve`, `backtest.results`). A number that is not stored is either read from the run's own log
line (named) or recomputed here from the stored bars with the stage's formula, and the table says which. The
recomputation of the volatility and the forecast reproduces the stored forecast in `trading.signals` to the last
stored digit. In both runs a row dated D carries the close of the bar before it as its price and books the move of
bar D as its P&L.

**Notation.**

| Symbol | Meaning |
|---|---|
| S_0 | the starting capital, 500,000 |
| E | the sizing capital of the day (section 1.9) |
| V | the account value, S_0 plus the cumulative net P&L |
| tau | the annual risk target, 0.20 |
| IDM | the instrument diversification multiplier, 2.5 |
| w_i | the instrument weight of contract i (section 1.8) |
| M_i | the contract multiplier (metadata `Contract Size`) |
| P_i | the raw close of the signal bar |
| A_i | the back-adjusted level (section 1.3) |
| sigma_i | the annual volatility of contract i (section 1.4) |
| F_i | the combined forecast, between -20 and +20 (sections 1.5 to 1.7) |
| N*_i | the unrounded target in contracts (section 1.10) |
| u_i | the weight of one contract on the sizing capital, M_i x P_i / E |
| L | the per-name cap, 2 |
| m | the overlay's scalar, between 0 and 1 (section 1.11) |

---

## 1. The method

The sections follow the engine from bars to stored rows. Inside one rebalance the order is: the hold set and the
deferral band (section 1.12) are settled first, because they decide which rows are free; then the cap and the
overlay (1.11); then the forecast-sign close (1.12), the search (1.13), the buffer, the rounding, the clip and the
trim (1.14). The ROLL legs of a confirmed roll (1.15) are booked ahead of the day's STRATEGY fills.

### 1.1 The instrument universe

**What it does.** The book trades the 36 continuous futures series stored in `futures_data.ohlcv_1d`, each under a
symbol such as `ZN.v.0`. Every contract's size, tick, fee, sector and margin come from one row of
`metadata.contract_metadata`. A contract trades only from its listing date: the four equity index micros were listed
on 2019-05-06, and before that date the book trades the E-mini of the same index on the same price history. A
vendor relabel of the same contract is told apart from a roll by a short list in the config.

**The 36 contracts.** Weight is the instrument weight of section 1.8. "Pairs not run" is the removal list of
section 1.6; every other contract runs all six trend speeds.

| Symbol | Contract | Sector | Multiplier | Tick | Fee per contract | Weight | Pairs not run |
|---|---|---|---|---|---|---|---|
| GF | Feeder cattle | Agriculture | 500 | 0.025 | 2.961 | 0.015476 | 2/8 |
| HE | Lean hogs | Agriculture | 400 | 0.025 | 2.961 | 0.015476 | 2/8 |
| KE | KC hard red winter wheat | Agriculture | 50 | 0.25 | 3.010 | 0.015476 | 2/8, 4/16 |
| LE | Live cattle | Agriculture | 400 | 0.025 | 2.961 | 0.015476 | 2/8 |
| ZC | Corn | Agriculture | 50 | 0.25 | 3.010 | 0.015476 | 2/8, 4/16 |
| ZL | Soybean oil | Agriculture | 600 | 0.01 | 3.010 | 0.015476 | 2/8 |
| ZM | Soybean meal | Agriculture | 100 | 0.1 | 3.010 | 0.015476 | none |
| ZR | Rough rice | Agriculture | 2,000 | 0.005 | 3.010 | 0.015476 | 2/8, 4/16, 8/32 |
| ZS | Soybeans | Agriculture | 50 | 0.25 | 3.010 | 0.015476 | 2/8 |
| ZW | Chicago wheat | Agriculture | 50 | 0.25 | 3.010 | 0.015476 | 2/8, 4/16 |
| MBT | Micro Bitcoin | Crypto | 0.1 | 5 | 2.011 | 0.071429 | none |
| CL | Crude oil | Energy | 1,000 | 0.01 | 2.360 | 0.038690 | none |
| HO | Heating oil | Energy | 42,000 | 0.0001 | 2.360 | 0.038690 | none |
| NG | Natural gas | Energy | 10,000 | 0.001 | 2.460 | 0.038690 | 2/8 |
| RB | RBOB gasoline | Energy | 42,000 | 0.0001 | 2.360 | 0.038690 | none |
| M2K | Micro E-mini Russell 2000 | Equities | 5 | 0.1 | 0.614 | 0.038690 | none |
| MES | Micro E-mini S&P 500 | Equities | 5 | 0.25 | 0.614 | 0.038690 | none |
| MNQ | Micro E-mini Nasdaq 100 | Equities | 2 | 0.25 | 0.614 | 0.038690 | none |
| MYM | Micro E-mini Dow | Equities | 0.5 | 1 | 0.610 | 0.038690 | none |
| 6A | Australian dollar | FX | 100,000 | 0.00005 | 2.461 | 0.017196 | none |
| 6B | British pound | FX | 62,500 | 0.0001 | 2.461 | 0.017196 | none |
| 6C | Canadian dollar | FX | 100,000 | 0.00005 | 2.461 | 0.017196 | none |
| 6E | Euro | FX | 125,000 | 0.00005 | 2.461 | 0.017196 | none |
| 6J | Japanese yen | FX | 12,500,000 | 0.0000005 | 2.461 | 0.017196 | none |
| 6L | Brazilian real | FX | 100,000 | 0.00005 | 2.461 | 0.017196 | 2/8, 4/16 |
| 6M | Mexican peso | FX | 500,000 | 0.00001 | 2.461 | 0.017196 | 2/8 |
| 6N | New Zealand dollar | FX | 100,000 | 0.00005 | 2.461 | 0.017196 | none |
| 6S | Swiss franc | FX | 125,000 | 0.00005 | 2.461 | 0.017196 | none |
| UB | Ultra US Treasury bond | Interest Rates | 1,000 | 0.03125 | 1.810 | 0.038690 | none |
| ZF | 5-year US Treasury note | Interest Rates | 1,000 | 0.0078125 | 1.510 | 0.038690 | none |
| ZN | 10-year US Treasury note | Interest Rates | 1,000 | 0.015625 | 1.660 | 0.038690 | none |
| ZT | 2-year US Treasury note | Interest Rates | 2,000 | 0.00390625 | 1.510 | 0.038690 | none |
| GC | Gold | Metals | 100 | 0.10 | 2.510 | 0.038690 | none |
| HG | Copper | Metals | 25,000 | 0.0005 | 2.510 | 0.038690 | none |
| PL | Platinum | Metals | 50 | 0.1 | 2.510 | 0.038690 | none |
| SI | Silver | Metals | 5,000 | 0.005 | 2.510 | 0.038690 | none |

The four contracts traded before a micro's listing date (they have metadata rows and no price series of their own):

| Symbol | Contract | Multiplier | Tick | Fee per contract | Replaced on 2019-05-06 by |
|---|---|---|---|---|---|
| ES | E-mini S&P 500 | 50 | 0.25 | 2.247 | MES (10 MES = 1 ES) |
| NQ | E-mini Nasdaq 100 | 20 | 0.25 | 2.247 | MNQ (10 MNQ = 1 NQ) |
| YM | E-mini Dow | 5 | 1 | 2.240 | MYM (10 MYM = 1 YM) |
| RTY | E-mini Russell 2000 | 50 | 0.1 | 2.247 | M2K (10 M2K = 1 RTY) |

**Listing dates.** The vendor stores the E-mini's bars under the micro's symbol for the years before the micro
existed (MES, MNQ and MYM from 2010-06-07, M2K from 2017-07-09). The price is right and the size would be fiction,
so a backtest whose window trades before 2019-05-06 runs the E-mini as a symbol of its own on those bars, with its
own multiplier, fee and margin, and the micro only from the listing date. The two contracts of a pair count as one
instrument: one weight, one forecast history, the same rules. On the first signal bar on or after the listing date
the switch rule `open_at_target` runs before the day's pass: a held E-mini is closed with one fill (id `LC-...`)
and the micro is opened at that day's capped, scaled target rounded to the nearest whole contract (id `LO-...`); a
pair whose E-mini traded earlier in the run and is flat that day is opened at its rounded target the same way. Both
fills are STRATEGY fills priced by the cost model. Three exceptions: an E-mini held inside the deferral band
(section 1.12) is carried instead, q E-minis becoming ten times q micros, and the pass holds them; if either
contract is in the hold set or has no bar that day, or the micro does not signal, the switch waits for the next
rebalance on which both can trade and the E-mini stays held; a pair whose E-mini never traded in the run has no
switch, and the micro is opened by the day's pass. The day's search, buffer and trim then run from that book. A
window that trades before 2019-05-06 therefore holds four symbols that are not among the 36. A live run trades no
predecessor and refuses one in its universe or its stored book on or after the listing date.

**The relabel list.** The engine detects a roll from a change of the vendor's instrument id on the bars
(section 1.3). On the bar dated 2026-02-22 the vendor relabelled the same quarter's contract of the four equity
micros with a new id. That is not a roll. `instrument_id_relabels` names the four changes; from the listed date a
bar carrying the new id is read as carrying the old one, so no change bar is held, no ROLL legs are booked and that
day's move is booked as on any day. The stored `instrument_id` stays the old id until the next real roll.

**Settings in force.**

| Setting | Value | Where |
|---|---|---|
| Universe | every symbol of `futures_data.ohlcv_1d` (36) | `apps/backtest/bt_portfolio_conservative.cpp:151`, `apps/strategies/live_portfolio_conservative.cpp:517` |
| Contract size, tick, fee, sector, margins | one row per contract | `metadata.contract_metadata` (`Contract Size`, `Tick Size`, `Fee Per Contract`, `Sector`, `Overnight Initial Margin`, `Overnight Maintenance Margin`) |
| `listing_dates.contracts` | MES, MNQ, MYM, M2K listed 2019-05-06; before: ES, NQ, YM, RTY; ratio 10 | `config_template/portfolios/conservative/portfolio.json` |
| `listing_dates.switch_rule` | `open_at_target` | same file |
| `instrument_id_relabels` | date 2026-02-22: MES 42140878 to 42003800, MNQ 42002475 to 42004946, MYM 42005850 to 42001953, M2K 42005017 to 42002147 | same file |
| Engine position guard | 1,000 contracts in a backtest, 500 live (a guard that does not bind at this size) | `config_template/defaults.json` `execution.position_limit_backtest`, `execution.position_limit_live` |

**Worked example: the listing switch.** The backtest, signal bar 2019-05-06 (rows stamped 2019-05-07). Held
positions are the run's rows dated 2019-05-06; the targets are from the run's `LISTING_LEG` log lines; the fills
are the run's `backtest.executions` rows.

| Pair | E-mini held before | Close of 2019-05-06 | Micro's capped target | Rounded | Exit fill | Entry fill | Fill costs |
|---|---|---|---|---|---|---|---|
| ES to MES | 0 | 2,918.00 | 4.483502 | 4 | none | `LO-TREND_FOLLOWING-0` BUY 4 MES | 5.038467 (fee 4 x 0.614 = 2.456) |
| NQ to MNQ | 1 | 7,762.75 | 4.787865 | 5 | `LC-TREND_FOLLOWING-1` SELL 1 NQ | `LO-TREND_FOLLOWING-1` BUY 5 MNQ | 5.135746 (fee 2.247) and 4.754632 (fee 5 x 0.614 = 3.070) |
| YM to MYM | 1 | 26,281 | 1.899081 | 2 | `LC-TREND_FOLLOWING-2` SELL 1 YM | `LO-TREND_FOLLOWING-2` BUY 2 MYM | 5.226971 (fee 2.240) and 1.857736 (fee 2 x 0.610 = 1.220) |
| RTY to M2K | 0 | 1,609.60 | 3.525839 | 4 | none | `LO-TREND_FOLLOWING-3` BUY 4 M2K | 4.145604 (fee 4 x 0.614 = 2.456) |

Check on one row: the one NQ held was 20 x 7,762.75 = 155,255 of notional against a target worth 4.787865 micros,
4.787865 x 2 x 7,762.75 = 74,334; the five MNQ opened are 5 x 2 x 7,762.75 = 77,628. In the run's rows NQ and YM
have no fill after 2019-05-07, ES none after 2018-10-11, RTY none after 2019-05-03, and the four micros have none
before 2019-05-07.

**Worked example: the relabel.** The backtest, MYM, two contracts held. Closes and vendor ids are the stored bars;
the P&L is the run's `backtest.final_positions.realized_pnl` of the row for that bar, 2 x 0.5 x the change in
close.

| Bar date | Close | Vendor id on the bar | Id the engine reads | P&L booked |
|---|---|---|---|---|
| 2026-02-19 | 49,466 | 42005850 | 42005850 | -249.00 |
| 2026-02-20 | 49,680 | 42005850 | 42005850 | +214.00 |
| 2026-02-22 | 49,581 | 42001953 | 42005850 | -99.00 |
| 2026-02-23 | 48,889 | 42001953 | 42005850 | -692.00 |
| 2026-02-24 | 49,225 | 42001953 | 42005850 | +336.00 |

The move across the relabel (-99.00 = 1 x (49,581 - 49,680)) is booked like any other day, the stored
`instrument_id` stays 42005850 and the run stores no ROLL row for MES, MNQ, MYM or M2K in February 2026.

**Where in the code.** `ListingDates::tradeable` (`src/data/listing_dates.cpp:104`), `ListingDates::pair_root`
(`:138`), `ListingDates::conversions_due` (`:192`), `plan_listing_switch` (`:251`), `make_listing_switch_fills`
(`:279`), `ListingDates::read_id` (`:354`), `ListingDates::apply_relabels` (`:406`). The switch inside the
rebalance: `PortfolioManager::rebalance_one_pass`, `src/portfolio/portfolio_manager.cpp:2342` to `:2566`. The
config blocks are read in `src/core/config_loader.cpp` (`instrument_id_relabels` at `:370`, `listing_dates` at
`:404`). The design comment is the header `include/trade_ngin/data/listing_dates.hpp`.

**Source.** OURS. The book does not trade one contract before another's listing date: its appendix C notes that some
micro and mini futures in its data set have their history backfilled with the prices of the larger contract, and it
sizes the micro throughout. We trade what was listed at the time, because a position of a few tenths of an E-mini
could not have been held. The relabel list is ours as well: a vendor id change that is not a change of contract
is read as no change, because booking it as a roll would charge two legs and drop a day's move.

### 1.2 The bars the strategy consumes

**What it does.** Each contract has at most one bar per date (where the feed holds two rows for a date, the row
with the larger volume is kept). Before any bar reaches the strategy, a per-symbol session classifier decides what
that bar is for trading. A bar that is not a usable price is withheld: the signal never reads it, and the next
bar's return is taken against the last consumed close. A missing bar holds the position at its last mark.

**The rule.** For one symbol on one date, with `norm` the median volume of the symbol's 20 trailing weekday bars
that were themselves sessions:

| Verdict | Condition | Effect |
|---|---|---|
| JUNK | a locked bar (high equal to low), whatever its volume; or volume < 0.01 x norm and volume < 1,000 lots; or volume < 50 lots and volume < 0.25 x norm (the 50-lot floor alone while the symbol has no norm) | marked at the bar's price, withheld from the signal, the sizing and orders; position held |
| JUNK (id hold) | an unconfirmed change of instrument id on a bar whose volume is below 0.10 x norm (or while the symbol has no norm) | consumed as a change bar (section 1.3), position held |
| NO_BAR (closure) | no bar, and none expected (a named holiday; a Saturday or Sunday that is the eve of a fixed-date holiday; a day of the week the symbol printed on in none of the 8 preceding weeks) | held at its last mark |
| NO_BAR (feed hole) | no bar, on a day of the week the symbol printed on in at least one of the 8 preceding weeks | held at its last mark; an error is logged and counted |
| SESSION | any other bar | the signal updates and the position may change at that bar's close |

**Settings in force.** They are constants of the code; nothing reads them from a config file.

| Setting | Value | Where |
|---|---|---|
| `junk_fraction` | 0.01 | `include/trade_ngin/data/session_classifier.hpp:61` |
| `junk_ceiling_lots` | 1,000 | `:64` |
| `floor_lots`, `floor_fraction` | 50, 0.25 | `:67`, `:68` |
| `norm_window_bars` | 20 | `:74` |
| `expected_lookback_weeks` | 8 | `:77` |
| `id_change_hold_fraction` | 0.10 | `:54`, `:83` |

**Worked example.** The stored bars, 6C (Canadian dollar). The norm is the median volume of the 20 weekday session bars before the date: 56,566 lots before 2025-11-02 and
2025-11-03 (the bars of 2025-10-06 to 2025-10-31), and 58,477 lots before 2025-11-04, 11-05 and 11-06.

| Bar date | Day | Volume | 0.01 x norm | Verdict | Why |
|---|---|---|---|---|---|
| 2025-11-02 | Sunday | 992 | 565.66 | SESSION | 992 is above 565.66, and above the 50-lot floor |
| 2025-11-03 | Monday | 69,382 | 565.66 | SESSION | an ordinary session |
| 2025-11-04 | Tuesday | 52,379 | 584.77 | SESSION | an ordinary session |
| 2025-11-05 | Wednesday | 275 | 584.77 | JUNK | 275 is below 584.77 and below 1,000 lots: withheld |
| 2025-11-06 | Thursday | 53,980 | 584.77 | SESSION | its return is taken against the close of 2025-11-04 |

**Where in the code.** `SessionClassifier::judge_bar` (`src/data/session_classifier.cpp:265`),
`SessionClassifier::classify_symbol_day` (`:298`), `k01_consumed_bars` (`:472`). The verdicts and the rule's
numbers are documented in `include/trade_ngin/data/session_classifier.hpp:22` to `:84`.

**Source.** OURS. The book has no session logic: it assumes one usable daily price per business day. Our feed
carries Sunday session rows, holiday stubs and occasional corrupt prints, and a position sized or traded on such a
bar is an error.

### 1.3 Prices: back-adjusted for returns, real for levels

**What it does.** A continuous series splices one contract onto the next, and on the bar where the vendor switches
contract the close jumps by the price gap between two contracts. That jump is not a return. The engine marks those
bars from the vendor's instrument id and builds a back-adjusted series. Everything that is a return or a difference
reads the back-adjusted series; everything that is a level reads the real contract price.

**The rule.**

- A CHANGE bar is the first consumed bar whose instrument id differs from the last known id. Its status is decided
  on the next consumed bar: the id is kept (a ROLL, confirmed) or it reverts (a FLIP: no roll, both bars excluded).
  A bar with no id is not judged.
- The adjusted level: A_t = P_t + the sum, over every change bar j after t, of (P_j - P_j-1). The series is anchored
  on the latest bar and can be at or below zero, so it is used only for differences.
- The adjusted return: r_t = (A_t - A_t-1) / P_t-1, which is exactly 0 on a change bar and equals the raw simple
  return on every other bar. The date of a change bar stays in every window as a zero.

| Consumer | Series |
|---|---|
| Returns for the volatility, the forecast's volatility, the attenuation, the overlay's covariance, the optimiser's covariance | adjusted return r |
| The moving averages of the trend rules | adjusted level A (differences only) |
| Sizing, notional, the weight per contract, costs, margin, marks, P&L | raw close P |

**Settings in force.** None: the rule has no parameter. The instrument id comes from `futures_data.ohlcv_1d_raw`,
joined to each kept bar on the bar's own print.

**Worked example.** The stored bars, MBT (Micro Bitcoin, multiplier 0.1); the adjusted level and return are
computed here with the formulas above, as the engine holds them on signal bar 2026-04-28.

| Bar date | Raw close P | Vendor id | Status | Adjusted level A | Adjusted return r |
|---|---|---|---|---|---|
| 2026-04-22 | 78,235 | 42185193 | | 79,345 | +0.02388431 |
| 2026-04-23 | 78,225 | 42185193 | | 79,335 | -0.00012782 |
| 2026-04-24 | 77,960 | 42185193 | | 79,070 | -0.00338766 |
| 2026-04-26 | 79,070 | 42013708 | change bar | 79,070 | 0 |
| 2026-04-27 | 77,740 | 42013708 | confirms the roll | 77,740 | -0.01682054 |
| 2026-04-28 | 76,680 | 42013708 | | 76,680 | -0.01363519 |

Checks. The step on the change bar is 79,070 - 77,960 = 1,110, so every earlier level is the raw close plus 1,110
(78,225 + 1,110 = 79,335). On 2026-04-24, r = (79,070 - 79,335) / 78,225 = -0.00338766. On 2026-04-27,
r = (77,740 - 79,070) / 79,070 = -0.01682054. The raw series would have shown +1.42 percent on the change bar,
a gain nobody holding the contract made.

**Where in the code.** `classify_instrument_changes` (`src/data/roll_series.cpp:10`), `adjusted_levels` (`:75`),
`adjusted_returns` (`:89`), `build_series` (`:109`), `RollTracker::add` (`:121`). The strategy builds the series
for each symbol in `TrendFollowingStrategy::on_data` (`src/strategy/trend_following.cpp:428`). The design comment
is `include/trade_ngin/data/roll_series.hpp:13` to `:46`.

**Source.** CARVER, ADAPTED. The split is the book's: strategy one, "Back-adjusting futures price", and appendix B,
"Back-adjusting a futures price series", build the back-adjusted series; strategy two states that position sizing
uses the price of the contract held, not the back-adjusted price; appendix B, "Standard deviation estimation",
divides the adjusted change by the price of the contract held. What differs is how the series is built. The book
back-adjusts from the prices of each dated contract, taking the gap on a day when both contracts have a price. We
have one continuous series per symbol, so the gap is the whole close-to-close step across the vendor's switch and
the change bar's own move is not known: its return is set to 0.
See `FUTURES_ROLLS.md`.

### 1.4 Volatility

**What it does.** Each contract's annual volatility is estimated from its adjusted returns with an exponentially
weighted standard deviation over about a month, blended with its own long-run average so the estimate neither
collapses in a quiet spell nor stays high for long after a shock. Two versions come out of one recursion: the one
positions are sized on, and the one the forecast divides by.

**The formula.** Over a fixed trailing window of the last 3,200 consumed bars, every recursion starting at the
window's first bar, with lambda = 2 / (32 + 1) = 0.060606:

- mean_1 = r_1, var_1 = max(0.1 x r_1^2, 1e-6)
- mean_k = lambda x r_k + (1 - lambda) x mean_k-1
- var_k = max(lambda x (r_k - mean_k)^2 + (1 - lambda) x var_k-1, 1e-6)
- bars per year at bar k = 255 / (calendar days spanned by the trailing 256 bars / 365.25); factor_k is its square
  root (16 where it cannot be counted)
- sigma_short_k = sqrt(var_k) x factor_k, kept inside [0.005, 5.0]
- sigma_long_k = the mean of sigma_short over the trailing 2,520 values, bar k included
- sigma_k = 0.7 x sigma_short_k + 0.3 x sigma_long_k (the sizing volatility)
- the forecast's volatility is the same daily standard deviation times the fixed factor 16, with its own 2,520-value
  mean and the same 0.7 / 0.3 blend

The bars-per-year count exists because our series carries a Sunday session row: a contract prints about 300 bars a
year, not 256, and a daily standard deviation times 16 would understate its annual volatility.

**Settings in force.**

| Setting | Value | Where |
|---|---|---|
| EWMA span | 32 | `config_template/portfolios/conservative/portfolio.json`, `strategies.TREND_FOLLOWING.config.vol_lookback_short` |
| Estimator window | 3,200 bars | `kWindowBars`, `include/trade_ngin/strategy/trend_estimator.hpp:52` |
| Long-run length | 2,520 values | `kLongRunValues`, `:53` |
| Annualisation count | trailing 256 bars | `kAnnualisationBars`, `:54` |
| Fixed factor for the forecast's volatility | 16 | `kFixedAnnualisation`, `:60` |
| Variance seed share, variance floor | 0.1, 1e-6 | `src/strategy/trend_estimator.cpp:13`, `:14` |
| Volatility floor and cap | 0.005, 5.0 | `:15`, `:16` |
| Weight on the short estimate | 0.7 | `:17` |
| `vol_lookback_long` | 252 in the config file; not read by the estimator, whose long-run length is the constant above | `include/trade_ngin/strategy/trend_following.hpp:30` |

**Worked example.** The stored bars, ZN (10-year note), four consecutive signal bars of the chain. Closes are the
stored bars; every other column is recomputed from them with the formula. In this data set the bar before 2026-04-27 is
2026-04-20 (close 111.640625).

| Signal bar | Close | Return r | EWMA mean | Variance | Daily sd | Bars per year | sigma_short | sigma_long | sigma |
|---|---|---|---|---|---|---|---|---|---|
| 2026-04-27 | 111.062500 | -0.00517845 | -0.000140521 | 9.450347e-06 | 0.00307414 | 299.4815 | 0.053200 | 0.050132 | 0.052279 |
| 2026-04-28 | 110.906250 | -0.00140687 | -0.000217269 | 8.963364e-06 | 0.00299389 | 300.4476 | 0.051894 | 0.050137 | 0.051367 |
| 2026-04-29 | 110.312500 | -0.00535362 | -0.000528563 | 9.831111e-06 | 0.00313546 | 300.4476 | 0.054348 | 0.050143 | 0.053087 |
| 2026-04-30 | 110.609375 | +0.00269122 | -0.000333425 | 9.789738e-06 | 0.00312886 | 300.4476 | 0.054234 | 0.050150 | 0.053009 |

Check on 2026-04-28. Return: 110.90625 / 111.0625 - 1 = -0.00140687. Mean: 0.060606 x (-0.00140687) + 0.939394 x
(-0.000140521) = -0.000217269. Deviation: -0.00140687 + 0.000217269 = -0.001189601, squared 1.415151e-06.
Variance: 0.060606 x 1.415151e-06 + 0.939394 x 9.450347e-06 = 8.963364e-06, whose square root is 0.00299389. Bars
per year: the trailing 256 bars span 310 calendar days, 255 / (310 / 365.25) = 300.4476, square root 17.3334.
sigma_short = 0.00299389 x 17.3334 = 0.051894. sigma = 0.7 x 0.051894 + 0.3 x 0.050137 = 0.051367.

**Where in the code.** `trend_estimator::estimate` (`src/strategy/trend_estimator.cpp:132`): the annualisation
factor at `:157` to `:164`, the variance recursion at `:169` to `:187`, the long-run mean and the blend at `:189`
to `:216`. The design comment is `include/trade_ngin/strategy/trend_estimator.hpp:12` to `:51`. It is called from
`TrendFollowingStrategy::on_data` at `src/strategy/trend_following.cpp:489`.

**Source.** CARVER, ADAPTED. The estimator is strategy three, "Forecasting future volatility": an exponentially
weighted standard deviation with a span of 32 days, blended with a long-run estimate at weights 0.7 on the current
and 0.3 on the long run, on percentage returns formed as appendix B, "Standard deviation estimation", forms them.
The estimator is centred on its exponentially weighted mean, as the book's is. Three things differ. The annual
factor is counted from the bars instead of the book's fixed 16, because our series has Sunday rows. The variance is
seeded and floored, so a series that starts or sits flat cannot give a zero. And the book's long-run measure is a
ten-year average; ours is the mean of the trailing 2,520 consumed bars,
about 8.4 years on our bar count.

### 1.5 The trend rules: EWMAC, scaling, capping

**What it does.** A trend rule compares a fast and a slow exponentially weighted moving average of the adjusted
price. The difference, divided by the contract's daily price volatility, is a forecast that means the same thing
on every contract. Six speeds are run. Each is scaled so its typical absolute size is 10, adjusted for the current
volatility regime, and capped at plus or minus 20.

**The formula.** For a pair (f, s) with s = 4 x f:

- EMA_n,k = (2 / (n + 1)) x A_k + (1 - 2 / (n + 1)) x EMA_n,k-1, seeded at the window's first adjusted level
- raw = (EMA_f - EMA_s) / (P x forecast_sigma / 16)
- q = the share of the trailing 2,520 values of sigma_short that are at or below today's (defined once 252 values
  exist), smoothed by a 10-bar EWMA seeded at its first value
- attenuation = 2 - 1.5 x smoothed q (2 when volatility is at its lowest, 0.5 at its highest; 1 before q is defined)
- scaled = raw x attenuation x scalar, kept inside [-20, +20]

**Settings in force.**

| Setting | Value | Where |
|---|---|---|
| `ema_windows` | (2,8), (4,16), (8,32), (16,64), (32,128), (64,256) | `config_template/portfolios/conservative/portfolio.json`, `strategies.TREND_FOLLOWING.config.ema_windows` |
| Forecast scalars | 12.1, 8.53, 5.95, 4.10, 2.79, 1.91 for fast spans 2, 4, 8, 16, 32, 64 | `forecast_scalar`, `src/strategy/trend_estimator.cpp:46` |
| Forecast cap | 20 | `kForecastCap`, `include/trade_ngin/strategy/trend_estimator.hpp:61` |
| Attenuation: values needed, history, smoothing span | 252, 2,520, 10 | `kAttenuationMinValues` and `kLongRunValues` (`include/trade_ngin/strategy/trend_estimator.hpp:55`, `:53`), `kAttenuationSmoothSpan` (`src/strategy/trend_estimator.cpp:19`) |
| Warm-up | a contract publishes no target until it has 256 bars (the longest window) | `src/strategy/trend_following.cpp:410` |

A pair without a fixed scalar is refused at start-up (`TrendFollowingStrategy::validate_config`,
`src/strategy/trend_following.cpp:67`).

**Worked example.** The chain, ZN on signal bar 2026-04-27 (the run dated 2026-04-28), recomputed from the stored
bars. P = 111.0625, forecast_sigma = 0.04807733, so the denominator is 111.0625 x 0.04807733 / 16 = 0.333724. The
quantile q is 0.634921, its smoothed value 0.582600, so the attenuation is 2 - 1.5 x 0.582600 = 1.126100.

| Pair | Fast EMA | Slow EMA | Difference | raw | x attenuation | x scalar | Scaled forecast |
|---|---|---|---|---|---|---|---|
| 2/8 | 111.213111 | 111.159822 | +0.053288 | +0.159678 | +0.179813 | 12.1 | +2.175743 |
| 4/16 | 111.233806 | 111.106836 | +0.126970 | +0.380464 | +0.428440 | 8.53 | +3.654595 |
| 8/32 | 111.159822 | 111.316514 | -0.156692 | -0.469526 | -0.528734 | 5.95 | -3.145966 |
| 16/64 | 111.106836 | 111.746647 | -0.639811 | -1.917184 | -2.158942 | 4.10 | -8.851662 |
| 32/128 | 111.316514 | 112.161147 | -0.844633 | -2.530929 | -2.850081 | 2.79 | -7.951725 |
| 64/256 | 111.746647 | 112.406744 | -0.660097 | -1.977971 | -2.227394 | 1.91 | -4.254322 |

The same six forecasts on the four signal bars of section 1.4, with the attenuation of each day:

| Signal bar | Attenuation | 2/8 | 4/16 | 8/32 | 16/64 | 32/128 | 64/256 |
|---|---|---|---|---|---|---|---|
| 2026-04-27 | 1.126100 | +2.1757 | +3.6546 | -3.1460 | -8.8517 | -7.9517 | -4.2543 |
| 2026-04-28 | 1.117351 | -3.9247 | +0.5696 | -3.8254 | -8.9309 | -8.1029 | -4.4002 |
| 2026-04-29 | 1.100018 | -15.1692 | -5.7459 | -5.9295 | -9.1870 | -8.0419 | -4.3830 |
| 2026-04-30 | 1.086812 | -10.5164 | -6.3815 | -6.4849 | -9.2274 | -8.0658 | -4.4428 |

**Where in the code.** `trend_estimator::estimate`: the attenuation at `src/strategy/trend_estimator.cpp:218` to
`:255`, the moving averages and the scaled forecast at `:259` to `:281`. The scalars are `forecast_scalar` at
`:46`.

**Source.** CARVER AS WRITTEN for the rule, the six speeds, the scalars and the cap; CARVER, ADAPTED for the
attenuation. The rule, the six speeds, the fixed scalars and the cap: strategy
seven, "From trend strength to forecast", gives the forecast as the crossover over the daily price volatility
scaled to an average absolute value of 10; strategy nine, "Selecting a series of trend following filters", gives
the six pairs and table 29 the scalars 12.1, 8.53, 5.95, 4.10, 2.79 and 1.91; the cap of 20 is strategy seven,
"A maximum forecast value".
The attenuation is strategy thirteen, "Adjusting forecasts for volatility regimes": a multiplier of 2 minus 1.5
times a quantile, smoothed over ten days, applied to the raw forecast with the scalars unchanged. Two things
differ in the attenuation. The book takes the quantile of the relative volatility (the blended estimate over its
ten-year average) across the instrument's whole history; we take the quantile of sigma_short against its own
trailing 2,520 values, which needs no second average and has a fixed window. And the book introduces the
attenuation as a variation on its trend and carry strategy; we apply it to the trend rules alone.

### 1.6 Forecast weights, the diversification multiplier, and rules removed by cost

**What it does.** The six scaled forecasts of a contract are averaged with equal weights. Because the six are not
perfectly correlated their average is smaller than each, so it is multiplied back up by a forecast diversification
multiplier and capped again at 20. Before the averaging, a rule that is too expensive to trade on a given contract
is removed from that contract: twelve contracts run fewer than six speeds.

**The formula.**

- pairs_i = the sleeve's six pairs less the pairs removed from contract i (always its fastest ones)
- combined_i = FDM(number of pairs_i) x the equal-weight mean of the scaled forecasts of pairs_i, kept inside
  [-20, +20]

The removal rule, applied contract by contract and speed by speed:

- cost per trade = cost of trading one contract / (P x M) / sigma, in Sharpe-ratio units
- yearly cost of a rule = (turnover of the rule + 2 x rolls a year) x cost per trade
- the rule is removed from the contract when its yearly cost exceeds 0.15

**Settings in force.**

| Setting | Value | Where |
|---|---|---|
| Forecast weights | equal across the pairs a contract runs | `src/strategy/trend_estimator.cpp:286` |
| Multiplier table | 1.26 for six pairs, 1.19 for five, 1.13 for four, 1.08 for three, 1.03 for two, 1.00 for one | `TrendFollowingConfig::fdm`, `include/trade_ngin/strategy/trend_following.hpp:32` (the table in force; `config_template/defaults.json` `strategy_defaults.fdm` carries the same six rows) |
| `trading_rule_removals` | no 2/8 on 6M, GF, HE, LE, NG, ZL, ZS; no 2/8 or 4/16 on 6L, KE, ZC, ZW; no 2/8, 4/16 or 8/32 on ZR (twelve contracts, eighteen rules) | `config_template/portfolios/conservative/portfolio.json` |
| Cost limit for one rule on one contract | 0.15 Sharpe-ratio units a year | `LIMIT`, `scripts/trading_rule_costs.py` |
| Turnover of each speed | 98.5, 50.2, 25.4, 13.2, 7.6, 5.2 times a year for fast spans 2 to 64 (the book's table 35) | `TABLE_35`, `scripts/trading_rule_costs.py` |
| Cost per trade | the median, over every signal day of the 16-year run the list is computed from, of the engine's cost of one contract over its notional and its sizing volatility | `scripts/trading_rule_costs.py` |
| Rolls a year | the rolls the engine confirmed on the contract's series in that run | same |

The committed list is the authority. `scripts/trading_rule_costs.py` shows how the list follows from the recipe
and, with `--check`, reports whether recomputed costs still give it. A list that comes out different is
reviewed before the config is changed; the tool never writes it. Only a contract's fastest pairs can be listed, at
least one pair is left, the multiplier table must carry a row for the number left, and the two contracts of a
listing-date pair must carry the same entry. A pair the equity slow rule reads cannot be removed from a symbol
that rule names. A contract is named by its base symbol as the metadata holds it, with at least one pair, each of
them one of the sleeve's and named once. Any other list is refused at start-up
(`TrendFollowingStrategy::validate_config`, `src/strategy/trend_following.cpp:96` to `:164`;
`trend_estimator::pairs_after_removal`, `src/strategy/trend_estimator.cpp:94`).

**Worked example: the combination.** The chain, signal bar 2026-04-27. ZN runs six pairs; ZW (Chicago wheat) runs
four. The combined forecast is the value stored in `trading.signals.signal_value` by the run dated 2026-04-28.

| Contract | Pairs run | Scaled forecasts | Mean | FDM | Combined (stored) |
|---|---|---|---|---|---|
| ZN | six | +2.1757, +3.6546, -3.1460, -8.8517, -7.9517, -4.2543 | -3.062223 | 1.26 | -3.858401 |
| ZW | four (8/32 to 64/256) | -1.0180, +4.0081, +3.5371, -1.1746 | +1.338116 | 1.13 | +1.512071 |

On the same bar ZW's two removed rules would have read +1.8874 (2/8) and -4.7223 (4/16), and all six would have
combined to +0.528689; the stored value is the four-pair one. On the next two signal bars ZW stores +2.422757 and
+3.087894, each 1.13 times the mean of its four scaled forecasts.

**Worked example: the removal.** The recipe's own table for four contracts, from the sixteen-year run of the
conservative book the list is computed from, made without the rule-removal block (cost per trade is the median over
that run's signal days). Yearly cost = (turnover + 2 x rolls a year)
x cost per trade.

| Contract | Cost per trade | Rolls a year | 2/8 (98.5) | 4/16 (50.2) | 8/32 (25.4) | 16/64 (13.2) | 32/128 (7.6) | 64/256 (5.2) | Pairs removed |
|---|---|---|---|---|---|---|---|---|---|
| ZW | 0.002477840 | 5.2518 | 0.2701 | 0.1504 | 0.0890 | 0.0587 | 0.0449 | 0.0389 | 2/8, 4/16 |
| ZR | 0.004996193 | 7.7527 | 0.5696 | 0.3283 | 0.2044 | 0.1434 | 0.1154 | 0.1034 | 2/8, 4/16, 8/32 |
| HE | 0.002230181 | 8.0027 | 0.2554 | 0.1477 | 0.0923 | 0.0651 | 0.0526 | 0.0473 | 2/8 |
| MES | 0.000393733 | 4.0014 | 0.0419 | 0.0229 | 0.0132 | 0.0083 | 0.0061 | 0.0052 | none |

Check on ZW 4/16: (50.2 + 2 x 5.2518) x 0.002477840 = 60.7036 x 0.002477840 = 0.1504, above 0.15, so the rule is
removed. HE 4/16 at 0.1477 is below the limit and is kept. A cell within 2 percent of the limit (these two) is
flagged by the tool, because it can change sides when costs are recomputed.

**Where in the code.** The contract's own pairs: `trend_estimator::pairs_after_removal`
(`src/strategy/trend_estimator.cpp:94`), applied in `TrendFollowingStrategy::on_data`
(`src/strategy/trend_following.cpp:463` to `:481`); the multiplier lookup at `:482` to `:488`; the mean and the
combined forecast in `trend_estimator::estimate` (`src/strategy/trend_estimator.cpp:286`, `:296`). The config
block is read at `src/core/config_loader.cpp:455` and handed to the sleeve by `hand_over_trading_rule_removals`
(`include/trade_ngin/strategy/sleeve_config.hpp:58`).

**Source.** CARVER AS WRITTEN for the equal weights and the multiplier table; CARVER, ADAPTED for the removal of
rules by cost. The procedure is the book's. Strategy nine, "Putting it together: a method for
allocating forecast weights", says that forecast weights are set instrument by instrument in two steps: first
drop, for that instrument, every rule whose cost is over the limit, then give the rules left the same weight. "Removing expensive trading rules"
sets the limit for a rule at 0.15 Sharpe-ratio units of cost a year, counts two trades for each roll, and gives the
turnover of each speed in table 35. "Top down method for choosing forecast weights" gives the equal weights, and
"Accounting for forecast diversification" with table 36 gives the multiplier for each set of speeds left (1.26,
1.19, 1.13, 1.08, 1.03, 1.00) and the second cap at 20. The book applies the removal as the first step of setting
forecast weights from strategy nine onward, with the same method in strategies ten and eleven, and its own test of
dynamic optimisation on a 500,000 account (strategy twenty-five, "Testing and understanding dynamic optimisation",
table 124) runs on forecasts whose weights were set by the cost of each instrument. What differs is the inputs.
The cost per trade is our engine's cost of one contract (the IBKR fee plus our spread and impact model, section
1.16), taken as a 16-year median; the rolls are the ones our engine confirms; the turnovers are the book's table
35; and the list is fixed in the config and reviewed, not recomputed each day. The costs are ours because they
must be the ones this engine charges.

### 1.7 The equity-index slow rule

**What it does.** For the four equity index contracts, a negative combined forecast stands only when both slow
trend speeds are themselves negative. Otherwise the forecast is set to 0. A positive forecast and every other
contract are untouched. The forecast after this rule is the forecast everywhere downstream: the sizing, its sign,
and the value stored in `trading.signals`.

**The formula.** For contract i in {M2K, MES, MNQ, MYM} (and the E-mini of each pair before its listing date):

- if combined_i < 0 and not (scaled_32/128 < 0 and scaled_64/256 < 0): F_i = 0
- otherwise F_i = combined_i

A held short whose forecast the rule sets to 0 is not closed by rule. A zero forecast has no sign, so the contract
is a free row with a target of 0 that may only step toward zero, and the short leaves the book as fast as the
search and the buffer take it.

**Settings in force.**

| Setting | Value | Where |
|---|---|---|
| `equity_slow_rule.symbols` | M2K, MES, MNQ, MYM | `config_template/portfolios/conservative/portfolio.json` (required on a futures book) |
| `equity_slow_rule.pairs` | (32,128), (64,256) | same |

**Worked example.** The stored bars, MES, recomputed with the formulas of sections 1.4 to 1.6.

| Signal bar | 2/8 | 4/16 | 8/32 | 16/64 | 32/128 | 64/256 | Mean x 1.26 | Slow speeds | F after the rule |
|---|---|---|---|---|---|---|---|---|---|
| 2025-11-20 | -20.0000 | -16.0136 | -7.0624 | +2.5570 | +10.1335 | +13.1786 | -3.613440 | both positive | 0 |
| 2025-11-21 | -14.6012 | -14.5118 | -7.6215 | +1.5364 | +9.1033 | +12.2759 | -2.901956 | both positive | 0 |
| 2025-11-23 | -8.4037 | -12.3355 | -7.7109 | +0.9167 | +8.6034 | +11.9714 | -1.461321 | both positive | 0 |
| 2025-11-24 | +1.3914 | -7.2370 | -6.2195 | +0.8321 | +8.0643 | +11.4018 | +1.728927 | not read | +1.728927 |

On the first three bars the fast speeds pull the average below zero while the two slow speeds are still positive,
so the short is not taken. On the fourth the average is positive and the rule does not act.

**Where in the code.** `trend_estimator::equity_slow_ruled` (`src/strategy/trend_estimator.cpp:66`), applied in
`TrendFollowingStrategy::on_data` (`src/strategy/trend_following.cpp:542` to `:556`). The block is read at
`src/core/config_loader.cpp:296`.

**Source.** OURS. It is not in the book, and it goes against the book's advice: strategy twelve, in its discussion
of forecast adjustments by asset class, recommends avoiding any adjustment that treats long and short
differently. The reason for it: an equity index short taken on the fast speeds alone, against a slow uptrend,
is a bet against the drift the slow speeds measure. The runner sets the rule on the book's first sleeve only
(CONSERVATIVE's one sleeve; BASE's faster sleeve runs neither slow pair;
`include/trade_ngin/strategy/trend_following.hpp:36`).

### 1.8 Instrument weights and the IDM

**What it does.** The instrument weight says what share of the book's risk each contract is given before the
forecast speaks. Weights are equal across sectors and equal within a sector, with no contract allowed more than
half of its sector's share. The instrument diversification multiplier (IDM) then scales every position up, because
36 imperfectly correlated positions carry less risk together than the sum of their parts.

**The formula.**

- the universe of the weights is every metadata symbol that has a bar in `futures_data.ohlcv_1d` at any date, less
  ES. It is computed once per run and is the same on every date, so in a backtest that starts before a contract's
  first bar the contract carries its weight from the first day and the other weights are not rescaled for the
  dates it has no bar: MBT, whose first bar is dated 2021-05-03, holds 0.071429 on every date of a window that
  starts in 2010
- sector weight = 1 / (number of sectors with a contract in that universe) = 1/7
- w_i = sector weight / (contracts in the sector), capped at 0.50 x sector weight
- the weights are brought back to a sum of 1 by scaling only the contracts that were not capped
- the E-mini of a listing-date pair takes the weight of its micro

**Settings in force.**

| Setting | Value | Where |
|---|---|---|
| Sector of each contract | `metadata.contract_metadata.Sector` (seven sectors) | the metadata table |
| Cap on one contract's share of its sector | 0.50 | `MAX_SYMBOL_TO_SECTOR_RATIO`, `src/strategy/trend_following.cpp:985` |
| `idm` | 2.5 | `config_template/portfolios/conservative/portfolio.json`, `strategies.TREND_FOLLOWING.config.idm` |

**Worked example.** The weights of the run config, computed from the metadata with the formula; they are the
`weight` the engine holds for each contract on every day of the chain (0.015476 for ZW, 0.038690 for ZN).

| Sector | Contracts | Equal share (1/7 over the count) | After the 50 percent cap | Scale on the uncapped | Weight each | Sector total |
|---|---|---|---|---|---|---|
| Agriculture | 10 | 0.014286 | 0.014286 | 1.083333 | 0.015476 | 0.154762 |
| Crypto | 1 | 0.142857 | 0.071429 (capped) | not scaled | 0.071429 | 0.071429 |
| Energy | 4 | 0.035714 | 0.035714 | 1.083333 | 0.038690 | 0.154762 |
| Equities | 4 | 0.035714 | 0.035714 | 1.083333 | 0.038690 | 0.154762 |
| FX | 9 | 0.015873 | 0.015873 | 1.083333 | 0.017196 | 0.154762 |
| Interest Rates | 4 | 0.035714 | 0.035714 | 1.083333 | 0.038690 | 0.154762 |
| Metals | 4 | 0.035714 | 0.035714 | 1.083333 | 0.038690 | 0.154762 |

Check. MBT is alone in its sector, so the cap halves it to 0.071429. The uncapped contracts then share
1 - 0.071429 = 0.928571 where they summed to 6/7 = 0.857143, a scale of 1.083333. The total is
6 x 0.1547619 + 0.0714286 = 1.000000.

**Where in the code.** `TrendFollowingStrategy::get_weights` (`src/strategy/trend_following.cpp:915`): the universe
at `:926` to `:937` and `:968`, read with no date filter (`src/data/postgres_database.cpp:819` to `:829`) and
cached (`:916`); the weight of an E-mini at `:574` to `:581`.

**Source.** CARVER, ADAPTED for the weights; CARVER AS WRITTEN for the IDM. Strategy four, "An algorithm for
allocating instrument weights", is the book's top-down method: equal shares to each asset class, then to groups
within a class, then to instruments within a group. We use one layer (the metadata's sector, then the contracts in
it) and add the 50 percent cap so that a sector with a single contract cannot take a whole sector share. The IDM
is the value of strategy four's table 16 for 30 or more instruments, 2.50, which is also the ceiling appendix B
recommends for the calculated multiplier.

### 1.9 The risk target and the sizing capital

**What it does.** The book aims at an annual volatility of 20 percent of its capital. The capital it is sized on is
not the account: it is the starting 500,000 less whatever the account has lost from its high-water mark. A loss
comes off the sizing capital at once, a profit rebuilds it, and nothing above 500,000 is ever sized on: profits
beyond the start are set aside. The account therefore never holds less than the capital the book is sized on, and its
positions stop growing with its profits while still shrinking after every loss.

**The formula.** With net_d the net P&L of settled day d (the day's P&L less every fill cost of the day), C the
cumulative net P&L and P the running peak of C floored at 0:

- E = S_0 - (P - C)
- the same thing day by day: E_next = min(S_0, E + net)
- V = S_0 + C, and V - E = P is the profit set aside: the account above the sizing capital, equal to the account
  above the starting capital only while E = S_0

E is the capital of every capital-terms quantity: the target, the weight per contract u, the overlay's weights and
leverage, the per-name cap and the search's cost term. It is recomputed by every run from the stored daily P&L
history in date order (a backtest: the run's own equity curve; a live run: the settled `trading.live_results` rows,
with the day before the run rebuilt from its settlement move and stored costs). No run reads it back from a
stored value: it is recomputed, logged on the `SIZING_CAPITAL` line and written each day, for the record, to
`risk_detail.sizing_capital` beside `risk_detail.account_value`.

**Settings in force.**

| Setting | Value | Where |
|---|---|---|
| `risk_target` | 0.20 | `config_template/portfolios/conservative/portfolio.json`, `strategies.TREND_FOLLOWING.config.risk_target` |
| `initial_capital` | 500,000 | same file |
| `sizing_mode` | `half_compounding` (the one value accepted; required on a futures book) | same file |
| `starting_capital` | 500,000 (must equal `initial_capital`) | same file |

**Worked example.** The chain, stored `trading.live_results` rows. The row dated D holds the day's net P&L and the
sizing capital the run of D sized its book on; the next sized row's capital must equal min(500,000, E + net).
(2026-04-26 and 2026-05-03 are Sundays whose runs had no new bar to size on and store no `risk_detail`; their P&L
still counts.)

| Row date | Net P&L of the day | Account value before the day | Sizing capital E (stored) | Check: min(500,000, previous E + previous net) |
|---|---|---|---|---|
| 2026-04-24 | -201.48 | 497,440.87 | 495,186.14 | |
| 2026-04-25 | 0.00 | 497,239.40 | 494,984.67 | 495,186.14 - 201.48 = 494,984.67 (a loss comes off at once) |
| 2026-04-26 | +234.38 | 497,239.40 | not sized | |
| 2026-04-27 | +6,139.19 | 497,473.77 | 495,219.04 | 494,984.67 + 0.00 + 234.375 = 495,219.04 (two days' net) |
| 2026-04-28 | +4,224.78 | 503,612.96 | 500,000.00 | 495,219.04 + 6,139.19 = 501,358.23, capped at 500,000.00 |
| 2026-04-29 | +3,168.22 | 507,837.74 | 500,000.00 | 500,000.00 + 4,224.78, capped: the profit is set aside |
| 2026-04-30 | -5,134.39 | 511,005.96 | 500,000.00 | 500,000.00 + 3,168.22, capped |
| 2026-05-01 | -572.06 | 505,871.57 | 494,865.61 | 500,000.00 - 5,134.39 = 494,865.61 |
| 2026-05-02 | 0.00 | 505,299.51 | 494,293.55 | 494,865.61 - 572.06 = 494,293.55 |

On 2026-05-01 the account stands at 505,871.57, above the start, and the book is sized on 494,865.61: the loss of
the day before came off the sizing capital even though the account was still ahead. The profit set aside that day
is 505,871.57 - 494,865.61 = 11,005.96 (the peak of the cumulative net P&L, 511,005.96 - 500,000), although the
account is only 5,871.57 above the starting capital.

**Where in the code.** `half_compounded_capital` (`include/trade_ngin/portfolio/sizing_capital.hpp:58`) and
`backtest_half_compounding` (`:78`), called by the backtest at `src/backtest/backtest_coordinator.cpp:955`; the
live read is `read_live_sizing_equity` (`include/trade_ngin/live/live_sizing_read.hpp:146`); both hand the result
to `PortfolioManager::set_sizing_capital` (`src/portfolio/portfolio_manager.cpp:3812`). The keys are read at
`src/core/config_loader.cpp:520` and `:532`.

**Source.** CARVER AS WRITTEN for the risk target; CARVER, ADAPTED for the sizing capital. The 20 percent target
is the book's standard risk target (derived in strategy two and used throughout). Half compounding is Tactic
three, "Cash and compounding", section "Half compounding": the capital used for sizing never rises above the
amount the account began with, gains beyond that are set aside, and a loss reduces it straight away. The book
offers two forms of capital for a live account, full compounding and half compounding, and warns against running
real money on fixed capital; we use half compounding. What differs is where it runs: the book reports its
backtests on fixed capital with non-compounded returns, and we size the backtest on the same half compounding as
the live book, so the two engines size on the same capital.

### 1.10 The unrounded target

**What it does.** For each contract the strategy turns the forecast into the number of contracts that would give
that contract its share of the risk target, as a fraction. The strategy publishes this number as it is: no
rounding, no buffer, no minimum, no clamp. Everything that makes it a tradeable whole-contract book happens
afterwards, on the whole book at once.

**The formula.**

N*_i = (F_i / 10) x (E x IDM x w_i x tau) / (M_i x P_i x sigma_i)

A negative forecast gives a short target. All 36 contracts are in US dollars, so the exchange rate in the book's
formula is 1. The engine's guards: sigma is kept inside [0.01, 1.0] for sizing; a forecast outside [-20, +20] or
not a number sizes at 0; a contract outside its tradeable window (section 1.1) publishes no target. Also floored:
ten times the formula's denominator at 1.0, the capital at 1,000, the IDM and the exchange rate at 0.1, with tau kept inside
[0.01, 0.5]. A sigma that is not a positive number is replaced (by 0.2 where the strategy reads it, by 0.01 inside
the formula), a price that is not positive by the last price of the contract's history, and a result that is not
finite by 0 (`src/strategy/trend_following.cpp:1051` to `:1107`, `:614`). None binds on this book.

**Settings in force.** The inputs of the earlier sections (E, IDM 2.5, w_i, tau 0.20, the metadata multiplier) and
no others.

**Worked example.** The chain, signal bar 2026-04-27 (the run dated 2026-04-28), E = 500,000.00. F is the stored
signal; sigma is recomputed from the stored bars; N* is computed with the formula and equals the target on the
run's `BOOK` log line. The two product columns are computed at full precision, so a product of the rounded cells
beside them can differ in the last digit.

| Contract | F | E x IDM x w x tau | M | P | sigma | M x P x sigma | N* |
|---|---|---|---|---|---|---|---|
| ZN | -3.858401 | 9,672.62 | 1,000 | 111.0625 | 0.052279 | 5,806.26 | -0.642769 |
| MES | +10.382171 | 9,672.62 | 5 | 7,217.25 | 0.208513 | 7,524.45 | +1.334620 |
| ZW | +1.512071 | 3,869.05 | 50 | 630.75 | 0.282641 | 8,913.78 | +0.065632 |

Check on ZN: 500,000 x 2.5 x 0.03869048 x 0.20 = 9,672.62; 1,000 x 111.0625 x 0.05227925 = 5,806.26; (-3.858401 / 10)
x 9,672.62 / 5,806.26 = -0.642769. At 500,000 the wheat target is seven hundredths of a contract and the note
target two thirds of one: most targets on this book are fractions of a contract, which is why the next stages
work on the whole book and not contract by contract.

**Where in the code.** `TrendFollowingStrategy::calculate_position` (`src/strategy/trend_following.cpp:1046`; the
formula at `:1097` to `:1100`), called from `on_data` at `:620`.

**Source.** CARVER AS WRITTEN for the formula; OURS for the guards. The engine keeps the sizing volatility inside
[0.01, 1.0] and sizes a forecast that is not a number at 0, which the book's formula does not do, because a
volatility near zero would give an unbounded target. The position formula with the forecast
over 10 is strategy seven, "Position scaling
with forecasts"; the instrument weight and IDM enter it in strategy four; and strategy twenty-five, "Optimising for
the best portfolio", states that the optimal positions handed to the optimisation are unrounded and taken before
any buffering.

### 1.11 The per-name cap and the risk overlay

**What it does.** Before anything is traded, the target book is read for risk. First each contract's target is
capped so that its notional is at most twice the sizing capital. Then five readings are taken of the capped
target book. If any reading is above its limit, every free target is multiplied by one scalar m below 1, once.
The scalar m is computed from the target book alone and nothing feeds back into it; the rounded book is read again
only for the trim of section 1.14.

**The formula.** With x_i = N_i x M_i x P_i / E the weight of contract i (signed), over the participants: every
free contract at its capped target, every held contract at its held quantity.

- the capped target: N*c_i = sign(N*_i) x min(|N*_i|, L / u_i)

| Reading | Formula | Limit | Multiplier |
|---|---|---|---|
| R, portfolio risk | sqrt(x' Sigma x) | 2.25 x tau = 0.45 | min(1, 0.45 / R) |
| R_jump, jump risk | sqrt(x' Sigma_jump x), Sigma_jump built from each contract's jump volatility (the 99th percentile of its trailing 2,520 sigma_short values, each taken back to a daily figure by its own bar's factor, then annualised by the gate window's bars a year) and the same correlations | 4.5 x tau = 0.90 | min(1, 0.90 / R_jump) |
| R_shock, correlation shock | the sum of abs(x_i) x sqrt(Sigma_ii), the contract's own volatility in the gate window and not the sizing volatility of section 1.4 (every correlation at its worst) | 4.0 x tau = 0.80 | min(1, 0.80 / R_shock) |
| L_g, gross leverage | the sum of abs(x_i) | 8.0 | min(1, 8.0 / L_g) |
| L_n, net leverage | abs(the sum of x_i) | 6.0 | min(1, 6.0 / abs(L_n)) |

- m = the smallest of the five multipliers; the binding term is the reading that set it, or "none" when m is 1
- the scaled target of a free contract: N~_i = m x N*c_i; a held contract is counted in the readings and not scaled

Sigma is the sample covariance of the participants' adjusted daily returns over the gate window: the last 252
dates on which any participant has a return, ending at the signal date, using the dates on which every participant
has one, annualised by the bars a year those dates themselves show. The window's rules: a participant with no
return in the window is out of R, R_jump and R_shock and stays in leverage; one with fewer than 120 returns is
left out of R and R_jump and stays in R_shock (on its own window volatility, once it has two returns) and in
leverage; with fewer than 120
complete dates the covariance is taken over every window date with missing returns as zeros; with fewer than 21
complete dates the three covariance readings are not computed (the day is marked blind) and the two leverage
readings still apply. If the overlay cannot produce m for any other reason the rebalance is refused: no search, no
fill, the held book kept.

**Settings in force.**

| Setting | Value | Where |
|---|---|---|
| `per_name_cap` (L) | 2 | `config_template/portfolios/conservative/risk.json`, `modules[0]` (the `carver` module) |
| `R_max`, `R_jump_max`, `R_shock_max` | 2.25, 4.5, 4.0, as ratios to tau (the pass multiplies them by tau, `src/portfolio/portfolio_manager.cpp:2224` to `:2226`) | same |
| `max_gross_leverage`, `max_net_leverage` | 8.0, 6.0 | same |
| Gate window, blind floor | 252 dates, 21 complete dates | `kWindowDates`, `kBlindBelow`, `include/trade_ngin/risk/overlay.hpp:32`, `:34` (code constants; `risk.json` carries `lookback_period` 252 and `min_gate_dates` 21 with the same values, which are validated only: changing them does not change the window) |
| Complete-date floor, participant floor | 120, 120 | `kCompleteDatesFloor`, `kParticipantReturnsFloor`, `include/trade_ngin/risk/overlay.hpp:33`, `:35` |
| Jump percentile | 99, over the trailing 2,520 values of sigma_short, each taken back to a daily figure by its own bar's factor | `kJumpPercentile`, `src/strategy/trend_estimator.cpp:20`; taken at `:302` to `:308`, annualised at `src/optimization/one_pass.cpp:434` to `:437` |

**Worked example: the readings.** The chain, every sized rebalance. The readings are the values on each run's
`OVERLAY` log line; the multipliers are computed here. L_g and L_n were also recomputed from the capped targets and
the held rows and agree to the last digit.

| Signal bar | E | R | R_jump | R_shock | L_g | L_n | Smallest of limit over reading | m | Binding |
|---|---|---|---|---|---|---|---|---|---|
| 2026-04-23 | 495,186.14 | 0.0930 | 0.2521 | 0.2592 | 2.9152 | -0.4117 | 2.74 (L_g) | 1 | none |
| 2026-04-24 | 494,984.67 | 0.0836 | 0.2346 | 0.2259 | 2.5324 | -0.1260 | 3.16 (L_g) | 1 | none |
| 2026-04-26 | 495,219.04 | 0.0865 | 0.2395 | 0.2240 | 2.6272 | -0.1422 | 3.05 (L_g) | 1 | none |
| 2026-04-27 | 500,000.00 | 0.1029 | 0.2890 | 0.2961 | 3.2217 | -0.4034 | 2.48 (L_g) | 1 | none |
| 2026-04-28 | 500,000.00 | 0.0996 | 0.2853 | 0.2926 | 3.4269 | -0.9715 | 2.33 (L_g) | 1 | none |
| 2026-04-29 | 500,000.00 | 0.1059 | 0.3007 | 0.3199 | 4.1905 | -1.9934 | 1.91 (L_g) | 1 | none |
| 2026-04-30 | 494,865.61 | 0.1046 | 0.3040 | 0.3081 | 3.9098 | -1.2051 | 2.05 (L_g) | 1 | none |
| 2026-05-01 | 494,293.55 | 0.1048 | 0.3073 | 0.2965 | 3.7916 | -1.0946 | 2.11 (L_g) | 1 | none |

Check on 2026-04-27: 0.45 / 0.1029 = 4.37, 0.90 / 0.2890 = 3.11, 0.80 / 0.2961 = 2.70, 8.0 / 3.2217 = 2.48,
6.0 / 0.4034 = 14.87. Every ratio is above 1, so m = 1 and nothing is cut. On that day the window held 252 dates
from 2025-07-08, of which 184 were complete for all 36 participants.

**Worked example: a cut.** An illustration with invented inputs: every number is invented and round, and none is a
stored row or the output of a run. A book that started with 500,000 has the cumulative settled net P&L C below on
four consecutive rebalances; its sizing capital is E = 500,000 - (P - C), with P the running peak of C (section
1.9). The net notional is that of the book the overlay reads (free contracts at their capped targets, held
contracts at their holdings), so L_n = abs(net notional) / E. The other four readings are taken as inside their
limits, so m is the multiplier of L_n.

| Rebalance | C | Peak P | E | Net notional | L_n | 6.0 / L_n | m | Binding |
|---|---|---|---|---|---|---|---|---|
| 1 | +10,000 | 10,000 | 500,000 | 2,400,000 | 4.8000 | 1.2500 | 1 | none |
| 2 | -10,000 | 10,000 | 480,000 | 3,000,000 | 6.2500 | 0.9600 | 0.960000 | L_n |
| 3 | -15,000 | 10,000 | 475,000 | 3,040,000 | 6.4000 | 0.9375 | 0.937500 | L_n |
| 4 | -5,000 | 10,000 | 485,000 | 2,910,000 | 6.0000 | 1.0000 | 1 | none |

Check on rebalance 2: E = 500,000 - (10,000 - (-10,000)) = 480,000; 3,000,000 / 480,000 = 6.25; 6.0 / 6.25 = 0.96.
On rebalances 2 and 3 the net exposure of the target book is more than six times the sizing capital, and every free
target is scaled by m before the search: a free capped target of 8.000000 contracts becomes 7.680000 on rebalance
2 and 7.500000 on rebalance 3. On rebalance 4 the reading is at its limit, not above it: the multiplier is 1 and
nothing is cut. The same net notional is a larger reading on a smaller sizing capital: 3,000,000 on 500,000 is
6.0000, at the limit.

**Worked example: the cap.** An illustration with invented inputs, none of them a stored row or the output of a
run: one contract with a multiplier of 2,000, a close of 100.00, a sizing volatility of 0.016 and an instrument
weight of 0.04, its combined forecast at the cap of 20 on the short side, on a sizing capital of 500,000. IDM, tau
and L are the settings in force.

| Quantity | Value | How |
|---|---|---|
| F | -20.000000 | the combined forecast at its cap |
| sigma | 0.016 | invented |
| E | 500,000 | invented |
| N* | -6.250000 | (-20 / 10) x (500,000 x 2.5 x 0.04 x 0.20) / (2,000 x 100.00 x 0.016) = -2 x 10,000 / 3,200 |
| u | 0.400000 | 2,000 x 100.00 / 500,000 |
| abs(N*) x u | 2.5000 | above L = 2 |
| L / u | 5.000000 | 2 / 0.400000 |
| N*c | -5.000000 | the target the overlay reads; the search reads m x N*c |

**Where in the code.** The cap: `one_pass::cap_target` (`src/optimization/one_pass.cpp:35`). The window, the
readings and the multiplier: `overlay::gate_window` (`src/risk/overlay.cpp:23`), `overlay::readings` (`:143`),
`overlay::multiplier` (`:180`); the limits from the ratios at `:315` to `:319`. The order inside one rebalance is
`one_pass::rebalance` (`src/optimization/one_pass.cpp:379`), called from
`PortfolioManager::rebalance_one_pass` (`src/portfolio/portfolio_manager.cpp:2015`; the inputs at `:2215` to
`:2226`). The module that carries the limits is `CarverRiskModule` (`src/risk/carver_risk_module.cpp`). The design
comment is `include/trade_ngin/risk/overlay.hpp:12` to `:31`. See `RISK_MODULES.md`.

**Source.** CARVER AS WRITTEN for the one multiplier and its place (the lowest of its readings, never above 1,
applied once to the unrounded targets before the search); CARVER, ADAPTED for the limits, what is scaled, the jump
reading, the covariance and the per-name cap; OURS for the net leverage reading and the rule for thin windows. The
overlay is Tactic four, "Risk management", section "An exogenous risk overlay": a position multiplier that is the
lowest of four (estimated portfolio risk, jump risk on 99th-percentile volatilities, correlation shock, leverage),
each limit set at the 99th percentile of its own history and rounded, never above 1, applied to the unrounded
optimal positions before the dynamic optimisation. The per-name cap is the same chapter's "Position limit for
maximum leverage", whose example uses a leverage ratio of two for a single instrument, and "Position limits and
dynamic optimisation" allows such limits to be applied as constraints inside the optimisation. What differs from
the book's method: (1) the values of the limits are ours, fixed in `risk.json` (0.45, 0.90, 0.80 and 8.0 where the
book's portfolio gives 0.30, 0.75, 0.65 and 20); the book's values come from the history of the author's own
portfolio, and it notes that another set of instruments calls for other limits; (2) the covariance is a sample
covariance of 252 dates of daily adjusted returns where the book combines its 32-day volatility with correlations
from weekly returns, one estimator from the series the engine already holds; (3) the jump volatility is taken over
the trailing 2,520 values of the engine's own short estimate; (4) the readings are taken on the capped target, and
a held row is counted and not scaled, because it cannot trade that day; (5) of the book's three position limits
only the leverage limit is applied, because the other two overlap it or do not bind at this size. Two things are
not in the book: a fifth reading, net leverage, with its own limit of 6.0, because the book is long and short and
its net exposure is limited as well as its gross; and the rule for thin windows (below 21 complete dates the
covariance readings are blind and the leverage readings still apply, and a contract with fewer than 120 returns is
left out of the two covariance readings), so that a new listing does not blind the book.

### 1.12 Holds, the deferral band and the forecast-sign close

**What it does.** Some contracts must not be traded today, and the search must know before it starts. A held
contract keeps its position, is counted in every reading at that quantity and gets no fill. Separately, a position
whose forecast has turned against it is closed, but only once the opposite forecast is strong enough.

**The rule.**

- The hold set: every contract whose bar verdict is not SESSION or that printed no bar this cycle; every contract
  whose last consumed bar is a change bar still waiting to be confirmed, or part of a flip (section 1.3); and every
  contract in the deferral band.
- The deferral band: a contract whose held position has the opposite sign to its forecast while abs(F) < 2 is
  held. The band is strict: at abs(F) = 2 the contract is free. A zero forecast has no sign and is never in the
  band.
- The forecast-sign close: a free contract whose held position has the opposite sign to its target is closed with
  one fill to flat at the signal close, before the search. The search, its cost term and the buffer start from the
  closed book.
- A contract that signalled before and no longer does is closed with one fill to flat on a session bar.

**Settings in force.**

| Setting | Value | Where |
|---|---|---|
| `sign_close_band` | 2 | `config_template/defaults.json`, `optimization.sign_close_band` (required on a futures book) |

**Worked example.** The chain, MBT. The forecast is the stored signal; the held and stored positions are the
stored rows; the hold flags are from each run's `BOOK` and `OPTIMISER` log lines.

| Run date | Signal bar | F | Held | Status | What the pass did | Stored position | Fills (stored) |
|---|---|---|---|---|---|---|---|
| 2026-04-27 | 2026-04-26 | +3.393680 | +1 | held: change bar pending (the roll of section 1.15) | nothing | +1 | none |
| 2026-04-28 | 2026-04-27 | +1.217473 | +1 | free | search answer +1 | +1 | the two ROLL legs |
| 2026-04-29 | 2026-04-28 | -1.319947 | +1 | held: deferral band (opposite sign, abs(F) below 2) | nothing | +1 | none |
| 2026-04-30 | 2026-04-29 | -3.414781 | +1 | free: abs(F) is 2 or more | sign close to flat, then the search asks for -2 and the buffer delivers -1 | -1 | `EXEC_MBT.v.0_20260430_SC` SELL 1 at 76,035; `EXEC_MBT.v.0_20260430` SELL 1 at 76,035 |
| 2026-05-01 | 2026-04-30 | -3.342031 | -1 | free | search answer -1 | -1 | none |

The long was not closed on the first weak negative forecast (-1.32); it was closed the next day when the forecast
reached -3.41, and the same rebalance opened the short.

**Where in the code.** The band, the free and held rows and the close-outs: `one_pass::rebalance`
(`src/optimization/one_pass.cpp:393` to `:414`); the sign close at `:475` to `:482`. The hold set handed in by the
runners is described in `include/trade_ngin/optimization/one_pass.hpp:168`.

**Source.** OURS. The book has neither a hold set nor a band. Its dynamic optimisation starts from a book of zero
every day and only ever takes positions in the direction of the forecast, so, as strategy twenty-five, "Dealing
with costs: buffering", observes, a forecast that crosses zero takes the whole position off. Our search
starts from the held book (section 1.13), so the close has to be a rule of its own, and we defer it while the
opposite forecast is weaker than 2 so that a forecast hovering around zero does not close and reopen a position.
The holds exist because our bars need them (sections 1.2 and 1.3). The band reads the first sleeve's forecast
(`src/optimization/one_pass.cpp:400`).

### 1.13 The whole-contract search

**What it does.** Positions can only be whole contracts, and on a 500,000 book most targets are fractions of one.
The search looks for the whole-contract book that tracks the scaled target book as closely as possible, where
"closely" is the volatility of the difference between the two books, plus a penalty for the cost of getting there
from the book already held. A contract too large to hold can be stood in for by a smaller, correlated one.

**The formula.** Over whole-contract books y on the free contracts, with x~_i = N~_i x u_i the target weights, h
the held book (after any sign close) and c_i the cost in currency of trading one contract of i:

TE(y) = sqrt((y u - x~)' Sigma_opt (y u - x~)) + 100 x the sum over i of abs(y_i - h_i) x c_i / E

The search starts from the held book. Each pass tries one contract up and one contract down in every free
contract and keeps the single step that lowers TE the most; it stops when no step lowers TE by more than 1e-6.
A step is not allowed if it crosses zero against the target's sign, if it moves a contract with a zero target
away from zero, or if it moves away from zero to a position above the per-name cap.

Sigma_opt is the optimiser's own covariance: the sample covariance of the free contracts' adjusted returns over
their last 756 consumed closes, one per date, on the dates all of them share, annualised by the bars a year those
dates show. A contract whose last close is more than five dates behind the newest, or that would leave fewer than
20 common returns, takes a variance of 0.01 on its own diagonal.

**Settings in force.**

| Setting | Value | Where |
|---|---|---|
| `cost_penalty_scalar` (the cost weight) | 100 | `config_template/defaults.json`, `optimization.cost_penalty_scalar` (required on a futures book) |
| Smallest improvement a step must make | 1e-6 | a constant of the code, `src/optimization/one_pass.cpp:532` (the config key `optimization.convergence_threshold` is not read by this search) |
| `max_iterations` (floor on the pass cap) | 100 | `optimization.max_iterations` |
| Closes behind the covariance | 756 | `kOptimiserBars`, `src/strategy/trend_following.cpp:505` |
| Stale rule, minimum common returns, filled variance | 5 dates, 20, 0.01 | `src/optimization/one_pass.cpp:519`, `:14`, `:15` |
| Cost of one contract c_i | the cost model's price of a one-contract fill at the signal close (section 1.16) | `src/portfolio/portfolio_manager.cpp:2134` |

**Worked example.** The chain, the run dated 2026-04-28 (signal bar 2026-04-27, E = 500,000). Thirty-four
contracts were free and two (ZC, ZS) were held on pending change bars. The search took 6 passes from the held book
and ended at TE(y) = 0.048923 (the run's `OPTIMISER` line). These are the five contracts where its answer differs
from the held book; every other free contract was left where it was.

| Contract | Held h | Target N~ | u (one contract's weight) | Cost of one contract c | Search answer y |
|---|---|---|---|---|---|
| 6A | 0 | +0.538622 | 0.143700 | 4.77 | +1 |
| KE | +1 | +0.224617 | 0.067700 | 19.95 | 0 |
| M2K | +1 | +3.280898 | 0.028067 | 1.09 | +3 |
| MNQ | +1 | +0.685970 | 0.109958 | 0.98 | +2 |
| UB | 0 | -0.642125 | 0.231750 | 10.09 | -1 |

The cost term of one step is 100 x c / E: for one M2K contract 100 x 1.0931 / 500,000 = 0.000219, for one KE contract
100 x 19.9549 / 500,000 = 0.003991. A step is taken only if it lowers the tracking error by more than that. MNQ's
answer is 2 against a target of 0.69 while MYM's stays at its held 1 against a target of 2.06: the search reads the
book as a whole through the covariance, not contract by contract.

**Where in the code.** `one_pass::tracking_error` (`src/optimization/one_pass.cpp:145`), `one_pass::admissible`
(`:156`), `one_pass::pass_cap` (`:162`), `one_pass::search` (`:170`), `one_pass::optimiser_covariance` (`:61`).
The design comment is `include/trade_ngin/optimization/one_pass.hpp:15` to `:25`. See
`OPTIMIZER_AND_RISK_DESIGN.md`.

**Source.** CARVER, ADAPTED. This is strategy twenty-five, dynamic optimisation: "Optimising for the best
portfolio" defines the tracking error between the whole-contract book and the unrounded one in portfolio weights;
"The greedy algorithm" adds or removes one contract at a time and keeps the best step; "Dealing with costs: a cost
penalty" adds the cost of trading from the held book, multiplied by 50, and says of that multiplier that "any
number between 10 and 100 will give very similar results". Three things differ. The cost weight is 100, the top of
that range, because on a book this small the cost weight is the setting that governs how often the book trades.
The search starts from the held book and can step either way, where the book starts each day
from a book of zero and only adds in the direction of the target; from the held book a holding that is already
right costs nothing to keep. And the covariance is a sample covariance
of 756 daily closes where the book uses its 32-day volatility with correlations from six months of weekly returns:
one estimator from the series the engine already holds.

### 1.14 The buffer, the rounding, the clip and the trim

**What it does.** The search's answer is not traded in full. If the held book is already close enough to it, no
trade is made at all; otherwise the book moves only part of the way, to the edge of a no-trade band, and is
rounded to whole contracts. The rounded book is then clipped to the per-name cap and re-read by the overlay; if
rounding has pushed a reading over its limit, single contracts are removed until it is back under.

**The formula.**

- B_sigma = max(0.05 x tau, the largest u_i x sqrt(Sigma_opt,ii) over the free contracts with a non-zero search
  answer or a non-zero holding)
- TE_h = sqrt((h - y)' Sigma_opt (h - y)) in weights, with no cost term
- if TE_h <= B_sigma: no trade
- otherwise a = (TE_h - B_sigma) / TE_h and the new book is h + a x (y - h), rounded to the nearest whole contract,
  a half away from zero; if every row of the rounded book equals the held book, nothing is traded although TE_h is
  above the band
- the clip: a free row with abs(n_i) x u_i > L is cut toward zero to floor(L / u_i) contracts
- the trim: while a reading of the rounded book is over its limit (served in the order R, R_jump, R_shock, L_g,
  L_n), remove the one contract among the free rows whose removal lowers that reading the most; at most 5
  contracts a day. What is still over after that is stored as it is and marked in `risk_detail`
  (`over_limit_after_rounding_terms`), or `over_limit_by_hold_terms` when held rows alone keep it over.
- the delivered scale stored as `risk_scale` = the stored book's gross weight / the capped target book's gross
  weight

The band's floor, 0.05 x 0.20 = 0.01, is one percent of annual volatility. The second term is the risk of the
largest single contract in the book: on a 500,000 book one contract can carry several times that one percent, and
a band narrower than one contract's risk would trade on every rounding flip.

**Settings in force.**

| Setting | Value | Where |
|---|---|---|
| `b_sigma_floor` | 0.05, a ratio to tau | `config_template/defaults.json`, `optimization.b_sigma_floor` (required on a futures book) |
| `trim_max` | 5 | `config_template/portfolios/conservative/risk.json`, `modules[0]` |
| `per_name_cap` | 2 | same |
| Rounding | nearest, a half away from zero | `one_pass::round_half_away`, `src/optimization/one_pass.cpp:227` |

**Worked example.** The chain, the same rebalance as section 1.13. From the run's `OPTIMISER` line: TE_h =
0.049760, B_sigma = 0.029154 (set by LE, live cattle, whose one contract weighs 0.199180 of the capital), so
a = (0.04975989 - 0.02915412) / 0.04975989 = 0.414104.

| Contract | Held h | Search answer y | h + a x (y - h) | Rounded | Fill (stored) |
|---|---|---|---|---|---|
| 6A | 0 | +1 | +0.414104 | 0 | none |
| KE | +1 | 0 | +0.585896 | +1 | none |
| M2K | +1 | +3 | +1.828208 | +2 | `EXEC_M2K.v.0_20260428` BUY 1 at 2,806.70 |
| MNQ | +1 | +2 | +1.414104 | +1 | none |
| UB | 0 | -1 | -0.414104 | 0 | none |

Of five moves the search wanted, the band delivered one contract. No row was clipped, the re-read was inside every
limit (stored book: R 0.0838, R_jump 0.2355, R_shock 0.1970, L_g 2.0468, L_n 0.0363) and no contract was trimmed.
`risk_scale` = 2.046785 / 3.221653 = 0.6353, the value stored on the `trading.live_results` row of 2026-04-28.

The same quantities on the eight sized rebalances of the chain (each run's `OPTIMISER` line; `risk_scale` is the
stored column):

| Signal bar | Passes | TE_h | B_sigma | Set by | a | Traded | `risk_scale` |
|---|---|---|---|---|---|---|---|
| 2026-04-23 | 11 | 0.086010 | 0.028708 | LE | 0.666228 | yes | 0.5497 |
| 2026-04-24 | 4 | 0.023563 | 0.028924 | LE | 0 | no (inside the band) | 0.6335 |
| 2026-04-26 | 3 | 0.014777 | 0.010000 | the floor | 0.323255 | yes | 0.7697 |
| 2026-04-27 | 6 | 0.049760 | 0.029154 | LE | 0.414104 | yes | 0.6353 |
| 2026-04-28 | 7 | 0.057657 | 0.029757 | LE | 0.483894 | yes | 0.6622 |
| 2026-04-29 | 11 | 0.091370 | 0.035220 | NG | 0.614532 | yes | 0.7177 |
| 2026-04-30 | 6 | 0.031132 | 0.037248 | NG | 0 | no (inside the band) | 0.7806 |
| 2026-05-01 | 6 | 0.048953 | 0.037680 | NG | 0.230284 | no (outside the band, but every row rounded back to the held book: no fill) | 0.8064 |

On 2026-04-26 most contracts were held (they printed no bar), the free rows held nothing large, and the band fell
to its floor of 0.05 x 0.20 = 0.010. On 2026-05-01 the band asked for a move of 0.230284 of the way and every row
rounded back to the held book, so the run dated 2026-05-02 stored no fill (`one_pass::buffer`,
`src/optimization/one_pass.cpp:268` to `:274`).

**Where in the code.** `one_pass::b_sigma` (`src/optimization/one_pass.cpp:231`), `one_pass::buffer` (`:253`),
`one_pass::clip_to_cap` (`:48`), `one_pass::trim` (`:305`), `one_pass::over_limit` (`:290`); the delivered scale at
`:631` to `:636`.

**Source.** CARVER, ADAPTED for the buffer and for the clip, which is the per-name cap of section 1.11 applied to
the rounded book; OURS for the trim. The buffer's shape is strategy
twenty-five, "Dealing with costs: buffering": measure the tracking error of the held book against the optimised
one without the cost term, do nothing inside a band, otherwise trade the fraction (T - B) / T of the way and round;
the book sets the band at 5 percent of the risk target, 1 percent, which is our floor. What differs is the second
term: the band is never narrower than the risk of the largest single contract in the book. The reason is contract
size on a 500,000 book, where one contract of a large future carries many times the 1 percent band. The clip is the
cap read once more after the rounding, because rounding can put a row over a cap its target was under. The trim
is not in the book, whose overlay scales every position and stops there; ours exists because rounding and held
rows can leave the stored book over a limit the target was under.

### 1.15 Rolls

**What it does.** When the vendor's series moves to the next contract, the position is rolled: it is held through
the change bar, and once the next bar confirms the switch, two ROLL fills are booked, one closing the old contract
and one opening the new. The price gap between the two contracts is never booked as profit or loss.

**The rule.**

- On a change bar the contract is held (section 1.12) and its P&L for that bar is 0: the step is the gap between
  two contracts, and the held contract's own move that day is not known from a continuous series.
- On the confirming bar, each sleeve holding the contract gets two fills of type ROLL at its held quantity: the
  closing leg at the last consumed close before the change, the opening leg at the change bar's close. Realised
  P&L on both is 0.
- Each leg is priced by the cost model as an outright trade. This is an upper bound: a roll traded as a spread
  costs less.
- ROLL fills are mechanical. They are counted apart from STRATEGY fills, are outside round trips and turnover, are
  never netted between sleeves (a ROLL row that carries a netting adjustment is refused, section 1.16), and their
  costs are inside the equity curve and reported in their own column.
- If the id reverts on the next bar the change was a flip: no fills, both bars held with no P&L.

**Settings in force.** None of its own: the rule has no parameter. Live ids are
`EXEC_<symbol>_<date of the confirming bar>_RC` and `_RO`; backtest ids start `RL-`.

**Worked example.** The chain, MBT, one contract held (multiplier 0.1). Bars and stored `trading.positions` and
`trading.executions` rows.

| Date | Bar close | Vendor id | Bar status | P&L stored for the date | What is stored |
|---|---|---|---|---|---|
| 2026-04-24 | 77,960 | 42185193 | session | -26.50 = 0.1 x (77,960 - 78,225) | position 1, id 42185193 |
| 2026-04-25 | no bar | | | 0.00 | position 1 |
| 2026-04-26 | 79,070 | 42013708 | change bar: held | 0.00 (the step of +1,110 is not booked) | position 1, id 42185193 |
| 2026-04-27 | 77,740 | 42013708 | confirms the roll | -133.00 = 0.1 x (77,740 - 79,070) | position 1, id 42013708 |
| 2026-04-28 | 76,680 | 42013708 | session | -106.00 = 0.1 x (76,680 - 77,740) | position 1; the two legs below |

The legs, stored by the run dated 2026-04-28, the run that consumed the confirming bar:

| Fill id | Type | Side | Quantity | Price | Vendor id | Fee | Total cost |
|---|---|---|---|---|---|---|---|
| `EXEC_MBT.v.0_20260427_RC` | ROLL | SELL | 1 | 77,960 (last close before the change) | 42185193 | 2.011 | 2.446734 |
| `EXEC_MBT.v.0_20260427_RO` | ROLL | BUY | 1 | 79,070 (the change bar's close) | 42013708 | 2.011 | 2.448311 |

The same run booked the legs of three more rolls confirmed on that bar (ZL, ZM, ZW): eight legs costing 87.003759
in all, which is that day's stored `daily_roll_costs`. With the one STRATEGY fill of the day (1.093132) the day's
`daily_transaction_costs` is 88.096891.

**Where in the code.** `roll_series::RollTracker::add` (`src/data/roll_series.cpp:121`) and
`roll_series::make_roll_legs` (`:171`); the backtest books the legs at
`src/backtest/backtest_coordinator.cpp:1063` to `:1067`, the live runner at
`apps/strategies/live_portfolio_conservative.cpp:2409`; the zero P&L on a change bar is
`src/live/live_pnl_manager.cpp:118` to `:121`. See `FUTURES_ROLLS.md`.

**Source.** CARVER, ADAPTED for the cost; OURS for the mechanics. Counting a roll as two separate trades, each
paying its own costs, is the book's own conservative assumption in its cost arithmetic (strategy nine, "Removing
expensive trading rules"), and Tactic one, "Contract selection and rolling", section "How to roll to a new expiry
date", describes rolling with individual legs and says a spread trade is cheaper. The mechanics are ours, because
the book chooses its own roll dates from the prices of each dated contract and we follow the vendor's switch on a
continuous series: the hold through the change bar, the confirmation on the next bar, the zero P&L on the change
bar and the legs' prices.

### 1.16 Fees and the cost model

**What it does.** Every fill, STRATEGY or ROLL, is charged a cost made of three parts: the broker's fee per
contract, a share of the bid-ask spread (half, or a quarter on thirteen contracts) widened when the contract is volatile, and a market impact term that grows with
the size of the trade against that day's volume. The same model prices the one-contract cost the search weighs
(section 1.13), so the cost that decides a trade is the cost the trade is charged.

**The formula.** For a fill of q contracts at price P:

- fee = abs(q) x `Fee Per Contract` of the contract's metadata row
- z = (the standard deviation of the contract's last 20 daily log returns - 0.01) / 0.005, kept inside [-2, +2]
- volatility multiplier = 1 + 0.15 x z, kept inside [0.8, 1.5]
- spread, in price units per contract = share x (baseline ticks x volatility multiplier, kept inside the contract's
  minimum and maximum ticks) x tick size, where share is the part of the quoted spread paid: 0.25 on the four rate
  contracts and the nine FX contracts, 0.5 on every other contract
- participation = abs(q) / V, at most 0.10, where V is the volume of the signal bar itself (with the weekend's
  thin bars merged into the session beside them)
- impact, in basis points = k x sqrt(participation), with k = 10 when V is above 1,000,000, 20 above 200,000,
  40 above 50,000, 60 above 20,000, else 80; capped at the contract's maximum impact
- impact, in price units = impact in basis points / 10,000 x P
- spread + impact is capped at the contract's `max_total_implicit_bps` (200 basis points of P unless the contract
  sets its own)
- total = fee + min(spread + impact, that cap) x abs(q) x M

Every cost total (a day's costs, a run's costs, the equity curve, the statistics) is taken after netting: a fill's
own cost minus its signed `netting_adjustment`. The adjustment is non-zero only when two sleeves of one book trade
the same contract in opposite directions on the same day and the account sends a smaller order than the two
sleeves' fills add up to. CONSERVATIVE has one sleeve, so every adjustment is 0 and the cost after netting is the
fill's own cost. A ROLL row is never netted, and a ROLL or BORROW row that carries an adjustment is refused, not
summed (`transaction_cost::unnetted_row_refusal`, `src/transaction_cost/netting.cpp:166`). The roll total is built
two ways that give the same number on any run that is not refused: a backtest passes each leg through that
refusal (`run_cost_totals`, `:192`); a live run adds each leg's own cost as it builds the leg
(`apps/strategies/live_portfolio_conservative.cpp:2439`), leaves ROLL rows out of the netting (`:2556`) and
refuses an adjusted row in the day's sum (`:2576`). See `COST_MODEL.md`.

**Settings in force.**

| Setting | Value | Where |
|---|---|---|
| Fee per contract | IBKR's all-in fee per contract per side, one value per metadata row (the table of section 1.1) | `metadata.contract_metadata."Fee Per Contract"`, set by `migrations/026_contract_metadata_ibkr_fees.sql` |
| Fallback fee | 1.50, used only where a metadata row carries no fee | `include/trade_ngin/transaction_cost/transaction_cost_manager.hpp:100` |
| Tick size, multiplier | the metadata row's `Tick Size` and `Contract Size` | read by `registry_contract_cost_spec`, `src/transaction_cost/transaction_cost_manager.cpp:14` |
| Spread: share of the quoted spread paid | 0.25 on ZN, ZF, ZT, UB, 6A, 6B, 6C, 6E, 6J, 6L, 6M, 6N, 6S; 0.5 on the other 23 contracts | `spread_cost_multiplier`: the per-contract entries of `src/transaction_cost/asset_cost_config.cpp`, default 0.5 in `include/trade_ngin/transaction_cost/asset_cost_config.hpp:32` |
| Spread: baseline ticks | 2 for 6L, 6M, GF, HE, KE, LE, NG, ZC, ZL, ZR, ZS, ZW; 1 for every other contract | `src/transaction_cost/asset_cost_config.cpp` |
| Volatility multiplier: sensitivity, bounds, window | 0.15, [0.8, 1.5], 20 returns | `include/trade_ngin/transaction_cost/spread_model.hpp:39` |
| Volatility multiplier: baseline daily volatility and its spread | 0.01, 0.005 | `src/transaction_cost/spread_model.cpp:58`, `:59` |
| Impact: volume floor, participation cap | 100 lots, 0.10 | `include/trade_ngin/transaction_cost/impact_model.hpp:37` |
| Impact: coefficient by volume | 10, 20, 40, 60, 80 basis points | `ImpactModel::get_impact_k_bps`, `src/transaction_cost/impact_model.cpp:46` |
| Impact cap per contract | 30 to 100 basis points by contract | `src/transaction_cost/asset_cost_config.cpp` |

**Worked example.** The chain, two stored fills of the run dated 2026-04-28. The volume and the volatility
multiplier are from the run's `COST_FEED` and `ROLL_LEG` log lines; the stored columns are
`commissions_fees`, `implicit_price_impact` and `total_transaction_costs`.

| Step | M2K BUY 1 at 2,806.70 (STRATEGY) | MBT SELL 1 at 77,960 (ROLL closing leg) |
|---|---|---|
| Fee | 1 x 0.614 = 0.614 | 1 x 2.011 = 2.011 |
| Volume V | 76,197 | 79,305 (Sunday's 8,550 merged with Monday's 70,755) |
| Volatility multiplier | 1.103104 | 1.3 |
| Spread | 0.5 x 1 x 1.103104 x 0.1 = 0.055155 | 0.5 x 1 x 1.3 x 5 = 3.250000 |
| Impact coefficient k | 40 (V between 50,000 and 200,000) | 40 |
| Impact in basis points | 40 x sqrt(1 / 76,197) = 0.144908 | 40 x sqrt(1 / 79,305) = 0.142040 |
| Impact in price units | 0.144908 / 10,000 x 2,806.70 = 0.040671 | 0.142040 / 10,000 x 77,960 = 1.107341 |
| Spread + impact (stored `implicit_price_impact`) | 0.095826 | 4.357341 |
| In currency, x 1 x M | 0.095826 x 5 = 0.479132 | 4.357341 x 0.1 = 0.435734 |
| Total (stored) | 0.614 + 0.479132 = 1.093132 | 2.011 + 0.435734 = 2.446734 |

**Where in the code.** `TransactionCostManager::calculate_costs`
(`src/transaction_cost/transaction_cost_manager.cpp:127`; the fee at `:185`, the spread at `:219`, the impact at
`:223`, the conversion to currency at `:254`); `SpreadModel::calculate_spread_price_impact`
(`src/transaction_cost/spread_model.cpp:13`) and `SpreadModel::calculate_volatility_multiplier` (`:41`);
`ImpactModel::calculate_market_impact` (`src/transaction_cost/impact_model.cpp:12`); the volume a fill is priced
on is described in `include/trade_ngin/live/futures_cost_feed.hpp:47`. The cost after netting is
`transaction_cost::net_cost` (`include/trade_ngin/transaction_cost/netting.hpp:97`) and
`transaction_cost::add_net_costs` (`src/transaction_cost/netting.cpp:183`).

**Source.** OURS. The book costs a trade as the commission plus half the bid-ask spread, turned into Sharpe-ratio
units for its speed limits (strategy three, "Risk adjusted costs"), and notes that a large trader must also
estimate market impact. Our model keeps those two parts, takes the commission from the broker's schedule per
contract, widens the spread with volatility and adds an impact term on the day's own volume, so that a thin bar
costs more to trade than a full session. The fee is the broker's own schedule per contract, so the fee charged is
the one paid, and cost totals are after netting between the sleeves of one book because that is what the account
pays.

### 1.17 The statistics as they are stored

**What it does.** A backtest stores one row of summary statistics per run in `backtest.results`, computed from its
equity curve and its fills. A live run stores running statistics on each day's `trading.live_results` row,
computed from the stored daily rows up to that day. Both are statistics of the ACCOUNT (the equity curve V), not
of the sizing capital. The sizing capital of each day is stored beside them in `risk_detail`, so a measure on the
sizing capital can be computed from the stored rows (section 4), but none is stored as a column.

**The backtest row.** The equity curve has one row per bar date (Sundays included: about 312 rows a year). The
first 256 rows are warm-up and are dropped before anything is computed. With r_t = V_t / V_t-1 - 1 over the rows
left:

| Column of `backtest.results` | Definition | Base | Annualisation |
|---|---|---|---|
| `total_return` | last equity / first equity after warm-up - 1 | account | none |
| `volatility` | standard deviation of r (dividing by n) x sqrt(252) | account | 252 |
| `sharpe_ratio` | mean(r) x 252 / `volatility`; no cash rate is subtracted | account | 252 |
| `sortino_ratio` | mean(r) x 252 / `downside_volatility` | account | 252 |
| `downside_volatility` | sqrt(the mean of r squared over the negative r only) x sqrt(252) | account | 252 |
| `max_drawdown` | the largest fall of the equity from its running peak, as a fraction of the peak | account | none |
| `calmar_ratio` | mean(r) x 252 / `max_drawdown` | account | 252 |
| `var_95`, `cvar_95` | the 5th percentile daily loss, and the mean of the losses beyond it | account | daily |
| `total_trades` | STRATEGY fills that reduce or close a position (round trips); ROLL legs are not counted | fills | count |
| `win_rate`, `avg_win`, `avg_loss`, `max_win`, `max_loss`, `profit_factor`, `avg_holding_period` | per round trip: contracts closed x (exit price - entry price), in price points with NO contract multiplier, less the fill's cost after netting in currency; an open trade's entry price is carried across a roll by the gap between the two legs | fills | none |
| `transaction_costs` | the sum of every fill's cost after netting (STRATEGY and ROLL) | fills | none |
| `roll_costs`, `total_roll_fills` | the ROLL legs' own costs, and their count | fills | none |
| `beta`, `correlation` | the slope and the correlation of each day's return on the return of the day before (a lag-one autocorrelation of the account's own returns, not a market beta) | account | none |

The annualisation constant is 252 while the equity curve has about 312 rows a year, so a stored annualised figure
is stated on a 252-row year.

**The live row.** There is one row per calendar date the run is made for (about 365 a year; a date with no new
bar stores a zero return).

| Column of `trading.live_results` | Definition | Base | Annualisation |
|---|---|---|---|
| `daily_pnl` | the day's settlement P&L less the day's transaction costs | account | none |
| `daily_return` | `daily_pnl` / the previous row's `current_portfolio_value` x 100 (percent) | account | none |
| `total_cumulative_return` | (`current_portfolio_value` - 500,000) / 500,000 x 100 | account | none |
| `total_annualized_return` | ((1 + cumulative return)^(252 / days) - 1) x 100, days counted from the book's first trading day | account | 252 |
| `volatility` | standard deviation of the stored `daily_return` values (dividing by n) x sqrt(252), in percent | account | 252 |
| `sharpe_ratio` | `total_annualized_return` / `volatility` | account | 252 |
| `downside_deviation`, `sortino_ratio` | sqrt(the mean of the squared negative returns) x sqrt(252), and `total_annualized_return` over it | account | 252 |
| `max_drawdown` | the largest fall of the stored portfolio value from its running peak, percent of the peak | account | none |
| `best_day`, `worst_day`, `winning_days`, `losing_days`, `win_rate`, `profit_factor` | over the stored daily rows | account | none |
| `gross_notional`, `net_notional`, `gross_leverage`, `net_leverage`, `margin_posted`, `equity_to_margin_ratio` | the stored book's notional, leverage and margin measures (defined in `LIVE_RUN_CYCLE.md`) | see that document | none |
| `risk_scale` | the delivered scale of section 1.14 | sizing capital | none |
| `risk_detail` | `sizing_capital`, `account_value`, `risk_requested` (m), `binding_term`, the over-limit marks, `overlay_blind` | both | none |
| `daily_transaction_costs`, `total_transaction_costs`, `daily_roll_costs`, `total_roll_costs` | costs after netting, and the ROLL part | fills | none |

A row dated D is final only after the run of D+1, which books bar D's move onto it. See `LIVE_RUN_CYCLE.md` for
the column dictionary.

**Settings in force.**

| Setting | Value | Where |
|---|---|---|
| Backtest annualisation constant | 252 | `src/backtest/backtest_metrics_calculator.cpp:75`, `:125`, `:159`, `:715` |
| Backtest warm-up rows dropped | 256 (the longest moving average) | `BacktestCoordinator::calculate_warmup_days`, `src/backtest/backtest_coordinator.cpp:1788` |
| Live annualisation constant | 252 | `src/live/live_historical_metrics.cpp:10`, `src/live/live_metrics_calculator.cpp:54` |
| Cash rate in the Sharpe ratio | 0 | the same functions |

**Worked example: the rows a backtest statistic is computed on.** The backtest's equity curve has 4,992 rows. The
first 256 are warm-up and are dropped, which leaves 4,736 rows, from 2011-08-04 to 2026-10-07, and 4,735 daily
returns over 15.18 years: about 312 a year, against the constant 252 of the table above.

**Worked example: the live row.** An example of the arithmetic of one column: the chain, the stored row dated
2026-04-30. `daily_return` = `daily_pnl` / the previous row's `current_portfolio_value` x 100 =
-5,134.3919 / 511,005.961 x 100 = -1.004762, the stored value.

**Where in the code.** The backtest: `BacktestMetricsCalculator::calculate_all_metrics`
(`src/backtest/backtest_metrics_calculator.cpp:670`), `calculate_volatility` (`:111`), `calculate_sharpe_ratio`
(`:58`), `calculate_sortino_ratio` (`:81`), `calculate_drawdowns` (`:174`), `calculate_trade_statistics` (`:414`),
`filter_warmup_period` (`:777`); the cost totals at `src/backtest/backtest_coordinator.cpp:474` to `:476`; the
row is written by `BacktestResultsManager::save_summary_results` (`src/storage/backtest_results_manager.cpp:109`)
and the curve by `save_equity_curve` (`:126`). Live: `LiveHistoricalMetricsCalculator::calculate`
(`src/live/live_historical_metrics.cpp:89`), `LiveMetricsCalculator::calculate_annualized_return`
(`src/live/live_metrics_calculator.cpp:37`); the row is first written by
`LiveResultsManager::save_live_results` (`src/storage/live_results_manager.cpp:211`) and finalised by the next
day's run (`LIVE_RUN_CYCLE.md`). The `risk_detail` column is `migrations/020_risk_detail.sql`.

**Source.** OURS. These are the engine's own reporting conventions. The book reports its strategies on fixed
capital with non-compounded
percentage returns, annualises by 256 business days and reads the Sharpe ratio on that base; the stored columns
are compounded returns of the account annualised by 252. Section 4 says which base to read each measure on.

---

## 2. Where we depart from the book

One row per departure: what the book does, what we do, why, and whether it changes live trading. The Source line
of the section named in each row gives the book's citation.

| # | Stage | What the book does | What we do | Why | Changes live trading |
|---|---|---|---|---|---|
| 1 | Universe (1.1) | Sizes each contract over its whole price history; its data set backfills some micro contracts with the larger contract's prices | The E-mini is traded before 2019-05-06, the micro from it, with a switch at the day's target | Only contracts that were listed could be held | No (a live run trades no predecessor) |
| 2 | Universe (1.1) | No such case | A vendor id change that is not a roll is read as the old id | A relabel booked as a roll charges two legs and drops a day's move | Yes |
| 3 | Bars (1.2) | One usable price per business day | A per-symbol session classifier; junk bars withheld, missing bars held | Our feed has Sunday rows, stubs and corrupt prints | Yes |
| 4 | Prices (1.3) | Back-adjusts from the prices of each dated contract and trades through the roll | Back-adjusts a continuous series at the vendor's switch; the change bar's return is 0 | No per-contract bars | Yes |
| 5 | Volatility (1.4) | Mean-centred daily standard deviation x 16; no seed or floor stated; a ten-year long-run average | Annual factor counted from the bars; the same centring, with a seeded and floored variance; a long run of 2,520 consumed bars | Sunday rows make about 300 bars a year; a flat series must not give zero | Yes |
| 6 | Trend rules (1.5) | Attenuation from the quantile of relative volatility over the whole history, on a trend and carry strategy | Quantile of the short volatility against its trailing 2,520 values, on trend alone | One fixed window, no second average | Yes |
| 7 | Rule removal (1.6) | Removes a rule where its cost exceeds 0.15, from its own costs and its table of turnovers | The same rule, limit and table of turnovers; our engine's 16-year median cost per contract and the rolls it confirms; a fixed, reviewed list | The procedure is the book's; the costs must be the ones this engine charges | Yes |
| 8 | Slow rule (1.7) | Advises against adjustments that treat long and short differently | An equity index short needs both slow speeds negative | A fast-only short against a slow uptrend bets against the drift | Yes |
| 9 | Weights (1.8) | Equal shares by asset class, then by group, then by instrument | One layer by metadata sector, with a 50 percent cap on one contract's share of its sector | The metadata carries one sector field; a one-contract sector must not take a whole share | Yes |
| 10 | Sizing capital (1.9) | Reports its backtests on fixed capital; for a live account offers full compounding and half compounding | Half compounding, in the backtest as in live | The backtest and the live book size on the same capital | No (the difference is in the backtest; a live run sizes by one of the book's two forms) |
| 10a | Target (1.10) | The position formula with no bound on the volatility | The sizing volatility is kept inside [0.01, 1.0]; a forecast that is not a number sizes at 0 | A volatility near zero would give an unbounded target | Yes |
| 11 | Overlay (1.11) | Four readings; limits 1.5, 3.75 and 3.25 times the target and leverage 20, each the rounded 99th percentile of the reading's history on its own portfolio; scales every position | Five readings (net leverage added); limits 2.25, 4.5 and 4.0 times the target, 8.0 and 6.0, fixed in `risk.json`; held rows not scaled; the covariance is a sample covariance of 252 dates of daily returns where the book uses its 32-day volatility with correlations from weekly returns; the jump volatility is taken over the trailing 2,520 values of the engine's short estimate; a contract with fewer than 120 returns is out of the two covariance readings; a blind day keeps the two leverage readings | The book sets each limit from the history of its own portfolio and notes that another set of instruments calls for other limits; our book holds rows it may not trade | Yes |
| 12 | Per-name cap (1.11) | Three position limits: at the maximum forecast, at a maximum leverage, and by open interest | The leverage limit only, at 2, on the target, in the search and on the rounded book | The other two overlap it or do not bind at this size | Yes |
| 13 | Holds and the sign close (1.12) | No holds; a position closes in full when its forecast changes sign | Non-session, missing and change bars are held; a held contract's target is set to its holding, so no other contract stands in for it; the sign close waits until the opposite forecast reaches 2; a contract that stops signalling is closed with one fill | Our bars need the holds; a forecast near zero should not close and reopen | Yes |
| 14 | Search (1.13) | Greedy from a book of zero each day; cost multiplier 50 | Greedy from the held book, steps either way; cost multiplier 100 | A holding that is already right costs nothing to keep; the cost weight governs how often a small book trades | Yes |
| 15 | Search covariance (1.13; the overlay's covariance is in row 11) | 32-day volatility with correlations from six months of weekly returns | Sample covariance of 756 daily closes | One estimate from the series the engine already holds | Yes |
| 16 | Buffer (1.14) | A band of 5 percent of the risk target (1 percent) | The larger of that and the risk of the largest single contract in the book | One contract of a large future is many times 1 percent of a 500,000 book | Yes |
| 17 | Trim (1.14) | None | The rounded book, once clipped to the per-name cap (row 12), is trimmed, at most 5 contracts a day, when a reading is over | Rounding and held rows can put the stored book over a limit | Yes |
| 18 | Rolls (1.15) | Chooses its roll dates; recommends spread trades; counts two trades a roll in its cost arithmetic | Follows the vendor's switch; holds the change bar; books two outright legs at full cost on the confirming bar | No per-contract bars; an upper bound on the cost | Yes |
| 19 | Costs (1.16) | Commission plus half the spread, risk adjusted | Per-contract broker fee, a spread widened by volatility, an impact term on the day's volume | A thin bar costs more to trade; the fee must be the one paid | Yes |
| 20 | Netting (1.16) | One strategy per account | Cost totals after netting between the sleeves of one book | It is what the account pays | BASE only |
| 21 | Statistics (1.17) | Non-compounded returns on fixed capital, 256 business days | Stored columns are compounded account returns, annualised by 252 | The engine's reporting convention | No (reporting only) |

---

## 3. What was tried and not adopted

These options are not part of the design. Each row says what the option is and why the design does not use it.

| Tried | Why not adopted |
|---|---|
| Blanket speed cuts: dropping the fastest one, two or three speeds on every contract | The six speeds are the design. A rule is removed only from a contract on which it is too expensive to trade, which is the book's own procedure and what section 1.6 does |
| Equity index shorts off | The system is long and short by design. The slow rule of section 1.7 is the form in force: it conditions an equity index short on the slow speeds and does not forbid it |
| A contract-size guard: a contract is not held while one contract's own risk exceeds a set share of the risk target | It takes a contract out of the book when its volatility is highest, and it has no hysteresis, so a contract near the threshold enters and leaves. The per-name cap (1.11) and the one-contract term of the buffer (1.14) are the design's account of contract size |
| A buffer twice as wide | A wider band lets the held book stand further from its target than the design allows, so the book no longer holds its risk target |
| Other cost weights in the search: 50 and 200 against 100 | The weight sets how often the book trades. 100 is the top of the range the book gives and is chosen because the book is small; 50 trades more by construction, and a value above 100 is outside the book's range |
| Sizing capital, proportional drawdown: E = 500,000 x account / the account's peak | Compelling, and not adopted: it is not in the book, we did not want to differ from Carver, and we chose the conservative route |
| Sizing capital, capped account: E = min(500,000, account) | It is not the book's rule: under half compounding a loss from the high-water mark comes off the sizing capital at once, and this form takes nothing off while the account is above the start |
| Sizing capital, half rate: only half of the drawdown comes off the start | A tuned number with no source in the book |
| Sizing capital, floor at 75 percent of the start | A tuned number with no source in the book, whose rule lets the capital fall with every loss |
| Sizing capital, yearly reset to the start | A tuned rule with no source in the book: it restores size on a calendar date and not on recovered profit |
| Sizing capital, fixed 500,000 | The book warns against running real money on fixed capital, because size must fall after a loss |
| A flat fee of 1.50 per contract in place of the per-contract fees | Fees follow the broker's per-contract schedule. The fee is an input to the trade decision, and a flat 1.50 prices an equity index micro at more than twice its fee and every other contract below its fee |
| Other account sizes | The book is built for 500,000 |
| Switching other contracts to micro or mini sizes | The contract set of section 1.1 is the design. With small contracts the no-trade band falls toward its floor, because it is sized by the risk of one contract, and the book trades every small change in its targets. The four equity index micros are a different matter: they are in the universe, from their listing date |
| The book's per-instrument position limit at the maximum forecast | The per-name cap already bounds each contract, so this would be a second, overlapping per-name bound on a chosen weight |
| The book's own overlay limits (1.5, 3.75 and 3.25 times the target) | They are the limits of the book's own portfolio, and the book notes that another set of instruments calls for other limits (section 2, row 11) |
| Sector caps: a limit on the number of contracts held per sector | A cap on the count lowers the risk held, so the book no longer holds its risk target. The balance between sectors is already in the instrument weights (1.8) |
| Search hysteresis: the search pays a multiple of a contract's own risk to trade it | A second trading threshold on top of the cost term and the buffer, and one under which a contract whose own risk is large against the account is seldom or never traded |
| The book's own buffer width: a fixed 1 percent (0.05 x the risk target) with no one-contract term | One contract of a large future is many times 1 percent of a 500,000 book, so a band that narrow trades on rounding flips |
| A one-step trim: a reduction of two or more contracts is delivered in one trade and a one-contract gap is held | A second, per-contract delivery rule on top of the whole-book buffer |
| A roll back-refusal: refuse a vendor switch back into the contract just rolled out of | The rule cannot tell a whipsaw from an early switch, and a refused switch leaves a contract held under an id the vendor's series has left. The engine follows the vendor's switch (1.15) |
| Other switch-day rules for a listing date: close and let the pass reopen; carry ten for one; carry only held pairs to target | Opening at the day's target shows one exit and one entry at the signal's own size |

---

## 4. Reading the results

**The book is built for 500,000.** At that size most targets are a fraction of a contract, so the book is limited
to the contracts that fit, and the no-trade band is sized by the risk of one contract (section 1.14).

**One backtest is one path.** A whole-contract book at 500,000 is a sequence of single-contract decisions, and
moving the start of a window by a month or two deals those decisions differently. A result is therefore read
across the standard windows and across start dates, never from one run. A difference between two settings that is
smaller than the difference between two start dates of the same setting is not a finding.

**The standard windows.** A futures backtest is named by the years it TRADES, not by its lookback. The runner
takes a whole-year lookback (`backtest.lookback_years`) and an end date (`backtest.frozen_end_date`, or the wall
clock when the key is absent; `src/core/config_loader.cpp:807` to `:866`), and the first 256 rows of every window
(about ten months) are warm-up with no trading. Windows that are compared end on the same date.

| Name | Lookback (`backtest.lookback_years`) | Years traded |
|---|---|---|
| 2-year | 3 | about 2.2 |
| 5-year | 6 | about 5.2 |
| 10-year | 11 | about 10.2 |
| 16-year | 16 (the data begin in 2010-06) | about 15.2 |

A result is labelled with the window's first traded day and its end date (for the backtest used in this document:
first sized signal bar 2011-08-04, stored on the row dated 2011-08-05; end 2026-10-07). A window that trades before
2019-05-06 holds E-minis before that date (section 1.1).

**Which base each measure is read on.**

| Measure | Base | Why |
|---|---|---|
| Volatility | the sizing capital: the day's net P&L over the day's E | Positions are sized on E and the 20 percent target applies to it. Volatility on the account falls as set-aside profits accumulate and says nothing about whether the target was held |
| Drawdown | the account, in percent and in dollars | It is what the account loses. A drawdown summed in points of a shrinking sizing capital overstates the picture |
| Sharpe ratio | both, each labelled | On the sizing capital each day is weighted by one over E, so a path whose sizing capital is low in a losing stretch reads lower on that form |
| Return | the account for what was made; points of the sizing capital for the strategy's return per unit of risk | |
| Costs and round trips | per year traded | Warm-up rows carry none |

The stored statistics of section 1.17 are all on the account; `risk_scale` and `risk_detail` are the two columns
that carry the sizing capital. A measure on the sizing capital is computed from the stored daily P&L and the stored
`risk_detail.sizing_capital` of the same row.

**What a stored statistic is annualised by.** The stored backtest columns use 252 on a curve of about 312 rows a
year, and the stored live columns use 252 on about 365 rows a year. Two figures are comparable only when they are
computed on the same row grid with the same constant.

---

## 5. Related documents

| Document | What it adds |
|---|---|
| `OPTIMIZER_AND_RISK_DESIGN.md` | the daily rebalance step by step for an engineer |
| `RISK_MODULES.md` | the overlay as a module, its config and its refusal contract |
| `COST_MODEL.md` | the cost model's inputs, the weekend volume merge, netting between sleeves |
| `FUTURES_ROLLS.md` | change bars, confirmation, the legs, the known limits |
| `LIVE_RUN_CYCLE.md` | the live day, what each table holds, the column dictionary |
| `DATA_SOURCES_OF_TRUTH.md` | the tables and series the engine reads |
| `CONFIG_GUIDE.md` | the config files and their keys |
