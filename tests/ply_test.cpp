#include "ply.hpp"
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
static void check(bool b) {
  if (!b)
    throw std::runtime_error("PLY assertion failed");
}
static void bytes(std::ostream &f, uint32_t x, bool big) {
  for (int i = 0; i < 4; i++)
    f.put(char((x >> (8 * (big ? 3 - i : i))) & 255));
}
static void fixture(const std::filesystem::path &p, int format, bool badIndex = false,
                    bool packed = false) {
  std::ofstream f(p, std::ios::binary);
  f << "ply\nformat "
    << (format == 0   ? "ascii"
        : format == 1 ? "binary_little_endian"
                      : "binary_big_endian")
    << " 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\nproperty "
       "float nx\nproperty float ny\nproperty float nz\n";
  if (packed)
    f << "property uint rgba\n";
  else
    f << "property uchar red\nproperty uchar green\nproperty uchar blue\nproperty uchar alpha\n";
  f << "element face 1\nproperty list uchar int vertex_indices\nend_header\n";
  for (int i = 0; i < 3; i++) {
    float values[] = {float(i == 1), float(i == 2), 0, 0, 0, 1};
    if (format == 0) {
      for (auto v : values)
        f << v << ' ';
      if (packed)
        f << 0x804080c0u << '\n';
      else
        f << "64 128 192 128\n";
    } else {
      for (auto v : values)
        bytes(f, std::bit_cast<uint32_t>(v), format == 2);
      if (packed)
        bytes(f, 0x804080c0u, format == 2);
      else
        for (auto c : {64, 128, 192, 128})
          f.put(char(c));
    }
  }
  if (format == 0)
    f << "3 0 1 " << (badIndex ? 99 : 2) << '\n';
  else {
    f.put(3);
    bytes(f, 0, format == 2);
    bytes(f, 1, format == 2);
    bytes(f, badIndex ? 99 : 2, format == 2);
  }
}
int main() {
  auto root = std::filesystem::temp_directory_path() /
              std::filesystem::path(
                  "viewer-ply-test-" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(root);
  try {
    auto p = root / "mesh.ply";
    for (int fmt = 0; fmt < 3; fmt++)
      for (bool packed : {false, true}) {
        fixture(p, fmt, false, packed);
        auto v = read_ply(p);
        check(v.size() == 3);
        check(v[1].position.x == 1);
        check(v[2].position.y == 1);
        check(v[0].normal.z == 1);
        check(std::abs(v[0].color.r - 64.f / 255) < 1e-6);
        check(std::abs(v[0].color.a - 128.f / 255) < 1e-6);
        fixture(p, fmt, true, packed);
        bool rejected = false;
        try {
          read_ply(p);
        } catch (...) {
          rejected = true;
        }
        check(rejected);
      }
    fixture(p, 1);
    std::filesystem::resize_file(p, std::filesystem::file_size(p) - 1);
    bool rejected = false;
    try {
      read_ply(p);
    } catch (...) {
      rejected = true;
    }
    check(rejected);
    std::filesystem::remove_all(root);
    std::cout << "PASS: ASCII / LE / BE, RGBA / packed, normals, indices, truncation\n";
    return 0;
  } catch (const std::exception &e) {
    std::filesystem::remove_all(root);
    std::cerr << e.what() << '\n';
    return 1;
  }
}
