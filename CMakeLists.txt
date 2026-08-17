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
option(CLAYFRAY_ASAN "Build with Address+UB sanitizers (desktop only)" OFF)

# ---- warnings ----
# This target built with NO warning flags at all until now, which is a strange
# place for a codebase whose documented worst failure mode is "boots, prints a
# healthy banner, renders black, exits 0" to be standing. Measured cost of
# turning them on: 2 warnings across all ten translation units.
#
# NOT -Werror. FetchContent pulls SDL3, Dawn and imgui, whose headers we do not
# control; those are silenced with SYSTEM below instead, and a hard error on a
# dependency's next release would break the build for no defect of ours.
add_library(clayfray_warnings INTERFACE)
if(MSVC)
  target_compile_options(clayfray_warnings INTERFACE /W4)
else()
  target_compile_options(clayfray_warnings INTERFACE -Wall -Wextra -Wshadow)
endif()
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
# SYSTEM so imgui's headers do not trip clayfray's warnings (see above); imgui
# itself is deliberately NOT linked against clayfray_warnings.
target_include_directories(imgui_lib SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
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
# SYSTEM: stb and cgltf are vendored single-header libraries. Without this the
# warnings enabled above fire on THEIR code, which we cannot fix and would
# quickly learn to ignore — and an ignored warning list is the same as none.
target_include_directories(clayfray SYSTEM PRIVATE ${stb_SOURCE_DIR} ${cgltf_SOURCE_DIR})
target_link_libraries(clayfray PRIVATE clayfray_deps imgui_lib clayfray_warnings)

# Sanitizers are opt-in and desktop-only: the strict-aliasing and alignment
# casts off mapped GPU ranges are the things worth pointing UBSan at, and none
# of them exist on the web path. Run the conservation gate under it:
#   cmake -S . -B build-asan -DCLAYFRAY_ASAN=ON && cmake --build build-asan
#   ./build-asan/clayfray --carve-test --frames 300 --size 320x180 --aa 1
if(CLAYFRAY_ASAN AND NOT EMSCRIPTEN)
  target_compile_options(clayfray PRIVATE
    -fsanitize=address,undefined -fno-omit-frame-pointer -g)
  target_link_options(clayfray PRIVATE -fsanitize=address,undefined)
endif()

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
    # startup line `import: 20480 tris binned (10975582 refs)` is the whole
    # budget: the voxelizer bins every triangle into every cell its
    # band-dilated AABB touches, and at kBand = 12 voxels that fans 20,480
    # triangles out to ELEVEN MILLION cell references, ~536 per triangle.
    #
    #   tri refs `ids`           10,975,582 * 4        = 41.9 MiB
    #   `merged` = offsets + ids 11,100,583 * 4        = 42.3 MiB
    #     ^ a full COPY of ids, alive at the same time as ids (brick.cpp,
    #       just before the "cell tris" upload) — together they ARE the peak
    #   cell offsets (kGrid^3+1)                        = 0.48 MiB
    #   inside bits (kGrid^3)                           = 0.48 MiB
    #   posNrm 0.93 + normals 0.45                      = 1.38 MiB
    #   CharacterAsset vectors (39,040 verts/20,480 tris) ~2.2 MiB
    #   .glb read buffer + MEMFS preload package        ~2.6 MiB
    #   static data (measured from the linked .wasm)      0.16 MiB
    #   ImGui + SDL + libc + Dawn's client-side objects   a few MiB
    #
    # Peak ~= 95 MiB, and it is a STARTUP TRANSIENT: once the import uploads
    # to the GPU, everything above but the asset vectors is freed. That does
    # not help the reservation, because wasm memory never shrinks — whatever
    # the module grows to during import it holds for the session. So size for
    # the peak up front and skip the growth memcpys rather than pretend the
    # steady state (~5 MiB) is the requirement.
    #
    # 128 MiB = the ~95 MiB peak plus room for a somewhat denser asset.
    #
    # THE NUMBER MOVES WITH THE BRUSH COUNT, and by more than the geometry
    # suggests. M-FIST added two hand shape keys as two more brushes: 14,848
    # tris -> 20,480 (+38%) took the refs 8.0M -> 11.0M and the peak ~70 ->
    # ~95 MiB, straight past the previous 96 MiB reservation. Every brush is a
    # whole extra mesh through the binner, so RE-READ THE `import:` LINE after
    # adding one — estimating it from vertex counts is hopeless (CLAUDE.md
    # trap 10), and the stage-1 guess of 64 MiB was already under the peak of
    # the day, which grew the heap during startup on every single load.
    #
    # WORTH FIXING, AND NOW WORTH MORE: `merged` is a pure copy that exists
    # only to upload offsets and ids as one buffer. Two WriteBuffer calls into
    # the same buffer at offsets 0 and offsets.size()*4 are byte-identical and
    # would cut 42 MiB off the peak — it was 31 MiB before the fist brushes —
    # letting this drop back to 64 MiB. Left alone because it is shared
    # native/web code and neither branch that touched it was about the
    # voxelizer.
    #
    # ALLOW_MEMORY_GROWTH stays ON regardless: it is what makes being wrong
    # about the number above recoverable rather than fatal.
    -sALLOW_MEMORY_GROWTH=1
    -sINITIAL_MEMORY=134217728
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
  # Our own shell, not the stock emcc one. The stock shell leaves <canvas> at
  # the HTML default 300x150 and never sizes it, and since the C++ derives the
  # trace resolution from the canvas CSS box that came out as "TRACING 160x90".
  # web/shell.html sizes the canvas to the viewport, keeps the drawing buffer
  # in step with DPR, and surfaces the startup log — which is the first thing
  # to read when a frame does not appear.
  target_link_options(clayfray PRIVATE
    "SHELL:--shell-file ${CMAKE_SOURCE_DIR}/web/shell.html")
  # A standalone capability probe, copied beside the build. It touches none of
  # the wasm, so it separates "this browser/origin has no WebGPU" from "our
  # code broke" — which is the first fork in the road every time a frame does
  # not appear, and it also prints the device's real
  # maxStorageBuffersPerShaderStage (see trap 8).
  add_custom_command(TARGET clayfray POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/web/webgpu-check.html $<TARGET_FILE_DIR:clayfray>)
  set_source_files_properties(${CMAKE_SOURCE_DIR}/web/shell.html PROPERTIES
    GENERATED FALSE)
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
