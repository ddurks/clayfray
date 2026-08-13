#include "gpu.h"

#include <SDL3/SDL.h>
#include <cstdio>

#ifdef __APPLE__
#include <SDL3/SDL_metal.h>
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

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
    wgpu::Future af = instance.RequestAdapter(
        &adapterOpts, wgpu::CallbackMode::WaitAnyOnly,
        [this](wgpu::RequestAdapterStatus status, wgpu::Adapter a, wgpu::StringView msg) {
            if (status == wgpu::RequestAdapterStatus::Success) {
                adapter = std::move(a);
            } else {
                std::fprintf(stderr, "wgpu: adapter request failed: %.*s\n",
                             (int)msg.length, msg.data);
            }
        });
    instance.WaitAny(af, UINT64_MAX);
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
