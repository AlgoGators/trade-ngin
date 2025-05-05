# Segfault Investigation Notes (bt_trend)

This document tracks potential causes and hypotheses related to the random segfault occurring in the `bt_trend` backtest when the number of contracts reaches approximately 23-25.

**Primary Suspect Areas:**
*   Memory Management (leaks, excessive allocation, fragmentation)
*   Threading Issues (race conditions, deadlocks)
*   RAII Implementation Errors
*   Large Data Handling (especially covariance matrices)

**Relevant Files:**
*   `apps/backtest/bt_trend.cpp`
*   `src/portfolio/portfolio_manager.cpp` & `include/trade_ngin/portfolio/portfolio_manager.hpp`
*   `src/optimization/dynamic_optimizer.cpp` & `include/trade_ngin/optimization/dynamic_optimizer.hpp`
*   Files related to `risk_measures` (to be identified)

## Initial Hypotheses

*   **Memory Exhaustion/Fragmentation:** Increasing contract size leads to large memory allocations (e.g., for covariance matrices) that eventually fail or cause fragmentation, leading to a segfault later.
*   **Out-of-Bounds Access:** Data structures holding contract information or matrix data might be accessed incorrectly as the size grows near 23-25, perhaps due to an off-by-one error or incorrect size calculation.
*   **Threading Race Condition:** If portfolio updates, risk calculations, or optimization happen concurrently, a race condition might occur more frequently or become critical when data structures reach a certain size.
*   **RAII Failure:** A resource (memory, file handle, lock) might not be properly released due to an exception or complex control flow, becoming problematic under load.

## Investigation Steps & Findings

**1. Analysis of `apps/backtest/bt_trend.cpp`:**
   *   Confirms dynamic loading of symbols from DB; the number of instruments is variable and can reach the problematic ~23-25 range.
   *   Risk Management (`use_risk_management = true`) and Optimization (`use_optimization = true`) are enabled. These are primary suspects due to calculations potentially scaling poorly (e.g., N^2 for NxN covariance matrices) with the number of instruments (N).
   *   The core simulation logic is likely within `BacktestEngine`. Need to examine its run loop.
   *   No obvious explicit threading in `bt_trend.cpp` setup, but internal threading in core components (`BacktestEngine`, `PortfolioManager`, `DynamicOptimizer`, `DatabasePool`) is possible.
   *   An early `std::atomic_thread_fence` call suggests potential multi-threading awareness/concerns elsewhere in the codebase.

**2. Analysis of `include/trade_ngin/portfolio/portfolio_manager.hpp`:**
   *   Includes `<mutex>`, `dynamic_optimizer.hpp`, and `risk_manager.hpp`, confirming integration of optimization and risk management.
   *   Contains `std::unique_ptr<RiskManager> risk_manager_`, indicating risk logic is likely in the `RiskManager` class (files `risk/risk_manager.hpp` & `.cpp`).
   *   Uses `std::mutex mutex_`, highlighting potential threading issues (race conditions, deadlocks) during concurrent access/updates.
   *   Has an explicit `calculate_covariance_matrix()` method. This is a **high-priority suspect** for segfaults due to:
       *   Potential memory allocation failures for large matrices (N >= 23).
       *   Potential out-of-bounds access bugs in matrix math.
       *   Performance bottlenecks interacting with threading.
   *   Manages historical data buffers (`price_history_`, `historical_returns_`) which could grow large, but seem capped.

**3. Analysis of `src/portfolio/portfolio_manager.cpp` (Part 1: lines 1-500):**
   *   Constructor initializes `Optimizer` and `RiskManager`.
   *   Subscribes to `MarketDataBus` with a callback lambda to handle `BAR` and `POSITION_UPDATE` events.
   *   `add_strategy`: Uses `lock_guard` correctly.
   *   `process_market_data`:
       *   Uses a broad `lock_guard` for strategy processing and position updates.
       *   Calls `update_historical_returns` *inside* this lock.
       *   Calls `optimize_positions()` and `apply_risk_management()` *outside* this main lock. This is a potential issue if these functions access shared state without proper internal locking, or if the state changes concurrently.
       *   Re-acquires lock later for execution report generation.
   *   `update_historical_returns`:
       *   Accesses `strategy->get_price_history()` and modifies internal `price_history_` map.
       *   **Appears to lack mutex protection.** If `process_market_data` is called concurrently (e.g., via market data callback), this is a **potential race condition** leading to data corruption/segfaults.
   *   Implementations for `calculate_covariance_matrix`, `optimize_positions`, and `apply_risk_management` are still needed.

