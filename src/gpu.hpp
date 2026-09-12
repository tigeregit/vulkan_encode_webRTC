#pragma once
#include "common.hpp"
#include <array>
#include <atomic>
#include <cuda.h>
#include <glm/glm.hpp>
#include <mutex>
#include <nvEncodeAPI.h>
#include <vector>
constexpr int SLOT_COUNT = 4;
struct Vertex {
  glm::vec3 position, normal;
  glm::vec4 color;
};
struct Camera {
  float yaw = .5f, pitch = .25f, distance = 3.5f, panX = 0, panY = 0;
};
struct FrameTiming {
  uint64_t seq = 0, frame = 0;
  double client = 0, received = 0, applied = 0, submit = 0, encodeStart = 0, encodeEnd = 0;
  float renderMs = 0, convertMs = 0;
};
struct Slot {
  std::atomic<bool> busy{false};
  VkImage image{}, depth{};
  VkDeviceMemory memory{}, depthMemory{};
  VkImageView view{}, depthView{};
  VkFramebuffer framebuffer{};
  VkCommandBuffer command{};
  VkFence fence{};
  VkSemaphore ready{}, released{};
  bool used = false;
  cudaExternalMemory_t external{};
  cudaMipmappedArray_t mip{};
  cudaSurfaceObject_t surface{};
  cudaExternalSemaphore_t cudaReady{}, cudaReleased{};
  cudaStream_t stream{};
  cudaEvent_t start{}, end{};
  unsigned char *nv12{};
  unsigned char *encodeInput{};
  bool legacyInput = false;
  NV_ENC_REGISTERED_PTR registered{};
  NV_ENC_OUTPUT_PTR bitstream{};
  NV_ENC_INPUT_PTR mapped{};
  FrameTiming timing;
};
class Gpu {
public:
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
  std::array<Slot, SLOT_COUNT> slots;

private:
  VkInstance instance{};
  VkPhysicalDevice physical{};
  VkDevice device{};
  VkQueue queue{};
  uint32_t family = 0, nextSlot = 0;
  VkCommandPool commands{};
  VkRenderPass renderpass{};
  VkPipelineLayout layout{};
  VkPipeline pipeline{};
  VkBuffer vertices{};
  VkDeviceMemory vertexMemory{};
  size_t vertexCount = 0;
  VkQueryPool queries{};
  float timestampPeriod = 1;
  cudaMemPool_t pool{};
  CUcontext context{};
  void *nvlib{};
  NV_ENCODE_API_FUNCTION_LIST nv{};
  void *encoder{};
  unsigned currentBitrate = 6000000;
  NV_ENC_CONFIG encoderConfig{};
  NV_ENC_INITIALIZE_PARAMS encoderInit{};
  uint32_t memoryType(uint32_t, VkMemoryPropertyFlags);
  void initVulkan(const std::string &);
  void initCuda();
  void initEncoder();
  void createSlot(Slot &);
  void cleanup() noexcept;
  void image(VkFormat, VkImageUsageFlags, bool, VkImage &, VkDeviceMemory &, VkImageView &);
};
