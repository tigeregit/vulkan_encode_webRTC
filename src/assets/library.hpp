#pragma once
#include "core/mesh.hpp"
#include <filesystem>
#include <string>
#include <vector>
// Lists supported model files directly inside the library directory.
std::vector<std::string> list_models(const std::filesystem::path &root);
// Loads a model from the library and returns it normalized for the viewer.
// Throws when the path escapes the library, the file is too large, or the mesh
// cannot be triangulated.
std::vector<Vertex> load_mesh(const std::filesystem::path &path, const std::filesystem::path &root);
