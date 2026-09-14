// Viewer entry point: parse the options, bring up the GPU pipeline and the
// WebRTC peer, then run the fixed-rate frame loop until a signal or a failure.
#include "app/app.hpp"
#include "app/http.hpp"
#include "app/options.hpp"
#include "assets/library.hpp"
#include "core/common.hpp"
#include "core/env.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <httplib.h>
#include <iostream>
#include <stdexcept>
#include <thread>
static volatile std::sig_atomic_t stopRequested = 0;
static void signal_handler(int) {
  stopRequested = 1;
}
int main(int argc, char **argv) {
  try {
    Options options = parse_options(argc, argv);
    if (options.help) {
      print_usage();
      return 0;
    }
    auto models = list_models(options.library);
    if (models.empty())
      throw std::runtime_error("model library is empty");
    Gpu gpu(options.width, options.height, options.shaders);
    gpu.load(load_mesh(std::filesystem::path(options.library) / models.front(), options.library));
    App app(gpu);
    app.models = models;
    auto iceUrl = env("SERVER_ICE_URL"), iceUser = env("TURN_USER"), icePass = env("TURN_PASSWORD");
    RtcCallbacks callbacks{App::message, App::encode, App::encoded, App::release, App::rtcError};
    app.rtc =
        viewer_rtc_create(&app, callbacks, iceUrl.c_str(), iceUser.c_str(), icePass.c_str(), 0, 0);
    if (!app.rtc)
      throw std::runtime_error("WebRTC initialization failed");
    httplib::Server server;
    install_http_routes(server, app, options);
    if (!server.bind_to_port(options.bind, options.port)) {
      viewer_rtc_destroy(app.rtc);
      throw std::runtime_error("HTTP port unavailable");
    }
    std::thread http([&] { server.listen_after_bind(); });
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::cout << "Viewer ready: http://" << options.bind << ":" << options.port << " | library "
              << std::filesystem::absolute(options.library) << std::endl;
    uint64_t frame = 0;
    auto next = std::chrono::steady_clock::now();
    try {
      while (!stopRequested && !app.failed) {
        if (app.sessionExpired()) {
          viewer_rtc_close_peer(app.rtc);
          app.resetSession();
        }
        next += std::chrono::microseconds(33333);
        FrameInput input = app.takeFrameInput();
        if (!input.model.empty()) {
          try {
            auto mesh =
                load_mesh(std::filesystem::path(options.library) / input.model, options.library);
            gpu.load(mesh);
            app.send({{"type", "loaded"}, {"id", input.model}, {"triangles", mesh.size() / 3}});
          } catch (const std::exception &e) {
            app.error(e.what());
          }
        }
        if (app.active) {
          int slot = gpu.acquire();
          if (slot >= 0) {
            input.timing.frame = ++frame;
            gpu.render(slot, input.camera, input.timing);
            viewer_rtc_push(app.rtc, slot, options.width, options.height,
                            int64_t(now_ms() * 1000));
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
