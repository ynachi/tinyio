cmake_minimum_required(VERSION 3.28)

project(tinyio
        VERSION 0.1.0
        DESCRIPTION "Tinyio blazing fast IO lib"
        LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# -----------------------------------------
# Options
#------------------------------------------
option(TINYIO_BUILD_SHARED "Build tinyio as a shared library instead of a static one" OFF)
option(TINYIO_BUILD_TESTS "Build the test suite" ${PROJECT_IS_TOP_LEVEL})
option(TINYIO_BUILD_EXAMPLES "Build the example programs" ${PROJECT_IS_TOP_LEVEL})
option(TINYIO_BUILD_BENCH "Build the benchmark servers" OFF)
option(TINYIO_USE_MIMALLOC "Link mimalloc as the global allocator" OFF)
option(TINYIO_FETCH_LIBURING "Build a pinned liburing instead of the system one" ON)
option(TINYIO_CLANG_TIDY "Run clang-tidy during compilation" OFF)
set(TINYIO_SANITIZER "none" CACHE STRING "none | address (ASan+UBSan) | thread (TSan)")
set_property(CACHE TINYIO_SANITIZER PROPERTY STRINGS none address thread)

#-----------------------------------------
# Toolchain and dependencies
#----------------------------------------
include(GNUInstallDirs)
include(TinyioToolchain)
include(TinyioSanitizers)
include(TinyioWarnings)
include(TinyioDependencies)
include(TinyioHelpers)

# Sanitizers first: their flags only reach targets defined after them, and the
# fetched dependencies should be instrumented too.
tinyio_configure_sanitizers()
tinyio_configure_warnings()
tinyio_configure_dependencies()

# easylog prints a stack trace on a fatal record. Resolving the addresses in
# it needs the executable's symbols to be in the dynamic symbol table, so every
# executable is linked with -rdynamic. Without it a crash prints bare
# addresses and the trace is close to useless.
set(CMAKE_ENABLE_EXPORTS ON)

#-----------------------------------------
# Targets
#----------------------------------------
# The library, tinyio::tinyio, and its install/export rules.
add_subdirectory(src/tinyio)

if (TINYIO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()

if (TINYIO_BUILD_BENCH)
    add_subdirectory(benchmarks/cpp)
endif ()

if (TINYIO_BUILD_TESTS)
    enable_testing()
    # Defines gtest_discover_tests(). FetchContent(googletest) only supplies the
    # GTest:: targets, so without this the call in tinyio_add_test is unknown.
    include(GoogleTest)
    add_subdirectory(tests/tinyio)
endif ()

install(FILES LICENCE DESTINATION "${CMAKE_INSTALL_DOCDIR}")
