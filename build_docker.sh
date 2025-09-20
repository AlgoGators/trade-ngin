#!/bin/bash
set -e

echo "Building TradingNet with fixed code..."

# Use the path inside the container
CONTAINER_PATH="/app"

# Run Docker container with all dependencies
docker run -it --rm \
  -v "$(pwd):${CONTAINER_PATH}" \
  -w "${CONTAINER_PATH}" \
  ubuntu:22.04 \
  bash -c "
    set -e

    # Install dependencies
    echo 'Installing build dependencies...'
    apt-get update
    apt-get install -y build-essential cmake git libpq-dev wget gnupg lsb-release pkg-config

    # Install Arrow
    echo 'Installing Arrow...'
    wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | apt-key add -
    wget -O /tmp/apache-arrow-apt-source.deb https://apache.jfrog.io/artifactory/arrow/ubuntu/apache-arrow-apt-source-latest-\$(lsb_release -cs).deb
    apt-get install -y /tmp/apache-arrow-apt-source.deb
    apt-get update
    apt-get install -y libarrow-dev libarrow-dataset-dev

    # Install nlohmann_json
    echo 'Installing nlohmann_json...'
    git clone --depth 1 https://github.com/nlohmann/json.git /tmp/json
    cd /tmp/json
    mkdir -p build && cd build
    cmake .. -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    make -j\$(nproc)
    make install

    # Install libpqxx
    echo 'Installing libpqxx...'
    git clone --depth 1 https://github.com/jtv/libpqxx.git /tmp/libpqxx
    cd /tmp/libpqxx
    mkdir -p build && cd build
    cmake .. -DSKIP_BUILD_TEST=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=ON
    make -j\$(nproc)
    make install
    ldconfig

    # Install Google Test
    echo 'Installing Google Test...'
    apt-get install -y libgtest-dev googletest
    cd /usr/src/googletest
    cmake .
    make -j\$(nproc)
    cp lib/*.a /usr/lib/

    # Create GTest CMake config files manually since they're missing
    echo 'Creating GTest CMake config files...'
    mkdir -p /usr/local/lib/cmake/GTest

    # Create GTestConfig.cmake
    cat > /usr/local/lib/cmake/GTest/GTestConfig.cmake << 'EOL'
if(NOT TARGET GTest::GTest)
    include(CMakeFindDependencyMacro)
    include(\${CMAKE_CURRENT_LIST_DIR}/GTestTargets.cmake)
endif()
EOL

    # Create GTestConfigVersion.cmake
    cat > /usr/local/lib/cmake/GTest/GTestConfigVersion.cmake << 'EOL'
set(PACKAGE_VERSION \"1.11.0\")
if(PACKAGE_VERSION VERSION_LESS PACKAGE_FIND_VERSION)
    set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
    set(PACKAGE_VERSION_COMPATIBLE TRUE)
    if(PACKAGE_FIND_VERSION STREQUAL PACKAGE_VERSION)
        set(PACKAGE_VERSION_EXACT TRUE)
    endif()
endif()
EOL

    # Create GTestTargets.cmake
    cat > /usr/local/lib/cmake/GTest/GTestTargets.cmake << 'EOL'
if(NOT TARGET GTest::GTest)
    add_library(GTest::GTest IMPORTED STATIC)
    set_target_properties(GTest::GTest PROPERTIES
        IMPORTED_LOCATION \"/usr/lib/libgtest.a\"
        INTERFACE_INCLUDE_DIRECTORIES \"/usr/include\"
    )

    add_library(GTest::Main IMPORTED STATIC)
    set_target_properties(GTest::Main PROPERTIES
        IMPORTED_LOCATION \"/usr/lib/libgtest_main.a\"
        INTERFACE_INCLUDE_DIRECTORIES \"/usr/include\"
    )
endif()
EOL

    # Update the source with fixes for the segmentation fault...
    echo 'Checking if fixes are already applied:'
    cd ${CONTAINER_PATH}
    grep -n 'symbol.find(\".v.\")' src/strategy/trend_following.cpp || echo 'Symbol normalization code not found - may need manual check'

    # Build our project
    echo 'Building project...'
    cd ${CONTAINER_PATH}
    rm -rf build
    mkdir -p build
    cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j\$(nproc) || make VERBOSE=1

    # Find executables
    echo 'Looking for built executables...'
    find ${CONTAINER_PATH}/build -type f -executable | grep -v CMakeFiles

    # Look specifically for the trend following strategy
    echo 'Checking for trend_following files:'
    find ${CONTAINER_PATH} -name '*trend_following*'

    # Run tests if they exist
    if [ -f ${CONTAINER_PATH}/build/bin/trade_ngin_tests ]; then
      echo 'Running tests...'
      ${CONTAINER_PATH}/build/bin/trade_ngin_tests
    elif [ -f ${CONTAINER_PATH}/build/tests/strategy/test_trend_following ]; then
      echo 'Running trend following test...'
      ${CONTAINER_PATH}/build/tests/strategy/test_trend_following
    else
      echo 'Test executable not found in expected location.'
      # Try to find the test
      find ${CONTAINER_PATH}/build -name '*test*' -type f -executable
    fi
  "

echo "Build completed!"
