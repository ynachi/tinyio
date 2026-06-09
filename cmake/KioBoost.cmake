# tinyio/cmake/FindBoost.cmake
#
# Do not provide a project-local FindBoost.cmake.
#
# CMake already ships a maintained FindBoost module, and modern Boost
# installations may also provide BoostConfig.cmake. A local FindBoost.cmake in
# this directory would shadow CMake's built-in module because tinyio prepends
# cmake/ to CMAKE_MODULE_PATH. That makes Boost discovery less portable, not
# more portable.
#
# This wrapper gives tinyio one place to define Boost policy while still using the
# upstream discovery logic:
#
# - prefer BoostConfig.cmake when the installation provides it
# - fall back to CMake's built-in FindBoost module
# - require imported component targets such as Boost::context

include_guard(GLOBAL)

function(kio_find_boost)
    set(options REQUIRED)
    set(oneValueArgs VERSION)
    set(multiValueArgs COMPONENTS)
    cmake_parse_arguments(KIO_BOOST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if (NOT KIO_BOOST_COMPONENTS)
        set(KIO_BOOST_COMPONENTS context)
    endif ()

    set(_version_arg "")
    if (KIO_BOOST_VERSION)
        set(_version_arg "${KIO_BOOST_VERSION}")
    endif ()

    find_package(Boost ${_version_arg} CONFIG QUIET COMPONENTS ${KIO_BOOST_COMPONENTS})

    set(_missing_targets "")
    foreach (_component IN LISTS KIO_BOOST_COMPONENTS)
        if (NOT TARGET Boost::${_component})
            list(APPEND _missing_targets Boost::${_component})
        endif ()
    endforeach ()

    if ((NOT Boost_FOUND) OR _missing_targets)
        find_package(Boost ${_version_arg} MODULE QUIET COMPONENTS ${KIO_BOOST_COMPONENTS})
    endif ()

    set(_missing_targets "")
    foreach (_component IN LISTS KIO_BOOST_COMPONENTS)
        if (NOT TARGET Boost::${_component})
            list(APPEND _missing_targets Boost::${_component})
        endif ()
    endforeach ()

    if ((NOT Boost_FOUND) OR _missing_targets)
        string(REPLACE ";" ", " _components "${KIO_BOOST_COMPONENTS}")
        string(REPLACE ";" ", " _targets "${_missing_targets}")

        if (KIO_BOOST_REQUIRED)
            message(FATAL_ERROR
                    "Boost discovery failed for components: ${_components}. "
                    "Missing imported targets: ${_targets}. "
                    "Install Boost.Context or set Boost_ROOT/BOOST_ROOT.")
        endif ()
    endif ()

    if (Boost_FOUND)
        message(STATUS "Boost: ${Boost_VERSION}")
    endif ()
endfunction()
