#include "gpu.h"

#include <SDL3/SDL.h>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <SDL3/SDL_properties.h>
#elif defined(__APPLE__)
#include <SDL3/SDL_metal.h>
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

bool gpuBlockOn(const wgpu::Instance& instance, wgpu::Future future,
                const char* what) {
#if CLAYFRAY_HAS_BLOCKING_GPU_WAIT
    (void)what;
    return instance.WaitAny(future, UINT64_MAX) == wgpu::WaitStatus::Success;
#else
    // Not "we chose not to wait" — we CANNOT. wgpuInstanceWaitAny is an
    // abort() in a non-Asyncify browser module at every timeout including 0
    // (platform.h note 1), so calling it would take the tab down with a stack
    // trace pointing at Dawn instead of at the real problem. Report and fail.
    (void)instance;
    (void)future;
    std::fprintf(stderr,
                 "[web] blocking GPU wait for '%s' is not available in a "
                 "browser — this call site still needs the stage-2 callback "
                 "inversion (or build with -DCLAYFRAY_WEB_JSPI=ON)\n",
                 what);
    return false;
#endif
}

namespace {

#ifndef __EMSCRIPTEN__
// ---- persistent pipeline cache (native only) -----------------------------
// Not compiled on web for two independent reasons, either of which is fatal:
// `DawnCacheDeviceDescriptor` does not exist in emdawnwebgpu (it is a native
// Dawn extension), and the backing store would be MEMFS, i.e. thrown away
// when the tab closes. The browser keeps its own compiled-pipeline cache
// across loads, so nothing is lost.
//
// Dawn compiles every WGSL module from scratch on each launch — ~6 s for the
// trace/pick shaders alone, paid on EVERY run of an edit-rebuild-look cycle.
// Dawn exposes a blob cache hook; back it with one file per entry so warm
// starts skip the shader compiler entirely.
//
// Blobs are specific to the GPU, the driver, and the Dawn build. Dawn folds
// the first two into the key it hands us; `kCacheVersion` in the isolation
// key covers the third — bump it after a Dawn upgrade rather than debugging
// a stale-binary crash.
constexpr const char* kCacheVersion = "clayfray-1";

std::filesystem::path cacheDir() {
    return std::filesystem::path(".cache") / "pipeline";
}

uint64_t fnv1a(const uint8_t* d, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h ^= d[i];
        h *= 1099511628211ull;
    }
    return h;
}

std::filesystem::path cachePathFor(const uint8_t* key, size_t keySize) {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.bin",
                  (unsigned long long)fnv1a(key, keySize));
    return cacheDir() / name;
}

// Entry layout: [u32 keySize][key][value]. The key is stored so a filename
// hash collision reads back as a MISS instead of handing Dawn some other
// pipeline's binary.
size_t cacheLoad(size_t keySize, const uint8_t* key, size_t valueSize, uint8_t* value) {
    std::ifstream f(cachePathFor(key, keySize), std::ios::binary);
    if (!f) return 0;
    uint32_t storedKeySize = 0;
    f.read(reinterpret_cast<char*>(&storedKeySize), sizeof(storedKeySize));
    if (!f || storedKeySize != keySize) return 0;
    std::vector<uint8_t> storedKey(storedKeySize);
    f.read(reinterpret_cast<char*>(storedKey.data()), (std::streamsize)storedKeySize);
    if (!f || std::memcmp(storedKey.data(), key, keySize) != 0) return 0;
    const std::streampos start = f.tellg();
    f.seekg(0, std::ios::end);
    const size_t n = (size_t)(f.tellg() - start);
    // Dawn probes with valueSize 0 to learn the size, then calls again with a
    // buffer: report the size without writing when it doesn't fit.
    if (n == 0 || n > valueSize) return n;
    f.seekg(start);
    f.read(reinterpret_cast<char*>(value), (std::streamsize)n);
    return f ? n : 0;
}

void cacheStore(size_t keySize, const uint8_t* key, size_t valueSize,
                const uint8_t* value) {
    std::error_code ec;
    std::filesystem::create_directories(cacheDir(), ec);
    const std::filesystem::path path = cachePathFor(key, keySize);
    // write-then-rename: a crash mid-write must not leave a truncated entry
    // that still passes the key check and loads as a valid blob
    const std::filesystem::path tmp = std::filesystem::path(path.string() + ".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        const uint32_t ks = (uint32_t)keySize;
        f.write(reinterpret_cast<const char*>(&ks), sizeof(ks));
        f.write(reinterpret_cast<const char*>(key), (std::streamsize)keySize);
        f.write(reinterpret_cast<const char*>(value), (std::streamsize)valueSize);
        if (!f) return;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}
#endif // !__EMSCRIPTEN__

} // namespace

