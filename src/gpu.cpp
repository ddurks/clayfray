#include "gpu.h"

#include <SDL3/SDL.h>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

#ifdef __APPLE__
#include <SDL3/SDL_metal.h>
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

// ---- persistent pipeline cache -------------------------------------------
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

} // namespace

bool Gpu::init(SDL_Window* window) {
    static const wgpu::InstanceFeatureName instFeatures[] = {
        wgpu::InstanceFeatureName::TimedWaitAny};
    wgpu::InstanceDescriptor instDesc{};
    instDesc.requiredFeatureCount = 1;
    instDesc.requiredFeatures = instFeatures;
    instance = wgpu::CreateInstance(&instDesc);
    if (!instance) {
        std::fprintf(stderr, "wgpu: failed to create instance\n");
        return false;
    }

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
    auto requestAdapter = [this](const wgpu::RequestAdapterOptions& opts) {
        wgpu::Future af = instance.RequestAdapter(
            &opts, wgpu::CallbackMode::WaitAnyOnly,
            [this](wgpu::RequestAdapterStatus status, wgpu::Adapter a, wgpu::StringView msg) {
                if (status == wgpu::RequestAdapterStatus::Success) {
                    adapter = std::move(a);
                } else {
                    std::fprintf(stderr, "wgpu: adapter request failed: %.*s\n",
                                 (int)msg.length, msg.data);
                }
            });
        instance.WaitAny(af, UINT64_MAX);
    };
    requestAdapter(adapterOpts);
    if (!adapter && adapterOpts.backendType == wgpu::BackendType::Vulkan) {
        std::fprintf(stderr, "wgpu: no Vulkan adapter, falling back to default backend\n");
        adapterOpts.backendType = wgpu::BackendType::Undefined;
        requestAdapter(adapterOpts);
    }
    if (!adapter) return false;

    wgpu::AdapterInfo info{};
    adapter.GetInfo(&info);
    std::printf("wgpu adapter: %.*s (%.*s)%s\n", (int)info.device.length, info.device.data,
                (int)info.description.length, info.description.data,
                adapter.HasFeature(wgpu::FeatureName::TimestampQuery)
                    ? ""
                    : " [no gpu timestamps; wall-clock benchmarking only]");

    wgpu::DeviceDescriptor devDesc{};
    // request the adapter's full limits: the voxelizer needs 10 storage
    // buffers per stage (default cap is 8)
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

    wgpu::Future df = adapter.RequestDevice(
        &devDesc, wgpu::CallbackMode::WaitAnyOnly,
        [this](wgpu::RequestDeviceStatus status, wgpu::Device d, wgpu::StringView msg) {
            if (status == wgpu::RequestDeviceStatus::Success) {
                device = std::move(d);
            } else {
                std::fprintf(stderr, "wgpu: device request failed: %.*s\n",
                             (int)msg.length, msg.data);
            }
        });
    instance.WaitAny(df, UINT64_MAX);
    if (!device) return false;
    queue = device.GetQueue();

    if (window) {
#ifdef __APPLE__
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
    if (!instance || !queue) return;
    wgpu::Future f = queue.OnSubmittedWorkDone(
        wgpu::CallbackMode::WaitAnyOnly,
        [](wgpu::QueueWorkDoneStatus status, wgpu::StringView msg) {
            if (status != wgpu::QueueWorkDoneStatus::Success) {
                std::fprintf(stderr, "wgpu: queue work done status %d: %.*s\n",
                             (int)status, (int)msg.length, msg.data);
            }
        });
    instance.WaitAny(f, UINT64_MAX);
}
