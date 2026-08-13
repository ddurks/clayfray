cmake_minimum_required(VERSION 3.25)
if(APPLE)
  # OBJCXX must be declared here: enable_language() later in the file fails
  # to initialize the toolchain (missing CMAKE_OBJCXX_COMPILE_OBJECT)
  project(clayfray LANGUAGES C CXX OBJCXX)
else()
  project(clayfray LANGUAGES C CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
  set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
endif()

option(CLAYFRAY_TRACY "Enable Tracy profiler client" OFF)

include(FetchContent)

# ---- SDL3 (static) ----
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(sdl3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL
  GIT_TAG release-3.2.30
  GIT_SHALLOW TRUE)

# ---- Dawn (WebGPU) ----
set(DAWN_FETCH_DEPENDENCIES ON CACHE BOOL "" FORCE)
set(DAWN_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(DAWN_USE_GLFW OFF CACHE BOOL "" FORCE)
set(DAWN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TINT_BUILD_CMD_TOOLS OFF CACHE BOOL "" FORCE)
# GIT_SUBMODULES "" is load-bearing: Dawn's repo declares dozens of huge
# submodules (ANGLE, Mesa, SwiftShader...). DAWN_FETCH_DEPENDENCIES fetches
# the small required subset instead.
FetchContent_Declare(dawn
  GIT_REPOSITORY https://github.com/google/dawn
  GIT_TAG v20260811.203823
  GIT_SHALLOW TRUE
  GIT_SUBMODULES ""
  GIT_SUBMODULES_RECURSE FALSE)

# ---- Dear ImGui (no CMake upstream; we build our own target) ----
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui
  GIT_TAG v1.92.9b
  GIT_SHALLOW TRUE)

# ---- stb (headers only) ----
FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb
  GIT_TAG master
  GIT_SHALLOW TRUE)

# ---- cgltf (single-header glTF parser) ----
FetchContent_Declare(cgltf
  GIT_REPOSITORY https://github.com/jkuhlmann/cgltf
  GIT_TAG v1.15
  GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(sdl3 dawn imgui stb cgltf)

if(APPLE)
  # the wgpu backend pulls in Cocoa/Metal headers on macOS, so compile it as
  # Objective-C++ via a .mm wrapper (LANGUAGE source property is unreliable
  # across generators)
  set(IMGUI_WGPU_BACKEND ${CMAKE_SOURCE_DIR}/src/imgui_impl_wgpu_objcxx.mm)
else()
  set(IMGUI_WGPU_BACKEND ${imgui_SOURCE_DIR}/backends/imgui_impl_wgpu.cpp)
endif()

add_library(imgui_lib STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${IMGUI_WGPU_BACKEND})
target_include_directories(imgui_lib PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui_lib PUBLIC IMGUI_IMPL_WEBGPU_BACKEND_DAWN)
target_link_libraries(imgui_lib PUBLIC SDL3::SDL3 dawn::webgpu_dawn)

add_executable(clayfray
  src/main.cpp
  src/gpu.cpp
  src/brick.cpp
  src/asset.cpp
  src/anim.cpp
  src/ground.cpp
  src/renderer.cpp
  src/ui.cpp)
target_include_directories(clayfray PRIVATE ${stb_SOURCE_DIR} ${cgltf_SOURCE_DIR})
target_link_libraries(clayfray PRIVATE SDL3::SDL3 dawn::webgpu_dawn imgui_lib)
target_compile_definitions(clayfray PRIVATE CLAYFRAY_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders")

if(CLAYFRAY_TRACY)
  FetchContent_Declare(tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy
    GIT_TAG v0.14.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(tracy)
  target_link_libraries(clayfray PRIVATE TracyClient)
  target_compile_definitions(clayfray PRIVATE CLAYFRAY_TRACY)
endif()
