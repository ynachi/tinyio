# tinyio_add_test(name source [LIBS ...]): one gtest executable, registered with ctest.
function(tinyio_add_test name source)
    cmake_parse_arguments(ARG "" "" "LIBS" ${ARGN})
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE Tinyio::tinyio ${ARG_LIBS} GTest::gtest GTest::gtest_main tinyio_warnings)
    gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
endfunction()
