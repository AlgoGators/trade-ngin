# Equities Implementation Deliverable

## Executive Summary

The current equities implementation contains **multiple critical bugs** that must be addressed before production deployment. This document outlines the required steps to properly integrate equities support into the trading system.

> [!IMPORTANT]
> **Current Implementation Version Notes:**
> - The current equities implementation is based on the **single-strategy version** and uses the **`algo_data` schema**. Test using `algo_data` first.
> - When porting to the **multiple-strategies architecture**, the schema must be changed to **`new_algo_data`**.
> - The initial implementation was based on **fully adjusted OHLCV data** (adjusted close). However, the **corporate actions table now has foundational data** for more accurate data and analysis.

> [!NOTE]
> **Corporate Actions Data Availability:**
> - **Dividends**: ✅ Available
> - **Splits**: ✅ Available
> - **Relations**: ✅ Available (instrument-issuer mapping)
> - **Mergers**: ❌ Not yet available
>
> **What "relation" action type means:**
> Each relation row maps securities to their issuer, enabling:
> - Roll up instruments to an issuer-level view
> - Map all securities a company has (common, preferred, warrants, units, rights, etc.)
> - Handle cases where a company has multiple share classes or SPAC structures
> - Avoid treating "same company, different instrument" as unrelated entities
>
> **Common relation patterns:**
> | Pattern | Example | Meaning |
> |---------|---------|--------|
> | Multiple share classes | Z / ZG (Zillow) | Class A vs Class C shares |
> | Preferred stock | WFC / WFC-PA / WFC-PD | Common vs preferred series |
> | SPAC structures | YORK / YORKW / YORKU | Common / Warrant / Unit |
> | Warrants | XOS / XOSWW | Common vs warrants (WW suffix) |
> | Share class suffix | PBR / PBR.A (Petrobras) | .A / .B class differences |

---

## Phase 0: Branch Integration (PREREQUISITE)

### ⚠️ IMPORTANT: Code Base Mismatch

The current equities implementation is based on an **older version of the backtest engine** that predates the multiple strategies/portfolio architecture. Some issues identified (particularly the PnL lag model timing bug) may already be fixed in the main codebase. 

### Required Steps:

1. **Create New Integration Branch**
   ```bash
   git checkout multiple_strategies
   git checkout -b equities_integration
   ```

2. **Cherry-Pick Equity Commits from `equity_dom` Branch**
   - Commit `427a119` - "equity backtest working"
   - Commit `b91bf2f` - "Cron equity trading set up"
   
   ```bash
   git cherry-pick 427a119
   git cherry-pick b91bf2f
   ```

3. **Resolve Merge Conflicts**
   - Pay special attention to:
     - `src/backtest/backtest_engine.cpp` (PnL lag model may differ)
     - `src/data/postgres_database.cpp` (database queries)
     - Strategy configuration structures

4. **Verify Multiple Strategy Compatibility**
   - Ensure `MeanReversionStrategy` works within `PortfolioManager`
   - Test with multiple strategies running simultaneously
   - Verify risk management integration

5. **Update Build System**
   - Add new equity targets to CMakeLists.txt
   - Verify all dependencies compile

---

## Phase 1: Critical Bugs - MUST FIX Before Any Use

### 🔴 CRITICAL #1: OHLC Data Not Adjusted for Corporate Actions

**Location**: `src/data/postgres_database.cpp:740`

**Problem**: Only `closeadj` is mapped; `open`, `high`, `low` remain unadjusted.

**Current Code**:
```cpp
"SELECT date as time, ticker as symbol, open, high, low, closeadj as close, volume "
```

**Impact**:
- After stock splits: impossible OHLC bars (close < low)
- Slippage model uses high-low range → broken calculations
- Any volatility/ATR using OHLC range → wrong

> [!CAUTION]
> **Strategy Safeguard Required**: If any strategy uses raw OHLC values (not just close) for:
> - **Slippage models** (using high-low range)
> - **Volatility calculations** (ATR, daily range)
> - **Intraday price validation**
> 
> A runtime safeguard/warning MUST be added to detect and alert when adjusted close differs significantly from raw close (indicating a split/dividend), as OHLC-based calculations will produce **massive discrepancies**.

