cmake_minimum_required(VERSION 3.25)
# `APPLE` here is the HOST, not the target: this line runs before project(),
# and MEASURED at that point (emcmake, macOS host) APPLE=1 while
# CMAKE_SYSTEM_NAME and EMSCRIPTEN are both still EMPTY — the toolchain file
# has not been read yet. CMAKE_TOOLCHAIN_FILE is the only signal available,
# because emcmake passes it on the command line as a cache entry. Without this
# test the web configure enables an Objective-C++ language it never uses and
# points it at the HOST clang from inside a cross toolchain.
if(APPLE AND NOT CMAKE_TOOLCHAIN_FILE MATCHES "Emscripten")
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
# Web escape hatch: link the module with JSPI so gpuBlockOn() can really block
# (see src/platform.h). OFF is the intended shape — stage 2 removes the last
# blocking callers instead — but a flag beats a rewrite if that stalls.
option(CLAYFRAY_WEB_JSPI "Web: allow blocking GPU waits via JSPI" OFF)

include(FetchContent)

# ---- window + WebGPU: ONE interface target, two very different sources ----
# Desktop builds SDL3 and Dawn from source. The browser CANNOT: a wasm module
# has no native Dawn to link, and the browser's WebGPU is reached through
# Dawn's Emscripten port, emdawnwebgpu, which Emscripten ships as a `--use-port`
# (it downloads a prebuilt package + JS glue). SDL3 likewise has an Emscripten
# port. Everything downstream links `clayfray_deps` and stays platform-blind.
add_library(clayfray_deps INTERFACE)

if(EMSCRIPTEN)
  # Ports must appear on the COMPILE line as well as the link line: they
  # contribute the include directories (webgpu/webgpu_cpp.h, SDL3/SDL.h), and
  # a compile-only or link-only spelling fails with "file not found" or
  # undefined symbols respectively.
  set(CLAYFRAY_EM_PORTS --use-port=sdl3 --use-port=emdawnwebgpu)
  target_compile_options(clayfray_deps INTERFACE ${CLAYFRAY_EM_PORTS})
  target_link_options(clayfray_deps INTERFACE ${CLAYFRAY_EM_PORTS})
  # Version skew is deliberate and web-only: the ports pin their own SDL3 and
  # Dawn revisions, and the desktop pins below are untouched by this branch.
  # Only code under __EMSCRIPTEN__ ever compiles against the port headers.
else()

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
# Windows: lets Dawn load vulkan-1.dll by bare name from System32. Without
# it, DynamicLib::Open passes LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR (which
# REQUIRES a fully qualified path) -> error 87 -> no Vulkan backend, and the
# D3D12 fallback compiles the raymarch shaders through FXC: 7+ min startup.
set(DAWN_FORCE_SYSTEM_COMPONENT_LOAD ON CACHE BOOL "" FORCE)
# GIT_SUBMODULES "" is load-bearing: Dawn's repo declares dozens of huge
# submodules (ANGLE, Mesa, SwiftShader...). DAWN_FETCH_DEPENDENCIES fetches
# the small required subset instead.
FetchContent_Declare(dawn
  GIT_REPOSITORY https://github.com/google/dawn
  GIT_TAG v20260811.203823
  GIT_SHALLOW TRUE
  GIT_SUBMODULES ""
  GIT_SUBMODULES_RECURSE FALSE)

FetchContent_MakeAvailable(sdl3 dawn)
target_link_libraries(clayfray_deps INTERFACE SDL3::SDL3 dawn::webgpu_dawn)

endif() # EMSCRIPTEN

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

FetchContent_MakeAvailable(imgui stb cgltf)

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
# emdawnwebgpu IS Dawn's API surface, so the Dawn spelling of imgui's WebGPU
# backend is correct on web too — it is `IMGUI_IMPL_WEBGPU_BACKEND_WGPU` (the
# wgpu-native flavour) that would be wrong.
target_compile_definitions(imgui_lib PUBLIC IMGUI_IMPL_WEBGPU_BACKEND_DAWN)
target_link_libraries(imgui_lib PUBLIC clayfray_deps)

add_executable(clayfray
  src/main.cpp
  src/gpu.cpp
  src/brick.cpp
  src/asset.cpp
  src/anim.cpp
  src/ground.cpp
  src/renderer.cpp
  src/snapshot.cpp
  src/ctl.cpp
  src/ui.cpp)
target_include_directories(clayfray PRIVATE ${stb_SOURCE_DIR} ${cgltf_SOURCE_DIR})
target_link_libraries(clayfray PRIVATE clayfray_deps imgui_lib)