namespace {
bool gPipelineFailed = false;
int gUncapturedErrors = 0;
} // namespace

bool gpuAnyPipelineFailed() { return gPipelineFailed; }
int gpuUncapturedErrorCount() { return gUncapturedErrors; }

void wgslConstantsFit(int needed, size_t bufSize, const char* who) {
    if (needed >= 0 && (size_t)needed < bufSize) return;
    std::fprintf(stderr,
                 "[fatal] %s::wgslConstants() truncated: needs %d bytes, buffer is "
                 "%zu. Raise it — a cut constants block compiles to a black screen "
                 "with a zero exit code.\n",
                 who, needed, bufSize);
    std::fflush(stderr);
    std::abort();
}

GpuPipelineScope::GpuPipelineScope(const wgpu::Device& device, const char* what)
    : device_(device), what_(what) {
    if (device_) device_.PushErrorScope(wgpu::ErrorFilter::Validation);
}

GpuPipelineScope::~GpuPipelineScope() {
    if (!device_) return;
    const char* what = what_;
    device_.PopErrorScope(
        wgpu::CallbackMode::AllowSpontaneous,
        [what](wgpu::PopErrorScopeStatus, wgpu::ErrorType type, wgpu::StringView msg) {
            if (type == wgpu::ErrorType::NoError) return;
            gPipelineFailed = true;
            std::fprintf(stderr,
                         "[wgpu] PIPELINE '%s' FAILED TO CREATE: %.*s\n"
                         "       Every pass using it is a silent no-op from "
                         "here on — expect a black screen.\n",
                         what, (int)msg.length, msg.data);
            // The one failure mode that is portable-by-construction and only
            // shows up on conformant/mobile browsers: core WebGPU guarantees
            // just 8 storage buffers per stage (CLAUDE.md trap 8), and desktop
            // adapters report far more, so a shader over budget runs fine on
            // the dev box and dies on a phone.
            const std::string_view m(msg.data, msg.length);
            if (m.find("torage") != std::string_view::npos &&
                (m.find("imit") != std::string_view::npos ||
                 m.find("xceed") != std::string_view::npos)) {
                std::fprintf(stderr,
                             "       This looks like a storage-buffer limit. "
                             "Core WebGPU guarantees only 8 per stage; see "
                             "CLAUDE.md trap 8 for the region-packing fix.\n");
            }
            std::fflush(stderr);
        });
}

bool Gpu::createInstance() {
    wgpu::InstanceDescriptor instDesc{};
#if CLAYFRAY_HAS_BLOCKING_GPU_WAIT
    // Asking for TimedWaitAny is not a hint on web: emdawnwebgpu's
    // CreateInstance returns NULL when the feature is requested without
    // Asyncify/JSPI, so a stray request here reads as "failed to create
    // instance" with no further explanation.
    static const wgpu::InstanceFeatureName instFeatures[] = {
        wgpu::InstanceFeatureName::TimedWaitAny};
    instDesc.requiredFeatureCount = 1;
    instDesc.requiredFeatures = instFeatures;
#endif
    instance = wgpu::CreateInstance(&instDesc);
    if (!instance) {
        std::fprintf(stderr, "wgpu: failed to create instance\n");
        return false;
    }
    return true;
}

wgpu::RequestAdapterOptions Gpu::adapterOptions() const {
    wgpu::RequestAdapterOptions adapterOpts{};
    adapterOpts.powerPreference = wgpu::PowerPreference::HighPerformance;
#ifdef _WIN32
    // Prefer Vulkan: Dawn's D3D12 backend compiles WGSL through FXC unless
    // built with DAWN_USE_BUILT_DXC, and FXC takes 7+ MINUTES on the big
    // raymarch shaders. Vulkan goes Tint -> SPIR-V in seconds. Requires
    // DAWN_FORCE_SYSTEM_COMPONENT_LOAD (CMakeLists) so vulkan-1.dll loads.
    // CLAYFRAY_D3D12=1 opts back into D3D12 for comparison.
    if (!std::getenv("CLAYFRAY_D3D12")) {
        adapterOpts.backendType = wgpu::BackendType::Vulkan;
    }
#endif
    return adapterOpts;
}

