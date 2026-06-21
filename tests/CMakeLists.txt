# tinyio tests

#========= tinyio unit tests (no io_uring required)
tio_add_test(intrusive_list_tests tinyio/intrusive_list_test.cc
        TAGS tinyio unit
        LIBS tinyio GTest::gtest GTest::gtest_main
)

tio_add_test(buffer_pool_tests tinyio/buffer_pool_test.cc
        TAGS tinyio unit
        LIBS tinyio GTest::gtest GTest::gtest_main
)

tio_add_test(fiber_lifecycle_tests tinyio/fiber_lifecycle_test.cc
        TAGS tinyio unit fiber
        LIBS tinyio GTest::gtest GTest::gtest_main
)

tio_add_test(fiber_sync_tests tinyio/fiber_sync_test.cc
        TAGS tinyio unit fiber sync
        LIBS tinyio GTest::gtest GTest::gtest_main
)

tio_add_test(shutdown_cancellation_tests tinyio/shutdown_cancellation_test.cc
        TAGS tinyio unit cancellation
        LIBS tinyio GTest::gtest GTest::gtest_main
)

tio_add_test(cross_thread_post_tests tinyio/cross_thread_post_test.cc
        TAGS tinyio unit post
        LIBS tinyio GTest::gtest GTest::gtest_main
)

tio_add_test(io_correctness_tests tinyio/io_correctness_test.cc
        TAGS tinyio unit io correctness
        LIBS tinyio GTest::gtest GTest::gtest_main
)


#========= uring
#tio_add_test(op_pool_tests uring/op_pool_test.cpp
#        TAGS uring unit
#        LIBS uring GTest::gtest GTest::gtest_main
#)
#
#tio_add_test(task_tests uring/task_test.cpp
#        TAGS uring unit
#        LIBS uring GTest::gtest GTest::gtest_main
#)
#
#tio_add_test(mpsc_queue_tests uring/mpsc_queue_test.cpp
#        TAGS uring unit queue
#        LIBS GTest::gtest GTest::gtest_main
#)
#if (TARGET mpsc_queue_tests)
#    target_include_directories(mpsc_queue_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
#endif ()
#
#tio_add_test(logger_tests uring/logger_test.cpp
#        TAGS uring unit
#        LIBS uring GTest::gtest GTest::gtest_main
#)
#
#tio_add_test(context_remote_tests uring/context_remote_test.cpp
#        TAGS uring integration queue
#        LIBS uring GTest::gtest GTest::gtest_main
#)
#
#tio_add_test(fiber_io_tests uring/fiber_io_test.cpp
#        TAGS uring integration fiber
#        LIBS uring GTest::gtest GTest::gtest_main
#)
#
#tio_add_test(fiber_sync_tests uring/fiber_sync_test.cpp
#        TAGS uring integration fiber sync
#        LIBS uring GTest::gtest GTest::gtest_main
#)
#
##===============================
## Bitcask tests
##================================
#tio_add_test(bitcask_cache_tests bitcask/cache_test.cc
#        TAGS bitcask unit cache
#        LIBS bitcask GTest::gtest GTest::gtest_main
#)
#
#tio_add_test(bitcask_segment_tests bitcask/segment_test.cc
#        TAGS bitcask unit segment
#        LIBS bitcask GTest::gtest GTest::gtest_main
#)
