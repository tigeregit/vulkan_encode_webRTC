# Model import: the hand-written PLY parser plus the Assimp bridge, both
# confined to the model library directory.
add_library(viewer_assets STATIC ply_parse.cpp ply.cpp library.cpp)
# Assimp is an implementation detail of library.cpp, so its headers stay private.
target_link_libraries(viewer_assets PUBLIC viewer_core PRIVATE assimp::assimp)
