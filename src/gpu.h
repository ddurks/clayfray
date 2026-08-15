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
    // Block until the GPU has retired everything submitted so far. The
    // windowed loop is paced by Fifo Present; the headless loops have no
    // swapchain and no wall clock, so without this they submit thousands of
    // frames per second and Dawn's pending-submission memory kills the device
    // (see the backpressure note in main.cpp).
    void waitForGpu();
};
