# Headless Vulkan rendering, the Vulkan<->CUDA interop and the NVENC H.264
# encoder. Split by stage: gpu_vulkan / gpu_cuda / gpu_render / gpu_nvenc.
add_library(viewer_gpu STATIC platform.cpp gpu.cpp gpu_vulkan.cpp gpu_cuda.cpp
                             gpu_render.cpp gpu_nvenc.cpp convert.cu
                             handles.hpp)
target_link_libraries(
  viewer_gpu
  PUBLIC viewer_core Vulkan::Vulkan CUDA::cudart CUDA::cuda_driver
  PRIVATE ${CMAKE_DL_LIBS})
# nvEncodeAPI.h is only needed inside this directory.
target_include_directories(viewer_gpu PRIVATE "${THIRD_PARTY_HEADERS}")
if(WIN32)
  target_compile_definitions(viewer_gpu PUBLIC VK_USE_PLATFORM_WIN32_KHR
                                               NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
target_compile_options(
  viewer_gpu PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:--default-stream=per-thread>)
