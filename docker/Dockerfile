# Nubix — PS5 build image: Ubuntu 24.04 + clang-18 + pacbrew ps5-payload-dev v0.40.2.
# Copyright (C) 2026 Nubix contributors
# SPDX-License-Identifier: GPL-3.0-only
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  clang-18 lld-18 llvm-18 llvm-18-dev make cmake pkg-config python3 python3-pyelftools git wget ca-certificates unzip nasm perl file xz-utils \
  && rm -rf /var/lib/apt/lists/*
RUN wget -qO /tmp/p.tgz https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/v0.40.2/ps5-payload-dev.tar.gz && tar -xzf /tmp/p.tgz -C / && rm /tmp/p.tgz
ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
ENV LLVM_CONFIG=/usr/bin/llvm-config-18
