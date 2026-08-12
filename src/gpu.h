#pragma once
#include <webgpu/webgpu_cpp.h>

struct SDL_Window;

// Owns the WebGPU instance/adapter/device and (in windowed mode) the surface.
// Pass window = nullptr for headless rendering (screenshot mode).
struct Gpu {
    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Queue queue;
    wgpu::Surface surface; // null when headless
    wgpu::TextureFormat surfaceFormat = wgpu::TextureFormat::BGRA8Unorm;
    bool hasTimestamps = false;

    bool init(SDL_Window* window);
    void configureSurface(int pixelWidth, int pixelHeight);
    void processEvents();
};
