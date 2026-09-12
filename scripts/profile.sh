#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
output=${1:-/tmp/viewer-profile}
nsys profile --trace=cuda,vulkan,nvtx --sample=none --cpuctxsw=none \
 -o "$output" "$root/build/gpu_smoke" "$root/models/colored_torus.ply" "$root/build/shaders" "$output.h264"
nsys stats --report cuda_api_sum,cuda_gpu_mem_time_sum,cuda_gpu_kern_sum "$output.nsys-rep"
