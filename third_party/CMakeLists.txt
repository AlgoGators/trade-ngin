cmake_minimum_required(VERSION 3.10)
project(TwsSocketClient)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Create the TWS API library
add_library(TwsSocketClient
    EClient.cpp
    EClientSocket.cpp
    EDecoder.cpp
    EMessage.cpp
    EMutex.cpp
    EReader.cpp
    EReaderSignal.cpp
    ESocket.cpp
    DefaultEWrapper.cpp
)

target_include_directories(TwsSocketClient PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
