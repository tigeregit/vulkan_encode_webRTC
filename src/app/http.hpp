#pragma once
namespace httplib {
class Server;
} // namespace httplib
struct App;
struct Options;
// Registers the static web assets and the /api/* control endpoints, together
// with the same-origin check that guards them.
void install_http_routes(httplib::Server &server, App &app, const Options &options);
