# Vulkan / CUDA / NVENC / Google WebRTC 远程 3D Viewer

C++20 + CMake。服务器解析、保存和渲染模型，浏览器接收 H.264 视频，并通过 WebRTC DataChannel 控制相机。首版为**一个活动会话、一个共享场景、固定 30 fps、固定分辨率**。默认 1280×720；首版 H.264 level 3.1 协商限制最大 1280×720，可通过参数降低分辨率。

## 已实现

- Headless Vulkan 1.2 三角网格渲染、深度测试、基础光照、顶点 RGBA。
- 以设备 UUID 匹配 Vulkan 与 CUDA，检查外部图像/信号量导出能力。
- 4 个固定帧槽：RGBA 图像、深度图、命令缓冲、外部信号量、CUDA surface、CUDA non-blocking stream、计时 events、NV12 输入和 NVENC 注册/码流资源。
- Vulkan 与 CUDA 共用原始 RGBA 显存，以外部 binary semaphore 和 `VK_QUEUE_FAMILY_EXTERNAL` 转移所有权。
- CUDA kernel 将 RGBA 转成 BT.709 limited-range NV12；专用 `cudaMemPool_t` 在初始化时通过 `cudaMallocFromPoolAsync` 分配全部 NV12 槽。帧循环不分配/释放显存，不使用 stream 0。
- NVENC H.264 baseline，P1 / ultra-low-latency，关闭 B 帧和 lookahead。响应 WebRTC 码率调整及 IDR 请求。
- Google libwebrtc 自定义 `VideoEncoderFactory` / `VideoEncoder` / native `VideoFrameBuffer`。没有软件编码回退，没有 I420 原始帧回读。
- 浏览器鼠标旋转、平移、滚轮缩放；触屏单指旋转、双指平移/捏合；模型选择、重置视角、连接/断开、延迟分布与 JSON 导出。
- 单会话标识防止其他标签页误断开当前会话；无控制/心跳 30 秒自动回收。

## 拷贝边界：不要把整个流程称为绝对零拷贝

```text
服务器模型 -> Vulkan GPU 顶点缓冲（加载时一次上传）
                     |
              Vulkan RGBA render target
                     | 外部内存 + 外部信号量，无图像拷贝
              CUDA surface -> NV12 pool slot（颜色转换写入）
                     |
             NVENC registered CUDA input
                     |
              压缩 H.264 -> Google libwebrtc -> SRTP -> 浏览器 video
```

**严格模式（默认）**直接注册 pool NV12 指针。若驱动支持，不需要额外 raw-frame GPU memcpy；是否支持以 `nvEncRegisterResource` 的实际结果为准。

**当前 RTX 3080 / 驱动 580.142 的兼容模式**：NVENC 拒绝 pool 指针，返回 `NV_ENC_ERR_RESOURCE_REGISTER_FAILED (23)`。设置 `VIEWER_NVENC_LEGACY_INPUT=1` 后，仅在初始化时额外分配固定 NVENC 输入槽。CUDA pool 转换结果通过同一显式 stream **每帧一次 GPU→GPU NV12 memcpy** 写入该槽。没有原始图像 GPU→CPU/CPU→GPU 往返，也没有逐帧显存分配。严格模式不会静默切换。

NVENC 码流在主机可读内存中交给 `EncodedImageBuffer`，Google WebRTC/网络栈和浏览器各自仍有压缩数据缓冲、解码和合成开销。驱动内部的 NV12 布局转换和 resource mapping 也不属于应用能保证消除的操作。网页 debug JSON 显示 `rawFrameHostCopies` 和 `gpuCopies`。

Linux NVENC 输出使用同步 lock，等待发生在 WebRTC 编码线程；CUDA 转换/拷贝为异步 stream 操作。帧槽生命周期由 native VideoFrameBuffer 引用计数保护，未释放槽不会被重写。槽耗尽时丢弃新渲染机会，不无限排队。

## PLY 数据约定

支持 `ascii`、`binary_little_endian`、`binary_big_endian`，版本 1.0，属性顺序可变。

```text
element vertex N
property float x
property float y
property float z
property float nx
property float ny
property float nz
property uchar r
property uchar g
property uchar b
property uchar a
element face M
property list uchar int vertex_indices
```

