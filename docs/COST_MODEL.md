# Cost model

What a fill costs in this engine, where each number comes from, what is stored, and how the
cost of a symbol traded by two sleeves on one day is netted. It covers futures and equities,
the backtest and the live runners. The code is in `src/transaction_cost/` and
`include/trade_ngin/transaction_cost/`; the design comments in those headers are the detailed
reference and this document points at them.

Companion documents: `docs/FUTURES_ROLLS.md` (the two ROLL legs), `docs/OPTIMIZER_AND_RISK_DESIGN.md`
(how the cost of one contract enters the whole-contract search), `docs/AVERAGE_PRICE_LIFECYCLE.md`
(section 8b, the equity cost feed), `docs/LIVE_RUN_CYCLE.md` (the stored tables),
`docs/TREND_FOLLOWING_SYSTEM.md` (the strategy end to end).

---

## 1. One formula

A fill of `q` units (contracts or shares, signed: a sell is negative) at the reference price `P`
costs

```
fee       = |q| x fee_per_contract                                  (futures)
          = min(cap, max(1.00, |q| x 0.005)),  cap = 0.01 x |q| x P (equities)
spread    = spread_cost_multiplier x clamp(baseline_ticks x vol_mult, min_ticks, max_ticks) x tick_size
impact    = min(k_bps(V) x sqrt(clamp(|q| / max(V, 100), 0, 0.1)), max_impact_bps) / 10000 x P
implicit  = min(spread + impact, max_total_implicit_bps / 10000 x P)        price units per unit
slippage  = implicit x |q| x point_value                                     dollars
total     = fee + slippage                                                   dollars
```

`TransactionCostManager::calculate_costs` (`src/transaction_cost/transaction_cost_manager.cpp:127`)
is the only place this is computed. `P` is the close of the signal bar (the last bar before the
fill day): the fill price itself carries no cost, and the cost is charged separately against the
day's P&L. `V` is the volume the impact term reads (section 5). The stored row keeps the four
parts (section 8).

The sign of `q` is read in one place only, the equity sell-side regulatory fees, which are
switched off on every config (section 3). Every other term takes `|q|`.

---

## 2. Futures: the contract comes from the metadata row

A futures fill is priced with the same contract the strategy sizes with: the symbol's row of
`metadata.contract_metadata`, read through the instrument registry and looked up by the symbol
without its continuous-contract suffix (`registry_contract_cost_spec`,
`transaction_cost_manager.cpp:14`; `resolve_asset_config`, `:54`).

| Metadata column | Becomes | Used for |
|---|---|---|
| `"Contract Size"` | `point_value` | dollars per one price unit: slippage = implicit x contracts x point_value |
| `"Tick Size"` | `tick_size` | one tick in price units: the spread term |
| `"Fee Per Contract"` | the fee | dollars per contract per side |

A future that the calibration table names, or whose metadata row has a positive `"Contract Size"`,
and that lacks a usable `"Contract Size"` or a usable `"Tick Size"` (either one is enough), is not
priced on a guessed constant: its spread and impact are 0, its fee is still charged (the row's fee
when the row has a positive `"Contract Size"` and a usable fee, else the fallback 1.50), and an ERROR names it once per cost manager
(`transaction_cost_manager.cpp:68` to `:93`, `:212` to `:216`). A symbol that has no calibration
entry and no metadata row with a positive `"Contract Size"` (`registry_contract_cost_spec` gives
none for such a row, `:33` to `:35`) keeps the generic default of section 4
(`transaction_cost_manager.cpp:71` to `:73`); no contract of the book is in that case.

### The fee

`"Fee Per Contract"` is a text column added by migration 014 and priced row by row by migration
026 from Interactive Brokers' published futures schedule: the commission at the lowest volume
tier, plus the exchange fee recovery charge (the non-member column), plus the regulatory fee
recovery charge, in dollars per contract per side. The migration file lists the pages, the date
they were read and the three parts of every row (`migrations/026_contract_metadata_ibkr_fees.sql:63`
to `:103`).

| Fee | Contracts |
|---:|---|
| 0.610 | MYM |
| 0.614 | MES, MNQ, M2K |
| 1.510 | ZT, ZF |
| 1.660 | ZN |
| 1.810 | UB |
| 2.011 | MBT |
| 2.240 | YM |
| 2.247 | ES, NQ, RTY |
| 2.360 | CL, HO, RB |
| 2.460 | NG |
| 2.461 | 6A, 6B, 6C, 6E, 6J, 6L, 6M, 6N, 6S |
| 2.510 | PL, GC, HG, SI |
| 2.961 | GF, HE, LE |
| 3.010 | ZC, ZL, ZM, ZR, ZS, ZW, KE |

