set(TINYIO_INCLUDES "$<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/src>"
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>")

set(TINYIO_SOURCES
        buffer_pool.cc
        net.cc
        runtime.cc
        stack.cc
        tcp_listener.cc)

set(TINYIO_STATIC_BUILD_INTERFACE_LIBS)
set(TINYIO_SHARED_BUILD_INTERFACE_LIBS)
set(TINYIO_STATIC_INSTALL_INTERFACE_LIBS)
set(TINYIO_SHARED_INSTALL_INTERFACE_LIBS)

# TODO: adapt to tinyio
#list(APPEND
#        ICEBERG_STATIC_BUILD_INTERFACE_LIBS
#        "$<IF:$<BOOL:${NANOARROW_VENDORED}>,nanoarrow::nanoarrow_static,$<IF:$<TARGET_EXISTS:nanoarrow::nanoarrow_static>,nanoarrow::nanoarrow_static,nanoarrow::nanoarrow_shared>>"
#        nlohmann_json::nlohmann_json
#        spdlog::spdlog
#        ZLIB::ZLIB)
#list(APPEND
#        ICEBERG_SHARED_BUILD_INTERFACE_LIBS
#        "$<IF:$<BOOL:${NANOARROW_VENDORED}>,nanoarrow::nanoarrow_static,$<IF:$<TARGET_EXISTS:nanoarrow::nanoarrow_shared>,nanoarrow::nanoarrow_shared,nanoarrow::nanoarrow_static>>"
#        nlohmann_json::nlohmann_json
#        spdlog::spdlog
#        ZLIB::ZLIB)
#list(APPEND
#        ICEBERG_STATIC_INSTALL_INTERFACE_LIBS
#        "$<IF:$<BOOL:${NANOARROW_VENDORED}>,iceberg::nanoarrow_static,$<IF:$<TARGET_EXISTS:nanoarrow::nanoarrow_static>,nanoarrow::nanoarrow_static,nanoarrow::nanoarrow_shared>>"
#        "$<IF:$<BOOL:${NLOHMANN_JSON_VENDORED}>,iceberg::nlohmann_json,$<IF:$<TARGET_EXISTS:nlohmann_json::nlohmann_json>,nlohmann_json::nlohmann_json,nlohmann_json::nlohmann_json>>"
#        "$<IF:$<BOOL:${SPDLOG_VENDORED}>,iceberg::spdlog,spdlog::spdlog>")
#list(APPEND
#        ICEBERG_SHARED_INSTALL_INTERFACE_LIBS
#        "$<IF:$<BOOL:${NANOARROW_VENDORED}>,iceberg::nanoarrow_static,$<IF:$<TARGET_EXISTS:nanoarrow::nanoarrow_shared>,nanoarrow::nanoarrow_shared,nanoarrow::nanoarrow_static>>"
#        "$<IF:$<BOOL:${NLOHMANN_JSON_VENDORED}>,iceberg::nlohmann_json,$<IF:$<TARGET_EXISTS:nlohmann_json::nlohmann_json>,nlohmann_json::nlohmann_json,nlohmann_json::nlohmann_json>>"
#        "$<IF:$<BOOL:${SPDLOG_VENDORED}>,iceberg::spdlog,spdlog::spdlog>")

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