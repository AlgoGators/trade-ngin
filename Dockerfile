FROM ubuntu:22.04

# Install dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    gdb \
    valgrind \
    libgtest-dev \
    git \
    pkg-config \
    liblz4-dev \
    libbrotli-dev \
    nlohmann-json3-dev \
    dos2unix \
    wget \
    gnupg \
    libpqxx-dev \
    && rm -rf /var/lib/apt/lists/*

# Install newer CMake
RUN wget https://github.com/Kitware/CMake/releases/download/v3.27.7/cmake-3.27.7-linux-x86_64.sh \
    -q -O /tmp/cmake-install.sh \
    && chmod u+x /tmp/cmake-install.sh \
    && mkdir /opt/cmake-3.27.7 \
    && /tmp/cmake-install.sh --skip-license --prefix=/opt/cmake-3.27.7 \
    && rm /tmp/cmake-install.sh \
    && ln -s /opt/cmake-3.27.7/bin/* /usr/local/bin

# Install Apache Arrow
RUN wget https://apache.jfrog.io/artifactory/arrow/ubuntu/apache-arrow-apt-source-latest-jammy.deb \
    && apt-get update \
    && apt install -y ./apache-arrow-apt-source-latest-jammy.deb \
    && apt-get update \
    && apt-get install -y libarrow-dev \
    && rm -rf /var/lib/apt/lists/* \
    && rm apache-arrow-apt-source-latest-jammy.deb

# Create non-root user
RUN useradd -m -s /bin/bash runner

# Set up working directory
WORKDIR /app

# Copy source code and test files
COPY trade-ngin /app/trade-ngin/
COPY tests /app/tests/
COPY valgrind.supp /app/
COPY tsan.supp /app/
COPY docker-entrypoint.sh /app/

# Set proper ownership and permissions
RUN chown -R runner:runner /app && \
    chmod +x /app/docker-entrypoint.sh && \
    dos2unix /app/docker-entrypoint.sh

# Ensure /app/build/bin exists and is owned by runner
RUN mkdir -p /app/build/bin && chown -R runner:runner /app/build

# Copy test scripts (if any)
COPY test_memory_thread.sh /app/test_memory_thread.sh
RUN chmod +x /app/test_memory_thread.sh

# Set default working directory
WORKDIR /app

# Switch to non-root user
USER runner

# Build all test binaries
RUN mkdir -p /app/build && cd /app/build \
    && cmake /app/trade-ngin \
    && make -j$(nproc) \
    && find . -type f -perm +111 -exec cp --parents {} /app/build/bin/ \;

# No CMD or ENTRYPOINT here; use docker-compose.yml to specify command 