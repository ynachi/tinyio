function(tinyio_configure_warnings)
    add_library(tinyio_warnings INTERFACE)
    add_library(tinyio::warnings ALIAS tinyio_warnings)

    set(TINYIO_WARNING_OPTIONS
            -Wall
            -Wextra
            -Wconversion
            -Wpedantic
            -Wshadow
            -Wsign-conversion
            -Werror)

    # clang 22 warns that __COUNTER__ is a C2y extension, and -Wpedantic turns
    # that into an error. The TRY macros need __COUNTER__ to name their
    # temporaries uniquely (kio's TRY macros do the same), and __COUNTER__ is
    # standard from C++26 on, so the warning is about the language version, not
    # about the code. Only clang versions that raise the diagnostic get the
    # suppression, so older clangs never see an unknown warning option.
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND
            CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 22)
        list(APPEND TINYIO_WARNING_OPTIONS -Wno-c2y-extensions)
    endif ()

    target_compile_options(
            tinyio_warnings
            INTERFACE
            ${TINYIO_WARNING_OPTIONS})
endfunction()
