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
# Targets are defined in subdirectories, but the executables and the RTC shared
# library keep the historical top-level layout (build/<Config>/ on multi-config
# generators, build/ otherwise) so scripts/ and the README paths stay valid.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
find_package(CUDAToolkit 11.2 REQUIRED)
find_package(Threads REQUIRED)
if(WIN32)
  # Self-contained Windows dependencies under deps/: bundled Vulkan-Headers and
  # Vulkan-Loader sources, Assimp/GLM/nlohmann-json sources. No Vulkan SDK,
  # vcpkg, or system package installation is required. The Vulkan import
  # library is generated from the official loader vulkan-1.def; vulkan-1.dll
  # itself is installed by the GPU driver.
  set(VIEWER_VULKAN_HEADERS
      "${CMAKE_SOURCE_DIR}/deps/vulkan/Vulkan-Headers-vulkan-sdk-1.4.357.0"
      CACHE PATH "Bundled Vulkan-Headers source tree")
  set(VIEWER_VULKAN_DEF
      "${CMAKE_SOURCE_DIR}/deps/vulkan/Vulkan-Loader-vulkan-sdk-1.4.357.0/loader/vulkan-1.def"
      CACHE FILEPATH "Bundled Vulkan-Loader module definition")
  foreach(dep "${VIEWER_VULKAN_HEADERS}/include/vulkan/vulkan.h"
              "${VIEWER_VULKAN_DEF}")
    if(NOT EXISTS "${dep}")
      message(
        FATAL_ERROR
          "Missing bundled dependency ${dep}; see README Windows section")
    endif()
  endforeach()
  set(VIEWER_VULKAN_IMPLIB "${CMAKE_BINARY_DIR}/vulkan/vulkan-1.lib")
  if(NOT EXISTS "${VIEWER_VULKAN_IMPLIB}")
    get_filename_component(viewer_msvc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(MSVC_LIB NAMES lib.exe HINTS "${viewer_msvc_bin}" REQUIRED)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/vulkan")
    execute_process(
      COMMAND "${MSVC_LIB}" /nologo /machine:x64 /def:${VIEWER_VULKAN_DEF}
              /out:${VIEWER_VULKAN_IMPLIB}
      RESULT_VARIABLE viewer_vulkan_result
      ERROR_VARIABLE viewer_vulkan_error)
    if(NOT viewer_vulkan_result EQUAL 0)
      message(FATAL_ERROR "lib.exe failed on vulkan-1.def: ${viewer_vulkan_error}")
    endif()
  endif()
  add_library(Vulkan::Vulkan UNKNOWN IMPORTED GLOBAL)
  set_target_properties(
    Vulkan::Vulkan
    PROPERTIES IMPORTED_LOCATION "${VIEWER_VULKAN_IMPLIB}"
               INTERFACE_INCLUDE_DIRECTORIES "${VIEWER_VULKAN_HEADERS}/include")

  set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
  set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory("${CMAKE_SOURCE_DIR}/deps/glm-1.0.2"
                   "${CMAKE_BINARY_DIR}/glm" EXCLUDE_FROM_ALL)
  set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
  set(JSON_Install OFF CACHE BOOL "" FORCE)
  add_subdirectory("${CMAKE_SOURCE_DIR}/deps/json-3.12.0"
                   "${CMAKE_BINARY_DIR}/nlohmann_json" EXCLUDE_FROM_ALL)
  set(BUILD_SHARED_LIBS OFF)
  set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
  set(ASSIMP_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
  set(ASSIMP_NO_EXPORT ON CACHE BOOL "" FORCE)
  set(ASSIMP_BUILD_ZLIB ON CACHE BOOL "" FORCE)
  set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
  set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF CACHE BOOL "" FORCE)
  set(ASSIMP_BUILD_OBJ_IMPORTER ON CACHE BOOL "" FORCE)
  set(ASSIMP_BUILD_PLY_IMPORTER ON CACHE BOOL "" FORCE)
  set(ASSIMP_BUILD_STL_IMPORTER ON CACHE BOOL "" FORCE)
  set(ASSIMP_BUILD_GLTF_IMPORTER ON CACHE BOOL "" FORCE)
  add_subdirectory("${CMAKE_SOURCE_DIR}/deps/assimp-5.4.3"
                   "${CMAKE_BINARY_DIR}/assimp" EXCLUDE_FROM_ALL)
else()
  find_package(Vulkan REQUIRED)
  find_package(assimp REQUIRED)
  find_package(glm REQUIRED)
  find_package(nlohmann_json REQUIRED)
endif()
# Configuration shared with the src/ subdirectories.
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
# Shaders must exist as a target before src/ because the viewer and the smoke
# test both depend on it.
# glslang 16+ ships the tool as `glslang`; the bundled Windows build lives in
# deps/glslang/bin.
find_program(GLSLANG NAMES glslangValidator glslang
             HINTS "${CMAKE_SOURCE_DIR}/deps/glslang/bin" REQUIRED)
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
enable_testing()
add_subdirectory(src)
add_subdirectory(tests)
