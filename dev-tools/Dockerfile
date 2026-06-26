FROM ubuntu:26.04

ARG DEBIAN_FRONTEND=noninteractive

# Update apt and install sudo/wget/ca-certificates so we can run the support script
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        sudo \
        wget \
        ca-certificates \
        lsb-release \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

# Copy and run the build support script
COPY build-support.sh /tmp/build-support.sh
RUN chmod +x /tmp/build-support.sh && /tmp/build-support.sh \
 && rm /tmp/build-support.sh
