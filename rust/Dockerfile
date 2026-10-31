# Toolchain image for tinyio-rs: stable for the build, nightly for Miri and sanitizers.
# Run with:  --security-opt seccomp=unconfined --ulimit memlock=-1   (see Makefile)
FROM rust:1.97-bookworm

RUN rustup toolchain install nightly --profile minimal -c miri,rust-src \
 && rustup component add clippy rustfmt

WORKDIR /work
