# gpu_smoke drives the full Vulkan -> CUDA -> NVENC path and cycles all four
# slots; ply_test covers the pure parser and needs no GPU at all.
add_executable(gpu_smoke gpu_smoke.cpp)
target_link_libraries(gpu_smoke PRIVATE viewer_gpu viewer_assets)
add_dependencies(gpu_smoke shaders)
add_executable(ply_test ply_test.cpp)
target_link_libraries(ply_test PRIVATE viewer_assets)
add_test(NAME gpu_smoke
         COMMAND gpu_smoke ${CMAKE_SOURCE_DIR}/models/demo.obj
                 ${CMAKE_BINARY_DIR}/shaders ${CMAKE_BINARY_DIR}/smoke.h264)
add_test(NAME ply_formats COMMAND ply_test)
add_test(NAME gpu_ply_smoke
         COMMAND gpu_smoke ${CMAKE_SOURCE_DIR}/models/colored_torus.ply
                 ${CMAKE_BINARY_DIR}/shaders ${CMAKE_BINARY_DIR}/smoke-ply.h264)
