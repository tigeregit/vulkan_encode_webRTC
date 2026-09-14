# Viewer executable: command line, session state, HTTP endpoints and the
# fixed-rate frame loop.
add_executable(viewer main.cpp app.cpp http.cpp options.cpp)
target_link_libraries(
  viewer PRIVATE viewer_gpu viewer_assets viewer_rtc
                 nlohmann_json::nlohmann_json Threads::Threads)
# httplib.h is only needed by the HTTP layer in this directory.
target_include_directories(viewer PRIVATE "${THIRD_PARTY_HEADERS}")
add_dependencies(viewer shaders)
set_target_properties(viewer PROPERTIES BUILD_RPATH "${CMAKE_BINARY_DIR}")
