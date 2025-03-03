# Use a base image with C++ build tools
FROM ubuntu:22.04 AS builder

# Avoid prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install basic tools and dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    wget \
    gnupg \
    lsb-release \
    libgtest-dev \
    pkg-config \
    libpq-dev \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Install Google Test and Google Mock (gmock) from source
RUN git clone https://github.com/google/googletest.git /tmp/googletest && \
    cd /tmp/googletest && \
    mkdir build && cd build && \
    cmake .. && \
    make -j$(nproc) && \
    make install

# Add Apache Arrow repository
RUN wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | apt-key add - && \
    wget -O - https://apache.jfrog.io/artifactory/arrow/ubuntu/apache-arrow-apt-source-latest-$(lsb_release -cs).deb -O /tmp/apache-arrow-apt-source.deb && \
    apt-get update && apt-get install -y /tmp/apache-arrow-apt-source.deb && \
    apt-get update

# Install Arrow and other dependencies
RUN apt-get install -y \
    libarrow-dev \
    libarrow-dataset-dev \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Install nlohmann_json from source
RUN cd /tmp && \
    git clone https://github.com/nlohmann/json.git && \
    cd json && \
    mkdir build && \
    cd build && \
    cmake .. -DCMAKE_POSITION_INDEPENDENT_CODE=ON && \
    make -j$(nproc) && \
    make install

# Install libpqxx from source with PIC
RUN cd /tmp && \
    git clone https://github.com/jtv/libpqxx.git && \
    cd libpqxx && \
    mkdir build && \
    cd build && \
    cmake .. -DSKIP_BUILD_TEST=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=ON && \
    make -j$(nproc) && \
    make install && \
    ldconfig

# Create a directory for the project
WORKDIR /app

# Copy the source code
COPY . .

# Fix the missing includes
RUN sed -i '1i\#include <algorithm>' src/core/logger.cpp && \
    sed -i '1i\#include <atomic>' include/trade_ngin/order/order_manager.hpp && \
    sed -i '1i\#include <atomic>' src/order/order_manager.cpp && \
    sed -i '1i\#include <thread>' tests/data/test_postgres_database.cpp && \
    sed -i '1i\#include <thread>' tests/order/test_order_manager.cpp && \
    sed -i '1i\#include <thread>' tests/execution/test_execution_engine.cpp && \
    sed -i '1i\#include <cmath>' tests/portfolio/mock_strategy.hpp && \
    sed -i '1i\#include <thread>' tests/portfolio/test_portfolio_manager.cpp

# Create build directory
RUN mkdir -p build

# Configure and build the project
WORKDIR /app/build
RUN cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    && cmake --build . --config Release

# Debug: List ALL executables built in the project
RUN echo "ALL EXECUTABLES:" && \
    find /app -type f -executable -not -path "*/\.*" | sort

# Create a minimal runtime image
FROM ubuntu:22.04

# Avoid prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install basic tools and dependencies
RUN apt-get update && apt-get install -y \
    wget \
    gnupg \
    lsb-release \
    libpq5 \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Add Apache Arrow repository
RUN wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | apt-key add - && \
    wget -O - https://apache.jfrog.io/artifactory/arrow/ubuntu/apache-arrow-apt-source-latest-$(lsb_release -cs).deb -O /tmp/apache-arrow-apt-source.deb && \
    apt-get update && apt-get install -y /tmp/apache-arrow-apt-source.deb && \
    apt-get update

# Install runtime dependencies
RUN apt-get install -y \
    libarrow-dev \
    libarrow-dataset-dev \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Copy built libraries from builder
COPY --from=builder /usr/local/lib/ /usr/local/lib/
COPY --from=builder /usr/local/include/ /usr/local/include/

# Update the library cache
RUN ldconfig

# Copy the entire app directory (including build artifacts)
COPY --from=builder /app /app/

# Create a startup script
RUN echo '#!/bin/bash\n\
echo "Searching for trade_ngin executable..."\n\
EXES=$(find /app -type f -executable -name "*trade_ngin*" 2>/dev/null)\n\
\n\
if [ -n "$EXES" ]; then\n\
  # Use the first matching executable\n\
  EXECUTABLE=$(echo "$EXES" | head -n 1)\n\
  echo "Found executable: $EXECUTABLE"\n\
  exec "$EXECUTABLE" "$@"\n\
else\n\
  echo "Error: No trade_ngin executable found. Available executables:"\n\
  find /app -type f -executable | sort\n\
  exit 1\n\
fi' > /app/startup.sh && \
    chmod +x /app/startup.sh

# Set the library path
ENV LD_LIBRARY_PATH=/usr/local/lib:/app/build:/app/lib

# Set the working directory
WORKDIR /app

# Set the entry point to our startup script
ENTRYPOINT ["/app/startup.sh"]