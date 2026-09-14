#include "render/platform.hpp"
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif
#ifdef _WIN32
void import_cuda_memory(VkDevice device, VkDeviceMemory memory, size_t size,
                        cudaExternalMemory_t *out) {
  VkMemoryGetWin32HandleInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
  info.memory = memory;
  info.handleType = MEM_HANDLE;
  HANDLE handle = nullptr;
  auto get =
      (PFN_vkGetMemoryWin32HandleKHR)vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR");
  if (!get)
    throw std::runtime_error("vkGetMemoryWin32HandleKHR unavailable");
  VK(get(device, &info, &handle));
  cudaExternalMemoryHandleDesc md{};
  md.size = size;
  md.flags = cudaExternalMemoryDedicated;
  md.type = cudaExternalMemoryHandleTypeOpaqueWin32;
  md.handle.win32.handle = handle;
  auto result = cudaImportExternalMemory(out, &md);
  CloseHandle(handle);
  CU(result);
}
void import_cuda_semaphore(VkDevice device, VkSemaphore semaphore,
                           cudaExternalSemaphore_t *out) {
  VkSemaphoreGetWin32HandleInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
  info.semaphore = semaphore;
  info.handleType = SEM_HANDLE;
  HANDLE handle = nullptr;
  auto get =
      (PFN_vkGetSemaphoreWin32HandleKHR)vkGetDeviceProcAddr(device, "vkGetSemaphoreWin32HandleKHR");
  if (!get)
    throw std::runtime_error("vkGetSemaphoreWin32HandleKHR unavailable");
  VK(get(device, &info, &handle));
  cudaExternalSemaphoreHandleDesc sd{};
  sd.type = cudaExternalSemaphoreHandleTypeOpaqueWin32;
  sd.handle.win32.handle = handle;
  auto result = cudaImportExternalSemaphore(out, &sd);
  CloseHandle(handle);
  CU(result);
}
#else
void import_cuda_memory(VkDevice device, VkDeviceMemory memory, size_t size,
                        cudaExternalMemory_t *out) {
  VkMemoryGetFdInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
  info.memory = memory;
  info.handleType = MEM_HANDLE;
  int fd = -1;
  auto get = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR");
  if (!get)
    throw std::runtime_error("vkGetMemoryFdKHR unavailable");
  VK(get(device, &info, &fd));
  cudaExternalMemoryHandleDesc md{};
  md.size = size;
  md.flags = cudaExternalMemoryDedicated;
  md.type = cudaExternalMemoryHandleTypeOpaqueFd;
  md.handle.fd = fd;
  auto result = cudaImportExternalMemory(out, &md);
  if (result != cudaSuccess)
    close(fd);
  CU(result);
}
void import_cuda_semaphore(VkDevice device, VkSemaphore semaphore,
                           cudaExternalSemaphore_t *out) {
  VkSemaphoreGetFdInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
  info.semaphore = semaphore;
  info.handleType = SEM_HANDLE;
  int fd = -1;
  auto get = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(device, "vkGetSemaphoreFdKHR");
  if (!get)
    throw std::runtime_error("vkGetSemaphoreFdKHR unavailable");
  VK(get(device, &info, &fd));
  cudaExternalSemaphoreHandleDesc sd{};
  sd.type = cudaExternalSemaphoreHandleTypeOpaqueFd;
  sd.handle.fd = fd;
  auto result = cudaImportExternalSemaphore(out, &sd);
  if (result != cudaSuccess)
    close(fd);
  CU(result);
}
#endif
DynamicLibrary::DynamicLibrary(const char *name) {
#ifdef _WIN32
  handle_ = LoadLibraryA(name);
#else
  handle_ = dlopen(name, RTLD_NOW);
#endif
  if (!handle_)
    throw std::runtime_error(std::string("cannot load ") + name);
}
DynamicLibrary::~DynamicLibrary() {
  if (!handle_)
    return;
#ifdef _WIN32
  FreeLibrary((HMODULE)handle_);
#else
  dlclose(handle_);
#endif
}
void *DynamicLibrary::symbol(const char *name) const {
#ifdef _WIN32
  return reinterpret_cast<void *>(GetProcAddress((HMODULE)handle_, name));
#else
  return dlsym(handle_, name);
#endif
}
