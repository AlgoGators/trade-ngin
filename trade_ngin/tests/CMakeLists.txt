add_executable(test_dataframe test_dataframe.cpp)
target_link_libraries(test_dataframe PRIVATE trade_ngin_core)
add_test(NAME test_dataframe COMMAND test_dataframe)

add_executable(test_database_client test_database_client.cpp)
target_link_libraries(test_database_client PRIVATE trade_ngin_core)
add_test(NAME test_database_client COMMAND test_database_client)

# Suppress the "unknown attribute ‘assume’" warnings when using Clang
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-Wno-unknown-attributes)
endif() 