- `red green blue alpha` 与 `r g b a` 均可。整型颜色按 0–255，浮点颜色按 0–1。
- 也支持 `property uint rgba`，约定 **0xAARRGGBB**；`uint rgb` 按 0x00RRGGBB。其他打包字节顺序需先转换。
- 每个 face 必须恰好 3 个有效顶点索引。无三角片的点云 PLY 会明确报错。
- 法线存在时保留方向并归一化；缺失/零法线回退到面法线。模型统一居中缩放，但不改拓扑和颜色。
- RGBA 中的 alpha 参与基础混合；当前没有透明三角形排序，交叠透明表面不保证正确，推荐不透明网格。
- `models/colored_torus.ply` 为本项目生成的二进制 PLY 样例，含 XYZ、法线、RGBA 和 2,304 个三角片。
- OBJ/STL/glTF/GLB 通过 Assimp 解析。当前不加载纹理、不播放动画；材质使用基础颜色。导入器限制附属文件只能位于模型库目录内。
- 单文件上限 256 MiB，展开后最多 500 万三角形。库目录只列顶层文件；未知文件名、越界索引、截断数据、非有限坐标和越界路径会被拒绝。

## Ubuntu 22.04 x86_64 构建

需要已有 NVIDIA 驱动和 CUDA Toolkit。建议 CUDA 12/13；本次验证为 CUDA 13.0。CUDA pool API 最低 11.2，但新 GPU / NVENC SDK 13 还要求相应新驱动。本项目启动时检查 NVENC API 版本。

```bash
sudo scripts/install-ubuntu-deps.sh
# 国内服务器先设置可用代理；SSH 反向隧道必须保持连接。
export http_proxy=http://127.0.0.1:7897
export https_proxy="$http_proxy"
VIEWER_DEPS=/root/autodl-tmp/viewer-deps scripts/bootstrap-linux.sh
scripts/run.sh
```

依赖下载固定版本并检查 SHA-256；CMake 本身不在配置时联网。Google libwebrtc 的 `std::__Cr` ABI 与 CUDA/Assimp 的系统 C++ ABI 通过独立共享库的 C 接口隔离，避免混用 STL 对象。不要随意替换 libwebrtc 版本而不同时适配 API/头文件/工具链。

主要固定依赖：

| 组件 | 版本 |
|---|---|
| Google libwebrtc 预编译包 | shiguredo m152.7977.0.3 |
| Google WebRTC 源提交 | `6f37672d358475cd17544121a12494da454d85fb` |
| libc++ 源提交 | `5abc7f839700f0f17338434e1c1c6a8c87c00c11` |
| Chromium Clang | `llvmorg-24-init-7747-g62397f8b-27` |
| NVENC 头文件 | nv-codec-headers n13.0.19.0 |
| cpp-httplib | v0.18.7 |

依赖包的 `VERSIONS` / `NOTICE` 保留在下载目录。编译器与 libc++ 包缺少的配置头位于 `cmake/`，来源见 [第三方说明](docs/THIRD_PARTY.md)。

## 当前临时服务器运行

服务器到期：**2026-09-14 14:04，北京时间**。代码和 Git 历史保留在本机。

本机启动 SSH 隧道（已存在同端口隧道时不要重复启动）：

```bash
scripts/connect.sh
```

服务器端：

```bash
cd /root/vulkan_encode_webRTC
VIEWER_ENV=/root/autodl-tmp/viewer-runtime/viewer.env scripts/run.sh
```

本机浏览器访问 **http://127.0.0.1:8080**，点击“连接服务器”。临时服务器采用本机 `3478/TCP` 经 SSH 转发到服务器的 coturn，媒体仍是 WebRTC/SRTP，额外经过 TURN/TCP 和 SSH，存在 TCP 队头阻塞。这条测试路径的延迟不能直接当作公网 UDP 性能。

运行凭证、TURN 配置及日志在 `/root/autodl-tmp/viewer-runtime/`，未提交 Git。服务默认只绑定 `127.0.0.1`；本项目没有公网登录/权限系统。实际公网部署应配置 HTTPS、访问控制和可达的 TURN 服务，避免直接暴露本演示控制端点。

两个容器兼容选项：

