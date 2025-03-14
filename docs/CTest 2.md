Here's a comprehensive guide for setting up and running the trade-ngin tests:

# Trade-Ngin Setup Guide

## Prerequisites

### 1. Operating System Requirements
- Linux, macOS, or Windows (with WSL recommended for Windows users)
- Terminal/Command Line access

### 2. Required Tools
```bash
# macOS (using Homebrew)
brew install \
    cmake \
    postgresql \
    libpqxx \
    googletest

# Ubuntu/Debian
sudo apt update
sudo apt install -y \
    build-essential \
    cmake \
    postgresql-client \
    libpqxx-dev \
    libgtest-dev
```

### 3. Compiler Requirements
- GCC 7+ or Clang 6+
- C++11 or later support

## Installation Steps

1. **Clone the Repository**
```bash
git clone https://github.com/your-username/trade-ngin.git
cd trade-ngin
```

2. **Database Access**
- Ensure you can connect to the PostgreSQL database:
```bash
# Test database connection (should prompt for password: algogators)
psql -h 3.140.200.228 -p 5432 -U postgres -d algo_data
```
- If connection fails, check:
  - Your network can reach AWS instance (3.140.200.228)
  - No firewall blocking port 5432
  - VPN requirements if any

3. **Build the Project**
```bash
# Create and enter build directory
mkdir build
cd build

# Configure with CMake
cmake ..

# Build the project
make -j$(nproc)  # Use multiple cores for faster build
```

4. **Run Tests**
```bash
# Run all tests with detailed output
ctest --output-on-failure
```

## Troubleshooting

### Common Issues

1. **CMake Configuration Errors**
```bash
# If libpqxx not found
# macOS:
brew install libpqxx

# Ubuntu:
sudo apt install libpqxx-dev
```

2. **Build Errors**
```bash
# If Google Test not found
# macOS:
brew install googletest

# Ubuntu:
sudo apt install libgtest-dev
cd /usr/src/googletest
sudo cmake .
sudo make
sudo cp lib/*.a /usr/lib
```

3. **Database Connection Issues**
```bash
# Test database connection components
nc -zv 3.140.200.228 5432  # Check if port is reachable
```

4. **Library Path Issues**
```bash
# macOS
export DYLD_LIBRARY_PATH=/usr/local/lib:$DYLD_LIBRARY_PATH

# Linux
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH
```

## Version Requirements

- CMake >= 3.10
- libpqxx >= 7.0
- Google Test >= 1.10
- PostgreSQL Client >= 12.0

## Directory Structure
```
trade-ngin/
├── build/           # Build directory (created during setup)
├── trade_ngin/      # Source code
│   ├── data/       # Data handling
│   └── system/     # Trading system
└── tests/          # Test files
```

## Verification

After setup, verify your installation:

1. **Check Dependencies**
```bash
# Check versions
cmake --version
g++ --version
pkg-config --modversion libpqxx
```

2. **Verify Build**
```bash
# In build directory
make clean
make
```

3. **Run Individual Tests**
```bash
# Run specific test groups
./tests/test_strategy
./tests/test_portfolio
./tests/test_integration
```

# Complete CTest Tutorial for Trade-Ngin

## Part 1: Setting Up Your CMake File

First, create or edit your `CMakeLists.txt` in the root directory:

```cmake
# CMakeLists.txt
cmake_minimum_required(VERSION 3.10)
project(trade_ngin)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable testing
enable_testing()

# Find required packages
find_package(GTest REQUIRED)
find_package(libpqxx REQUIRED)

# Add include directories
include_directories(${GTEST_INCLUDE_DIRS})
include_directories(${LIBPQXX_INCLUDE_DIRS})

# Add all source files
add_library(trade_ngin_lib
    trade_ngin/system/portfolio.cpp
    trade_ngin/system/test_trend_strategy.cpp
    # Add other source files here
)

# Create test executables
add_executable(test_strategy tests/test_strategy.cpp)
add_executable(test_portfolio tests/test_portfolio.cpp)
add_executable(test_integration tests/test_integration.cpp)

# Link libraries to tests
target_link_libraries(test_strategy
    trade_ngin_lib
    GTest::GTest
    GTest::Main
    libpqxx
)

target_link_libraries(test_portfolio
    trade_ngin_lib
    GTest::GTest
    GTest::Main
    libpqxx
)

target_link_libraries(test_integration
    trade_ngin_lib
    GTest::GTest
    GTest::Main
    libpqxx
)

# Add tests to CTest
add_test(NAME StrategyTest COMMAND test_strategy)
add_test(NAME PortfolioTest COMMAND test_portfolio)
add_test(NAME IntegrationTest COMMAND test_integration)
```

