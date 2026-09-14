#include "app/http.hpp"
#include "app/app.hpp"
#include "app/options.hpp"
#include "core/common.hpp"
#include "core/env.hpp"
#include <filesystem>
#include <fstream>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>
using nlohmann::json;
void install_http_routes(httplib::Server &server, App &app, const Options &options) {
  const std::string iceUser = env("TURN_USER"), icePass = env("TURN_PASSWORD");
  server.set_payload_max_length(1024 * 1024);
  server.set_read_timeout(20, 0);
  server.set_write_timeout(30, 0);
  // Without this an arbitrary page could drive the control endpoints of a
  // viewer that happens to be listening on the same host.
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
    server.Get(route, [web = options.web, name](const auto &, auto &res) {
      std::ifstream f(std::filesystem::path(web) / name, std::ios::binary);
      if (!f) {
        res.status = 404;
        return;
      }
      std::string contents((std::istreambuf_iterator<char>(f)), {});
      std::string ext = std::filesystem::path(name).extension().string();
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
                          {"width", options.width},
                          {"height", options.height},
                          {"gpu", app.gpu.deviceName},
                          {"codec", "H.264 NVENC"},
                          {"slots", SLOT_COUNT},
                          {"gpuCopies", app.gpu.slots[0].legacyInput ? 1 : 0}})
                        .dump(),
                    "application/json");
  });
  server.Get("/api/models", [&](const auto &, auto &res) {
    res.set_content(json(app.models).dump(), "application/json");
  });
  server.Get("/api/health", [&](const auto &, auto &res) {
    std::lock_guard lock(app.mutex);
    res.set_content(json({{"ok", !app.failed.load()},
                          {"active", app.active.load()},
                          {"gpu", app.gpu.deviceName},
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
        // Only the session that owns the peer may tear it down.
        if (j.value("session", std::string()) != app.sessionId) {
          res.status = 403;
          return;
        }
      }
      viewer_rtc_close_peer(app.rtc);
      app.resetSession();
      res.set_content("ok", "text/plain");
    } catch (...) {
      res.status = 400;
    }
  });
}
