function(tinyio_configure_warnings)
    include(CheckCXXCompilerFlag)
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

    # Newer clang (observed in 23, first release uncertain) classifies
    # __COUNTER__ as a C2y extension, and -Wpedantic turns that into an
    # error. The TRY macros need __COUNTER__ to name their temporaries
    # uniquely (kio's TRY macros do the same), and __COUNTER__ is on the C
    # standardization track (C2y), not in any C++ standard yet, so the
    # warning is about the language version, not about the code. The flag
    # is probed rather than version-gated: compilers without the option
    # reject it as unknown, which would itself trip -Werror.
    check_cxx_compiler_flag("-Wno-c2y-extensions" TINYIO_HAVE_WNO_C2Y_EXTENSIONS)
    if (TINYIO_HAVE_WNO_C2Y_EXTENSIONS)
        list(APPEND TINYIO_WARNING_OPTIONS -Wno-c2y-extensions)
    endif ()

    target_compile_options(
            tinyio_warnings
            INTERFACE
            ${TINYIO_WARNING_OPTIONS})
endfunction()
