FROM ubuntu:22.04

# Prevent interactive prompts during package installation
ARG DEBIAN_FRONTEND=noninteractive

# Install basic dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    ninja-build \
    pkg-config \
    curl \
    wget \
    python3 \
    python3-pip \
    libpq-dev \
    graphviz \
    doxygen \
    clang-format \
    clang-tidy \
    ccache \
    gnupg \
    lsb-release \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Install Arrow 
RUN apt-get update && apt-get install -y \
    libboost-all-dev \
    libbrotli-dev \
    liblz4-dev \
    libsnappy-dev \
    libzstd-dev \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# Install Apache Arrow (updated repository URL and installation method)
RUN cd /tmp && \
    wget https://apache.jfrog.io/artifactory/arrow/ubuntu/apache-arrow-apt-source-latest-jammy.deb && \
    apt-get update && apt-get install -y ./apache-arrow-apt-source-latest-jammy.deb && \
    apt-get update && apt-get install -y libarrow-dev libarrow-dataset-dev && \
    rm -rf /tmp/apache-arrow-apt-source-latest-jammy.deb && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

# Install nlohmann JSON headers
RUN mkdir -p /opt/json && \
    cd /opt/json && \
    wget -O json.hpp https://github.com/nlohmann/json/releases/download/v3.11.2/json.hpp && \
    mkdir -p /usr/include/nlohmann && \
    cp json.hpp /usr/include/nlohmann/ && \
    rm -rf /opt/json

# Install libpqxx (PostgreSQL C++ client library)
RUN cd /tmp && \
    git clone --depth 1 --branch 7.7.5 https://github.com/jtv/libpqxx.git && \
    cd libpqxx && \
    mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX=/usr \
          -DSKIP_BUILD_TEST=ON \
          -DBUILD_DOC=OFF \
          .. && \
    make -j$(nproc) && \
    make install && \
    cd / && rm -rf /tmp/libpqxx

# Install GoogleTest
RUN cd /tmp && \
    git clone --depth 1 --branch v1.13.0 https://github.com/google/googletest.git && \
    cd googletest && \
    mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX=/usr \
          .. && \
    make -j$(nproc) && \
    make install && \
    cd / && rm -rf /tmp/googletest

# Install Python packages
RUN pip3 install \
    matplotlib \
    pandas \
    numpy \
    seaborn \
    pytest \
    pytest-cov \
    pylint \
    black \
    psycopg2-binary

# Create a non-root user
RUN useradd -m -s /bin/bash developer

# Set up the workspace directory
WORKDIR /workspace
RUN chown -R developer:developer /workspace

# Switch to the non-root user
USER developer

# Set the default command
CMD ["/bin/bash"]
