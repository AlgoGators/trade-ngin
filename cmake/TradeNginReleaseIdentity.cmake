function(trade_ngin_validate_release_identity build_type short_sha full_sha dirty)
    if(NOT build_type STREQUAL "Release")
        return()
    endif()

    string(LENGTH "${short_sha}" short_length)
    string(LENGTH "${full_sha}" full_length)
    string(REGEX MATCH "^[0-9a-f]+$" short_hex "${short_sha}")
    string(REGEX MATCH "^[0-9a-f]+$" full_hex "${full_sha}")
    string(FIND "${full_sha}" "${short_sha}" prefix_position)

    if(short_length LESS 7 OR short_length GREATER 40 OR
       NOT short_hex STREQUAL short_sha OR
       NOT full_length EQUAL 40 OR NOT full_hex STREQUAL full_sha OR
       NOT prefix_position EQUAL 0 OR dirty)
        message(FATAL_ERROR
            "release_identity_invalid: Release requires a clean exact lowercase Git SHA "
            "(short='${short_sha}', full='${full_sha}', dirty='${dirty}')")
    endif()
endfunction()
