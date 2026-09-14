#pragma once
// Owning wrappers for every Vulkan and CUDA handle the renderer creates. Two
// rules make them safe with no runtime bookkeeping:
//   1. they are data members, so C++ reverse-declaration destruction produces
//      exactly the teardown order documented in gpu.hpp;
//   2. each one stores the parent handle it must be released with, which
//      guarantees a member is freed while its device / pool is still alive.
// Destructors discard the driver's error code because they cannot throw.
//
// Each owner converts implicitly to its raw handle so call sites stay short;
// ownership only ever leaves an owner through adopt()/reset().
#include "core/common.hpp"
#include <cuda_runtime.h>
#include <utility>
#include <vulkan/vulkan.h>
// Root Vulkan handles: destroyed with (handle, allocator).
template <typename Handle, auto Destroy>
class VkRoot {
public:
  VkRoot() = default;
  VkRoot(const VkRoot &) = delete;
  VkRoot &operator=(const VkRoot &) = delete;
  VkRoot(VkRoot &&o) noexcept {
    steal(o);
  }
  VkRoot &operator=(VkRoot &&o) noexcept {
    if (this != &o) {
      reset();
      steal(o);
    }
    return *this;
  }
  ~VkRoot() {
    reset();
  }
  void adopt(Handle handle) {
    reset();
    handle_ = handle;
  }
  void reset() {
    if (handle_ != Handle{}) {
      Destroy(handle_, nullptr);
      handle_ = {};
    }
  }
  operator Handle() const {
    return handle_;
  }
  Handle get() const {
    return handle_;
  }
  // Address for create calls that write the handle out.
  Handle *out() {
    reset();
    return &handle_;
  }

private:
  void steal(VkRoot &o) {
    handle_ = o.handle_;
    o.handle_ = {};
  }
  Handle handle_{};
};
// Handles released through a VkDevice: the vkDestroy*/vkFree* entrypoints this
// project uses all share the (device, handle, allocator) signature.
template <typename Handle, auto Destroy>
class VkChild {
public:
  VkChild() = default;
  VkChild(const VkChild &) = delete;
  VkChild &operator=(const VkChild &) = delete;
  VkChild(VkChild &&o) noexcept {
    steal(o);
  }
  VkChild &operator=(VkChild &&o) noexcept {
    if (this != &o) {
      reset();
      steal(o);
    }
    return *this;
  }
  ~VkChild() {
    reset();
  }
  // Takes ownership of `handle`, created with `device`.
  void adopt(VkDevice device, Handle handle) {
    reset();
    device_ = device;
    handle_ = handle;
  }
  void reset() {
    if (handle_ != Handle{}) {
      Destroy(device_, handle_, nullptr);
      handle_ = {};
    }
  }
  operator Handle() const {
    return handle_;
  }
  Handle get() const {
    return handle_;
  }
  // Address for create calls that write the handle out. The device is taken
  // here because creation bypasses adopt().
  Handle *out(VkDevice device) {
    reset();
    device_ = device;
    return &handle_;
  }

private:
  void steal(VkChild &o) {
    device_ = o.device_;
    handle_ = o.handle_;
    o.handle_ = {};
  }
  VkDevice device_{};
  Handle handle_{};
};
// CUDA handles, whose destroy entrypoints take only the handle.
template <typename Handle, auto Destroy>
class CudaOwned {
public:
  CudaOwned() = default;
  CudaOwned(const CudaOwned &) = delete;
  CudaOwned &operator=(const CudaOwned &) = delete;
  CudaOwned(CudaOwned &&o) noexcept {
    steal(o);
  }
  CudaOwned &operator=(CudaOwned &&o) noexcept {
    if (this != &o) {
      reset();
      steal(o);
    }
    return *this;
  }
  ~CudaOwned() {
    reset();
  }
  void adopt(Handle handle) {
    reset();
    handle_ = handle;
  }
  void reset() {
    if (handle_ != Handle{}) {
      Destroy(handle_);
      handle_ = {};
    }
  }
  operator Handle() const {
    return handle_;
  }
  Handle get() const {
    return handle_;
  }
  // Stable address of the stored handle, valid for this owner's lifetime.
  // Needed for APIs such as nvEncSetIOCudaStreams that retain the pointer they
  // are given rather than copying the handle.
  Handle *address() {
    return &handle_;
  }
  // Address for create calls that write the handle out.
  Handle *out() {
    reset();
    return &handle_;
  }

private:
  void steal(CudaOwned &o) {
    handle_ = o.handle_;
    o.handle_ = {};
  }
  Handle handle_{};
};
// Command buffers are freed as a batch and need their pool, so they do not fit
// any template above.
class VkCommandBufferOwner {
public:
  VkCommandBufferOwner() = default;
  VkCommandBufferOwner(const VkCommandBufferOwner &) = delete;
  VkCommandBufferOwner &operator=(const VkCommandBufferOwner &) = delete;
  ~VkCommandBufferOwner() {
    reset();
  }
  void adopt(VkDevice device, VkCommandPool pool, VkCommandBuffer handle) {
    reset();
    device_ = device;
    pool_ = pool;
    handle_ = handle;
  }
  void reset() {
    if (handle_ != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(device_, pool_, 1, &handle_);
      handle_ = VK_NULL_HANDLE;
    }
  }
  operator VkCommandBuffer() const {
    return handle_;
  }
  VkCommandBuffer get() const {
    return handle_;
  }

private:
  VkDevice device_{};
  VkCommandPool pool_{};
  VkCommandBuffer handle_{};
};
// A CUDA memory-pool allocation is released on the stream it came from, so it
// has to carry that stream. The synchronize matches the previous explicit
// teardown: the free is ordered on the stream and must complete before the
// stream itself is destroyed.
class CudaPoolBuffer {
public:
  CudaPoolBuffer() = default;
  CudaPoolBuffer(const CudaPoolBuffer &) = delete;
  CudaPoolBuffer &operator=(const CudaPoolBuffer &) = delete;
  CudaPoolBuffer(CudaPoolBuffer &&o) noexcept {
    steal(o);
  }
  CudaPoolBuffer &operator=(CudaPoolBuffer &&o) noexcept {
    if (this != &o) {
      reset();
      steal(o);
    }
    return *this;
  }
  ~CudaPoolBuffer() {
    reset();
  }
  void reset() {
    if (data_) {
      cudaFreeAsync(data_, stream_);
      cudaStreamSynchronize(stream_);
      data_ = nullptr;
    }
  }
  // Address for cudaMallocFromPoolAsync, which writes the pointer out.
  void **out() {
    reset();
    return &data_;
  }
  void set_stream(cudaStream_t stream) {
    stream_ = stream;
  }
  operator unsigned char *() const {
    return static_cast<unsigned char *>(data_);
  }
  unsigned char *get() const {
    return static_cast<unsigned char *>(data_);
  }

private:
  void steal(CudaPoolBuffer &o) {
    stream_ = o.stream_;
    data_ = o.data_;
    o.data_ = nullptr;
  }
  cudaStream_t stream_{};
  void *data_{};
};
using VkInstanceOwner = VkRoot<VkInstance, vkDestroyInstance>;
using VkDeviceOwner = VkRoot<VkDevice, vkDestroyDevice>;
using VkImageOwner = VkChild<VkImage, vkDestroyImage>;
using VkImageViewOwner = VkChild<VkImageView, vkDestroyImageView>;
using VkFramebufferOwner = VkChild<VkFramebuffer, vkDestroyFramebuffer>;
using VkFenceOwner = VkChild<VkFence, vkDestroyFence>;
using VkSemaphoreOwner = VkChild<VkSemaphore, vkDestroySemaphore>;
using VkBufferOwner = VkChild<VkBuffer, vkDestroyBuffer>;
using VkMemoryOwner = VkChild<VkDeviceMemory, vkFreeMemory>;
using VkQueryPoolOwner = VkChild<VkQueryPool, vkDestroyQueryPool>;
using VkPipelineOwner = VkChild<VkPipeline, vkDestroyPipeline>;
using VkPipelineLayoutOwner = VkChild<VkPipelineLayout, vkDestroyPipelineLayout>;
using VkRenderPassOwner = VkChild<VkRenderPass, vkDestroyRenderPass>;
using VkCommandPoolOwner = VkChild<VkCommandPool, vkDestroyCommandPool>;
using VkShaderModuleOwner = VkChild<VkShaderModule, vkDestroyShaderModule>;
using CudaStreamOwner = CudaOwned<cudaStream_t, cudaStreamDestroy>;
using CudaEventOwner = CudaOwned<cudaEvent_t, cudaEventDestroy>;
using CudaMemPoolOwner = CudaOwned<cudaMemPool_t, cudaMemPoolDestroy>;
using CudaExternalMemoryOwner = CudaOwned<cudaExternalMemory_t, cudaDestroyExternalMemory>;
using CudaExternalSemaphoreOwner = CudaOwned<cudaExternalSemaphore_t, cudaDestroyExternalSemaphore>;
using CudaMipmappedArrayOwner = CudaOwned<cudaMipmappedArray_t, cudaFreeMipmappedArray>;
using CudaSurfaceOwner = CudaOwned<cudaSurfaceObject_t, cudaDestroySurfaceObject>;
// Fixed allocation used only by the NVENC driver compatibility path.
using CudaAllocation = CudaOwned<void *, cudaFree>;
