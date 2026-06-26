# -----------------------------------------
# Common Build Functions
#------------------------------------------
function(tio_target_compile_options target_name)
    # Check if target is an INTERFACE library
    get_target_property(target_type ${target_name} TYPE)

    # Common flags for both Interface and Normal targets
    set(COMMON_COMPILE_FLAGS
            -Wall
            -Wextra
            -Wpedantic
            -Wno-gnu-statement-expression
            -Wno-missing-field-initializers

            #"-DLOG_BUILD_LEVEL=${LOG_BUILD_LEVEL}"

            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:Release>:-march=native>
    )

    if (target_type STREQUAL "INTERFACE_LIBRARY")
        target_compile_options(${target_name} INTERFACE ${COMMON_COMPILE_FLAGS})
    else ()
        target_compile_options(${target_name} PRIVATE ${COMMON_COMPILE_FLAGS})
    endif ()

endfunction()

# -----------------------------------------------------------------------------
# Function: tio_add_demo
# Usage: tio_add_demo(demo_name source.cpp TAGS core network LIBS <libs to link>)
# -----------------------------------------------------------------------------
function(tio_add_demo target_name source_file)
    # Parse Arguments (supports optional TAGS list)
    set(options "")
    set(oneValueArgs "")
    set(multiValueArgs TAGS LIBS)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # 2. Check Global Toggle
    if (NOT TIO_BUILD_DEMOS)
        return()
    endif ()

    # Check Tag Filtering
    # If TIO_DEMO_FILTER is set (and not "all"), we check if this demo has a matching tag.
    if (DEFINED TIO_DEMO_FILTER AND NOT "${TIO_DEMO_FILTER}" STREQUAL "all")
        set(MATCH_FOUND FALSE)
        foreach (tag IN LISTS ARG_TAGS)
            if ("${tag}" IN_LIST TIO_DEMO_FILTER)
                set(MATCH_FOUND TRUE)
                break()
            endif ()
        endforeach ()

        if (NOT MATCH_FOUND)
            return() # Skip this target
        endif ()
    endif ()

    # Create the Target (If we passed the checks)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE ${ARG_LIBS})
    tio_target_compile_options(${target_name})

    # Install to bin/examples
    install(TARGETS ${target_name}
            RUNTIME DESTINATION bin/examples
    )

    message(STATUS "Enabled Demo: ${target_name} [${ARG_TAGS}]")
endfunction()

set(TIO_SANITIZER "thread" CACHE STRING "Sanitizer to use (address, thread, undefined, none)")

function(tio_add_sanitizers target_name)
    if (TIO_SANITIZER STREQUAL "none")
        return()
    endif ()

    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        message(STATUS "🔍 ${TIO_SANITIZER} Sanitizer enabled on target ${target_name}")

        target_compile_options(${target_name}
                PRIVATE
                -fsanitize=${TIO_SANITIZER}
                -g -O1
                -fno-omit-frame-pointer
        )
        target_link_options(${target_name} PRIVATE -fsanitize=${TIO_SANITIZER})

        if (TIO_SANITIZER STREQUAL "thread")
            target_compile_definitions(${target_name} PUBLIC __SANITIZE_THREAD__=1)
        endif ()
    endif ()
endfunction()


# -----------------------------------------------------------------------------
# Function: tio_add_test
# Usage: tio_add_test(test_name source.cpp TAGS core network LIBS <libs to link>)
# -----------------------------------------------------------------------------
function(tio_add_test target_name source_file)
    # Parse Arguments (supports optional TAGS list)
    set(options "")
    set(oneValueArgs "")
    set(multiValueArgs TAGS LIBS)
    cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    # 2. Check Global Toggle
    if (NOT TIO_BUILD_TESTS)
        return()
    endif ()

    # Check Tag Filtering
    # If TIO_TEST_FILTER is set (and not "all"), we check if this demo has a matching tag.
    if (DEFINED TIO_TEST_FILTER AND NOT "${TIO_TEST_FILTER}" STREQUAL "all")
        set(MATCH_FOUND FALSE)
        foreach (tag IN LISTS ARG_TAGS)
            if ("${tag}" IN_LIST TIO_TEST_FILTER)
                set(MATCH_FOUND TRUE)
                break()
            endif ()
        endforeach ()

        if (NOT MATCH_FOUND)
            return() # Skip this target
        endif ()
    endif ()

    # Create the Target (If we passed the checks)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE ${ARG_LIBS})
    tio_target_compile_options(${target_name})
    # always enable sanitizer on debug builds
    tio_add_sanitizers(${target_name})

    string(REPLACE ";" "\\;" ESCAPED_TAGS "${ARG_TAGS}")
    gtest_discover_tests(${target_name} PROPERTIES LABELS "${ESCAPED_TAGS}")

    message(STATUS "Enabled Test: ${target_name} [${ARG_TAGS}]")
endfunction()
