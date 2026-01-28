FROM ubuntu:latest

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
    valgrind \
    lcov \
    gcovr \
    clang-tidy \
    clang-format \
    python3 \
    python3-pip \
    cppcheck \
    unzip \
    libeigen3-dev \
    && pip3 install --break-system-packages cpplint \
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
    wget -O /tmp/apache-arrow-apt-source.deb https://apache.jfrog.io/artifactory/arrow/ubuntu/apache-arrow-apt-source-latest-$(lsb_release -cs).deb && \
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

# Install Docker CLI inside the container
RUN apt-get update && apt-get install -y \
ca-certificates \
curl \
gnupg \
lsb-release && \
mkdir -p /etc/apt/keyrings && \
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | gpg --dearmor -o /etc/apt/keyrings/docker.gpg && \
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable" \
| tee /etc/apt/sources.list.d/docker.list > /dev/null && \
apt-get update && apt-get install -y docker-ce-cli && \
rm -rf /var/lib/apt/lists/*
