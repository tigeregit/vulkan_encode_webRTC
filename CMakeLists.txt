cmake_minimum_required(VERSION 3.22)
# Ampere cubin + forward-compatible PTX, works on Ada and Blackwell via driver
# JIT.
if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES 86-real 86-virtual)
endif()
project(vulkan_encode_webRTC LANGUAGES C CXX CUDA)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
find_package(Vulkan REQUIRED)
find_package(CUDAToolkit 11.2 REQUIRED)
find_package(assimp REQUIRED)
find_package(glm REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)
set(WEBRTC_ROOT
    ""
    CACHE PATH "Pinned Google libwebrtc package")
set(LIBCXX_INCLUDE
    ""
    CACHE PATH "Full matching libc++ headers")
set(RTC_CLANG
    ""
    CACHE FILEPATH "Matching Chromium clang++")
set(THIRD_PARTY_HEADERS
    "${CMAKE_SOURCE_DIR}/deps/headers"
    CACHE PATH "nvEncodeAPI.h and httplib.h")
if(WIN32)
  if(NOT EXISTS "${WEBRTC_ROOT}/lib/webrtc.lib")
    message(FATAL_ERROR "Set WEBRTC_ROOT to matching Windows x86_64 package")
  endif()
  add_library(viewer_rtc SHARED src/rtc_bridge.cpp)
  target_include_directories(
    viewer_rtc PRIVATE "${WEBRTC_ROOT}/include"
                       "${WEBRTC_ROOT}/include/third_party/abseil-cpp")
  target_compile_definitions(viewer_rtc PRIVATE WEBRTC_WIN NOMINMAX
                                                WIN32_LEAN_AND_MEAN)
  target_link_libraries(
    viewer_rtc
    PRIVATE "${WEBRTC_ROOT}/lib/webrtc.lib"
            ws2_32
            secur32
            crypt32
            bcrypt
            iphlpapi
            winmm
            dmoguids
            wmcodecdspuuid
            msdmo
            amstrmid
            strmiids
            ole32
            oleaut32
            uuid
            shlwapi
            wininet
            advapi32
            user32
            gdi32)
  target_compile_options(viewer_rtc PRIVATE /GR)
else()
  if(NOT EXISTS "${WEBRTC_ROOT}/lib/libwebrtc.a")
    message(
      FATAL_ERROR "Set WEBRTC_ROOT to the extracted m152.7977.0.3 package")
  endif()
  if(NOT RTC_CLANG)
    message(
      FATAL_ERROR
        "Set RTC_CLANG to Chromium clang++; WebRTC uses its own libc++ ABI")
  endif()
  # Isolate libwebrtc's std::__Cr ABI from CUDA/Assimp's system C++ ABI using a
  # C boundary.
  set(RTC_OUTPUT "${CMAKE_BINARY_DIR}/libviewer_rtc.so")
  add_custom_command(
    OUTPUT "${RTC_OUTPUT}"
    COMMAND
      "${RTC_CLANG}" -std=c++20 -O2 -fPIC -shared -frtti -fno-exceptions
      -fvisibility=hidden -nostdinc++ -nostdlib++ -fuse-ld=lld
      -Wno-deprecated-attributes -Wno-nullability-completeness -DWEBRTC_POSIX
      -DWEBRTC_LINUX -DNDEBUG
      -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_NONE
      -I${CMAKE_SOURCE_DIR}/cmake -I${LIBCXX_INCLUDE}
      -I${WEBRTC_ROOT}/include/third_party/abseil-cpp -I${WEBRTC_ROOT}/include
      ${CMAKE_SOURCE_DIR}/src/rtc_bridge.cpp ${WEBRTC_ROOT}/lib/libwebrtc.a
      -Wl,-Bsymbolic -Wl,--exclude-libs,ALL -Wl,-z,defs -lpthread -ldl -lm -lrt
      -lX11 -o ${RTC_OUTPUT}
    DEPENDS src/rtc_bridge.cpp src/rtc_bridge.h cmake/__config_site
            cmake/__assertion_handler
    VERBATIM)
  add_custom_target(rtc_bridge_build DEPENDS "${RTC_OUTPUT}")
  add_library(viewer_rtc SHARED IMPORTED)
  set_target_properties(viewer_rtc PROPERTIES IMPORTED_LOCATION "${RTC_OUTPUT}")
  add_dependencies(viewer_rtc rtc_bridge_build)
