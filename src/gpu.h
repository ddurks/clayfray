#pragma once
#include <webgpu/webgpu_cpp.h>

#include <functional>

#include "platform.h"

struct SDL_Window;

// Guard for the generated `//#constants` blocks (BrickSystem::wgslConstants,
// GroundClay::wgslConstants), which build into a fixed char buffer.
//
// snprintf returns the length it WOULD have written, so an overflow is SILENT:
// the block gets cut mid-token, the shader fails to compile, and per trap 8 a
// rejected pipeline is an invalid object every later pass no-ops on. The app
// then boots, prints a healthy asset banner, renders black, and --carve-test
// still exits 0 (0 == 0 balances). It is nastier than trap 2's version of the
// same failure because the corruption exists only in generated text — nothing
// in the source tree looks wrong, and only CLAYFRAY_DUMP_WGSL would show it.
//
// The blocks are mostly comments, so one more explanatory paragraph is all it
// takes. Fail at startup, loudly, naming the buffer to raise.
void wgslConstantsFit(int needed, size_t bufSize, const char* who);

// Name a pipeline creation so a failure is LOUD and attributable instead of a
// black screen. Dawn reports a rejected CreateComputePipeline/CreateRenderPipeline
// through the uncaptured-error callback and then hands back an INVALID object
// that every later frame silently no-ops on — which is exactly how a
// storage-buffer overflow presents (trap 8): nothing throws, nothing returns
// null, the screen is just black.
//
// Scoping the creation converts that into a message naming the pipeline. The
// pop is async (AllowSpontaneous, delivered by processEvents) because the
// blocking form is illegal on web — see platform.h note 1 — so the diagnostic
// may land a frame or two later. That is fine: it still names the pipeline,
// which is the whole point.
struct GpuPipelineScope {
    GpuPipelineScope(const wgpu::Device& device, const char* what);
    ~GpuPipelineScope();

    GpuPipelineScope(const GpuPipelineScope&) = delete;
    GpuPipelineScope& operator=(const GpuPipelineScope&) = delete;

private:
    wgpu::Device device_;
    const char* what_;
};

// True once any GpuPipelineScope has reported a failure. Checked after init so
// startup can say "the picture will be black and here is why" rather than
// leaving the user to guess.
bool gpuAnyPipelineFailed();

// How many uncaptured wgpu errors the device has reported. Together with the
// flag above this is what makes a broken build an EXIT CODE rather than a
// `grep -c 'wgpu error'` a human has to remember to run — see runHeadless,
// which returns 4. Trap 2's failure (a bind-group layout mismatch) renders a
// black screen while --carve-test exits 0, because 0 == 0 balances.
int gpuUncapturedErrorCount();

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

    // Native: blocking, exactly the sequence it always was.
    bool init(SDL_Window* window);

    // Web: the SAME sequence, inverted. The adapter and device requests were
    // the last two gpuBlockOn() callers (platform.h note 1 — a blocking wait
    // in a browser is an abort(), not a hang), so on web they become
    // AllowSpontaneous callbacks and everything downstream of them moves into
    // the callback. `onReady` fires exactly once, with false if any step
    // failed; the caller resumes startup there instead of on the next line.
    //
    // The stages below are shared by both paths so the two cannot drift over
    // WHAT gets requested — only over who waits.
    void initAsync(SDL_Window* window, std::function<void(bool)> onReady);

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

private:
    // ---- init stages, shared by the blocking and callback paths ----
    // Each is a plain step with no waiting in it; the WAIT (or the callback)
    // lives in init()/initAsync() around them. Splitting here is what lets the
    // browser path reuse the desktop path's exact requests.
    bool createInstance();
    wgpu::RequestAdapterOptions adapterOptions() const;
    // Kicks RequestAdapter and returns its future. `mode` and `then` are the
    // only things the two platforms disagree about: native passes WaitAnyOnly
    // and an empty `then` (it blocks on the future instead), web passes
    // AllowSpontaneous and the rest of startup as the continuation. `then`
    // runs INSIDE the wgpu callback, once the adapter has been stored.
    wgpu::Future requestAdapter(const wgpu::RequestAdapterOptions& opts,
                                wgpu::CallbackMode mode,
                                std::function<void()> then = {});
    // Prints the adapter line and latches hasTimestamps. Call once the adapter
    // has actually arrived.
    void reportAdapter();
    // Builds the device descriptor (limits, features, callbacks, and on native
    // the pipeline cache) and kicks RequestDevice. The descriptor is built and
    // consumed inside this call because WebGPU reads descriptors synchronously
    // — hoisting it to a member would be a lifetime trap for no gain.
    wgpu::Future requestDevice(wgpu::CallbackMode mode, std::function<void()> then = {});
    // Queue + surface. The last stage, once the device exists.
    bool finishInit(SDL_Window* window);
};
