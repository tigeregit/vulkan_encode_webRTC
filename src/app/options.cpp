#include "app/options.hpp"
#include <iostream>
#include <stdexcept>
void print_usage() {
  std::cout << "viewer [--models DIR] [--web DIR] [--shaders DIR] [--bind HOST] [--port N] "
               "[--width N] [--height N]\n";
}
Options parse_options(int argc, char **argv) {
  Options o;
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--help") {
      o.help = true;
      return o;
    }
    if (i + 1 >= argc)
      throw std::runtime_error("missing option value");
    std::string v = argv[++i];
    if (arg == "--models")
      o.library = v;
    else if (arg == "--web")
      o.web = v;
    else if (arg == "--shaders")
      o.shaders = v;
    else if (arg == "--bind")
      o.bind = v;
    else if (arg == "--port")
      o.port = std::stoi(v);
    else if (arg == "--width")
      o.width = std::stoi(v);
    else if (arg == "--height")
      o.height = std::stoi(v);
    else
      throw std::runtime_error("unknown option " + arg);
  }
  return o;
}
