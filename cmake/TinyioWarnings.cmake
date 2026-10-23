function(tinyio_configure_warnings)
    add_library(tinyio_warnings INTERFACE)
    add_library(tinyio::warnings ALIAS tinyio_warnings)

    target_compile_options(
            tinyio_warnings
            INTERFACE
            -Wall
            -Wextra
            -Wconversion
            -Wpedantic
            -Wshadow
            -Wsign-conversion
            -Werror)
endfunction()
