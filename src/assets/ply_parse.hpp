#pragma once
// Low-level PLY scalar decoding, split out of ply.cpp so the header/assembly
// logic there stays readable.
#include <cstddef>
#include <istream>
#include <string>
struct PlyType {
  int bytes;
  bool floating;
  bool sign;
};
// Maps a PLY type name to its width and signedness. Throws on unknown names.
PlyType ply_type(const std::string &name);
// Reads one scalar: whitespace-separated for ASCII, fixed width otherwise.
// Throws on truncation or non-finite ASCII input.
double ply_scalar(std::istream &in, PlyType t, bool ascii, bool swap);
// Validates a non-negative integral value within [0, limit].
size_t ply_integer(double v, size_t limit);
