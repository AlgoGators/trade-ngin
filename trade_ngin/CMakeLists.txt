cmake_minimum_required(VERSION 3.10)
project(TradeEngine)

set(CMAKE_CXX_STANDARD 17)

add_executable(main
    data/contract.cpp
    data/data_client.cpp   # if you have this
    data/dataframe.cpp     # if you have this
    data/enums.cpp         # if needed
    system/instrument.cpp
    main.cpp
)
# run this:
#mkdir build && cd build
#cmake ..
#make
#./main
