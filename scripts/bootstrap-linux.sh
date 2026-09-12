#!/usr/bin/env bash
set -euo pipefail
# Ubuntu 22.04 x86_64. CUDA toolkit + host NVIDIA driver must already exist.
root=$(cd "$(dirname "$0")/.." && pwd)
deps=${VIEWER_DEPS:-"$root/deps"}
mkdir -p "$deps/headers" "$deps/clang" "$deps/libcxx"
fetch() {
 local url=$1 output=$2 expected=$3
 if [[ ! -f "$output" ]]; then curl --fail --location --retry 3 "$url" -o "$output.part"; mv "$output.part" "$output"; fi
 printf '%s  %s\n' "$expected" "$output" | sha256sum --check --status
}
fetch https://github.com/shiguredo-webrtc-build/webrtc-build/releases/download/m152.7977.0.3/webrtc.ubuntu-22.04_x86_64.tar.gz "$deps/webrtc.tar.gz" f49ab6546b9e19ba081055b237afdeb80d51798484bb70fa4e4e342bb3c13f71
fetch https://commondatastorage.googleapis.com/chromium-browser-clang/Linux_x64/clang-llvmorg-24-init-7747-g62397f8b-27.tar.xz "$deps/clang.tar.xz" 83f4a4ea3292bfc236d85575fa7af27d39180d88534bc3117b73ad1df34bb959
fetch 'https://chromium.googlesource.com/external/github.com/llvm/llvm-project/libcxx.git/+archive/5abc7f839700f0f17338434e1c1c6a8c87c00c11/include.tar.gz' "$deps/libcxx-include.tar.gz" d9b3b0fde676d3e26ad5d47ce3ce17d2514a3d047b2178502189f31212511818
fetch https://raw.githubusercontent.com/FFmpeg/nv-codec-headers/n13.0.19.0/include/ffnvcodec/nvEncodeAPI.h "$deps/headers/nvEncodeAPI.h" 4fe4094541ef0f8a13249d97a8692dc5f835a6e9dd42eeadb3e2f7321d54dc7e
fetch https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.7/httplib.h "$deps/headers/httplib.h" 4770f8ea3d3fcd27b0b713c783d9b53bce3baddbd5adad13714011df96142526
[[ -f "$deps/webrtc/lib/libwebrtc.a" ]] || tar xzf "$deps/webrtc.tar.gz" -C "$deps"
[[ -f "$deps/clang/bin/clang++" ]] || tar xf "$deps/clang.tar.xz" -C "$deps/clang"
[[ -f "$deps/libcxx/vector" ]] || tar xzf "$deps/libcxx-include.tar.gz" -C "$deps/libcxx"
cmake -S "$root" -B "$root/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
 -DCMAKE_CUDA_COMPILER="${CUDACXX:-/usr/local/cuda/bin/nvcc}" \
 -DCMAKE_CUDA_ARCHITECTURES='86-real;86-virtual' \
 -DWEBRTC_ROOT="$deps/webrtc" -DRTC_CLANG="$deps/clang/bin/clang++" \
 -DLIBCXX_INCLUDE="$deps/libcxx" -DTHIRD_PARTY_HEADERS="$deps/headers" \
 -DBUILD_CONTAINER_GPU_FILTER="${BUILD_CONTAINER_GPU_FILTER:-OFF}"
cmake --build "$root/build" -j "${BUILD_JOBS:-8}"