**Fix Options**:

**Option A** (Preferred - if database has adjusted columns):
```cpp
"SELECT date as time, ticker as symbol, openadj as open, highadj as high, lowadj as low, closeadj as close, volume "
```

**Option B** (Compute adjustment ratio):
```cpp
// Add to query: compute adjustment ratio from close/closeadj
// Then apply to open, high, low in code
double adj_factor = closeadj / close_raw;
bar.open = open_raw * adj_factor;
bar.high = high_raw * adj_factor;
bar.low = low_raw * adj_factor;
```

**Option C** (Document limitation + Runtime Safeguard):
- If only closeadj is available:
  - Slippage model should only use close prices
  - Volatility calculations should use close-to-close only
  - Any high-low based metrics are unreliable for split stocks
  - **Add runtime warning** when `abs(close_raw - closeadj) / close_raw > 0.01` to flag potential OHLC inconsistency

**Testing Required**:
- Test with AAPL (4:1 split Aug 2020)
- Test with TSLA (5:1 split Aug 2020, 3:1 split Aug 2022)
- Verify OHLC consistency: low ≤ open,close ≤ high

---

### 🔴 CRITICAL #2: Unrealized PnL Forced to Zero (Live Only)

**Location**: `apps/strategies/live_equity_mean_reversion.cpp`
- Lines: 608, 620, 805-808, 1226-1227, 1235-1236, 1287-1290

**Problem**: Copy-pasted futures logic treats all equity PnL as "realized".

**Current Code**:
```cpp
// Line 805-808 - WRONG FOR EQUITIES:
// For futures, daily PnL is all realized (mark-to-market)
validated_position.unrealized_pnl = Decimal(0.0);  // Always 0 for futures
```

**Impact**:
- Portfolio value incorrect for open positions
- Risk metrics wrong
- Reports show wrong PnL breakdown

**Fix**:
```cpp
// For equities, calculate unrealized PnL for open positions
if (position.quantity.as_double() != 0.0) {
    double current_price = previous_day_close_prices[symbol];  // or market price
    double entry_price = position.average_price.as_double();
    double unrealized = (current_price - entry_price) * position.quantity.as_double();
    validated_position.unrealized_pnl = Decimal(unrealized);
    validated_position.realized_pnl = Decimal(0.0);  // Only non-zero when closed
}
```

**Also Fix**: Replace all "For futures" comments with "For equities" (17 occurrences).

**Testing Required**:
- Open position, verify unrealized PnL shows correctly
- Close position, verify realized PnL calculated
- Verify portfolio value = cash + unrealized

---

### 🔴 CRITICAL #3: Backtest PnL Timing Bug (May Be Fixed in Main Branch)

**Location**: `src/backtest/backtest_engine.cpp:617-754`

**Problem**: Price update happens BEFORE prices are used, causing:
- Execution at TODAY's price instead of YESTERDAY's
- Daily PnL = 0 for all positions

**Current Code Flow**:
```cpp
// Step 1: UPDATE (happens first!)
previous_day_close_prices[symbol] = current_close;  // Overwrites to Day T

// Step 2: USE (too late - already updated!)
execution_price = previous_day_close_prices[symbol];  // Gets Day T, not Day T-1!
daily_pnl = qty * (current - previous_day_close_prices[symbol]);  // = qty * 0!
```

**Fix** (if not already fixed in main branch):
```cpp
// Option A: Use two_days_ago_close_prices
execution_price = two_days_ago_close_prices[symbol];  // Actual Day T-1
daily_pnl = qty * (current - two_days_ago_close_prices[symbol]);

// Option B: Update AFTER using
// Move lines 627-631 to AFTER the PnL calculation
```

**Note**: Check if `multiple_strategies` branch has this fixed. If so, porting commits should resolve this automatically.

**Testing Required**:
- Run backtest, verify equity curve changes (not flat)
- Compare execution prices to expected Day T-1 closes
- Verify daily PnL ≠ 0 for held positions

---

