# ===============================================================================
# TRADE_NGIN - DEPENDENCY MANAGEMENT
# ===============================================================================

# This file contains helper functions for managing dependencies in the project

# Function to add and manage a vendored dependency
function(add_vendored_dependency NAME VERSION URL)
    set(VENDOR_DIR "${THIRD_PARTY_DIR}/${NAME}")
    
    # Check if the dependency is already vendored
    if(NOT EXISTS "${VENDOR_DIR}")
        message(STATUS "Vendoring ${NAME} ${VERSION} from ${URL}")
        
        # Create the vendor directory
        file(MAKE_DIRECTORY "${VENDOR_DIR}")
        
        # Clone/download the dependency
        # This is a simple implementation - for production use, you should implement
        # proper downloading with checksum verification
        execute_process(
            COMMAND git clone --depth 1 --branch "v${VERSION}" ${URL} ${VENDOR_DIR}
            RESULT_VARIABLE GIT_RESULT
        )
        
        if(NOT GIT_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to clone ${NAME} ${VERSION} from ${URL}")
        endif()
    endif()
    
    # Add the vendored dependency to the project
    add_subdirectory("${VENDOR_DIR}")
endfunction()

# Function to check dependency versions
function(check_dependency_version NAME REQUIRED_VERSION)
    if(${NAME}_VERSION)
        if(${NAME}_VERSION VERSION_LESS ${REQUIRED_VERSION})
            message(WARNING "${NAME} version ${${NAME}_VERSION} is older than required ${REQUIRED_VERSION}")
        endif()
    endif()
endfunction()

# Function to find a dependency with version check
function(find_dependency_with_version NAME VERSION)
    find_package(${NAME} ${VERSION} QUIET)
    if(NOT ${NAME}_FOUND)
        message(STATUS "${NAME} ${VERSION} not found, trying without version constraint")
        find_package(${NAME} REQUIRED)
        check_dependency_version(${NAME} ${VERSION})
    endif()
endfunction()

# Function to report all dependencies and their versions
function(report_dependencies)
    message(STATUS "==== DEPENDENCY SUMMARY ====")
    
    # External dependencies
    if(GTest_FOUND)
        message(STATUS "GTest: ${GTest_VERSION}")
    endif()
    
    if(nlohmann_json_FOUND)
        message(STATUS "nlohmann_json: ${nlohmann_json_VERSION}")
    endif()
    
    if(Arrow_FOUND)
        message(STATUS "Arrow: ${Arrow_VERSION}")
    endif()
    
    if(libpqxx_FOUND)
        message(STATUS "libpqxx: ${libpqxx_VERSION}")
    endif()
    
    message(STATUS "============================")
endfunction()

# Function to generate a version header
function(generate_version_header OUTPUT_FILE)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in
        ${OUTPUT_FILE}
        @ONLY
    )
endfunction()

# Call report_dependencies at the end of this file
report_dependencies() 