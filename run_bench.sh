#!/bin/bash
set -e

# Run tinyio benchmark
echo "=========================================================="
echo " Benchmarking tinyio"
echo "=========================================================="
~/bench/tinyio/build_release/benchmarks/cpp/tinyio_http_bench --port=8081 --workers=4 &
TINYIO_PID=$!
sleep 2
oha -m GET -z 30s -c 1000 http://127.0.0.1:8081/ > tinyio_oha_results.txt
kill $TINYIO_PID
sleep 2

# Run helio benchmark
echo "=========================================================="
echo " Benchmarking helio"
echo "=========================================================="
~/bench/helio/build-opt/examples/echo_server --http_port=8082 --threads=4 &
HELIO_PID=$!
sleep 2
oha -m GET -z 30s -c 1000 http://127.0.0.1:8082/ > helio_oha_results.txt
kill $HELIO_PID
sleep 2

echo "Benchmark complete!"
