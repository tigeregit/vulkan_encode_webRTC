#include "app/app.hpp"
#include "core/common.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
using nlohmann::json;
namespace {
json timing_json(const FrameTiming &t, uint32_t rtp) {
  return {{"type", "frame"},
          {"frame", t.frame},
          {"seq", t.seq},
          {"rtp", rtp},
          {"client", t.client},
          {"received", t.received},
          {"applied", t.applied},
          {"submit", t.submit},
          {"renderMs", t.renderMs},
          {"convertMs", t.convertMs},
          {"encodeStart", t.encodeStart},
          {"encodeEnd", t.encodeEnd},
          {"encodeMs", t.encodeEnd - t.encodeStart}};
}
} // namespace
App::App(Gpu &g) : gpu(g) {}
void App::send(const json &j) {
  if (rtc)
    viewer_rtc_send(rtc, j.dump().c_str());
}
void App::error(const std::string &e) {
  std::cerr << e << '\n';
  {
    std::lock_guard lock(mutex);
    lastError = e;
  }
  send({{"type", "error"}, {"message", e}});
}
void App::resetSession() {
  std::lock_guard lock(mutex);
  active = false;
  control = {};
  sessionId.clear();
}
bool App::sessionExpired() {
  std::lock_guard lock(mutex);
  return active && now_ms() - lastActivity.load() > 30000;
}
FrameInput App::takeFrameInput() {
  std::lock_guard lock(mutex);
  FrameInput input;
  input.model.swap(pendingModel);
  input.camera = camera;
  if (control.received && !control.applied)
    control.applied = now_ms();
  input.timing = control;
  return input;
}
void App::message(void *ptr, const char *text, size_t size) {
  auto &a = *static_cast<App *>(ptr);
  double received = now_ms();
  a.lastActivity = received;
  try {
    auto j = json::parse(text, text + size);
    std::string type = j.at("type");
    if (type == "ping") {
      a.send({{"type", "pong"},
              {"client", j.at("client")},
              {"received", received},
              {"sent", now_ms()}});
      return;
    }
    if (type == "load") {
      std::string id = j.at("id");
      std::lock_guard lock(a.mutex);
      if (std::find(a.models.begin(), a.models.end(), id) == a.models.end())
        throw std::runtime_error("unknown library model");
      a.pendingModel = id;
      return;
    }
    if (type != "camera")
      return;
    Camera cam;
    cam.yaw = j.at("yaw");
    cam.pitch = j.at("pitch");
    cam.distance = j.at("distance");
    cam.panX = j.at("panX");
    cam.panY = j.at("panY");
    double client = j.at("client");
    uint64_t seq = j.at("seq");
    if (!std::isfinite(cam.yaw) || !std::isfinite(cam.pitch) || !std::isfinite(cam.distance) ||
        !std::isfinite(cam.panX) || !std::isfinite(cam.panY) || !std::isfinite(client))
      throw std::runtime_error("nonfinite camera input");
    cam.yaw = std::remainder(cam.yaw, 6.2831853f);
    cam.pitch = std::clamp(cam.pitch, -1.5f, 1.5f);
    cam.distance = std::clamp(cam.distance, .3f, 25.f);
    cam.panX = std::clamp(cam.panX, -10.f, 10.f);
    cam.panY = std::clamp(cam.panY, -10.f, 10.f);
    std::lock_guard lock(a.mutex);
    // Superseded controls are merged away rather than queued.
    if (seq <= a.control.seq)
      return;
    a.camera = cam;
    a.control.seq = seq;
    a.control.client = client;
    a.control.received = received;
    a.control.applied = 0;
  } catch (const std::exception &e) {
    a.error(e.what());
  }
}
int App::encode(void *ptr, int slot, int key, unsigned bitrate, uint32_t,
                const unsigned char **data, size_t *size, int *isKey) {
  auto &a = *static_cast<App *>(ptr);
  try {
    return a.gpu.encode(slot, key, bitrate, data, size, isKey);
  } catch (const std::exception &e) {
    a.failed = true;
    a.error(e.what());
    return -1;
  }
}
void App::encoded(void *ptr, int slot, uint32_t rtp) {
  auto &a = *static_cast<App *>(ptr);
  try {
    auto t = a.gpu.slots[slot].timing;
    a.gpu.unlock(slot);
    a.send(timing_json(t, rtp));
  } catch (const std::exception &e) {
    a.failed = true;
    a.error(e.what());
  }
}
void App::release(void *ptr, int slot) {
  static_cast<App *>(ptr)->gpu.release(slot);
}
void App::rtcError(void *ptr, const char *msg) {
  static_cast<App *>(ptr)->error(msg);
}
