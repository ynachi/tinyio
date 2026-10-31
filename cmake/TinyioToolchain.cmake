# Build tools and toolchain probes. Included from the top-level CMakeLists.txt;
# the tool settings run at include time so they land in the directory scope
# every target is defined in.

# ccache and mold only when tinyio is the project being built: a parent
# project that pulls tinyio in picks its own launcher and linker.
if (PROJECT_IS_TOP_LEVEL)
    find_program(TINYIO_CCACHE ccache)
    if (TINYIO_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
        set(CMAKE_C_COMPILER_LAUNCHER "${TINYIO_CCACHE}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${TINYIO_CCACHE}")
    endif ()
    find_program(TINYIO_MOLD mold)
    if (TINYIO_MOLD)
        add_link_options(-fuse-ld=mold)
    endif ()
endif ()

if (TINYIO_CLANG_TIDY)
    find_program(TINYIO_CLANG_TIDY_EXE NAMES clang-tidy)
    if (TINYIO_CLANG_TIDY_EXE)
        set(CMAKE_CXX_CLANG_TIDY "${TINYIO_CLANG_TIDY_EXE}")
        message(STATUS "clang-tidy enabled: ${TINYIO_CLANG_TIDY_EXE}")
    else ()
        message(WARNING "clang-tidy requested but not found")
    endif ()
endif ()

# tinyio_configure_stacktrace(<target>)
#
# easylog prints a stack trace when a fatal record is logged. It prefers
# std::stacktrace (C++23) and falls back to YLT's own b_stacktrace helper
# when the standard library does not offer it.
#
# Some toolchains ship the <stacktrace> header without the matching runtime
# support. On a box with GCC 15 headers and a GCC 16 runtime, every
# translation unit that reaches easylog's fatal path fails to link with
#
#   undefined reference to `std::stacktrace_entry::_Info::_M_populate(long)'
#   undefined reference to `std::__stacktrace_impl::_S_current(...)'
#
# The probe below compiles *and links* a program that uses std::stacktrace,
# so it catches that class of breakage and not just a missing header. When
# the probe fails, __cpp_lib_stacktrace is defined to 0 on <target>, which is
# exactly the condition easylog (and YLT's ff_ptr) test before switching to
# ylt::util::b_stacktrace_get_string(). That fallback is header-only and
# needs no libstdc++ support at all. The definition is PUBLIC: easylog sits
# in tinyio's public headers, so consumers need it as well.
include(CheckCXXSourceCompiles)

function(tinyio_configure_stacktrace target)
    # The answer depends on the headers and on the runtime the probe links
    # against, so re-probe whenever the compiler changes.
    set(_compiler_id "${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}")
    if (DEFINED TINYIO_HAS_STD_STACKTRACE AND
            NOT "${TINYIO_STACKTRACE_COMPILER}" STREQUAL "${_compiler_id}")
        unset(TINYIO_HAS_STD_STACKTRACE CACHE)
    endif ()

    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    check_cxx_source_compiles("
#include <stacktrace>
int main() {
  const auto trace = std::stacktrace::current();
  return trace.size() > 0 ? 0 : 0;
}
" TINYIO_HAS_STD_STACKTRACE)

    set(TINYIO_STACKTRACE_COMPILER "${_compiler_id}" CACHE INTERNAL
            "compiler the std::stacktrace probe ran with")

    if (TINYIO_HAS_STD_STACKTRACE)
        message(STATUS "std::stacktrace: usable")
    else ()
        message(STATUS "std::stacktrace: not linkable, easylog uses YLT's "
                "b_stacktrace instead")
        target_compile_definitions(${target} PUBLIC "__cpp_lib_stacktrace=0")
    endif ()
endfunction()
