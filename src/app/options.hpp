#pragma once
#include <string>
struct Options {
  bool help = false;
  std::string library = "models", web = "web", shaders = "build/shaders", bind = "127.0.0.1";
  int port = 8080, width = 1280, height = 720;
};
// Parses the command line. Throws std::runtime_error on unknown or malformed
// options; --help sets Options::help rather than exiting, so main() stays in
// control of the process.
Options parse_options(int argc, char **argv);
void print_usage();