wgpu::Future Gpu::requestAdapter(const wgpu::RequestAdapterOptions& opts,
                                 wgpu::CallbackMode mode, std::function<void()> then) {
    return instance.RequestAdapter(
        &opts, mode,
        [this, then = std::move(then)](wgpu::RequestAdapterStatus status, wgpu::Adapter a,
                                       wgpu::StringView msg) {
            if (status == wgpu::RequestAdapterStatus::Success) {
                adapter = std::move(a);
            } else {
                std::fprintf(stderr, "wgpu: adapter request failed: %.*s\n",
                             (int)msg.length, msg.data);
            }
            if (then) then();
        });
}

void Gpu::reportAdapter() {
    wgpu::AdapterInfo info{};
    adapter.GetInfo(&info);
    std::printf("wgpu adapter: %.*s (%.*s)%s\n", (int)info.device.length, info.device.data,
                (int)info.description.length, info.description.data,
                adapter.HasFeature(wgpu::FeatureName::TimestampQuery)
                    ? ""
                    : " [no gpu timestamps; wall-clock benchmarking only]");
    std::fflush(stdout);
    hasTimestamps = adapter.HasFeature(wgpu::FeatureName::TimestampQuery);
}

wgpu::Future Gpu::requestDevice(wgpu::CallbackMode mode, std::function<void()> then) {
    wgpu::DeviceDescriptor devDesc{};
    // Request the adapter's full limits.
    //
    // The old justification here — "the voxelizer needs 10 storage buffers per
    // stage" — was TRUE WHEN WRITTEN and is now STALE. It counted `meshFill`
    // back when it also reached `mSkin` and `bWeights`; commit 3eee63d (the
    // skeleton-free three-brush rig) deleted both, and an entry-point-level
    // audit of every shader now puts the worst case at exactly 8:
    //
    //     voxelize.wgsl meshFill  8 of 8   (zero spare)
    //     trace.wgsl    cs        7 of 8   (one spare, trap 8)
    //     pick.wgsl     cs        7 of 8
    //
    // What matters is the per-ENTRY-POINT count, not the 11 storage vars
    // voxelize.wgsl declares: every pipeline here uses an AUTO layout, so Dawn
    // derives the bind group layout from what that entry point statically
    // reaches. `meshFill` never reaches group 2, `meshClassify` never reaches
    // mCol/voxParity — see the note at brick.cpp's vxG0_ construction.
    //
    // So nothing in the tree needs raised limits any more, and a core-minimum
    // browser can create every pipeline. This stays as-is because dropping to
    // default limits is a device-configuration change that wants a run on real
    // hardware to confirm, not a compile. Be aware of the cost of keeping it:
    // asking for the adapter's real (larger) limits means the dev box will
    // happily create a pipeline that a core-minimum device rejects, which is
    // exactly the trap-8 failure that only shows up on someone else's phone.
    wgpu::Limits adapterLimits{};
    adapter.GetLimits(&adapterLimits);
    devDesc.requiredLimits = &adapterLimits;
    hasTimestamps = adapter.HasFeature(wgpu::FeatureName::TimestampQuery);
    wgpu::FeatureName requiredFeatures[1] = {wgpu::FeatureName::TimestampQuery};
    if (hasTimestamps) {
        devDesc.requiredFeatureCount = 1;
        devDesc.requiredFeatures = requiredFeatures;
    }
    devDesc.SetUncapturedErrorCallback(
        [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView msg) {
            // LATCHED, not just printed. CLAUDE.md's own check for trap 2 is a
            // human running `2>&1 | grep -c 'wgpu error'` and remembering to —
            // while every other gate in this project (carve-test, replay,
            // imgdiff) is an exit code. The failure most likely to be
            // catastrophic and invisible was the one you had to grep for.
            gUncapturedErrors++;
            std::fprintf(stderr, "[wgpu error %d] %.*s\n", (int)type, (int)msg.length,
                         msg.data);
        });
    devDesc.SetDeviceLostCallback(
        wgpu::CallbackMode::AllowSpontaneous,
        [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView msg) {
            if (reason != wgpu::DeviceLostReason::Destroyed) {
                std::fprintf(stderr, "[wgpu device lost %d] %.*s\n", (int)reason,
                             (int)msg.length, msg.data);
            }
        });

#ifndef __EMSCRIPTEN__
    // Warm starts skip shader compilation entirely (see cacheLoad above).
    // CLAYFRAY_NO_PIPELINE_CACHE=1 forces cold compiles when a cache entry is
    // suspect; deleting .cache/pipeline/ has the same effect permanently.
    wgpu::DawnCacheDeviceDescriptor cacheDesc{};
    if (!std::getenv("CLAYFRAY_NO_PIPELINE_CACHE")) {
        cacheDesc.isolationKey = kCacheVersion;
        // Dawn's C++ wrapper delivers these as std::span, NOT as the
        // (size, ptr) pairs the C typedef uses: webgpu_cpp.h only specializes
        // CArgConverter for the span form, so a raw-pointer lambda fails to
        // instantiate with an "undefined type CArgConverter<...>" error deep
        // inside the generated header, pointing at Dawn rather than at here.
        // Unpack at the boundary and keep cacheLoad/cacheStore on plain
        // pointers.
        cacheDesc.SetDawnLoadCacheDataCallback(
            [](std::span<const std::byte> key, std::span<std::byte> value) -> size_t {
                return cacheLoad(key.size(),
                                 reinterpret_cast<const uint8_t*>(key.data()),
                                 value.size(),
                                 reinterpret_cast<uint8_t*>(value.data()));
            });
        cacheDesc.SetDawnStoreCacheDataCallback(
            [](std::span<const std::byte> key, std::span<const std::byte> value) {
                cacheStore(key.size(), reinterpret_cast<const uint8_t*>(key.data()),
                           value.size(),
                           reinterpret_cast<const uint8_t*>(value.data()));
            });
        devDesc.nextInChain = &cacheDesc;
    }
#endif

    return adapter.RequestDevice(
        &devDesc, mode,
        [this, then = std::move(then)](wgpu::RequestDeviceStatus status, wgpu::Device d,
                                       wgpu::StringView msg) {
            if (status == wgpu::RequestDeviceStatus::Success) {
                device = std::move(d);
            } else {
                std::fprintf(stderr, "wgpu: device request failed: %.*s\n",
                             (int)msg.length, msg.data);
            }
            if (then) then();
        });
}

