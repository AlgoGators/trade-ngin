#!/bin/bash

# Create third_party directory if it doesn't exist
mkdir -p third_party
cd third_party

# Download TWS API
if [ ! -d "tws-api" ]; then
    echo "Downloading TWS API..."
    curl -O https://download2.interactivebrokers.com/portal/clientportal.beta.gw.zip
    curl -O https://download2.interactivebrokers.com/twsapi_macunix.1019.01.zip
    
    # Extract the archive
    unzip -o twsapi_macunix.1019.01.zip -d tws-api
    rm twsapi_macunix.1019.01.zip
else
    echo "TWS API already exists"
fi

# Create CMakeLists.txt for TWS API
cd tws-api/IBJts/source/cppclient/client
if [ ! -f "CMakeLists.txt" ]; then
    echo "Creating CMakeLists.txt for TWS API..."
    cat > CMakeLists.txt << 'EOL'
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
EOL
fi

echo "TWS API setup complete"
