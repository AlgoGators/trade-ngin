cmake_minimum_required(VERSION 3.14)
project(trade_ngin VERSION 1.0.0 LANGUAGES CXX)

# Set build type if not specified
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Add compile options
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Werror
    -O3
)

# Find required packages
find_package(Arrow REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PQXX REQUIRED libpqxx)
find_package(nlohmann_json 3.2.0 REQUIRED)
find_package(GTest REQUIRED)
find_package(Threads REQUIRED)

# Add include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${Arrow_INCLUDE_DIRS}
    ${PQXX_INCLUDE_DIRS}
)

# Link directories
link_directories(${PQXX_LIBRARY_DIRS})

# Add library target
add_library(trade_ngin SHARED
    src/core/types.cpp
    src/data/db_interface.cpp
    src/instruments/base.cpp
    src/instruments/futures.cpp
    src/strategy/base.cpp
    src/portfolio/manager.cpp
    src/risk/engine.cpp
    src/optimization/dynamic.cpp
    src/execution/gateway.cpp
)

# Link libraries
target_link_libraries(trade_ngin
    PRIVATE
    Arrow::arrow_shared
    ${PQXX_LIBRARIES}
    nlohmann_json::nlohmann_json
    Threads::Threads
)

# Install rules
install(TARGETS trade_ngin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/trade_ngin
    DESTINATION include
)

# Testing
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    enable_testing()
    add_subdirectory(tests)
endif()

# Examples
add_subdirectory(examples)