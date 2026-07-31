# Reproducible build environment for bpf-tv.
# Base: official Ubuntu LTS, same family as alive2's CI (ubuntu-24.04).
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential gcc-14 g++-14 \
    cmake ninja-build re2c ccache \
    git python3 ca-certificates curl unzip \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Z3, built from a pinned tag (apt's z3 is ancient and brew's is untested by
# alive2; a pinned source build is what Minotaur recommends for Alive2 use).
ARG Z3_VERSION=z3-4.15.4
RUN git clone --depth 1 --branch ${Z3_VERSION} https://github.com/Z3Prover/z3.git /tmp/z3 \
    && cmake -S /tmp/z3 -B /tmp/z3/build -G Ninja \
         -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_INSTALL_PREFIX=/opt/z3 \
         -DZ3_BUILD_TEST_EXECUTABLES=OFF \
    && ninja -C /tmp/z3/build install \
    && rm -rf /tmp/z3

ENV CC=gcc-14 CXX=g++-14 \
    Z3_PREFIX=/opt/z3 \
    PATH=/opt/z3/bin:$PATH \
    LD_LIBRARY_PATH=/opt/z3/lib
