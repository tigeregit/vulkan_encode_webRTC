#pragma once
#include <chrono>
#include <cuda_runtime.h>
#include <stdexcept>
#include <string>
#include <vulkan/vulkan.h>
inline double now_ms() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
inline void vkcheck(VkResult r, const char *op) {
  if (r != VK_SUCCESS)
    throw std::runtime_error(std::string(op) + ": Vulkan " + std::to_string(r));
}
inline void cucheck(cudaError_t r, const char *op) {
  if (r != cudaSuccess)
    throw std::runtime_error(std::string(op) + ": " + cudaGetErrorString(r));
}
#define VK(x) vkcheck((x), #x)
#define CU(x) cucheck((x), #x)
