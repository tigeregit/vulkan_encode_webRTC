#include "ply.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>
namespace {
struct Type {
  int bytes;
  bool floating;
  bool sign;
};
Type type(const std::string &s) {
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
struct Property {
  std::string name;
  Type value;
  bool list = false;
  Type count{0, false, false};
};
struct Element {
  std::string name;
  size_t count;
  std::vector<Property> properties;
};
double scalar(std::istream &in, Type t, bool ascii, bool swap) {
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
size_t integer(double v, size_t limit) {
  if (!std::isfinite(v) || v < 0 || std::floor(v) != v || v > double(limit))
    throw std::runtime_error("invalid PLY count/index");
  return size_t(v);
}
} // namespace
std::vector<Vertex> read_ply(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  std::string line;
  std::getline(in, line);
  if (line != "ply" && line != "ply\r")
    throw std::runtime_error("invalid PLY magic");
  bool ascii = false, swap = false, format = false, ended = false;
  std::vector<Element> elements;
  size_t headerBytes = 0;
  while (std::getline(in, line)) {
    headerBytes += line.size() + 1;
    if (headerBytes > 1024 * 1024)
      throw std::runtime_error("PLY header too large");
    std::istringstream s(line);
    std::string word;
    s >> word;
    if (word == "end_header") {
      ended = true;
      break;
    }
    if (word == "format") {
      std::string f, v;
      s >> f >> v;
      if (v != "1.0")
        throw std::runtime_error("PLY version must be 1.0");
      ascii = f == "ascii";
      if (!ascii && f != "binary_little_endian" && f != "binary_big_endian")
        throw std::runtime_error("unsupported PLY encoding");
      swap =
          !ascii && ((f == "binary_little_endian") != (std::endian::native == std::endian::little));
      format = true;
    } else if (word == "element") {
      std::string n;
      double c;
      s >> n >> c;
      if (!s)
        throw std::runtime_error("invalid PLY element");
      elements.push_back({n, integer(c, 15000000), {}});
    } else if (word == "property") {
      if (elements.empty())
        throw std::runtime_error("PLY property without element");
      std::string t, n;
      s >> t;
      Property p;
      if (t == "list") {
        std::string count;
        s >> count >> t >> n;
        p = {n, type(t), true, type(count)};
        if (p.count.floating)
          throw std::runtime_error("floating PLY list count");
      } else {
        s >> n;
        p = {n, type(t), false, {0, false, false}};
      }
      if (!s || n.empty())
        throw std::runtime_error("invalid PLY property");
      elements.back().properties.push_back(p);
    }
  }
  if (!format || !ended)
    throw std::runtime_error("incomplete PLY header");
  std::vector<Vertex> vertices;
  std::vector<std::array<uint32_t, 3>> faces;
  bool hasNormals = false, hasVertices = false;
  for (const auto &e : elements) {
    bool vertex = e.name == "vertex", face = e.name == "face";
    if (vertex) {
      if (hasVertices)
        throw std::runtime_error("duplicate PLY vertex element");
      hasVertices = true;
      std::unordered_map<std::string, int> names;
      for (auto &p : e.properties)
        names[p.name]++;
      for (auto name : {"x", "y", "z"})
        if (names[name] != 1)
          throw std::runtime_error("PLY requires unique x y z");
      hasNormals = names["nx"] && names["ny"] && names["nz"];
      vertices.reserve(e.count);
    }
    if (face)
      faces.reserve(e.count);
    for (size_t row = 0; row < e.count; row++) {
      Vertex v{{0, 0, 0}, {0, 0, 0}, {.25f, .72f, .85f, 1}};
      bool gotFace = false;
      for (const auto &p : e.properties) {
        if (p.list) {
          size_t count = integer(scalar(in, p.count, ascii, swap), 1000000);
          bool indices = face && (p.name == "vertex_indices" || p.name == "vertex_index");
          if (indices && count != 3)
            throw std::runtime_error("PLY viewer requires triangle faces");
          std::array<uint32_t, 3> f{};
          for (size_t k = 0; k < count; k++) {
            double value = scalar(in, p.value, ascii, swap);
            if (indices)
              f[k] = uint32_t(integer(value, 14999999));
          }
          if (indices) {
            if (gotFace)
              throw std::runtime_error("duplicate PLY face indices");
            faces.push_back(f);
            gotFace = true;
          }
          continue;
        }
        double value = scalar(in, p.value, ascii, swap);
        if (!std::isfinite(value))
          throw std::runtime_error("nonfinite PLY property");
        if (!vertex)
          continue;
        const auto &name = p.name;
        if (name == "x")
          v.position.x = value;
        else if (name == "y")
          v.position.y = value;
        else if (name == "z")
          v.position.z = value;
        else if (name == "nx")
          v.normal.x = value;
        else if (name == "ny")
          v.normal.y = value;
        else if (name == "nz")
          v.normal.z = value;
        else if (name == "r" || name == "red" || name == "g" || name == "green" || name == "b" ||
                 name == "blue" || name == "a" || name == "alpha") {
          int channel = (name == "r" || name == "red")     ? 0
                        : (name == "g" || name == "green") ? 1
                        : (name == "b" || name == "blue")  ? 2
                                                           : 3;
          double max = p.value.floating ? 1. : 255.;
          if (value < 0 || value > max)
            throw std::runtime_error("PLY colors must be uchar 0..255 or float 0..1");
          v.color[channel] = float(value / max);
        } else if (name == "rgba" || name == "rgb") {
          if (p.value.floating || p.value.bytes != 4 || p.value.sign)
            throw std::runtime_error("packed PLY rgba/rgb must be uint32 0xAARRGGBB");
          uint32_t rgba = uint32_t(integer(value, UINT32_MAX));
          v.color = {float((rgba >> 16) & 255) / 255, float((rgba >> 8) & 255) / 255,
                     float(rgba & 255) / 255, name == "rgba" ? float(rgba >> 24) / 255 : 1.f};
        }
      }
      if (vertex) {
        for (int k = 0; k < 3; k++)
          if (!std::isfinite(v.position[k]) || !std::isfinite(v.normal[k]))
            throw std::runtime_error("PLY float overflow");
        vertices.push_back(v);
      }
      if (face && !gotFace)
        throw std::runtime_error("PLY face missing vertex_indices");
    }
  }
  if (vertices.empty() || faces.empty())
    throw std::runtime_error("PLY must contain vertices and triangle faces");
  if (faces.size() > 5000000)
    throw std::runtime_error("PLY exceeds 5 million triangles");
  std::vector<Vertex> result;
  result.reserve(faces.size() * 3);
  for (auto f : faces) {
    for (auto i : f)
      if (i >= vertices.size())
        throw std::runtime_error("PLY face index out of bounds");
    auto normal = glm::cross(vertices[f[1]].position - vertices[f[0]].position,
                             vertices[f[2]].position - vertices[f[0]].position);
    float length = glm::length(normal);
    if (length < 1e-12f)
      continue;
    normal /= length;
    for (auto i : f) {
      auto v = vertices[i];
      float n = glm::length(v.normal);
      v.normal = hasNormals && n > 1e-12f ? v.normal / n : normal;
      result.push_back(v);
    }
  }
  if (result.empty())
    throw std::runtime_error("PLY contains only degenerate faces");
  return result;
}
