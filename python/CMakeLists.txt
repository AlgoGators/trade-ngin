find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(trade_ngin_py
    module.cpp
    bindings/types_bindings.hpp
    bindings/strategy_bindings.hpp
)

set_target_properties(trade_ngin_py PROPERTIES
    OUTPUT_NAME trade_ngin
)

target_link_libraries(trade_ngin_py
    PRIVATE
        trade_ngin
)

target_include_directories(trade_ngin_py
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

target_compile_features(trade_ngin_py PRIVATE cxx_std_20)
