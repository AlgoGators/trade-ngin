# Trade-Ngin Trading Strategy

A C++ trading strategy implementation with trend following and position management.

## Prerequisites

- CMake (version 3.10 or higher)
- C++ compiler with C++17 support
- Apache Arrow
- libpqxx (PostgreSQL C++ client library)
- PostgreSQL database
- Databento API key (for market data)

## Setup Instructions

1. Clone the repository:
```bash
git clone https://github.com/yourusername/trade-ngin.git
cd trade-ngin
```

2. Set up environment variables by creating a `.env` file:
```bash
# Databento API key
DATABENTO_API_KEY=your_api_key_here

# Database credentials
DB_HOST=your_db_host
DB_PORT=5432
DB_USER=your_db_user
DB_PASSWORD=your_db_password
DB_NAME=your_db_name
```

3. Create and enter the build directory:
```bash
mkdir -p build
cd build
```

4. Configure CMake:
```bash
cmake ..
```

5. Build the project:
```bash
cmake --build .
```

## Running the Strategy

1. To run the mock trading strategy:
```bash
./build/mock_trading
```

2. To run database tests and strategy backtests:
```bash
./build/test_db
```

## Project Structure

- `trade_ngin/system/`: Core strategy and signal generation code
- `trade_ngin/data/`: Data handling and database interface
- `test_trend_strategy.cpp`: Strategy testing and performance metrics
- `run_mock_trading.cpp`: Mock trading implementation
- `.env`: Environment configuration (API keys and database credentials)

## Configuration

The strategy parameters can be adjusted in `trade_ngin/data/run_mock_trading.cpp`:
- Moving average windows
- Volatility parameters
- Position sizing
- Risk management settings

## Dependencies Installation

### macOS (using Homebrew):
```bash
brew install cmake
brew install apache-arrow
brew install libpqxx
brew install postgresql
```

### Ubuntu/Debian:
```bash
sudo apt-get update
sudo apt-get install cmake
sudo apt-get install libarrow-dev
sudo apt-get install libpqxx-dev
sudo apt-get install postgresql postgresql-contrib
```

### Windows:
1. Install MSYS2 from https://www.msys2.org/
   - During installation, choose the default options
   - After installation, open "MSYS2 MINGW64" terminal

2. Update MSYS2 and install required packages:
```bash
pacman -Syu
pacman -S mingw-w64-x86_64-cmake
pacman -S mingw-w64-x86_64-gcc
pacman -S mingw-w64-x86_64-arrow
pacman -S mingw-w64-x86_64-brotli
pacman -S mingw-w64-x86_64-libpqxx
pacman -S mingw-w64-x86_64-postgresql
```

3. Add MSYS2 to your system PATH:
   - Open Windows Settings → System → About → Advanced system settings
   - Click "Environment Variables"
   - Under "System variables", find "Path" and click "Edit"
   - Add these paths (adjust if you installed MSYS2 in a different location):
     ```
     C:\msys64\mingw64\bin
     C:\msys64\usr\bin
     ```

4. Build the project using MSYS2 MINGW64 terminal:
```bash
cd /path/to/trade-ngin
mkdir -p build
cd build
cmake -G "MSYS Makefiles" ..
cmake --build .
```

Note: If you encounter any CMake configuration issues on Windows:
- Make sure you're using the MSYS2 MINGW64 terminal
- Ensure all MSYS2 packages are up to date
- Try removing the build directory and starting fresh:
  ```bash
  rm -rf build/
  mkdir build
  cd build
  cmake -G "MSYS Makefiles" ..
  ```

## Database Setup

1. Create a new PostgreSQL database:
```sql
CREATE DATABASE your_db_name;
```

2. The required tables will be created automatically when running the application for the first time.

## Getting Started with Development

1. Fork the repository
2. Create a new branch for your feature:
```bash
git checkout -b feature/your-feature-name
```
3. Make your changes and commit them:
```bash
git add .
git commit -m "Description of your changes"
```
4. Push to your fork:
```bash
git push origin feature/your-feature-name
```
5. Create a Pull Request
