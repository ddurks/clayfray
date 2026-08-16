#pragma once
#include <webgpu/webgpu_cpp.h>

#include "platform.h"

struct SDL_Window;

// THE blocking-wait chokepoint. Every place that stops the world until a GPU
// future resolves goes through here — there is no bare `instance.WaitAny` left
// in the codebase, and adding one back is what would break the web target
// silently (see platform.h note 1: WaitAny is an abort() in a browser).
//
// `what` names the caller for the diagnostic, e.g. "adapter request".
// Returns true if the future completed. Native always completes (the wait is
// unbounded, as it always was); web returns false without waiting, so the
// caller fails cleanly instead of aborting the wasm module.
//
// Stage 2 empties this of callers: with CLAYFRAY_DEV_TOOLS off, the ONLY two
// left are the adapter and device requests in Gpu::init, and both are
// one-shot startup steps that become a callback chain.
bool gpuBlockOn(const wgpu::Instance& instance, wgpu::Future future, const char* what);

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
    // (see the backpressure note in main.cpp). Only the headless loops call
    // it, so on web it is a no-op — the browser's own rAF pacing is the
    // backpressure there.
    void waitForGpu();
};
