#include "gpu.hpp"
#include "library.hpp"
#include "rtc_bridge.h"
#include <algorithm>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <csignal>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
using json = nlohmann::json;
static volatile std::sig_atomic_t stopRequested = 0;
static void signal_handler(int) {
  stopRequested = 1;
}
static std::string env(const char *key) {
  const char *v = std::getenv(key);
  return v ? v : "";
}
struct App {
  Gpu &gpu;
  void *rtc = nullptr;
  std::mutex mutex;
  Camera camera;
  FrameTiming control;
  std::string pendingModel, lastError, sessionId;
  std::atomic<double> lastActivity{0};
  std::atomic<bool> active{false}, failed{false};
  std::vector<std::string> models;
  explicit App(Gpu &g) : gpu(g) {}
  void send(const json &j) {
    if (rtc)
      viewer_rtc_send(rtc, j.dump().c_str());
  }
  void error(const std::string &e) {
    std::cerr << e << '\n';
    {
      std::lock_guard lock(mutex);
      lastError = e;
    }
    send({{"type", "error"}, {"message", e}});
  }
  static void message(void *ptr, const char *text, size_t size) {
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
  static int encode(void *ptr, int slot, int key, unsigned bitrate, uint32_t,
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
  static void encoded(void *ptr, int slot, uint32_t rtp) {
    auto &a = *static_cast<App *>(ptr);
    try {
      auto t = a.gpu.slots[slot].timing;
      a.gpu.unlock(slot);
      a.send({{"type", "frame"},
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
              {"encodeMs", t.encodeEnd - t.encodeStart}});
    } catch (const std::exception &e) {
      a.failed = true;
      a.error(e.what());
    }
  }
  static void release(void *ptr, int slot) {
    static_cast<App *>(ptr)->gpu.release(slot);
  }
  static void rtcError(void *ptr, const char *msg) {
    static_cast<App *>(ptr)->error(msg);
  }
};
int main(int argc, char **argv) {
  try {
    std::string library = "models", web = "web", shaders = "build/shaders", bind = "127.0.0.1";
    int port = 8080, w = 1280, h = 720;
    for (int i = 1; i < argc; i++) {
      std::string arg = argv[i];
      if (arg == "--help") {
        std::cout << "viewer [--models DIR] [--web DIR] [--shaders DIR] [--bind HOST] [--port N] "
                     "[--width N] [--height N]\n";
        return 0;
      }
      if (i + 1 >= argc)
        throw std::runtime_error("missing option value");
      std::string v = argv[++i];
      if (arg == "--models")
        library = v;
      else if (arg == "--web")
        web = v;
      else if (arg == "--shaders")
        shaders = v;
      else if (arg == "--bind")
        bind = v;
      else if (arg == "--port")
        port = std::stoi(v);
      else if (arg == "--width")
        w = std::stoi(v);
      else if (arg == "--height")
        h = std::stoi(v);
      else
        throw std::runtime_error("unknown option " + arg);
    }
    auto models = list_models(library);
    if (models.empty())
      throw std::runtime_error("model library is empty");
    Gpu gpu(w, h, shaders);
    gpu.load(load_mesh(std::filesystem::path(library) / models.front(), library));
    App app(gpu);
    app.models = models;
    auto iceUrl = env("SERVER_ICE_URL"), iceUser = env("TURN_USER"), icePass = env("TURN_PASSWORD");
    RtcCallbacks callbacks{App::message, App::encode, App::encoded, App::release, App::rtcError};
    app.rtc =
        viewer_rtc_create(&app, callbacks, iceUrl.c_str(), iceUser.c_str(), icePass.c_str(), 0, 0);
    if (!app.rtc)
      throw std::runtime_error("WebRTC initialization failed");
    httplib::Server server;
    server.set_payload_max_length(1024 * 1024);
    server.set_read_timeout(20, 0);
    server.set_write_timeout(30, 0);
    server.set_pre_routing_handler([](const httplib::Request &req, httplib::Response &res) {
      const auto origin = req.get_header_value("Origin");
      if (!origin.empty() && origin != "http://" + req.get_header_value("Host") &&
          origin != "https://" + req.get_header_value("Host")) {
        res.status = 403;
        res.set_content("origin denied", "text/plain");
        return httplib::Server::HandlerResponse::Handled;
      }
      return httplib::Server::HandlerResponse::Unhandled;
    });
    for (auto name : {"index.html", "app.js", "style.css"}) {
      std::string route = std::string(name) == "index.html" ? "/" : "/" + std::string(name);
      server.Get(route, [web, name](const auto &, auto &res) {
        std::ifstream f(std::filesystem::path(web) / name, std::ios::binary);
        if (!f) {
          res.status = 404;
          return;
        }
        std::string contents((std::istreambuf_iterator<char>(f)), {});
        std::string ext = std::filesystem::path(name).extension();
        res.set_header("Cache-Control", "no-store");
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_content(contents, ext == ".js"    ? "text/javascript"
                                  : ext == ".css" ? "text/css"
                                                  : "text/html");
      });
    }
    server.Get("/favicon.ico", [](const auto &, auto &res) { res.status = 204; });
    server.Get("/api/config", [&](const auto &, auto &res) {
      json ice = json::array();
      auto url = env("BROWSER_ICE_URL");
      if (!url.empty())
        ice.push_back({{"urls", url}, {"username", iceUser}, {"credential", icePass}});
      res.set_header("Cache-Control", "no-store");
      res.set_content(json({{"iceServers", ice},
                            {"width", w},
                            {"height", h},
                            {"gpu", gpu.deviceName},
                            {"codec", "H.264 NVENC"},
                            {"slots", SLOT_COUNT},
                            {"gpuCopies", gpu.slots[0].legacyInput ? 1 : 0}})
                          .dump(),
                      "application/json");
    });
    server.Get("/api/models", [&](const auto &, auto &res) {
      res.set_content(json(models).dump(), "application/json");
    });
    server.Get("/api/health", [&](const auto &, auto &res) {
      std::lock_guard lock(app.mutex);
      res.set_content(json({{"ok", !app.failed.load()},
                            {"active", app.active.load()},
                            {"gpu", gpu.deviceName},
                            {"error", app.lastError}})
                          .dump(),
                      "application/json");
    });
    server.Post("/api/offer", [&](const auto &req, auto &res) {
      {
        std::lock_guard lock(app.mutex);
        bool expected = false;
        if (!app.active.compare_exchange_strong(expected, true)) {
          res.status = 409;
          res.set_content("viewer already in use; disconnect the active session", "text/plain");
          return;
        }
        app.lastActivity = now_ms();
      }
      try {
        auto offer = json::parse(req.body);
        std::string session = offer.at("session");
        if (session.size() < 16 || session.size() > 128)
          throw std::runtime_error("invalid session identifier");
        {
          std::lock_guard lock(app.mutex);
          app.sessionId = session;
        }
        app.lastActivity = now_ms();
        std::string sdp = offer.at("sdp");
        std::vector<char> answer(256 * 1024);
        if (viewer_rtc_offer(app.rtc, sdp.c_str(), answer.data(), answer.size()))
          throw std::runtime_error(answer.data());
        res.set_content(json({{"type", "answer"}, {"sdp", answer.data()}}).dump(),
                        "application/json");
      } catch (const std::exception &e) {
        app.active = false;
        res.status = 400;
        res.set_content(e.what(), "text/plain");
      }
    });
    server.Post("/api/disconnect", [&](const auto &req, auto &res) {
      try {
        auto j = json::parse(req.body);
        {
          std::lock_guard lock(app.mutex);
          if (j.value("session", std::string()) != app.sessionId) {
            res.status = 403;
            return;
          }
        }
        viewer_rtc_close_peer(app.rtc);
        app.active = false;
        {
          std::lock_guard lock(app.mutex);
          app.control = {};
          app.sessionId.clear();
        }
        res.set_content("ok", "text/plain");
      } catch (...) {
        res.status = 400;
      }
    });
    if (!server.bind_to_port(bind, port)) {
      viewer_rtc_destroy(app.rtc);
      throw std::runtime_error("HTTP port unavailable");
    }
    std::thread http([&] { server.listen_after_bind(); });
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::cout << "Viewer ready: http://" << bind << ":" << port << " | library "
              << std::filesystem::absolute(library) << std::endl;
    uint64_t frame = 0;
    auto next = std::chrono::steady_clock::now();
    try {
      while (!stopRequested && !app.failed) {
        bool expired;
        {
          std::lock_guard lock(app.mutex);
          expired = app.active && now_ms() - app.lastActivity.load() > 30000;
        }
        if (expired) {
          viewer_rtc_close_peer(app.rtc);
          std::lock_guard lock(app.mutex);
          app.active = false;
          app.control = {};
          app.sessionId.clear();
        }
        next += std::chrono::microseconds(33333);
        std::string model;
        Camera camera;
        FrameTiming timing;
        {
          std::lock_guard lock(app.mutex);
          model.swap(app.pendingModel);
          camera = app.camera;
          if (app.control.received && !app.control.applied)
            app.control.applied = now_ms();
          timing = app.control;
        }
        if (!model.empty()) {
          try {
            auto mesh = load_mesh(std::filesystem::path(library) / model, library);
            gpu.load(mesh);
            app.send({{"type", "loaded"}, {"id", model}, {"triangles", mesh.size() / 3}});
          } catch (const std::exception &e) {
            app.error(e.what());
          }
        }
        if (app.active) {
          int slot = gpu.acquire();
          if (slot >= 0) {
            timing.frame = ++frame;
            gpu.render(slot, camera, timing);
            viewer_rtc_push(app.rtc, slot, w, h, int64_t(now_ms() * 1000));
          }
        }
        std::this_thread::sleep_until(next);
        if (next < std::chrono::steady_clock::now() - std::chrono::milliseconds(100))
          next = std::chrono::steady_clock::now();
      }
    } catch (const std::exception &e) {
      app.failed = true;
      app.error(e.what());
    }
    server.stop();
    http.join();
    viewer_rtc_destroy(app.rtc);
    app.rtc = nullptr;
    return app.failed ? 1 : 0;
  } catch (const std::exception &e) {
    std::cerr << "fatal: " << e.what() << '\n';
    return 1;
  }
}
