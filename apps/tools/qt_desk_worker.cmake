# Included by the integration lane from apps/tools/CMakeLists.txt.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PostgreSQL REQUIRED)
    find_library(UUID_LIBRARY uuid REQUIRED)

    add_executable(qt_desk_worker
        qt_desk_worker.cpp
        "${CMAKE_SOURCE_DIR}/src/apps/qt_desk_dispatcher.cpp")
    target_include_directories(qt_desk_worker PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_link_libraries(qt_desk_worker PRIVATE
        trade_ngin PostgreSQL::PostgreSQL ${UUID_LIBRARY})
    set_target_properties(qt_desk_worker PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release")
    set(QT_DESK_WORKER_TARGET qt_desk_worker)

    add_executable(qt_desk_dispatcher_tests
        "${CMAKE_SOURCE_DIR}/tests/apps/test_qt_desk_dispatcher.cpp"
        "${CMAKE_SOURCE_DIR}/src/apps/qt_desk_dispatcher.cpp")
    target_include_directories(qt_desk_dispatcher_tests PRIVATE "${CMAKE_SOURCE_DIR}/include")
    target_link_libraries(qt_desk_dispatcher_tests PRIVATE ${UUID_LIBRARY})
    add_test(NAME qt_desk_dispatcher_tests COMMAND qt_desk_dispatcher_tests)
endif()
