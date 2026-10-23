include(FetchContent)
include(ExternalProject)

function(tinyio_configure_dependencies)
    # google/crc32c 1.1.2 still declares a pre-3.5 CMake policy baseline.
    # CMake 4 removed that compatibility mode, so provide the supported floor
    # without modifying the dependency's source tree.
    if (CMAKE_VERSION VERSION_GREATER_EQUAL 4.0 AND
            NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    endif ()

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

    # openssl, min 3 required
    find_package(OpenSSL 3 REQUIRED)
    message(STATUS "OpenSSL: ${OPENSSL_VERSION}")
    find_package(Threads REQUIRED)

    #
    # LIBURING
    #
    # tinyio needs liburing >= 2.9 (io_uring_prep_ftruncate). Distributions lag, so the
    # default is to build a pinned tag as a static library inside the build tree.
    set(TINYIO_LIBURING_TAG liburing-2.9)
    if (TINYIO_FETCH_LIBURING)
        set(_lu_src ${CMAKE_BINARY_DIR}/_deps/liburing)
        set(_lu_lib ${_lu_src}/src/liburing.a)
        ExternalProject_Add(liburing_ext
                GIT_REPOSITORY https://github.com/axboe/liburing.git
                GIT_TAG ${TINYIO_LIBURING_TAG}
                GIT_SHALLOW ON
                SOURCE_DIR ${_lu_src}
                BUILD_IN_SOURCE ON
                UPDATE_DISCONNECTED ON
                CONFIGURE_COMMAND ./configure --cc=${CMAKE_C_COMPILER} --cxx=${CMAKE_CXX_COMPILER}
                BUILD_COMMAND make -C src -j4 liburing.a
                INSTALL_COMMAND ""
                BUILD_BYPRODUCTS ${_lu_lib})
        file(MAKE_DIRECTORY ${_lu_src}/src/include)
        add_library(tinyio_liburing STATIC IMPORTED GLOBAL)
        set_target_properties(tinyio_liburing PROPERTIES
                IMPORTED_LOCATION ${_lu_lib}
                INTERFACE_INCLUDE_DIRECTORIES ${_lu_src}/src/include)
        add_dependencies(tinyio_liburing liburing_ext)
        add_library(tinyio::liburing ALIAS tinyio_liburing)
    else ()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LibUring REQUIRED IMPORTED_TARGET liburing>=2.9)
        add_library(tinyio::liburing ALIAS PkgConfig::LibUring)
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
    # ABSEIL
    #
    message(STATUS "Setting up Google Abseil")

    FetchContent_Declare(
            abseil-cpp
            SYSTEM
            GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
            GIT_TAG 20260107.1
            EXCLUDE_FROM_ALL                  # Don't build Abseil's tests/docs by default
    )

    # Configure Abseil build options BEFORE MakeAvailable
    set(ABSL_BUILD_TESTING OFF CACHE BOOL "Build Abseil tests" FORCE)
    set(ABSL_BUILD_DOCS OFF CACHE BOOL "Build Abseil docs" FORCE)
    set(ABSL_USE_EXTERNAL_GOOGLETEST ON CACHE BOOL "Use system googletest if needed" FORCE)
    set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "Don't install Abseil" FORCE)  # Optional

    FetchContent_MakeAvailable(abseil-cpp)

    #
    # BOOST
    #
    # tinyio's fibers are stackful: boost::context provides the fiber stacks
    # and the jump_fcontext trampoline.
    include(TinyioBoost)
    tinyio_find_boost(REQUIRED COMPONENTS context)

    #
    # BENCHMARK
    #
    if (TINYIO_BUILD_BENCH)
        set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
        set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        FetchContent_Declare(
                benchmark
                SYSTEM
                GIT_REPOSITORY https://github.com/google/benchmark.git
                GIT_TAG v1.9.5
                GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(benchmark)

        FetchContent_Declare(
                photon
                SYSTEM
                GIT_REPOSITORY https://github.com/ynachi/PhotonLibOS.git
                GIT_TAG main
        )
        FetchContent_MakeAvailable(photon)

        # Photon does not build clean on GCC 15: it needs cstdint force-included
        # and its warnings silenced. Only the photon targets get this; applying
        # it globally would mask warnings in tinyio's own sources.
        # photon_obj is the object library that actually compiles the sources;
        # photon_static / photon_shared only collect its objects.
        foreach (photon_target photon_obj photon_static photon_shared)
            if (TARGET ${photon_target})
                target_compile_options(${photon_target} PRIVATE
                        -w -include cstdint)
            endif ()
        endforeach ()
    endif ()
endfunction()