## Phase 2: Major Issues - Fix Before Production

### 🟠 MAJOR #1: Memory Growth - Price History Never Trimmed

**Location**: `src/strategy/mean_reversion.cpp:107`

**Problem**: `price_history` grows unboundedly.

**Fix**:
```cpp
// After adding new price:
inst_data.price_history.push_back(bar.close.as_double());

// Add trimming:
size_t max_size = static_cast<size_t>(
    std::max(mr_config_.lookback_period, mr_config_.vol_lookback) * 2);
while (inst_data.price_history.size() > max_size) {
    inst_data.price_history.erase(inst_data.price_history.begin());
}
```

---

### 🟠 MAJOR #2: Position Limits Not Enforced in Strategy

**Location**: `src/strategy/mean_reversion.cpp:210-229`

**Problem**: `config_.position_limits[symbol]` is never checked.

**Fix**:
```cpp
double num_shares = position_value / price;
// Note: Do NOT floor shares - see MAJOR #5 for fractional share support

// Add limit enforcement:
auto limit_it = config_.position_limits.find(symbol);
if (limit_it != config_.position_limits.end()) {
    num_shares = std::min(num_shares, limit_it->second);
}

return num_shares;
```

---

### 🟠 MAJOR #5: Fractional Shares Not Supported

**Location**: `src/strategy/mean_reversion.cpp:206`

**Problem**: Shares are rounded down to integers using `std::floor()`, but most modern brokers support fractional shares.

**Current Code**:
```cpp
double num_shares = position_value / price;
num_shares = std::floor(num_shares);  // Whole shares only
```

**Impact**:
- Suboptimal capital utilization for high-priced stocks
- Small accounts cannot properly diversify
- Position sizing less precise than broker allows

**Fix**:
```cpp
double num_shares = position_value / price;
// Allow fractional shares - most brokers (Alpaca, Robinhood, Schwab) support this
// Round to reasonable precision (e.g., 6 decimal places) to avoid floating point issues
num_shares = std::round(num_shares * 1000000.0) / 1000000.0;

// Optional: Add config flag for brokers that don't support fractional
if (!config_.allow_fractional_shares) {
    num_shares = std::floor(num_shares);
}
```

**Configuration Addition**:
```json
{
  "allow_fractional_shares": true  // default: true for equity strategies
}
```

---

### 🟠 MAJOR #3: No Data Quality Validation

**Location**: `src/data/postgres_database.cpp`

**Problem**: No sanity checks for suspicious price changes that might indicate bad data or missing corporate action adjustments.

**Fix**: Add validation in data loading:
```cpp
// After loading bars, validate:
for (size_t i = 1; i < bars.size(); ++i) {
    if (bars[i].symbol == bars[i-1].symbol) {
        double change = std::abs(bars[i].close - bars[i-1].close) / bars[i-1].close;
        if (change > 0.25) {  // >25% change
            WARN("Suspicious price change for " + bars[i].symbol + 
                 ": " + std::to_string(change * 100) + "% - check for corporate action");
        }
    }
}
```

---

### 🟠 MAJOR #4: DividendInfo Infrastructure Unused

**Location**: `include/trade_ngin/instruments/equity.hpp`, `src/instruments/equity.cpp`

**Problem**: `DividendInfo` struct and `get_next_dividend()` exist but are never used.

**Decision Required**:
- **Option A**: Remove unused code (if relying on adjusted close)
- **Option B**: Implement dividend awareness (for advanced strategies)

**If keeping**: Document that strategy assumes adjusted close handles dividends.

---

## Phase 3: Moderate Issues - Fix When Possible

### 🟡 MOD #1: Wrong Comments Throughout Live Code

**Location**: `apps/strategies/live_equity_mean_reversion.cpp`

**Problem**: 17 occurrences of "futures" in comments.

**Fix**: Global search and replace:
- "For futures" → "For equities"
- "futures instruments" → "equity instruments"
- "Always 0 for futures" → "Calculated for open equity positions"

---

### 🟡 MOD #2: Test Doesn't Catch OHLC Bug

**Location**: `tests/strategy/test_mean_reversion_backtest.cpp:312-318`

