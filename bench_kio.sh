#!/bin/bash
set -e

STACKLESS_BIN=$(find ./build -name "kio_http_bench" | head -n 1)
STACKFUL_BIN=$(find ./build -name "kio_fiber_http_bench" | head -n 1)

echo "=========================================================="
echo " Benchmarking Stackless (kio_http_bench)"
echo "=========================================================="

$STACKLESS_BIN --port=8081 --workers=1 &
STACKLESS_PID=$!
sleep 1

oha -m GET -z 60s -c 1500 http://127.0.0.1:8081/

kill $STACKLESS_PID
sleep 1

echo ""
echo "=========================================================="
echo " Benchmarking Stackful (kio_fiber_http_bench)"
echo "=========================================================="

$STACKFUL_BIN --port=8082 --workers=1 &
STACKFUL_PID=$!
sleep 1

oha -m GET -z 60s -c 1500 http://127.0.0.1:8082/

kill $STACKFUL_PID
sleep 1
