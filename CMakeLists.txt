cmake_minimum_required(VERSION 3.28)
project(tinyio LANGUAGES CXX C ASM)

set(CMAKE_CXX_STANDARD 23)
# We need that fr our TRY macro, the code is Linux only so this is fine
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(GTEST_GIT_TAG v1.17.0)

# For Google CRC32
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Cmake find modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(Functions)

# -----------------------------------------
# Optional Components
#------------------------------------------
set(TIO_DEMO_FILTER "all" CACHE STRING "List of tags to build (or 'all')")
option(TIO_BUILD_TESTS "Build the tinyio test suite" ON)
set(TIO_TEST_FILTER "all" CACHE STRING "List of tags to build (or 'all')")
option(TIO_BUILD_BITCASK "Build bitcask storage engine" ON)
option(TIO_BUILD_DEMOS "Build the tinyio demo executables" ON)
# pass -DTIO_DEMO_FILTER="core;http" to build specific subsets of demo.
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
set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig:/usr/lib64/pkgconfig")
find_package(PkgConfig REQUIRED)
pkg_check_modules(LibUring REQUIRED IMPORTED_TARGET liburing)

#gflags
find_package(gflags REQUIRED)

# gtest
if (TIO_BUILD_TESTS)
    find_package(GTest REQUIRED)
endif ()

# CRC32C
find_package(CRC32C REQUIRED)

# Abseil
find_package(Abseil REQUIRED)

# mimalloc
find_package(mimalloc REQUIRED)

include(KioBoost)
kio_find_boost(REQUIRED COMPONENTS context)

# openssl, min 3 required
find_package(OpenSSL 3 REQUIRED)
message(STATUS "OpenSSL: ${OPENSSL_VERSION}")
find_package(Threads REQUIRED)

# for benchmarks
if (TIO_BUILD_BENCHMARK)
    find_package(PhotonLibOS REQUIRED)
endif ()

include(DevTargets)


#-----------------------------------------------
# Aio lib, change later to tinyio
#----------------------------------------------
# Library definition
add_library(tinyio
        src/tinyio/runtime.cc
        src/tinyio/buffer_pool.cc
        src/tinyio/net.cc
        src/tinyio/stack.cc
        src/tinyio/tcp_listener.cc
)

# No architecture-specific assembly required when using Boost.Context
# Include directories
target_include_directories(tinyio PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
# Link dependencies
target_link_libraries(tinyio PUBLIC
        mimalloc-static
        PkgConfig::LibUring
        pthread
        OpenSSL::SSL
        OpenSSL::Crypto
        Boost::context
)
tio_add_sanitizers(tinyio)

#-----------------------------------------------
# bitcask lib
#----------------------------------------------
# Library definition
if (TIO_BUILD_BITCASK)
    add_library(bitcask
            src/bitcask/cache.cc
            src/bitcask/segment.cc
    )

    # Include directories
    target_include_directories(bitcask PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
    # Link dependencies
    target_link_libraries(bitcask
            PUBLIC
            PkgConfig::LibUring
            absl::flat_hash_map
            absl::flat_hash_set
            absl::inlined_vector
            PRIVATE
            crc32c
    )
    tio_add_sanitizers(bitcask)
endif ()


#-----------------------------------------------
# Includes
#----------------------------------------------
include_directories(${CMAKE_SOURCE_DIR})

if (TIO_BUILD_DEMOS)
    add_subdirectory(examples)
endif ()

# for benchmarks
if (TIO_BUILD_BENCHMARK)
    add_subdirectory(benchmarks/cpp)
endif ()

if (TIO_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)

    # Check target - runs all tests
    add_custom_target(tio-check
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            COMMENT "Running all tests"
            VERBATIM
    )

    # Quick check - only fast tests
    add_custom_target(tio-check-quick
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L "unit"
            COMMENT "Running quick unit tests"
            VERBATIM
    )

    message(STATUS "Added targets: check, check-quick")
endif ()

