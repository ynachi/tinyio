# tinyio_add_test(<name> <source>... [LIBS <lib>...]): one gtest executable,
# registered with ctest.
function(tinyio_add_test name)
    cmake_parse_arguments(ARG "" "" "LIBS" ${ARGN})
    add_executable(${name} ${ARG_UNPARSED_ARGUMENTS})
    target_link_libraries(${name} PRIVATE tinyio::tinyio ${ARG_LIBS} GTest::gtest GTest::gtest_main tinyio::warnings)
    gtest_discover_tests(${name} DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
endfunction()
