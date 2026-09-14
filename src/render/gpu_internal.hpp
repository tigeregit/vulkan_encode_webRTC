#pragma once
#include "render/gpu.hpp"
#include "render/handles.hpp"
#include "render/platform.hpp"
#include <array>
#include <cuda.h>
#include <memory>
#include <nvEncodeAPI.h>
#include <stdexcept>
#include <string>
inline void nvcheck(NVENCSTATUS s, const char *op) {
  if (s != NV_ENC_SUCCESS)
    throw std::runtime_error(std::string(op) + ": NVENC " + std::to_string(s));
}
#define NV(x) nvcheck((x), #x)
// NVENC objects are released through the function list and session that created
// them, so each owner carries both. Unmapping, unregistering and destroying a
// bitstream all share the (session, handle) shape, so one template covers them.
// `Release` selects the entrypoint from the function list.
template <typename Handle, auto Release>
class NvencChild {
public:
  NvencChild() = default;
  NvencChild(const NvencChild &) = delete;
  NvencChild &operator=(const NvencChild &) = delete;
  ~NvencChild() {
    reset();
  }
  // Takes ownership of `handle`, created by `session`.
  void adopt(void *session, const NV_ENCODE_API_FUNCTION_LIST *api, Handle handle) {
    reset();
    session_ = session;
    api_ = api;
    handle_ = handle;
  }
  void reset() {
    if (handle_) {
      (api_->*Release)(session_, handle_);
      handle_ = {};
    }
  }
  operator Handle() const {
    return handle_;
  }
  Handle get() const {
    return handle_;
  }

private:
  void *session_{};
  const NV_ENCODE_API_FUNCTION_LIST *api_{};
  Handle handle_{};
};
using NvencRegistered =
    NvencChild<NV_ENC_REGISTERED_PTR, &NV_ENCODE_API_FUNCTION_LIST::nvEncUnregisterResource>;
using NvencBitstream =
    NvencChild<NV_ENC_OUTPUT_PTR, &NV_ENCODE_API_FUNCTION_LIST::nvEncDestroyBitstreamBuffer>;
using NvencInput =
    NvencChild<NV_ENC_INPUT_PTR, &NV_ENCODE_API_FUNCTION_LIST::nvEncUnmapInputResource>;
// The session is closed with a single-argument call, so it needs its own type.
class NvencSession {
public:
  NvencSession() = default;
  NvencSession(const NvencSession &) = delete;
  NvencSession &operator=(const NvencSession &) = delete;
  ~NvencSession() {
    reset();
  }
  void adopt(const NV_ENCODE_API_FUNCTION_LIST *api, void *session) {
    reset();
    api_ = api;
    session_ = session;
  }
  void reset() {
    if (session_) {
      api_->nvEncDestroyEncoder(session_);
      session_ = nullptr;
    }
  }
  // Address for nvEncOpenEncodeSessionEx, which writes the session out. The
  // function list is taken here because creation bypasses adopt().
  void **out(const NV_ENCODE_API_FUNCTION_LIST *api) {
    reset();
    api_ = api;
    return &session_;
  }
  operator void *() const {
    return session_;
  }
  void *get() const {
    return session_;
  }

private:
  const NV_ENCODE_API_FUNCTION_LIST *api_{};
  void *session_{};
};
// NVENC session state. Member order matters: the per-slot resources are
// unregistered and destroyed before the session is closed, and the library
// stays loaded until both are done.
struct NvencState {
  struct Resource {
    NvencRegistered registered;
    NvencBitstream bitstream;
    // Per-frame mapping; released by Gpu::unlock(), not by destruction.
    NvencInput mapped;
  };
  // Declared first so it is released last: the function list points into it.
  std::unique_ptr<DynamicLibrary> lib;
  NV_ENCODE_API_FUNCTION_LIST api{};
  NvencSession session;
  unsigned currentBitrate = 6000000;
  NV_ENC_CONFIG config{};
  NV_ENC_INITIALIZE_PARAMS init{};
  std::array<Resource, SLOT_COUNT> resources;
};
