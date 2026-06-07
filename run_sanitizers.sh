#!/bin/bash
set -e
echo "=== Running ASAN ==="
ssh 192.168.1.155 'cd ~/codes/linio && rm -rf build_asan && mkdir build_asan && cd build_asan && cmake -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" -DTIO_BUILD_TESTS=OFF -DTIO_BUILD_DEMOS=OFF .. && make tinyio_disk_bench tinyio_http_bench -j4 && ./benchmarks/cpp/tinyio_disk_bench --bytes=10M && (./benchmarks/cpp/tinyio_http_bench --port=8080 > http.log 2>&1 & pid=$!; sleep 1; oha -m GET http://127.0.0.1:8080/ -z 5s -c 50; kill $pid; wait $pid || true; cat http.log)' || echo "ASAN FAILED"

echo "=== Running TSAN ==="
ssh 192.168.1.155 'cd ~/codes/linio && rm -rf build_tsan && mkdir build_tsan && cd build_tsan && cmake -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" -DCMAKE_C_FLAGS="-fsanitize=thread -g" -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" -DTIO_BUILD_TESTS=OFF -DTIO_BUILD_DEMOS=OFF .. && make tinyio_disk_bench tinyio_http_bench -j4 && ./benchmarks/cpp/tinyio_disk_bench --bytes=10M && (./benchmarks/cpp/tinyio_http_bench --port=8080 > http.log 2>&1 & pid=$!; sleep 1; oha -m GET http://127.0.0.1:8080/ -z 5s -c 50; kill $pid; wait $pid || true; cat http.log)' || echo "TSAN FAILED"

echo "=== Running Valgrind ==="
ssh 192.168.1.155 'cd ~/codes/linio/build_bench && valgrind --leak-check=full ./benchmarks/cpp/tinyio_disk_bench --bytes=10M' || echo "VALGRIND FAILED"
