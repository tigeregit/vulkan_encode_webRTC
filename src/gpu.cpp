#include "gpu.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <limits>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
extern "C" cudaError_t convert_nv12(cudaSurfaceObject_t, unsigned char *, int, int, int,
                                    cudaStream_t);
using PNVENCODEAPICREATEINSTANCE = NVENCSTATUS(NVENCAPI *)(NV_ENCODE_API_FUNCTION_LIST *);
using PNVENCODEAPIGETMAXSUPPORTEDVERSION = NVENCSTATUS(NVENCAPI *)(uint32_t *);
static void nvcheck(NVENCSTATUS s, const char *op) {
  if (s != NV_ENC_SUCCESS)
    throw std::runtime_error(std::string(op) + ": NVENC " + std::to_string(s));
}
#define NV(x) nvcheck((x), #x)
#ifdef _WIN32
constexpr auto MEM_HANDLE = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
constexpr auto SEM_HANDLE = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
constexpr auto MEM_HANDLE = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
constexpr auto SEM_HANDLE = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
Gpu::Gpu(int w, int h, const std::string &path) : width(w), height(h), pitch((w + 255) & ~255) {
  if (w < 64 || h < 64 || w % 2 || h % 2 || w > 1280 || h > 720)
    throw std::runtime_error("H264 level 3.1 requires even dimensions from 64x64 through 1280x720");
  try {
    initVulkan(path);
    initCuda();
    for (auto &s : slots)
      createSlot(s);
    initEncoder();
  } catch (...) {
    cleanup();
    throw;
  }
}
uint32_t Gpu::memoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
  VkPhysicalDeviceMemoryProperties p;
  vkGetPhysicalDeviceMemoryProperties(physical, &p);
  for (uint32_t i = 0; i < p.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (p.memoryTypes[i].propertyFlags & flags) == flags)
      return i;
  throw std::runtime_error("Vulkan memory type unavailable");
}
void Gpu::initVulkan(const std::string &path) {
  VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  ai.pApplicationName = "Vulkan remote viewer";
  ai.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &ai;
  VK(vkCreateInstance(&ci, nullptr, &instance));
  uint32_t n = 0;
  VK(vkEnumeratePhysicalDevices(instance, &n, nullptr));
  std::vector<VkPhysicalDevice> devices(n);
  VK(vkEnumeratePhysicalDevices(instance, &n, devices.data()));
  int nc = 0;
  CU(cudaGetDeviceCount(&nc));
  for (auto d : devices) {
    VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p.pNext = &id;
    vkGetPhysicalDeviceProperties2(d, &p);
    if (p.properties.vendorID != 0x10de)
      continue;
    for (int i = 0; i < nc; i++) {
      cudaDeviceProp cp{};
      CU(cudaGetDeviceProperties(&cp, i));
      if (!memcmp(cp.uuid.bytes, id.deviceUUID, 16)) {
        physical = d;
        cudaDevice = i;
        deviceName = p.properties.deviceName;
        timestampPeriod = p.properties.limits.timestampPeriod;
        break;
      }
    }
    if (physical)
      break;
  }
  if (!physical)
    throw std::runtime_error(
        "no NVIDIA Vulkan device with matching CUDA UUID (check container ICD)");
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, nullptr);
  std::vector<VkQueueFamilyProperties> qs(n);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &n, qs.data());
  bool found = false;
  for (uint32_t i = 0; i < n; i++)
    if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && qs[i].timestampValidBits) {
      family = i;
      found = true;
      break;
    }
  if (!found)
    throw std::runtime_error("graphics queue with timestamps unavailable");
  const char *extensions[] = {
#ifdef _WIN32
      VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME
#else
      VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME
#endif
  };
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> ex(n);
  vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, ex.data());
  for (auto e : extensions)
    if (std::none_of(ex.begin(), ex.end(), [&](auto &v) { return !strcmp(v.extensionName, e); }))
      throw std::runtime_error(std::string("missing extension ") + e);
  VkPhysicalDeviceExternalImageFormatInfo external{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
  external.handleType = MEM_HANDLE;
  VkPhysicalDeviceImageFormatInfo2 fmt{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
  fmt.pNext = &external;
  fmt.format = VK_FORMAT_R8G8B8A8_UNORM;
  fmt.type = VK_IMAGE_TYPE_2D;
  fmt.tiling = VK_IMAGE_TILING_OPTIMAL;
  fmt.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  VkExternalImageFormatProperties ep{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
  VkImageFormatProperties2 fp{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
  fp.pNext = &ep;
  VK(vkGetPhysicalDeviceImageFormatProperties2(physical, &fmt, &fp));
  if (!(ep.externalMemoryProperties.externalMemoryFeatures &
        VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT))
    throw std::runtime_error("RGBA image memory is not exportable");
  VkPhysicalDeviceExternalSemaphoreInfo si{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
  si.handleType = SEM_HANDLE;
  VkExternalSemaphoreProperties sp{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
  vkGetPhysicalDeviceExternalSemaphoreProperties(physical, &si, &sp);
  if (!(sp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT))
    throw std::runtime_error("external semaphore not exportable");
  float priority = 1;
  VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qc.queueFamilyIndex = family;
  qc.queueCount = 1;
  qc.pQueuePriorities = &priority;
  VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dc.queueCreateInfoCount = 1;
  dc.pQueueCreateInfos = &qc;
  dc.enabledExtensionCount = 2;
  dc.ppEnabledExtensionNames = extensions;
  VK(vkCreateDevice(physical, &dc, nullptr, &device));
  vkGetDeviceQueue(device, family, 0, &queue);
  VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pc.queueFamilyIndex = family;
  pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VK(vkCreateCommandPool(device, &pc, nullptr, &commands));
  VkAttachmentDescription a[2]{};
  a[0].format = VK_FORMAT_R8G8B8A8_UNORM;
  a[0].samples = VK_SAMPLE_COUNT_1_BIT;
  a[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  a[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  a[0].initialLayout = a[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  a[1].format = VK_FORMAT_D32_SFLOAT;
  a[1].samples = VK_SAMPLE_COUNT_1_BIT;
  a[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  a[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  a[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  a[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
      depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &color;
  sub.pDepthStencilAttachment = &depth;
  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dep.dstStageMask =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dep.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dep.dstAccessMask =
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rp.attachmentCount = 2;
  rp.pAttachments = a;
  rp.subpassCount = 1;
  rp.pSubpasses = &sub;
  rp.dependencyCount = 1;
  rp.pDependencies = &dep;
  VK(vkCreateRenderPass(device, &rp, nullptr, &renderpass));
  VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, 64};
  VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  lc.pushConstantRangeCount = 1;
  lc.pPushConstantRanges = &range;
  VK(vkCreatePipelineLayout(device, &lc, nullptr, &layout));
  auto shader = [&](const char *name) {
    std::ifstream f(path + "/" + name, std::ios::binary | std::ios::ate);
    if (!f)
      throw std::runtime_error("missing SPIR-V shader");
    size_t size = f.tellg();
    std::vector<uint32_t> code((size + 3) / 4);
    f.seekg(0);
    f.read((char *)code.data(), size);
    VkShaderModuleCreateInfo sc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sc.codeSize = size;
    sc.pCode = code.data();
    VkShaderModule m;
    VK(vkCreateShaderModule(device, &sc, nullptr, &m));
    return m;
  };
  VkShaderModule vs = shader("scene.vert.spv"), fs = shader("scene.frag.spv");
  VkPipelineShaderStageCreateInfo stages[2]{};
  for (auto &s : stages) {
    s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    s.pName = "main";
  }
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;
  VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription attrs[] = {
      {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
      {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)},
      {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)}};
  VkPipelineVertexInputStateCreateInfo vi{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = 1;
  vi.pVertexBindingDescriptions = &binding;
  vi.vertexAttributeDescriptionCount = 3;
  vi.pVertexAttributeDescriptions = attrs;
  VkPipelineInputAssemblyStateCreateInfo ia{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport viewport{0, 0, float(width), float(height), 0, 1};
  VkRect2D scissor{{0, 0}, {uint32_t(width), uint32_t(height)}};
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.pViewports = &viewport;
  vp.scissorCount = 1;
  vp.pScissors = &scissor;
  VkPipelineRasterizationStateCreateInfo rs{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = ds.depthWriteEnable = VK_TRUE;
  ds.depthCompareOp = VK_COMPARE_OP_LESS;
  VkPipelineColorBlendAttachmentState ba{};
  ba.colorWriteMask = 15;
  ba.blendEnable = VK_TRUE;
  ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  ba.colorBlendOp = VK_BLEND_OP_ADD;
  ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  ba.alphaBlendOp = VK_BLEND_OP_ADD;
  VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  bs.attachmentCount = 1;
  bs.pAttachments = &ba;
  VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.stageCount = 2;
  gp.pStages = stages;
  gp.pVertexInputState = &vi;
  gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vp;
  gp.pRasterizationState = &rs;
  gp.pMultisampleState = &ms;
  gp.pDepthStencilState = &ds;
  gp.pColorBlendState = &bs;
  gp.layout = layout;
  gp.renderPass = renderpass;
  VkResult result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline);
  vkDestroyShaderModule(device, vs, nullptr);
  vkDestroyShaderModule(device, fs, nullptr);
  VK(result);
  VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
  qi.queryCount = SLOT_COUNT * 2;
  VK(vkCreateQueryPool(device, &qi, nullptr, &queries));
}
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
  CU(cudaMemPoolCreate(&pool, &p));
  uint64_t threshold = UINT64_MAX;
  CU(cudaMemPoolSetAttribute(pool, cudaMemPoolAttrReleaseThreshold, &threshold));
  if (cuCtxGetCurrent(&context) != CUDA_SUCCESS || !context)
    throw std::runtime_error("CUDA primary context missing");
}
void Gpu::image(VkFormat format, VkImageUsageFlags usage, bool shared, VkImage &img,
                VkDeviceMemory &mem, VkImageView &view) {
  VkExternalMemoryImageCreateInfo ex{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  ex.handleTypes = MEM_HANDLE;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.pNext = shared ? &ex : nullptr;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = format;
  ci.extent = {uint32_t(width), uint32_t(height), 1};
  ci.mipLevels = ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = usage;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VK(vkCreateImage(device, &ci, nullptr, &img));
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device, img, &req);
  VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated.image = img;
  VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
  exp.handleTypes = MEM_HANDLE;
  exp.pNext = &dedicated;
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.pNext = shared ? static_cast<void *>(&exp) : nullptr;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VK(vkAllocateMemory(device, &alloc, nullptr, &mem));
  VK(vkBindImageMemory(device, img, mem, 0));
  VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vc.image = img;
  vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vc.format = format;
  vc.subresourceRange = {shared ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0,
                         1};
  VK(vkCreateImageView(device, &vc, nullptr, &view));
}
void Gpu::createSlot(Slot &s) {
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
  VK(vkCreateFramebuffer(device, &fc, nullptr, &s.framebuffer));
  VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ca.commandPool = commands;
  ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ca.commandBufferCount = 1;
  VK(vkAllocateCommandBuffers(device, &ca, &s.command));
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  VK(vkCreateFence(device, &fi, nullptr, &s.fence));
  CU(cudaStreamCreateWithFlags(&s.stream, cudaStreamNonBlocking));
  CU(cudaEventCreate(&s.start));
  CU(cudaEventCreate(&s.end));
  CU(cudaMallocFromPoolAsync((void **)&s.nv12, size_t(pitch) * height * 3 / 2, pool, s.stream));
  CU(cudaStreamSynchronize(s.stream));
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device, s.image, &req);
  cudaExternalMemoryHandleDesc md{};
  md.size = req.size;
  md.flags = cudaExternalMemoryDedicated;
#ifdef _WIN32
  VkMemoryGetWin32HandleInfoKHR mi{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
  mi.memory = s.memory;
  mi.handleType = MEM_HANDLE;
  HANDLE mh;
  VK(((PFN_vkGetMemoryWin32HandleKHR)vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR"))(
      device, &mi, &mh));
  md.type = cudaExternalMemoryHandleTypeOpaqueWin32;
  md.handle.win32.handle = mh;
  auto importResult = cudaImportExternalMemory(&s.external, &md);
  CloseHandle(mh);
  CU(importResult);
#else
  VkMemoryGetFdInfoKHR mi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
  mi.memory = s.memory;
  mi.handleType = MEM_HANDLE;
  int fd = -1;
  VK(((PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"))(device, &mi, &fd));
  md.type = cudaExternalMemoryHandleTypeOpaqueFd;
  md.handle.fd = fd;
  auto importResult = cudaImportExternalMemory(&s.external, &md);
  if (importResult != cudaSuccess)
    close(fd);
  CU(importResult);
#endif
  cudaExternalMemoryMipmappedArrayDesc ad{};
  ad.formatDesc = cudaCreateChannelDesc<uchar4>();
  ad.extent = make_cudaExtent(width, height, 0);
  ad.numLevels = 1;
  ad.flags = cudaArrayColorAttachment | cudaArraySurfaceLoadStore;
  CU(cudaExternalMemoryGetMappedMipmappedArray(&s.mip, s.external, &ad));
  cudaArray_t arr;
  CU(cudaGetMipmappedArrayLevel(&arr, s.mip, 0));
  cudaResourceDesc rd{};
  rd.resType = cudaResourceTypeArray;
  rd.res.array.array = arr;
  CU(cudaCreateSurfaceObject(&s.surface, &rd));
  auto semaphore = [&](VkSemaphore &sem, cudaExternalSemaphore_t &cs) {
    VkExportSemaphoreCreateInfo es{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    es.handleTypes = SEM_HANDLE;
    VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sc.pNext = &es;
    VK(vkCreateSemaphore(device, &sc, nullptr, &sem));
    cudaExternalSemaphoreHandleDesc sd{};
#ifdef _WIN32
    VkSemaphoreGetWin32HandleInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
    info.semaphore = sem;
    info.handleType = SEM_HANDLE;
    HANDLE handle;
    VK(((PFN_vkGetSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(
        device, "vkGetSemaphoreWin32HandleKHR"))(device, &info, &handle));
    sd.type = cudaExternalSemaphoreHandleTypeOpaqueWin32;
    sd.handle.win32.handle = handle;
    auto result = cudaImportExternalSemaphore(&cs, &sd);
    CloseHandle(handle);
    CU(result);
#else
    VkSemaphoreGetFdInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    info.semaphore = sem;
    info.handleType = SEM_HANDLE;
    int handle = -1;
    VK(((PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR"))(device, &info,
                                                                                     &handle));
    sd.type = cudaExternalSemaphoreHandleTypeOpaqueFd;
    sd.handle.fd = handle;
    auto result = cudaImportExternalSemaphore(&cs, &sd);
    if (result != cudaSuccess)
      close(handle);
    CU(result);
#endif
  };
  semaphore(s.ready, s.cudaReady);
  semaphore(s.released, s.cudaReleased);
}
void Gpu::initEncoder() {
#ifdef _WIN32
  nvlib = LoadLibraryA("nvEncodeAPI64.dll");
  auto create =
      (PNVENCODEAPICREATEINSTANCE)GetProcAddress((HMODULE)nvlib, "NvEncodeAPICreateInstance");
  auto version = (PNVENCODEAPIGETMAXSUPPORTEDVERSION)GetProcAddress(
      (HMODULE)nvlib, "NvEncodeAPIGetMaxSupportedVersion");
#else
  nvlib = dlopen("libnvidia-encode.so.1", RTLD_NOW);
  if (!nvlib)
    throw std::runtime_error(dlerror());
  auto create = (PNVENCODEAPICREATEINSTANCE)dlsym(nvlib, "NvEncodeAPICreateInstance");
  auto version =
      (PNVENCODEAPIGETMAXSUPPORTEDVERSION)dlsym(nvlib, "NvEncodeAPIGetMaxSupportedVersion");
#endif
  if (!create || !version)
    throw std::runtime_error("NVENC entrypoints missing");
  uint32_t max = 0;
  NV(version(&max));
  if (max < ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION))
    throw std::runtime_error("driver NVENC API older than SDK 13.0; update driver");
  nv.version = NV_ENCODE_API_FUNCTION_LIST_VER;
  NV(create(&nv));
  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER};
  open.device = context;
  open.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
  open.apiVersion = NVENCAPI_VERSION;
  NV(nv.nvEncOpenEncodeSessionEx(&open, &encoder));
  NV_ENC_PRESET_CONFIG preset{NV_ENC_PRESET_CONFIG_VER};
  preset.presetCfg.version = NV_ENC_CONFIG_VER;
  NV(nv.nvEncGetEncodePresetConfigEx(encoder, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P1_GUID,
                                     NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset));
  auto config = preset.presetCfg;
  config.profileGUID = NV_ENC_H264_PROFILE_BASELINE_GUID;
  config.gopLength = 120;
  config.frameIntervalP = 1;
  config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
  config.rcParams.averageBitRate = currentBitrate;
  config.rcParams.maxBitRate = currentBitrate;
  config.rcParams.vbvBufferSize = currentBitrate / 30;
  config.rcParams.vbvInitialDelay = config.rcParams.vbvBufferSize;
  config.rcParams.enableLookahead = 0;
  config.rcParams.zeroReorderDelay = 1;
  auto &h = config.encodeCodecConfig.h264Config;
  h.idrPeriod = 120;
  h.repeatSPSPPS = 1;
  h.chromaFormatIDC = 1;
  h.h264VUIParameters.videoSignalTypePresentFlag = 1;
  h.h264VUIParameters.colourDescriptionPresentFlag = 1;
  h.h264VUIParameters.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
  h.h264VUIParameters.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
  h.h264VUIParameters.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
  h.h264VUIParameters.videoFullRangeFlag = 0;
  NV_ENC_INITIALIZE_PARAMS init{NV_ENC_INITIALIZE_PARAMS_VER};
  init.encodeGUID = NV_ENC_CODEC_H264_GUID;
  init.presetGUID = NV_ENC_PRESET_P1_GUID;
  init.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
  init.encodeWidth = init.darWidth = width;
  init.encodeHeight = init.darHeight = height;
  init.frameRateNum = 30;
  init.frameRateDen = 1;
  init.enablePTD = 1;
  init.encodeConfig = &config;
  init.maxEncodeWidth = width;
  init.maxEncodeHeight = height;
  NV(nv.nvEncInitializeEncoder(encoder, &init));
  encoderConfig = config;
  encoderInit = init;
  encoderInit.encodeConfig = &encoderConfig;
  for (auto &s : slots) {
    NV_ENC_REGISTER_RESOURCE reg{NV_ENC_REGISTER_RESOURCE_VER};
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR;
    reg.resourceToRegister = s.nv12;
    reg.width = width;
    reg.height = height;
    reg.pitch = pitch;
    reg.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    s.encodeInput = s.nv12;
    auto status = nv.nvEncRegisterResource(encoder, &reg);
    if (status == NV_ENC_ERR_RESOURCE_REGISTER_FAILED && std::getenv("VIEWER_NVENC_LEGACY_INPUT") &&
        std::string(std::getenv("VIEWER_NVENC_LEGACY_INPUT")) == "1") {
      // Opt-in compatibility: fixed initialization-only NVENC allocations. Never in the frame loop.
      CU(cudaMalloc((void **)&s.encodeInput, size_t(pitch) * height * 3 / 2));
      s.legacyInput = true;
      reg.resourceToRegister = s.encodeInput;
      NV(nv.nvEncRegisterResource(encoder, &reg));
    } else
      NV(status);
    s.registered = reg.registeredResource;
    NV_ENC_CREATE_BITSTREAM_BUFFER bs{NV_ENC_CREATE_BITSTREAM_BUFFER_VER};
    NV(nv.nvEncCreateBitstreamBuffer(encoder, &bs));
    s.bitstream = bs.bitstreamBuffer;
  }
  std::cout << "GPU: " << deviceName << " | Vulkan/CUDA UUID matched | NVENC H264 | " << SLOT_COUNT
            << " slots | explicit CUDA streams + pool\n";
}
void Gpu::load(const std::vector<Vertex> &data) {
  VK(vkDeviceWaitIdle(device));
  if (vertices)
    vkDestroyBuffer(device, vertices, nullptr);
  vertices = {};
  if (vertexMemory)
    vkFreeMemory(device, vertexMemory, nullptr);
  vertexMemory = {};
  vertexCount = data.size();
  if (data.empty())
    throw std::runtime_error("empty triangle mesh");
  // Upload once at model load. Draws must read device-local memory, not PCIe host memory.
  VkBuffer staging{};
  VkDeviceMemory stagingMemory{};
  VkCommandBuffer upload{};
  VkFence fence{};
  auto cleanupUpload = [&] {
    if (fence) vkDestroyFence(device, fence, nullptr);
    if (upload) vkFreeCommandBuffers(device, commands, 1, &upload);
    if (staging) vkDestroyBuffer(device, staging, nullptr);
    if (stagingMemory) vkFreeMemory(device, stagingMemory, nullptr);
  };
  try {
    VkDeviceSize bytes = data.size() * sizeof(Vertex);
    auto buffer = [&](VkBufferUsageFlags usage, VkMemoryPropertyFlags properties,
                      VkBuffer &buffer, VkDeviceMemory &memory) {
      VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      b.size = bytes;
      b.usage = usage;
      VK(vkCreateBuffer(device, &b, nullptr, &buffer));
      VkMemoryRequirements req;
      vkGetBufferMemoryRequirements(device, buffer, &req);
      VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      a.allocationSize = req.size;
      a.memoryTypeIndex = memoryType(req.memoryTypeBits, properties);
      VK(vkAllocateMemory(device, &a, nullptr, &memory));
      VK(vkBindBufferMemory(device, buffer, memory, 0));
    };
    buffer(VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
           staging, stagingMemory);
    buffer(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, vertices, vertexMemory);
    void *ptr;
    VK(vkMapMemory(device, stagingMemory, 0, bytes, 0, &ptr));
    memcpy(ptr, data.data(), bytes);
    vkUnmapMemory(device, stagingMemory);
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = commands;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VK(vkAllocateCommandBuffers(device, &alloc, &upload));
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK(vkBeginCommandBuffer(upload, &begin));
    VkBufferCopy copy{0, 0, bytes};
    vkCmdCopyBuffer(upload, staging, vertices, 1, &copy);
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = vertices;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(upload, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
    VK(vkEndCommandBuffer(upload));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK(vkCreateFence(device, &fi, nullptr, &fence));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &upload;
    VK(vkQueueSubmit(queue, 1, &submit, fence));
    VK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
  } catch (...) {
    vkDeviceWaitIdle(device);
    cleanupUpload();
    throw;
  }
  cleanupUpload();
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
void Gpu::render(int i, const Camera &cam, FrameTiming t) {
  auto &s = slots.at(i);
  CU(cudaSetDevice(cudaDevice));
  VK(vkWaitForFences(device, 1, &s.fence, VK_TRUE, UINT64_MAX));
  VK(vkResetFences(device, 1, &s.fence));
  VK(vkResetCommandBuffer(s.command, 0));
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  VK(vkBeginCommandBuffer(s.command, &begin));
  vkCmdResetQueryPool(s.command, queries, i * 2, 2);
  vkCmdWriteTimestamp(s.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, i * 2);
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.oldLayout = s.used ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b.srcQueueFamilyIndex = s.used ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = s.used ? family : VK_QUEUE_FAMILY_IGNORED;
  b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  b.image = s.image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(s.command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
                       &b);
  VkClearValue clear[2]{};
  clear[0].color = {{.028f, .043f, .068f, 1}};
  clear[1].depthStencil = {1, 0};
  VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rp.renderPass = renderpass;
  rp.framebuffer = s.framebuffer;
  rp.renderArea.extent = {uint32_t(width), uint32_t(height)};
  rp.clearValueCount = 2;
  rp.pClearValues = clear;
  vkCmdBeginRenderPass(s.command, &rp, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(s.command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  glm::vec3 target(cam.panX, cam.panY, 0),
      eye = target + cam.distance * glm::vec3(cos(cam.pitch) * sin(cam.yaw), sin(cam.pitch),
                                              cos(cam.pitch) * cos(cam.yaw));
  auto projection = glm::perspectiveRH_ZO(glm::radians(45.f), float(width) / height, .01f, 100.f);
  projection[1][1] *= -1;
  auto mvp = projection * glm::lookAtRH(eye, target, glm::vec3(0, 1, 0));
  vkCmdPushConstants(s.command, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, &mvp);
  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(s.command, 0, 1, &vertices, &offset);
  vkCmdDraw(s.command, uint32_t(vertexCount), 1, 0, 0);
  // Small binary frame marker enables exact presented-frame/control association without clock
  // guessing.
  for (int bit = 0; bit < 24; bit++) {
    VkClearAttachment a{};
    a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    float v = (t.frame & (1ull << bit)) ? 1.f : 0.f;
    a.clearValue.color = {{v, v, v, 1}};
    VkClearRect r{{{bit * 8, 0}, {8, 8}}, 0, 1};
    vkCmdClearAttachments(s.command, 1, &a, 1, &r);
  }
  vkCmdEndRenderPass(s.command);
  b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  b.srcQueueFamilyIndex = family;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
  b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  b.dstAccessMask = 0;
  vkCmdPipelineBarrier(s.command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
  vkCmdWriteTimestamp(s.command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, i * 2 + 1);
  VK(vkEndCommandBuffer(s.command));
  VkPipelineStageFlags stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  if (s.used) {
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &s.released;
    submit.pWaitDstStageMask = &stage;
  }
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &s.command;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &s.ready;
  t.submit = now_ms();
  VK(vkQueueSubmit(queue, 1, &submit, s.fence));
  cudaExternalSemaphoreWaitParams wp{};
  CU(cudaWaitExternalSemaphoresAsync(&s.cudaReady, &wp, 1, s.stream));
  CU(cudaEventRecord(s.start, s.stream));
  CU(convert_nv12(s.surface, s.nv12, pitch, width, height, s.stream));
  CU(cudaEventRecord(s.end, s.stream));
  if (s.legacyInput)
    CU(cudaMemcpyAsync(s.encodeInput, s.nv12, size_t(pitch) * height * 3 / 2,
                       cudaMemcpyDeviceToDevice, s.stream));
  cudaExternalSemaphoreSignalParams sp{};
  CU(cudaSignalExternalSemaphoresAsync(&s.cudaReleased, &sp, 1, s.stream));
  s.used = true;
  s.timing = t;
}
int Gpu::encode(int i, bool key, unsigned bitrate, const unsigned char **data, size_t *length,
                int *isKey) {
  auto &s = slots.at(i);
  CU(cudaSetDevice(cudaDevice));
  // A CPU wait is required before Linux synchronous NVENC mapping; no raw image crosses the PCIe
  // bus.
  CU(cudaStreamSynchronize(s.stream));
  CU(cudaEventElapsedTime(&s.timing.convertMs, s.start, s.end));
  uint64_t ts[2]{};
  VK(vkGetQueryPoolResults(device, queries, i * 2, 2, sizeof(ts), ts, sizeof(uint64_t),
                           VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
  s.timing.renderMs = float(ts[1] - ts[0]) * timestampPeriod / 1e6f;
  NV(nv.nvEncSetIOCudaStreams(encoder, &s.stream, &s.stream));
  if (bitrate >= 200000 && bitrate != currentBitrate) {
    encoderConfig.rcParams.averageBitRate = encoderConfig.rcParams.maxBitRate = bitrate;
    encoderConfig.rcParams.vbvBufferSize = encoderConfig.rcParams.vbvInitialDelay = bitrate / 30;
    NV_ENC_RECONFIGURE_PARAMS r{NV_ENC_RECONFIGURE_PARAMS_VER};
    r.reInitEncodeParams = encoderInit;
    NV(nv.nvEncReconfigureEncoder(encoder, &r));
    currentBitrate = bitrate;
  }
  s.timing.encodeStart = now_ms();
  NV_ENC_MAP_INPUT_RESOURCE map{NV_ENC_MAP_INPUT_RESOURCE_VER};
  map.registeredResource = s.registered;
  NV(nv.nvEncMapInputResource(encoder, &map));
  s.mapped = map.mappedResource;
  NV_ENC_PIC_PARAMS pic{NV_ENC_PIC_PARAMS_VER};
  pic.inputBuffer = s.mapped;
  pic.bufferFmt = NV_ENC_BUFFER_FORMAT_NV12;
  pic.inputWidth = width;
  pic.inputHeight = height;
  pic.inputPitch = pitch;
  pic.outputBitstream = s.bitstream;
  pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
  pic.inputTimeStamp = s.timing.frame;
  if (key)
    pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
  NV(nv.nvEncEncodePicture(encoder, &pic));
  NV_ENC_LOCK_BITSTREAM lock{NV_ENC_LOCK_BITSTREAM_VER};
  lock.outputBitstream = s.bitstream;
  lock.doNotWait = 0;
  NV(nv.nvEncLockBitstream(encoder, &lock));
  s.timing.encodeEnd = now_ms();
  *data = (const unsigned char *)lock.bitstreamBufferPtr;
  *length = lock.bitstreamSizeInBytes;
  *isKey = lock.pictureType == NV_ENC_PIC_TYPE_IDR;
  return 0;
}
void Gpu::unlock(int i) {
  auto &s = slots.at(i);
  NV(nv.nvEncUnlockBitstream(encoder, s.bitstream));
  NV(nv.nvEncUnmapInputResource(encoder, s.mapped));
  s.mapped = {};
}
Gpu::~Gpu() {
  cleanup();
}
void Gpu::cleanup() noexcept {
  if (cudaDevice >= 0)
    cudaSetDevice(cudaDevice);
  for (auto &s : slots)
    if (s.stream)
      cudaStreamSynchronize(s.stream);
  if (device)
    vkDeviceWaitIdle(device);
  if (encoder) {
    for (auto &s : slots) {
      if (s.mapped)
        nv.nvEncUnmapInputResource(encoder, s.mapped);
      if (s.registered)
        nv.nvEncUnregisterResource(encoder, s.registered);
      if (s.bitstream)
        nv.nvEncDestroyBitstreamBuffer(encoder, s.bitstream);
    }
    nv.nvEncDestroyEncoder(encoder);
    encoder = nullptr;
  }
  for (auto &s : slots) {
    if (s.surface)
      cudaDestroySurfaceObject(s.surface);
    if (s.mip)
      cudaFreeMipmappedArray(s.mip);
    if (s.external)
      cudaDestroyExternalMemory(s.external);
    if (s.cudaReady)
      cudaDestroyExternalSemaphore(s.cudaReady);
    if (s.cudaReleased)
      cudaDestroyExternalSemaphore(s.cudaReleased);
    if (s.legacyInput && s.encodeInput)
      cudaFree(s.encodeInput);
    if (s.nv12)
      cudaFreeAsync(s.nv12, s.stream);
    if (s.stream)
      cudaStreamSynchronize(s.stream);
    if (s.start)
      cudaEventDestroy(s.start);
    if (s.end)
      cudaEventDestroy(s.end);
    if (s.stream)
      cudaStreamDestroy(s.stream);
    if (device) {
      if (s.framebuffer)
        vkDestroyFramebuffer(device, s.framebuffer, nullptr);
      if (s.view)
        vkDestroyImageView(device, s.view, nullptr);
      if (s.depthView)
        vkDestroyImageView(device, s.depthView, nullptr);
      if (s.image)
        vkDestroyImage(device, s.image, nullptr);
      if (s.depth)
        vkDestroyImage(device, s.depth, nullptr);
      if (s.memory)
        vkFreeMemory(device, s.memory, nullptr);
      if (s.depthMemory)
        vkFreeMemory(device, s.depthMemory, nullptr);
      if (s.fence)
        vkDestroyFence(device, s.fence, nullptr);
      if (s.ready)
        vkDestroySemaphore(device, s.ready, nullptr);
      if (s.released)
        vkDestroySemaphore(device, s.released, nullptr);
    }
  }
  if (pool)
    cudaMemPoolDestroy(pool);
  if (device) {
    if (vertices)
      vkDestroyBuffer(device, vertices, nullptr);
    if (vertexMemory)
      vkFreeMemory(device, vertexMemory, nullptr);
    if (queries)
      vkDestroyQueryPool(device, queries, nullptr);
    if (pipeline)
      vkDestroyPipeline(device, pipeline, nullptr);
    if (layout)
      vkDestroyPipelineLayout(device, layout, nullptr);
    if (renderpass)
      vkDestroyRenderPass(device, renderpass, nullptr);
    if (commands)
      vkDestroyCommandPool(device, commands, nullptr);
    vkDestroyDevice(device, nullptr);
  }
  if (instance)
    vkDestroyInstance(instance, nullptr);
#ifdef _WIN32
  if (nvlib)
    FreeLibrary((HMODULE)nvlib);
#else
  if (nvlib)
    dlclose(nvlib);
#endif
}
