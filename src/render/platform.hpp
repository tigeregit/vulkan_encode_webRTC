#pragma once
// Single place where the Win32 / POSIX split lives. Everything else in the
// renderer works with these aliases and never writes its own #ifdef.
#include "core/common.hpp"
#include <cuda_runtime.h>
#include <vulkan/vulkan.h>
#include <cstddef>
#ifdef _WIN32
constexpr auto MEM_HANDLE = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
constexpr auto SEM_HANDLE = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
constexpr const char *EXTERNAL_MEMORY_EXTENSION = VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;
constexpr const char *EXTERNAL_SEMAPHORE_EXTENSION = VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME;
#else
constexpr auto MEM_HANDLE = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
constexpr auto SEM_HANDLE = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
constexpr const char *EXTERNAL_MEMORY_EXTENSION = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
constexpr const char *EXTERNAL_SEMAPHORE_EXTENSION = VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME;
#endif
// Imports a Vulkan allocation as CUDA external memory, closing the temporary
// platform handle before returning. Throws on failure.
void import_cuda_memory(VkDevice device, VkDeviceMemory memory, size_t size,
                        cudaExternalMemory_t *out);
// Imports a Vulkan binary semaphore as a CUDA external semaphore. Throws on
// failure.
void import_cuda_semaphore(VkDevice device, VkSemaphore semaphore,
                           cudaExternalSemaphore_t *out);
// Minimal RAII handle over LoadLibrary/dlopen so the NVENC entrypoints stay a
// runtime dependency rather than a link-time one.
class DynamicLibrary {
public:
  explicit DynamicLibrary(const char *name);
  ~DynamicLibrary();
  DynamicLibrary(const DynamicLibrary &) = delete;
  DynamicLibrary &operator=(const DynamicLibrary &) = delete;
  void *symbol(const char *name) const;

private:
  void *handle_ = nullptr;
};
