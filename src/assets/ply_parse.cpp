#include "assets/ply_parse.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
PlyType ply_type(const std::string &s) {
  if (s == "char" || s == "int8")
    return {1, false, true};
  if (s == "uchar" || s == "uint8")
    return {1, false, false};
  if (s == "short" || s == "int16")
    return {2, false, true};
  if (s == "ushort" || s == "uint16")
    return {2, false, false};
  if (s == "int" || s == "int32")
    return {4, false, true};
  if (s == "uint" || s == "uint32")
    return {4, false, false};
  if (s == "float" || s == "float32")
    return {4, true, true};
  if (s == "double" || s == "float64")
    return {8, true, true};
  throw std::runtime_error("unsupported PLY scalar: " + s);
}
double ply_scalar(std::istream &in, PlyType t, bool ascii, bool swap) {
  if (ascii) {
    double v;
    if (!(in >> v) || !std::isfinite(v))
      throw std::runtime_error("invalid/truncated ASCII PLY");
    return v;
  }
  std::array<unsigned char, 8> b{};
  if (!in.read(reinterpret_cast<char *>(b.data()), t.bytes))
    throw std::runtime_error("truncated binary PLY");
  if (swap)
    std::reverse(b.begin(), b.begin() + t.bytes);
  if (t.floating) {
    if (t.bytes == 4) {
      float v;
      memcpy(&v, b.data(), 4);
      return v;
    }
    double v;
    memcpy(&v, b.data(), 8);
    return v;
  }
  if (t.sign) {
    if (t.bytes == 1) {
      int8_t v;
      memcpy(&v, b.data(), 1);
      return v;
    }
    if (t.bytes == 2) {
      int16_t v;
      memcpy(&v, b.data(), 2);
      return v;
    }
    int32_t v;
    memcpy(&v, b.data(), 4);
    return v;
  }
  if (t.bytes == 1)
    return b[0];
  if (t.bytes == 2) {
    uint16_t v;
    memcpy(&v, b.data(), 2);
    return v;
  }
  uint32_t v;
  memcpy(&v, b.data(), 4);
  return v;
}
size_t ply_integer(double v, size_t limit) {
  if (!std::isfinite(v) || v < 0 || std::floor(v) != v || v > double(limit))
    throw std::runtime_error("invalid PLY count/index");
  return size_t(v);
}
