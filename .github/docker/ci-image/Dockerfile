FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Tier 1: bootstrap toolchain — minimal so adding CI/project libs later
# doesn't bust the slow source-build cache.
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

# Tier 2: project deps + CI-only tooling (valgrind, coverage, linters, python).
RUN apt-get update && apt-get install -y --no-install-recommends \
        libeigen3-dev \
        libnlopt-dev \
        libnlopt-cxx-dev \
        libcurl4-openssl-dev \
        valgrind \
        lcov \
        gcovr \
        clang-tidy \
        clang-format \
        cppcheck \
        python3 \
        python3-pip \
        unzip \
    && pip3 install --break-system-packages cpplint \
    && rm -rf /var/lib/apt/lists/*

# Docker CLI (for CI jobs that build the runtime image)
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl gnupg lsb-release \
    && mkdir -p /etc/apt/keyrings \
    && curl -fsSL https://download.docker.com/linux/ubuntu/gpg \
        | gpg --dearmor -o /etc/apt/keyrings/docker.gpg \
    && echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable" \
        > /etc/apt/sources.list.d/docker.list \
    && apt-get update && apt-get install -y --no-install-recommends docker-ce-cli \
    && rm -rf /var/lib/apt/lists/*