That is 40 rows: the 36 contracts of the book and the four E-minis (ES, NQ, YM, RTY) the book
holds before the micros' listing date. An E-mini fill is charged the E-mini's own fee.

Migration 026 also adds the CHECK `contract_metadata_fee_per_contract_positive`: the cell must be a
plain decimal number above zero (`026_contract_metadata_ibkr_fees.sql:169` to `:173`). Without it
an empty cell is legal text and loads as a fee of 0.

**The fallback.** The cost manager carries one default fee, `explicit_fee_per_contract = 1.50`
(`TransactionCostManager::Config`, `include/trade_ngin/transaction_cost/transaction_cost_manager.hpp:100`).
It is a compiled default, not a JSON key: the portfolio backtest, both live futures runners and the
`PortfolioManager` build their cost managers with it. It is charged only when the metadata gives
no fee for the contract (`transaction_cost_manager.cpp:185`):

| Case | What the registry does | Fee charged |
|---|---|---|
| The row has a usable fee | reads it (`src/instruments/instrument_registry.cpp:422` to `:427`) | the row's |
| The table has no `"Fee Per Contract"` column | one WARN at load (`instrument_registry.cpp:157`) | 1.50 on every future |
| The cell is not a number, or is negative | a WARN naming the row (`instrument_registry.cpp:429`) | 1.50 for that row |
| The symbol has a calibration entry and no metadata row (ZB) | nothing to read | 1.50, and no spread or impact (above) |

Every one of the 40 rows has a figure on the schedule, so 1.50 is charged on no row.

---

## 3. Equities: the fee schedule

An equity is priced per share (`point_value` 1). Every equity config carries the same schedule,
Interactive Brokers Pro Fixed (`get_equity_default_config`,
`src/transaction_cost/asset_cost_config.cpp:613`; the field comments in
`include/trade_ngin/transaction_cost/asset_cost_config.hpp:44` to `:81`):

| Item | Value | Field |
|---|---|---|
| Commission | 0.005 per share | `commission_per_unit` |
| Minimum per order | 1.00 | `min_commission_per_order` |
| Maximum per order | 1 percent of trade value | `max_commission_pct = 0.01` |
| SEC fee and FINRA TAF on sells | not charged: the Fixed schedule includes them | `apply_regulatory_fees = false` |

The maximum is applied after the minimum (`transaction_cost_manager.cpp:180`), so on a very small
order the 1 percent cap wins over the 1.00 floor. The regulatory-fee code (20.60 per million of
sale proceeds, 0.000195 per share capped at 9.79) stays in place for a config that turns the flag
on; none does.

A short equity position in the backtest also accrues an overnight borrow fee, booked as its own
BORROW row (section 7).

---

## 4. The spread model and the impact model: every constant

### Spread

`SpreadModel::calculate_spread_price_impact` (`src/transaction_cost/spread_model.cpp:13`). The
spread is anchored to the tick, never to the day's range. `vol_mult` widens or narrows the
baseline number of ticks; the result is clamped; the multiplier turns a quoted spread into the
one-way cost (0.5 is half the spread, 0.25 models a resting order).

```
sigma    = sample standard deviation of the symbol's last 20 log returns
z        = clip((sigma - 0.01) / 0.005, -2, 2)
vol_mult = clip(1 + 0.15 x z, 0.8, 1.5)          (1.0 with fewer than 2 returns)
```

The baseline (1 percent a day) and the scale (0.5 percent) are fixed numbers, the same for every
instrument (`calculate_volatility_multiplier`, `spread_model.cpp:41`). On a bar where the contract
changes, the return fed to the window is 0 (`record_log_return`, `transaction_cost_manager.cpp:288`).
An equity's spread is never below one tick (half a tick for a tick-constrained name),
`spread_model.cpp:27`.

### Impact

`ImpactModel::calculate_market_impact` (`src/transaction_cost/impact_model.cpp:12`): the square
root of the share of the day's volume the order takes, times a coefficient chosen by that volume.

| Volume `V` | `k_bps` |
|---|---:|
| above 1,000,000 | 10 |
| above 200,000 | 20 |
| above 50,000 | 40 |
| above 20,000 | 60 |
| otherwise | 80 |

### Constants in force

None of these is a JSON key. All are compiled. `defaults.json` carries
`execution.commission_rate` and `execution.slippage_bps`; neither is read by the cost model or by
any futures path, and no fill is priced with them (their one reader, the equity live runner, prints
them in its start banner, `apps/strategies/live_equity_mean_reversion.cpp:266-267`, `:535-536`).

