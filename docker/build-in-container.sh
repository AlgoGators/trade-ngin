#!/bin/bash
set -e
# Sync repo source into the persistent build volume (timestamps preserved -> incremental)
rsync -a --delete \
  --exclude build/ --exclude .git/ --exclude logs/ \
  /src/ /work/

cd /work
# Missing-include fixes the repo's own Dockerfile applies for GCC; applied to the
# copy, never to the mounted repo.
grep -q '^#include <algorithm>' src/core/logger.cpp || sed -i '1i\#include <algorithm>' src/core/logger.cpp
grep -q '^#include <atomic>' include/trade_ngin/order/order_manager.hpp || sed -i '1i\#include <atomic>' include/trade_ngin/order/order_manager.hpp
grep -q '^#include <atomic>' src/order/order_manager.cpp || sed -i '1i\#include <atomic>' src/order/order_manager.cpp
grep -q '^#include <thread>' tests/data/test_postgres_database.cpp || sed -i '1i\#include <thread>' tests/data/test_postgres_database.cpp
grep -q '^#include <thread>' tests/order/test_order_manager.cpp || sed -i '1i\#include <thread>' tests/order/test_order_manager.cpp
grep -q '^#include <thread>' tests/execution/test_execution_engine.cpp || sed -i '1i\#include <thread>' tests/execution/test_execution_engine.cpp
grep -q '^#include <cmath>' tests/portfolio/mock_strategy.hpp || sed -i '1i\#include <cmath>' tests/portfolio/mock_strategy.hpp
grep -q '^#include <thread>' tests/portfolio/test_portfolio_manager.cpp || sed -i '1i\#include <thread>' tests/portfolio/test_portfolio_manager.cpp
grep -q '^#include <chrono>' tests/backtesting/test_engine.cpp || sed -i '1i\#include <chrono>' tests/backtesting/test_engine.cpp
grep -q '^#include <thread>' tests/backtesting/test_engine.cpp || sed -i '1i\#include <thread>' tests/backtesting/test_engine.cpp
sed -i 's/void BacktestEngineTest::patch_mock_db_to_return_test_data/void patch_mock_db_to_return_test_data/' tests/backtesting/test_engine.cpp

mkdir -p /build
cd /build
cmake /work -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON > /tmp/cmake.log 2>&1 || { tail -40 /tmp/cmake.log; exit 1; }
cmake --build . --config Release -j"$(nproc)" "$@"
