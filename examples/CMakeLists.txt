function(add_tinyio_demo demo_name)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SOURCES LIBS)
    cmake_parse_arguments(ARG
            "${options}"
            "${oneValueArgs}"
            "${multiValueArgs}"
            ${ARGN})

    add_executable(${demo_name})

    if (ARG_SOURCES)
        target_sources(${demo_name} PRIVATE ${ARG_SOURCES})
    endif ()

    # Link against the tinyio ALIAS, plus GTest and any extra LIBS provided
    target_link_libraries(${demo_name}
            PRIVATE
            Tinyio::tinyio
            ${ARG_LIBS})
endfunction()

# ============================================================================
# Examples
# ============================================================================
add_tinyio_demo(03_http_services SOURCES tinyio/03_http_services.cpp)

#tio_add_demo(02_fanout tinyio/02_fanout.cc TAGS core LIBS gflags::gflags tinyio)
#tio_add_sanitizers(02_fanout)
#
#tio_add_demo(01_hello_echo tinyio/01_hello_echo.cc TAGS core LIBS gflags::gflags tinyio)
#tio_add_sanitizers(01_hello_echo)
#
#tio_add_demo(04_echo_client tinyio/04_echo_client.cc TAGS core LIBS gflags::gflags tinyio)
#tio_add_sanitizers(04_echo_client)
#
#tio_add_demo(05_bench_io tinyio/05_bench_io.cpp TAGS core LIBS gflags::gflags tinyio)
#tio_add_sanitizers(05_bench_io)

# Create a custom target to build all examples
add_custom_target(examples
        DEPENDS
        01_hello_echo
        02_fanout
        03_http_services
        04_echo_client
        05_bench_io
)

message(STATUS "  Examples will be installed to: ${CMAKE_INSTALL_PREFIX}/bin/examples")
