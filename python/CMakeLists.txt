# Create the Python module
pybind11_add_module(trade_ngin_py
    module.cpp
    bindings/type_bindings.cpp
    bindings/database_bindings.cpp
    bindings/data_source_bindings.cpp
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

# libpqxx 8 marks its classes __declspec(dllimport) via the PQXX_SHARED
# interface definition. On MSVC that re-exports the std::string_view inline
# members from pqxx.dll, which then collide with the same inlines emitted into
# the binding objects (LNK2005 -> LNK1169). /FORCE:MULTIPLE tells the linker to
# keep the first definition; the implementations are identical, so this is safe.
if(MSVC)
    target_link_options(trade_ngin_py PRIVATE /FORCE:MULTIPLE /IGNORE:4006 /IGNORE:4088)
endif()

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
