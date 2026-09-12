# Dragon local encoding benchmark

2026-09-12, NVIDIA RTX 3080, Stanford Dragon (870,615 rendered triangles), H.264
2 Mbps, camera yaw changes 0.02 radians each frame. No network or browser in the
measured path. Existing synchronous render/convert/encode loop, no FPS pacing.
Thirty warmup frames and model loading are excluded; output flush is included.

| Resolution | Before, local H.264 | After, local H.264 | After, discard output |
| --- | ---: | ---: | ---: |
| 640x360 | 47.16 FPS | 1144.94 FPS | 1181.00 FPS |
| 1280x720 | 45.17 FPS | 696.28 FPS | 700.67 FPS |

Before: 300 measured frames. After: 1800 measured frames. These are short-run
observations, not guaranteed hardware limits. Local writes use the OS page cache;
no fsync or durable-storage throughput guarantee is implied. H.264 timing remains
configured for the viewer; measured FPS is processing throughput, not playback FPS.
The earlier approximately 24 FPS process-wall-time result included loading and
must not be used as a steady-state baseline.

720p after: mean Vulkan GPU interval 0.192 ms, CUDA conversion 0.0098 ms,
NVENC host interval 0.946 ms, total frame 1.436 ms. Encoding is now the largest
measured stage; remaining time includes submission, synchronization and output.
Synchronization intervals overlap GPU work and must not be added to it twice.

The change uploads vertex data through a temporary staging buffer to DEVICE_LOCAL
memory once per model load. The original HOST_VISIBLE allocation caused very slow
vertex fetch on this server. No geometry, normals, colors, winding, blending or
per-frame CUDA allocation behavior changes. Staging resources are freed after a
transfer fence and an explicit transfer-to-vertex-input memory barrier.

Run with the server's existing Vulkan/CUDA environment:

```sh
./build/gpu_smoke models/stanford_dragon.ply build/shaders /tmp/dragon.h264 1280 720 1800
./build/gpu_smoke models/stanford_dragon.ply build/shaders /dev/null 1280 720 1800
ctest --test-dir build --output-on-failure
```

The browser viewer retains its 30 FPS scheduler. Windows compilation/runtime has
not been tested; the upload path uses portable Vulkan core commands.
