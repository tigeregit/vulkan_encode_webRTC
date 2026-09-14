// CUDA context creation and the per-slot Vulkan<->CUDA interop: exporting the
// Vulkan RGBA allocation and semaphores, importing them into CUDA and building
// the pool-backed NV12 input.
#include "render/gpu.hpp"
#include "render/platform.hpp"
#include <cstddef>
#include <cstdint>
#include <stdexcept>
void Gpu::initCuda() {
  CU(cudaSetDevice(cudaDevice));
  int supported = 0;
  CU(cudaDeviceGetAttribute(&supported, cudaDevAttrMemoryPoolsSupported, cudaDevice));
  if (!supported)
    throw std::runtime_error("CUDA memory pools required");
  cudaMemPoolProps p{};
  p.allocType = cudaMemAllocationTypePinned;
#ifndef _WIN32
  p.handleTypes = cudaMemHandleTypePosixFileDescriptor;
#endif
  p.location.type = cudaMemLocationTypeDevice;
  p.location.id = cudaDevice;
  CU(cudaMemPoolCreate(pool.out(), &p));
  uint64_t threshold = UINT64_MAX;
  CU(cudaMemPoolSetAttribute(pool, cudaMemPoolAttrReleaseThreshold, &threshold));
}
void Gpu::createSlot(Slot &s) {
  // The stream comes first because every CUDA object below is associated with
  // it, and the slot's teardown order guarantees it outlives them.
  CU(cudaStreamCreateWithFlags(s.stream.out(), cudaStreamNonBlocking));
  image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        true, s.image, s.memory, s.view);
  image(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, false, s.depth,
        s.depthMemory, s.depthView);
  VkImageView attachments[] = {s.view, s.depthView};
  VkFramebufferCreateInfo fc{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fc.renderPass = renderpass;
  fc.attachmentCount = 2;
  fc.pAttachments = attachments;
  fc.width = width;
  fc.height = height;
  fc.layers = 1;
  VK(vkCreateFramebuffer(device, &fc, nullptr, s.framebuffer.out(device)));
  VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ca.commandPool = commands;
  ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ca.commandBufferCount = 1;
  VkCommandBuffer rawCommand{};
  VK(vkAllocateCommandBuffers(device, &ca, &rawCommand));
  s.command.adopt(device, commands, rawCommand);
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  VK(vkCreateFence(device, &fi, nullptr, s.fence.out(device)));
  CU(cudaEventCreate(s.start.out()));
  CU(cudaEventCreate(s.end.out()));
  s.nv12.set_stream(s.stream);
  CU(cudaMallocFromPoolAsync(s.nv12.out(), size_t(pitch) * height * 3 / 2, pool, s.stream));
  CU(cudaStreamSynchronize(s.stream));
  // Export the Vulkan allocation and adopt it as CUDA external memory, then
  // map it into a surface for the colour-conversion kernel.
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device, s.image, &req);
  cudaExternalMemory_t external{};
  import_cuda_memory(device, s.memory, req.size, &external);
  s.external.adopt(external);
  cudaExternalMemoryMipmappedArrayDesc ad{};
  ad.formatDesc = cudaCreateChannelDesc<uchar4>();
  ad.extent = make_cudaExtent(width, height, 0);
  ad.numLevels = 1;
  ad.flags = cudaArrayColorAttachment | cudaArraySurfaceLoadStore;
  cudaMipmappedArray_t mip{};
  CU(cudaExternalMemoryGetMappedMipmappedArray(&mip, s.external, &ad));
  s.mip.adopt(mip);
  cudaArray_t arr;
  CU(cudaGetMipmappedArrayLevel(&arr, s.mip, 0));
  cudaResourceDesc rd{};
  rd.resType = cudaResourceTypeArray;
  rd.res.array.array = arr;
  cudaSurfaceObject_t surface{};
  CU(cudaCreateSurfaceObject(&surface, &rd));
  s.surface.adopt(surface);
  // ready: Vulkan -> CUDA after the RGBA image is written.
  // released: CUDA -> Vulkan before the slot is reused.
  auto semaphore = [&](VkSemaphoreOwner &sem, CudaExternalSemaphoreOwner &cs) {
    VkExportSemaphoreCreateInfo es{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    es.handleTypes = SEM_HANDLE;
    VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sc.pNext = &es;
    VK(vkCreateSemaphore(device, &sc, nullptr, sem.out(device)));
    cudaExternalSemaphore_t raw{};
    import_cuda_semaphore(device, sem, &raw);
    cs.adopt(raw);
  };
  semaphore(s.ready, s.cudaReady);
  semaphore(s.released, s.cudaReleased);
  s.encodeInput = s.nv12.get();
}
