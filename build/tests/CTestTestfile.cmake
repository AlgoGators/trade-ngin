# CMake generated Testfile for 
# Source directory: /Users/ryanmathieu/Documents/GitHub/trade-ngin/tests
# Build directory: /Users/ryanmathieu/Documents/GitHub/trade-ngin/build/tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(test_strategy "/Users/ryanmathieu/Documents/GitHub/trade-ngin/build/bin/test_strategy")
set_tests_properties(test_strategy PROPERTIES  ENVIRONMENT "POSTGRES_CONNECTION=postgresql://localhost:5432/trade_ngin" _BACKTRACE_TRIPLES "/Users/ryanmathieu/Documents/GitHub/trade-ngin/tests/CMakeLists.txt;58;add_test;/Users/ryanmathieu/Documents/GitHub/trade-ngin/tests/CMakeLists.txt;0;")
add_test(test_portfolio "/Users/ryanmathieu/Documents/GitHub/trade-ngin/build/bin/test_portfolio")
set_tests_properties(test_portfolio PROPERTIES  ENVIRONMENT "POSTGRES_CONNECTION=postgresql://localhost:5432/trade_ngin" _BACKTRACE_TRIPLES "/Users/ryanmathieu/Documents/GitHub/trade-ngin/tests/CMakeLists.txt;59;add_test;/Users/ryanmathieu/Documents/GitHub/trade-ngin/tests/CMakeLists.txt;0;")
add_test(test_integration "/Users/ryanmathieu/Documents/GitHub/trade-ngin/build/bin/test_integration")
set_tests_properties(test_integration PROPERTIES  ENVIRONMENT "POSTGRES_CONNECTION=postgresql://localhost:5432/trade_ngin" _BACKTRACE_TRIPLES "/Users/ryanmathieu/Documents/GitHub/trade-ngin/tests/CMakeLists.txt;60;add_test;/Users/ryanmathieu/Documents/GitHub/trade-ngin/tests/CMakeLists.txt;0;")
