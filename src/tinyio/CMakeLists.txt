set(TINYIO_INCLUDES "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/src>"
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>")

set(TINYIO_SOURCES
        buffer_pool.cc
        net.cc
        runtime.cc
        stack.cc
        tcp_listener.cc)

# mimalloc is optional: TINYIO_USE_MIMALLOC turns it on, and the release
# preset does. The sanitizers refuse to run with it, because mimalloc
# replaces the allocator they need to see.
if (TINYIO_USE_MIMALLOC)
    set(TINYIO_MIMALLOC_LIBS mimalloc-static)
    set(TINYIO_MIMALLOC_SHARED_LIBS mimalloc)
else ()
    set(TINYIO_MIMALLOC_LIBS)
    set(TINYIO_MIMALLOC_SHARED_LIBS)
endif ()

set(TINYIO_STATIC_BUILD_INTERFACE_LIBS
        tinyio::liburing
        Boost::context
        ${TINYIO_MIMALLOC_LIBS}
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
        yalantinglibs)
set(TINYIO_SHARED_BUILD_INTERFACE_LIBS
        tinyio::liburing
        Boost::context
        ${TINYIO_MIMALLOC_SHARED_LIBS}
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
        yalantinglibs)
set(TINYIO_STATIC_INSTALL_INTERFACE_LIBS)
set(TINYIO_SHARED_INSTALL_INTERFACE_LIBS)


add_tinyio_lib(tinyio
        SOURCES ${TINYIO_SOURCES}
        OUTPUTS TINYIO_LIBS
        DEPENDENCIES ${TINYIO_DEPENDENCIES}
        DEFINITIONS ${TINYIO_DEFINITIONS}
        EXTRA_INCLUDES ${TINYIO_INCLUDES}
        SHARED_LINK_LIBS ${TINYIO_SHARED_BUILD_INTERFACE_LIBS}
        STATIC_LINK_LIBS ${TINYIO_STATIC_BUILD_INTERFACE_LIBS}
        SHARED_INSTALL_INTERFACE_LIBS ${TINYIO_SHARED_INSTALL_INTERFACE_LIBS}
        STATIC_INSTALL_INTERFACE_LIBS ${TINYIO_STATIC_INSTALL_INTERFACE_LIBS}
)

# The strict warning set applies to tinyio's own sources, not to the fetched
# dependencies.
if (TINYIO_BUILD_SHARED)
    target_link_libraries(tinyio_shared PRIVATE tinyio_warnings)
endif ()
if (TINYIO_BUILD_STATIC)
    target_link_libraries(tinyio_static PRIVATE tinyio_warnings)
endif ()

# easylog prints a stack trace on a fatal record, and it reaches for
# std::stacktrace when the standard library provides it. Where that cannot be
# linked, cmake/TinyioStacktrace.cmake defines __cpp_lib_stacktrace to 0 so
# easylog uses YLT's own backtrace helper instead. The definition is PUBLIC:
# easylog sits in tinyio's public headers, so consumers need it as well.
if (TINYIO_STACKTRACE_DEFINITIONS)
    foreach (tinyio_target IN ITEMS tinyio_static tinyio_shared)
        if (TARGET ${tinyio_target})
            target_compile_definitions(${tinyio_target} PUBLIC
                    ${TINYIO_STACKTRACE_DEFINITIONS})
        endif ()
    endforeach ()
endif ()

# easylog picks its formatting backend from __has_include(<fmt/format.h>), so
# the backend would otherwise depend on whether the host has fmt installed.
# cmake/TinyioFmtShim/fmt/format.h pins it to std::format, the implementation
# the CI container already uses, and keeps the build reproducible. The shim
# directory is on the include path of every consumer of the build tree because
# easylog sits in tinyio's public headers. It is not installed: tinyio's
# public headers must not shadow the real fmt for consumers.
if (NOT TINYIO_USE_SYSTEM_FMT)
    foreach (tinyio_target IN ITEMS tinyio_static tinyio_shared)
        if (TARGET ${tinyio_target})
            target_include_directories(${tinyio_target} PUBLIC
                    "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/cmake/TinyioFmtShim>")
        endif ()
    endforeach ()
endif ()

if (TINYIO_BUILD_SHARED)
    add_library(Tinyio::tinyio ALIAS tinyio_shared)
elseif (TINYIO_BUILD_STATIC)
    add_library(Tinyio::tinyio ALIAS tinyio_static)
endif ()
