cmake_minimum_required(VERSION 3.30)

project(Tinyio
        VERSION 0.1.0
        DESCRIPTION "Tinyio blazing fast IO lib"
        LANGUAGES CXX
)

set(TINYIO_VERSION_STRING "${PROJECT_VERSION}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/tinyio/version.h.in"
        "${CMAKE_BINARY_DIR}/src/tinyio/version.h")

# Cmake find modules
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")


#
# CMAKE GENERAL VARIABLES
#
set(CMAKE_SUPPRESS_DEVELOPER_WARNINGS ON CACHE INTERNAL "Suppress dev warnings from dependencies")
if (POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif ()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

#
# BUILD OPTIONS
#
option(TINYIO_BUILD_STATIC "Build static library" ON)
option(TINYIO_BUILD_SHARED "Build shared library" OFF)
option(TINYIO_BUILD_TESTS "Build tests" ON)
option(TINYIO_BUILD_EXAMPLES "Build the examples" OFF)
option(TINYIO_BUILD_BENCHMARKS "Build the benchmarks" OFF)
option(TINYIO_ENABLE_ASAN "Enable Address Sanitizer" OFF)
option("Enable Undefined Behavior Sanitizer" OFF)

include(GNUInstallDirs)
include(FetchContent)
include(CTest)

set(TINYIO_INSTALL_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
set(TINYIO_INSTALL_BINDIR "${CMAKE_INSTALL_BINDIR}")
set(TINYIO_INSTALL_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
set(TINYIO_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake")
set(TINYIO_INSTALL_DOCDIR "share/doc/tinyio")

#-----------------------------------------
# Dependencies
#----------------------------------------
include(CMakeParseArguments)
include(TinyioBuildUtils)
include(TinyioSanitizer)

set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig:/usr/lib64/pkgconfig")
find_package(PkgConfig REQUIRED)
pkg_check_modules(LibUring REQUIRED IMPORTED_TARGET liburing)

#abseil
find_package(Abseil REQUIRED)

# mimalloc
find_package(mimalloc REQUIRED)

include(TinyioBoost)
tinyio_find_boost(REQUIRED COMPONENTS context)

# openssl, min 3 required
find_package(OpenSSL 3 REQUIRED)
message(STATUS "OpenSSL: ${OPENSSL_VERSION}")
find_package(Threads REQUIRED)

add_subdirectory(src)

if (TINYIO_BUILD_TESTS)
    add_subdirectory(tests)
endif ()

if (TINYIO_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif ()

install(FILES LICENSE NOTICE DESTINATION ${TINYIO_INSTALL_DOCDIR})


# for benchmarks
if (TINYIO_BUILD_BENCHMARKS)
    include(FindPhotonLibOS)
    add_subdirectory(benchmarks/cpp)
endif ()
