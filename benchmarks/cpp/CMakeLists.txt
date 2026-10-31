# The HTTP benchmark server `make bench` runs. Deliberately has no external
# dependency beyond tinyio itself, so the release preset can build it without
# fetching anything.
add_executable(tinyio_http_bench tinyio_http_bench.cpp)
target_link_libraries(tinyio_http_bench PRIVATE tinyio::tinyio)

add_executable(tinyio_disk_bench tinyio_disk_bench.cpp)
target_link_libraries(tinyio_disk_bench PRIVATE tinyio::tinyio)

add_executable(tinyio_disk_read_bench tinyio_disk_read_bench.cpp)
target_link_libraries(tinyio_disk_read_bench PRIVATE tinyio::tinyio)

add_executable(tinyio_dispatch_bench tinyio_dispatch_bench.cpp)
target_link_libraries(tinyio_dispatch_bench PRIVATE tinyio::tinyio)

add_executable(context_switch_bench context_switch_bench.cpp)
target_link_libraries(context_switch_bench PRIVATE tinyio::tinyio)

add_executable(pingpong_bench pingpong_bench.cpp)
target_link_libraries(pingpong_bench PRIVATE tinyio::tinyio)

add_executable(sync_bench sync_bench.cpp)
target_link_libraries(sync_bench PRIVATE tinyio::tinyio)

add_executable(try_macro_bench try_macro_bench.cpp)
target_link_libraries(try_macro_bench PRIVATE tinyio::tinyio)

# Only tinyio's own benchmarks live here. The comparison benchmarks against
# other libraries (kio, asio, helio, photon, coro_io) were removed: AGENTS.md
# makes tinyio the baseline and forbids cross-library comparison arms, and
# most of them no longer compiled against the current API anyway.
