// Gpu construction, teardown and the vertex-buffer / frame-slot lifecycle.
// Rendering lives in gpu_render.cpp, Vulkan setup in gpu_vulkan.cpp, the
// Vulkan<->CUDA interop in gpu_cuda.cpp and encoding in gpu_nvenc.cpp.
#include "render/gpu_internal.hpp"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>
Gpu::Gpu(int w, int h, const std::string &path) : width(w), height(h), pitch((w + 255) & ~255) {
  if (w < 64 || h < 64 || w % 2 || h % 2 || w > 1280 || h > 720)
    throw std::runtime_error("H264 level 3.1 requires even dimensions from 64x64 through 1280x720");
  // No catch-and-release here: every owner already constructed is freed by
  // ordinary unwinding in the order documented in gpu.hpp. Construction submits
  // no GPU work, so the destructor's synchronize is not needed on this path.
  initVulkan(path);
  initCuda();
  for (auto &s : slots)
    createSlot(s);
  initEncoder();
}
Gpu::~Gpu() {
  // Anything still in flight has to finish before the handles below disappear.
  if (cudaDevice >= 0)
    cudaSetDevice(cudaDevice);
  for (auto &s : slots)
    if (s.stream)
      cudaStreamSynchronize(s.stream);
  if (device)
    vkDeviceWaitIdle(device);
  // The members now release themselves in reverse declaration order: NVENC,
  // frame slots, CUDA pool, Vulkan device, Vulkan instance.
}
void Gpu::load(const std::vector<Vertex> &data) {
  VK(vkDeviceWaitIdle(device));
  vertices.reset();
  vertexMemory.reset();
  vertexCount = data.size();
  if (data.empty())
    throw std::runtime_error("empty triangle mesh");
  VkDeviceSize bytes = data.size() * sizeof(Vertex);
  // Host-visible staging, the device-local destination and the one-shot upload
  // resources. Declaring them as owners makes the failure path implicit: an
  // exception after this point unwinds them in the correct order without the
  // hand-written cleanup this function used to carry.
  VkMemoryOwner sourceMemory;
  VkBufferOwner source;
  VkMemoryOwner targetMemory;
  VkBufferOwner target;
  VkCommandBufferOwner upload;
  VkFenceOwner fence;
  auto buffer = [&](VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBufferOwner &out,
                    VkMemoryOwner &outMemory) {
    VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    b.size = bytes;
    b.usage = usage;
    VkBuffer raw{};
    VK(vkCreateBuffer(device, &b, nullptr, &raw));
    out.adopt(device, raw);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, raw, &req);
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    a.allocationSize = req.size;
    a.memoryTypeIndex = memoryType(req.memoryTypeBits, properties);
    VkDeviceMemory memory{};
    VK(vkAllocateMemory(device, &a, nullptr, &memory));
    outMemory.adopt(device, memory);
    VK(vkBindBufferMemory(device, raw, memory, 0));
  };
  // Upload once at model load. Draws must read device-local memory, not PCIe host memory.
  buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, source,
         sourceMemory);
  buffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, target, targetMemory);
  void *ptr;
  VK(vkMapMemory(device, sourceMemory, 0, bytes, 0, &ptr));
  memcpy(ptr, data.data(), bytes);
  vkUnmapMemory(device, sourceMemory);
  VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = commands;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer rawCommand{};
  VK(vkAllocateCommandBuffers(device, &alloc, &rawCommand));
  upload.adopt(device, commands, rawCommand);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK(vkBeginCommandBuffer(upload, &begin));
  VkBufferCopy copy{0, 0, bytes};
  vkCmdCopyBuffer(upload, source, target, 1, &copy);
  VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.buffer = target;
  barrier.size = VK_WHOLE_SIZE;
  vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                       0, 0, nullptr, 1, &barrier, 0, nullptr);
  VK(vkEndCommandBuffer(upload));
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence rawFence{};
  VK(vkCreateFence(device, &fi, nullptr, &rawFence));
  fence.adopt(device, rawFence);
  VkCommandBuffer command = upload;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &command;
  VK(vkQueueSubmit(queue, 1, &submit, fence));
  VK(vkWaitForFences(device, 1, &rawFence, VK_TRUE, UINT64_MAX));
  // Only once the copy has completed does this become the live vertex buffer.
  vertices = std::move(target);
  vertexMemory = std::move(targetMemory);
}
int Gpu::acquire() {
  for (int j = 0; j < SLOT_COUNT; j++) {
    int i = (nextSlot + j) % SLOT_COUNT;
    bool expected = false;
    if (slots[i].busy.compare_exchange_strong(expected, true, std::memory_order_acquire)) {
      nextSlot = (i + 1) % SLOT_COUNT;
      return i;
    }
  }
  return -1;
}
void Gpu::release(int i) {
  slots.at(i).busy.store(false, std::memory_order_release);
}
