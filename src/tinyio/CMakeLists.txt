set(TINYIO_INCLUDES "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/src>"
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>")

set(TINYIO_SOURCES
        buffer_pool.cc
        net.cc
        runtime.cc
        stack.cc
        tcp_listener.cc)

set(TINYIO_STATIC_BUILD_INTERFACE_LIBS
        PkgConfig::LibUring
        Boost::context
        mimalloc-static
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
        absl::log)
set(TINYIO_SHARED_BUILD_INTERFACE_LIBS
        PkgConfig::LibUring
        Boost::context
        mimalloc
        OpenSSL::SSL
        OpenSSL::Crypto
        Threads::Threads
        absl::log)
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
if (TINYIO_BUILD_SHARED)
    add_library(Tinyio::tinyio ALIAS tinyio_shared)
elseif (TINYIO_BUILD_STATIC)
    add_library(Tinyio::tinyio ALIAS tinyio_static)
endif ()
