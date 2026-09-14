#pragma once
#include "core/mesh.hpp"
#include "render/gpu.hpp"
#include "rtc/rtc_bridge.h"
#include <atomic>
#include <cstddef>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
// One frame's worth of input, snapshotted under the control lock so the frame
// loop never holds that lock while doing GPU work.
struct FrameInput {
  Camera camera;
  FrameTiming timing;
  std::string model;
};
// Owns the viewer's mutable state: the control channel callbacks, the latest
// camera/telemetry snapshot, the session bookkeeping and the model library.
// Every field guarded by `mutex` is documented at its point of use.
struct App {
  Gpu &gpu;
  void *rtc = nullptr;
  // Guards camera, control, pendingModel, lastError, sessionId.
  std::mutex mutex;
  Camera camera;
  FrameTiming control;
  std::string pendingModel, lastError, sessionId;
  std::atomic<double> lastActivity{0};
  std::atomic<bool> active{false}, failed{false};
  std::vector<std::string> models;
  explicit App(Gpu &g);
  void send(const nlohmann::json &j);
  void error(const std::string &e);
  // Snapshots the latest camera and telemetry, swaps out any pending model
  // request and stamps the control as applied.
  FrameInput takeFrameInput();
  // Drops the session and any queued control.
  void resetSession();
  // True when an active session has been silent for longer than the timeout.
  bool sessionExpired();
  static void message(void *ptr, const char *text, size_t size);
  static int encode(void *ptr, int slot, int key, unsigned bitrate, uint32_t rtp,
                    const unsigned char **data, size_t *size, int *isKey);
  static void encoded(void *ptr, int slot, uint32_t rtp);
  static void release(void *ptr, int slot);
  static void rtcError(void *ptr, const char *msg);
};
