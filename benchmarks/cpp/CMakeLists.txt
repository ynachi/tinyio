# The HTTP benchmark server `make bench` runs. Deliberately has no external
# dependency beyond tinyio itself, so the release preset can build it without
# fetching anything.
add_executable(tinyio_http_bench tinyio_http_bench.cpp)
target_link_libraries(tinyio_http_bench PRIVATE Tinyio::tinyio)

add_executable(tinyio_disk_bench tinyio_disk_bench.cpp)
target_link_libraries(tinyio_disk_bench PRIVATE Tinyio::tinyio)

add_executable(tinyio_disk_read_bench tinyio_disk_read_bench.cpp)
target_link_libraries(tinyio_disk_read_bench PRIVATE Tinyio::tinyio)

add_executable(tinyio_post_smoke tinyio_post_smoke.cpp)
target_link_libraries(tinyio_post_smoke PRIVATE Tinyio::tinyio)

add_executable(tinyio_dispatch_bench tinyio_dispatch_bench.cpp)
target_link_libraries(tinyio_dispatch_bench PRIVATE Tinyio::tinyio)

add_executable(context_switch_bench context_switch_bench.cpp)
target_link_libraries(context_switch_bench PRIVATE Tinyio::tinyio)

add_executable(sync_bench sync_bench.cpp)
target_link_libraries(sync_bench PRIVATE Tinyio::tinyio)

add_executable(try_macro_bench try_macro_bench.cpp)
target_link_libraries(try_macro_bench PRIVATE Tinyio::tinyio)

# gflags and Photon are NOT vendored or fetched by this project, so these
# targets are off by default: leaving them in the unconditional path made the
# whole benchmark directory fail to configure for anyone who had never enabled
# TINYIO_BUILD_BENCH before. Turn them on only in an environment where
# gflags::gflags and photon_static already exist.
option(TINYIO_BUILD_GFLAGS_BENCH "Build the gflags network benchmark" OFF)
if (TINYIO_BUILD_GFLAGS_BENCH)
    # gflags 2.2 only creates the namespaced target (gflags::gflags) when this
    # is set before find_package.
    set(GFLAGS_USE_TARGET_NAMESPACE TRUE)
    find_package(gflags REQUIRED)
    add_executable(net_perf net_perf.cpp)
    target_link_libraries(net_perf PRIVATE Tinyio::tinyio gflags::gflags)
endif ()

option(TINYIO_BUILD_PHOTON_BENCH "Build the Photon network benchmarks" OFF)
if (TINYIO_BUILD_PHOTON_BENCH)
    add_executable(net_perf_photon net_perf_photon.cpp)
    target_link_libraries(net_perf_photon PRIVATE gflags::gflags photon_static)

    add_executable(photon_disk_bench photon_disk_bench.cpp)
    target_link_libraries(photon_disk_bench PRIVATE photon_static)
endif ()
