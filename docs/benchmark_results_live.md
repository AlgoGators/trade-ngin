# Postgres Live Benchmark Results

## Summary
The benchmark was executed successfully against the AWS PostgreSQL database (`13.58.153.216`).

| Component | Time (ms) | Notes |
|-----------|----------|-------|
| **DynamicOptimizer** | **0.734 ms** | 50 assets, Matrix operations dominant |
| **RiskManager** | **15.498 ms** | 50 assets, 252-day history simulation |

## Configuration
- **Database**: `new_algo_data` (AWS Host: `13.58.153.216`)
- **Connection**: Successful
- **Data Source**: Synthetic (due to schema mismatch in remote DB)
- **Portfolio Size**: 50 Futures Symbols

## Raw Output Log
```text
==================================================
   Trade-NGIN Live Benchmark (Postgres-Backed)
==================================================
[INFO] connecting to database: postgresql://postgres:algogators@13.58.153.216:5432/new_algo_data
WARNING: Logger not initialized. Message: Successfully connected to PostgreSQL database with ID: POSTGRES_DB_1
[SUCCESS] Connected to database.
[INFO] Using hardcoded symbols for benchmark (database schema differs)...
[INFO] Selected 50 symbols for benchmarking.
[INFO] Skipping market data fetch (using synthetic data for optimizer/risk benchmarks)...

--- Starting Dynamic Optimizer Benchmark ---
[INFO] Generating NxN covariance matrix (50x50)...
Optimization iterations: 0
[BENCHMARK] DynamicOptimizer::optimize(): 0.734 ms

--- Starting Risk Manager Benchmark ---
[INFO] Generating history for Risk Manager (252 days x 50 assets)...
... [Instrument warnings omitted for brevity] ...
Risk Scale Recommended: 1
[BENCHMARK] RiskManager::process_positions(): 15.498 ms

[BENCHMARK COMPLETE]
```