if(EMSCRIPTEN)
  # Runtime file reads are UNCHANGED on web — that is the point of preloading.
  # `shaders/` and `assets/` are baked into a .data file the loader mounts into
  # MEMFS before main() runs, at the paths given after '@'. loadShader() and
  # the fighter.glb ifstream then open exactly the paths they always did; only
  # the shader ROOT differs, because the source tree's absolute path is
  # meaningless inside the module.
  #
  # Emscripten's initial working directory is "/", so main.cpp's relative
  # "assets/fighter.glb" resolves to the mount below without a chdir.
  target_compile_definitions(clayfray PRIVATE CLAYFRAY_SHADER_DIR="/shaders")
  target_link_options(clayfray PRIVATE
    "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/shaders@/shaders"
    # the .glb by name, not the whole assets/ directory: the sibling
    # fighter.blend and the Blender export scripts are 375 KB of payload every
    # visitor would download and nothing would read. Cost of naming it: a NEW
    # asset needs a line here or it is silently absent on web.
    "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/assets/fighter.glb@/assets/fighter.glb"
    # ---- wasm heap sizing (stage 2: now measured, not reserved) ----
    #
    # The big buffers are NOT here. distPool (16 MiB) + albedoPool (32 MiB) +
    # volume + JFA scratch — call it ~50 MiB per fighter — are wgpu::Buffers,
    # i.e. GPU-side allocations the wasm heap never sees. Sizing this to cover
    # them was the stage-1 guess and it overshot by ~4x.
    #
    # What DOES land in the wasm heap is dominated by ONE number, and it is
    # not a number anyone would guess — it has to be read off a run. The
    # startup line `import: 14848 tris binned (8000190 refs)` is the whole
    # budget: the voxelizer bins every triangle into every cell its
    # band-dilated AABB touches, and at kBand = 12 voxels that fans 14,848
    # triangles out to EIGHT MILLION cell references, ~539 per triangle.
    #
    #   tri refs `ids`            8,000,190 * 4        = 30.5 MiB
    #   `merged` = offsets + ids  8,125,191 * 4        = 31.0 MiB
    #     ^ a full COPY of ids, alive at the same time as ids (brick.cpp,
    #       just before the "cell tris" upload) — together they ARE the peak
    #   cell offsets (kGrid^3+1)                        = 0.48 MiB
    #   inside bits (kGrid^3)                           = 0.48 MiB
    #   posNrm 0.66 + normals 0.32                      = 0.98 MiB
    #   CharacterAsset vectors (27,776 verts/14,848 tris) ~1.6 MiB
    #   .glb read buffer + MEMFS preload package        ~1.9 MiB
    #   static data (measured from the linked .wasm)      0.16 MiB
    #   ImGui + SDL + libc + Dawn's client-side objects   a few MiB
    #
    # Peak ~= 70 MiB, and it is a STARTUP TRANSIENT: once the import uploads
    # to the GPU, everything above but the asset vectors is freed. That does
    # not help the reservation, because wasm memory never shrinks — whatever
    # the module grows to during import it holds for the session. So size for
    # the peak up front and skip the growth memcpys rather than pretend the
    # steady state (~5 MiB) is the requirement.
    #
    # 96 MiB = the ~70 MiB peak plus room for a somewhat denser asset. Note
    # the stage-1 guess of 64 MiB was slightly UNDER the real peak, so it
    # would have grown during startup on every load.
    #
    # WORTH FIXING LATER: `merged` is a pure copy that exists only to upload
    # offsets and ids as one buffer. Two WriteBuffer calls into the same
    # buffer at offsets 0 and offsets.size()*4 are byte-identical and would
    # cut 31 MiB off the peak, letting this drop to 64 MiB or below. Left
    # alone here because it is shared native/web code and this branch is
    # meant to be additive.
    #
    # ALLOW_MEMORY_GROWTH stays ON regardless: it is what makes being wrong
    # about the number above recoverable rather than fatal.
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=100663296
    # Emscripten defaults the stack to 64 KB; this code was written against a
    # native 8 MB one. Nothing here has a frame over ~2 KB (the largest local
    # buffer in src/ is a 2048-byte char array), so 64 KB would probably do —
    # but a Release build has STACK_OVERFLOW_CHECK off, which makes an
    # overflow silent heap corruption rather than an error. 1 MiB is 3% of the
    # heap above and removes the question.
    -sSTACK_SIZE=1048576)
  if(CLAYFRAY_WEB_JSPI)
    # -sJSPI is a LINK setting (em++ warns and ignores it on a compile line);
    # the define is what flips CLAYFRAY_HAS_BLOCKING_GPU_WAIT in platform.h,
    # and it must reach every TU that contains a wait.
    target_compile_definitions(clayfray PRIVATE CLAYFRAY_WEB_JSPI)
    target_link_options(clayfray PRIVATE -sJSPI)
  endif()
  # emit the stock emcc shell, whose <canvas id="canvas"> is what both SDL and
  # the WebGPU surface bind to (see the selector lookup in gpu.cpp)
  set_target_properties(clayfray PROPERTIES SUFFIX ".html")
else()
  # the SOURCE tree, not an install dir: editing a .wgsl and having the running
  # binary pick it up is the whole shader hot-reload loop (CLAUDE.md trap 1)
  target_compile_definitions(clayfray PRIVATE CLAYFRAY_SHADER_DIR="${CMAKE_SOURCE_DIR}/shaders")
endif()

if(CLAYFRAY_TRACY)
  FetchContent_Declare(tracy
    GIT_REPOSITORY https://github.com/wolfpld/tracy
    GIT_TAG v0.14.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(tracy)
  target_link_libraries(clayfray PRIVATE TracyClient)
  target_compile_definitions(clayfray PRIVATE CLAYFRAY_TRACY)
endif()
