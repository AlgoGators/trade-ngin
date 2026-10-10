# trade-ngin
[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)]()
[![C++](https://img.shields.io/badge/C++-20-blue.svg)]()
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)

## 📖 Project Overview

trade-ngin is a high-performance, modular quantitative trading system built in C++20 designed for professional algorithmic traders and financial institutions. The system supports systematic trading strategies with a focus on futures trading, featuring comprehensive risk management, portfolio optimization, and realistic backtesting capabilities.

### Core Capabilities

| Capability | Description |
|------------|-------------|
| **Multi-Strategy Portfolio Management** | Dynamic capital allocation across multiple strategies (e.g., TREND_FOLLOWING, TREND_FOLLOWING_FAST) |
| **Risk Management** | Configurable risk modules and, on futures books, a risk overlay with five readings (risk, jump risk, shock risk, gross leverage, net leverage) and a per-name cap |
| **High-Performance Backtesting** | Realistic execution simulation with tick-based spread and square-root market impact models |
| **Live Trading Support** | A daily live cycle on paper (no broker connection), with position persistence, email reports and CSV exports |
| **Fixed-Point Arithmetic** | Custom Decimal class for financial precision (no floating-point errors) |
| **PostgreSQL Integration** | Apache Arrow for efficient data processing with connection pooling |
| **Transaction Cost Modeling** | Explicit (commissions) + Implicit (spread, market impact) cost decomposition |
| **Statistical Analysis** | A standalone library: PCA, stationarity and cointegration tests, regressions, GARCH-family models, Kalman filters, HMM, Markov switching |

---

## 🚀 Quick Start (First Clone)

This section provides step-by-step instructions to get trade-ngin running from a fresh clone.

### Step 1: Clone the Repository

```bash
git clone https://github.com/AlgoGators/trade-ngin.git
cd trade-ngin
```

### Step 2: Install System Dependencies

The scripts under `requirements/` install the system libraries the build uses (CMake, pkg-config,
GoogleTest, nlohmann_json, Apache Arrow, libpq and libpqxx, Eigen, NLopt, and on Ubuntu libcurl).
The Ubuntu script adds the Apache Arrow apt source, which `libarrow-dev` needs. The `Dockerfile`
installs `libnlopt-cxx-dev` as well, which the Ubuntu script does not.

**Ubuntu/Debian**

```bash
sudo bash requirements/install_ubuntu.sh
```

**macOS (Homebrew)**

```bash
bash requirements/install_macos.sh
```

The lint and coverage tools (`clang-format`, `cpplint`, `lcov`, `gcovr`) are not installed by these
scripts; install them separately if you need them (see [docs/CI_CD_README.md](docs/CI_CD_README.md)).

### Step 3: Configure Database Connection

Configuration uses a **template + local override** setup:

1. **Copy the template** (templates are committed; real configs are gitignored):
   ```bash
   cp -r config_template config
   ```

2. **Fill in placeholders** in `config/`:
   - `config/defaults.json`: the database placeholders `YOUR_DB_HOST`, `YOUR_DB_USERNAME`, `YOUR_DB_PASSWORD`, `YOUR_DB_NAME`
   - `config/portfolios/<name>/email.json` (for `base`, `conservative` and `equity_mr`): SMTP credentials and recipients

The `config/` directory is gitignored so credentials and local overrides are never committed. See [Configuration](#configuration) for details.

### Step 4: Build the Project

```bash
# Create build directory
mkdir -p build && cd build

# Configure CMake (Release)
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build . -j

# Return to project root
cd ..
```

### Step 5: Verify Installation

```bash
# Check executables exist
ls -la build/bin/Release/

# Expected outputs, among others (bt_equity_validation, bt_transaction_cost_report,
# trade_ngin_tests):
# bt_portfolio_conservative    - CONSERVATIVE futures backtest
# bt_portfolio                 - BASE futures backtest
# bt_equity_mr                 - equity backtest
# live_portfolio_conservative  - CONSERVATIVE futures live run
# live_portfolio               - BASE futures live run
# live_equity_mr               - equity live run
```

### Step 6: Run Your First Backtest

A backtest needs a database with market data and the migrations in `migrations/` applied. Run it
from the repository root (the runners read `./config`):

```bash
./build/bin/Release/bt_portfolio_conservative
```

Run one runner at a time. The backtest and live runners share tables and process-wide state, so two
runners must never run against the same database at once.

---

## 🚦 Quick Reference

| Document | Purpose |
|----------|---------|
| [Config Template](config_template/README.md) | Setting up `config/` from the templates, the placeholder list |
| [Config Guide](docs/CONFIG_GUIDE.md) | Every configuration file and key |
| [System Architecture](docs/SYSTEM_ARCHITECTURE.md) | The module map, the runners, what reads and writes what |
| [Trend Following System](docs/TREND_FOLLOWING_SYSTEM.md) | The futures strategy end to end, with every setting in force |
| [Optimizer and Risk Design](docs/OPTIMIZER_AND_RISK_DESIGN.md) | How a futures rebalance turns targets into the stored book |
| [Risk Modules](docs/RISK_MODULES.md) | The risk modules, the overlay and `risk.json` |
| [Cost Model](docs/COST_MODEL.md) | What a fill costs, fees, netting between sleeves |
| [Futures Rolls](docs/FUTURES_ROLLS.md) | The adjusted series, change bars, roll fills |
| [Live Run Cycle](docs/LIVE_RUN_CYCLE.md) | What a live run does on a date, the P&L frame, the stored tables |
| [Performance & Upkeep](docs/performance_upkeep.md) | Operations: the schedule, the wrapper, the watchdog, migrations |
| [Data Sources of Truth](docs/DATA_SOURCES_OF_TRUTH.md) | Which table is the source of each fact and how to read it |
| [Equity Strategy Guide](docs/EQUITY_STRATEGY_GUIDE.md) | The equity mean-reversion book |
| [Corporate Actions Data Boundary](docs/CORP_ACTIONS_DATA_BOUNDARY.md) | How corporate actions are applied |
| [Average Price Lifecycle](docs/AVERAGE_PRICE_LIFECYCLE.md) | What `average_price` means on a stored position |
| [Broker Basis Reconciliation](docs/BROKER_BASIS_RECONCILIATION.md) | Reconciling stored positions with a broker statement |
| [CI/CD Pipeline](docs/CI_CD_README.md) | GitHub Actions workflows, linting, coverage |
| [Docker Guide](docs/README.Docker.md) | Container deployment |

### Module-Specific Documentation

| Module | README | Purpose |
|--------|--------|---------|
| Data | [src/data/README.md](src/data/README.md) | PostgreSQL, Arrow, connection pooling |
| Transaction Cost | [src/transaction_cost/README.md](src/transaction_cost/README.md) | Spread and impact models, netting |
| Statistics | [src/statistics/README.md](src/statistics/README.md) | The statistical analysis library |
| Optimization | [src/optimization/README.md](src/optimization/README.md) | The one pass (futures) and the generic optimiser step |
| Live Trading | [src/live/README.md](src/live/README.md) | The live run |
| Backtest | [src/backtest/README.md](src/backtest/README.md) | BacktestCoordinator, the window, the metrics |
| Strategy | [src/strategy/README.md](src/strategy/README.md) | The strategy interface and the trend sleeve |
| Portfolio | [src/portfolio/README.md](src/portfolio/README.md) | Sleeves, sizing capital, the rebalance |

---

## 🎯 Current System State

### Implemented Features

- ✅ **Trend following on futures** with EWMAC forecasts, in two sleeves: TREND (six EMA pairs) and FAST (the four fast pairs)
- ✅ **Two futures books**: CONSERVATIVE (one sleeve) and BASE (two sleeves)
- ✅ **36 futures contracts**, with the four equity index micros traded from their listing date and the E-mini contracts before it
- ✅ **Equity mean-reversion book** with corporate action handling
- ✅ **Whole-contract rebalance** from the held book with a cost penalty, a no-trade buffer and a per-name cap
- ✅ **Risk overlay and risk modules** configured per book in `risk.json`
- ✅ **Sizing on half-compounded capital**
- ✅ **Roll handling** on a back-adjusted series, with roll fills and their costs booked apart
- ✅ **Transaction cost model** (per-contract fees, tick-based spread, square-root impact), with netting between sleeves
- ✅ **Fixed-point arithmetic** with a custom Decimal class
- ✅ **PostgreSQL integration** with Apache Arrow and connection pooling
- ✅ **Live runs** with position persistence, email reports and CSV exports
- ✅ **Closed-day and holiday handling**
- ✅ **Logging** to console and rotating files

### Configuration

Configuration uses a **template + local override** model:

| Directory       | Tracked in Git? | Purpose                                                                 |
|----------------|-----------------|-------------------------------------------------------------------------|
| `config_template/` | Yes             | Template files with placeholders; safe to commit                        |
| `config/`          | No (gitignored) | Local config with real credentials; never committed                     |

**Setup flow:**
1. Copy: `cp -r config_template config`
2. Edit `config/defaults.json`: replace `YOUR_DB_HOST`, `YOUR_DB_USERNAME`, `YOUR_DB_PASSWORD`, `YOUR_DB_NAME`
3. Edit `config/portfolios/<name>/email.json` for each book: replace the SMTP credentials and recipient emails

**Structure:**
```
config/
├── defaults.json           # Database, execution, optimization, backtest, live, strategy_defaults (shared)
└── portfolios/
    ├── base/               # BASE_PORTFOLIO: bt_portfolio, live_portfolio
    │   ├── portfolio.json  # Strategies, capital, allocations, sizing mode
    │   ├── risk.json       # Risk modules and the overlay's limits
    │   └── email.json      # Email notifications
    ├── conservative/       # CONSERVATIVE_PORTFOLIO: bt_portfolio_conservative, live_portfolio_conservative
    │   ├── portfolio.json
    │   ├── risk.json
    │   └── email.json
    └── equity_mr/          # the equity book: bt_equity_mr, live_equity_mr
        ├── portfolio.json
        ├── risk.json
        └── email.json
```

**Loading:** Each runner loads its own book from `./config`, for example `ConfigLoader::load("./config", "conservative")`. Defaults are merged with the book's files.

For the full placeholder list see `config_template/README.md`; for every key see [docs/CONFIG_GUIDE.md](docs/CONFIG_GUIDE.md).

### Example Configuration

Example `config/defaults.json` database block (after replacing placeholders):

```json
{
  "database": {
    "host": "your-database-host",
    "port": "your-database-port",
    "username": "your-username",
    "password": "your-password",
    "name": "your-database-name",
    "num_connections": 5
  }
}
```

Example `config/portfolios/base/portfolio.json` (the strategies block; the template carries the
other required keys):

```json
{
  "portfolio_id": "BASE_PORTFOLIO",
  "initial_capital": 500000,
  "strategies": {
    "TREND_FOLLOWING": {
      "enabled_backtest": true,
      "enabled_live": true,
      "default_allocation": 0.7,
      "type": "TrendFollowingStrategy",
      "config": {
        "risk_target": 0.2,
        "idm": 2.5,
        "ema_windows": [[2, 8], [4, 16], [8, 32], [16, 64], [32, 128], [64, 256]],
        "vol_lookback_short": 32,
        "vol_lookback_long": 252
      }
    },
    "TREND_FOLLOWING_FAST": {
      "enabled_backtest": true,
      "enabled_live": true,
      "default_allocation": 0.3,
      "type": "TrendFollowingFastStrategy",
      "config": {
        "risk_target": 0.25,
        "idm": 2.5,
        "ema_windows": [[2, 8], [4, 16], [8, 32], [16, 64]],
        "vol_lookback_short": 16,
        "vol_lookback_long": 252
      }
    }
  }
}
```

> **Note**: `vol_lookback_long` is in the template and is not read by anything that computes: the estimator's long-run window is a constant of the code (see [docs/CONFIG_GUIDE.md](docs/CONFIG_GUIDE.md), "Keys that are present and not read").
>
> **Note**: Each strategy can be enabled or disabled independently for backtest and for live. The runners normalise the `default_allocation` values to sum to 1.0 and log a warning when the configured values do not; they are not required to sum to 1.0 in the file.

### Running Multiple Portfolios

The repository carries three books, each with its own `portfolio_id`, its own directory under
`config/portfolios/` and its own runners:

| Book | Config directory | Backtest | Live |
|------|------------------|----------|------|
| CONSERVATIVE (futures, one sleeve) | `conservative` | `bt_portfolio_conservative` | `live_portfolio_conservative` |
| BASE (futures, two sleeves) | `base` | `bt_portfolio` | `live_portfolio` |
| Equity mean reversion | `equity_mr` | `bt_equity_mr` | `live_equity_mr` |

Stored rows are separated by `portfolio_id` and each book has its own email report. The two futures
books each write their position CSV files under `apps/strategies/results/<portfolio_id>/`; the
equity book writes no CSV. The books are independent, but the runners are run one at a time, never
in parallel.

### Key Parameters Explained

The values below are the ones in `config_template/` for the CONSERVATIVE book. The full list, with
the file and key of each, is in [docs/CONFIG_GUIDE.md](docs/CONFIG_GUIDE.md).

| Parameter | Value | Where |
|-----------|-------|-------|
| **Initial capital** | 500,000 | `portfolio.json` `initial_capital` |
| **Sizing mode** | `half_compounding` | `portfolio.json` `sizing_mode` |
| **Risk target** | 0.20 | `portfolio.json` sleeve `risk_target` |
| **IDM** | 2.5 | `portfolio.json` sleeve `idm` |
| **Gross leverage limit** | 8.0 | `risk.json` carver module `max_gross_leverage` |
| **Net leverage limit** | 6.0 | `risk.json` carver module `max_net_leverage` |
| **Risk limits (ratios to the risk target)** | 2.25, 4.5, 4.0 | `risk.json` `R_max`, `R_jump_max`, `R_shock_max` |
| **Per-name cap** | 2 | `risk.json` `per_name_cap` |
| **Cost multiplier of the rebalance** | 100 | `defaults.json` `optimization.cost_penalty_scalar` |

---

## 📂 Repository Structure

```
trade-ngin/
├── apps/
│   ├── backtest/                   # bt_portfolio.cpp, bt_portfolio_conservative.cpp,
│   │                               #   bt_equity_mean_reversion.cpp, bt_equity_validation.cpp,
│   │                               #   bt_transaction_cost_report.cpp
│   └── strategies/                 # live_portfolio.cpp, live_portfolio_conservative.cpp,
│                                   #   live_equity_mean_reversion.cpp
│
├── include/trade_ngin/             # Public headers, one directory per module
│   ├── backtest/                   # coordinator, data loader, execution, P&L, metrics, CSV export
│   ├── core/                       # types (Decimal, Bar, Position, ExecutionReport), error, logger,
│   │                               #   config_loader, email_sender, chart_generator, holiday_checker,
│   │                               #   time_utils
│   ├── data/                       # postgres_database, database_pooling, market_data_bus,
│   │                               #   market_data_utils, session_classifier, roll_series, listing_dates
│   ├── execution/                  # execution_engine
│   ├── instruments/                # instrument, futures, equity, option, instrument_registry
│   ├── live/                       # the live run: daily cycle, data loader, P&L, prices, margin,
│   │                               #   corporate actions, CSV export, roll legs, sizing reads
│   ├── optimization/               # one_pass (futures rebalance), dynamic_optimizer
│   ├── order/                      # order_manager
│   ├── portfolio/                  # portfolio_manager, sizing_capital, allocation_split, loop_config
│   ├── risk/                       # risk_module, carver_risk_module, basic_risk_modules, overlay,
│   │                               #   risk_module_config, risk_detail, risk_manager
│   ├── statistics/                 # the statistical analysis library
│   ├── storage/                    # backtest_results_manager, live_results_manager
│   ├── strategy/                   # strategy_interface, base_strategy, trend_following,
│   │                               #   trend_estimator, mean_reversion
│   └── transaction_cost/           # transaction_cost_manager, spread_model, impact_model,
│                                   #   asset_cost_config, netting
│
├── src/                            # Implementations, the same module directories as include/
├── tests/                          # Unit tests, one directory per module, plus tests/scripts/
├── benchmarks/                     # Benchmark harness
├── migrations/                     # Numbered SQL migrations, each with a rollback
├── docs/                           # Reference documents (see Quick Reference)
├── scripts/                        # run_live_portfolio.sh (the scheduled wrapper),
│                                   #   docker-entrypoint.sh, check_live_trading.py (the watchdog),
│                                   #   migrate_risk_json.py, trading_rule_costs.py,
│                                   #   generate_market_holidays.py, pre-commit-hook.sh,
│                                   #   setup-dev-environment.sh, dev_build_run.sh, and two SQL files
├── requirements/                   # install_ubuntu.sh, install_macos.sh
├── config_template/                # Config templates (committed, no secrets)
│   ├── defaults.json
│   └── portfolios/                 # base/, conservative/, equity_mr/
├── config/                         # Local config (gitignored; copy from config_template)
├── cmake/                          # CMake modules
├── CMakeLists.txt
├── Makefile
├── Dockerfile
├── build_docker.sh
└── live_portfolio.cron             # the scheduled job
```

---

## ⚙️ System Architecture

The module map, the runners and what each reads and writes are in
[docs/SYSTEM_ARCHITECTURE.md](docs/SYSTEM_ARCHITECTURE.md). In brief:

| Layer | Modules | Role |
|-------|---------|------|
| Core | `core/` | types, errors, logging, configuration loading, email, charts |
| Data | `data/`, `instruments/` | database access, bar loading, session classification, roll series, contract metadata |
| Strategy | `strategy/` | forecasts and unrounded targets per sleeve |
| Portfolio | `portfolio/`, `optimization/`, `risk/` | sizing capital, the rebalance, the risk overlay and modules |
| Costs | `transaction_cost/` | what a fill costs, netting between sleeves |
| Backtest | `backtest/`, `storage/` | replaying stored bars, metrics, stored results |
| Live | `live/`, `storage/` | the daily live run, P&L, reports, stored results |

Bars flow from the data layer to the sleeves; the `PortfolioManager` turns the sleeves' targets into
one whole-contract book; the backtest coordinator or a live runner books the fills, the costs and
the P&L and stores them.

---

## 🔢 Core Type System

trade-ngin uses a custom type system for financial precision:

### Decimal Class

The `Decimal` class (`include/trade_ngin/core/types.hpp`) provides fixed-point arithmetic with 8 decimal places:

```cpp
// 8 decimal places for financial precision
class Decimal {
    static constexpr int64_t SCALE = 100000000LL;  // 10^8
    int64_t value_;
    
public:
    Decimal(double d);  // Converts with rounding
    explicit operator double() const;
    
    // Arithmetic with overflow checking
    Decimal operator+(const Decimal& other) const;
    Decimal operator*(const Decimal& other) const;
    // ... etc
    
    // Utilities
    Decimal abs() const;
    bool is_zero() const;
    std::string to_string() const;
};
```

### Type Aliases

```cpp
using Timestamp = std::chrono::system_clock::time_point;
using Price = Decimal;
using Quantity = Decimal;
```

### Core Structures

```cpp
// Market data bar
struct Bar {
    Timestamp timestamp;
    Price open, high, low, close;
    double volume;
    std::string symbol;
    std::string instrument_id;         // the vendor's contract id behind a futures bar
};

// Position tracking
struct Position {
    std::string symbol;
    Quantity quantity;
    Price average_price;
    Decimal unrealized_pnl;
    Decimal realized_pnl;
    Timestamp last_update;
    std::string instrument_id;         // the contract a futures position is held in
};

// Execution report with cost breakdown (the main fields)
struct ExecutionReport {
    std::string order_id;
    std::string exec_id;
    std::string symbol;
    Side side;
    Quantity filled_quantity;
    Price fill_price;                  // reference fill price, no costs embedded
    Timestamp fill_time;
    Decimal commissions_fees;          // explicit costs
    Decimal implicit_price_impact;     // spread + impact, in price units
    Decimal slippage_market_impact;    // implicit costs in dollars
    Decimal total_transaction_costs;   // commissions_fees + slippage_market_impact
    Decimal netting_adjustment;        // see docs/COST_MODEL.md
    ExecutionType execution_type;      // STRATEGY, ROLL or BORROW
    std::string instrument_id;
};
```

A cost total is always taken after netting, through `transaction_cost::net_cost`; see
[docs/COST_MODEL.md](docs/COST_MODEL.md).

---

## Logging System

trade-ngin includes a comprehensive, thread-safe logging system for debugging, monitoring, and auditing.

### Log Levels

| Level | Macro | Usage |
|-------|-------|-------|
| `TRACE` | `TRACE(msg)` | Detailed debug information (very verbose) |
| `DEBUG` | `DEBUG(msg)` | General debug information |
| `INFO` | `INFO(msg)` | General operational information |
| `WARNING` | `WARN(msg)` | Warnings that don't affect operation |
| `ERR` | `ERROR(msg)` | Errors that affect operation but don't stop system |
| `FATAL` | `FATAL(msg)` | Critical errors requiring system shutdown |

### Using the Logger

```cpp
#include "trade_ngin/core/logger.hpp"

// Initialize once at startup
LoggerConfig config;
config.min_level = LogLevel::INFO;
config.destination = LogDestination::BOTH;  // Console + file
config.log_directory = "logs";
config.filename_prefix = "live_trend";
config.max_file_size = 50 * 1024 * 1024;  // 50MB
config.max_files = 10;

Logger::instance().initialize(config);

// Log messages
INFO("Portfolio initialized with capital: $" << initial_capital);
DEBUG("Loading positions for date: " << date_str);
WARN("No market data available for: " << symbol);
ERROR("Database connection failed: " << error_msg);
```

### Log Output Format

```
2025-01-15 09:30:45 [INFO] Portfolio initialized with capital: $500000
2025-01-15 09:30:45 [DEBUG] Loading instruments from registry
2025-01-15 09:30:46 [INFO] [TrendFollowingStrategy] Generating signals
```

### What Is Stored in the Database

The runs' results are stored in the database, not their log lines. A backtest writes to the
`backtest` schema and a live run to the `trading` schema; what each table holds is in
[docs/LIVE_RUN_CYCLE.md](docs/LIVE_RUN_CYCLE.md).

### Log File Location

Log files are written under `logs/` as `<prefix>_YYYYMMDD_HHMMSS_partN.log`. The prefix is set by
each runner. A dated `live_equity_mr` run writes under `logs/<YYYY-MM-DD>/`.

| Runner | Prefix |
|--------|--------|
| `bt_portfolio` | `bt_portfolio` |
| `bt_portfolio_conservative` | `bt_portfolio_conservative` |
| `bt_equity_mr` | `bt_equity_mr` |
| `live_portfolio` | `live_trend` |
| `live_portfolio_conservative` | `live_trend_conservative` |
| `live_equity_mr` | `live_equity_mr` |

### Changing Log Level at Runtime

```cpp
// Change minimum log level
Logger::instance().set_level(LogLevel::DEBUG);

// Get current level
LogLevel current = Logger::instance().get_min_level();
```

---

## 💰 Transaction Cost Model

A fill's cost is an explicit per-contract (or per-share) fee plus an implicit cost made of a
tick-based spread and a square-root market impact. Futures fees are read per contract from the
contract metadata. On a book with more than one sleeve, the fills of one symbol on one day are
netted, and every cost total is taken after that netting.

The formulas, the inputs, the values in force and worked examples are in
[docs/COST_MODEL.md](docs/COST_MODEL.md). The code is in `src/transaction_cost/`
([README](src/transaction_cost/README.md)).

---

## 📊 Statistics Module

The statistics module is a standalone analysis library: transformers (normalisation, PCA),
pre-processing (outliers, missing data), stationarity and cointegration tests (ADF, KPSS,
Phillips-Perron, variance ratio, Johansen, Engle-Granger), the Hurst exponent, regressions (OLS,
ridge, lasso), volatility models (GARCH, EGARCH, GJR-GARCH, DCC-GARCH) and state estimators (Kalman
filter, extended Kalman filter, HMM, Markov switching). The trading path does not use it. The class
list and a usage example are in [src/statistics/README.md](src/statistics/README.md).

---

## 🔄 Workflows

### Backtesting Workflow

1. The runner loads its book from `./config`, connects to the database and loads the instruments.
2. `BacktestCoordinator::run_portfolio` loads the bars of the window. The window is
   `backtest.lookback_years` ending at the end date, and its first 256 rows are warm-up for a trend
   book (see [src/backtest/README.md](src/backtest/README.md)).
3. For each bar date: the sleeves are fed and publish their targets; the `PortfolioManager`
   rebalances the book (see [docs/OPTIMIZER_AND_RISK_DESIGN.md](docs/OPTIMIZER_AND_RISK_DESIGN.md));
   the fills, their costs and the day's P&L are booked and a row is added to the equity curve.
4. The metrics are computed over the rows after the warm-up and the run is stored in the `backtest`
   schema.

### Live Trading Workflow

A live run is for one date. In production the scheduled wrapper `scripts/run_live_portfolio.sh`
runs `live_portfolio_conservative <date> --send-email` once a day. The run loads the stored book
and the bars up to the day before its date, sizes and rebalances the book, books the previous day's
P&L, stores positions, executions and results, exports the position CSV files (futures books) and,
with `--send-email`, mails the report.

What a run does step by step, the P&L frame and what each table holds are in
[docs/LIVE_RUN_CYCLE.md](docs/LIVE_RUN_CYCLE.md). The schedule, the wrapper's exit codes, the
watchdog and the rules for manual and catch-up runs are in
[docs/performance_upkeep.md](docs/performance_upkeep.md).

---

## 🛠️ Setup & Installation

> **Quick Start**: For the fastest setup, follow the [Quick Start](#-quick-start-first-clone) section.

### Prerequisites

| Requirement | Version | Purpose |
|-------------|---------|---------|
| C++ Compiler | GCC 10+ / Clang 10+ / MSVC 2019+ | C++20 support |
| CMake | 3.17+ | Build configuration |
| PostgreSQL | 12+ | Market data storage |
| nlohmann_json | Latest | JSON configuration |
| NLopt | Latest | Required by the build |
| Apache Arrow C++ | Latest | Efficient data processing |
| libpqxx | Latest | PostgreSQL C++ client |
| Eigen3 | Latest | Linear algebra for optimization |
| libcurl | Latest | Email functionality |
| GoogleTest | Latest | Unit testing |

### System Requirements

- **Memory**: 8GB+ RAM for typical backtests
- **Storage**: SSD recommended for database
- **OS**: Linux (preferred), macOS, Windows (WSL2)

### Debug Build (with coverage)

```bash
mkdir -p build && cd build

cmake .. \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-g -O0 -fprofile-arcs -ftest-coverage"

cmake --build . -j
cd ..
```

### Running Unit Tests

```bash
cd build
ctest --output-on-failure --verbose
```

---

## 🚀 Running the System

Run every runner from the repository root, and run one at a time: never start two runners (backtest
or live) in parallel against the same database.

### Running Backtests

```bash
# CONSERVATIVE futures book
./build/bin/Release/bt_portfolio_conservative

# BASE futures book
./build/bin/Release/bt_portfolio

# Equity book
./build/bin/Release/bt_equity_mr
```

### Running Live Trading

A live run takes an optional date, `YYYY-MM-DD`, and an optional `--send-email`. Always pass the
date:

```bash
# A run for a date, no email
./build/bin/Release/live_portfolio_conservative 2025-01-15

# The production form: a date and the email report
./build/bin/Release/live_portfolio_conservative 2025-01-15 --send-email

# The same run, writing the report body to a file instead of mailing it
TRADE_NGIN_EMAIL_BODY_DIR=/path/to/dir ./build/bin/Release/live_portfolio_conservative 2025-01-15
```

- **Run modes.** A live run is in one of three modes, by its arguments and by the host's date: no
  date; a date equal to the host's date (the scheduled run); a past date (a replay or a catch-up).
  With a date, the run clock is midnight of that date, the bar window ends the day before, a stale
  or incomplete feed is a warning, and the email is sent only with `--send-email`. With no date the
  run clock is the wall clock with its time of day, a bar dated the run day is read if the feed has
  already loaded it, a stale or incomplete feed stops the run with exit 1, and the email is always
  sent. A held symbol with no bar for more than `live.data_staleness_tolerance_days` stops a futures
  run for the host's date or with no date, and is a warning on a past date. No code refuses a run
  with no date; the scheduled run always passes one. The full table is in
  [docs/LIVE_RUN_CYCLE.md](docs/LIVE_RUN_CYCLE.md).
- **The date and the host.** The two futures runners read the date as midnight in the host's local
  time and the equity runner as midnight UTC; every stored key is rendered in UTC. On a host
  whose clock is UTC the two are the same instant. The image in this repository sets
  `TZ=America/New_York` (`Dockerfile:85`); [docs/LIVE_RUN_CYCLE.md](docs/LIVE_RUN_CYCLE.md) says what
  that changes.
- **The report body file.** `TRADE_NGIN_EMAIL_BODY_DIR` applies only to a futures run that does not
  send: with the variable set to a directory, the run builds the report body exactly as for a send,
  writes it there as `email_body_<portfolio>_<date>.html` and mails nothing. With `--send-email` the
  variable is ignored with one warning line in the log, and the report is mailed as usual. A run
  with no date sends, so it ignores the variable in the same way. The equity runner does not read
  the variable.
- **Exit codes.** A futures live run exits 0 when it reaches the end of the run with the book not
  refused, 3 when it reaches the end with the book held because the risk step refused it or the
  sizing read failed, and 1 when it stops early. Exit 0 covers a completed day, a carried day, and
  a run in which a store failed, was logged and was passed over; the evidence of a completed day is
  the date's `trading.live_results` row, not the exit code. The wrapper also returns 0, without
  starting the binary, when it finds its lock held (nothing stored). A refusal by the risk step never ends at 0: the
  `live_run_metadata` row is marked `risk_refusal` and the run ends at 3, whether a module could not
  answer or a sleeve module decided to refuse, or at 1 when the refused book has no stored previous
  positions to be held at. Exit 1 stores nothing when the run refuses before its `live_run_metadata`
  row is written, and can leave that row, the signals or a half-written day when it stops later. The
  equity runner and the backtest runners return only 0 or 1.
- **Re-runs.** A lone re-run of an older date is not supported and is not refused: the runner
  accepts it, rewrites that day and the day before, and leaves every later day as it was. A replay
  runs every date forward in order. See [docs/performance_upkeep.md](docs/performance_upkeep.md).

### Output Locations

| Output Type | Location | Description |
|-------------|----------|-------------|
| Logs | `logs/<prefix>_YYYYMMDD_HHMMSS_partN.log` | One set of files per run |
| CSV Exports | `apps/strategies/results/<portfolio_id>/` | Live position files of the futures books (the day's positions and the finalized positions of the day before) |
| Backtest CSV | `apps/backtest/results/` | Backtest output |
| Database | `trading` and `backtest` schemas | See [docs/LIVE_RUN_CYCLE.md](docs/LIVE_RUN_CYCLE.md) |

---

## 📝 Additional Configuration

### Strategy Types

The `type` string of a sleeve in `portfolio.json` selects what the futures runners build:

| Type | Built as | Description |
|------|----------|-------------|
| `TrendFollowingStrategy` | `TrendFollowingStrategy` with the sleeve's configuration | The TREND sleeve: six EMA pairs |
| `TrendFollowingFastStrategy` | `TrendFollowingStrategy` with `fast_trend_following_config()` | The FAST sleeve: the four fast EMA pairs |

The equity book's strategies are built by `equity_strategy_builder.hpp`.

### Email Configuration

`config/portfolios/<name>/email.json` is a flat object with the keys `smtp_host`, `smtp_port` (a
number), `username`, `password`, `from_email`, `to_emails`, `to_emails_production` and `use_tls`.
Copy it from `config_template/portfolios/<name>/email.json` and replace the placeholders. The last
two keys are present and not read: every report goes to `to_emails`, and the sender always asks for
TLS.

> **Gmail Setup**: Use an [App Password](https://support.google.com/accounts/answer/185833) rather than your account password.

---

## 🗄️ Database Schema

The engine reads market data and contract metadata and writes its own results:

| Schema | Tables the runners use | Purpose |
|--------|------------------------|---------|
| `trading` | `positions`, `executions`, `signals`, `live_results`, `equity_curve`, `live_run_metadata`, `strategy_trading_days_metadata`, `corp_action_applied` | Live results |
| `backtest` | `results`, `equity_curve`, `executions`, `final_positions`, `run_metadata`; `signals` (present, never written) | Backtest results |
| `metadata` | `contract_metadata` | Contract size, tick size, fee per contract |
| `futures_data` | `ohlcv_1d`, `ohlcv_1d_raw` | Daily futures bars (read only) |
| `equities_data` | `ohlcv_1d`, `corporate_action`, `ticker_aliases` | Equity data (read only) |

What one row of each `trading` and `backtest` table is, its key, who writes it and when it is final
are in [docs/LIVE_RUN_CYCLE.md](docs/LIVE_RUN_CYCLE.md). Which table is the source of each fact is
in [docs/DATA_SOURCES_OF_TRUTH.md](docs/DATA_SOURCES_OF_TRUTH.md). Schema changes are the numbered
files in `migrations/`.

---

## 🏗️ Creating a New Strategy

The interface, the base class and the steps are in [src/strategy/README.md](src/strategy/README.md).
In short:

1. **Create the header** in `include/trade_ngin/strategy/your_strategy.hpp` and the implementation
   in `src/strategy/your_strategy.cpp`
2. **Inherit from** `BaseStrategy`, whose constructor is
   `BaseStrategy(std::string id, StrategyConfig config, std::shared_ptr<PostgresDatabase> db)`
3. **Override** `initialize()` and `on_data()`, and `validate_config()` for parameter checks
4. **Register** the `.cpp` in the root `CMakeLists.txt` (add it to `TRADE_NGIN_SOURCES`)
5. **Add a branch** for its `type` string where the runners build their sleeves (for example in
   `live_portfolio.cpp` and `bt_portfolio.cpp`)
6. **Configure** it in `config/portfolios/<name>/portfolio.json` under `strategies`

---

## 🛡️ Error Handling

trade-ngin uses the `Result<T>` pattern for error propagation (see `include/trade_ngin/core/error.hpp`):

```cpp
Result<void> MyComponent::operation() {
    auto result = other_component->do_something();
    if (result.is_error()) {
        return make_error<void>(
            result.error()->code(),
            "Failed during operation: " + std::string(result.error()->what()),
            "MyComponent"
        );
    }
    return Result<void>();
}

// Usage
auto result = component.operation();
if (result.is_error()) {
    ERROR(result.error()->what());
    return 1;
}
```

### Common Error Codes

| Code | Meaning |
|------|---------|
| `INVALID_ARGUMENT` | Invalid parameter provided |
| `NOT_INITIALIZED` | Component not initialized |
| `DATABASE_ERROR` | Database operation failed |
| `DATA_NOT_FOUND` | Requested data not found |
| `INVALID_DATA` | Data failed a validity check |
| `STRATEGY_ERROR` | Strategy logic error |
| `RISK_LIMIT_EXCEEDED` | Risk constraint violated |

---

## 📊 Performance Considerations

### Data Processing

- Apache Arrow for zero-copy data sharing
- Columnar memory layout for vectorized operations
- Connection pooling for database efficiency (5 connections by default)

### Memory Management

- RAII pattern throughout
- Smart pointers (`std::shared_ptr`, `std::unique_ptr`) for automatic resource management
- Preallocated buffers for performance-critical operations

### Concurrency

- Components guard their own state with mutexes, and the connection pool hands out connections safely
- Pub-sub pattern for market data distribution
- Runners are not concurrent with each other: they share database tables and process-wide
  singletons, so run one runner at a time, never two in parallel

### Optimization Tips

- Use `reserve()` on vectors when size is known
- Avoid unnecessary copies (use `const&` or move semantics)
- Profile with `perf` or `valgrind` for bottlenecks

---

## 🐳 Docker Deployment

The container image is built from the `Dockerfile`:

```bash
docker build -t trade-ngin -f Dockerfile .

docker run -d \
    --name trade-ngin \
    -v $(pwd)/config:/app/config \
    -v $(pwd)/logs:/app/logs \
    trade-ngin
```

### Docker Configuration

The container includes:
- All system dependencies
- The scheduled job (`live_portfolio.cron`), which calls `scripts/run_live_portfolio.sh`
- Gnuplot for chart generation
- Timezone set to America/New_York

Mount your local `config/` directory (created from `config_template/`) into the container so it has
database and email credentials. See [docs/README.Docker.md](docs/README.Docker.md) and
[docs/performance_upkeep.md](docs/performance_upkeep.md).

---

## 🔧 CI/CD Pipeline

The project uses GitHub Actions for continuous integration. See [docs/CI_CD_README.md](docs/CI_CD_README.md) for details.

### Pipelines

| Workflow | Trigger | Purpose |
|----------|---------|---------|
| `ci-cd-pipeline.yml` | Push to `main`, `develop`, `main-hd`, `prod` or `staging`; every pull request | Lint, build (Debug and Release), test, coverage, security scan, image |
| `live-trading-watchdog.yml` | Daily schedule, manual, pull requests that touch the watchdog | Checks that the live run wrote its rows |
| `dependency-review.yml` | Pull requests onto `main` | Dependency review |
| `sbom.yml`, `scorecard.yml`, `branch-protection.yml` | Push to `main`, schedules | Supply-chain and repository settings |

### Local Pre-commit Checks

```bash
# Run before each commit
./scripts/pre-commit-hook.sh

# Check formatting
find src include -name "*.cpp" -o -name "*.hpp" | xargs clang-format --dry-run --Werror

# Fix formatting in place
find src include -name "*.cpp" -o -name "*.hpp" | xargs clang-format -i
```

### Coverage Requirements

- **Threshold enforced by CI**: 10 percent line coverage (`COVERAGE_THRESHOLD` in `ci-cd-pipeline.yml`), checked in the Debug build
- **Tools**: lcov, gcovr
- **Reports**: lcov, XML (Cobertura), text

---

## 🧪 Testing

### Running Tests

```bash
cd build

# Run all tests
ctest --output-on-failure

# Run the suites whose names match a pattern
ctest -R "OnePass|HalfCompounding" --output-on-failure

# Run with verbose output
ctest -V

# Run with coverage
cmake .. -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-g -O0 -fprofile-arcs -ftest-coverage"
cmake --build . -j
ctest
lcov --capture --directory . --output-file coverage.info
genhtml coverage.info --output-directory coverage_html
```

### Test Structure

Every C++ test is built into one binary, `trade_ngin_tests`; the Python script tests under
`tests/scripts/` run as two ctest entries of their own.

```
tests/
├── backtest/             # Backtest coordinator, metrics, P&L
├── core/                 # Logger, config
├── data/                 # Database, bar loading, session classifier
├── execution/            # Execution engine
├── instruments/          # Instrument registry
├── live/                 # Live run components
├── optimization/         # The one pass, the generic optimiser
├── order/                # Order manager
├── portfolio/            # Portfolio manager, sizing capital
├── risk/                 # Risk modules, the overlay
├── scripts/              # Tests of the Python scripts
├── statistics/           # Statistics library
├── storage/              # Results managers
├── strategy/             # Strategies, the trend estimators
└── transaction_cost/     # Cost model, netting
```

---

## 📋 Troubleshooting

### Common Issues

#### Build Failures

**Missing dependencies:** run the install script for your system again.
```bash
# Ubuntu
sudo bash requirements/install_ubuntu.sh

# macOS
bash requirements/install_macos.sh
```

**libpqxx not found:**
```bash
# Check pkg-config
pkg-config --libs libpqxx

# If missing, add to PKG_CONFIG_PATH
export PKG_CONFIG_PATH="/opt/homebrew/lib/pkgconfig:$PKG_CONFIG_PATH"
```

#### Database Connection Errors

**Connection refused:**
- Check database host is accessible
- Verify the database port is reachable
- Confirm credentials in `config/defaults.json` (copy from `config_template/` and fill placeholders)

**Missing tables:**
- Ensure required schemas exist (`trading`, `backtest`, `metadata`, `futures_data`)
- Apply the migrations in `migrations/` in number order

#### Live Trading Issues

**No market data:**
- Check if date is a trading day
- Verify data exists in `futures_data.ohlcv_1d`
- Ensure symbols list is correct

**Email not sending:**
- Verify SMTP credentials
- Check Gmail App Password (not account password)
- Confirm recipient email addresses

### Debug Logging

Enable verbose logging:
```cpp
// In config or at runtime
LoggerConfig config;
config.min_level = LogLevel::DEBUG;  // or TRACE for maximum detail
```

### Getting Help

1. Check the `logs/` directory for error details
2. Review relevant documentation in `docs/`
3. Create an issue with:
   - Error message
   - Steps to reproduce
   - Configuration (sanitized)
   - Log snippets

---

## 👥 Development Guidelines

### Code Style

- Follow [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
- Use `clang-format` with the provided `.clang-format` configuration
- Run linting before committing

### Naming Conventions

| Item | Convention | Example |
|------|------------|---------|
| Classes | PascalCase | `PortfolioManager` |
| Functions | snake_case | `calculate_position()` |
| Variables | snake_case | `target_position` |
| Constants | UPPER_SNAKE | `MAX_LEVERAGE` |
| Files | snake_case | `portfolio_manager.cpp` |
| Headers | snake_case.hpp | `portfolio_manager.hpp` |

### Git Workflow

1. Create a feature branch from the branch the work targets
2. Make changes with meaningful commits
3. Run pre-commit checks
4. Create a pull request onto that branch
5. Address code review feedback
6. Merge after approval



## 📄 License

This project is licensed under the [GNU General Public License v3.0](https://www.gnu.org/licenses/gpl-3.0). The licence text is at that link; the repository holds no licence file.

---

## Acknowledgments

- Robert Carver for systematic trading methodology
- AlgoGators team for contributions
- Open source community for tooling