**Problem**: Test adjusts ALL OHLC values, but real data only has closeadj.

**Fix**: Update test to simulate real data:
```cpp
// Simulate real database: only closeadj is adjusted
bar.close = base_price * (1.0 + random_var);  // Adjusted
bar.open = raw_base_price * 0.99;   // NOT adjusted
bar.high = raw_base_price * 1.02;   // NOT adjusted  
bar.low = raw_base_price * 0.98;    // NOT adjusted
// where raw_base_price = pre-split price
```

---

### 🟡 MOD #3: Entry Price Not Updated for Position Scaling (Cost-Basis Averaging)

**Location**: `src/strategy/mean_reversion.cpp:138-140`, `apps/strategies/live_equity_mean_reversion.cpp`

**Problem**: Entry price is set on initial position open but:
1. **Not cleared on close** - stale entry price persists
2. **Not updated when adding to positions** - scaling in doesn't recalculate average cost
3. **Not updated when reducing positions** - can lead to double-counting of PnL

**Impact**:
- **Double-counting PnL**: If entry price stays at original value when adding shares, realized PnL on partial closes is calculated from wrong base
- **Incorrect unrealized PnL**: Portfolio value reports wrong mark-to-market
- **Wrong trade attribution**: Performance metrics skewed

**Fix - Part A** (Reset on Close):
```cpp
// When position closes (target becomes 0):
if (inst_data.target_position == 0.0) {
    inst_data.entry_price = 0.0;  // Reset for next trade
}
```

**Fix - Part B** (Average Cost on Add):
```cpp
// When adding to existing position:
if (current_position != 0.0 && new_shares != 0.0) {
    double total_cost = (current_position * inst_data.entry_price) + (new_shares * execution_price);
    double new_total_position = current_position + new_shares;
    inst_data.entry_price = total_cost / new_total_position;  // Weighted average
}
```

**Fix - Part C** (Partial Close Handling):
```cpp
// When reducing position (partial close):
if (std::abs(new_position) < std::abs(current_position) && new_position != 0.0) {
    // Entry price stays the same - FIFO or average cost basis
    // Realized PnL = (execution_price - entry_price) * shares_closed
    double shares_closed = std::abs(current_position) - std::abs(new_position);
    double realized = (execution_price - inst_data.entry_price) * shares_closed;
    // Log/store realized PnL
}
```

---

## Phase 4: Database Schema Requirements

### Schema Verification Checklist

Before running, verify PostgreSQL has:

1. **Schema**: `equities_data` exists
2. **Table**: `equities_data.ohlcv_1d` exists
3. **Columns**:
   - `date` (timestamp/date)
   - `ticker` (varchar)
   - `open`, `high`, `low` (numeric)
   - `closeadj` (numeric) - **CRITICAL: Must be adjusted**
   - `volume` (numeric/bigint)

4. **Ideally Also Have** (for OHLC fix):
   - `openadj`, `highadj`, `lowadj` (numeric)

### SQL to Verify:
```sql
SELECT column_name, data_type 
FROM information_schema.columns 
WHERE table_schema = 'equities_data' 
AND table_name = 'ohlcv_1d';
```

---

## ⚠️ WARNING: Additional Issues May Exist

The issues documented above were found through code review. **Additional issues may exist** that were not detected, including:

1. **Integration issues** when porting to multiple strategies branch
2. **Edge cases** in position sizing for high-priced stocks
3. **Timezone issues** with equity market hours vs. database timestamps
4. **Holiday handling** differences between equity and futures markets
5. **Pre-market/after-hours** data handling
6. **Margin calculation** differences (equities use Reg-T, not futures margin)

### Recommended Testing Before Production:

1. **Unit Tests**: All existing tests pass after porting
2. **Integration Tests**: Multi-strategy portfolio with equities
3. **Backtest Validation**: Compare against known benchmark
4. **Paper Trading**: Run live system without real orders for 1+ week
5. **Data Validation**: Spot-check OHLC consistency for split stocks

---

## Implementation Checklist

