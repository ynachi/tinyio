# ============================================================================
# Development Targets
# ============================================================================

# Find clang-format
find_program(CLANG_FORMAT_EXE NAMES clang-format)

# Find clang-tidy
find_program(CLANG_TIDY_EXE NAMES clang-tidy)

# Collect all source files for formatting/linting
file(GLOB_RECURSE ALL_SOURCE_FILES
        ${CMAKE_SOURCE_DIR}/tinyio/*.cpp
        ${CMAKE_SOURCE_DIR}/tinyio/*.h
        ${CMAKE_SOURCE_DIR}/bitcask/src/*.cpp
        ${CMAKE_SOURCE_DIR}/bitcask/include/*.h
        ${CMAKE_SOURCE_DIR}/examples/*.cpp
        ${CMAKE_SOURCE_DIR}/tests/*.cpp
        ${CMAKE_SOURCE_DIR}/benchmarks/cpp/*.cpp
)

# Format target - applies clang-format
if (CLANG_FORMAT_EXE)
    add_custom_target(format
            COMMAND ${CLANG_FORMAT_EXE} -i ${ALL_SOURCE_FILES}
            COMMENT "Running clang-format on all source files"
            VERBATIM
    )

    add_custom_target(format-check
            COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror ${ALL_SOURCE_FILES}
            COMMENT "Checking code formatting (no changes)"
            VERBATIM
    )
    message(STATUS "Added targets: format, format-check")
else ()
    message(WARNING "clang-format not found - format targets not available")
endif ()

# Tidy target - runs clang-tidy on all sources
if (CLANG_TIDY_EXE)
    add_custom_target(tidy
            COMMAND ${CLANG_TIDY_EXE}
            -p ${CMAKE_BINARY_DIR}
            ${ALL_SOURCE_FILES}
            COMMENT "Running clang-tidy on all source files"
            VERBATIM
    )

    add_custom_target(tidy-fix
            COMMAND ${CLANG_TIDY_EXE}
            -p ${CMAKE_BINARY_DIR}
            --fix
            ${ALL_SOURCE_FILES}
            COMMENT "Running clang-tidy with automatic fixes"
            VERBATIM
    )
    message(STATUS "Added targets: tidy, tidy-fix")
else ()
    message(WARNING "clang-tidy not found - tidy targets not available")
endif ()
