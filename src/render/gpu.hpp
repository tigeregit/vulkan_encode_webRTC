#pragma once
#include "core/common.hpp"
#include "core/mesh.hpp"
#include "render/handles.hpp"
#include <array>
#include <atomic>
#include <cuda_runtime.h>
#include <memory>
#include <string>
#include <vector>
// Opaque NVENC session state (see render/gpu_internal.hpp). Forward declared so
// this header does not pull in nvEncodeAPI.h or cuda.h.
struct NvencState;
// One slot of the frame pipeline. Member order is deliberate: C++ destroys
// members in reverse declaration order, which yields the required teardown
// sequence of CUDA surface -> imported memory -> pool buffer -> stream ->
// Vulkan views/framebuffers -> image -> image memory -> command buffer.
struct Slot {
  std::atomic<bool> busy{false};
  VkCommandBufferOwner command;
  VkMemoryOwner memory, depthMemory;
  VkImageOwner image, depth;
  VkImageViewOwner view, depthView;
  VkFramebufferOwner framebuffer;
  VkFenceOwner fence;
  VkSemaphoreOwner ready, released;
  CudaStreamOwner stream;
  CudaEventOwner start, end;
  // Freed on `stream`, so the stream outlives it.
  CudaPoolBuffer nv12;
  // Allocated only when NVENC refuses the pool pointer.
  CudaAllocation legacyBuffer;
  CudaExternalSemaphoreOwner cudaReady, cudaReleased;
  CudaExternalMemoryOwner external;
  CudaMipmappedArrayOwner mip;
  CudaSurfaceOwner surface;
  // Non-owning view of the buffer NVENC is registered against: nv12 in strict
  // mode, legacyBuffer in the driver compatibility mode.
  unsigned char *encodeInput{};
  bool legacyInput = false;
  bool used = false;
  FrameTiming timing;
};
class Gpu {
private:
  // Device-level owners, listed in the reverse of their teardown order; see
  // the note on ~Gpu(). `instance` is destroyed last, `device` after all the
  // objects created from it.
  VkInstanceOwner instance;
  VkDeviceOwner device;
  VkCommandPoolOwner commands;
  VkRenderPassOwner renderpass;
  VkPipelineLayoutOwner layout;
  VkPipelineOwner pipeline;
  VkQueryPoolOwner queries;
  // Memory before the buffer that is bound to it, so the buffer is destroyed
  // first.
  VkMemoryOwner vertexMemory;
  VkBufferOwner vertices;
  CudaMemPoolOwner pool;
  VkQueue queue{};
  VkPhysicalDevice physical{};
  uint32_t family = 0, nextSlot = 0;
  float timestampPeriod = 1;
  size_t vertexCount = 0;
  uint32_t memoryType(uint32_t, VkMemoryPropertyFlags);
  void initVulkan(const std::string &);
  void initCuda();
  void initEncoder();
  void createSlot(Slot &);
  void image(VkFormat, VkImageUsageFlags, bool, VkImageOwner &, VkMemoryOwner &, VkImageViewOwner &);

public:
  // Frame slots sit between `pool` and `nv`: NVENC resources reference the slot
  // buffers, so they must be unregistered before the slots are freed, and the
  // pool must outlive the slot allocations.
  std::array<Slot, SLOT_COUNT> slots;
  Gpu(int width, int height, const std::string &shaders);
  ~Gpu();
  Gpu(const Gpu &) = delete;
  Gpu &operator=(const Gpu &) = delete;
  void load(const std::vector<Vertex> &vertices);
  int acquire();
  void release(int index);
  void render(int index, const Camera &, FrameTiming);
  int encode(int index, bool key, unsigned bitrate, const unsigned char **data, size_t *length,
             int *isKey);
  void unlock(int index);
  int width, height, pitch, cudaDevice = -1;
  std::string deviceName;

private:
  // Declared last so it is destroyed first.
  std::unique_ptr<NvencState> nv;
};
