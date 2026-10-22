# Transaction cost module

What a fill costs: an explicit fee plus an implicit cost (spread plus market impact), computed
separately from the fill price and charged against the day's P&L. The reference for the model,
every constant in force, the inputs, what is stored and the netting between sleeves is
[`docs/COST_MODEL.md`](../../docs/COST_MODEL.md). This file is the map of the folder.

## Files

| File | Holds |
|---|---|
| `transaction_cost_manager.cpp` | `TransactionCostManager`: `calculate_costs` (the one place a cost is computed), the volume and return feeds (`record_volume`, `set_own_day_volume`, `record_log_return`, `update_market_data`), the equity tier registration (`register_equity_costs_from_bars`), the equity overnight borrow fee (`calculate_overnight_borrow_fees`) |
| `spread_model.cpp` | `SpreadModel`: the spread in ticks, widened or narrowed by a volatility multiplier, clamped, times the per-asset spread cost multiplier |
| `impact_model.cpp` | `ImpactModel`: square-root impact on the share of volume, the coefficient chosen by volume tier, capped |
| `asset_cost_config.cpp` | `AssetCostConfigRegistry`: the per-contract futures calibration (spread ticks, bounds, multiplier, impact cap), the equity fee schedule and the equity liquidity tiers |
| `netting.cpp` | the netting adjustment of a symbol traded by two or more sleeves on one day, and the helpers every cost total reads: `add_net_costs`, `unnetted_cost`, `run_cost_totals` (`net_cost` is inline in `netting.hpp`) |

Headers are in `include/trade_ngin/transaction_cost/`. The futures feed of the two inputs is
`include/trade_ngin/live/futures_cost_feed.hpp`; the equity backtest's re-tier is
`include/trade_ngin/backtest/equity_cost_retier.hpp`.

## The formula

```
fee       = |q| x fee_per_contract                                   (futures)
          = min(0.01 x |q| x P, max(1.00, |q| x 0.005))              (equities)
spread    = spread_cost_multiplier x clamp(baseline_ticks x vol_mult, min_ticks, max_ticks) x tick_size
impact    = min(k_bps(V) x sqrt(clamp(|q| / max(V, 100), 0, 0.1)), max_impact_bps) / 10000 x P
implicit  = min(spread + impact, max_total_implicit_bps / 10000 x P)
total     = fee + implicit x |q| x point_value
```

## Facts to keep straight

- A future's `point_value`, `tick_size` and fee come from its row of `metadata.contract_metadata`
  (`"Contract Size"`, `"Tick Size"`, `"Fee Per Contract"`), not from `AssetCostConfig`. The
  per-contract entries in `asset_cost_config.cpp` hold calibration only. A future the
  calibration table names, or whose row has a positive `"Contract Size"`, and that lacks a usable
  `"Contract Size"` or `"Tick Size"` is an ERROR: its fee is charged, its spread and impact are not
  (`docs/COST_MODEL.md` section 2 has the cases).
- The fee is the row's `"Fee Per Contract"`. `TransactionCostManager::Config::explicit_fee_per_contract`
  defaults to 1.50 and is charged only when the metadata gives no fee for the contract.
- `spread_cost_multiplier` defaults to 0.5 and is 0.25 on the treasury and currency contracts.
  Defaults in `AssetCostConfig`: `max_spread_ticks` 10, `max_impact_bps` 100,
  `max_total_implicit_bps` 200; each contract's entry sets its own.
- CL's baseline spread is 1 tick.
- `vol_mult = clip(1 + 0.15 x clip((sigma - 0.01) / 0.005, -2, 2), 0.8, 1.5)`, where `sigma` is the
  sample standard deviation of the last 20 log returns. The 0.01 and 0.005 are fixed numbers, not
  rolling estimates.
- The impact model floors the volume at `min_adv` = 100 and caps participation at 0.1. `k_bps` is
  10, 20, 40, 60 or 80 for a volume above 1,000,000, 200,000, 50,000, 20,000, or below.
- A futures fill is priced on ONE volume, the signal bar's with the weekend block added
  (`futures_cost_feed.hpp`), the same in the backtest and live. Only equities use a 20-bar average.
- Equities: 0.005 per share, 1.00 minimum per order, a maximum of 1 percent of trade value applied
  after the minimum; the sell-side regulatory fees are off on every config.
- `calculate_costs` takes the SIGNED quantity. Every term uses its absolute value except the
  sell-side regulatory fees.
- Every cost total is a fill's own cost minus its signed `netting_adjustment`, through
  `net_cost` / `add_net_costs`. The fill's own `total_transaction_costs` is never changed. A ROLL or
  BORROW row that carries an adjustment is refused (`NettingRefused`).

## Tests

`tests/transaction_cost/`.
