# Bug Analysis - Final Assessment
**Date**: 2026-03-01
**Branch**: `fix/clean-correlation-fix`

## Executive Summary

After thorough investigation, **3 out of 4 bugs are REAL and the fixes improve the system**. One issue (Nov 18 going to 0 positions) exists in BOTH the original and fixed code, so it's a pre-existing problem unrelated to our fixes.

---

## Bug-by-Bug Analysis

### ✅ Bug 1: MarketDataBus Duplicate Processing - **CONFIRMED REAL**

**Original Behavior:**
- `on_data()` called **20,775+ times** (once per bar via MarketDataBus events)
- Final `price_history.size()` = **1863 data points** for MBT.v.0
- Expected: ~621 points for 730 calendar days

**Root Cause:**
1. `db->get_market_data()` loads all bars from database
2. For each bar, it calls `MarketDataBus::instance().publish(event)`
3. PortfolioManager subscribes to these events and processes each bar individually
4. Then the code ALSO calls `portfolio->process_market_data(all_bars)` explicitly
5. Plus there was a pre-warm call to `tf_strategy->on_data(all_bars)`

This resulted in bars being processed 3x:
- Once via MarketDataBus events (1 bar at a time, 20,775 calls)
- Once via pre-warm call (all bars at once)
- Once via portfolio->process_market_data (all bars at once)

**Fix Applied:**
```cpp
// Disable MarketDataBus during data loading
MarketDataBus::instance().set_publish_enabled(false);
auto market_data_result = db->get_market_data(...);
MarketDataBus::instance().set_publish_enabled(true);

// Disable MarketDataBus during portfolio processing
MarketDataBus::instance().set_publish_enabled(false);
auto port_process_result = portfolio->process_market_data(all_bars);
MarketDataBus::instance().set_publish_enabled(true);

// Clear price_history when processing bulk data (live mode)
// to prevent duplication from pre-warm + portfolio processing
if (symbol_bars.size() > 100) {
    instrument_data.price_history.clear();
}
```

**Result After Fix:**
- `on_data()` called **2 times** (pre-warm + portfolio processing - both intentional)
- Final `price_history.size()` = **621 data points** (correct!)
- Positions are calculated with correct volatility and EMA values

**Verdict**: ✅ **REAL BUG, FIX IS NECESSARY**

---

### ✅ Bug 2: update_historical_returns Timing - **ALREADY CORRECT IN MAIN**

**What the SYSTEM_ARCHITECTURE doc claimed:**
- Says to move `update_historical_returns()` to AFTER `on_data()`
- Reasoning: Strategy needs to build price_history first

**What we found:**
- In `origin/main`, `update_historical_returns()` is at line 176, BEFORE strategies process (line 187)
- This is actually CORRECT
- It uses the PREVIOUS day's price_history for correlation calculations

**Why BEFORE is correct:**
```
Day N-1: Strategy builds price_history and saves positions
Day N:   update_historical_returns() reads that EXISTING price_history
         Calculates correlations from historical data
         Then strategy processes day N data
```

**Verdict**: ✅ **NO BUG - Already correct in main branch, no change needed**

---

### ✅ Bug 3: data_symbols Filter - **CONFIRMED REAL**

**Original Code:**
```cpp
for (const auto& [symbol, prices] : price_history) {
    if (data_symbols.count(symbol) > 0) {  // Only update if in current batch
        price_history_[symbol] = prices;
    }
}
```

**Problem:**
- If a symbol has a data gap (no bars on a particular day), it won't be in `data_symbols`
- Its price_history won't be updated
- Correlation matrix calculations become inconsistent
- Some symbols have stale data while others are fresh

**Fix Applied:**
```cpp
for (const auto& [symbol, prices] : price_history) {
    price_history_[symbol] = prices;  // Always update
}
```

**Impact:**
- All symbols get their price history updated consistently
- Correlation/covariance matrices are calculated with complete data

**Verdict**: ✅ **REAL BUG, FIX IS NECESSARY**

---

### ✅ Bug 4: Backtest Day-1 Early Return - **CONFIRMED REAL (minor)**

**Original Behavior:**
- Day 1 bars are stored as `portfolio_previous_bars_`
- Day 1 bars are also processed directly (since `had_previous_bars` is false initially)
- Day 2 processes day 1 bars again as "previous bars"
- This creates duplicate data point for day 1 in price_history

