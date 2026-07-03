function(add_fastlog_test test_name)
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

    target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/src)

    target_link_libraries(${test_name}
            PRIVATE
            GTest::gtest_main
            ${ARG_LIBS})

    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

add_fastlog_test(fastlog_cache_test
        SOURCES cache_test.cc
        LIBS fastlog_static
)

add_fastlog_test(fastlog_address_manager_test
        SOURCES address_manager_test.cc
        LIBS fastlog_static
)

add_fastlog_test(fastlog_append_test
        SOURCES append_test.cc
        LIBS fastlog_static
)

add_fastlog_test(fastlog_page_arena_test
        SOURCES page_arena_test.cc
        LIBS fastlog_static
)

add_fastlog_test(fastlog_read_test
        SOURCES read_test.cc
        LIBS fastlog_static
)

add_fastlog_test(fastlog_commit_test
        SOURCES commit_test.cc
        LIBS fastlog_static
)

add_fastlog_test(fastlog_concurrency_test
        SOURCES concurrency_test.cc
        LIBS fastlog_static
)
