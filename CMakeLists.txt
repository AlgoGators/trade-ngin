cmake_minimum_required(VERSION 3.10)
project(trade_ngin VERSION 0.1.0 LANGUAGES CXX)

# Set CMake policies
cmake_policy(SET CMP0079 NEW)

# Set C++ standard and compiler flags
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Add compiler flags
if(APPLE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
endif()
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -pedantic -fvisibility=hidden)
endif()

# Add preprocessor definitions to help with namespace resolution
add_definitions(-D_LIBCPP_ENABLE_NAMESPACE_ATTRIBUTES)

# Add compiler warnings
if(CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
    add_compile_options(-Wall -Wextra -pedantic)
    message(STATUS "Added compiler warnings for ${CMAKE_CXX_COMPILER_ID}")
endif()

# Find required packages
find_package(spdlog REQUIRED)
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)
find_package(nlohmann_json REQUIRED)
message(STATUS "Found nlohmann_json: ${nlohmann_json_VERSION}")

# TWS API setup
set(TWS_API_ROOT "$ENV{HOME}/IBJts/source/cppclient")
if(NOT EXISTS "${TWS_API_ROOT}")
    message(FATAL_ERROR "TWS API directory not found at ${TWS_API_ROOT}")
endif()

# Source files for TWS API
set(TWS_API_SOURCES
    ${TWS_API_ROOT}/client/ContractCondition.cpp
    ${TWS_API_ROOT}/client/DefaultEWrapper.cpp
    ${TWS_API_ROOT}/client/EClient.cpp
    ${TWS_API_ROOT}/client/EClientSocket.cpp
    ${TWS_API_ROOT}/client/EDecoder.cpp
    ${TWS_API_ROOT}/client/EMessage.cpp
    ${TWS_API_ROOT}/client/EMutex.cpp
    ${TWS_API_ROOT}/client/EReader.cpp
    ${TWS_API_ROOT}/client/EReaderOSSignal.cpp
    ${TWS_API_ROOT}/client/ESocket.cpp
    ${TWS_API_ROOT}/client/MarginCondition.cpp
    ${TWS_API_ROOT}/client/OrderCondition.cpp
    ${TWS_API_ROOT}/client/PercentChangeCondition.cpp
    ${TWS_API_ROOT}/client/PriceCondition.cpp
    ${TWS_API_ROOT}/client/SoftDollarTier.cpp
    ${TWS_API_ROOT}/client/StdAfx.cpp
    ${TWS_API_ROOT}/client/TimeCondition.cpp
    ${TWS_API_ROOT}/client/VolumeCondition.cpp
)

# Create TWS API library
add_library(TwsSocketClient STATIC ${TWS_API_SOURCES})
target_include_directories(TwsSocketClient PUBLIC ${TWS_API_ROOT}/client)
target_compile_definitions(TwsSocketClient PRIVATE -D_REENTRANT)
target_compile_options(TwsSocketClient PRIVATE -fvisibility=hidden)

# Our source files
set(SOURCE_FILES
    trade_ngin/system/ibkr_interface.cpp
    trade_ngin/system/ibkr_wrapper.cpp
)

# Create our main library
add_library(trade_ngin_lib STATIC ${SOURCE_FILES})

# Add include directories
target_include_directories(trade_ngin_lib PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${TWS_API_ROOT}/client
)

# Add compile definitions
target_compile_definitions(trade_ngin_lib PRIVATE)

# Link dependencies
target_link_libraries(trade_ngin_lib
    PRIVATE
    TwsSocketClient
    spdlog::spdlog
    CURL::libcurl
    Threads::Threads
    nlohmann_json::nlohmann_json
)

# Print configuration for debugging
message(STATUS "trade_ngin_lib configuration:")
get_target_property(TARGET_INCLUDES trade_ngin_lib INCLUDE_DIRECTORIES)
if(TARGET_INCLUDES)
    message(STATUS "  Include directories:")
    foreach(include ${TARGET_INCLUDES})
        message(STATUS "    ${include}")
    endforeach()
endif()

get_target_property(TARGET_DEFS trade_ngin_lib COMPILE_DEFINITIONS)
if(TARGET_DEFS)
    message(STATUS "  Compile definitions:")
    foreach(def ${TARGET_DEFS})
        message(STATUS "    ${def}")
    endforeach()
endif()

get_target_property(TARGET_LIBS trade_ngin_lib LINK_LIBRARIES)
if(TARGET_LIBS)
    message(STATUS "  Linked libraries:")
    foreach(lib ${TARGET_LIBS})
        message(STATUS "    ${lib}")
    endforeach()
endif()

# Create test executable
add_executable(test_ibkr_connection
    tests/test_ibkr_connection.cpp
)

target_link_libraries(test_ibkr_connection
    PRIVATE
    trade_ngin_lib
)
