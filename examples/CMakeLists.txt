# ============================================================================
# Examples
# ============================================================================
tio_add_demo(03_http_services tinyio/03_http_services.cpp TAGS core LIBS gflags::gflags tinyio)
tio_add_sanitizers(03_http_services)
target_compile_definitions(03_http_services PRIVATE DAIO_STATS=1)

# Create a custom target to build all examples
add_custom_target(examples
        DEPENDS
        02_echo_server
        03_http_services
)

message(STATUS "  Examples will be installed to: ${CMAKE_INSTALL_PREFIX}/bin/examples")
