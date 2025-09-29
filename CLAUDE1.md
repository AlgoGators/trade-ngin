Trade Ngin PnL Calculation Fixes - Session Summary
Overview
This session focused on fixing critical PnL calculation issues in the Trade Ngin live trading system where all PnL values were showing as $0.00 despite having active positions. A complete modular overhaul was implemented to address fundamental calculation and storage problems.
Note: The database schema has already been updated everywhere necessary, so no ALTER commands need to be applied again.
1. SQL Table Modifications and Database Changes
Schema Updates Already Applied
All database tables now include the proper columns for daily and cumulative PnL tracking, gross/net notional, previous price, and contract size.
Column comments and mappings have been verified.
Position Table Updates
Updated all queries from unrealized_pnl/realized_pnl to daily_unrealized_pnl/daily_realized_pnl
Added support for previous_price and contract_size columns
Position struct updated to include missing fields
SQL Query Improvements
INSERT and SELECT statements updated to retrieve all required columns
live_results table storage enhanced with full PnL and notional tracking
Reminder: While schema and queries are in place, PnL calculations still require detailed verification and further work.
2. Code Modularization and Architecture Changes
New Modular Components Created
PnL Calculator Module (src/strategy/pnl_calculator.cpp/.hpp)
Centralized, reusable PnL calculation logic
Handles futures-specific accounting (all PnL realized)
Supports contract size, daily vs cumulative PnL, execution-based calculations
Trading Logger Module (src/data/trading_logger.cpp/.hpp)
Modular database logging with deduplication
Tracks positions, portfolio results, executions, equity curve, and daily commissions
live_trend.cpp Refactoring
Replaced monolithic PnL calculations with modular approach
Added proper position change tracking and execution generation
Improved error handling and logging
Core Position Structure Updates
struct Position {
    // ... existing fields ...
    Decimal previous_price;  // Store previous day's market price
    Decimal contract_size;   // Store instrument contract size
};
3. PnL, Portfolio Value, and Execution Tracking Improvements
Current Status
PnL formulas implemented, contract sizes and previous prices stored
Portfolio and cumulative PnL tracking in place
Execution deduplication and commission calculation added
Critical: These calculations still need a lot of detailed verification. Users should carefully check PnL outputs against expected values before relying on them.
4. Key Technical Fixes Implemented
Fixed SQL column mappings and query parameterization
Integrated instrument registry for contract sizes
Futures-specific logic implemented (all PnL realized, proper daily settlement)
5. Outstanding Issues and Areas for Further Work
Mathematical accuracy of PnL calculations: still requires careful verification
Real-time price data accuracy: ensure get_latest_prices() returns correct values
Commission calculation integration: validate end-to-end
Integration testing: full end-to-end workflow testing required
Performance optimization: monitor with new modular architecture
6. Files Modified/Created
New Files
src/strategy/pnl_calculator.cpp
include/trade_ngin/strategy/pnl_calculator.hpp
src/data/trading_logger.cpp
include/trade_ngin/data/trading_logger.hpp
update_live_results_schema.sql (for reference only)
Major Files Modified
apps/strategies/live_trend.cpp (extensive refactoring)
src/data/postgres_database.cpp
include/trade_ngin/core/types.hpp (Position struct updates)
Database interface files (function signature updates)
7. Verification Status
✅ Implemented
Modular PnL calculation system
Contract size storage
Previous price tracking
Database schema compatibility
Compilation success
Live system runs
⚠️ Needs Verification
Mathematical accuracy of PnL calculations (priority)

> the issue is that the prreivous price you are using for calucaiton is not what is stored in \
"NQ.v.0"    2.000000    24720.250000    0.000000    106.000000    "2025-09-29 03:57:40+00"    
"2025-09-29 03:57:40+00"    "LIVE_TREND_FOLLOWING"    24726.50000000    2.00000000\
\
this means the calculatioons shoudl be 2 × (24720.25 - 24726.50) × 2.0 = -25.00\
I am not at all sure how you are getting your caluations \
  - NQ: 2 × (24726.50 - 24700.00) × 2 = 106 ✓


  
Real-time price data
Commission calculations
Full end-to-end testing
8. Next Steps for New Session
Focus on PnL calculations: step-by-step validation with known data
Verify all real-time price sources
Integration testing with live data
Performance monitoring of modular architecture
The modular foundation is set and database schema is fully updated. The main remaining work is fine-tuning and validating all PnL calculations and execution tracking.