### Before Starting:
- [ ] Create `equities_integration` branch from `multiple_strategies`
- [ ] Cherry-pick equity commits
- [ ] Resolve merge conflicts
- [ ] All tests pass

### Phase 1 (Critical):
- [ ] Fix OHLC adjustment (Critical #1)
- [ ] Fix unrealized PnL (Critical #2)  
- [ ] Verify/fix PnL timing (Critical #3)
- [ ] Run backtest, verify equity curve not flat

### Phase 2 (Major):
- [ ] Add price history trimming
- [ ] Add position limit enforcement
- [ ] Add data quality validation
- [ ] Decide on dividend handling

### Phase 3 (Moderate):
- [ ] Fix comments (futures → equities)
- [ ] Update tests for real OHLC data
- [ ] Reset entry price on close

### Before Production:
- [ ] Verify database schema
- [ ] Run paper trading for 1+ week
- [ ] Review PnL reports for accuracy
- [ ] Test with stocks that have had splits

---

## Phase 5: Future Enhancements - Post Core Implementation

> [!NOTE]
> These items should be implemented AFTER Phase 1-4 critical bugs are fixed and core functionality is verified.

---

### 🔮 FUTURE #1: Short Selling Mechanics & Borrow Costs

**Current State**: Mean reversion strategy can generate short signals, but infrastructure lacks short-specific handling.

**Required Components**:

#### A. Short Locate Requirements
```cpp
// Before executing a short sale, verify stock is available to borrow
struct ShortLocateResult {
    bool available;
    double borrow_rate_annual;     // Annual rate (e.g., 0.25% for easy-to-borrow)
    int shares_available;           // Shares available at broker
    std::string locate_id;          // Broker's locate confirmation ID
    Timestamp expiry;               // When locate expires
};

Result<ShortLocateResult> check_short_availability(const std::string& symbol, int shares);
```

#### B. Borrow Fee Tracking
| Fee Type | Calculation | When Applied |
|----------|-------------|--------------|
| **General Collateral (GC)** | ~0.25% - 1% annually | Easy-to-borrow stocks |
| **Hard-to-Borrow (HTB)** | 1% - 100%+ annually | Limited supply stocks |
| **Daily Calculation** | `shares × price × (annual_rate / 360)` | Accrued daily |

**Implementation**:
```cpp
// Add to position tracking
struct ShortPositionCosts {
    double daily_borrow_cost;
    double cumulative_borrow_cost;
    std::string locate_id;
    Timestamp locate_expiry;
};

// Add to daily PnL calculation
daily_pnl -= calculate_borrow_cost(symbol, shares, current_price, borrow_rate);
```

#### C. Short Squeeze Risk Management
- Add position limit reduction for stocks with high short interest (>20%)
- Add alert when days-to-cover exceeds threshold (>5 days)
- Consider reducing position size for HTB stocks

**Database Schema Addition**:
```sql
CREATE TABLE trading.short_borrow_rates (
    date DATE NOT NULL,
    symbol VARCHAR(10) NOT NULL,
    borrow_rate_annual NUMERIC(10,6),
    shares_available BIGINT,
    is_hard_to_borrow BOOLEAN DEFAULT FALSE,
    PRIMARY KEY (date, symbol)
);
```

---

### 🔮 FUTURE #2: Equity-Specific Commission Model

**Current State**: Uses generic commission rate (0.001 = 10 bps), but equity costs are more complex.

#### A. Complete Equity Transaction Costs

| Cost Component | Rate | Applied To | Notes |
|----------------|------|-----------|-------|
| **Broker Commission** | $0 - $0.005/share | All trades | Most brokers now $0 |
| **SEC Fee** | $22.90 per $1M | Sells only | Updated periodically |
| **FINRA TAF** | $0.000119/share | Sells only | Max $5.95 per trade |
| **Exchange Fees** | $0.0003/share | All trades | Varies by exchange |

**Implementation**:
```cpp
struct EquityTransactionCosts {
    double broker_commission_per_share = 0.0;    // Most brokers now $0
    double sec_fee_per_million = 22.90;          // SEC Section 31 fee
    double finra_taf_per_share = 0.000119;       // FINRA Trading Activity Fee
    double finra_taf_max_per_trade = 5.95;
    double exchange_fee_per_share = 0.0003;
};

double calculate_equity_costs(Side side, double shares, double price) {
    double costs = shares * config_.broker_commission_per_share;
    costs += shares * config_.exchange_fee_per_share;
    
    if (side == Side::SELL) {
        // SEC fee only on sells
        double notional = shares * price;
        costs += (notional / 1000000.0) * config_.sec_fee_per_million;
        
        // FINRA TAF only on sells
        double taf = shares * config_.finra_taf_per_share;
        costs += std::min(taf, config_.finra_taf_max_per_trade);
    }
    return costs;
}
```

#### B. Configuration
```json
{
  "equity_costs": {
    "broker_commission_per_share": 0.0,
    "sec_fee_per_million": 22.90,
    "finra_taf_per_share": 0.000119,
    "finra_taf_max_per_trade": 5.95,
    "exchange_fee_per_share": 0.0003
  }
}
```

---

### 🔮 FUTURE #3: Holiday Calendar - NYSE/NASDAQ vs CME

**Current State**: `HolidayChecker` exists but doesn't differentiate between equity and futures markets.

#### A. Holiday Calendar Comparison

| Holiday | NYSE/NASDAQ (Equities) | CME (Futures) | Impact |
|---------|------------------------|---------------|--------|
| **New Year's Day** | Closed | Closed | Both |
| **MLK Day** | Closed | Open (limited hours) | Different |
| **Presidents' Day** | Closed | Open (limited hours) | Different |
| **Good Friday** | Closed | Open | **Equities only** |
| **Memorial Day** | Closed | Closed | Both |
| **Juneteenth** | Closed | Closed | Both |
| **Independence Day** | Closed | Closed | Both |
| **Labor Day** | Closed | Closed | Both |
| **Thanksgiving** | Closed | Closed | Both |
| **Christmas** | Closed | Closed | Both |

#### B. Early Close Days (1:00 PM ET)

| Day | Equities Close | Futures |
|-----|----------------|---------|
| Day before Independence Day | 1:00 PM ET | Normal |
| Day after Thanksgiving | 1:00 PM ET | Normal |
| Christmas Eve | 1:00 PM ET | Normal |

**Implementation**:
```cpp
enum class MarketType { EQUITIES, FUTURES };

class MarketCalendar {
public:
    bool is_market_open(const Timestamp& ts, MarketType market) const;
    Timestamp get_market_close(const Timestamp& date, MarketType market) const;
    bool is_early_close(const Timestamp& date, MarketType market) const;
    std::vector<Timestamp> get_holidays(int year, MarketType market) const;
};

// Usage in strategy
if (!calendar.is_market_open(current_time, MarketType::EQUITIES)) {
    WARN("Equity markets closed - skipping signal generation");
    return;
}
```

**Database Schema**:
```sql
CREATE TABLE trading.market_holidays (
    date DATE NOT NULL,
    market_type VARCHAR(10) NOT NULL,  -- 'EQUITIES', 'FUTURES'
    is_closed BOOLEAN DEFAULT TRUE,
    close_time TIME,                    -- NULL = fully closed, otherwise early close
    holiday_name VARCHAR(100),
    PRIMARY KEY (date, market_type)
);
```

---

### 🔮 FUTURE #4: Market Hours Validation

**Current State**: No validation that data timestamps align with market hours.

#### A. Market Hours Definition

| Market | Regular Hours (ET) | Pre-Market (ET) | After-Hours (ET) |
|--------|-------------------|-----------------|------------------|
| **NYSE/NASDAQ** | 9:30 AM - 4:00 PM | 4:00 AM - 9:30 AM | 4:00 PM - 8:00 PM |
| **CME Futures** | 6:00 PM (Sun) - 5:00 PM (Fri) | N/A | N/A |

#### B. Data Validation Requirements

```cpp
struct MarketHoursConfig {
    int regular_open_hour = 9;
    int regular_open_minute = 30;
    int regular_close_hour = 16;
    int regular_close_minute = 0;
    bool allow_premarket_data = false;
    bool allow_afterhours_data = false;
};

Result<void> validate_data_timestamp(const Timestamp& ts, AssetClass asset_class) {
    if (asset_class == AssetClass::EQUITIES) {
        auto hour = get_hour_et(ts);
        auto minute = get_minute_et(ts);
        
        bool in_regular_hours = (hour > 9 || (hour == 9 && minute >= 30)) 
                              && hour < 16;
        
        if (!in_regular_hours && !config_.allow_afterhours_data) {
            return make_error("Data timestamp outside market hours: " + to_string(ts));
        }
    }
    return Result<void>();
}
```

#### C. Pre-Market/After-Hours Data Handling

**Option A**: Reject non-regular-hours data
```cpp
// Filter out pre/post market bars
bars = filter_regular_hours_only(bars, AssetClass::EQUITIES);
```

**Option B**: Flag and use cautiously
```cpp
struct Bar {
    // ... existing fields
    bool is_regular_hours = true;  // Add flag
};

// In strategy, weight regular hours data higher
if (!bar.is_regular_hours) {
    // Use with caution - lower liquidity, wider spreads
}
```

#### D. Timestamp Timezone Handling

```cpp
// Ensure database timestamps are correctly interpreted
// Option 1: Store as UTC, convert on read
Timestamp to_eastern_time(const Timestamp& utc_ts);

// Option 2: Store as Eastern Time (market time)
// Ensure database column is TIMESTAMPTZ with correct timezone
```

**Configuration**:
```json
{
  "market_hours": {
    "timezone": "America/New_York",
    "regular_hours_only": true,
    "validate_timestamps": true,
    "warn_on_gaps": true
  }
}
```

---

## Phase 5 Implementation Checklist

### Short Selling:
- [ ] Add `short_borrow_rates` table
- [ ] Implement `check_short_availability()` function
- [ ] Add borrow cost to daily PnL calculation
- [ ] Add HTB stock alerts
- [ ] Integrate with broker API for locate

### Commission Model:
- [ ] Update `EquityTransactionCosts` struct
- [ ] Add SEC fee calculation (sells only)
- [ ] Add FINRA TAF calculation (sells only)
- [ ] Update backtest cost model
- [ ] Add configuration for costs

### Holiday Calendar:
- [ ] Add `market_holidays` table
- [ ] Implement `MarketCalendar` class
- [ ] Add early close handling
- [ ] Add holiday validation to live trading
- [ ] Populate 2026+ holidays

### Market Hours:
- [ ] Add timestamp validation
- [ ] Handle pre-market/after-hours data
- [ ] Add timezone configuration
- [ ] Add data gap detection
- [ ] Update live trading validation

---

## Appendix: Files to Modify

### Phase 1-4 Files:

| File | Changes Needed |
|------|---------------|
| `src/data/postgres_database.cpp` | OHLC adjustment, data validation |
| `apps/strategies/live_equity_mean_reversion.cpp` | Unrealized PnL, comments |
| `src/strategy/mean_reversion.cpp` | Memory trim, position limits, entry price reset |
| `src/backtest/backtest_engine.cpp` | PnL timing (if not fixed in main) |
| `tests/strategy/test_mean_reversion_backtest.cpp` | Update OHLC test |

### Phase 5 Files (Future):

| File | Changes Needed |
|------|---------------|
| `src/live/short_locate_manager.cpp` | **[NEW]** Short locate infrastructure |
| `src/live/borrow_cost_calculator.cpp` | **[NEW]** Borrow fee tracking |
| `src/backtest/equity_cost_model.cpp` | **[NEW]** SEC fee, FINRA TAF calculations |
| `include/trade_ngin/core/market_calendar.hpp` | **[NEW]** Holiday/market hours calendar |
| `src/core/market_calendar.cpp` | **[NEW]** Market calendar implementation |
| `include/trade_ngin/core/holiday_checker.hpp` | Update for market-specific holidays |

---

*Document Version: 1.1*  
*Last Updated: 2026-01-18*  
*Review Status: Code review complete, Phase 5 future enhancements documented*
