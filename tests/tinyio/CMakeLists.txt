tinyio_add_test(buffer_pool_tests
        buffer_pool_test.cc
)

tinyio_add_test(context_tests
        context_test.cc
        # Switch() calls a .hidden symbol, so a shared libtinyio.so cannot
        # provide it to an executable; the test links the switch itself.
        ${PROJECT_SOURCE_DIR}/src/tinyio/detail/context.cc
)

tinyio_add_test(ring_buffer_tests
        ring_buffer_test.cc
)

tinyio_add_test(timer_wheel_tests
        timer_wheel_test.cc
)

tinyio_add_test(runtime_tests
        fiber_sync_test.cc
        fiber_lifecycle_test.cc
        cross_thread_post_test.cc
        io_correctness_test.cc
        shutdown_cancellation_test.cc
)
