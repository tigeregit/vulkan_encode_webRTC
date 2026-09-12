#!/usr/bin/env bash
set -euo pipefail
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential cmake ninja-build curl \
 libvulkan-dev vulkan-tools vulkan-validationlayers glslang-tools libassimp-dev \
 libglm-dev nlohmann-json3-dev libx11-dev libxext-dev libxdamage-dev libxfixes-dev \
 libxrandr-dev libgbm-dev libdrm-dev libglib2.0-dev pkg-config ffmpeg coturn
