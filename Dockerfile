FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        ca-certificates \
        wget \
        gnupg \
        lsb-release \
        pkg-config \
        libpq-dev \
    && rm -rf /var/lib/apt/lists/*

# Google Test / Google Mock from source
RUN git clone --depth 1 https://github.com/google/googletest.git /tmp/googletest && \
    cmake -S /tmp/googletest -B /tmp/googletest/build && \
    cmake --build /tmp/googletest/build -j"$(nproc)" --target install && \
    rm -rf /tmp/googletest

# Apache Arrow apt repository + dev headers
RUN wget -qO /tmp/apache-arrow-apt-source.deb \
        "https://apache.jfrog.io/artifactory/arrow/ubuntu/apache-arrow-apt-source-latest-$(lsb_release -cs).deb" && \
    apt-get install -y --no-install-recommends /tmp/apache-arrow-apt-source.deb && \
    rm /tmp/apache-arrow-apt-source.deb && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        libarrow-dev \
        libarrow-dataset-dev && \
    rm -rf /var/lib/apt/lists/*

# nlohmann_json — disable its test suite
RUN git clone --depth 1 https://github.com/nlohmann/json.git /tmp/json && \
    cmake -S /tmp/json -B /tmp/json/build \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DJSON_BuildTests=OFF && \
    cmake --build /tmp/json/build -j"$(nproc)" --target install && \
    rm -rf /tmp/json

# libpqxx 7.10.4 — pinned.
RUN git clone --branch 7.10.4 --depth 1 https://github.com/jtv/libpqxx.git /tmp/libpqxx && \
    cmake -S /tmp/libpqxx -B /tmp/libpqxx/build \
        -DSKIP_BUILD_TEST=ON \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_SHARED_LIBS=ON && \
    cmake --build /tmp/libpqxx/build -j"$(nproc)" --target install && \
    rm -rf /tmp/libpqxx && \
    ldconfig

RUN apt-get update && apt-get install -y --no-install-recommends \
        libeigen3-dev \
        libnlopt-dev \
        libnlopt-cxx-dev \
        libcurl4-openssl-dev \
        cron \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

# ADR-005 5.2/6.3: TRADE_NGIN_GIT_SHA identifies which build produced each
# day's run_inputs row -- load-bearing for benchmark_replay's --engine
# frozen (resolves this SHA to a real image) and for post-hoc "which code
# made this trade" forensics. .dockerignore excludes .git (smaller, faster
# build context), so CMakeLists.txt's `git rev-parse` always fails inside
# this build and silently falls back to "unknown" -- every image built from
# this Dockerfile has done that so far. Pass the real SHA in from outside
# instead, where it's already known: CI computes it from its own checkout
# (which does have .git) and passes --build-arg; a local `docker build` can
# do the same with `--build-arg TRADE_NGIN_GIT_SHA=$(git rev-parse --short HEAD)`.
# Defaults to "unknown" (CMakeLists.txt's existing fallback) if omitted, so
# an unmodified `docker build .` behaves exactly as before.
ARG TRADE_NGIN_GIT_SHA=unknown
ENV TRADE_NGIN_GIT_SHA=${TRADE_NGIN_GIT_SHA}

# Patch missing includes / test signatures
RUN sed -i '1i\#include <algorithm>' src/core/logger.cpp && \
    sed -i '1i\#include <atomic>' include/trade_ngin/order/order_manager.hpp && \
    sed -i '1i\#include <atomic>' src/order/order_manager.cpp && \
    sed -i '1i\#include <thread>' tests/data/test_postgres_database.cpp && \
    sed -i '1i\#include <thread>' tests/order/test_order_manager.cpp && \
    sed -i '1i\#include <thread>' tests/execution/test_execution_engine.cpp && \
    sed -i '1i\#include <cmath>' tests/portfolio/mock_strategy.hpp && \
    sed -i '1i\#include <thread>' tests/portfolio/test_portfolio_manager.cpp && \
    sed -i '1i\#include <chrono>' tests/backtesting/test_engine.cpp && \
    sed -i '1i\#include <thread>' tests/backtesting/test_engine.cpp && \
    sed -i 's/void BacktestEngineTest::patch_mock_db_to_return_test_data/void patch_mock_db_to_return_test_data/' tests/backtesting/test_engine.cpp

RUN cmake -S /app -B /app/build \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DNLopt_DIR=/usr/lib/x86_64-linux-gnu/cmake/nlopt && \
    cmake --build /app/build --config Release -j"$(nproc)"

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive \
    TZ=America/New_York \
    LD_LIBRARY_PATH=/usr/local/lib:/app/build:/app/lib

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates wget gnupg lsb-release \
    && wget -qO /tmp/apache-arrow-apt-source.deb \
        "https://apache.jfrog.io/artifactory/arrow/ubuntu/apache-arrow-apt-source-latest-$(lsb_release -cs).deb" \
    && apt-get install -y --no-install-recommends /tmp/apache-arrow-apt-source.deb \
    && rm /tmp/apache-arrow-apt-source.deb \
    && apt-get update && apt-get install -y --no-install-recommends \
        libarrow-dev \
        libarrow-dataset-dev \
        libpq5 \
        libnlopt0 \
        libcurl4t64 \
        cron \
        gnuplot-nox \
        procps \
        tzdata \
    && apt-get purge -y --auto-remove gnupg lsb-release \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /usr/local/lib/ /usr/local/lib/
RUN ldconfig

COPY --from=builder /app /app

WORKDIR /app

COPY live_portfolio.cron /etc/cron.d/live_portfolio
RUN chmod 0644 /etc/cron.d/live_portfolio && \
    crontab /etc/cron.d/live_portfolio && \
    chmod 0755 /app/scripts/run_live_portfolio.sh /app/scripts/docker-entrypoint.sh

# The container's only job is running cron. If the cron daemon dies, the container
# can sit there looking alive while nothing is scheduled -- externally
# indistinguishable from the three-month silence this change addresses.
# Pair with `restart: unless-stopped` in the compose file on the host.
HEALTHCHECK --interval=5m --timeout=10s --start-period=30s --retries=3 \
    CMD pgrep -x cron > /dev/null || exit 1

# The entrypoint snapshots the container environment for cron (cron does not
# inherit it) and then execs cron in the foreground.
ENTRYPOINT ["/app/scripts/docker-entrypoint.sh"]
