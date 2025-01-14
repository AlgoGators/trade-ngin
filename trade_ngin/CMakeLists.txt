set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)

cmake_minimum_required(VERSION 3.10)
project(trade_ngin)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_PREFIX_PATH "C:/Users/kbott/vcpkg/installed/x64-windows")

# Set vcpkg toolchain file
set(CMAKE_TOOLCHAIN_FILE "C:/Users/kbott/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")

# Add include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/data                          # Project headers
    "C:/Users/kbott/libpqxx-master/include"           # Path to libpqxx headers
    "C:/Users/kbott/libpqxx-master/build/include"     # Path to libpqxx headers
    "C:/Program Files/PostgreSQL/16/include"          # Path to PostgreSQL headers
    "C:/Users/kbott/vcpkg/installed/x64-windows/include" # Path to vcpkg headers
)

# Add library directories
link_directories(
    "C:/Users/kbott/libpqxx-master/build/src/Release" # Path to libpqxx library
    "C:/Program Files/PostgreSQL/16/lib"              # Path to PostgreSQL library
    "C:/Users/kbott/vcpkg/installed/x64-windows/lib"  # Path to vcpkg libraries
)

# Find Apache Arrow via vcpkg
set(arrow_DIR "C:/Users/kbott/vcpkg/installed/x64-windows/share/arrow")
find_package(arrow REQUIRED)
set(libcurl_DIR "C:/Users/kbott/vcpkg/installed/x64-windows/share/curl")
find_package(CURL REQUIRED)
set(nlohmann-json_DIR "C:/Users/kbott/vcpkg/installed/x64-windows/share/nlohmann-json")
find_package(nlohmann_json REQUIRED)
find_package(ZLIB REQUIRED)

# Add executable
add_executable(test_db
    trade_ngin/data/test_trend_strategy.cpp
    trade_ngin/data/database_interface.cpp
    trade_ngin/data/portfolio.cpp
)

# Link libraries
target_link_libraries(test_db
    arrow
    pq
    pqxx
)
