set(TINYIO_TEST_RESOURCES "${CMAKE_SOURCE_DIR}/tests/tinyio/resources")
configure_file("test_config.h.in" "test_config.h")

tinyio_add_test(buffer_pool_tests
        buffer_pool_test.cc
)

tinyio_add_test(intrusive_list_tests
        intrusive_list_test.cc
)

tinyio_add_test(runtime_tests
        "fiber_sync_test.cc;fiber_lifecycle_test.cc;cross_thread_post_test.cc;io_correctness_test.cc;shutdown_cancellation_test.cc"
)