bool Gpu::finishInit(SDL_Window* window) {
    if (!device) return false;
    queue = device.GetQueue();

    if (window) {
#ifdef __EMSCRIPTEN__
        // The surface is a <canvas> named by CSS selector. It must be the SAME
        // canvas SDL is drawing/eventing on, or input goes to one element and
        // pixels to another — so ask SDL which one it took rather than
        // hardcoding. SDL's Emscripten backend defaults to "#canvas", which is
        // also what the stock emcc HTML shell emits.
        const char* selector = SDL_GetStringProperty(
            SDL_GetWindowProperties(window),
            SDL_PROP_WINDOW_EMSCRIPTEN_CANVAS_ID_STRING, "#canvas");
        wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvasSource{};
        canvasSource.selector = selector;
        wgpu::SurfaceDescriptor surfDesc{};
        surfDesc.nextInChain = &canvasSource;
        surface = instance.CreateSurface(&surfDesc);
#elif defined(__APPLE__)
        SDL_MetalView view = SDL_Metal_CreateView(window);
        void* layer = SDL_Metal_GetLayer(view);
        wgpu::SurfaceSourceMetalLayer metalSource{};
        metalSource.layer = layer;
        wgpu::SurfaceDescriptor surfDesc{};
        surfDesc.nextInChain = &metalSource;
        surface = instance.CreateSurface(&surfDesc);
#elif defined(_WIN32)
        void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                            SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        wgpu::SurfaceSourceWindowsHWND winSource{};
        winSource.hwnd = hwnd;
        winSource.hinstance = GetModuleHandleW(nullptr);
        wgpu::SurfaceDescriptor surfDesc{};
        surfDesc.nextInChain = &winSource;
        surface = instance.CreateSurface(&surfDesc);
#else
        // Linux (X11/Wayland) surface creation lands with M8.
        std::fprintf(stderr, "windowed mode: unsupported platform\n");
        return false;
#endif
        if (!surface) {
            std::fprintf(stderr, "wgpu: failed to create surface\n");
            return false;
        }
        wgpu::SurfaceCapabilities caps{};
        surface.GetCapabilities(adapter, &caps);
        if (caps.formatCount > 0) surfaceFormat = caps.formats[0];
    }
    return true;
}

bool Gpu::init(SDL_Window* window) {
    // The desktop sequence, unchanged in every observable way: request,
    // block, request, block, finish. WaitAnyOnly is the cheapest callback
    // mode when something really is going to wait on the future.
    if (!createInstance()) return false;

    wgpu::RequestAdapterOptions adapterOpts = adapterOptions();
    gpuBlockOn(instance, requestAdapter(adapterOpts, wgpu::CallbackMode::WaitAnyOnly),
               "adapter request");
    if (!adapter && adapterOpts.backendType == wgpu::BackendType::Vulkan) {
        std::fprintf(stderr, "wgpu: no Vulkan adapter, falling back to default backend\n");
        adapterOpts.backendType = wgpu::BackendType::Undefined;
        gpuBlockOn(instance, requestAdapter(adapterOpts, wgpu::CallbackMode::WaitAnyOnly),
                   "adapter request");
    }
    if (!adapter) return false;
    reportAdapter();

    gpuBlockOn(instance, requestDevice(wgpu::CallbackMode::WaitAnyOnly), "device request");
    if (!device) return false;
    return finishInit(window);
}

