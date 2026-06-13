# ============================================================================
# Examples
# ============================================================================
tio_add_demo(03_http_services tinyio/03_http_services.cpp TAGS core LIBS gflags::gflags tinyio)
tio_add_sanitizers(03_http_services)

tio_add_demo(02_fanout tinyio/02_fanout.cc TAGS core LIBS gflags::gflags tinyio)
tio_add_sanitizers(02_fanout)

tio_add_demo(01_hello_echo tinyio/01_hello_echo.cc TAGS core LIBS gflags::gflags tinyio)
tio_add_sanitizers(01_hello_echo)

tio_add_demo(04_echo_client tinyio/04_echo_client.cc TAGS core LIBS gflags::gflags tinyio)
tio_add_sanitizers(04_echo_client)

# Create a custom target to build all examples
add_custom_target(examples
        DEPENDS
        01_hello_echo
        02_fanout
        03_http_services
        04_echo_client
)

message(STATUS "  Examples will be installed to: ${CMAKE_INSTALL_PREFIX}/bin/examples")
