# Google libwebrtc bridge. The public surface is the plain-C rtc_bridge.h; the
# three sources are rtc_bridge.cpp (exports), rtc_context.cpp (PeerConnection)
# and rtc_encoder.cpp (native frame buffer + encoder).
if(WIN32)
  if(NOT EXISTS "${WEBRTC_ROOT}/lib/webrtc.lib")
    message(FATAL_ERROR "Set WEBRTC_ROOT to matching Windows x86_64 package")
  endif()
  add_library(viewer_rtc SHARED rtc_bridge.cpp rtc_context.cpp rtc_encoder.cpp)
  target_include_directories(
    viewer_rtc PRIVATE "${PROJECT_SOURCE_DIR}/src" "${WEBRTC_ROOT}/include"
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
  # The pinned libwebrtc package is built against the static CRT (/MT); the
  # bridge must use the same runtime. The plain C boundary keeps the rest of
  # the viewer on /MD.
  set_property(TARGET viewer_rtc PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
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
  # C boundary. Every source must be listed both in the command and in DEPENDS;
  # a source missing from DEPENDS is silently not rebuilt on change. Paths are
  # absolute because the custom command runs from this directory's build dir.
  set(RTC_SOURCES
      "${CMAKE_CURRENT_SOURCE_DIR}/rtc_bridge.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/rtc_context.cpp"
      "${CMAKE_CURRENT_SOURCE_DIR}/rtc_encoder.cpp")
  set(RTC_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/rtc_bridge.h"
                  "${CMAKE_CURRENT_SOURCE_DIR}/rtc_internal.hpp")
  set(RTC_OUTPUT "${CMAKE_BINARY_DIR}/libviewer_rtc.so")
  add_custom_command(
    OUTPUT "${RTC_OUTPUT}"
    COMMAND
      "${RTC_CLANG}" -std=c++20 -O2 -fPIC -shared -frtti -fno-exceptions
      -fvisibility=hidden -nostdinc++ -nostdlib++ -fuse-ld=lld
      -Wno-deprecated-attributes -Wno-nullability-completeness -DWEBRTC_POSIX
      -DWEBRTC_LINUX -DNDEBUG
      -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_NONE
      -I${PROJECT_SOURCE_DIR}/src -I${CMAKE_SOURCE_DIR}/cmake -I${LIBCXX_INCLUDE}
      -I${WEBRTC_ROOT}/include/third_party/abseil-cpp -I${WEBRTC_ROOT}/include
      ${RTC_SOURCES} ${WEBRTC_ROOT}/lib/libwebrtc.a -Wl,-Bsymbolic
      -Wl,--exclude-libs,ALL -Wl,-z,defs -lpthread -ldl -lm -lrt -lX11 -o
      ${RTC_OUTPUT}
    DEPENDS ${RTC_SOURCES} ${RTC_HEADERS} "${CMAKE_SOURCE_DIR}/cmake/__config_site"
            "${CMAKE_SOURCE_DIR}/cmake/__assertion_handler"
    VERBATIM)
  add_custom_target(rtc_bridge_build DEPENDS "${RTC_OUTPUT}")
  add_library(viewer_rtc SHARED IMPORTED)
  set_target_properties(viewer_rtc PROPERTIES IMPORTED_LOCATION "${RTC_OUTPUT}")
  add_dependencies(viewer_rtc rtc_bridge_build)
endif()
