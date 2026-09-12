#pragma once
#include "gpu.hpp"
#include <filesystem>
std::vector<Vertex> load_mesh(const std::filesystem::path &path, const std::filesystem::path &root);
std::vector<std::string> list_models(const std::filesystem::path &root);
