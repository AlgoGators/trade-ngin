# Create the Python module
pybind11_add_module(trade_ngin_py
    module.cpp
    bindings/type_bindings.cpp
    bindings/database_bindings.cpp
    bindings/instrument_registry_bindings.cpp
    bindings/strategy_bindings.cpp
    bindings/api_bindings.cpp
)

# Set the output name of the Python module
set_target_properties(trade_ngin_py PROPERTIES
    OUTPUT_NAME tradengin
)

# Link against the main C++ library
target_link_libraries(trade_ngin_py PRIVATE
        trade_ngin
)

# Set proper rpath
if(APPLE)
    set_target_properties(trade_ngin_py PROPERTIES
        INSTALL_RPATH "@loader_path"
    )
else()
    set_target_properties(trade_ngin_py PROPERTIES
        INSTALL_RPATH "$ORIGIN"
    )
endif()

install(TARGETS trade_ngin_py
    LIBRARY DESTINATION .
)

# Set C++ standard
target_compile_features(trade_ngin_py PRIVATE cxx_std_20)
