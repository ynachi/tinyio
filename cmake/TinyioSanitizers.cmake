function(tinyio_configure_sanitizers)
    if (TINYIO_USE_MIMALLOC AND NOT TINYIO_SANITIZER STREQUAL "none")
        message(FATAL_ERROR "mimalloc replaces the allocator the sanitizers need; use one or the other")
    endif ()

    # Sanitizers apply to every target, fetched dependencies included, so one
    # build tree is instrumented consistently. That only holds because this
    # runs before tinyio_configure_dependencies(): add_compile_options() does
    # not reach targets that already exist. liburing is built by its own
    # Makefile and stays uninstrumented. mimalloc must stay off with them.
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
