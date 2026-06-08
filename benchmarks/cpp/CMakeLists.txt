#add_executable(net_perf net_perf.cpp)
#target_link_libraries(net_perf PRIVATE kio_lib gflags::gflags)

function(add_sanitizers target_name)
    if (CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(${target_name} PRIVATE -fsanitize=thread -g)
        target_link_options(${target_name} PRIVATE -fsanitize=thread)
    endif ()
endfunction()

add_executable(net_perf_photon net_perf_photon.cpp)
target_link_libraries(net_perf_photon PRIVATE gflags::gflags photon_static)
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(net_perf_photon PRIVATE -fsanitize=thread -g)
    target_link_options(net_perf_photon PRIVATE -fsanitize=thread)
endif ()

add_executable(tinyio_disk_bench tinyio_disk_bench.cpp)
target_link_libraries(tinyio_disk_bench PRIVATE tinyio gflags::gflags)
add_sanitizers(tinyio_disk_bench)

add_executable(tinyio_http_bench tinyio_http_bench.cpp)
target_link_libraries(tinyio_http_bench PRIVATE tinyio gflags::gflags)
add_sanitizers(tinyio_http_bench)

add_executable(tinyio_post_smoke tinyio_post_smoke.cpp)
target_link_libraries(tinyio_post_smoke PRIVATE tinyio gflags::gflags)
add_sanitizers(tinyio_post_smoke)

add_executable(tinyio_dispatch_bench tinyio_dispatch_bench.cpp)
target_link_libraries(tinyio_dispatch_bench PRIVATE tinyio gflags::gflags)
add_sanitizers(tinyio_dispatch_bench)
