set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)

cmake_minimum_required(VERSION 3.10)
project(TradeEngine)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set vcpkg toolchain file
set(CMAKE_TOOLCHAIN_FILE "C:/Users/kbott/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")

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

# Find libpqxx and link libraries
set(libpqxx_DIR "C:/Users/kbott/vcpkg/installed/x64-windows/share/libpqxx")
find_package(libpqxx CONFIG REQUIRED)

# Add source files
add_executable(database_test
    # data/contract.cpp
    # data/data_client.cpp   # if you have this
    # data/dataframe.cpp     # if you have this
    # data/enums.cpp         # if needed
    data/database_client.cpp
    data/ohlcv_data_handler.cpp
    # system/instrument.cpp
    data/main.cpp
)

# Link libraries: libpqxx, libpq, ws2_32 (Windows networking), and Arrow
target_link_libraries(database_test
    PRIVATE libpqxx::pqxx
    libpq
    ws2_32
    arrow
)
