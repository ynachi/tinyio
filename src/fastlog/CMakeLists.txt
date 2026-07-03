set(FASTLOG_INCLUDES "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/src>"
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>")

set(FASTLOG_SOURCES
        page_arena.cc
        fastlog.cc)

set(FASTLOG_STATIC_BUILD_INTERFACE_LIBS
        Tinyio::tinyio
        absl::crc32c
        absl::flat_hash_map
        absl::hash
        mimalloc-static
        Threads::Threads)

set(FASTLOG_SHARED_BUILD_INTERFACE_LIBS
        Tinyio::tinyio
        absl::crc32c
        absl::flat_hash_map
        absl::hash
        mimalloc-static
        Threads::Threads)

set(FASTLOG_STATIC_INSTALL_INTERFACE_LIBS)
set(FASTLOG_SHARED_INSTALL_INTERFACE_LIBS)


add_tinyio_lib(fastlog
        SOURCES ${FASTLOG_SOURCES}
        OUTPUTS FASTLOG_LIBS
        DEPENDENCIES ${FASTLOG_DEPENDENCIES}
        DEFINITIONS ${FASTLOG_DEFINITIONS}
        EXTRA_INCLUDES ${FASTLOG_INCLUDES}
        SHARED_LINK_LIBS ${FASTLOG_SHARED_BUILD_INTERFACE_LIBS}
        STATIC_LINK_LIBS ${FASTLOG_STATIC_BUILD_INTERFACE_LIBS}
        SHARED_INSTALL_INTERFACE_LIBS ${FASTLOG_SHARED_INSTALL_INTERFACE_LIBS}
        STATIC_INSTALL_INTERFACE_LIBS ${FASTLOG_STATIC_INSTALL_INTERFACE_LIBS}
)
if (FASTLOG_BUILD_SHARED)
    add_library(Fastlog::fastlog ALIAS fastlog_shared)
elseif (FASTLOG_BUILD_STATIC)
    add_library(Fastlog::fastlog ALIAS fastlog_static)
endif ()
