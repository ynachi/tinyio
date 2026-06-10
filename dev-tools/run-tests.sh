#!/usr/bin/env bash
# Entrypoint script executed inside the test container.
# Env vars (all optional, override from `docker compose run -e`):
#   TIO_SANITIZER  — address | thread | undefined | none (default: none)
#   TEST_LABEL     — ctest label filter (default: unit)

set -euo pipefail

SANITIZER="${TIO_SANITIZER:-none}"
LABEL="${TEST_LABEL:-unit}"

cd /workspace

echo "==> Configuring (sanitizer=${SANITIZER})"
cmake --preset dev --fresh \
  -G Ninja \
  -DCMAKE_ASM_COMPILER=clang \
  -DTIO_SANITIZER="$SANITIZER" \
  -DTIO_BUILD_DEMOS=OFF \
  -DTIO_BUILD_BITCASK=OFF \
  -DTIO_BUILD_BENCHMARK=OFF

echo "==> Building"
cmake --build --preset dev

echo "==> Running tests (label=${LABEL})"
ctest --preset dev -L "$LABEL"
