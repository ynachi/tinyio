function(tinyio_configure_sanitizers)
    add_library(tinyio-sanitizers INTERFACE)
    add_library(tinyio::sanitizers ALIAS tinyio-sanitizers)

    if (TINYIO_USE_MIMALLOC AND NOT TINYIO_SANITIZER STREQUAL "none")
        message(FATAL_ERROR "mimalloc replaces the allocator the sanitizers need; use one or the other")
    endif ()

    # Sanitizers apply to every target, fetched dependencies included, so one
    # build tree is instrumented consistently. mimalloc must stay off with them.
    # ---------------------------------------------------------------------------
    if (TINYIO_SANITIZER STREQUAL "address")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
        add_link_options(-fsanitize=address,undefined)
    elseif (TINYIO_SANITIZER STREQUAL "thread")
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
        # GCC's ThreadSanitizer does not implement atomic_thread_fence and warns
        # on every use of it. The vendored concurrent queue uses fences for its
        # SPMC fast path, so silence that one diagnostic.
        if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            add_compile_options(-Wno-tsan)
        endif ()
    elseif (NOT TINYIO_SANITIZER STREQUAL "none")
        message(FATAL_ERROR "TINYIO_SANITIZER must be none, address or thread")
    endif ()
endfunction()