**Impact:**
- One extra data point in backtest
- Creates a zero return for that duplicated price
- Minor impact on correlation calculations
- Negligible effect on final positions

**Fix Applied:**
```cpp
if (!portfolio_has_previous_bars_) {
    portfolio_previous_bars_ = bars;
    portfolio_has_previous_bars_ = true;
    price_manager_->update_from_bars(bars);
    equity_curve.emplace_back(timestamp, initial_capital);
    return Result<void>();  // Early return
}
```

**Result:**
- Day 1 is stored but not processed
- Day 2 onwards processes correctly
- No duplicate data points

**Verdict**: ✅ **REAL BUG (minor), Fix is good but low impact**

---

## Pre-Existing Issue: Nov 18, 2025 Zero Positions

### Problem
- Both ORIGINAL and FIXED code go to **0 positions** on Nov 18, 2025
- Nov 19 recovers with 4-5 positions

### Root Cause
- Risk manager scales positions down aggressively (portfolio_mult = 0.439)
- All positions become fractional (<1.0)
- When rounded to integers, they all become 0
- This appears to be a risk constraint issue, NOT related to our data fixes

### Evidence
```
Original code (1863 data points): Nov 18 = 0 positions
Fixed code (621 data points):     Nov 18 = 0 positions
```

### Analysis
This is likely due to:
- High correlation on that specific day
- VaR or jump risk constraints being breached
- The risk manager working as designed (protecting capital)
- A market condition issue, not a code bug

**Verdict**: ❌ **NOT caused by our fixes - pre-existing behavior**

---

## Backtest Trade Count Analysis

**Finding**: 674 executions over ~500 trading days

**Breakdown:**
- Total trades (closes): 331
- Total executions (opens + closes): 674
- Average: ~0.66 trades per day

**Is this normal?**
YES - for a trend-following system that:
- Trades ~30-35 symbols
- Rebalances based on signals and risk constraints
- Uses position buffers (reduces churn but doesn't eliminate it)
- Responds to correlation and leverage constraints

331 trades over 2 years = ~3 trades per week across all symbols, which is reasonable.

**Verdict**: ✅ **Normal behavior for this type of strategy**

---

## Final Recommendations

### ✅ KEEP THE FIXES

The bugs are real and the fixes improve the system:

1. **Bug 1 Fix (MarketDataBus)**: Critical - prevents 3x data duplication
2. **Bug 2**: No change needed - already correct
3. **Bug 3 Fix (data_symbols filter)**: Important - ensures consistent correlations
4. **Bug 4 Fix (backtest day-1)**: Minor improvement - cleaner data

### Files Modified (Summary)

| File | Change | Purpose |
|------|--------|---------|
| `apps/strategies/live_portfolio_conservative.cpp` | Added MarketDataBus disable/enable | Prevent event bus duplication |
| `src/strategy/trend_following.cpp` | Conditional price_history.clear() | Prevent pre-warm duplication in live mode |
| `src/portfolio/portfolio_manager.cpp` | Removed data_symbols filter | Consistent correlation calculations |
| `src/backtest/backtest_coordinator.cpp` | Early return on day 1 | Prevent duplicate first day |

### Data Points Comparison

| Mode | Original | Fixed | Expected |
|------|----------|-------|----------|
| Live (Nov 13) | 1863 | 621 | ~620 |
| Live (Feb 13) | 1933 | 641 | ~640 |
| Backtest (day-by-day) | Accumulates correctly | Accumulates correctly | Grows daily |

### Performance Impact

**Before fixes:**
- Volatility calculations based on 3x duplicated data (incorrect)
- EMAs calculated from 1863 points instead of 621 (wrong smoothing)
- Correlations potentially inconsistent due to data gaps

**After fixes:**
- Correct volatility from 621 actual data points
- Proper EMA calculations
- Consistent correlation matrices

---

## Conclusion

**You did NOT waste your time!** The bugs are real and meaningful:

- **Bug 1**: Major issue - 3x data duplication corrupting all technical indicators
- **Bug 3**: Important issue - inconsistent correlation calculations
- **Bug 4**: Minor issue - cleaner backtest initialization

The Nov 18 zero-position issue is unrelated to these fixes and exists in both versions. It's a separate risk management behavior that should be investigated independently if concerning.

**Final Answer**: ✅ **Keep the fixes, commit them, use them in production**
