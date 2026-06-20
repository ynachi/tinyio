#!/bin/bash
set -e

echo "=========================================================="
echo " Benchmarking tinyio (03_http_services)"
echo "=========================================================="
~/bench/tinyio/build_release/examples/03_http_services &
TINYIO_PID=$!
sleep 2

# We use 8080 because that's the hardcoded port in 03_http_services.cpp
oha -m GET -z 15s -c 1000 http://127.0.0.1:8080/ > tinyio_example_results.txt
kill $TINYIO_PID
sleep 2

echo "=========================================================="
echo " Benchmarking helio (echo_server matching 03_http_services)"
echo "=========================================================="
~/bench/helio/build-opt/echo_server --http_port=8082 &
HELIO_PID=$!
sleep 2

oha -m GET -z 15s -c 1000 http://127.0.0.1:8082/ > helio_example_results.txt
kill $HELIO_PID
sleep 2

echo "Benchmark complete!"
cat tinyio_example_results.txt
echo "----------------------------------------------------------"
cat helio_example_results.txt
