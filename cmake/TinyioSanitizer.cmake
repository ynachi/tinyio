add_library(tinyio_sanitizer_flags INTERFACE)

if(TINYIO_ENABLE_ASAN)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tinyio_sanitizer_flags INTERFACE -fsanitize=address
                                                             -fno-omit-frame-pointer)
    target_link_options(tinyio_sanitizer_flags INTERFACE -fsanitize=address)
    message(STATUS "Address Sanitizer enabled")
  else()
    message(WARNING "Address Sanitizer is only supported for GCC and Clang compilers")
  endif()
endif()

if(TINYIO_ENABLE_UBSAN)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tinyio_sanitizer_flags INTERFACE -fsanitize=undefined
                                                             -fno-omit-frame-pointer)
    target_link_options(tinyio_sanitizer_flags INTERFACE -fsanitize=undefined)
    message(STATUS "Undefined Behavior Sanitizer enabled")
  else()
    message(WARNING "Undefined Behavior Sanitizer is only supported for GCC and Clang compilers"
    )
  endif()
endif()

if(TINYIO_ENABLE_TSAN)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tinyio_sanitizer_flags INTERFACE -fsanitize=thread)
    target_link_options(tinyio_sanitizer_flags INTERFACE -fsanitize=thread)
    message(STATUS "Thread Sanitizer enabled")
  else()
    message(WARNING "Thread Sanitizer is only supported for GCC and Clang compilers")
  endif()
endif()