## Part 2: Step-by-Step Build Guide

1. **Create Build Directory**
```bash
# Open terminal
# Go to your project directory
cd /path/to/trade-ngin

# Create and enter build directory
mkdir build
cd build
```

2. **Run CMake**
```bash
# Configure project
cmake ..

# If you see any errors, make sure you have all dependencies:

# On macOS:
brew install cmake googletest libpqxx

# On Ubuntu:
sudo apt-get update
sudo apt-get install cmake libgtest-dev libpqxx-dev
```

3. **Build Project**
```bash
# In the build directory
make
```

## Part 3: Running Tests with CTest

### Basic Test Commands

1. **Run All Tests**
```bash
# In the build directory
ctest
```

2. **Run Tests with Detailed Output**
```bash
ctest --output-on-failure
```

3. **Run Specific Tests**
```bash
# Run just the strategy test
ctest -R Strategy

# Run just the portfolio test
ctest -R Portfolio

# Run just the integration test
ctest -R Integration
```

### Advanced CTest Commands

1. **Run Tests in Parallel**
```bash
ctest -j4  # Run 4 tests at once
```

2. **Run Tests with Different Output Levels**
```bash
# Verbose output
ctest -V

# Extra verbose output
ctest -VV
```

3. **Run Tests Multiple Times**
```bash
# Run tests 3 times
ctest --repeat-until-fail 3
```

## Part 4: Troubleshooting

### Common Error 1: Tests Not Found
```bash
# If you see: "No tests were found!!!"
# Solution:
cd build
rm -rf *
cmake ..
make
ctest
```

### Common Error 2: Library Not Found
```bash
# If you see: "error while loading shared libraries"
# Solution:

# On macOS:
export DYLD_LIBRARY_PATH=/usr/local/lib:$DYLD_LIBRARY_PATH

# On Linux:
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH
```

### Common Error 3: Database Connection
```bash
# If tests fail due to database connection:
# Test database connection:
psql -h 3.140.200.228 -p 5432 -U postgres -d algo_data
```

## Part 5: Test Output Understanding

When you run `ctest --output-on-failure`, you'll see something like:

```bash
Test project /path/to/trade-ngin/build
    Start 1: StrategyTest
1/3 Test #1: StrategyTest .....................   Passed    0.01 sec
    Start 2: PortfolioTest
2/3 Test #2: PortfolioTest ...................   Passed    0.01 sec
    Start 3: IntegrationTest
3/3 Test #3: IntegrationTest .................   Failed    0.02 sec
```

### Understanding Test Results:
- `Passed`: Test completed successfully
- `Failed`: Test failed, check output for details
- Numbers like `0.01 sec` show how long each test took

## Part 6: Daily Testing Workflow

1. **Morning Test Check**
```bash
# Update and test
cd trade-ngin
git pull
cd build
make clean
make
ctest --output-on-failure
```

2. **After Making Changes**
```bash
# In build directory
make
ctest --output-on-failure
```

3. **Full Reset If Things Go Wrong**
```bash
# In project root
rm -rf build
mkdir build
cd build
cmake ..
make
ctest --output-on-failure
```

## Part 7: Test Result Files

CTest creates several useful files in the build directory:

```bash
# View test log
cat Testing/Temporary/LastTest.log

# View test failures
cat Testing/Temporary/LastTestsFailed.log
```