| Constant | Value | Where |
|---|---|---|
| Fallback futures fee | 1.50 | `transaction_cost_manager.hpp:100` (`Config::explicit_fee_per_contract`) |
| Volatility sensitivity `lambda` | 0.15 | `spread_model.hpp:39` (`VolatilityConfig`) |
| `vol_mult` bounds | 0.8 to 1.5 | `spread_model.hpp:39` |
| Return window | 20 returns | `spread_model.hpp:39` (`lookback_days`) |
| Baseline sigma, sigma scale | 0.01, 0.005 | `spread_model.cpp:58`, `:59` |
| z clip | -2 to 2 | `spread_model.cpp:65` |
| `k_bps` tiers | table above | `impact_model.cpp:46` (`get_impact_k_bps`) |
| Volume floor `min_adv` | 100 | `impact_model.hpp:37` (`ImpactModel::Config`) |
| Participation clamp | 0 to 0.1 | `impact_model.hpp:37` |
| Volume window length | 20 observations (equities; a future's window holds one, section 5) | `impact_model.hpp:37` (`adv_lookback_days`) |
| Volume when none was ever fed | 100,000 | `transaction_cost_manager.cpp:117` |
| `spread_cost_multiplier` default | 0.5 | `asset_cost_config.hpp:32` |
| `max_total_implicit_bps` default | 200 (a WARN when it binds) | `asset_cost_config.hpp:84`, `transaction_cost_manager.cpp:241` |
| Per-contract futures calibration | table below | `asset_cost_config.cpp:18` to `:489` (`initialize_default_configs`) |
| A future with no entry of its own | baseline 2, min 1, max 10 ticks, multiplier 0.5, impact cap 100 bps | `asset_cost_config.cpp:598` (`get_default_config`) |
| Equity tiers | table below | `asset_cost_config.cpp:634` (`get_tiered_equity_config`) |

The per-contract futures entries hold calibration only. Their `tick_size` and `point_value`
fields are overwritten from the metadata row (section 2).
The table lists every futures entry in the code; ZB has an entry and no metadata row, and is not in
the book. The same function also holds fixed entries for ten equity symbols
(`asset_cost_config.cpp:496` to `:553`); `register_equity_costs_from_bars` replaces the entry of
every symbol a runner registers, on every live run and every backtest cycle (section 5).

| Contracts | baseline ticks | min | max | multiplier | impact cap (bps) |
|---|---:|---:|---:|---:|---:|
| ES, NQ, YM, MES, MNQ, MYM, CL, GC | 1 | 1 | 5 | 0.5 | 50 |
| RTY, M2K, SI, HG, HO, RB, ZM | 1 | 1 | 5 | 0.5 | 60 |
| PL | 1 | 1 | 5 | 0.5 | 70 |
| MBT | 1 | 1 | 10 | 0.5 | 80 |
| ZC, ZS, ZW, KE, ZL | 2 | 1 | 5 | 0.5 | 60 |
| GF, HE, LE | 2 | 1 | 5 | 0.5 | 80 |
| NG | 2 | 1 | 10 | 0.5 | 80 |
| ZR | 2 | 1 | 10 | 0.5 | 100 |
| ZT, ZF, ZN, ZB, UB | 1 | 1 | 3 | 0.25 | 30 |
| 6A, 6B, 6C, 6E, 6J, 6S | 1 | 1 | 5 | 0.25 | 40 |
| 6N | 1 | 1 | 5 | 0.25 | 50 |
| 6L, 6M | 2 | 1 | 10 | 0.25 | 80 |

Equity tiers, by the 20-bar average volume in shares (tick 0.01, or 0.0001 below a price of 1.00;
multiplier 0.5):

| Tier | Average volume above | baseline ticks | min | max | impact cap (bps) | total implicit cap (bps) |
|---|---:|---:|---:|---:|---:|---:|
| Mega | 10,000,000 | 1 | 1 | 3 | 50 | 75 |
| Large | 2,000,000 | 2 | 1 | 5 | 75 | 100 |
| Mid | 500,000 | 3 | 1.5 | 8 | 100 | 150 |
| Small | 100,000 | 5 | 2 | 15 | 150 | 250 |
| Penny | 0 | 10 | 5 | 50 | 300 | 500 |

---

## 5. Inputs: the volume and the returns

The cost manager is told two things per symbol before it prices anything: a volume (the impact
term and its tier) and a walk of log returns (the spread's `vol_mult`).

### Futures: the signal bar's own volume, with the weekend merge

The design comment is `include/trade_ngin/live/futures_cost_feed.hpp:16` to `:82`. A futures fill
is not priced on a rolling average. The impact model's window holds exactly one number, `V`, built
from the symbol's latest bar `B` (the signal bar, whose close prices the fill) by
`FuturesSessionVolume::participation` (`futures_cost_feed.hpp:155`). A weekend bar is one dated
Saturday or Sunday (UTC); a session is a weekday-dated bar; the weekend block is the run of the
symbol's weekend bars with no session between them.

| Case | `B` is | Fill day | `V` |
|---|---|---|---|
| 1 | a session with a weekend block right before it | any | `B`'s volume plus the block's (a Tuesday fill: Monday plus the Sunday stub, plus Saturday where one printed) |
| 2 | a weekend bar | a weekday | the last session before the block (its own volume) plus the whole block, `B` included (a Monday fill: Friday plus the stub) |
| 3 | a session with no weekend bar right before it | any | `B`'s own volume |
| 3 | a weekend bar | a weekend day | `B`'s own volume: the fill is in the thin session itself |

So the engine adds the weekend block's volume to the signal bar's. No calendar is read: a holiday
is a weekday on which the symbol printed no bar, and its weekend block merges into the next
session the symbol prints. Only `V` moves; the walk of returns does not.

The returns are every consecutive log return of the symbol's bars up to `B`, of which the spread
model keeps the last 20.

Backtest and live compute the same two inputs with the same merge rule
(`FuturesSessionVolume::add` and `participation`, `futures_cost_feed.hpp:133`, `:155`):

| | Function | Fed with | Cost managers fed |
|---|---|---|---|
| Live | `feed_futures_cost_model` (`futures_cost_feed.hpp:186`) | the run's strategy feed, once, into fresh managers | the execution manager's and the `PortfolioManager`'s (`apps/strategies/live_portfolio_conservative.cpp:1665` to `:1709`; `live_portfolio.cpp` carries the same lines) |
| Backtest | `feed_futures_cost_model_step` (`futures_cost_feed.hpp:258`) | each cycle's signal feed; the window is set to `V` every cycle (`set_own_day_volume`, `transaction_cost_manager.cpp:283`) | the execution manager's and the `PortfolioManager`'s (`src/backtest/backtest_coordinator.cpp:899` to `:908`) |

After every backtest cycle each manager holds what the live function gives on the same bars and
the same fill day. Nothing of the fill day's own bar is read.

### Equities: a 20-bar average, and the re-tier

An equity is priced on the average of its last 20 daily volumes, and the same 20 bars pick its
tier (section 4). The rules of the live feed (`LiveDailyCycle::feed_cost_model`,
`include/trade_ngin/live/live_daily_cycle.hpp:210`) are in `docs/AVERAGE_PRICE_LIFECYCLE.md`
section 8b and are not repeated here. In both the live runner and the backtest the window ends at
the signal bar.

| | Tier | Volume and returns |
|---|---|---|
| Live | `register_equity_costs_from_bars` on every run, from the 20 bars ending at the signal bar (`apps/strategies/live_equity_mean_reversion.cpp:995`) | `feed_cost_model`, the same bars (`:997`) |
| Backtest, before the first cycle | the 30 calendar days before the start date (`register_equity_cost_warmup`, `include/trade_ngin/backtest/equity_cost_warmup.hpp:83`) | none yet |
| Backtest, every cycle | `EquityCostRetier::retier` on both cost managers, from the 20 bars ending at the signal bar (`include/trade_ngin/backtest/equity_cost_retier.hpp:148`; `backtest_coordinator.cpp:697`) | the signal group, each volume through `split_consistent_volume` (`equity_cost_retier.hpp:128`; `backtest_coordinator.cpp:731` to `:782`) |

The re-tier exists because the backtest's prices and quantities are adjusted to the end of the
window while the loader leaves volume raw. `split_consistent_volume` multiplies a bar's volume by
the split factor of every later ex-date, so the volume, the price and the traded quantity are in
one share unit. The header comment of `equity_cost_retier.hpp` states the unit and the frame.

A symbol with no bars, or a zero average, is registered with the untiered equity default so it can
never resolve to a futures config (`register_equity_costs_from_bars`,
`transaction_cost_manager.cpp:325`).

---

## 6. The two cost managers

Every runner holds two `TransactionCostManager` objects: the execution manager's and the
`PortfolioManager`'s own. Both are fed the same inputs (section 5). What each one prices differs
between the backtest and live.

| Priced item | Backtest | Live |
|---|---|---|
| The cost vector of the whole-contract search | `PortfolioManager`'s (`src/portfolio/portfolio_manager.cpp:2132` to `:2136`) | `PortfolioManager`'s (the same lines) |
| The stored STRATEGY fills | `PortfolioManager`'s (`portfolio_manager.cpp:2834`); the coordinator does not re-price them (`backtest_coordinator.cpp:1179` to `:1187`) | the execution manager's (`src/live/execution_manager.cpp:163`) |
| The account order `C(Q)` of the netting | `PortfolioManager`'s (`portfolio_manager.cpp:2866`) | the execution manager's (`live_portfolio_conservative.cpp:2561`) |
| The two ROLL legs | `PortfolioManager`'s (`backtest_coordinator.cpp:1044` to `:1076`) | the execution manager's (`live_portfolio_conservative.cpp:2395` to `:2419`) |
| The listing-date switch legs | `PortfolioManager`'s (`portfolio_manager.cpp:2514` to `:2522`) | not booked by a live run |
| Equity overnight borrow fee | the execution manager's (`backtest_coordinator.cpp:1523` to `:1527`) | not accrued |

In every row the fills of a day and the account order they are netted against are priced by the
same manager on the same state, which is what makes the netting identity of section 9 hold to the
last stored digit.

---

## 7. Other rows that carry a cost

| Row | `execution_type` | Cost | Netted |
|---|---|---|---|
| A strategy fill, a forecast-sign close | `STRATEGY` | section 1 | yes, when two sleeves trade the symbol |
| A listing-date switch leg (backtest only) | `STRATEGY` | section 1 at the leg's quantity and price | yes, among the sleeves' switch legs of the same symbol, as their own group and before they join the bar's rows (`portfolio_manager.cpp:2542` to `:2557`); never against the bar's other fills |
| A roll's closing leg and opening leg | `ROLL` | section 1 at the leg's quantity and price: two outright fills (`roll_series::make_roll_legs`, `include/trade_ngin/data/roll_series.hpp:150`) | never |
| An equity short's overnight borrow, backtest only | `BORROW` | annual rate x short value / 365, quantity 0 (`calculate_overnight_borrow_fees`, `transaction_cost_manager.cpp:422`; the row, `backtest_coordinator.cpp:1536` to `:1556`) | never |

The borrow rate is 25, 50, 150 or 500 basis points a year by a count of three flags (dollar volume
under 5 million a day, price under 5.00, not easy to borrow), times `clamp(annual vol / 0.25, 1, 3)`,
unless the instrument carries its own rate. The live equity runner accrues no borrow fee and
refuses to store a short position (`live_equity_mean_reversion.cpp:4390` to `:4404`).

---

## 8. What is stored

### Execution rows

`trading.executions` (live, `PostgresDatabase::store_executions`, `src/data/postgres_database.cpp:236`)
and `backtest.executions` (`store_backtest_executions`, `:2274`, and `store_backtest_executions_with_strategy`, `:2390`) carry the same cost
columns, from `ExecutionReport` (`include/trade_ngin/core/types.hpp:447`):

| Column | Unit | Holds |
|---|---|---|
| `price` | price | the reference price `P`, no cost in it |
| `commissions_fees` | dollars | the fee |
| `implicit_price_impact` | price units per contract or share | spread plus impact, after the total cap, kept to 8 decimals |
| `slippage_market_impact` | dollars | implicit x quantity x point value, computed before the 8-decimal rounding |
| `total_transaction_costs` | dollars | the fill's OWN cost: `commissions_fees + slippage_market_impact`, as if this row were the only order |
| `netting_adjustment` | dollars, signed | section 9; 0 on a symbol-day with one sleeve row (migration 013; NULL on rows written before it) |
| `execution_type` | | `STRATEGY`, `ROLL` or `BORROW` |

### Daily and run totals

Every total below is after netting: each fill's own cost minus its adjustment.

| Table | Column | Holds | Written at |
|---|---|---|---|
| `trading.live_results` | `daily_transaction_costs` | the day's charge: STRATEGY and ROLL fills at their cost after netting | `live_portfolio_conservative.cpp:2575` (the sum), `:4201` |
| `trading.live_results` | `total_transaction_costs` | the previous row's total plus the day's | `:3823`, `:4198` |
| `trading.live_results` | `daily_roll_costs` | the ROLL legs' part of the day's charge (migration 017) | `:2439`, `:4202` |
| `trading.live_results` | `total_roll_costs` | the previous row's total plus the day's | `:3832`, `:4203` |
| `backtest.results` | `transaction_costs` | the run: STRATEGY, ROLL and BORROW rows, each after netting (migration 018) | `backtest_coordinator.cpp:474` |
| `backtest.results` | `roll_costs`, `total_roll_fills` | the ROLL rows' own cost, and their count | `backtest_coordinator.cpp:475`, `:476` |
| `backtest.equity_curve` | `equity` | each day's value has that day's charge taken off: `value += P&L - costs` | `backtest_coordinator.cpp:1249`, `:1564` |

The trade statistics, each strategy's running P&L and the email's cost lines read the same figure
(`src/backtest/backtest_metrics_calculator.cpp:428`, `src/strategy/base_strategy.cpp:298`,
`src/core/email_sender.cpp:3515`).

---

## 9. Netting between sleeves

A book with two sleeves stores one execution row per sleeve. The account sends one order per
symbol and day: the signed sum of the sleeve quantities. This section is the whole rule; the design
comment is the head of `include/trade_ngin/transaction_cost/netting.hpp`.

**A fill's own cost** is `total_transaction_costs`, written `C(q_i)`: what the cost model charges
for that sleeve's quantity alone. Netting never changes this column.

**The adjustment.** For one portfolio, one day (one bar in the backtest) and one symbol with two or
more STRATEGY rows at the same price (`net_symbol_day`, `src/transaction_cost/netting.cpp:46`):

```
Q             = the signed sum of the sleeve quantities          (the account's order)
C(Q)          = the same cost model, state and price, at Q;  C(0) = 0, the model is not called
credit_total  = sum of C(q_i) - C(Q)
adjustment_i  = credit_total x C(q_i) / sum of C(q_i)
```

The split is done in whole units of 0.00000001: each row gets the floor of its exact share, then
one unit goes to the largest remainders, a tie to the smaller sleeve name, so the adjustments sum
to `credit_total` exactly (`netting.cpp:85` to `:111`). The weight is the row's cost, not its
quantity, so no row can end with a negative cost.

`netting_adjustment` is signed:

| Case | Sign | Why |
|---|---|---|
| The sleeves trade opposite ways | positive | the account's order is smaller than the sleeves' orders |
| A full cross, `Q = 0` | positive, equal to each row's own cost | the account sends no order; both rows are credited in full, and there is no broker fill |
| The sleeves trade the same way | negative | one order of the summed size costs more than the two priced alone, because impact grows faster than the quantity |
| One sleeve row on the symbol-day | 0 | nothing to net |
| Rows at different fill prices | 0, with a `NETTING_MIXED_PRICES` warning | a symbol-day is netted at one price only |
| `Q` is not 0 and the rows' own costs sum to 0 | 0 | nothing to split (`netting.cpp:76` to `:80`); no row costs 0 while a fee is charged |

A sleeve's forecast-sign closes of a symbol are one account order and its other fills another: a
close is never netted against the fill that follows it (`live_portfolio_conservative.cpp:2550`;
`portfolio_manager.cpp:2861`).

**Every total uses own cost minus the adjustment.** The one place the two columns are combined is

```cpp
transaction_cost::net_cost(fill)        // total_transaction_costs - netting_adjustment   netting.hpp:97
transaction_cost::add_net_costs(...)    // a day's or a bar's charge                       netting.cpp:183
transaction_cost::run_cost_totals(...)  // a backtest run's totals                         netting.cpp:192
```

and every total of section 8 is built from them. A sum of `total_transaction_costs` taken straight
from the table is the sleeves' attribution, not what the account was charged. The net costs of a
symbol-day sum to `C(Q)`.

**Where it runs.** In the backtest the `PortfolioManager` nets each bar's rows as it books them
(`portfolio_manager.cpp:2860` to `:2872`). A live runner nets the rows it is about to store, then
adds up the day (`live_portfolio_conservative.cpp:2550` to `:2578`).

**ROLL and BORROW rows are never netted.** A ROLL leg or a BORROW row must carry an adjustment of
exactly 0. One that does not is refused, never corrected: `unnetted_row_refusal`
(`netting.cpp:166`) gives the text and `unnetted_cost`, `add_net_costs` and `run_cost_totals` throw
`NettingRefused` (`netting.hpp:108`).

| Runner | What the refusal does |
|---|---|
| Backtest, on the day | a `NETTING STOP` error; the run fails on that day (`backtest_coordinator.cpp:1649`, `:367`) |
| Backtest, at the end of the run | a `NETTING STOP` error before the metrics; no result is returned (`backtest_coordinator.cpp:458` to `:467`) |
| Live | the exception leaves the run and the runner exits 1 (`live_portfolio_conservative.cpp:4779`); the day's `live_run_metadata` row and its signals are already stored (`:1554`, `:1940`) and none of the day's executions is (`:2632`) |

Roll totals read the leg's own cost. In the backtest `results.roll_costs` and the trade statistics'
roll cost go through `unnetted_cost` (`netting.cpp:198`; `backtest_metrics_calculator.cpp:452`). A
live run adds each leg's own cost as it builds the leg (`live_portfolio_conservative.cpp:2439`),
leaves ROLL rows out of the netting (`:2556`), and the day's sum refuses a ROLL row that carries an
adjustment (`:2576`).

**One-sleeve books.** CONSERVATIVE and the equity book have one sleeve, so every symbol-day has
one row and every adjustment is 0: for them cost after netting equals own cost, row by row.

---

## 10. Where cost enters decisions

The cost model is called for a decision in one place: the whole-contract search of the futures
book. For every symbol the search weighs, the `PortfolioManager` prices one contract bought at the
signal close (`portfolio_manager.cpp:2132` to `:2136`), divides it by the sizing capital
(`src/optimization/one_pass.cpp:529`) and the search adds `cost multiplier x contracts traded x
that cost` to the tracking error it minimises (`one_pass.cpp:205`). The multiplier is
`optimization.cost_penalty_scalar`, 100 (`config_template/defaults.json:19`). A symbol the cost
model has no usable volume for is not priced on the 100,000 default: it is held at its held
quantity with no fill (`portfolio_manager.cpp:2144` to `:2160`). The search itself is in
`docs/OPTIMIZER_AND_RISK_DESIGN.md`.

The cost model is not called by the forecasts, the sizing or the risk overlay. The per-contract
list of trading rules a contract does not run (`trading_rule_removals`) is a fixed config block,
not something a run computes; it is described in `docs/TREND_FOLLOWING_SYSTEM.md`. A book with
no overlay sleeve (the equity book) does not run that search, and the generic optimiser step's cost
vector is zero for every symbol (`PortfolioManager::calculate_trading_costs`,
`portfolio_manager.cpp:991`).

---

## 11. Worked examples

Examples of the arithmetic. Every number below is a stored cell or is computed from stored bars
with the formulas above, and each example says which configuration its run used.

### 11.1 One futures fill, end to end

A sixteen-year backtest of the conservative book (2010-10-07 to 2026-10-07) without the
listing-date, relabel and rule-removal blocks, so this run holds the micros throughout and no
E-mini. None of the three enters the cost of a 6A fill. Fill dated Tuesday 2026-08-25:
`TREND_FOLLOWING` BUY 1 `6A.v.0` at 0.71515.

| Step | Input | Value |
|---|---|---|
| Metadata row 6A | `"Contract Size"`, `"Tick Size"`, `"Fee Per Contract"` | 100,000; 0.00005; 2.461 |
| Calibration 6A | baseline, min, max ticks; multiplier; impact cap | 1; 1; 5; 0.25; 40 bps |
| Signal bar `B` | Monday 2026-08-24, close 0.71515, volume | 61,507 |
| Weekend block before `B` | Sunday 2026-08-23, volume | 2,304 |
| `V` (case 1) | 61,507 + 2,304 | 63,811 |
| `sigma` of the 20 returns ending at `B` | | 0.0036938 |
| `z` | (0.0036938 - 0.01) / 0.005 | -1.2612 |
| `vol_mult` | 1 + 0.15 x -1.2612 | 0.8108 |
| Spread ticks | clamp(1 x 0.8108, 1, 5) | 1 |
| Spread | 0.25 x 1 x 0.00005 | 0.0000125 |
| `k_bps` | 63,811 is above 50,000 | 40 |
| Participation | 1 / 63,811 | 0.000015671 |
| Impact in bps | 40 x sqrt(0.000015671); the 40 bps cap does not bind | 0.158348 |
| Impact | 0.158348 / 10000 x 0.71515 | 0.0000113242 |
| Implicit | 0.0000125 + 0.0000113242; the 200 bps cap (0.0143) does not bind | 0.0000238242 |
| Slippage | 0.0000238242 x 1 x 100,000 | 2.38242 |
| Fee | 1 x 2.461 | 2.461 |
| Total | 2.461 + 2.38242 | 4.84342 |

The stored row:

| `price` | `commissions_fees` | `implicit_price_impact` | `slippage_market_impact` | `total_transaction_costs` | `netting_adjustment` |
|---:|---:|---:|---:|---:|---:|
| 0.71515 | 2.461 | 0.00002382 | 2.38242477 | 4.84342477 | 0 |

### 11.2 Netting: a full cross

A three-year backtest of the two-sleeve BASE book (2023-10-07 to 2026-10-07) on its template
configuration, which carries none of the three blocks. Monday 2026-09-14, `MES.v.0` at 7615.5.
The signal bar is the Sunday stub of 2026-09-13 (volume 25,392), so case 2 applies:
`V` = Friday's 1,052,226 + 25,392 = 1,077,618, `k_bps` 10. One contract: spread 0.5 x 1 x 0.25 =
0.125; impact 10 x sqrt(1 / 1,077,618) / 10000 x 7615.5 = 0.00733611; slippage 0.13233611 x 5 =
0.66168056; fee 0.614; own cost 1.27568056.

| Sleeve | Side | Qty | `total_transaction_costs` | `netting_adjustment` | Cost after netting |
|---|---|---:|---:|---:|---:|
| `TREND_FOLLOWING` | BUY | 1 | 1.27568056 | 1.27568056 | 0 |
| `TREND_FOLLOWING_FAST` | SELL | 1 | 1.27568056 | 1.27568056 | 0 |
| Account order | | `Q` = 0 | 2.55136112 | 2.55136112 | `C(0)` = 0 |

Two stored rows, both credited in full, no order. That day's four fill rows: own costs 9.26900400,
adjustments 2.55136112, charged 6.71764288.

### 11.3 Netting: a partial cross

The same run. Sunday 2026-03-01, `MYM.v.0` at 48906, the close of Friday 2026-02-27 (volume
167,330, no weekend bar before it: case 3; `k_bps` 40; fee 0.610; point value 0.5; tick 1). One
contract: implicit 0.5 + 40 x sqrt(1 / 167,330) / 10000 x 48906 = 0.97822825, cost 0.610 +
0.97822825 x 0.5 = 1.09911413. Two contracts: implicit 0.5 + 40 x sqrt(2 / 167,330) / 10000 x
48906 = 1.17631688, cost 1.220 + 1.17631688 x 2 x 0.5 = 2.39631688.

| Sleeve | Side | Qty | `total_transaction_costs` | `netting_adjustment` | Cost after netting |
|---|---|---:|---:|---:|---:|
| `TREND_FOLLOWING` | SELL | 2 | 2.39631688 | 1.64281159 | 0.75350529 |
| `TREND_FOLLOWING_FAST` | BUY | 1 | 1.09911413 | 0.75350529 | 0.34560884 |
| Account order | SELL | `Q` = -1 | 3.49543101 | 2.39631688 | `C(-1)` = 1.09911413 |

`credit_total` = 3.49543101 - 1.09911413 = 2.39631688. Shares: 2.39631688 x 2.39631688 / 3.49543101
= 1.64281159 and 2.39631688 x 1.09911413 / 3.49543101 = 0.75350529. That day's four fill rows (M2K
is a full cross on the same day): own costs 5.57305943, adjustments 4.47394530, charged 1.09911413.

### 11.4 Netting: a same-side day

The same run. Sunday 2026-09-13, `MBT.v.0` at 77315, the close of the Saturday bar of 2026-09-12
(volume 794; a weekend bar and a weekend fill day: case 3, `V` = 794, `k_bps` 80; fee 2.011; point
value 0.1; tick 5; max 10 ticks). `sigma` 0.01637989 gives `vol_mult` 1.19139677 (the return of the
contract change on 2026-08-29 enters as 0), so the spread is 0.5 x 1.19139677 x 5 = 2.97849192.
One contract: impact 80 x sqrt(1 / 794) / 10000 x 77315 = 21.95045344, implicit 24.92894535, cost
2.011 + 24.92894535 x 0.1 = 4.50389454. Two contracts: impact 80 x sqrt(2 / 794) / 10000 x 77315 =
31.04262895, implicit 34.02112087, cost 4.022 + 34.02112087 x 2 x 0.1 = 10.82622417.

| Sleeve | Side | Qty | `total_transaction_costs` | `netting_adjustment` | Cost after netting |
|---|---|---:|---:|---:|---:|
| `TREND_FOLLOWING` | SELL | 1 | 4.50389454 | -0.90921755 | 5.41311209 |
| `TREND_FOLLOWING_FAST` | SELL | 1 | 4.50389454 | -0.90921754 | 5.41311208 |
| Account order | SELL | `Q` = -2 | 9.00778908 | -1.81843509 | `C(-2)` = 10.82622417 |

`credit_total` = 9.00778908 - 10.82622417 = -1.81843509: the account is charged more than the two
rows priced alone. The two exact shares are equal (0.909217545), so the odd last unit goes to the
smaller sleeve name.

### 11.5 The run's totals

A run's stored totals follow from its fill rows by one identity. `backtest.results.transaction_costs` is
the sum of `total_transaction_costs` over every stored fill row less the sum of `netting_adjustment`, and
`backtest.results.roll_costs` is the sum of the ROLL rows' own cost, which carry no adjustment (section 9).
On a book with one sleeve every `netting_adjustment` is 0: no symbol-day has a second sleeve's row to net
against (section 9).

---

## 12. Limits

- A roll is costed as two outright fills, the closing leg and the opening leg, each at the full
  model: see `docs/FUTURES_ROLLS.md`.
- The fee is the broker's published per-contract figure; it is not reconciled against a statement
  by the engine.
- The spread and impact calibrations are compiled constants. Changing one is a code change.
- `vol_mult` measures every instrument against the same fixed 1 percent daily baseline.
- A future's `V` is the continuous series' volume for the signal bar (plus the weekend block), not
  a per-contract volume.
- Rows of one symbol-day at different fill prices are not netted (adjustment 0 and a warning).
- Stored fills are priced by the `PortfolioManager`'s cost manager in the backtest and by the
  execution manager's in live; both are fed the same inputs.
- `implicit_price_impact` is kept to 8 decimals, so for a contract quoted in small price units the
  dollar columns, not this one, are the exact record.
- An equity short accrues a borrow fee in the backtest only; the live equity runner refuses to
  store a short.
- The equity backtest tiers in the share unit of the window's end and the live runner in the unit
  of its signal bar; the two coincide on every bar with no later split in the window.
- Rows written before migration 013 read NULL in `netting_adjustment`.
