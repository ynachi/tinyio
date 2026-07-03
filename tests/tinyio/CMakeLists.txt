set(TINYIO_TEST_RESOURCES "${CMAKE_SOURCE_DIR}/tests/tinyio/resources")
configure_file("test_config.h.in" "test_config.h")

function(add_tinyio_test test_name)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs SOURCES LIBS)
    cmake_parse_arguments(ARG
            "${options}"
            "${oneValueArgs}"
            "${multiValueArgs}"
            ${ARGN})

    add_executable(${test_name})

    if (ARG_SOURCES)
        target_sources(${test_name} PRIVATE ${ARG_SOURCES})
    endif ()

    target_link_libraries(${test_name}
            PRIVATE
            Tinyio::tinyio
            GTest::gtest_main
            ${ARG_LIBS})

    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

add_tinyio_test(buffer_pool_tests
        SOURCES buffer_pool_test.cc
)

add_tinyio_test(intrusive_list_tests
        SOURCES intrusive_list_test.cc
)

add_tinyio_test(runtime_tests
        SOURCES
        fiber_sync_test.cc
        fiber_lifecycle_test.cc
        cross_thread_post_test.cc
        io_correctness_test.cc
        shutdown_cancellation_test.cc
)
