cmake_minimum_required(VERSION 3.24)

project(TrackManiaForeverOpenXR LANGUAGES CXX)

# OpenXR: PC VR through an OpenXR runtime (MSVC on Windows).
# XrAPI:  standalone Quest/Pico through WinlatorXR's XrAPI (MSVC or MinGW).
set(TMFOXR_VR_BACKEND "OpenXR" CACHE STRING "VR backend: OpenXR or XrAPI")
set_property(CACHE TMFOXR_VR_BACKEND PROPERTY STRINGS OpenXR XrAPI)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "This project builds a 32-bit Win32 Direct3D 9 proxy.")
endif()
if(TMFOXR_VR_BACKEND STREQUAL "OpenXR" AND NOT MSVC)
  message(FATAL_ERROR "The OpenXR backend requires MSVC. Use -DTMFOXR_VR_BACKEND=XrAPI for MinGW builds.")
elseif(NOT TMFOXR_VR_BACKEND MATCHES "^(OpenXR|XrAPI)$")
  message(FATAL_ERROR "Unknown TMFOXR_VR_BACKEND '${TMFOXR_VR_BACKEND}'.")
endif()

include(FetchContent)
if(TMFOXR_VR_BACKEND STREQUAL "OpenXR")
  FetchContent_Declare(
    openxr_headers
    URL https://github.com/KhronosGroup/OpenXR-SDK-Source/archive/refs/tags/release-1.1.62.zip
    URL_HASH SHA256=FBEFD5E39E7D0D4C7D4552FDAB30A65AEB907D84B6C17AD5A06F7CD3F66CAAA0
  )
  FetchContent_MakeAvailable(openxr_headers)
endif()

FetchContent_Declare(
  dear_imgui
  URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.zip
  URL_HASH SHA256=E1C46D676C2BCB7CED847BA27F50553E33A19DB97B3CADAEC7F8BE64449139F8
)
FetchContent_MakeAvailable(dear_imgui)

if(TMFOXR_VR_BACKEND STREQUAL "XrAPI")
  set(TMFOXR_BRIDGE_SOURCE src/xrapi_bridge.cpp src/virtual_joypad.cpp src/sampling_profiler.cpp)
else()
  set(TMFOXR_BRIDGE_SOURCE src/vr_bridge.cpp)
endif()

add_library(d3d9 SHARED
  src/d3d9_proxy.cpp
  src/large_address_aware.cpp
  src/log.cpp
  src/runtime_paths.cpp
  ${TMFOXR_BRIDGE_SOURCE}
  src/d3d9_proxy.def
  ${dear_imgui_SOURCE_DIR}/imgui.cpp
  ${dear_imgui_SOURCE_DIR}/imgui_draw.cpp
  ${dear_imgui_SOURCE_DIR}/imgui_tables.cpp
  ${dear_imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${dear_imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
  ${dear_imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)

target_compile_features(d3d9 PRIVATE cxx_std_20)
target_compile_definitions(d3d9 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
if(TMFOXR_VR_BACKEND STREQUAL "OpenXR")
  target_compile_definitions(d3d9 PRIVATE XR_USE_GRAPHICS_API_D3D11 XR_NO_PROTOTYPES)
endif()
if(TMFOXR_VR_BACKEND STREQUAL "OpenXR")
  target_include_directories(d3d9 PRIVATE
    "${openxr_headers_SOURCE_DIR}/src"
    "${openxr_headers_BINARY_DIR}/include")
endif()
target_include_directories(d3d9 PRIVATE
  "${dear_imgui_SOURCE_DIR}"
  "${dear_imgui_SOURCE_DIR}/backends"
)
if(TMFOXR_VR_BACKEND STREQUAL "XrAPI")
  target_compile_definitions(d3d9 PRIVATE TMFOXR_LOG_BESIDE_MODULE TMFOXR_VIRTUAL_JOYPAD)
  target_link_libraries(d3d9 PRIVATE dxguid ws2_32)
else()
  target_link_libraries(d3d9 PRIVATE dxguid d3d11 dxgi)
endif()
if(MINGW)
  target_include_directories(d3d9 PRIVATE src/compat/mingw)
  # MSVC-style __asm blocks (RenderTreeVisibilityBranchHook).
  target_compile_options(d3d9 PRIVATE -fasm-blocks -fms-extensions)
  # Ship a self-contained DLL: no libc++/libunwind DLLs next to the game.
  target_link_options(d3d9 PRIVATE -static -Wl,--kill-at)
  target_link_libraries(d3d9 PRIVATE dwmapi)
endif()

if(MSVC)
add_executable(d3d9on12_compat_smoke EXCLUDE_FROM_ALL tests/d3d9on12_compat_smoke.cpp)
target_compile_features(d3d9on12_compat_smoke PRIVATE cxx_std_20)
target_compile_definitions(d3d9on12_compat_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(d3d9on12_compat_smoke PRIVATE dxguid d3d11 d3d12)

add_executable(tmloader_iat_smoke EXCLUDE_FROM_ALL tests/tmloader_iat_smoke.cpp)
target_compile_features(tmloader_iat_smoke PRIVATE cxx_std_20)
target_compile_definitions(tmloader_iat_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(tmloader_iat_smoke PROPERTIES OUTPUT_NAME "TmForever")

add_executable(d3d9_vram_probe EXCLUDE_FROM_ALL tests/d3d9_vram_probe.cpp)
target_compile_features(d3d9_vram_probe PRIVATE cxx_std_20)
target_compile_definitions(d3d9_vram_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(d3d9_vram_probe PRIVATE dxguid)
set_target_properties(d3d9_vram_probe PROPERTIES
  OUTPUT_NAME "TmForeverLauncher"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/vram-probe"
)

add_executable(large_address_aware_smoke EXCLUDE_FROM_ALL
  tests/large_address_aware_smoke.cpp
  src/large_address_aware.cpp
)
target_compile_features(large_address_aware_smoke PRIVATE cxx_std_20)
target_compile_definitions(large_address_aware_smoke PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(large_address_aware_smoke PRIVATE src)
endif()

set_target_properties(d3d9 PROPERTIES
  OUTPUT_NAME "d3d9"
  ARCHIVE_OUTPUT_NAME "TMFOXR"
  PREFIX ""
  SUFFIX ".dll"
)

add_custom_command(TARGET d3d9 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:d3d9>
    $<TARGET_FILE_DIR:d3d9>/TMFOXR.dll
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${dear_imgui_SOURCE_DIR}/LICENSE.txt
    $<TARGET_FILE_DIR:d3d9>/DearImGui-LICENSE.txt
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/TMFOXR.defaults.ini
    $<TARGET_FILE_DIR:d3d9>/TMFOXR.defaults.ini
  COMMENT "Creating the TrackMania ModLoader TMFOXR.dll"
)