void Gpu::initAsync(SDL_Window* window, std::function<void(bool)> onReady) {
#if CLAYFRAY_HAS_BLOCKING_GPU_WAIT
    // Native (and the JSPI escape hatch) can just do it the old way and call
    // back immediately. Keeping ONE caller shape means main.cpp's startup path
    // is the same code on both platforms, which is what stops the two from
    // drifting.
    onReady(init(window));
#else
    // Browser: no waiting allowed anywhere in here. AllowSpontaneous is the
    // mode whose callback the event loop delivers on its own (during
    // processEvents / between tasks) — WaitAnyOnly would simply never fire,
    // because nothing is permitted to call WaitAny.
    //
    // There is no Vulkan-fallback branch on this path: the backend preference
    // it retries is #ifdef _WIN32 and a browser has exactly one adapter
    // source. If RequestAdapter fails here, it failed for real (usually
    // "WebGPU not available" — an unsupported browser or a non-secure origin).
    if (!createInstance()) {
        onReady(false);
        return;
    }
    // shared_ptr, not capture-by-value: the continuation is copied into a
    // wgpu callback, and the SECOND continuation is copied out of the first.
    // One heap cell keeps `onReady` callable exactly once at the end of the
    // chain without moving a std::function through two nested lambdas.
    auto ready = std::make_shared<std::function<void(bool)>>(std::move(onReady));
    const wgpu::RequestAdapterOptions opts = adapterOptions();
    requestAdapter(opts, wgpu::CallbackMode::AllowSpontaneous, [this, window, ready]() {
        if (!adapter) {
            // Do NOT lead with "secure origin" here. The two failures look
            // identical from C++ but have different fixes, and guessing wrong
            // sends people chasing TLS when the real cause is a browser flag:
            //   navigator.gpu MISSING  -> the browser has no WebGPU, or it is
            //                             switched off. Safari: enable
            //                             Settings > Advanced > "Show features
            //                             for web developers", then Develop >
            //                             Feature Flags > WebGPU.
            //   navigator.gpu PRESENT  -> WebGPU exists but returned no
            //                             adapter (insecure origin, blocklisted
            //                             GPU, headless). http://localhost IS a
            //                             secure origin; file:// is not.
            // The shell's JS can tell these apart and says which one it is;
            // this message just points at it rather than asserting a cause.
            std::fprintf(stderr,
                         "wgpu: no adapter — nothing will render. The page "
                         "banner says which of the two causes it is (no "
                         "WebGPU in this browser, vs WebGPU present but no "
                         "adapter).\n");
            (*ready)(false);
            return;
        }
        reportAdapter();
        requestDevice(wgpu::CallbackMode::AllowSpontaneous, [this, window, ready]() {
            (*ready)(device && finishInit(window));
        });
    });
#endif
}

void Gpu::configureSurface(int pixelWidth, int pixelHeight) {
    if (!surface) return;
    wgpu::SurfaceConfiguration config{};
    config.device = device;
    config.format = surfaceFormat;
    config.usage = wgpu::TextureUsage::RenderAttachment;
    config.width = (uint32_t)pixelWidth;
    config.height = (uint32_t)pixelHeight;
    config.presentMode = wgpu::PresentMode::Fifo;
    surface.Configure(&config);
}

void Gpu::processEvents() {
    if (instance) instance.ProcessEvents();
}

void Gpu::waitForGpu() {
#if !CLAYFRAY_DEV_TOOLS
    // Backpressure for the headless loops only, and those are native-only.
    // The browser paces us by returning to the event loop between frames, so
    // there is nothing to throttle and nothing that may block here.
    return;
#else
    if (!instance || !queue) return;
    wgpu::Future f = queue.OnSubmittedWorkDone(
        wgpu::CallbackMode::WaitAnyOnly,
        [](wgpu::QueueWorkDoneStatus status, wgpu::StringView msg) {
            if (status != wgpu::QueueWorkDoneStatus::Success) {
                std::fprintf(stderr, "wgpu: queue work done status %d: %.*s\n",
                             (int)status, (int)msg.length, msg.data);
            }
        });
    gpuBlockOn(instance, f, "queue drain");
#endif
}
