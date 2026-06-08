FROM ubuntu:26.04

ARG DEBIAN_FRONTEND=noninteractive

# ── Step 1: minimal prerequisites for the LLVM apt installer ──────────────
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        wget \
        gnupg \
        ca-certificates \
        lsb-release \
        software-properties-common \
 && rm -rf /var/lib/apt/lists/*

# ── Step 2: install clang-22 via the official LLVM apt script ─────────────
# The script auto-detects the Ubuntu codename and configures the right repo.
RUN wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh \
 && bash /tmp/llvm.sh 22 \
 && rm /tmp/llvm.sh \
 && rm -rf /var/lib/apt/lists/*

# ── Step 3: remaining toolchain and system deps ───────────────────────────
# All C++ library deps (GTest, Abseil, CRC32C, mimalloc, gflags) are fetched
# by CMake FetchContent at configure time and cached in the build volume.
# Ubuntu 26.04 ships liburing 2.14 natively — no need to build from source.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        # lld shipped separately from the llvm.sh core install
        lld-22 \
        # libstdc++ 15 comes from GCC 15
        gcc-15 \
        g++-15 \
        # Build system
        cmake \
        ninja-build \
        pkg-config \
        # io_uring 2.14 — matches what tinyio is built against
        liburing-dev \
        # OpenSSL 3
        libssl-dev \
        # FetchContent needs git to clone dep repos
        git \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace
