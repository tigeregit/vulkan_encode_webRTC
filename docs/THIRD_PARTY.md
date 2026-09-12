# References and third-party materials

The viewer implementation is original project code informed by these upstream examples and APIs:

- [NVIDIA CUDA vulkanImageCUDA](https://github.com/NVIDIA/cuda-samples/tree/master/cpp/5_Domain_Specific/vulkanImageCUDA): device UUID matching, external memory and semaphore import/export, FD/Win32 ownership.
- [NVIDIA NvEncoderCuda](https://github.com/NVIDIA/video-sdk-samples/blob/master/Samples/NvCodec/NvEncoder/NvEncoderCuda.cpp) and [NvEncoder](https://github.com/NVIDIA/video-sdk-samples/blob/master/Samples/NvCodec/NvEncoder/NvEncoder.cpp): CUDA resource registration and NVENC session/bitstream lifecycle.
- [NVENC SDK 13.0 programming guide](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html).
- [Google WebRTC upstream](https://webrtc.googlesource.com/src/+/6f37672d358475cd17544121a12494da454d85fb/) and [shiguredo binary distribution](https://github.com/shiguredo-webrtc-build/webrtc-build/releases/tag/m152.7977.0.3). The latter distributes the real Google libwebrtc library; it is not libdatachannel or a replacement WebRTC stack. Google WebRTC's BSD license/PATENTS and dependency notices apply to redistributed binaries; retain the package NOTICE.
- [NVIDIA container-toolkit issue 1249](https://github.com/NVIDIA/nvidia-container-toolkit/issues/1249) and [device-plugin issue 1282 discussion](https://github.com/NVIDIA/k8s-device-plugin/issues/1282#issuecomment-4400059356), including [flexgrip's workaround](https://github.com/flexgrip/nvidia-gpu-enumeration). Our optional `container_gpu_filter.c` implements a narrower, process-local filter using the correct [NVIDIA 580.142 ctrl0000gpu.h ABI](https://github.com/NVIDIA/open-gpu-kernel-modules/blob/580.142/src/common/sdk/nvidia/inc/ctrl/ctrl0000/ctrl0000gpu.h). It does not use the upstream workaround's guessed PCI mapping fallback.
- [CUDA stream-ordered memory allocator](https://developer.nvidia.com/blog/using-cuda-stream-ordered-memory-allocator-part-1/).

Downloaded dependencies remain outside Git and keep their upstream licenses:

| Dependency | License / source |
|---|---|
| nv-codec-headers | MIT, NVIDIA copyright; license included in `nvEncodeAPI.h` |
| cpp-httplib | MIT, license included in `httplib.h` |
| Assimp | BSD-3-Clause |
| GLM | MIT / Happy Bunny |
| nlohmann/json | MIT |
| Clang / libc++ | Apache-2.0 WITH LLVM-exception |
| Google WebRTC | BSD-3-Clause, PATENTS, bundled third-party notices |
| shiguredo build tooling | Apache-2.0 |

`cmake/__config_site` and `cmake/__assertion_handler` are Chromium libc++ vendor configuration headers retrieved from Chromium's main branch on 2026-09-12. The assertion header retains the LLVM copyright/license header. Sources: [config](https://chromium.googlesource.com/chromium/src/+/main/third_party/libc++/__config_site), [assertion handler](https://chromium.googlesource.com/chromium/src/+/main/buildtools/third_party/libc++/__assertion_handler). Applicable [Chromium BSD license](https://chromium.googlesource.com/chromium/src/+/main/LICENSE) and [LLVM license](https://llvm.org/LICENSE.txt).

The octahedron OBJ and colored torus PLY samples are generated specifically for this project; they are not downloaded model assets.
