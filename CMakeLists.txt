cmake_minimum_required(VERSION 3.28)

project(Tinyio
        VERSION 0.1.0
        DESCRIPTION "Tinyio blazing fast IO lib"
        LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(GTEST_GIT_TAG v1.17.0)

# For Google CRC32
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

set(TINYIO_VERSION_STRING "${PROJECT_VERSION}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/tinyio/version.h.in"
        "${CMAKE_BINARY_DIR}/src/tinyio/version.h")

# Cmake find modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

# -----------------------------------------
# Optional Components
#------------------------------------------
option(TINYIO_BUILD_STATIC "Build static library" ON)
option(TINYIO_BUILD_SHARED "Build shared library" OFF)
option(TINYIO_BUILD_TESTS "Build the test suite" ON)
option(TINYIO_BUILD_EXAMPLES "Build the example programs" ON)
option(TINYIO_BUILD_BENCH "Build the benchmark servers" OFF)
option(TINYIO_BUILD_PHOTON_BENCH "Build the Photon network benchmarks" OFF)
option(TINYIO_BUILD_GFLAGS_BENCH "Build the gflags network benchmark" OFF)
option(TINYIO_USE_MIMALLOC "Link mimalloc as the global allocator" OFF)
option(TINYIO_FETCH_LIBURING "Build a pinned liburing instead of the system one" ON)
set(TINYIO_SANITIZER "none" CACHE STRING "none | address (ASan+UBSan) | thread (TSan)")
set_property(CACHE TINYIO_SANITIZER PROPERTY STRINGS none address thread)
option(ENABLE_CLANG_TIDY "Run clang-tidy during compilation" OFF)

# Enable clang-tidy during build if requested
if (ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if (CLANG_TIDY_EXE)
        set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY_EXE})
        message(STATUS "clang-tidy enabled: ${CLANG_TIDY_EXE}")
    else ()
        message(WARNING "clang-tidy requested but not found")
    endif ()
endif ()


#-----------------------------------------
# Dependencies
#----------------------------------------
find_program(TINYIO_CCACHE ccache)
if (TINYIO_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    set(CMAKE_C_COMPILER_LAUNCHER "${TINYIO_CCACHE}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${TINYIO_CCACHE}")
endif ()
find_program(TINYIO_MOLD mold)
if (TINYIO_MOLD)
    add_link_options(-fuse-ld=mold)
endif ()

include(TinyioWarnings)
include(TinyioDependencies)
include(TinyioSanitizers)
include(TinyioHelpers)

# add_tinyio_lib() and the install/export rules for the library.
include(TinyioBuildUtils)

tinyio_configure_dependencies()
tinyio_configure_sanitizers()
tinyio_configure_warnings()

include_directories(${CMAKE_SOURCE_DIR})

# The library itself: src/tinyio/CMakeLists.txt defines tinyio_static /
# tinyio_shared and the Tinyio::tinyio alias, and owns the install/export
# rules.
add_subdirectory(src)

if (TINYIO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()

# for benchmarks
if (TINYIO_BUILD_BENCH)
    add_subdirectory(benchmarks/cpp)
endif ()

if (TINYIO_BUILD_TESTS)
    include(CTest)
    enable_testing()
    # Defines gtest_discover_tests(). FetchContent(googletest) only supplies the
    # GTest:: targets, so without this the call in tinyio_add_test is unknown.
    include(GoogleTest)
    add_subdirectory(tests)
    message(STATUS "Added targets: check, check-quick")
endif ()

install(FILES LICENCE DESTINATION share/doc/tinyio)
