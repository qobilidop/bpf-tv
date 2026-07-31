# Reproducible build environment for bpf-tv.
# Base: official Ubuntu LTS, same family as alive2's CI (ubuntu-24.04).
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential gcc-14 g++-14 \
    cmake ninja-build re2c ccache \
    git python3 ca-certificates curl unzip \
    z3 libz3-dev zlib1g-dev \
    default-jre-headless \
    && rm -rf /var/lib/apt/lists/*

# ANTLR4 for alive2's aslp bridge (build-time only). The complete jar and the
# C++ runtime must be the SAME version, so install both from antlr.org rather
# than mixing apt's runtime with a downloaded jar.
ARG ANTLR_VERSION=4.13.2
RUN mkdir -p /opt/antlr \
    && curl -fsSL "https://www.antlr.org/download/antlr-${ANTLR_VERSION}-complete.jar" \
         -o /opt/antlr/antlr-complete.jar \
    && curl -fsSL "https://www.antlr.org/download/antlr4-cpp-runtime-${ANTLR_VERSION}-source.zip" \
         -o /tmp/antlr4-cpp.zip \
    && mkdir /tmp/antlr4-cpp && cd /tmp/antlr4-cpp && unzip -q /tmp/antlr4-cpp.zip \
    && cmake -S . -B build -G Ninja \
         -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_INSTALL_PREFIX=/opt/antlr \
         -DANTLR4_INSTALL=ON \
         -DANTLR_BUILD_CPP_TESTS=OFF \
         -DANTLR_BUILD_SHARED=ON \
    && ninja -C build install \
    && rm -rf /tmp/antlr4-cpp /tmp/antlr4-cpp.zip

ENV CC=gcc-14 CXX=g++-14 \
    ANTLR4_JAR=/opt/antlr/antlr-complete.jar \
    ANTLR4_PREFIX=/opt/antlr \
    LD_LIBRARY_PATH=/opt/antlr/lib
