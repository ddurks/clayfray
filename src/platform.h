#pragma once

// The one place the desktop build and the browser build are allowed to
// disagree. Everything web-hostile in clayfray reduces to two facts, and both
// are enforced by the browser rather than merely discouraged:
//
// 1. THE MAIN THREAD CANNOT BLOCK. A WebGPU promise is resolved by the same JS
//    event loop that runs wasm, so blocking on a GPU future there deadlocks
//    the tab. emdawnwebgpu does not let you try: `wgpuInstanceWaitAny` is a
//    hard `abort()` at ANY timeout, INCLUDING 0, unless the module is built
//    with Asyncify or JSPI — see `emwgpuWaitAny` in the port's
//    library_webgpu.js, whose non-Asyncify branch is one `abort()` line. There
//    is no "poll once and give up" fallback to write. So every blocking wait
//    goes through gpuBlockOn() (gpu.h): one named chokepoint, one place for
//    stage 2 to change. The same clause is why gpu.cpp must not request
//    `InstanceFeatureName::TimedWaitAny` on web — CreateInstance returns null
//    outright when it is asked for without Asyncify.
//
// 2. THERE IS NO LOCAL FILESYSTEM WORTH POLLING. Emscripten's MEMFS makes
//    `<filesystem>` and stdio COMPILE and mostly work, which is the trap: the
//    ctl inbox would happily scan an empty in-memory directory 60 times a
//    second, snapshots would write 80 MB into the wasm heap, and screenshots
//    would land somewhere no one can open. Those features are the desktop dev
//    loop, so the web build drops them wholesale rather than emulating them.
//    Reading is different and stays: shaders/ and assets/ are preloaded into
//    MEMFS by the linker at the SAME paths, so loadShader() and the .glb read
//    work unchanged.
//
// Native is the hard constraint (it is where the game is developed), so every
// switch below is defined such that the desktop build takes the path it always
// took and only __EMSCRIPTEN__ diverges.

// The desktop dev loop: the ctl file-polling server, snapshot save/load,
// shader hot reload, journal record/replay, screenshots, the debug GPU
// readbacks (debugStats/debugScanField) and the headless run modes
// (--screenshot / --carve-test / --serve / --replay).
//
// All of it is developer tooling that assumes a real filesystem and a process
// that may block; none of it is the game. 0 compiles it out.
#ifdef __EMSCRIPTEN__
#define CLAYFRAY_DEV_TOOLS 0
#else
#define CLAYFRAY_DEV_TOOLS 1
#endif

// Whether gpuBlockOn() can actually block. See note 1 above: on web this is 0
// unless the module is linked with JSPI (-DCLAYFRAY_WEB_JSPI=ON), which is the
// escape hatch if stage 2's callback inversion ever turns out to be more
// surgery than it is worth. Guard any NEW blocking wait with this rather than
// with __EMSCRIPTEN__, so the JSPI build stays a single flag flip.
#if !defined(__EMSCRIPTEN__) || defined(CLAYFRAY_WEB_JSPI)
#define CLAYFRAY_HAS_BLOCKING_GPU_WAIT 1
#else
#define CLAYFRAY_HAS_BLOCKING_GPU_WAIT 0
#endif
