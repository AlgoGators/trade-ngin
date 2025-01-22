# Set build type to Release
set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)

cmake_minimum_required(VERSION 3.10)
project(trade_ngin)

# Enable testing
enable_testing()

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find required packages
find_package(Arrow REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PQXX REQUIRED libpqxx)

# Find nlohmann_json
find_package(nlohmann_json 3.2.0 REQUIRED)

# Find GTest
find_package(GTest REQUIRED)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/trade_ngin/data
    ${Arrow_INCLUDE_DIRS}
    ${PQXX_INCLUDE_DIRS}
    ${GTEST_INCLUDE_DIRS}
)

# Link directories
link_directories(${PQXX_LIBRARY_DIRS})

# Add executables
add_executable(test_db trade_ngin/tests/test_database_client.cpp
                             trade_ngin/data/data_interface.cpp
                             trade_ngin/data/database_client.cpp
                             trade_ngin/data/ohlcv_data_handler.cpp)

add_executable(mock_trading trade_ngin/data/data_interface.cpp
                           trade_ngin/data/database_client.cpp
                           trade_ngin/data/ohlcv_data_handler.cpp
                           trade_ngin/system/test_trend_strategy.cpp
                           trade_ngin/system/run_mock_trading.cpp)

add_executable(test_vol_regime trade_ngin/data/data_interface.cpp
                              trade_ngin/data/database_client.cpp
                              trade_ngin/data/ohlcv_data_handler.cpp
                              trade_ngin/system/test_vol_regime.cpp)

# Link libraries
target_link_libraries(test_db PRIVATE Arrow::arrow_shared ${PQXX_LIBRARIES} nlohmann_json::nlohmann_json)
target_link_libraries(mock_trading PRIVATE Arrow::arrow_shared ${PQXX_LIBRARIES} nlohmann_json::nlohmann_json)
target_link_libraries(test_vol_regime PRIVATE Arrow::arrow_shared ${PQXX_LIBRARIES} nlohmann_json::nlohmann_json)

# Set compile options
add_compile_options(-Wall -Wextra -O3)

# Set output directories to match existing build structure
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Add tests subdirectory
add_subdirectory(tests)

# Optional: Windows-specific configurations (commented out but available if needed)
# if(WIN32)
#     set(CMAKE_TOOLCHAIN_FILE "C:/Users/rmathieu/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
#     target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32)
#     include_directories(
#         "C:/Program Files/PostgreSQL/16/include"
#         "C:/Users/rmathieu/vcpkg/installed/x64-windows/include"
#     )
#     link_directories(
#         "C:/Program Files/PostgreSQL/16/lib"
#         "C:/Users/rmathieu/vcpkg/installed/x64-windows/lib"
#     )
# endif()
