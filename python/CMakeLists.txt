find_package(pybind11 CONFIG REQUIRED)
find_package(pybind11_json CONFIG REQUIRED)

# Create the Python module
pybind11_add_module(trade_ngin_py
    module.cpp
    bindings/type_bindings.hpp
    bindings/strategy_bindings.hpp
)

# Link against pybind11_json
target_link_libraries(trade_ngin_py PRIVATE
        pybind11_json
)

# Set the output name of the Python module
set_target_properties(trade_ngin_py PROPERTIES
    OUTPUT_NAME trade_ngin
)

# Link against the main C++ library
target_link_libraries(trade_ngin_py PRIVATE
        trade_ngin
)

# Include the main library's headers
target_include_directories(trade_ngin_py PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

# Set C++ standard
target_compile_features(trade_ngin_py PRIVATE cxx_std_20)
