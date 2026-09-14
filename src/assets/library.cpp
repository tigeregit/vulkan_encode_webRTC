#include "assets/library.hpp"
#include "assets/ply.hpp"
#include "core/mesh_fit.hpp"
#include <algorithm>
#include <assimp/DefaultIOSystem.h>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <cctype>
#include <cmath>
namespace fs = std::filesystem;
namespace {
bool inside(const fs::path &p, const fs::path &root) {
  auto a = root.begin(), b = p.begin();
  for (; a != root.end(); ++a, ++b)
    if (b == p.end() || *a != *b)
      return false;
  return true;
}
// Confines Assimp's sidecar file access (materials, textures) to the library
// directory.
class LibraryIO : public Assimp::DefaultIOSystem {
  fs::path root;
  bool allowed(const char *file) const {
    std::error_code ec;
    auto p = fs::weakly_canonical(file, ec);
    return !ec && inside(p, root);
  }

public:
  explicit LibraryIO(fs::path r) : root(std::move(r)) {}
  bool Exists(const char *file) const override {
    return allowed(file) && DefaultIOSystem::Exists(file);
  }
  Assimp::IOStream *Open(const char *file, const char *mode = "rb") override {
    return allowed(file) && !strchr(mode, 'w') ? DefaultIOSystem::Open(file, mode) : nullptr;
  }
};
} // namespace
std::vector<std::string> list_models(const fs::path &root) {
  std::vector<std::string> names;
  auto canonical = fs::canonical(root);
  for (const auto &e : fs::directory_iterator(root)) {
    if (!e.is_regular_file())
      continue;
    std::error_code ec;
    auto p = fs::canonical(e.path(), ec);
    if (ec || !inside(p, canonical))
      continue;
    auto ext = e.path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (ext == ".obj" || ext == ".stl" || ext == ".ply" || ext == ".gltf" || ext == ".glb")
      names.push_back(e.path().filename().string());
  }
  std::sort(names.begin(), names.end());
  return names;
}
std::vector<Vertex> load_mesh(const fs::path &path, const fs::path &root) {
  auto canonicalRoot = fs::canonical(root), p = fs::canonical(path);
  if (!inside(p, canonicalRoot) || fs::file_size(p) > 256ull * 1024 * 1024)
    throw std::runtime_error("model outside library or exceeds 256 MiB");
  if (p.extension() == ".ply" || p.extension() == ".PLY") {
    auto data = read_ply(p);
    fit_mesh(data);
    return data;
  }
  Assimp::Importer importer;
  importer.SetIOHandler(new LibraryIO(canonicalRoot));
  const auto *scene = importer.ReadFile(
      p.string(), aiProcess_Triangulate | aiProcess_GenSmoothNormals |
                      aiProcess_PreTransformVertices | aiProcess_ValidateDataStructure);
  if (!scene || !scene->HasMeshes())
    throw std::runtime_error(std::string("model import: ") + importer.GetErrorString());
  std::vector<Vertex> result;
  for (unsigned m = 0; m < scene->mNumMeshes; m++) {
    auto *mesh = scene->mMeshes[m];
    aiColor4D color(.25f, .72f, .85f, 1);
    if (mesh->mMaterialIndex < scene->mNumMaterials)
      aiGetMaterialColor(scene->mMaterials[mesh->mMaterialIndex], AI_MATKEY_COLOR_DIFFUSE, &color);
    for (unsigned f = 0; f < mesh->mNumFaces; f++) {
      const auto &face = mesh->mFaces[f];
      if (face.mNumIndices != 3)
        continue;
      if (result.size() + 3 > 15000000)
        throw std::runtime_error("model exceeds 5 million triangles");
      for (unsigned k = 0; k < 3; k++) {
        auto j = face.mIndices[k];
        auto v = mesh->mVertices[j];
        auto n = mesh->HasNormals() ? mesh->mNormals[j] : aiVector3D(0, 1, 0);
        auto c = mesh->HasVertexColors(0) ? mesh->mColors[0][j] : color;
        if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
          throw std::runtime_error("nonfinite model vertex");
        result.push_back({{v.x, v.y, v.z}, {n.x, n.y, n.z}, {c.r, c.g, c.b, c.a}});
      }
    }
  }
  if (result.empty())
    throw std::runtime_error("no triangles; point-only PLY is not supported");
  fit_mesh(result);
  return result;
}
