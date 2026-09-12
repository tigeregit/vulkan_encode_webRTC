#pragma once
#include "gpu.hpp"
#include <filesystem>
std::vector<Vertex> read_ply(const std::filesystem::path &);
