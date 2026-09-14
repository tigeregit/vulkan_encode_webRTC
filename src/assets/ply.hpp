#pragma once
#include "core/mesh.hpp"
#include <filesystem>
#include <vector>
// Parses a PLY triangle mesh. Returns raw vertex positions; callers that need
// the viewer's normalized scale must apply fit_mesh() themselves.
std::vector<Vertex> read_ply(const std::filesystem::path &path);
