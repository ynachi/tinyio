# The tinyio library: one target, static by default, shared with
# TINYIO_BUILD_SHARED. In the build tree and from the installed package alike,
# consumers link tinyio::tinyio.

configure_file(version.h.in "${PROJECT_BINARY_DIR}/src/tinyio/version.h")

if (TINYIO_BUILD_SHARED)
    add_library(tinyio SHARED)
else ()
    add_library(tinyio STATIC)
endif ()
add_library(tinyio::tinyio ALIAS tinyio)

target_sources(tinyio
        PRIVATE
        detail/context.cc
        detail/mpsc_queue.hpp
        detail/stack.cc
        net/net.cc
        net/tcp_listener.cc
        runtime/buffer_pool.cc
        runtime/event_loop.cc
        runtime/io_ops.cc
        runtime/runtime.cc
        runtime/scheduler.cc

        # The headers are a file set, so they belong to the target: IDEs index
        # them as project files, the base directories become the include path,
        # and install(TARGETS) installs them with the layout they have here.
        # The detail/ headers are not API, but the public ones include them, so
        # they have to travel with them.
        PUBLIC
        FILE_SET HEADERS
        BASE_DIRS "${PROJECT_SOURCE_DIR}/src" "${PROJECT_BINARY_DIR}/src"
        FILES
        check.hpp
        error.hpp
        logging.hpp
        tinyio.hpp
        "${PROJECT_BINARY_DIR}/src/tinyio/version.h"
        detail/context.hpp
        detail/fiber.hpp
        detail/ring_buffer.hpp
        detail/sanitizer_hooks.hpp
        detail/stack.hpp
        detail/timer_wheel.hpp
        net/fd.hpp
        net/net.hpp
        net/tcp_listener.hpp
        runtime/buffer_pool.hpp
        runtime/fiber_sync.hpp
        runtime/runtime.hpp
)

target_compile_features(tinyio PUBLIC cxx_std_23)

if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-fcf-protection=branch" TINYIO_HAVE_FCF_PROTECTION_BRANCH)
    if (TINYIO_HAVE_FCF_PROTECTION_BRANCH)
        # TinyioSwitchContext pivots %rsp and returns to another stack, which is
        # incompatible with hardware shadow stacks (SHSTK). Marking context.cc
        # with branch-only protection drops GNU_PROPERTY_X86_FEATURE_1_SHSTK from
        # the binary so the loader does not enable shadow stacks for it.
        set_source_files_properties(detail/context.cc PROPERTIES
                COMPILE_OPTIONS "-fcf-protection=branch")
    endif ()
endif ()

target_link_libraries(tinyio
        PUBLIC
        tinyio::liburing
        Threads::Threads
        # YLT is header-only and installs its headers next to tinyio's, so only
        # the build tree needs its target.
        "$<BUILD_INTERFACE:yalantinglibs>"
        PRIVATE
        # The strict warning set applies to tinyio's own sources only. Kept out
        # of the export: a static library records its private dependencies in
        # the link interface, and the flags would reach consumers.
        "$<BUILD_INTERFACE:tinyio::warnings>")

# mimalloc is optional: TINYIO_USE_MIMALLOC turns it on, and the release
# preset does. It replaces the global allocator of everything that links
# tinyio in the build tree. The sanitizers refuse to run with it.
if (TINYIO_USE_MIMALLOC)
    target_link_libraries(tinyio PUBLIC "$<BUILD_INTERFACE:mimalloc-static>")
endif ()

tinyio_configure_stacktrace(tinyio)

#-----------------------------------------
# Install
#----------------------------------------
# <prefix>/include/tinyio/...    the header file set, version.h included
# <prefix>/lib/libtinyio.{a,so}
# <prefix>/lib/cmake/tinyio/     find_package(tinyio) -> tinyio::tinyio
include(CMakePackageConfigHelpers)
set(TINYIO_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/tinyio")

install(TARGETS tinyio
        EXPORT tinyio-targets
        FILE_SET HEADERS)
install(EXPORT tinyio-targets
        NAMESPACE tinyio::
        DESTINATION "${TINYIO_INSTALL_CMAKEDIR}")

configure_package_config_file(tinyio-config.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/tinyio-config.cmake"
        INSTALL_DESTINATION "${TINYIO_INSTALL_CMAKEDIR}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/tinyio-config-version.cmake"
        COMPATIBILITY SameMajorVersion)
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/tinyio-config.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/tinyio-config-version.cmake"
        DESTINATION "${TINYIO_INSTALL_CMAKEDIR}")