1. `VK_ICD_FILENAMES=/root/vulkan_encode_webRTC/scripts/nvidia-egl-icd.json`：使用 NVIDIA EGL ICD；本容器 GLX ICD 初始化失败。
2. CMake `-DBUILD_CONTAINER_GPU_FILTER=ON` 并以 `LD_PRELOAD=.../build/libcontainer_gpu_filter.so` 启动：修复 570/580 多 GPU 宿主机、单 GPU 容器的 NVENC 枚举问题。仅过滤没有可访问字符设备的 GPU，使用 580.142 的正式 RM ABI 映射，不猜 PCI ID、不改变宿主机、不增加设备访问权限。此选项默认关闭，升级到修复后的驱动时应移除。

## 延迟与带宽如何解释

| 阶段 | 方法 / 局限 |
|---|---|
| 控制上传 | DataChannel ping/pong 对齐单调时钟；最小 RTT 样本假设路径对称，显示 ±RTT/2 不确定度，属于估算 |
| 控制处理 / 等待 | 服务器收到控制到渲染线程应用最新相机状态，包含解析与帧调度等待 |
| Vulkan 渲染 | Vulkan GPU timestamp query |
| CUDA 转换 | 显式 stream 上的 CUDA events；不含兼容 memcpy |
| NVENC 编码 | 编码线程提交到码流 lock 可读，CPU 单调时钟 |
| 编码后到接收 | 编码完成到浏览器 `receiveTime`；时钟对齐估算，包含发送队列、分包和网络 |
| 解码 | `requestVideoFrameCallback.processingDuration`，不支持则保持无样本；`getStats.totalDecodeTime` 另列累计均值 |
| 接收到呈现 | `receiveTime` 到 `expectedDisplayTime`，包括解码、抖动缓冲、合成 |
| 控制到呈现 | 视频中 24-bit 帧编号标记对应最新控制 seq，在浏览器同一时钟下计算首个可见反馈；是呈现时间估计，不是显示器光子测量 |
| 带宽 | inbound-rtp bytesReceived 的时间差分，表示接收视频 RTP 字节率，不含 TURN/SSH 等外层总线路开销 |

所有分布保留最近 512 个有效样本并显示样本数；各阶段分位数不能相加。相机更新使用最新绝对状态，来不及进入帧的中间控制会合并，不把未显示事件当作低延迟样本。角落标记每个 bit 占 8×8 像素，调试 canvas 仅读取视频中 192×8 像素区域。

## 验证

```bash
# 按实际环境设置 VK_ICD_FILENAMES / LD_PRELOAD / VIEWER_NVENC_LEGACY_INPUT
ctest --test-dir build --output-on-failure
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation build/gpu_smoke \
 models/colored_torus.ply build/shaders /tmp/viewer.h264
ffprobe -v error -count_frames -show_entries stream=codec_name,width,height,nb_read_frames /tmp/viewer.h264
scripts/profile.sh /tmp/viewer-profile
```

测试覆盖 ASCII/LE/BE PLY、RGBA/打包色、法线、非法索引/截断，及 OBJ/PLY 各 90 帧 Vulkan→CUDA→NVENC 编码。`gpu_smoke` 循环使用全部 4 个槽。Nsight 可核对帧处理期间显存分配、stream 和 memcpy；NVENC 内部 CUDA kernel 也会被记录。

## Windows / RTX 30、40、50 系列

渲染与编码核心具有 FD/Win32 HANDLE 分支；CMake Windows 分支使用同版本 `webrtc.lib`、MSVC STL、Win32 系统库。使用 vcpkg 或等效方式提供 Vulkan、Assimp、GLM、nlohmann-json，并安装 Vulkan SDK 的 `glslangValidator` 和 CUDA Toolkit。

```powershell
cmake -S . -B build -A x64 `
  -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DWEBRTC_ROOT=C:/deps/webrtc `
  -DTHIRD_PARTY_HEADERS=C:/deps/headers `
  -DCMAKE_CUDA_ARCHITECTURES="86-real;86-virtual"
cmake --build build --config Release
```

默认包含 sm_86 cubin 和 compute_86 PTX，Ada/Blackwell 可以走新驱动的 PTX JIT；可用匹配 Toolkit 显式加入 89/120 架构。H.264 是三代 GPU 的共同编码路径，未依赖 RTX 3080 不具备的 AV1 编码。

**实机验证范围只有 Linux RTX 3080。Windows、RTX 40/50、其他驱动的 pool 指针注册、外部 HANDLE 权限和编译链接仍需对应机器验证，不能据此声称已全部兼容。** 当前没有 AV1/HEVC 协商、多用户隔离或自适应分辨率；需要新功能时应保持 native frame 生命周期与所有权同步约束。
