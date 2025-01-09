I'll create a comprehensive README.md that thoroughly documents the trading engine:

```markdown:README.md
# Trade-Ngin Trading System

<div align="center">
    <img src="static/trade-ngin.png" alt="Trade-Ngin Logo" width="200"/>
</div>

## Overview

Trade-Ngin is a high-performance, modular trading system designed for algorithmic trading strategies. It provides a robust framework for developing, testing, and executing trading strategies with real-time market data integration.

## System Architecture

### Core Components

1. **Data Management**
   - `DataClient`: Abstract interface for market data providers
   - `DatabentoClient`: Implementation for Databento market data
   - `DatabaseInterface`: TimescaleDB integration for data storage
   - `DataFrame`: Custom data structure for efficient market data handling

2. **Trading Components**
   - `TradingSystem`: Main system orchestrator
   - `Portfolio`: Position and risk management
   - `Strategy`: Base class for trading strategies
   - `Instrument`: Market instrument representation
   - `ExecutionEngine`: Order execution and management

3. **Risk Management**
   - `RiskEngine`: Risk metrics calculation
   - `Position Limits`: Per-instrument position constraints
   - `Risk Limits`: Portfolio-wide risk constraints

## Prerequisites

### System Requirements
- Linux/Unix-based OS (Ubuntu 20.04+ recommended)
- C++17 compiler (GCC 9+ or Clang 10+)
- CMake 3.15+
- PostgreSQL 13+ with TimescaleDB extension
- 16GB RAM minimum (32GB recommended)
- SSD storage for database

### Required Libraries
```bash
# Core development tools
sudo apt-get update && sudo apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config

# Database dependencies
sudo apt-get install -y \
    postgresql-13 \
    postgresql-server-dev-13 \
    libpqxx-dev

# Data processing libraries
sudo apt-get install -y \
    libarrow-dev \
    libboost-all-dev

# Optional: Development tools
sudo apt-get install -y \
    clang-format \
    clang-tidy \
    valgrind
```

## Installation

1. **Clone the Repository**
```bash
git clone https://github.com/yourusername/trade-ngin.git
cd trade-ngin
```

2. **Configure Database**
```bash
# Install TimescaleDB
sudo add-apt-repository ppa:timescale/timescaledb-ppa
sudo apt-get update
sudo apt-get install timescaledb-postgresql-13

# Configure PostgreSQL
sudo -u postgres psql -c "CREATE DATABASE algo_data;"
sudo -u postgres psql -d algo_data -c "CREATE EXTENSION IF NOT EXISTS timescaledb;"

# Create tables (example)
sudo -u postgres psql -d algo_data -f scripts/create_tables.sql
```

3. **Environment Setup**
```bash
# Copy example environment file
cp .env.example .env

# Edit .env with your credentials
nano .env
```

4. **Build the System**
```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Configuration

### Environment Variables
```env
DB_HOST=localhost
DB_PORT=5432
DB_USER=your_username
DB_PASSWORD=your_password
DB_NAME=algo_data
DATABENTO_API_KEY=your_api_key
```

### Strategy Configuration
```cpp
// Example strategy configuration
Strategy::StrategyConfig config{
    .capital_allocation = 1000000.0,
    .max_leverage = 2.0,
    .position_limit = 100,
    .risk_limit = 0.02
};
```

## Usage

### Running the Trading System
```bash
# Basic usage
./trade_ngin

# With specific configuration
./trade_ngin --config config.json

# Backtesting mode
./trade_ngin --backtest --start-date 2023-01-01 --end-date 2023-12-31
```

### Example Implementation
```cpp
#include "trading_system.hpp"

int main() {
    // Initialize system
    TradingSystem system(1000000.0, "your_databento_api_key");
    
    // Initialize instruments and strategies
    system.initialize();
    
    // Add trading strategy
    auto trend_strategy = std::make_shared<TrendFollowingStrategy>(
        500000.0, 50.0, 0.2
    );
    system.getPortfolio().addStrategy(trend_strategy, 0.7);
    
    // Run the system
    system.run();
    
    return 0;
}
```

## Development

### Adding New Strategies
1. Create a new strategy class inheriting from `Strategy`
2. Implement required methods:
   - `positions()`
   - `update()`
   - Risk management callbacks

```cpp
class MyStrategy : public Strategy {
public:
    MyStrategy(StrategyConfig config) : Strategy("MyStrategy", config) {}
    
    DataFrame positions() override {
        // Implement position logic
    }
    
    void update(const DataFrame& market_data) override {
        // Implement update logic
    }
};
```

### Database Schema
```sql
-- Example table structure
CREATE TABLE futures_data.ohlcv_1d (
    time TIMESTAMP NOT NULL,
    symbol VARCHAR(10) NOT NULL,
    open DOUBLE PRECISION,
    high DOUBLE PRECISION,
    low DOUBLE PRECISION,
    close DOUBLE PRECISION,
    volume DOUBLE PRECISION,
    PRIMARY KEY (time, symbol)
);
```

## Monitoring and Logging

### Performance Metrics
- PnL tracking
- Risk metrics
- Execution statistics
- System health metrics

### Log Files
```bash
/var/log/trade_ngin/
├── system.log    # System-level logs
├── trades.log    # Trade execution logs
├── errors.log    # Error logs
└── metrics.log   # Performance metrics
```

## Troubleshooting

### Common Issues
1. **Database Connection**
   ```bash
   # Check database status
   sudo systemctl status postgresql
   
   # View logs
   sudo tail -f /var/log/postgresql/postgresql-13-main.log
   ```

2. **Market Data**
   ```bash
   # Test Databento connection
   ./test_databento_connection
   
   # Verify data in database
   psql -d algo_data -c "SELECT COUNT(*) FROM futures_data.ohlcv_1d;"
   ```

## Performance Optimization

### Recommended Settings
1. **Database**
   ```bash
   # Edit postgresql.conf
   shared_buffers = 4GB
   work_mem = 64MB
   maintenance_work_mem = 512MB
   ```

2. **System**
   ```bash
   # Add to /etc/sysctl.conf
   vm.swappiness = 10
   ```

## Contributing

Please read [CONTRIBUTING.md](CONTRIBUTING.md) for details on our code of conduct and the process for submitting pull requests.

## License

This project is licensed under the MIT License - see the [LICENSE.md](LICENSE.md) file for details.

## Acknowledgments

- Databento for market data access
- TimescaleDB for time-series data management
- Arrow for efficient data processing

