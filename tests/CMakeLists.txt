find_package(GTest REQUIRED)

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

    # Link against the tinyio ALIAS, plus GTest and any extra LIBS provided
    target_link_libraries(${test_name}
            PRIVATE
            Tinyio::tinyio
            GTest::gtest_main
            ${ARG_LIBS})

    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

add_tinyio_test(buffer_pool_tests
        SOURCES tinyio/buffer_pool_test.cc
)

add_tinyio_test(intrusive_list_tests
        SOURCES tinyio/intrusive_list_test.cc
)

## tinyio tests
#
##========= tinyio unit tests (no io_uring required)
#tio_add_test(intrusive_list_tests tinyio/intrusive_list_test.cc
#        TAGS tinyio unit
#        LIBS tinyio GTest::gtest GTest::gtest_main
#)
#
#tio_add_test(buffer_pool_tests tinyio/buffer_pool_test.cc
#        TAGS tinyio unit
#        LIBS tinyio GTest::gtest GTest::gtest_main
#)
#
#tio_add_test(fiber_lifecycle_tests tinyio/fiber_lifecycle_test.cc
#        TAGS tinyio unit fiber
#        LIBS tinyio GTest::gtest GTest::gtest_main
#)
#
#tio_add_test(fiber_sync_tests tinyio/fiber_sync_test.cc
#        TAGS tinyio unit fiber sync
#        LIBS tinyio GTest::gtest GTest::gtest_main
#)
#
#tio_add_test(shutdown_cancellation_tests tinyio/shutdown_cancellation_test.cc
#        TAGS tinyio unit cancellation
#        LIBS tinyio GTest::gtest GTest::gtest_main
#)
#
#tio_add_test(cross_thread_post_tests tinyio/cross_thread_post_test.cc
#        TAGS tinyio unit post
#        LIBS tinyio GTest::gtest GTest::gtest_main
#)
#
#tio_add_test(io_correctness_tests tinyio/io_correctness_test.cc
#        TAGS tinyio unit io correctness
#        LIBS tinyio GTest::gtest GTest::gtest_main
#)


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
