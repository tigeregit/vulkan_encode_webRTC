# Targets are grouped by responsibility:
#   core   -- CPU-only shared types and geometry helpers (no GPU headers)
#   assets -- model import and library sandboxing
#   render -- Vulkan rendering, Vulkan<->CUDA interop, NVENC encoding
#   rtc    -- ABI-isolated Google WebRTC bridge
#   app    -- option parsing, viewer state and HTTP endpoints, plus the exe
add_subdirectory(core)
add_subdirectory(assets)
add_subdirectory(render)
add_subdirectory(rtc)
add_subdirectory(app)
option(BUILD_CONTAINER_GPU_FILTER
       "Build optional NVIDIA 570/580 enumeration workaround" OFF)
if(BUILD_CONTAINER_GPU_FILTER AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(container_gpu_filter SHARED container_gpu_filter.c)
  # Keep the documented LD_PRELOAD path: build/libcontainer_gpu_filter.so.
  set_target_properties(container_gpu_filter
                        PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
  target_link_libraries(container_gpu_filter PRIVATE ${CMAKE_DL_LIBS}
                                                     Threads::Threads)
endif()