endif()
add_library(viewer_gpu src/gpu.cpp src/convert.cu)
target_include_directories(viewer_gpu PUBLIC src "${THIRD_PARTY_HEADERS}")
target_link_libraries(
  viewer_gpu PUBLIC Vulkan::Vulkan CUDA::cudart CUDA::cuda_driver glm::glm
                    ${CMAKE_DL_LIBS})
target_compile_definitions(viewer_gpu PUBLIC GLM_FORCE_DEPTH_ZERO_TO_ONE)
if(WIN32)
  target_compile_definitions(viewer_gpu PUBLIC VK_USE_PLATFORM_WIN32_KHR
                                               NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
target_compile_options(
  viewer_gpu PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:--default-stream=per-thread>)
find_program(GLSLANG glslangValidator REQUIRED)
foreach(shader scene.vert scene.frag)
  add_custom_command(
    OUTPUT "${CMAKE_BINARY_DIR}/shaders/${shader}.spv"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/shaders"
    COMMAND ${GLSLANG} -V "${CMAKE_SOURCE_DIR}/shaders/${shader}" -o
            "${CMAKE_BINARY_DIR}/shaders/${shader}.spv"
    DEPENDS "shaders/${shader}"
    VERBATIM)
  list(APPEND SHADERS "${CMAKE_BINARY_DIR}/shaders/${shader}.spv")
endforeach()
add_custom_target(shaders ALL DEPENDS ${SHADERS})
add_executable(viewer src/main.cpp src/library.cpp src/ply.cpp)
target_link_libraries(
  viewer PRIVATE viewer_gpu viewer_rtc assimp::assimp
                 nlohmann_json::nlohmann_json Threads::Threads)
add_dependencies(viewer shaders)
set_target_properties(viewer PROPERTIES BUILD_RPATH "${CMAKE_BINARY_DIR}")
add_executable(gpu_smoke tests/gpu_smoke.cpp src/library.cpp src/ply.cpp)
target_link_libraries(gpu_smoke PRIVATE viewer_gpu assimp::assimp)
add_dependencies(gpu_smoke shaders)
enable_testing()
add_test(NAME gpu_smoke
         COMMAND gpu_smoke ${CMAKE_SOURCE_DIR}/models/demo.obj
                 ${CMAKE_BINARY_DIR}/shaders ${CMAKE_BINARY_DIR}/smoke.h264)

option(BUILD_CONTAINER_GPU_FILTER
       "Build optional NVIDIA 570/580 enumeration workaround" OFF)
if(BUILD_CONTAINER_GPU_FILTER AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_library(container_gpu_filter SHARED src/container_gpu_filter.c)
  target_link_libraries(container_gpu_filter PRIVATE ${CMAKE_DL_LIBS}
                                                     Threads::Threads)
endif()
add_executable(ply_test tests/ply_test.cpp src/ply.cpp)
target_include_directories(ply_test PRIVATE src "${THIRD_PARTY_HEADERS}")
target_link_libraries(ply_test PRIVATE Vulkan::Vulkan CUDA::cudart glm::glm)
add_test(NAME ply_formats COMMAND ply_test)
add_test(NAME gpu_ply_smoke
         COMMAND gpu_smoke ${CMAKE_SOURCE_DIR}/models/colored_torus.ply
                 ${CMAKE_BINARY_DIR}/shaders ${CMAKE_BINARY_DIR}/smoke-ply.h264)
