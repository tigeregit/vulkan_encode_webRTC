// Vulkan device setup, memory-type selection and image/framebuffer creation.
#include "render/gpu.hpp"
#include "render/platform.hpp"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
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
  VK(vkCreateInstance(&ci, nullptr, instance.out()));
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
  const char *extensions[] = {EXTERNAL_MEMORY_EXTENSION, EXTERNAL_SEMAPHORE_EXTENSION};
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
  VK(vkCreateDevice(physical, &dc, nullptr, device.out()));
  vkGetDeviceQueue(device, family, 0, &queue);
  VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pc.queueFamilyIndex = family;
  pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VK(vkCreateCommandPool(device, &pc, nullptr, commands.out(device)));
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
  VK(vkCreateRenderPass(device, &rp, nullptr, renderpass.out(device)));
  VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT, 0, 64};
  VkPipelineLayoutCreateInfo lc{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  lc.pushConstantRangeCount = 1;
  lc.pPushConstantRanges = &range;
  VK(vkCreatePipelineLayout(device, &lc, nullptr, layout.out(device)));
  // Shader modules are only needed to build the pipeline, so they are owners
  // scoped to this function and released even if pipeline creation fails.
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
    VkShaderModuleOwner module;
    VkShaderModule raw{};
    VK(vkCreateShaderModule(device, &sc, nullptr, &raw));
    module.adopt(device, raw);
    return module;
  };
  VkShaderModuleOwner vs = shader("scene.vert.spv"), fs = shader("scene.frag.spv");
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
  VK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr, pipeline.out(device)));
  VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
  qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
  qi.queryCount = SLOT_COUNT * 2;
  VK(vkCreateQueryPool(device, &qi, nullptr, queries.out(device)));
}
void Gpu::image(VkFormat format, VkImageUsageFlags usage, bool shared, VkImageOwner &img,
                VkMemoryOwner &mem, VkImageViewOwner &view) {
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
  VkImage raw{};
  VK(vkCreateImage(device, &ci, nullptr, &raw));
  img.adopt(device, raw);
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device, raw, &req);
  VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated.image = raw;
  VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
  exp.handleTypes = MEM_HANDLE;
  exp.pNext = &dedicated;
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.pNext = shared ? static_cast<void *>(&exp) : nullptr;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = memoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkDeviceMemory rawMemory{};
  VK(vkAllocateMemory(device, &alloc, nullptr, &rawMemory));
  mem.adopt(device, rawMemory);
  VK(vkBindImageMemory(device, raw, rawMemory, 0));
  VkImageViewCreateInfo vc{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vc.image = raw;
  vc.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vc.format = format;
  vc.subresourceRange = {static_cast<VkImageAspectFlags>(shared ? VK_IMAGE_ASPECT_COLOR_BIT
                                                                : VK_IMAGE_ASPECT_DEPTH_BIT),
                         0, 1, 0, 1};
  VkImageView rawView{};
  VK(vkCreateImageView(device, &vc, nullptr, &rawView));
  view.adopt(device, rawView);
}
