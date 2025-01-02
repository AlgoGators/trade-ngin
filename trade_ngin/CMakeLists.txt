set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)

cmake_minimum_required(VERSION 3.10)
project(TradeEngine)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)

# Add include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/data                          # Project headers
    "C:/Users/kbott/libpqxx-master/include"           # Path to libpqxx headers
    "C:/Users/kbott/libpqxx-master/build/include"     # Path to libpqxx headers
    "C:/Program Files/PostgreSQL/16/include"          # Path to PostgreSQL headers
)

# Add library directories
link_directories(
    "C:/Users/kbott/libpqxx-master/build/src/Release" # Path to libpqxx library
    "C:/Program Files/PostgreSQL/16/lib"              # Path to PostgreSQL library
)

# Add source files
add_executable(database_test
    # data/contract.cpp
    # data/data_client.cpp   # if you have this
    # data/dataframe.cpp     # if you have this
    # data/enums.cpp         # if needed
    data/database_interface.cpp
    # system/instrument.cpp
    data/main.cpp
)

# Link libpqxx and libpq libraries
target_link_libraries(database_test pqxx libpq ws2_32)
