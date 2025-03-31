# - Find libpqxx
# Find the libpqxx libraries
#
# This module defines the following variables:
#  LIBPQXX_FOUND - System has libpqxx
#  LIBPQXX_INCLUDE_DIRS - The libpqxx include directories
#  LIBPQXX_LIBRARIES - The libraries needed to use libpqxx

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(PC_LIBPQXX QUIET libpqxx)
endif()

# First look for manually specified paths
if(LIBPQXX_INCLUDE_DIR AND LIBPQXX_LIBRARY)
  # Use the manually specified paths
  message(STATUS "Using manually specified libpqxx paths")
else()
  # Search for the include directory
  find_path(LIBPQXX_INCLUDE_DIR
    NAMES pqxx/pqxx
    PATHS
      /usr/local/include
      /usr/include
      /opt/homebrew/include
    HINTS ${PC_LIBPQXX_INCLUDEDIR} ${PC_LIBPQXX_INCLUDE_DIRS}
  )

  # Search for the library
  find_library(LIBPQXX_LIBRARY
    NAMES pqxx libpqxx libpqxx-7.10 libpqxx.dylib
    PATHS
      /usr/local/lib
      /usr/lib
      /usr/local/lib64
      /usr/lib64
      /opt/homebrew/lib
    HINTS ${PC_LIBPQXX_LIBDIR} ${PC_LIBPQXX_LIBRARY_DIRS}
  )
endif()

# Debug output
message(STATUS "LIBPQXX_INCLUDE_DIR: ${LIBPQXX_INCLUDE_DIR}")
message(STATUS "LIBPQXX_LIBRARY: ${LIBPQXX_LIBRARY}")

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(libpqxx
  REQUIRED_VARS LIBPQXX_LIBRARY LIBPQXX_INCLUDE_DIR
)

if(LIBPQXX_FOUND)
  set(LIBPQXX_LIBRARIES ${LIBPQXX_LIBRARY})
  set(LIBPQXX_INCLUDE_DIRS ${LIBPQXX_INCLUDE_DIR})
  message(STATUS "libpqxx found: ${LIBPQXX_LIBRARIES}")
endif()

mark_as_advanced(LIBPQXX_INCLUDE_DIR LIBPQXX_LIBRARY) 