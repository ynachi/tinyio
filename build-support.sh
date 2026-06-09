#!/bin/bash

set -e

# Detect OS
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS=$ID
    OS_LIKE=$ID_LIKE
else
    echo "Cannot detect OS. /etc/os-release not found."
    exit 1
fi

echo "Detected OS: $OS (like $OS_LIKE)"

if [ "$EUID" -ne 0 ]; then
    SUDO="sudo"
else
    SUDO=""
fi

is_debian_like() {
    [[ "$OS" == "debian" || "$OS" == "ubuntu" || "$OS_LIKE" == *"debian"* || "$OS_LIKE" == *"ubuntu"* ]]
}

is_fedora_like() {
    [[ "$OS" == "fedora" || "$OS" == "centos" || "$OS" == "rhel" || "$OS_LIKE" == *"fedora"* || "$OS_LIKE" == *"rhel"* ]]
}

if is_debian_like; then
    echo "Updating apt cache and installing packages for Debian/Ubuntu-like OS..."
    $SUDO apt-get update
    $SUDO apt-get install -y \
        build-essential \
        cmake \
        ninja-build \
        pkg-config \
        liburing-dev \
        libboost-context-dev \
        libboost-dev \
        clang \
        gcc \
        g++ \
        sudo \
        libssl-dev \
        git \
        lld

    echo "Installation complete on Debian/Ubuntu-like system."

elif is_fedora_like; then
    echo "Installing packages for Fedora/RHEL-like OS..."
    $SUDO dnf install -y \
        gcc \
        gcc-c++ \
        clang \
        cmake \
        ninja-build \
        pkgconf \
        pkgconf-pkg-config \
        liburing-devel \
        boost-devel \
        openssl-devel \
        git \
        lld
        
    echo "Installation complete on Fedora/RHEL-like system."

else
    echo "Unsupported OS: $OS. Please install dependencies manually."
    exit 1
fi
