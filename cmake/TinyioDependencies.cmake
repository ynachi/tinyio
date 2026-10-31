# Every third-party dependency tinyio's build uses is declared here, each one
# behind the option that needs it. Library, tests, examples and benchmarks only
# link the targets this file defines; none of them looks a package up itself.
#
#   tinyio::liburing   liburing >= 2.9, pinned and built in the tree by default
#   Threads::Threads
#   yalantinglibs      YLT, header-only: easylog, struct_pack
#   mimalloc-static    TINYIO_USE_MIMALLOC
#   GTest::gtest       TINYIO_BUILD_TESTS
#   gflags::gflags     TINYIO_BUILD_EXAMPLES
include(FetchContent)

function(tinyio_configure_dependencies)
    find_package(Threads REQUIRED)

    tinyio_configure_liburing()

    #
    # YLT (yalantinglibs)
    #
    # tinyio uses two YLT modules: easylog for logging and struct_pack for
    # serialization. YLT is header-only the way we embed it here (FetchContent
    # -> add_subdirectory): none of it is compiled, the interface target only
    # carries the include path. The YLT_ENABLE_* switches below turn off every
    # optional backend YLT could otherwise pull in (OpenSSL, ibverbs, io_uring,
    # CUDA, PMR, ...), so the only reachable parts are the header modules we
    # actually use.
    #
    message(STATUS "Setting up YLT (yalantinglibs)")

    set(YLT_ENABLE_SSL OFF)
    set(YLT_ENABLE_NTLS OFF)
    set(YLT_ENABLE_IBV OFF)
    set(YLT_ENABLE_URMA OFF)
    set(YLT_ENABLE_IO_URING OFF)
    set(YLT_ENABLE_FILE_IO_URING OFF)
    set(YLT_ENABLE_OWN_RING OFF)
    set(YLT_ENABLE_CUDA OFF)
    set(YLT_ENABLE_NVCOMP OFF)
    set(YLT_ENABLE_ND OFF)
    set(YLT_ENABLE_PMR OFF)
    # YLT's own stacktrace support links dl; the YLT_ENABLE_LD macro that
    # actually turns dladdr on is not one of YLT's switches.
    set(YLT_ENABLE_DL ON)

    # YLT installs its own headers (include/ylt/...) because tinyio's public
    # headers include <ylt/easylog.hpp>. Its thirdparty and standalone trees
    # (asio, frozen, cinatra, iguana) are not reachable from easylog and are
    # several hundred files, so they stay out of the install.
    set(INSTALL_THIRDPARTY OFF)
    set(INSTALL_STANDALONE OFF)

    # Pinned to the 0.6.1 release tag, not the v0.6.1 branch: the branch moves,
    # the tag does not. 0.6.1 is the first release with the easylog fix for a
    # lost wakeup in appender::stop(): it set the stop flag while holding the
    # file mutex instead of the queue mutex the writer thread waits on, so the
    # notification could be dropped and ~appender() would block forever in
    # join(). Any tinyio program that logs and exits could hang at exit.
    FetchContent_Declare(
            yalantinglibs
            SYSTEM
            GIT_REPOSITORY https://github.com/alibaba/yalantinglibs.git
            GIT_TAG 0.6.1
            GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(yalantinglibs)

    #
    # MIMALLOC
    #
    if (TINYIO_USE_MIMALLOC)
        set(MI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(MI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
        set(MI_BUILD_OBJECT OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(mimalloc
                SYSTEM
                GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
                GIT_TAG v3.3.2 GIT_SHALLOW ON)
        FetchContent_MakeAvailable(mimalloc)
    endif ()

    #
    # GTEST
    #
    if (TINYIO_BUILD_TESTS)
        set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
        set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
                googletest
                SYSTEM
                GIT_REPOSITORY https://github.com/google/googletest.git
                GIT_TAG v1.17.0
                GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(googletest)
    endif ()

    #
    # GFLAGS
    #
    # Only the examples parse flags. gflags comes from the system (libgflags-dev
    # in the toolchain image); it never reaches the library.
    if (TINYIO_BUILD_EXAMPLES)
        # gflags 2.2 only creates the namespaced target (gflags::gflags) when
        # this is set before find_package.
        set(GFLAGS_USE_TARGET_NAMESPACE TRUE)
        find_package(gflags REQUIRED)
    endif ()
endfunction()

# tinyio::liburing. tinyio needs liburing >= 2.9 (io_uring_prep_ftruncate) and
# distributions lag, so the default is to build a pinned tag as a static
# library inside the build tree. TINYIO_FETCH_LIBURING=OFF takes the system one
# through pkg-config instead.
#
# The pinned copy is downloaded and configured while CMake configures, and
# only liburing.a is left for the build. liburing's ./configure is what
# generates part of its headers (compat.h, io_uring_version.h), so this way
# <liburing.h> resolves right after a fresh configure: an IDE indexing the
# project does not have to wait for a first build.
function(tinyio_configure_liburing)
    if (NOT TINYIO_FETCH_LIBURING)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LibUring REQUIRED IMPORTED_TARGET GLOBAL liburing>=2.9)
        add_library(tinyio::liburing INTERFACE IMPORTED GLOBAL)
        target_link_libraries(tinyio::liburing INTERFACE PkgConfig::LibUring)
        return ()
    endif ()

    FetchContent_Declare(
            liburing
            GIT_REPOSITORY https://github.com/axboe/liburing.git
            GIT_TAG liburing-2.9
            GIT_SHALLOW TRUE)
    # liburing has no CMakeLists.txt, so this only downloads it.
    FetchContent_MakeAvailable(liburing)

    set(_src "${liburing_SOURCE_DIR}")
    set(_lib "${_src}/src/liburing.a")
    # A shared libtinyio.so absorbs liburing.a, which then has to be PIC.
    if (TINYIO_BUILD_SHARED)
        set(_cflags "LIBURING_CFLAGS=-fPIC")
    else ()
        set(_cflags "")
    endif ()

    find_program(TINYIO_MAKE NAMES gmake make REQUIRED)

    # Start over when the compilers or the flags change: ./configure records
    # the compilers in config-host.mak, and a liburing.a built without -fPIC
    # cannot go into a shared library. The stamp is also a dependency of the
    # build step below, so liburing.a is rebuilt along with it.
    set(_stamp "${_src}/.tinyio-configured")
    set(_want "${CMAKE_C_COMPILER};${CMAKE_CXX_COMPILER};${_cflags}")
    set(_have "")
    if (EXISTS "${_stamp}")
        file(READ "${_stamp}" _have)
    endif ()
    if (NOT _have STREQUAL _want)
        message(STATUS "Configuring liburing")
        if (EXISTS "${_src}/config-host.mak")
            execute_process(COMMAND "${TINYIO_MAKE}" -C src clean
                    WORKING_DIRECTORY "${_src}" OUTPUT_QUIET)
        endif ()
        execute_process(
                COMMAND ./configure --cc=${CMAKE_C_COMPILER} --cxx=${CMAKE_CXX_COMPILER}
                WORKING_DIRECTORY "${_src}"
                RESULT_VARIABLE _rc
                OUTPUT_QUIET)
        if (NOT _rc EQUAL 0)
            message(FATAL_ERROR "liburing ./configure failed (${_rc})")
        endif ()
        file(WRITE "${_stamp}" "${_want}")
    endif ()

    add_custom_command(
            OUTPUT "${_lib}"
            COMMAND "${TINYIO_MAKE}" -C src -j4 ${_cflags} liburing.a
            DEPENDS "${_stamp}"
            WORKING_DIRECTORY "${_src}"
            COMMENT "Building liburing.a"
            VERBATIM)
    add_custom_target(tinyio_liburing_build DEPENDS "${_lib}")

    add_library(tinyio::liburing STATIC IMPORTED GLOBAL)
    set_target_properties(tinyio::liburing PROPERTIES
            IMPORTED_LOCATION "${_lib}"
            INTERFACE_INCLUDE_DIRECTORIES "${_src}/src/include")
    add_dependencies(tinyio::liburing tinyio_liburing_build)
endfunction()