**4. Analysis of `src/portfolio/portfolio_manager.cpp` (Part 2: lines 501-750):**
   *   `update_historical_returns` (cont.):
       *   Calculates returns and populates `historical_returns_`.
       *   **Still no mutex protection observed.** Race condition remains a high probability if `process_market_data` is called concurrently.
   *   `calculate_covariance_matrix`:
       *   Allocates `aligned_returns` (`min_periods` x N) and `covariance` (N x N) matrices using `std::vector<std::vector<double>>` where N = `num_assets`.
       *   **Memory allocation for these matrices could fail when N approaches 23-25**, especially with fragmentation.
       *   **Potential for out-of-bounds access** in nested loops calculating covariance if indices (`i`, `j`, `t`) are incorrect near limits.
       *   Operates on input `returns_by_symbol` map. **If this map originates from the non-thread-safe `historical_returns_`, the input data could be corrupt.**
       *   **No internal mutex protection observed.**
   *   `optimize_positions`:
       *   Gathers data, including accessing the non-thread-safe `historical_returns_`.
       *   Calls `calculate_covariance_matrix` (passing potentially corrupt data).
       *   Calls `optimizer_->optimize(...)`.
       *   **No internal mutex protection observed.** Relies on consistent state of shared data like `historical_returns_`, which may not hold true due to the race condition.

**Strong Hypothesis:** The lack of mutex protection around reads/writes to `historical_returns_` (updated in `update_historical_returns`, read in `optimize_positions` which calls `calculate_covariance_matrix`) is a likely cause of the segfault. Concurrent access could lead to corrupted data structures being used in matrix calculations or optimization, causing crashes especially under load (more assets, more frequent updates).

**9. Analysis of `include/trade_ngin/risk/risk_manager.hpp`:**
   *   Main interface `process_positions(...)` takes positions and a `MarketData` struct (containing returns, covariance).
   *   Returns `RiskResult` with metrics and overall scaling factor.
   *   Includes helper `create_market_data(...)` to build input from bars.
   *   Appears stateless (holds config).
   *   No explicit mutexes or threading primitives visible in header.
   *   Defines internal helper methods `calculate_returns` and `calculate_covariance`. **Need to check implementation:** Does it recalculate covariance (another potential N^2 memory/access issue) or expect it as input (susceptible to corrupt data from `PortfolioManager`)?
   *   Calculates VaR, jump risk, correlation, leverage.

**10. Analysis of `src/risk/risk_manager.cpp` (Part 1: lines 1-250):**
    *   `process_positions` takes pre-calculated `MarketData` (including returns and covariance) as input. **It does NOT recalculate covariance here.**
    *   It maps input positions to the market data order and calculates weights.
    *   Calls helpers to calculate portfolio VaR, jump risk, correlation, leverage.
    *   **No internal mutexes or threading observed.**
    *   `calculate_portfolio_multiplier` uses the input `market_data.covariance` in nested loops (`w' * Sigma * w`). **Direct access to potentially corrupt covariance matrix.**
    *   `calculate_correlation_multiplier` also iterates through the input `market_data.covariance` matrix using nested loops. **Another direct access point to potentially corrupt covariance matrix.**
    *   **Conclusion:** Like the Optimizer, the RiskManager is highly susceptible to crashing due to receiving a corrupted covariance matrix from `PortfolioManager` (from race conditions). Out-of-bounds access or NaN propagation during variance/correlation calculations are likely failure modes.

**11. Analysis of `src/risk/risk_manager.cpp` (Part 2: lines 251-end):**
    *   `create_market_data` takes raw bar data, calculates returns, and then **recalculates the covariance matrix**.
        *   Allocates N x N `market_data.covariance` matrix (`resize`). **Second potential point of memory allocation failure for N~25.**
        *   Uses nested loops (`for i, for j, for k`) to calculate covariance. **Second potential point for out-of-bounds access.**
    *   Internal helper methods `calculate_returns` and `calculate_covariance` appear **unused** by the main flow.
    *   Implementations for `calculate_var` and `calculate_99th_percentile` are missing/incomplete in the provided snippets.

**Consolidated `RiskManager` Hypotheses:**
*   **Primary Suspect:** Independent recalculation of the N x N covariance matrix within `create_market_data` introduces a **second location** prone to memory allocation failures or out-of-bounds access when N grows large (~23-25).
*   **Secondary Suspect:** The subsequent use of this internally calculated (and potentially faulty) covariance matrix within `process_positions` (e.g., in `calculate_portfolio_multiplier`, `calculate_correlation_multiplier`) provides further opportunities for crashes if the matrix contains NaNs or is otherwise invalid.
*   **Inefficiency:** Recalculating returns and covariance frequently is inefficient but unlikely the direct segfault cause.

*(Further findings will be added here)*