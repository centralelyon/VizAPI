#!/usr/bin/env bash
set -euo pipefail

LOOM_REF="1e4757838104d1e4d22186c9b77d5fc4b98681a0"
BUILD_JOBS="${BUILD_JOBS:-2}"

apt-get update
apt-get install -y --no-install-recommends \
    ca-certificates \
    git \
    cmake \
    g++ \
    make \
    libglpk-dev \
    coinor-libcbc-dev \
    libprotobuf-dev \
    protobuf-compiler \
    libzip-dev

rm -rf /var/lib/apt/lists/*

git clone https://github.com/ad-freiburg/loom.git /tmp/loom

cd /tmp/loom
git checkout "$LOOM_REF"
git submodule update --init --recursive

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target octi -j "$BUILD_JOBS"

cp build/octi /usr/local/bin/octi

cd /
rm -rf /tmp/loom
