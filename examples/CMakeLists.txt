# ============================================================================
# Examples
# ============================================================================

# 01_hello_echo, 02_fanout and 04_echo_client parse their flags with gflags.
# gflags 2.2 only creates the namespaced target (gflags::gflags) when this is
# set before find_package.
set(GFLAGS_USE_TARGET_NAMESPACE TRUE)
find_package(gflags REQUIRED)

add_executable(01_hello_echo tinyio/01_hello_echo.cc)
target_link_libraries(01_hello_echo PRIVATE Tinyio::tinyio tinyio_warnings gflags::gflags)

add_executable(02_fanout tinyio/02_fanout.cc)
target_link_libraries(02_fanout PRIVATE Tinyio::tinyio tinyio_warnings gflags::gflags)

add_executable(03_http_services tinyio/03_http_services.cc)
target_link_libraries(03_http_services PRIVATE Tinyio::tinyio tinyio_warnings)

add_executable(04_echo_client tinyio/04_echo_client.cc)
target_link_libraries(04_echo_client PRIVATE Tinyio::tinyio tinyio_warnings gflags::gflags)

add_executable(05_bench_io tinyio/05_bench_io.cc)
target_link_libraries(05_bench_io PRIVATE Tinyio::tinyio tinyio_warnings)

# Create a custom target to build all examples
add_custom_target(examples
        DEPENDS
        01_hello_echo
        02_fanout
        03_http_services
        04_echo_client
        05_bench_io
)

install(TARGETS
        01_hello_echo
        02_fanout
        03_http_services
        04_echo_client
        05_bench_io
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}/examples")

message(STATUS "  Examples will be installed to: ${CMAKE_INSTALL_PREFIX}/bin/examples")
