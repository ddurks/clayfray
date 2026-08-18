#include "renderer.h"
#include <array>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include "snapshot.h"

// stb_image_write's Radiance (.hdr) writer calls sprintf, which macOS marks
// deprecated, so this warned on EVERY build. We never write .hdr — screenshots
// are PNG — but STB_IMAGE_WRITE_IMPLEMENTATION compiles the whole library, so
// the dead path warns anyway.
//
// Suppressed AT THE INCLUDE, deliberately, rather than with a project-wide
// -Wno-deprecated-declarations: this is vendored third-party code we do not
// get to fix, and our own code should keep the diagnostic. A build whose
// output is one permanent warning is a build nobody reads.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace {

// CLAYFRAY_SHADER_DIR is the SOURCE tree's shaders/ on desktop (that is what
// makes hot reload work) and "/shaders" on web, where the linker preloads the
// same directory into MEMFS at that path. Either way this is a plain runtime
// read of the same files — the web build needed no new loading path, only a
// different root. See CMakeLists.txt.
std::string shaderPath(const char* name) {
    return std::string(CLAYFRAY_SHADER_DIR) + "/" + name;
}

std::string readFileRaw(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "failed to read %s\n", path.c_str());
        return {};
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Stitch //#include name.wgsl lines (recursive, no duplicate guard — the
// include graph is a tree by construction) and expand //#constants into the
// generated geometry blocks: the volume block from src/brick.h and the ground
// field's region map from src/ground.h.
//
// //#constants goes in each compiled ROOT, never in an include: brick_read is
// pulled into both trace and pick, and WGSL rejects a duplicate const. A root
// that uses only one of the two blocks (pick.wgsl reads no ground) just gets
// unused consts, which WGSL allows.
std::string loadShader(const char* name) {
    std::string src = readFileRaw(shaderPath(name));
    std::stringstream out;
    std::stringstream in(src);
    std::string line;
    while (std::getline(in, line)) {
        const std::string tag = "//#include ";
        if (line.rfind("//#constants", 0) == 0) {
            out << BrickSystem::wgslConstants();
            out << GroundClay::wgslConstants();
            out << Renderer::wgslConstants();
        } else if (line.rfind(tag, 0) == 0) {
            std::string inc = line.substr(tag.size());
            while (!inc.empty() && (inc.back() == ' ' || inc.back() == '\r')) inc.pop_back();
            out << loadShader(inc.c_str()) << "\n";
        } else {
            out << line << "\n";
        }
    }
#if CLAYFRAY_DEV_TOOLS
    // CLAYFRAY_DUMP_WGSL=1 writes what the GPU actually compiles to
    // build/wgsl_dump/. The //#constants block is generated, so a bad emit is
    // invisible in the source tree — this is the only way to see it.
    if (const char* d = std::getenv("CLAYFRAY_DUMP_WGSL")) {
        if (d[0] == '1') {
            std::error_code ec;
            std::filesystem::create_directories("build/wgsl_dump", ec);
            std::ofstream f(std::string("build/wgsl_dump/") + name);
            f << out.str();
        }
    }
#endif
    return out.str();
}

#if CLAYFRAY_DEV_TOOLS
// Newest mtime across the shader directory; drives hot reload. Web has no
// editor writing into MEMFS, so the whole poll goes with the dev loop.
long shaderDirStamp() {
    namespace fs = std::filesystem;
    long newest = 0;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(CLAYFRAY_SHADER_DIR, ec)) {
        if (e.path().extension() != ".wgsl") continue;
        auto t = fs::last_write_time(e.path(), ec);
        if (ec) continue;
        long secs = (long)std::chrono::duration_cast<std::chrono::seconds>(
                        t.time_since_epoch())
                        .count();
        if (secs > newest) newest = secs;
    }
    return newest;
}
#endif

wgpu::ShaderModule makeModule(wgpu::Device& device, const std::string& src,
                              const char* label) {
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = src.c_str();
    wgpu::ShaderModuleDescriptor desc{};
    desc.nextInChain = &wgsl;
    desc.label = label;
    return device.CreateShaderModule(&desc);
}

} // namespace

// The uniform block's ARRAY CAPACITIES, emitted so trace.wgsl and pick.wgsl can
// size `capsules` and `gobs` off the C++ constants instead of retyping them.
//
// Trap 2's rule, applied to the two arrays its own fix missed: any array sized
// by a C++ constant must be DERIVED in the WGSL, never written as a literal.
// `marbles` was `array<vec4f, 32>` agreeing with the C++ by coincidence, and
// when the coincidence broke EVERY bind group against that layout failed to
// create — a healthy-looking boot rendering a black screen while --carve-test
// exited 0. `capsules[32]` and `gobs[24]` were the same shape and survived that
// fix; now they cannot drift either.
std::string Renderer::wgslConstants() {
    char buf[512];
    const int need = std::snprintf(buf, sizeof(buf),
                                   "// GENERATED from Renderer in src/renderer.h — "
                                   "do not hand-copy into a shader.\n"
                                   "const MAX_CAPSULES: u32 = %uu;\n"
                                   "const MAX_GOBS: u32 = %uu;\n"
                                   // post.wgsl shares this one uniform buffer
                                   // but reads only the first 13 slots and the
                                   // focus pair at the very end, so it declares
                                   // one pad between them. GENERATED because
                                   // hand-counting it is trap 2 with no
                                   // validation error to catch it: a stale pad
                                   // still FITS the buffer and simply reads the
                                   // wrong 32 bytes, putting the defocus
                                   // ellipse somewhere the tracer never used.
                                   "const FOCUS_PAD: u32 = %uu;\n",
                                   (uint32_t)kMaxCapsules, (uint32_t)kMaxGobs,
                                   (uint32_t)(kSlotFocus - 13));
    wgslConstantsFit(need, sizeof(buf), "Renderer");
    return buf;
}

bool Renderer::init(Gpu& gpu, int width, int height) {
    gpu_ = &gpu;
    width_ = width;
    height_ = height;

    wgpu::SamplerDescriptor sampDesc{};
    sampDesc.magFilter = wgpu::FilterMode::Linear;
    sampDesc.minFilter = wgpu::FilterMode::Linear;
    sampDesc.addressModeU = wgpu::AddressMode::ClampToEdge;
    sampDesc.addressModeV = wgpu::AddressMode::ClampToEdge;
    sampler_ = gpu_->device.CreateSampler(&sampDesc);

    wgpu::BufferDescriptor ubDesc{};
    ubDesc.size = kUniformSlots * 16;
    ubDesc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    uniformBuf_ = gpu_->device.CreateBuffer(&ubDesc);

    wgpu::BufferDescriptor pickDesc{};
    pickDesc.size = 64; // pos+hit, normal+mat, albedo, REST point
    pickDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc;
    pickOut_ = gpu_->device.CreateBuffer(&pickDesc);
    wgpu::BufferDescriptor pickReadDesc{};
    pickReadDesc.size = 64;
    pickReadDesc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    pickRead_ = gpu_->device.CreateBuffer(&pickReadDesc);

    // OPT-IN: Dawn's Metal pass-boundary timestamps destabilize heavy
    // multi-pass frames on Apple Silicon (dropped command buffers -> blank
    // frames) and report dubious values. Wall-clock benchmarking is the
    // default tool here; timestamps work properly on D3D12/Vulkan.
    if (gpu_->hasTimestamps && std::getenv("CLAYFRAY_TS")) {
        wgpu::QuerySetDescriptor qsDesc{};
        qsDesc.type = wgpu::QueryType::Timestamp;
        qsDesc.count = 4;
        querySet_ = gpu_->device.CreateQuerySet(&qsDesc);
        wgpu::BufferDescriptor rDesc{};
        rDesc.size = 4 * 8;
        rDesc.usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc;
        queryResolve_ = gpu_->device.CreateBuffer(&rDesc);
        wgpu::BufferDescriptor mDesc{};
        mDesc.size = 4 * 8;
        mDesc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
        queryRead_ = gpu_->device.CreateBuffer(&mDesc);
    }

    // One store, one slice per fighter. Every BrickSystem still owns its whole
    // pass chain (voxelize, edit, JFA, redistance) and works on its own slice
    // unchanged, so one fighter's carving can never touch another's clay — but
    // they cost three storage bindings between them instead of three each.
    if (!store_.init(gpu)) return false;
    for (int i = 0; i < kMaxPlayers; i++) {
        if (!fighters_[i].vol.init(gpu, store_, i, loadShader("edit.wgsl"),
                                   loadShader("jfa.wgsl"),
                                   loadShader("redistance.wgsl"),
                                   loadShader("voxelize.wgsl")))
            return false;
    }
    fighters_[0].enabled = true; // the hero always exists
    if (!ground_.init(gpu, loadShader("ground.wgsl"))) return false;

    // default marbles = the classic hand-coded eyes (linearized colors)
    auto lin = [](float c) { return std::pow(c, 2.2f); };
    for (int s : {1, -1}) {
        MarbleProp eye{};
        eye.pos[0] = s * 0.084f; eye.pos[1] = 1.055f; eye.pos[2] = 0.055f;
        eye.radius = 0.078f;
        eye.color[0] = lin(0.84f); eye.color[1] = lin(0.82f); eye.color[2] = lin(0.72f);
        marbles_.push_back(eye);
        MarbleProp pupil{};
        pupil.pos[0] = s * 0.094f; pupil.pos[1] = 1.069f; pupil.pos[2] = 0.123f;
        pupil.radius = 0.026f;
        pupil.color[0] = pupil.color[1] = pupil.color[2] = 0.015f;
        marbles_.push_back(pupil);
    }
    buildTargets();
    if (!buildPipelines()) return false;
    buildBindGroups();
#if CLAYFRAY_DEV_TOOLS
    shaderDirStamp_ = shaderDirStamp(); // hot-reload baseline
#endif
    reuseEnabled_ = std::getenv("CLAYFRAY_NO_REUSE") == nullptr;
    return true;
}

void Renderer::resize(int width, int height) {
    if (width == width_ && height == height_) return;
    traceValid_ = false; // new targets: the cached trace is gone
    width_ = width;
    height_ = height;
    buildTargets();
    buildBindGroups();
}

void Renderer::buildTargets() {
    wgpu::TextureDescriptor hdrDesc{};
    hdrDesc.size = {(uint32_t)width_, (uint32_t)height_, 1};
    hdrDesc.format = wgpu::TextureFormat::RGBA16Float;
    hdrDesc.usage = wgpu::TextureUsage::StorageBinding | wgpu::TextureUsage::TextureBinding;
    hdrTex_ = gpu_->device.CreateTexture(&hdrDesc);
    hdrView_ = hdrTex_.CreateView();

    wgpu::TextureDescriptor presDesc{};
    presDesc.size = {(uint32_t)width_, (uint32_t)height_, 1};
    presDesc.format = wgpu::TextureFormat::RGBA8Unorm;
    presDesc.usage = wgpu::TextureUsage::RenderAttachment |
                     wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc;
    presentTex_ = gpu_->device.CreateTexture(&presDesc);
    presentView_ = presentTex_.CreateView();
}

bool Renderer::buildPipelines() {
    std::string traceSrc = loadShader("trace.wgsl");
    std::string postSrc = loadShader("post.wgsl");
    std::string blitSrc = loadShader("blit.wgsl");
    std::string pickSrc = loadShader("pick.wgsl");
    if (traceSrc.empty() || postSrc.empty() || blitSrc.empty() || pickSrc.empty())
        return false;

    auto tick = std::chrono::steady_clock::now();
    auto lap = [&tick](const char* what) {
        auto now = std::chrono::steady_clock::now();
        double s = std::chrono::duration<double>(now - tick).count();
        if (s > 0.5) {
            std::printf("[startup] %s pipeline: %.1fs\n", what, s);
            std::fflush(stdout);
        }
        tick = now;
    };
    wgpu::Device& dev = gpu_->device;
    {
        wgpu::ShaderModule mod = makeModule(dev, traceSrc, "trace");
        wgpu::ComputePipelineDescriptor desc{};
        desc.label = "trace";
        desc.compute.module = mod;
        desc.compute.entryPoint = "cs";
        {
            // trace sits at 7 of the 8 storage buffers core WebGPU
            // guarantees (trap 8) — if a conformant device ever rejects it,
            // this is the line that says so instead of a black screen.
            GpuPipelineScope scope(dev, "trace");
            tracePipeline_ = dev.CreateComputePipeline(&desc);
        }
        lap("trace");
        // B5: the periphery. Same module, same map()/shade(), same bindings —
        // it differs only in what it is dispatched over and how many texels
        // one result covers.
        desc.label = "traceCoarse";
        desc.compute.entryPoint = "csCoarse";
        {
            GpuPipelineScope scope(dev, "traceCoarse");
            traceCoarsePipeline_ = dev.CreateComputePipeline(&desc);
        }
        lap("traceCoarse");
    }
    {
        wgpu::ShaderModule mod = makeModule(dev, pickSrc, "pick");
        wgpu::ComputePipelineDescriptor desc{};
        desc.label = "pick";
        desc.compute.module = mod;
        desc.compute.entryPoint = "cs";
        {
            GpuPipelineScope scope(dev, "pick");
            pickPipeline_ = dev.CreateComputePipeline(&desc);
        }
        lap("pick");
    }
    {
        wgpu::ShaderModule mod = makeModule(dev, postSrc, "post");
        wgpu::ColorTargetState target{};
        target.format = wgpu::TextureFormat::RGBA8Unorm;
        wgpu::FragmentState frag{};
        frag.module = mod;
        frag.entryPoint = "fs";
        frag.targetCount = 1;
        frag.targets = &target;
        wgpu::RenderPipelineDescriptor desc{};
        desc.label = "post";
        desc.vertex.module = mod;
        desc.vertex.entryPoint = "vs";
        desc.fragment = &frag;
        desc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        GpuPipelineScope scope(dev, "post");
        postPipeline_ = dev.CreateRenderPipeline(&desc);
    }
    if (gpu_->surface) {
        wgpu::ShaderModule mod = makeModule(dev, blitSrc, "blit");
        wgpu::ColorTargetState target{};
        target.format = gpu_->surfaceFormat;
        wgpu::FragmentState frag{};
        frag.module = mod;
        frag.entryPoint = "fs";
        frag.targetCount = 1;
        frag.targets = &target;
        wgpu::RenderPipelineDescriptor desc{};
        desc.label = "blit";
        desc.vertex.module = mod;
        desc.vertex.entryPoint = "vs";
        desc.fragment = &frag;
        desc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        GpuPipelineScope scope(dev, "blit");
        blitPipeline_ = dev.CreateRenderPipeline(&desc);
    }
    return tracePipeline_ && traceCoarsePipeline_ && postPipeline_ && pickPipeline_;
}

void Renderer::buildBindGroups() {
    wgpu::Device& dev = gpu_->device;
    // The focus pass and the periphery pass are two entry points off one
    // module reaching the same code, so their auto-derived layouts hold the
    // same bindings — but a bind group is validated against the layout of the
    // pipeline it was created from, so each gets its own pair rather than a
    // bet on Dawn deduplicating them. Neither adds a binding (trap 8).
    auto traceBinds = [&](const wgpu::ComputePipeline& pipe, wgpu::BindGroup& g0,
                          wgpu::BindGroup& g1) {
        {
            wgpu::BindGroupEntry entries[2] = {};
            entries[0].binding = 0;
            entries[0].buffer = uniformBuf_;
            entries[1].binding = 1;
            entries[1].textureView = hdrView_;
            wgpu::BindGroupDescriptor desc{};
            desc.layout = pipe.GetBindGroupLayout(0);
            desc.entryCount = 2;
            desc.entries = entries;
            g0 = dev.CreateBindGroup(&desc);
        }
        {
            // FOUR storage buffers for the whole scene, at ANY number of
            // fighters. Three levels of packing get it there (CLAUDE.md trap 8:
            // core WebGPU guarantees a stage only EIGHT, which is the limit
            // that governs mobile):
            //   - indirection + seeds + coarse are regions of one `volume`
            //   - the ground's base + height + colour are regions of `field`
            //   - and every FIGHTER is a slice of the same three store buffers,
            //     which is what this bind group being fighter-independent means.
            // M5 bound fighter 0 here and fighter 1 in a second group, for 7 of
            // 8 and no room for a third body. This is 4 of 8 with four to
            // spare — and B5's second entry point spends none of them.
            wgpu::BindGroupEntry entries[4] = {};
            entries[0].binding = 0;
            entries[0].buffer = store_.volume;
            entries[1].binding = 1;
            entries[1].buffer = store_.distPool;
            entries[2].binding = 2;
            entries[2].buffer = store_.albedoPool;
            entries[3].binding = 7;
            entries[3].buffer = ground_.field;
            wgpu::BindGroupDescriptor desc{};
            desc.layout = pipe.GetBindGroupLayout(1);
            desc.entryCount = 4;
            desc.entries = entries;
            g1 = dev.CreateBindGroup(&desc);
        }
    };
    traceBinds(tracePipeline_, traceBind_, traceBrickBind_);
    traceBinds(traceCoarsePipeline_, traceCoarseBind_, traceCoarseBrickBind_);
    {
        wgpu::BindGroupEntry entries[2] = {};
        entries[0].binding = 0;
        entries[0].buffer = uniformBuf_;
        entries[1].binding = 1;
        entries[1].buffer = pickOut_;
        wgpu::BindGroupDescriptor desc{};
        desc.layout = pickPipeline_.GetBindGroupLayout(0);
        desc.entryCount = 2;
        desc.entries = entries;
        pickBind_ = dev.CreateBindGroup(&desc);
    }
    {
        // Same three as trace, minus the ground: pick marches the bodies only.
        // (Before the volume regions merged, trace and pick needed different
        // entry counts here — pick's auto layout dropped the coarse field it
        // never reads. One binding now carries every per-cell array, so both
        // layouts agree.) group(2) is GONE: it held fighter 1's volume, and
        // there is no per-fighter group left to hold.
        wgpu::BindGroupEntry entries[3] = {};
        entries[0].binding = 0;
        entries[0].buffer = store_.volume;
        entries[1].binding = 1;
        entries[1].buffer = store_.distPool;
        entries[2].binding = 2;
        entries[2].buffer = store_.albedoPool;
        wgpu::BindGroupDescriptor desc{};
        desc.layout = pickPipeline_.GetBindGroupLayout(1);
        desc.entryCount = 3;
        desc.entries = entries;
        pickBrickBind_ = dev.CreateBindGroup(&desc);
    }
    {
        wgpu::BindGroupEntry entries[3] = {};
        entries[0].binding = 0;
        entries[0].buffer = uniformBuf_;
        entries[1].binding = 1;
        entries[1].textureView = hdrView_;
        entries[2].binding = 2;
        entries[2].sampler = sampler_;
        wgpu::BindGroupDescriptor desc{};
        desc.layout = postPipeline_.GetBindGroupLayout(0);
        desc.entryCount = 3;
        desc.entries = entries;
        postBind_ = dev.CreateBindGroup(&desc);
    }
    if (blitPipeline_) {
        wgpu::BindGroupEntry entries[2] = {};
        entries[0].binding = 0;
        entries[0].textureView = presentView_;
        entries[1].binding = 1;
        entries[1].sampler = sampler_;
        wgpu::BindGroupDescriptor desc{};
        desc.layout = blitPipeline_.GetBindGroupLayout(0);
        desc.entryCount = 2;
        desc.entries = entries;
        blitBind_ = dev.CreateBindGroup(&desc);
    }
}

bool Renderer::reloadShadersIfChanged() {
#if !CLAYFRAY_DEV_TOOLS
    return false;
#else
    long stamp = shaderDirStamp();
    if (stamp == shaderDirStamp_) return false;
    shaderDirStamp_ = stamp;
    std::printf("reloading shaders\n");
    ground_.rebuildPipelines(loadShader("ground.wgsl"));
    if (buildPipelines()) buildBindGroups();
    traceValid_ = false; // recompiled shaders may trace differently
    // character source may have changed; rebuild the volume from it
    fighters_[0].vol.requestBake();
    return true;
#endif
}

// Digest of everything the TRACE pass reads. Deliberately excludes the three
// things that change every frame without changing the traced image:
//   camUp.w   frame.time  — not read by any shader
//   res.z     grainFrame  — post only (so grain still animates over a reuse)
//   post.xyzw, post2.xyz  — post only; post2.w is debugMode and IS traced
//   mouse     pick uv     — pick runs every frame regardless, on its own pass
// Volume CONTENTS are not in the uniform buffer at all, so the caller folds in
// the brick/ground generation counters separately.
// Slot -> what lives there, so a re-trace report reads as "gobs" rather than
// "slot 271". Mirrors the Uniforms layout (trap 2); boundaries match the
// hardcoded writes in packUniforms.
const char* Renderer::uniformSlotName(int s) {
    static const char* head[14] = {"camPos",   "camRight", "camUp",    "camFwd",
                                   "res",      "keyPos",   "keyColor", "rimDir",
                                   "rimColor", "ambient",  "material", "post",
                                   "post2",    "mouse"};
    if (s < 14) return head[s];
    if (s < kSlotMarbleMeta) return "marbles (eyes)";
    if (s == kSlotMarbleMeta) return "marbleMeta";
    if (s == kSlotSceneMeta) return "sceneMeta (live fighter count)";
    if (s == kSlotCapsMeta) return "capsMeta";
    if (s == kSlotCapsCenter) return "capsCenter";
    if (s < kSlotGobMeta) return "capsules (player 0 shadow proxy)";
    if (s == kSlotGobMeta) return "gobMeta (in-flight count)";
    if (s < kSlotGroundMeta) return "gobs (flying clay)";
    if (s == kSlotGroundMeta) return "groundMeta (clay top bound)";
    if (s == kSlotSwordA) return "swordA (hilt)";
    if (s == kSlotSwordA + 1) return "swordB (tip)";
    if (s == kSlotSwordA + 2) return "swordCol";
    if (s == kSlotFocus) return "focus (foveation centre/size)";
    if (s == kSlotFocusMeta) return "focusMeta (foveation)";
    if (s == kSlotKeyAim) return "keyAim (follow-spot target)";
    // One name per fighter block, so CLAYFRAY_DEBUG_REUSE blames the body that
    // actually moved rather than a bare slot number.
    static char buf[48];
    const int i = (s - kSlotFighters) / kFighterSlots;
    const int off = (s - kSlotFighters) % kFighterSlots;
    const char* what = off == 0 ? "meta" : (off == 1 ? "center" : "pieces");
    std::snprintf(buf, sizeof(buf), "fighter %d %s", i, what);
    return buf;
}

// Which uniform components the reuse digest is allowed to see. Kept beside
// traceInputDigest so the digest and the CLAYFRAY_DEBUG_REUSE report below
// cannot drift into disagreeing about what counts as a change.
bool Renderer::digestIncludes(int s, int c) {
    if (s == 13) return false;               // pick uv
    if (s == 11) return false;               // post
    if (s == 2 && c == 3) return false;      // frame.time
    if (s == 4 && c == 2) return false;      // grainFrame
    if (s == 12 && c != 3) return false;     // post2: keep only debugMode
    // focusMeta.z is the POST defocus radius. The rest of the focus block IS
    // traced — it decides which texels get a ray of their own — so only this
    // one component is excused, exactly like grainFrame above.
    if (s == kSlotFocusMeta && c == 2) return false;
    return true;
}

uint64_t Renderer::traceInputDigest(const float u[kUniformSlots][4]) const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](float f) {
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        if (bits == 0x80000000u) bits = 0u; // -0.0 and 0.0 are the same input
        h ^= bits;
        h *= 1099511628211ull;
    };
    for (int s = 0; s < kUniformSlots; s++) {
        for (int c = 0; c < 4; c++) {
            if (digestIncludes(s, c)) mix(u[s][c]);
        }
    }
    return h;
}

// ---------- M-PERF: the three-piece affine rig ----------

bool Renderer::affineOn(const LookParams& look) const {
    if (fighters_[0].pieces.empty()) return false;
    // THE A/B THAT USED TO LIVE HERE IS GONE, and it is worth saying why so
    // nobody tries to restore it. `look.affineRig` / CLAYFRAY_NO_AFFINE=1
    // switched between this rig and the 13-piece inverse-LBS warp, on one
    // binary, so the pair could be benchmarked without a shader edit. The warp
    // inverted a linear blend over a skeleton; the asset has no skeleton, no
    // skin and no weights, so there is no second rig left to switch to and the
    // comparison has no meaning.
    //
    // The flag is kept, repurposed to something that IS still useful on a
    // skeleton-free asset: off = draw the rest volume unposed (pieces = 0, the
    // shader's un-rigged path). That answers "is the rig wrong or is the
    // VOLUME wrong?" in one keystroke, which is the question that actually
    // comes up now. packUniforms implements the pn = 0 side.
    static const bool disabled = std::getenv("CLAYFRAY_NO_AFFINE") != nullptr;
    if (skeletonFree()) return !disabled && look.affineRig;
    return !disabled && look.affineRig && bones_.size() <= 16;
}

// Rest -> world for the three brush pieces, for ONE fighter.
//
// piece 0 BODY: the existing affine, unchanged — spring squish, then the lean
//   shear, then yaw, then translate/hop about the feet.
// piece 1/2 HANDS: rigid. Each maps its selected brush back to the authored
//   (canonical) hand frame, centres the palm, mirrors x for the right hand,
//   orients onto the blade, and lands on its grip.
// A brush's AABB as a capsule: axis along the box's longest dimension, radius
// the smaller of the other two half-extents. Crude, but it is only the SHADOW
// PROXY (charProxy), which is smin-ed and then max-ed against the real field —
// so it may not be tight, it just must not be wildly too small.
void Renderer::capsuleFromBox(const float lo[3], const float hi[3], float a[3],
                              float b[3], float& r) {
    float c[3], h[3];
    for (int k = 0; k < 3; k++) {
        c[k] = (lo[k] + hi[k]) * 0.5f;
        h[k] = (hi[k] - lo[k]) * 0.5f;
    }
    int axis = 0;
    for (int k = 1; k < 3; k++)
        if (h[k] > h[axis]) axis = k;
    const int o1 = (axis + 1) % 3, o2 = (axis + 2) % 3;
    r = std::min(h[o1], h[o2]);
    const float half = std::max(h[axis] - r, 0.f);
    for (int k = 0; k < 3; k++) {
        a[k] = c[k];
        b[k] = c[k];
    }
    a[axis] = c[axis] - half;
    b[axis] = c[axis] + half;
}

float Renderer::handReach(const LookParams& look) const {
    return look.hands.reach > 0.f ? look.hands.reach
                                  : autoReach_ * look.hands.reachScale;
}

// One unarmed mitt, in CHARACTER space (before the body affine and before the
// per-side mirror, so this is always the AUTHORED side's answer and the right
// mitt is its reflection).
//
// The rotation is Ry(yaw) * Rz(-pitch) about the PALM: the authored mitt points
// its fingers along +x, Rz(-pitch) tips them down, and Ry(yaw) then swings the
// whole hand about the vertical. Composed in that order so the droop happens in
// the hand's own frame — the other order rolls a hanging hand sideways.
void Renderer::unarmedHand(const FighterPose& disp, const LookParams& look, int side,
                           float poseTime, float outPos[3], float outRot[16]) const {
    const HandPoseParams& hp = look.handPose;
    // A punch IS the guard pose with the lead fist thrown, so anything mid-jab
    // reads as guarding whatever the flag says. That keeps the brush selection
    // in render() and the placement here agreeing on one predicate.
    const bool guard = disp.guard || disp.punch > 0.f;
    const float* base = guard ? hp.fistPos : hp.idlePos;
    float p[3] = {base[0], base[1], base[2]};
    float pitch = guard ? hp.fistPitch : hp.idlePitch;
    float yaw = guard ? hp.fistYaw : hp.idleYaw;

    if (disp.punch > 0.f && side == (disp.punchSide & 1)) {
        const float u = std::min(std::max(disp.punch, 0.f), 1.f);
        p[2] += hp.punchReach * u;
        // the fist crosses toward the midline and the knuckles square up as it
        // extends — a jab finishes in line with the body, not out at the hip
        p[0] -= p[0] * 0.55f * u;
        yaw *= 1.f - 0.6f * u;
    }

    // ---- the bob ----
    // Deliberately the same expression as GameState::swordOffset, sampled off
    // the same 12 Hz clock: lift on sin(rate), roll on sin(rate/2) so the two
    // never quite line up and the loop does not read as a metronome. Both mitts
    // are IN PHASE, because the sword bobs one hilt and two hands ride it — a
    // phase offset here would make putting the sword down change the idle.
    const float amp = hp.bobAmp * (disp.moving ? hp.bobAmpMove : 1.f);
    const float rate = disp.moving ? hp.bobRateMove : hp.bobRate;
    p[1] += amp * std::sin(poseTime * rate);
    pitch += hp.bobTilt * std::sin(poseTime * rate * 0.5f);

    // The reach ball, the only thing holding a mitt to an armless body. The
    // sword's grips are clamped by exactly this (HandParams::reach), so a
    // punch cannot out-reach a sword thrust: same ball, same radius.
    const float reach = handReach(look);
    if (reach > 0.f) {
        float d[3], len = 0.f;
        for (int k = 0; k < 3; k++) {
            d[k] = p[k] - brush_.bodyCenter[k];
            len += d[k] * d[k];
        }
        len = std::sqrt(len);
        if (len > reach && len > 1e-6f) {
            const float f = reach / len;
            for (int k = 0; k < 3; k++) p[k] = brush_.bodyCenter[k] + d[k] * f;
        }
    }
    std::memcpy(outPos, p, sizeof(float) * 3);

    const float cp = std::cos(pitch), sp = std::sin(pitch);
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    // Rz(-pitch) columns: (cp, -sp, 0), (sp, cp, 0), (0, 0, 1)
    // Ry(yaw) columns:    (cy, 0, -sy), (0, 1, 0), (sy, 0, cy)
    matIdentity(outRot);
    outRot[0] = cp * cy;  outRot[1] = -sp; outRot[2] = -cp * sy;
    outRot[4] = sp * cy;  outRot[5] = cp;  outRot[6] = -sp * sy;
    outRot[8] = sy;       outRot[9] = 0.f; outRot[10] = cy;
}

void Renderer::updateBrushRig(std::vector<AffinePiece>& pieces, const int handPose[2],
                              const FighterPose& disp, const LookParams& look,
                              const SwordParams* grip, float poseTime,
                              const float glance[2][3]) const {
    if (!brush_.valid || pieces.size() < 3) return;
    float A[16];
    bodyAffine(disp, look.rig, A);
    std::memcpy(pieces[0].xform, A, sizeof(A));
    for (int k = 0; k < 3; k++) {
        pieces[0].lo[k] = brush_.bodyLo[k];
        pieces[0].hi[k] = brush_.bodyHi[k];
    }

    // Blade frame. `offsetDir` is the fighter's own +X pushed perpendicular to
    // the blade — NOT cross(blade, up), which degenerates exactly in the
    // vertical guard the sword rests in most of the time.
    float hilt[3] = {0, 0, 0}, tip[3] = {0, 0, 0};
    float gA[3] = {0, 0, 0}, gB[3] = {0, 0, 0};
    float blade[3] = {0.f, 1.f, 0.f};
    if (grip) {
        swordGeometry(*grip, hilt, tip, gA, gB);
        for (int k = 0; k < 3; k++) blade[k] = tip[k] - hilt[k];
        const float bl = std::sqrt(blade[0] * blade[0] + blade[1] * blade[1] +
                                   blade[2] * blade[2]);
        if (bl > 1e-6f) for (int k = 0; k < 3; k++) blade[k] /= bl;
    }
    const float cy = std::cos(disp.yaw), sy = std::sin(disp.yaw);
    float offs[3] = {cy, 0.f, -sy};
    const float dotOB = offs[0] * blade[0] + offs[1] * blade[1] + offs[2] * blade[2];
    for (int k = 0; k < 3; k++) offs[k] -= dotOB * blade[k];
    float ol = std::sqrt(offs[0] * offs[0] + offs[1] * offs[1] + offs[2] * offs[2]);
    if (ol < 1e-5f) { offs[0] = 1.f; offs[1] = 0.f; offs[2] = 0.f; ol = 1.f; }
    for (int k = 0; k < 3; k++) offs[k] /= ol;

    for (int side = 0; side < 2; side++) { // 0 = left, 1 = right
        AffinePiece& ap = pieces[1 + side];
        const int b =
            std::min(std::max(handPose[side], 0), (int)kHandBrushCount - 1);
        for (int k = 0; k < 3; k++) {
            ap.lo[k] = brush_.handLo[b][k];
            ap.hi[k] = brush_.handHi[b][k];
        }
        // Mirror in x for the right hand. det = -1, but M^T M = I so every
        // singular value is 1: distance-preserving, and mat3MinSingular (which
        // forms M^T M) returns exactly 1 for it. Confirmed, not assumed.
        const float mir = (side == 1) ? -1.f : 1.f;

        // Pre-translation applied in BRUSH space, before the mirror: brush ->
        // canonical -> palm at the origin, so whatever transform follows lands
        // the PALM on its target and any rotation spins the mitt in place.
        //
        // It used to be brush -> canonical only when unarmed, which left the
        // hand wherever the artist happened to model it. That was fine while
        // "unarmed" meant "the sword is switched off"; it is not a pose, and
        // idle/guard/punch need the mitt placed, so both branches now centre
        // the palm and differ only in where they put it.
        float pre[3];
        for (int k = 0; k < 3; k++) {
            pre[k] = -brush_.ofs[b][k] - brush_.palm[b][k];
        }

        float M[16];
        if (grip) {
            // Fingers point ACROSS the blade, toward the other hand. The
            // mirror supplies the flip, so `offs` is the target finger axis for
            // BOTH hands: final finger dir = mir * offs, and each hand sits at
            // sideSign * offs, so the fingers always face inward.
            const float sideSign = (side == 1) ? 1.f : -1.f;
            const float* g = (side == 1) ? gA : gB; // right takes the near grip
            const float lateral = look.hands.gripSpread * grip->radius * sideSign;
            // WHICH mitt axis runs along the blade: the grip HOLE, i.e. the
            // axis the fingers curl AROUND. Read it off the grab morph rather
            // than guessing — its deltas are large in y (-0.154..0.050) and z
            // (-0.117..0.046) and small in x (-0.051..0.031), so the fingers
            // rotate about the mitt's x axis and the blade passes along x.
            // Mapping the THINNEST axis to the blade instead put the blade
            // across the knuckles rather than through the gap.
            float c0[3] = {blade[0], blade[1], blade[2]};
            float c1[3] = {offs[0], offs[1], offs[2]};
            // gripRoll spins the mitt about the blade so the finger gap can be
            // lined up by eye without a rebuild (ctl: hands.gripRoll). Mirrored
            // per side so both hands roll the same way relative to their own
            // palm rather than opposite ways in world space.
            {
                const float a = look.hands.gripRoll * ((side == 1) ? -1.f : 1.f);
                const float ca = std::cos(a), sa = std::sin(a);
                float perp[3] = {c0[1] * c1[2] - c0[2] * c1[1],
                                 c0[2] * c1[0] - c0[0] * c1[2],
                                 c0[0] * c1[1] - c0[1] * c1[0]};
                for (int k = 0; k < 3; k++) c1[k] = c1[k] * ca + perp[k] * sa;
            }
            float c2[3] = {c0[1] * c1[2] - c0[2] * c1[1],
                           c0[2] * c1[0] - c0[0] * c1[2],
                           c0[0] * c1[1] - c0[1] * c1[0]};
            // WHICH mitt axis runs along the blade is a LOOK call, not a
            // derivable one — it depends on how the artist authored the mesh,
            // and reasoning from the grab morph's delta axes gave the wrong
            // answer. So it is a control: hands.gripAxis picks the brush axis
            // (0=x, 1=y, 2=z) that the blade threads through, and gripRoll
            // spins the mitt about the blade. Both are ctl-settable and live
            // in the panel, so the orientation is dialled by eye in the
            // running app without a rebuild.
            //
            // frame[0] is the blade, frame[1] the lateral (toward the other
            // hand), frame[2] their cross. Writing them into M's columns in a
            // rotated order is what re-labels which brush axis is which.
            const float frame[3][3] = {{c0[0], c0[1], c0[2]},
                                       {c1[0], c1[1], c1[2]},
                                       {c2[0], c2[1], c2[2]}};
            const int ax = ((look.hands.gripAxis % 3) + 3) % 3;
            for (int c = 0; c < 3; c++) {
                // column `ax` gets the blade, the other two follow in order
                const int f = (c - ax + 3) % 3;
                // mir on the LATERAL column only, so det flips exactly once
                const float sgn = (f == 1) ? mir : 1.f;
                for (int k = 0; k < 3; k++) M[c * 4 + k] = frame[f][k] * sgn;
                M[c * 4 + 3] = 0.f;
            }
            for (int k = 0; k < 3; k++) M[12 + k] = g[k] + offs[k] * lateral;
            M[15] = 1.f;
        } else {
            // No sword: place the palm from look.handPose (idle hang, guard,
            // or a thrown punch) in CHARACTER space, then ride the body affine.
            // The mirror sits between the two, so the right mitt is the exact
            // reflection of the left — position, droop and knuckle roll all
            // flip together, which is what stops a mirrored hand from pointing
            // its fingers the wrong way round the body.
            float target[3], R[16];
            unarmedHand(disp, look, side, poseTime, target, R);
            R[12] = target[0];
            R[13] = target[1];
            R[14] = target[2];
            float Mir[16];
            matIdentity(Mir);
            Mir[0] = mir;
            float AM[16];
            matMul(A, Mir, AM);
            matMul(AM, R, M);
        }
        // fold the pre-translation in: xform = M * T(pre)
        float T[16];
        matIdentity(T);
        T[12] = pre[0]; T[13] = pre[1]; T[14] = pre[2];
        matMul(M, T, ap.xform);
        // R22: the glance is a WORLD-space push-out, so it lands straight on
        // the translation column — pre-multiplying T(glance) would be the same
        // three adds through a 4x4. It moves the WHOLE piece, which is what
        // makes it correct rather than convenient: the brush's rest AABB is
        // clipped after the inverse transform, so box and clay travel together,
        // posedCapsule reads the same matrix so the shadow proxy follows, and
        // updatePunchCut reads the fist's world position off it too — which is
        // the feedback that keeps the deflection from running away.
        for (int k = 0; k < 3; k++) ap.xform[12 + k] += glance[side][k];
    }
}

// ---- what the eye gets out of `u` ----
//
// The integrator that produces it is Body::stepHop in main.cpp — the hopper is
// SIM state now, because travel depends on it (see RigParams). Everything below
// is display.
//
// The rest position of the spring under the body's own weight: gravity balances
// the spring force at legK * u == -gravity. This is where a standing fighter
// sits, and it is the ZERO of the squish because the artist authored the blob
// standing — see RigParams.
float Renderer::springSag(const RigParams& r) {
    return r.legK > 1e-3f ? -r.gravity / r.legK : 0.f;
}

// Squish, as a fraction of the body's height: metres of compression over the
// lever. Grounded it is the compression relative to the sag; AIRBORNE the leg
// is at its unloaded length, so it saturates at +|sag|/height — the body
// relaxes to full size the moment it is off the floor, and holds there for the
// whole flight. That plateau is the stretch, and it is why min(u, 0) rather
// than u: past takeoff the shape stops tracking the height.
float Renderer::springSquish(float u, const RigParams& r, float bodyHeight) {
    const float lever = std::max(bodyHeight, 1e-3f);
    return std::min(
        std::max((std::min(u, 0.f) - springSag(r)) / lever, -0.45f), 0.45f);
}

// Feet off the floor, metres. No scale factor and no `moving` gate: this is a
// distance the simulation produced, and an idle fighter stays down because its
// breath is too weak to launch it, not because a flag says so.
float Renderer::springLift(float u) {
    return std::max(u, 0.f);
}

// The body's whole articulation, as one matrix:
//   world = T(pos + hop) . Yaw(yaw) . Shear(lean) . Scale(squish)
// Scale and shear are both about the FEET (y = 0), so the fighter stays
// planted while the top of the blob squashes and tips — which is what reads as
// clay rather than as a rigid body pivoting off the floor.
//
// Lean is a SHEAR, not the rotation the 13-piece path used. Same visual, but a
// rotation lifts the base off the ground and a shear does not. It also reduces
// to exactly that rotation when ky = cos(lean) and t = tan(lean), which is the
// sanity check that the sign convention below matches the old root transform:
// positive lean tips the head toward +Z, the fighter's forward.
//
void Renderer::bodyAffine(const FighterPose& disp, const RigParams& r,
                          float out[16]) const {
    // M-SPRING: both of these come out of the ONE hopper coordinate, which the
    // sim wrote and render() has already latched onto the pose grid. Squashed
    // while the body is on the floor, relaxed to full length the moment it is
    // off it, lifted by however far off it is.
    const float q = springSquish(disp.hopU, r, bodyHeight_);
    const float ky = std::min(std::max(1.f + q, 0.55f), 1.45f);
    // sideways bulge: squashing down pushes clay out. Not strictly volume
    // preserving — `widen` is a taste knob, and clay is not water.
    const float kxz = std::min(std::max(1.f - r.widen * q, 0.55f), 1.65f);
    const float t = std::tan(disp.lean);
    const float hop = springLift(disp.hopU);
    const float cy = std::cos(disp.yaw), sy = std::sin(disp.yaw);
    out[0] = kxz * cy;      out[1] = 0.f;  out[2] = -kxz * sy;    out[3] = 0.f;
    out[4] = t * ky * sy;   out[5] = ky;   out[6] = t * ky * cy;  out[7] = 0.f;
    out[8] = kxz * sy;      out[9] = 0.f;  out[10] = kxz * cy;    out[11] = 0.f;
    out[12] = disp.pos[0];
    out[13] = disp.pos[1] + hop;
    out[14] = disp.pos[2];
    out[15] = 1.f;
}

bool Renderer::focusActive(const LookParams& look) {
    // debugMode != 0 is an isolation render (normals, |grad| heatmap, flat
    // albedo). Those are per-texel diagnostics and a block-replicated
    // periphery under a defocus blur lies to every one of them.
    return look.focus.enabled && look.debugMode == 0.f;
}

int Renderer::coarseFactor(const LookParams& look) {
    if (!focusActive(look)) return 1;
    // 16 texels is already a caricature; the clamp is here so a fat-fingered
    // `set focus.coarse` cannot dispatch a one-thread grid over the frame.
    return std::clamp(look.focus.coarse, 1, 16);
}

int Renderer::packAffinePieces(float out[kUniformSlots][4], int idx,
                               const std::vector<AffinePiece>& pieces,
                               const std::vector<float>& mats) const {
    // +2 skips the block's meta and center slots; pieces follow them.
    const int base = fighterSlot(idx) + 2;
    if (pieces.size() > (size_t)BrickSystem::kPiecesPerFighter) {
        // A piece that does not fit would simply not be drawn — a limb quietly
        // missing from the body, with nothing in the log. Say so once.
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                         "[rig] asset wants %zu pieces, kPiecesPerFighter is %d — "
                         "the extras will NOT be drawn. Raise it in src/brick.h "
                         "(costs %d uniform slots per fighter each).\n",
                         pieces.size(), BrickSystem::kPiecesPerFighter, kPieceSlots);
        }
    }
    int n = 0;
    for (const AffinePiece& ap : pieces) {
        if (n >= BrickSystem::kPiecesPerFighter) break;
        float fwd[16];
        if (ap.srcBone >= 0 && (size_t)(ap.srcBone * 16 + 16) <= mats.size()) {
            std::memcpy(fwd, &mats[ap.srcBone * 16], sizeof(fwd));
        } else {
            // brush rig: the transform was written straight into the piece
            std::memcpy(fwd, ap.xform, sizeof(fwd));
        }
        // Only the INVERSE goes to the GPU. The forward matrix rode along for
        // the inverse-LBS round-trip check, which died with the armature.
        float inv[16];
        matInvAffine(fwd, inv);
        std::memcpy(out[base + n * kPieceSlots], inv, 16 * sizeof(float));
        float* lo = out[base + n * kPieceSlots + 4];
        float* hi = out[base + n * kPieceSlots + 5];
        for (int k = 0; k < 3; k++) {
            lo[k] = ap.lo[k];
            hi[k] = ap.hi[k];
        }
        // Lipschitz rescale — the SMALLEST SINGULAR VALUE, not a min column
        // norm. A shear can have unit-length columns and a much smaller
        // sigmaMin; trusting the column norm would overestimate world
        // distances and the march would tunnel. Floored at 0.25 so a
        // degenerate matrix cannot stall the trace to a crawl.
        lo[3] = std::min(std::max(mat3MinSingular(fwd), 0.25f), 1.f);
        n++;
    }
    return n;
}

float Renderer::affineBoundR(const std::vector<AffinePiece>& pieces,
                             const std::vector<float>& mats,
                             const float center[3]) const {
    float r = 0.f;
    for (const AffinePiece& ap : pieces) {
        float m[16];
        if (ap.srcBone >= 0 && (size_t)(ap.srcBone * 16 + 16) <= mats.size()) {
            std::memcpy(m, &mats[ap.srcBone * 16], sizeof(m));
        } else {
            std::memcpy(m, ap.xform, sizeof(m));
        }
        for (int c = 0; c < 8; c++) {
            const float q[3] = {(c & 1) ? ap.hi[0] : ap.lo[0],
                                (c & 2) ? ap.hi[1] : ap.lo[1],
                                (c & 4) ? ap.hi[2] : ap.lo[2]};
            float w[3];
            matTransformPoint(m, q, w);
            const float dx = w[0] - center[0], dy = w[1] - center[1],
                        dz = w[2] - center[2];
            r = std::max(r, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
    }
    return r;
}

void Renderer::packUniforms(const OrbitCamera& cam, const LookParams& look,
                            const FrameInfo& frame, float out[kUniformSlots][4]) const {
    Vec3 fwd, right, up;
    cam.basis(fwd, right, up);
    Vec3 pos = cam.pos();
    float aspect = (float)width_ / (float)height_;

    // ---- follow-spot: where the lamp is, and where it points ----
    // Off the hero's DISPLAY pose, not the sim pose: it is the position the
    // frame is drawn at, so the light cannot lead or lag the body it is lighting
    // by a frame. It is also what keeps this free for frame reuse — a standing
    // fighter's display root does not move, so neither does the light.
    float keyP[3] = {look.keyPos[0], look.keyPos[1], look.keyPos[2]};
    float keyAim[3] = {0.f, 0.55f, 0.f}; // the old hardcoded aim, as the default
    if (look.keyFollow) {
        const FighterPose& hero = fighters_[0].disp;
        keyAim[0] = hero.pos[0];
        keyAim[1] = hero.pos[1] + look.keyAimHeight;
        keyAim[2] = hero.pos[2];
        // The lamp slides with him in XZ only. Not in Y: the hopper bounces at
        // 2.3 Hz, and a lamp riding that would pulse the brightness of the
        // whole arena in time with his stride.
        const float t = std::min(std::max(look.keyLampTrack, 0.f), 1.f);
        keyP[0] += (hero.pos[0] - 0.f) * t;
        keyP[2] += (hero.pos[2] - 0.f) * t;
    }

    float packed[14][4] = {
        {pos.x, pos.y, pos.z, cam.fovY},
        {right.x, right.y, right.z, aspect},
        {up.x, up.y, up.z, frame.time},
        {fwd.x, fwd.y, fwd.z, frame.poseTime},
        {(float)width_, (float)height_, frame.grainFrame, (float)frame.aaSamples},
        {keyP[0], keyP[1], keyP[2], look.keyIntensity},
        {look.keyColor[0], look.keyColor[1], look.keyColor[2], look.keyFalloff},
        {look.rimDir[0], look.rimDir[1], look.rimDir[2], look.rimIntensity},
        {look.rimColor[0], look.rimColor[1], look.rimColor[2], 0.f},
        {look.ambient[0], look.ambient[1], look.ambient[2], look.aoStrength},
        {look.detailAmount, look.boilAmount, look.shadowSoft, look.sheenAmount},
        {look.grainAmount, look.vignetteInner, look.vignetteOuter, look.weaveAmount},
        {look.exposure, look.bloomAmount, look.bloomThreshold, look.debugMode},
        {pickU_, pickV_, 0.f, 0.f},
    };
    std::memset(out, 0, kUniformSlots * 16);
    std::memcpy(out, packed, sizeof(packed));
    // Beads for EVERY player, packed into the one array: 4 each (two eyeballs,
    // two pupils — the artist authors the LEFT eye's pair and the importer
    // mirrors both across x), so kMaxMarbles is 4 * kMaxPlayers by definition.
    // It was a flat 8, which fitted exactly two fighters and would have
    // silently dropped the third's eyes.
    //
    // Skeleton-free, a bead has no bone to ride, so it rides the BODY piece's
    // affine — which is what carries the squish, lean, yaw and hop. Radius is
    // deliberately NOT scaled by it: glass beads don't breathe with the torso.
    const bool brushRig = skeletonFree();
    int n = 0;
    // world centre + radius per packed bead, so the gaze pass below can pair
    // each pupil with its ball without re-deriving the transforms
    std::array<std::array<float, 4>, kMaxMarbles> eyeWorld{};
    auto packMarbles = [&](const Fighter& f) {
        for (const MarbleProp& m : marbles_) {
            if (n >= kMaxMarbles) break;
            float* slotA = out[kSlotMarbles + n * 2];
            float* slotB = out[kSlotMarbles + n * 2 + 1];
            float pos[3] = {m.pos[0], m.pos[1], m.pos[2]};
            if (m.bone >= 0 && (size_t)(m.bone * 16 + 16) <= f.skinMats.size()) {
                matTransformPoint(&f.skinMats[m.bone * 16], m.pos, pos);
            } else if (brushRig && !f.pieces.empty()) {
                if (const float* bx = bodyXformFor(f, m.pos)) {
                    matTransformPoint(bx, m.pos, pos);
                }
            }
            slotA[0] = pos[0]; slotA[1] = pos[1]; slotA[2] = pos[2];
            slotA[3] = m.radius;
            slotB[0] = m.color[0]; slotB[1] = m.color[1]; slotB[2] = m.color[2];
            eyeWorld[n] = {pos[0], pos[1], pos[2], m.radius};
            n++;
        }
    };
    for (int i = 0; i < kMaxPlayers; i++) {
        const Fighter& f = fighters_[i];
        if (f.dead) continue; // M-DEATH: the beads are loose_ now, packed below
        if (i > 0 && !(f.enabled && (!f.skinMats.empty() || brushRig))) continue;
        packMarbles(f);
    }
    // How many of those are still attached to a face. The gaze pass below
    // pairs a pupil with its eyeball BY PROXIMITY, so it must not see the loose
    // ones: a bead rolling on the floor has no eyeball to look out of, and
    // leaving it in the search would let a live pupil pick a corpse's eye as
    // its partner and stare through the floor.
    const int liveMarbles = n;

    // The loose eyes take the slots their corpse just gave up — four freed per
    // collapse, four spawned, so the array size is unchanged by construction
    // and trap 2's hand-mirrored layout never has to move.
    for (const LooseMarble& lm : loose_) {
        if (n >= kMaxMarbles) break;
        float* slotA = out[kSlotMarbles + n * 2];
        float* slotB = out[kSlotMarbles + n * 2 + 1];
        slotA[0] = lm.disp[0]; slotA[1] = lm.disp[1]; slotA[2] = lm.disp[2];
        slotA[3] = lm.radius;
        slotB[0] = lm.col[0]; slotB[1] = lm.col[1]; slotB[2] = lm.col[2];
        // Deliberately NOT written into eyeWorld: gaze pairs a pupil with its
        // eyeball by proximity, and a bead rolling on the floor has no eyeball
        // to look out of. Leaving them out keeps the gaze pass blind to them.
        n++;
    }
    // ---- gaze, without bones ----
    // The eye bones died with the armature, but a bead does not need one: an
    // eye is a big sphere (the ball) with a small one sitting on it (the
    // pupil), so gaze is just ROTATING the pupil about the ball's centre to
    // face the target. Pair them by proximity — the two beads of one eye are
    // nearly concentric — and the smaller of the pair is the pupil.
    //
    // Latched to the pose grid, like everything else that moves (trap 4):
    // sliding the pupils at 60 Hz would both break the stop-motion and make a
    // STANDING fighter a unique frame every frame, costing the idle reuse.
    if (look.gaze.track) {
        for (int i = 0; i < liveMarbles; i++) {
            int mate = -1;
            float best = 1e9f;
            for (int j = 0; j < liveMarbles; j++) {
                if (j == i) continue;
                float d2 = 0.f;
                for (int k = 0; k < 3; k++) {
                    const float t = eyeWorld[i][k] - eyeWorld[j][k];
                    d2 += t * t;
                }
                if (d2 < best) { best = d2; mate = j; }
            }
            // pupil = the smaller of a near-concentric pair
            if (mate < 0 || eyeWorld[i][3] >= eyeWorld[mate][3]) continue;
            if (best > eyeWorld[mate][3] * eyeWorld[mate][3]) continue;
            const float* ball = eyeWorld[mate].data();
            float off[3], len = 0.f;
            for (int k = 0; k < 3; k++) {
                off[k] = eyeWorld[i][k] - ball[k];
                len += off[k] * off[k];
            }
            len = std::sqrt(len);
            if (len < 1e-5f) continue;
            float dir[3], dl = 0.f;
            for (int k = 0; k < 3; k++) {
                dir[k] = gazeTarget_[k] - ball[k];
                dl += dir[k] * dir[k];
            }
            dl = std::sqrt(dl);
            if (dl < 1e-5f) continue;
            // clamp off the ball's forward so an eye never rolls fully round
            float* slotA = out[kSlotMarbles + i * 2];
            for (int k = 0; k < 3; k++) slotA[k] = ball[k] + dir[k] / dl * len;
        }
    }
    out[kSlotMarbleMeta][0] = (float)n;

    // Posed capsule shadow proxy, and the CENTRE of each fighter's trace reject
    // sphere — the same loop, because they are the same posed capsules.
    //
    // The proxy ITSELF is written for PLAYER 0 only, deliberately: capsules are
    // fitted per CHARACTER at import and `u.capsules` is not per-fighter, and
    // the opponent has always taken the plain loose field in mapPenumbra
    // instead. Generalising it would change the hero's penumbra, which is a
    // look change, not this refactor's business.
    //
    // The CENTRE is computed for every fighter, and THE DERIVATION IS
    // LOAD-BEARING, not an implementation detail. charLooseAffine RETURNS
    // `length(p - gFarCenter) - gFarR` for points outside the sphere, and that
    // value feeds AO and the penumbra term — so the reject sphere is not
    // merely an early-out, and moving it moves the shading on every surface it
    // is far from, including surfaces the fighter is nowhere near in frame.
    //
    // Measured, at 640x360 aa2 against origin/main:
    //   hero on this capsule-average centre     4 px differ, max delta 1
    //   hero on the transformed REST centre  38295 px differ, max delta 59
    // The second is how the OPPONENT's centre used to be derived, and it is
    // arguably the tighter sphere (the capsule average is dragged sideways by
    // the two mitts). Unifying on it anyway would have repainted the hero's
    // AO, so this keeps the hero's derivation and moves the opponent onto it.
    // The opponent's own shift is the entire residual against origin/main
    // (~4% of pixels, max 17, almost all +-1 LSB on ground near it) and shows
    // up as AO, not silhouette. Reproducing BOTH exactly would mean keeping a
    // per-player special case, which is the hero/foe asymmetry this refactor
    // exists to delete.
    //
    // Under the brush rig `bone` carries the PIECE index instead of a bone
    // index (see setCharacter), so each capsule rides the clay it stands in for.
    int cn = std::min((int)capsules_.size(), kMaxCapsules);
    std::array<std::array<float, 3>, kMaxPlayers> centers{};
    for (int pi = 0; pi < kMaxPlayers; pi++) {
      const Fighter& pf = fighters_[pi];
      float* center = centers[pi].data();
      const bool writeProxy = (pi == 0);
      for (int i = 0; i < cn; i++) {
        const BoneCapsule& c = capsules_[i];
        float a[3] = {c.a[0], c.a[1], c.a[2]};
        float b[3] = {c.b[0], c.b[1], c.b[2]};
        if (brushRig) {
            // The capsule must be expressed in the SAME brush space its piece
            // is currently sampling, or the piece's transform (which undoes
            // that brush's rest-space offset) throws it ~1 m across the arena
            // the moment a hand switches to the grab brush. The piece's own
            // lo/hi already IS the selected brush's box, so refit from that
            // every frame rather than caching a box from one pose.
            if (c.bone >= 0 && (size_t)c.bone < pf.pieces.size()) {
                const AffinePiece& ap = pf.pieces[c.bone];
                float ca[3], cb[3], cr;
                capsuleFromBox(ap.lo, ap.hi, ca, cb, cr);
                matTransformPoint(ap.xform, ca, a);
                matTransformPoint(ap.xform, cb, b);
                if (writeProxy) {
                    float* sA = out[kSlotCapsules + i * 2];
                    float* sB = out[kSlotCapsules + i * 2 + 1];
                    sA[0] = a[0]; sA[1] = a[1]; sA[2] = a[2]; sA[3] = cr;
                    sB[0] = b[0]; sB[1] = b[1]; sB[2] = b[2];
                }
                for (int k = 0; k < 3; k++) center[k] += (a[k] + b[k]) * 0.5f;
                continue;
            }
        } else if (c.bone >= 0 &&
                   (size_t)(c.bone * 16 + 16) <= pf.skinMats.size()) {
            matTransformPoint(&pf.skinMats[c.bone * 16], c.a, a);
            matTransformPoint(&pf.skinMats[c.bone * 16], c.b, b);
        }
        if (writeProxy) {
            float* slotA = out[kSlotCapsules + i * 2];
            float* slotB = out[kSlotCapsules + i * 2 + 1];
            slotA[0] = a[0]; slotA[1] = a[1]; slotA[2] = a[2]; slotA[3] = c.r;
            slotB[0] = b[0]; slotB[1] = b[1]; slotB[2] = b[2];
        }
        for (int k = 0; k < 3; k++) center[k] += (a[k] + b[k]) * 0.5f;
      }
      if (cn > 0) {
          for (int k = 0; k < 3; k++) center[k] /= (float)cn;
      } else {
          // No capsules fitted (analytic blob): fall back to the character's
          // rest centre, which is what the bound was measured about.
          for (int k = 0; k < 3; k++) center[k] = bodyCenterRest_[k];
      }
    }
    const float* center = centers[0].data();
    if (cn > 0) {
        float radius = 0.f;
        for (int i = 0; i < cn; i++) {
            for (int e = 0; e < 2; e++) {
                const float* p = out[kSlotCapsules + i * 2 + e];
                float dx = p[0] - center[0], dy = p[1] - center[1], dz = p[2] - center[2];
                radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz) +
                                              out[kSlotCapsules + i * 2][3]);
            }
        }
        out[kSlotCapsMeta][1] = radius + 0.05f;
        out[kSlotCapsCenter][0] = center[0];
        out[kSlotCapsCenter][1] = center[1];
        out[kSlotCapsCenter][2] = center[2];
    }
    out[kSlotCapsMeta][0] = (float)cn;

    // ---- ONE BLOCK PER FIGHTER ----
    // This replaced three packings: the hero's pieces at slot 66, the
    // opponent's near-identical copy at 293, and a dead third — the M4-P1
    // per-CAPSULE pieces, one Piece per bone for a rigged asset. That last one
    // went because the thing it fed is gone: brick_read.wgsl deleted the
    // inverse-LBS warp along with the armature, so charDistI has exactly two
    // modes now — "no pieces" (rest volume drawn unposed) and the affine brush
    // rig. Packing 16 bone chunks into a shader that only knows how to
    // box-clip three disjoint brushes could not have drawn anything correct;
    // BrickSystem::kPiecesPerFighter = 3 makes that explicit rather than
    // leaving 16 slots of vestigial capacity per fighter.
    static const bool noPieces = std::getenv("CLAYFRAY_NO_PIECES") != nullptr;
    const bool affine = affineOn(look);
    // Highest live fighter + 1: the shader loops 0..sceneMeta.x-1 and skips
    // any whose meta.x is 0, so a gap in the middle costs one rejected test,
    // not a wrong body.
    int live = 1; // the hero always draws
    for (int i = 0; i < kMaxPlayers; i++) {
        const Fighter& f = fighters_[i];
        if (i > 0 && !f.enabled) continue;
        // M-DEATH: leave the block zeroed. base[0] is the shader's `enabled`
        // flag and the memset above already cleared it, so a collapsed fighter
        // costs the tracer one rejected test and draws nothing — no clay, no
        // reject sphere, no shadow proxy.
        if (f.dead) continue;
        live = i + 1;
        const int base = fighterSlot(i);
        // Skeleton-free with the rig switched off: pn = 0 puts the shader on
        // its un-rigged path (the rest volume drawn where it was authored, all
        // three brushes side by side). That is the debug A/B described in
        // affineOn — NOT a fallback anything ships with.
        const int pn =
            (affine && !noPieces) ? packAffinePieces(out, i, f.pieces, f.skinMats) : 0;

        // The reject sphere the tracer tests before touching the volume. Both
        // centre and radius are re-derived EVERY FRAME rather than measured
        // once at rest, because a squish can push clay past any rest-mesh
        // bound — and a sphere that is too SMALL clips the fighter out of the
        // frame entirely, which is a far louder failure than one too large.
        const float* c = centers[i].data();
        float r = bodyBoundRest_;
        if (pn > 0 && !f.pieces.empty()) r = affineBoundR(f.pieces, f.skinMats, c);
        out[base][0] = 1.f;       // enabled
        out[base][1] = (float)pn; // piece count (0 = draw the rest volume)
        out[base][2] = r + 0.05f;
        out[base + 1][0] = c[0];
        out[base + 1][1] = c[1];
        out[base + 1][2] = c[2];
    }

    // B5 foveation / tilt-shift: where the sharp region sits, in TRACED texels
    // (so nothing here has to know about resScale). Inactive leaves both slots
    // at the memset zero, which means `focus.enabled 0` is bit-for-bit the
    // pre-foveation renderer rather than an approximation of it, and the reuse
    // digest sees a constant.
    //
    // NOT quantised to the pose grid, and that is deliberate against trap 4:
    // the centre is a pure function of the camera and the fighters' DISPLAY
    // roots, and all three are already traced inputs in the digest — so it can
    // only move on a frame that was re-tracing anyway. Quantising would buy no
    // reuse and would make the defocus boundary lag a subject that slides at
    // 60 Hz.
    if (focusActive(look)) {
        const FocusParams& fp = look.focus;
        const float th = std::tan(cam.fovY * 0.5f);
        // world -> traced-texel coordinates, on the same basis tracePixel
        // builds its rays from. A point behind the eye reports a miss and
        // leaves the caller on the frame centre.
        auto project = [&](const float w[3], float& sx, float& sy) {
            const float v[3] = {w[0] - pos.x, w[1] - pos.y, w[2] - pos.z};
            const float z = v[0] * fwd.x + v[1] * fwd.y + v[2] * fwd.z;
            if (!(z > 1e-3f)) return false;
            const float xr = (v[0] * right.x + v[1] * right.y + v[2] * right.z) /
                             (z * th * aspect);
            const float yu = (v[0] * up.x + v[1] * up.y + v[2] * up.z) / (z * th);
            sx = (xr * 0.5f + 0.5f) * (float)width_;
            sy = (0.5f - yu * 0.5f) * (float)height_;
            return true;
        };
        float cx = (float)width_ * 0.5f, cy = (float)height_ * 0.5f;
        float spanX = 0.f, spanY = 0.f;
        // Aim at the HERO first — it is the one the player is watching, and it
        // is the one guaranteed to exist.
        const FighterPose& hero = fighters_[0].disp;
        const float aimH[3] = {hero.pos[0], hero.pos[1] + fp.height, hero.pos[2]};
        float hx = 0.f, hy = 0.f;
        if (project(aimH, hx, hy)) {
            cx = hx;
            cy = hy;
            // Both fighters sharp: centre between them and grow the core by
            // half their separation, per axis. Duelling across the arena
            // therefore trades the saving back for the framing, which is the
            // right way round — that is exactly when both bodies matter.
            if (fp.pair) {
                for (int i = 1; i < kMaxPlayers; i++) {
                    if (!playerAlive(i)) continue;
                    const FighterPose& o = fighters_[i].disp;
                    const float aimF[3] = {o.pos[0], o.pos[1] + fp.height, o.pos[2]};
                    float ox = 0.f, oy = 0.f;
                    if (!project(aimF, ox, oy)) continue;
                    cx = (hx + ox) * 0.5f;
                    cy = (hy + oy) * 0.5f;
                    spanX = std::max(spanX, std::fabs(hx - ox) * 0.5f);
                    spanY = std::max(spanY, std::fabs(hy - oy) * 0.5f);
                }
            }
        }
        // A fighter far off frame must not push the ellipse out to absurd
        // coordinates; one frame of slack is plenty for the feather to reach in
        cx = std::clamp(cx, -(float)width_, 2.f * (float)width_);
        cy = std::clamp(cy, -(float)height_, 2.f * (float)height_);
        // Radii are fractions of frame HEIGHT so the framing survives a resize
        // or a resScale change. The ellipse is carried as (vertical radius,
        // x/y aspect) because that is the metric both shaders compare against.
        const float ry = std::max(fp.radius * (float)height_ + spanY, 1.f);
        const float rx = std::max(fp.radius * fp.aspect * (float)height_ + spanX, 1.f);
        out[kSlotFocus][0] = cx;
        out[kSlotFocus][1] = cy;
        out[kSlotFocus][2] = ry;
        out[kSlotFocus][3] = rx / ry;
        out[kSlotFocusMeta][0] = std::max(fp.feather * (float)height_, 0.f);
        out[kSlotFocusMeta][1] = (float)coarseFactor(look);
        out[kSlotFocusMeta][2] = std::max(fp.blur, 0.f); // 0 = no post defocus
    }

    out[kSlotKeyAim][0] = keyAim[0];
    out[kSlotKeyAim][1] = keyAim[1];
    out[kSlotKeyAim][2] = keyAim[2];
    out[kSlotSceneMeta][0] = (float)live;
    // joint smin k: with disjoint brush regions the blend only bridges
    // hairline handoff cracks; ~2 voxels seals them without re-introducing
    // ring bulges. VOXEL-relative, not metres — as a fixed 0.008 m it shrank
    // to 1.5 voxels at kGrid=37 and the cracks reopened at the shoulders (see
    // kJointSminVoxels in brick.h).
    out[kSlotSceneMeta][1] = BrickSystem::kJointSminVoxels * BrickSystem::kVoxel;
    // box test margin: sample within, bound outside. Span-relative for the
    // same reason.
    out[kSlotSceneMeta][2] = BrickSystem::kBoxMarginSpans * BrickSystem::kSpan;

    // M4.6 conservation: in-flight gobs (12 Hz-stepped positions) + the
    // ground field's clay top bound (0 disables the field in the tracer).
    // These slots are hand-mirrored in trace.wgsl AND pick.wgsl (trap 2);
    // this static_assert catches only the C++ side overrunning the buffer.
    static_assert(kSlotKeyAim + 1 == kUniformSlots &&
                      kSlotFocus == kSlotFighters + kFighterSlots * kMaxPlayers,
                  "the fighter blocks must end exactly where the focus pair "
                  "begins, and keyAim must end exactly at kUniformSlots; keep "
                  "the Uniforms struct in trace.wgsl + pick.wgsl + post.wgsl "
                  "(whose pad is FOCUS_PAD, generated from kSlotFocus) in sync");
    int gn = std::min((int)gobs_.size(), kMaxGobs);
    for (int i = 0; i < gn; i++) {
        const Gob& g = gobs_[i];
        float* slotA = out[kSlotGobs + i * 2];
        float* slotB = out[kSlotGobs + i * 2 + 1];
        slotA[0] = g.disp[0]; slotA[1] = g.disp[1]; slotA[2] = g.disp[2];
        slotA[3] = g.radius;
        slotB[0] = g.col[0]; slotB[1] = g.col[1]; slotB[2] = g.col[2];
    }
    out[kSlotGobMeta][0] = (float)gn;
    out[kSlotGroundMeta][0] = GroundClay::kOrigin;
    out[kSlotGroundMeta][1] = GroundClay::kTexel;
    out[kSlotGroundMeta][2] = (float)GroundClay::kN;
    out[kSlotGroundMeta][3] = ground_.maxTopY();

    // M4.7 sword: emissive blade endpoints (radius 0 = inactive). memset above
    // already zeroed the slots, so the disabled case needs no write.
    if (swordWorld_.enabled && (!bones_.empty() || brushRig)) {
        float hilt[3], tip[3], gA[3], gB[3];
        swordGeometry(swordWorld_, hilt, tip, gA, gB);
        float* a = out[kSlotSwordA];
        float* b = out[kSlotSwordA + 1];
        float* col = out[kSlotSwordA + 2];
        a[0] = hilt[0]; a[1] = hilt[1]; a[2] = hilt[2];
        a[3] = swordWorld_.radius;
        b[0] = tip[0]; b[1] = tip[1]; b[2] = tip[2];
        col[0] = swordWorld_.color[0];
        col[1] = swordWorld_.color[1];
        col[2] = swordWorld_.color[2];
    }
}

int Renderer::clipIndex(const char* name) const {
    for (size_t i = 0; i < clips_.size(); i++)
        if (clips_[i].name == name) return (int)i;
    return -1;
}

// Root transform: yaw about +Y, then a lean about the axis perpendicular to
// travel (so the fighter tips INTO its run), then translation. Built here and
// premultiplied onto every skin matrix, which carries marbles, capsules, the
// COM and the IK'd hands along without any of them knowing about it.
void Renderer::resolveSword(const LookParams& look) {
    swordWorld_ = look.sword;
    if (!look.sword.carry) return;
    float cy = std::cos(fighters_[0].disp.yaw), sy = std::sin(fighters_[0].disp.yaw);
    const float* p = look.sword.pos;
    // lean tips the held sword with the body: rotate about the travel-perp
    // axis, which in character space is +X (forward is +Z)
    float cl = std::cos(fighters_[0].disp.lean), sl = std::sin(fighters_[0].disp.lean);
    float ly = p[1] * cl - p[2] * sl;
    float lz = p[1] * sl + p[2] * cl;
    swordWorld_.pos[0] = fighters_[0].disp.pos[0] + cy * p[0] + sy * lz;
    swordWorld_.pos[1] = fighters_[0].disp.pos[1] + ly;
    swordWorld_.pos[2] = fighters_[0].disp.pos[2] - sy * p[0] + cy * lz;
    swordWorld_.yaw = look.sword.yaw + fighters_[0].disp.yaw;
    swordWorld_.pitch = look.sword.pitch + fighters_[0].disp.lean;
}

void Renderer::swordGeometry(const SwordParams& s, float hilt[3], float tip[3],
                             float gripA[3], float gripB[3]) const {
    // blade direction: +Z forward, pitched up toward +Y, then yawed about Y
    float cp = std::cos(s.pitch), sp = std::sin(s.pitch);
    float cy = std::cos(s.yaw), sy = std::sin(s.yaw);
    float dir[3] = {cp * sy, sp, cp * cy};
    for (int i = 0; i < 3; i++) {
        hilt[i] = s.pos[i];
        tip[i] = s.pos[i] + dir[i] * s.length;
        gripA[i] = s.pos[i] + dir[i] * s.grip0; // near hilt (one hand)
        gripB[i] = s.pos[i] + dir[i] * s.grip1; // stacked (other hand)
    }
}

void Renderer::applyBodyColors(const LookParams& look) {
    // LookParams::bodyColor is sized 4 and does not include renderer.h, so this
    // is the link between the two. Raising the fighter cap past 4 without
    // widening that array would silently reuse the last colour.
    static_assert(kMaxPlayers <= 4, "LookParams::bodyColor has 4 rows");
    for (int i = 0; i < kMaxPlayers; i++) {
        fighters_[i].vol.setBodyColor(look.bodyColor[i]);
    }
    // Until something has actually been cut, the "last wound" colours have
    // never been written — and their initialiser is a cyan that predates
    // fighters having colours at all. Seed them off the hero so no path can
    // emit it, rather than trusting that none reads them early.
    if (!haveWound_) {
        std::memcpy(woundCol_, look.bodyColor[0], sizeof(woundCol_));
        std::memcpy(sliceCol_, look.bodyColor[0], sizeof(sliceCol_));
    }
}

void Renderer::setCharacter(CharacterAsset asset) {
    // The .glb is authoritative once loaded — its props replace the hand-coded
    // default eyes EVEN WHEN IT HAS NONE. This rig models the eyes as skinned
    // geometry with their own gaze bones under the head, so keeping the
    // defaults hovered a second, stale pair (authored for the old 1.2 m
    // character) well above a 0.77 m blob.
    marbles_ = asset.marbles;
    bones_ = asset.bones;
    clips_ = asset.clips;
    capsules_ = deriveCapsules(asset);
    evalPose(bones_, nullptr, 0.f, fighters_[0].skinMats); // identity: rest pose

    // ---- M-RIG: the skeleton-free brush rig ----
    // Built entirely from the imported MeshParts, so it needs no armature. Done
    // FIRST because it decides whether any of the bone machinery below runs at
    // all (on this asset none of it does: every loop is over an empty bones_).
    brush_ = BrushRig{};
    for (Fighter& f : fighters_) f.pieces.clear();
    if (asset.hasBrushRig()) {
        const MeshPart& body = asset.parts[asset.partBody];
        // Pad each brush box outward. The pad only has to CONTAIN the brush's
        // clay (a box that contains it makes max(field, boxDist) exact); it
        // must not reach a neighbour, and with ~0.12 m of clearance a pad this
        // size is nowhere near doing that.
        const float pad = 0.012f;
        auto setBox = [&](const MeshPart& p, float lo[3], float hi[3]) {
            for (int k = 0; k < 3; k++) {
                lo[k] = p.lo[k] - pad;
                hi[k] = p.hi[k] + pad;
            }
        };
        setBox(body, brush_.bodyLo, brush_.bodyHi);
        // The reach ball's origin. Under the bone rig this was the blob's
        // centre of mass, decomposed off the skin; with no skin the body
        // brush's own box centre is the same quantity measured off the only
        // thing left that knows where the body is.
        for (int k = 0; k < 3; k++) {
            brush_.bodyCenter[k] = (brush_.bodyLo[k] + brush_.bodyHi[k]) * 0.5f;
        }
        for (int b = 0; b < kHandBrushCount; b++) {
            setBox(asset.parts[asset.handPart(b)], brush_.handLo[b], brush_.handHi[b]);
            // A pose the asset did not ship falls back to the REST brush, and
            // therefore has to carry the rest brush's offset — the offset says
            // where in the volume this pose's clay actually lives, so taking it
            // from the table while sampling rest's region would place the mitt
            // half a metre away from its own geometry.
            const bool own = asset.partHand[b] >= 0;
            const float* ofs = kHandBrushOffset[own ? b : kHandRest];
            for (int k = 0; k < 3; k++) brush_.ofs[b][k] = ofs[k];
            for (int k = 0; k < 3; k++) {
                brush_.canonLo[b][k] = brush_.handLo[b][k] - brush_.ofs[b][k];
                brush_.canonHi[b][k] = brush_.handHi[b][k] - brush_.ofs[b][k];
            }
            // The grip sits INSIDE the mitt: palmFrac of the way along the
            // finger axis from the wrist end. The hand is authored pointing
            // +x away from the body, so +x is the finger axis and the wrist
            // end is the box's min x. (The old rig measured this off the
            // wrist->fingertip bones; the box is the same measurement without
            // an armature to take it from.)
            brush_.palm[b][0] =
                brush_.canonLo[b][0] +
                (brush_.canonHi[b][0] - brush_.canonLo[b][0]) * 0.6f;
            brush_.palm[b][1] = (brush_.canonLo[b][1] + brush_.canonHi[b][1]) * 0.5f;
            brush_.palm[b][2] = (brush_.canonLo[b][2] + brush_.canonHi[b][2]) * 0.5f;
        }
        brush_.valid = true;

        // Three pieces: body, left hand, right hand. srcBone stays -1 — these
        // are driven by updateBrushRig writing `xform` directly.
        // Every fighter gets its own copy: same three brushes, but each
        // fighter's own pose writes its own `xform` per frame.
        for (Fighter& f : fighters_) {
            f.pieces.resize(BrickSystem::kBrushPieces);
            for (AffinePiece& ap : f.pieces) ap.srcBone = -1;
        }

        // Shadow proxy. deriveCapsules() fits capsules to BONES and there are
        // none, so synthesise one per brush from its own AABB: the capsule runs
        // along the box's longest axis with a radius set by the smaller of the
        // other two half-extents, which is a decent stand-in for a rounded
        // blob or mitt. `bone` carries the PIECE index here (packUniforms
        // knows), so each capsule rides the clay it stands in for.
        capsules_.clear();
        auto addCapsule = [&](const float lo[3], const float hi[3], int piece) {
            BoneCapsule bc{};
            capsuleFromBox(lo, hi, bc.a, bc.b, bc.r);
            bc.bone = piece; // PIECE index, not a bone id
            bc.rPiece = bc.r;
            for (int k = 0; k < 3; k++) { bc.lo[k] = lo[k]; bc.hi[k] = hi[k]; }
            capsules_.push_back(bc);
        };
        // Rest-pose defaults. packUniforms and posedCapsule both REFIT the
        // a/b/r of each of these from its piece's CURRENT brush box, so these
        // values are only the un-posed seed.
        addCapsule(brush_.bodyLo, brush_.bodyHi, 0);
        addCapsule(brush_.handLo[kHandRest], brush_.handHi[kHandRest], 1);
        addCapsule(brush_.handLo[kHandRest], brush_.handHi[kHandRest], 2);

        // How far a mitt may float from the body. The bone rig measured this
        // as the rest COM->wrist distance; the boxes give the same quantity
        // without an armature, and hands.reachScale still scales it.
        autoReach_ = 0.f;
        for (int k = 0; k < 3; k++) {
            const float d = brush_.palm[kHandRest][k] - brush_.bodyCenter[k];
            autoReach_ += d * d;
        }
        autoReach_ = std::sqrt(autoReach_);

        // M-SPRING's lever: the hopper works in metres of compression and
        // bodyAffine scales by a fraction, so the body's own height is what
        // converts one to the other. Off the MESH part, not off `brush_.body*`
        // — those are the padded clip box (0.715 m against the clay's 0.691),
        // and the padding is a shader-clip artifact with nothing to say about
        // how tall the fighter is. Guarded because a degenerate box would
        // divide a 4 cm sag by nothing and fold the fighter flat.
        const MeshPart& bodyPart = asset.parts[asset.partBody];
        const float bh = bodyPart.hi[1] - bodyPart.lo[1];
        if (bh > 1e-3f) bodyHeight_ = bh;

        // M-DEATH's denominator: the clay one fighter IS, as drawn — the body
        // plus TWO mitts (the rig mirrors one hand into two pieces, so the
        // brush is paid for twice). The other hand poses are alternates of the
        // same mitt, not extra limbs, so counting them would inflate the
        // denominator and make a fighter take ~35% more damage before dropping.
        fighterVolume_ = asset.parts[asset.partBody].volume +
                         2.f * asset.parts[asset.handPart(kHandRest)].volume;

        std::printf("rig: brush rig — body (%.3f %.3f %.3f)..(%.3f %.3f %.3f), "
                    "hands rest/grab/idle/fist x[%.3f %.3f]/[%.3f %.3f]/"
                    "[%.3f %.3f]/[%.3f %.3f], reach %.3f m, "
                    "stand %.3f m, %zu marbles\n",
                    brush_.bodyLo[0], brush_.bodyLo[1], brush_.bodyLo[2],
                    brush_.bodyHi[0], brush_.bodyHi[1], brush_.bodyHi[2],
                    brush_.handLo[kHandRest][0], brush_.handHi[kHandRest][0],
                    brush_.handLo[kHandGrab][0], brush_.handHi[kHandGrab][0],
                    brush_.handLo[kHandIdle][0], brush_.handHi[kHandIdle][0],
                    brush_.handLo[kHandFist][0], brush_.handHi[kHandFist][0],
                    autoReach_, bodyHeight_, marbles_.size());
        // Fail LOUDLY rather than rendering a silently clipped fighter: a brush
        // that pokes outside the volume box, or two brushes whose padded boxes
        // touch, both break the clip's exactness (CLAUDE.md trap 5 drops edits
        // near the boundary for the same reason).
        const float vlo[3] = {BrickSystem::kOrigin[0], BrickSystem::kOrigin[1],
                              BrickSystem::kOrigin[2]};
        const float vhi[3] = {vlo[0] + BrickSystem::kExtent,
                              vlo[1] + BrickSystem::kExtent,
                              vlo[2] + BrickSystem::kExtent};
        // One entry per DISTINCT region. A hand pose the asset did not ship
        // shares the rest brush's box, and comparing a box against itself would
        // report a guaranteed overlap — a warning that is always wrong is worse
        // than no warning, because it trains you to ignore the real one.
        struct NamedBox {
            const char* name;
            const float* lo;
            const float* hi;
        };
        std::vector<NamedBox> boxes{{"body", brush_.bodyLo, brush_.bodyHi}};
        static const char* kBrushNames[kHandBrushCount] = {
            "hand.rest", "hand.grab", "hand.idle", "hand.fist"};
        for (int b = 0; b < kHandBrushCount; b++) {
            if (b != kHandRest && asset.partHand[b] < 0) continue; // fell back
            boxes.push_back({kBrushNames[b], brush_.handLo[b], brush_.handHi[b]});
        }
        for (size_t i = 0; i < boxes.size(); i++) {
            for (int k = 0; k < 3; k++) {
                if (boxes[i].lo[k] < vlo[k] || boxes[i].hi[k] > vhi[k]) {
                    std::fprintf(stderr,
                                 "[rig] WARNING brush '%s' axis %d leaves the volume "
                                 "box (%.4f..%.4f vs %.4f..%.4f) — it will render "
                                 "clipped\n",
                                 boxes[i].name, k, boxes[i].lo[k], boxes[i].hi[k],
                                 vlo[k], vhi[k]);
                }
            }
            for (size_t j = i + 1; j < boxes.size(); j++) {
                bool overlap = true;
                for (int k = 0; k < 3; k++)
                    if (boxes[i].hi[k] < boxes[j].lo[k] || boxes[j].hi[k] < boxes[i].lo[k])
                        overlap = false;
                if (overlap) {
                    std::fprintf(stderr,
                                 "[rig] WARNING brushes '%s' and '%s' overlap — the "
                                 "AABB clip is no longer an exact separation\n",
                                 boxes[i].name, boxes[j].name);
                }
            }
        }
    }

    // M4.7: build the floating-hand IK chains from bone names. Rig convention
    // (see the .glb): a base.* blob spine plus two DETACHED mitts,
    // hand.<side> -> {thumb.<side> -> thumbtip.<side>, finger.<side> ->
    // fingertip.<side>}. No arm bones exist, so the whole mitt subtree moves
    // rigidly to the grip and the body only limits how far it can go.
    handIk_.clear();
    const size_t nb = bones_.size();
    std::vector<std::vector<int>> kids(nb);
    for (size_t i = 0; i < nb; i++)
        if (bones_[i].parent >= 0 && (size_t)bones_[i].parent < nb)
            kids[bones_[i].parent].push_back((int)i);
    auto findBone = [&](const std::string& nm) {
        for (size_t i = 0; i < nb; i++)
            if (bones_[i].name == nm) return (int)i;
        return -1;
    };
    auto subtree = [&](int root, std::vector<int>& outv) {
        std::vector<int> st{root};
        while (!st.empty()) {
            int b = st.back();
            st.pop_back();
            outv.push_back(b);
            for (int c : kids[b]) st.push_back(c);
        }
    };
    auto restOrigin = [&](int b, float* o) {
        float inv[16];
        matInvAffine(bones_[b].invBind, inv);
        o[0] = inv[12]; o[1] = inv[13]; o[2] = inv[14];
    };
    std::vector<int> handBones;
    for (const char* side : {"l", "r"}) {
        int wr = findBone(std::string("hand.") + side);
        if (wr < 0) continue;
        HandIkChain c;
        c.wrist = wr;
        subtree(wr, c.subtree);
        handBones.insert(handBones.end(), c.subtree.begin(), c.subtree.end());
        // each direct child of the wrist is one digit (thumb.*, finger.*),
        // kept separately so finger curl can move them without the palm
        for (int kid : kids[wr]) {
            std::vector<int> dig;
            subtree(kid, dig);
            c.digits.push_back(std::move(dig));
        }
        // finger axis: wrist -> fingertip in the WRIST'S OWN space, so the
        // solver can carry it through any pose to find where the mitt points.
        int ft = findBone(std::string("fingertip.") + side);
        if (ft >= 0) {
            float w[3], f[3];
            restOrigin(wr, w);
            restOrigin(ft, f);
            float d[3] = {f[0] - w[0], f[1] - w[1], f[2] - w[2]};
            c.restSpan = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (c.restSpan > 1e-5f) {
                // rotate the rest-space aim into wrist-local space
                const float* ib = bones_[wr].invBind;
                float l[3] = {ib[0] * d[0] + ib[4] * d[1] + ib[8] * d[2],
                              ib[1] * d[0] + ib[5] * d[1] + ib[9] * d[2],
                              ib[2] * d[0] + ib[6] * d[1] + ib[10] * d[2]};
                float ll = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
                if (ll > 1e-6f) {
                    c.restAim[0] = l[0] / ll;
                    c.restAim[1] = l[1] / ll;
                    c.restAim[2] = l[2] / ll;
                }
            }
        }
        // Which wrist-local axis lies along the handle: the one the mitt is
        // THINNEST along, measured off its own vertices. Stacking two hands on
        // a handle only works if each spends its narrow dimension there.
        float seed[3] = {1.f, 0.f, 0.f};
        if (std::fabs(c.restAim[0]) > 0.9f) { seed[0] = 0.f; seed[2] = 1.f; }
        float e1[3] = {c.restAim[1] * seed[2] - c.restAim[2] * seed[1],
                       c.restAim[2] * seed[0] - c.restAim[0] * seed[2],
                       c.restAim[0] * seed[1] - c.restAim[1] * seed[0]};
        float el = std::sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
        if (el > 1e-6f) {
            e1[0] /= el; e1[1] /= el; e1[2] /= el;
            float e2[3] = {c.restAim[1] * e1[2] - c.restAim[2] * e1[1],
                           c.restAim[2] * e1[0] - c.restAim[0] * e1[2],
                           c.restAim[0] * e1[1] - c.restAim[1] * e1[0]};
            std::vector<uint8_t> inHand(nb, 0);
            for (int b : c.subtree)
                if (b >= 0 && (size_t)b < nb) inHand[b] = 1;
            float lo1 = 1e9f, hi1 = -1e9f, lo2 = 1e9f, hi2 = -1e9f;
            const uint32_t nv = asset.vertexCount();
            for (uint32_t v = 0; v < nv && asset.weights.size() >= (size_t)nv * 4; v++) {
                int dom = -1;
                float bw = 0.f;
                for (int s = 0; s < 4; s++) {
                    float w = asset.weights[v * 4 + s];
                    if (w > bw) { bw = w; dom = asset.joints[v * 4 + s]; }
                }
                if (dom < 0 || (size_t)dom >= nb || !inHand[dom]) continue;
                float local[3];
                matTransformPoint(bones_[wr].invBind, &asset.positions[v * 3], local);
                float p1 = local[0] * e1[0] + local[1] * e1[1] + local[2] * e1[2];
                float p2 = local[0] * e2[0] + local[1] * e2[1] + local[2] * e2[2];
                lo1 = std::min(lo1, p1); hi1 = std::max(hi1, p1);
                lo2 = std::min(lo2, p2); hi2 = std::max(hi2, p2);
            }
            const float* thin = (hi1 - lo1) <= (hi2 - lo2) ? e1 : e2;
            if (hi1 >= lo1) std::memcpy(c.restEdge, thin, sizeof(c.restEdge));
        }
        handIk_.push_back(c);
    }

    // gaze: the eye bones are leaves under the head, aimed along local +Y
    gaze_.clear();
    for (size_t i = 0; i < nb; i++) {
        if (bones_[i].name.compare(0, 4, "eye.") != 0) continue;
        GazeChain g;
        g.bone = (int)i;
        gaze_.push_back(g);
    }

    // the tether anchor: the blob's centre of mass, hands excluded. autoReach_
    // is how far the wrists sit from it at rest — the natural "arm length" of
    // a fighter that has no arms.
    bodyCom_ = deriveBodyCom(asset, handBones);
    // Only a RIGGED asset re-derives it. The brush rig measured its own reach
    // off the boxes above and there is no COM to decompose without a skin, so
    // an unconditional reset here would zero it and let the mitts float free.
    if (!bodyCom_.bone.empty()) {
        autoReach_ = 0.f;
        float com[3];
        evalBodyCom(bodyCom_, fighters_[0].skinMats, com); // fighters_[0].skinMats = rest pose here
        for (const HandIkChain& c : handIk_) {
            float w[3];
            restOrigin(c.wrist, w);
            float dx = w[0] - com[0], dy = w[1] - com[1], dz = w[2] - com[2];
            autoReach_ = std::max(autoReach_,
                                  std::sqrt(dx * dx + dy * dy + dz * dz));
        }
    }
    // ---- M-PERF: the BONE-partitioned affine rig (legacy assets only) ----
    // Partition a rigged asset into an affine body plus one rigid piece per
    // mitt, by BONE. Skipped entirely when the brush rig already built the
    // pieces above — which is the case for the current fighter, and the guard
    // matters: this used to start with an unconditional clear() that would
    // silently throw the brush pieces away.
    if (!bones_.empty()) {
        fighters_[0].pieces.clear();
        std::vector<int> owner(nb, 0); // default: the body
        std::vector<int> srcBone{-1};  // piece 0 is the body; filled below
        for (const HandIkChain& c : handIk_) {
            const int pi = (int)srcBone.size();
            for (int b : c.subtree)
                if (b >= 0 && (size_t)b < nb) owner[b] = pi;
            srcBone.push_back(c.wrist);
        }
        // The body's transform is carried by any bone it owns — once posed
        // they all hold the same affine — so take the first.
        for (size_t b = 0; b < nb; b++) {
            if (owner[b] == 0) {
                srcBone[0] = (int)b;
                break;
            }
        }
        std::vector<AffinePiece> pieces(srcBone.size());
        for (size_t i = 0; i < srcBone.size(); i++) {
            pieces[i].srcBone = srcBone[i];
            pieces[i].lo[0] = pieces[i].lo[1] = pieces[i].lo[2] = 1e9f;
            pieces[i].hi[0] = pieces[i].hi[1] = pieces[i].hi[2] = -1e9f;
        }
        // Rest bounds of the clay each piece carries: the vertices whose
        // DOMINANT bone it owns — the same rule the per-cell skin field uses,
        // so the AABB cull and the ownership test agree about where a piece
        // lives.
        const uint32_t vc = asset.vertexCount();
        for (uint32_t v = 0; v < vc && asset.weights.size() >= (size_t)vc * 4; v++) {
            int dom = -1;
            float bw = 0.f;
            for (int s = 0; s < 4; s++) {
                const float w = asset.weights[v * 4 + s];
                if (w > bw) {
                    bw = w;
                    dom = asset.joints[v * 4 + s];
                }
            }
            if (dom < 0 || (size_t)dom >= nb) continue;
            AffinePiece& ap = pieces[owner[dom]];
            for (int k = 0; k < 3; k++) {
                ap.lo[k] = std::min(ap.lo[k], asset.positions[v * 3 + k] - 0.012f);
                ap.hi[k] = std::max(ap.hi[k], asset.positions[v * 3 + k] + 0.012f);
            }
        }
        // A piece with no clay (a rig with no mitts, or a hand nothing is
        // bound to) carries an INVERTED box, which would poison the clip. The
        // box test alone drops it — a per-piece bone mask used to mark the
        // same thing, but the only consumer was the shader's dominant-bone
        // ownership test, and that is deleted (brick_read.wgsl: "CLIPPING,
        // NOT OWNERSHIP - and do not put ownership back").
        if (pieces[0].hi[0] >= pieces[0].lo[0]) {
            for (const AffinePiece& ap : pieces)
                if (ap.hi[0] >= ap.lo[0]) fighters_[0].pieces.push_back(ap);
        }
        if (bones_.size() > 16 && !fighters_[0].pieces.empty()) {
            std::fprintf(stderr,
                         "[rig] %zu bones exceeds the 16-bone ownership mask; "
                         "falling back to the 13-piece warp\n",
                         bones_.size());
        }
    }
    std::printf("anim: %zu bones, %zu clip(s), %zu shadow capsules, %zu hand(s), "
                "rest reach %.3f m, %zu affine piece(s)\n",
                bones_.size(), clips_.size(), capsules_.size(), handIk_.size(),
                autoReach_, fighters_[0].pieces.size());
    // Every fighter is the same character in its own volume slice. The rest
    // capsules are shared outright (same mesh), and the rest bound below is
    // the fallback the tracer rejects against before the pieces exist.
    {
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        if (brush_.valid) {
            // The BODY brush only. Taking this over every vertex would now
            // include the grab hand parked in the negative-x half of the
            // volume, inflating the rest bound to cover ground the fighter
            // never occupies. The posed bound that actually gates the trace is
            // recomputed from the piece boxes each frame (affineBoundR); this
            // is only the fallback used before the pieces exist.
            for (int a = 0; a < 3; a++) {
                lo[a] = brush_.bodyLo[a];
                hi[a] = brush_.bodyHi[a];
            }
        } else {
            for (uint32_t v = 0; v < asset.vertexCount(); v++) {
                for (int a = 0; a < 3; a++) {
                    lo[a] = std::min(lo[a], asset.positions[v * 3 + a]);
                    hi[a] = std::max(hi[a], asset.positions[v * 3 + a]);
                }
            }
        }
        float r2 = 0.f;
        for (int a = 0; a < 3; a++) {
            bodyCenterRest_[a] = (lo[a] + hi[a]) * 0.5f;
            float h = (hi[a] - lo[a]) * 0.5f;
            r2 += h * h;
        }
        bodyBoundRest_ = std::sqrt(r2) + 0.05f;
    }
    // Every fighter is the same character, so the mesh-derived half of the
    // import (bins, watertight parity, smooth normals, and the read-only GPU
    // uploads) is computed ONCE and shared. Doing it per BrickSystem cost
    // ~2.5 s of startup and a duplicate ~32 MB upload PER EXTRA FIGHTER for
    // byte-identical results — at four bodies that would have been ~7.5 s and
    // ~96 MB. Only the OUTPUT volume slice is per fighter.
    std::shared_ptr<BrickSystem::MeshImport> mesh =
        BrickSystem::prepareImport(*gpu_, asset);
    for (Fighter& f : fighters_) f.vol.requestImport(mesh);
}

void Renderer::encodePick(wgpu::CommandEncoder& enc) {
    wgpu::ComputePassEncoder pass = enc.BeginComputePass();
    pass.SetPipeline(pickPipeline_);
    pass.SetBindGroup(0, pickBind_);
    pass.SetBindGroup(1, pickBrickBind_);
    pass.DispatchWorkgroups(1);
    pass.End();
    if (!pickMapPending_) {
        enc.CopyBufferToBuffer(pickOut_, 0, pickRead_, 0, 64);
    }
}

void Renderer::pollPick() {
    if (pickMapPending_) return;
    pickMapPending_ = true;
    auto alive = alive_;
    pickRead_.MapAsync(wgpu::MapMode::Read, 0, 64, wgpu::CallbackMode::AllowSpontaneous,
                       [this, alive](wgpu::MapAsyncStatus status, wgpu::StringView) {
                           if (!*alive) return;
                           if (status == wgpu::MapAsyncStatus::Success) {
                               const float* d =
                                   (const float*)pickRead_.GetConstMappedRange(0, 64);
                               pickPos_[0] = d[0];
                               pickPos_[1] = d[1];
                               pickPos_[2] = d[2];
                               pickValid_ = d[3] > 0.5f;
                               pickNormal_[0] = d[4];
                               pickNormal_[1] = d[5];
                               pickNormal_[2] = d[6];
                               pickMat_ = d[7];
                               pickAlbedo_[0] = d[8];
                               pickAlbedo_[1] = d[9];
                               pickAlbedo_[2] = d[10];
                               pickRest_[0] = d[12];
                               pickRest_[1] = d[13];
                               pickRest_[2] = d[14];
                               // which fighter's volume pickRest_ addresses;
                               // -1 when the ray hit no clay at all
                               pickPlayer_ = (int)d[15];
                               pickRead_.Unmap();
                               // world vs rest at the cursor: the gap between
                               // them IS the articulation, and edits must use
                               // rest. Silent divergence here is what makes a
                               // walked-away fighter stop carving.
                               static const bool dbg =
                                   std::getenv("CLAYFRAY_DEBUG_PICK") != nullptr;
                               if (dbg && pickValid_) {
                                   std::printf("[pick] world (%.4f %.4f %.4f) rest "
                                               "(%.4f %.4f %.4f) mat %.1f player %d\n",
                                               pickPos_[0], pickPos_[1], pickPos_[2],
                                               pickRest_[0], pickRest_[1], pickRest_[2],
                                               pickMat_, pickPlayer_);
                                   std::fflush(stdout);
                               }
                           }
                           pickMapPending_ = false;
                       });
}

BrickEdit Renderer::queueBrickEdit(BrickEdit e) {
    // WHICH body gets cut comes from the edit itself (BrickEdit::player), NOT
    // from the live pick: `pos` is rest space, so the caller that produced the
    // coordinates is the only one that knows which slice they address. The
    // interactive sculpt path sets it from pickPlayer(); everything scripted
    // leaves it at the hero, which is what every journal and --carve-test
    // assumes.
    const int target = clampPlayer(e.player);
    // snapshot the wound's facing + material color for the gobs this edit
    // will tear off (measurement arrives frames later; pick moves on)
    if (pickValid_ && !e.fromGob) {
        std::memcpy(e.outDir, pickNormal_, sizeof(e.outDir));
        std::memcpy(e.worldPos, pickPos_, sizeof(e.worldPos));
        if (pickMat_ > 2.5f && pickMat_ < 3.5f) {
            // (srcColor is stamped by queueEdit from the fighter's own clay
            // colour; pickAlbedo_ would carry bruise stains into the gob)
        }
    } else if (e.worldPos[0] == 0.f && e.worldPos[1] == 0.f && e.worldPos[2] == 0.f) {
        // scripted edit (ctl/replay/carve-test): authored in rest space, so
        // place its wound by the TARGET fighter's root. Exact while unposed,
        // and only the gob spawn point rides on it.
        const FighterPose& d = fighters_[target].disp;
        float cy = std::cos(d.yaw), sy = std::sin(d.yaw);
        e.worldPos[0] = d.pos[0] + cy * e.pos[0] + sy * e.pos[2];
        e.worldPos[1] = d.pos[1] + e.pos[1];
        e.worldPos[2] = d.pos[2] - sy * e.pos[0] + cy * e.pos[2];
    }
    e.player = target;
    fighters_[target].vol.queueEdit(e);
    return e;
}

void Renderer::absorbMeasured() {
    // Measurements from ops that ran 1-2 frames ago. Always drained — even
    // with conservation off — or a measurement queued while ON but arriving
    // while OFF would desync the ledger the next time it's toggled back on.
    // The toggle gates SPAWNING (updateConservation), not accounting.
    // EVERY fighter bills to ONE ledger: clay carved off any body becomes the
    // same gobs and lands on the same arena, so conservation is arena-wide.
    MeasuredEdit m;
    for (Fighter& fighter : fighters_) {
      BrickSystem* bs = &fighter.vol;
      while (bs->takeMeasured(m)) {
        if (std::getenv("CLAYFRAY_DEBUG_LEDGER")) {
            std::printf("[sploot] mode %d measured %.1f ml\n", m.edit.mode,
                        m.volume * 1e6f);
        }
        if (m.edit.mode == 1) {
            sploot_.carved += m.volume;
            sploot_.debt += m.volume;
            // M-DEATH: the same measurement, billed to the body it came off.
            // Damage is not a separate quantity — it is this one, per fighter,
            // which is why a wound cannot exist on the ledger and be missing
            // from the health bar or the other way round.
            if (m.edit.player >= 0 && m.edit.player < kMaxPlayers) {
                fighters_[m.edit.player].carved += m.volume;
            }
            std::memcpy(lastWound_, m.edit.worldPos, sizeof(lastWound_));
            std::memcpy(woundDir_, m.edit.outDir, sizeof(woundDir_));
            std::memcpy(woundCol_, m.edit.srcColor, sizeof(woundCol_));
            haveWound_ = true;
            if (m.edit.fromWeapon) {
                // Billing is UNCHANGED above (carved += v, debt += v). All this
                // does is withhold that much debt from the dribble spawner so
                // the whole slice can leave as one blob — the clay is on the
                // ledger the entire time.
                if (slicePending_ > 0) slicePending_--;
                sliceVol_ += m.volume;
                sliceOpen_ = true;
            }
        } else if (m.edit.mode == 3) {
            // R20: a dent moves clay around inside one body; the profile is
            // authored so the core and the rim cancel, and this measurement is
            // the SIGNED residual curvature leaves behind — measured at ~3% of
            // the displaced volume on a body this round, so a millilitre or two
            // per punch against the ~48 ml it moved, and positive (the dent
            // nets a little clay OFF) in every case tested.
            //
            // Billing it to BOTH sides of the ledger is what makes that exact
            // rather than approximately-zero: carved and debt move together, so
            // `carved == deposited + inFlight + debt` survives whatever the
            // residual turns out to be, in either direction. A negative one
            // (the dent net-created clay) reads as the body borrowing from what
            // it already owed the arena, which is the same bookkeeping a
            // re-stuck gob does.
            //
            // No wound, no gob, no fromWeapon: the dribble spawner needs debt
            // ABOVE its minimum gob to fire, so a residual this small simply
            // waits for real damage to carry it out.
            sploot_.carved += m.volume;
            sploot_.debt += m.volume;
            if (m.edit.player >= 0 && m.edit.player < kMaxPlayers) {
                Fighter& f = fighters_[m.edit.player];
                f.carved = std::max(0.f, f.carved + m.volume);
            }
        } else if (m.edit.mode == 2 && m.edit.fromGob) {
            // a landed gob became body clay; smin over/under-fill goes back
            // on the ledger so nothing is created or destroyed
            sploot_.deposited += m.volume;
            sploot_.debt += m.edit.gobVol - m.volume;
            // M-DEATH: clay that sticks back on is damage UNDONE. Without this
            // a fighter could be healed to visibly whole by re-sticks and still
            // drop dead, because the damage counter only ever went up.
            if (m.edit.player >= 0 && m.edit.player < kMaxPlayers) {
                Fighter& f = fighters_[m.edit.player];
                f.carved = std::max(0.f, f.carved - m.volume);
            }
        }
      }
    }
}

void Renderer::reportContact(int attacker, int target, int side, float bite,
                             const float dir[3], float speed, const float nrm[3]) {
    if (!(bite > 0.f)) return;
    for (StrikeContact& c : contacts_) {
        if (c.attacker != attacker || c.target != target || c.side != side) continue;
        // Deepest wins. Keeping the deepest sample's direction with it matters:
        // a blade's sweep direction barely changes across one frame's substeps,
        // but a fist's does at the turnaround, and pairing a deep bite with a
        // retreating direction would knock the target TOWARD the puncher. The
        // normal travels with them for the same reason — squareness is a
        // relation between the two, so a direction from one substep and a
        // normal from another describes a hit that never happened.
        if (bite > c.bite) {
            c.bite = bite;
            c.speed = speed;
            for (int k = 0; k < 3; k++) c.dir[k] = dir[k];
            for (int k = 0; k < 3; k++) c.nrm[k] = nrm[k];
        }
        return;
    }
    StrikeContact c;
    c.attacker = attacker;
    c.target = target;
    c.side = side;
    c.bite = bite;
    c.speed = speed;
    for (int k = 0; k < 3; k++) c.dir[k] = dir[k];
    for (int k = 0; k < 3; k++) c.nrm[k] = nrm[k];
    contacts_.push_back(c);
}

void Renderer::posedCapsule(const Fighter& owner, const BoneCapsule& c, float a[3],
                            float b[3], float& r) const {
    for (int k = 0; k < 3; k++) {
        a[k] = c.a[k];
        b[k] = c.b[k];
    }
    r = c.r;
    if (skeletonFree()) {
        // Under the brush rig `bone` is a PIECE index into THIS fighter's own
        // piece table. Falling through to identity would leave its hitboxes at
        // the origin while the body stands elsewhere, and a weapon would cut
        // empty air.
        if (c.bone >= 0 && (size_t)c.bone < owner.pieces.size()) {
            // REFIT from the piece's CURRENT box, do not transform the stored
            // rest capsule. The stored one was fitted to the rest brush; the
            // piece's transform undoes the SELECTED brush's rest-space offset,
            // so a capsule still expressed in another brush's frame lands a
            // metre across the arena. Invisible while every target held rest
            // hands; real the moment an opponent closes its fists.
            const AffinePiece& ap = owner.pieces[c.bone];
            float ca[3], cb[3];
            capsuleFromBox(ap.lo, ap.hi, ca, cb, r);
            matTransformPoint(ap.xform, ca, a);
            matTransformPoint(ap.xform, cb, b);
        }
        return;
    }
    if (c.bone >= 0 && (size_t)(c.bone * 16 + 16) <= owner.skinMats.size()) {
        matTransformPoint(&owner.skinMats[c.bone * 16], c.a, a);
        matTransformPoint(&owner.skinMats[c.bone * 16], c.b, b);
    }
}

// M-FIST: a travelling fist carves whatever it goes through.
//
// The same shape as updateBladeCut one weapon down, and the differences are
// all consequences of a fist being a BALL rather than a blade:
//   - the swept volume of a ball IS a capsule, so one edit covers the frame's
//     whole motion and there is no substep loop to bridge the gap;
//   - the hit test inflates the target's capsules by the fist radius, because
//     what has to overlap is two solids, not a point and a solid;
//   - there is no reach test, because the rig already applied one — the mitt
//     is clamped to the reach ball in unarmedHand, so a punch physically
//     cannot arrive from further away than a sword thrust.
// Everything else — rest-space mapping through the piece that was hit, the
// bounds pre-check, pooling into one gob — is the blade's, deliberately.
void Renderer::updatePunchCut(const LookParams& look) {
    // Needs the brush rig's piece transforms: the fist's world centre is a
    // piece's translation column, and a rigged asset has no such piece.
    if (!skeletonFree() || capsules_.empty()) return;
    const HandPoseParams& hp = look.handPose;
    const float fistR = std::max(hp.fistRadius, 0.005f);
    // Same gate as the blade, same reason: a fist resting against a body would
    // otherwise bore through it one edit per frame. Expressed as a speed and
    // converted at the sim's fixed tick rate, so it means the same thing
    // whatever the frame rate is.
    const float minSweep = std::max(hp.cutSpeed, 0.f) / 60.f;

    for (int atk = 0; atk < kMaxPlayers; atk++) {
        Fighter& att = fighters_[atk];
        if (atk > 0 && !att.enabled) continue;
        if (att.dead) continue; // no arms, no punch
        if (att.pieces.size() < 3) continue;

        // The fist's centre, in world.
        //
        // NOT the piece's translation column, which is the tempting one-liner
        // and is WRONG: xform = M * T(pre), so its translation is M applied to
        // `pre` — the world position of the brush-space ORIGIN, which is most
        // of a metre from the palm. Using it put every punch's wound low on the
        // target's body while the fist visibly struck high, and the ledger
        // cannot tell you that (it only ever reports a volume).
        //
        // The palm's own brush-space coordinate is palm + ofs, and running THAT
        // through the piece gives the fist where it is actually drawn.
        float cur[2][3];
        for (int side = 0; side < 2; side++) {
            const int b =
                std::min(std::max(att.handPose[side], 0), (int)kHandBrushCount - 1);
            const float palmBrush[3] = {brush_.palm[b][0] + brush_.ofs[b][0],
                                        brush_.palm[b][1] + brush_.ofs[b][1],
                                        brush_.palm[b][2] + brush_.ofs[b][2]};
            matTransformPoint(att.pieces[1 + side].xform, palmBrush, cur[side]);
        }
        if (!att.haveHandPrev) {
            std::memcpy(att.handPrev, cur, sizeof(cur));
            att.haveHandPrev = true;
            continue;
        }

        for (int side = 0; side < 2; side++) {
            float pv[3];
            std::memcpy(pv, att.handPrev[side], sizeof(pv));
            std::memcpy(att.handPrev[side], cur[side], sizeof(pv));
            // Only a CLOSED fist punches. An open idle hand brushing past a
            // body is not a strike, and the pose is already latched on the
            // 12 Hz grid so this cannot flicker within a pose step.
            if (att.handPose[side] != kHandFist) continue;

            float sweep = 0.f;
            for (int k = 0; k < 3; k++) {
                const float d = cur[side][k] - pv[k];
                sweep += d * d;
            }
            sweep = std::sqrt(sweep);
            // CLAYFRAY_DEBUG_PUNCH=1: the twin of CLAYFRAY_DEBUG_BLADE. A punch
            // that visibly connects but carves nothing has four possible
            // culprits — wrong brush, too slow, missed the capsules, or the
            // rest-space edit fell outside the volume — and they are
            // indistinguishable from the ledger, which just reads 0.0 ml.
            const bool dbg = std::getenv("CLAYFRAY_DEBUG_PUNCH") != nullptr;
            if (dbg) {
                std::printf("[punch] p%d side %d sweep=%.4f fist=(%.2f %.2f %.2f)\n",
                            atk, side, sweep, cur[side][0], cur[side][1],
                            cur[side][2]);
                std::fflush(stdout);
            }
            if (sweep < minSweep) continue;
            // A TELEPORT is not a punch. `set p1.pos`, a snapshot load, or a
            // pose index re-latch can move a mitt further in one frame than any
            // strike ever does, and carving that path would mow a trench across
            // the arena. 0.35 m in a frame is ~21 m/s — well past a jab and
            // well short of anything the rig produces.
            if (sweep > 0.35f) continue;

            for (int ti = 0; ti < kMaxPlayers; ti++) {
                if (ti == atk) continue; // a fighter cannot punch itself
                Fighter& tgt = fighters_[ti];
                if (ti > 0 && !tgt.enabled) continue;
                if (tgt.dead) continue; // nothing left to hit
                if (tgt.pieces.empty()) continue;

                // the span of this frame's fist path that lies inside the target
                float tIn = 2.f, tOut = -1.f, nrm[3] = {0, 1, 0};
                int hitPiece = -1;
                // M-PHYS: deepest penetration of the fist past the surface,
                // over the whole swept path. NOT the swept span (tOut - tIn) —
                // that is how far the fist TRAVELLED while inside, which for a
                // fist pressing slowly into a body is near zero exactly when
                // the resistance should be highest.
                float maxPen = 0.f;
                // Where the fist was when it was deepest. R20 puts the dent on
                // the SURFACE, and the surface under the fist is exactly this
                // point walked back along the contact normal by however much of
                // the fist is not buried.
                float deepP[3] = {0.f, 0.f, 0.f};
                const int kSamples = 16;
                for (int i = 0; i <= kSamples; i++) {
                    const float t = (float)i / (float)kSamples;
                    float p[3];
                    for (int k = 0; k < 3; k++) p[k] = pv[k] + (cur[side][k] - pv[k]) * t;
                    for (const BoneCapsule& c : capsules_) {
                        float a2[3], b2[3], cr;
                        posedCapsule(tgt, c, a2, b2, cr);
                        float ab[3] = {b2[0] - a2[0], b2[1] - a2[1], b2[2] - a2[2]};
                        float ap[3] = {p[0] - a2[0], p[1] - a2[1], p[2] - a2[2]};
                        const float len2 =
                            ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
                        float u = len2 > 1e-9f
                                      ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) /
                                            len2
                                      : 0.f;
                        u = std::min(std::max(u, 0.f), 1.f);
                        float q[3] = {a2[0] + ab[0] * u, a2[1] + ab[1] * u,
                                      a2[2] + ab[2] * u};
                        float d[3] = {p[0] - q[0], p[1] - q[1], p[2] - q[2]};
                        const float dist =
                            std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                        // inflate by the fist: two solids, not a point
                        if (dist >= cr + fistR) continue;
                        if ((cr + fistR) - dist > maxPen) {
                            maxPen = (cr + fistR) - dist;
                            std::memcpy(deepP, p, sizeof(deepP));
                            // THE NORMAL BELONGS TO THE DEEPEST SAMPLE, not to
                            // the first one that touched. contactW below is
                            // deepP walked back along it, and the dent's whole
                            // volume argument rests on that axis being the
                            // surface normal THERE — pairing the deepest point
                            // with the ENTRY normal tilted the dent by however
                            // far the fist had travelled around the body since
                            // it made contact, and R22 now reads squareness off
                            // the same vector, so an entry normal would grade a
                            // punch by how it arrived rather than how it landed.
                            const float ninv = dist > 1e-6f ? 1.f / dist : 0.f;
                            for (int k = 0; k < 3; k++) nrm[k] = d[k] * ninv;
                        }
                        tIn = std::min(tIn, t);
                        tOut = std::max(tOut, t);
                        if (hitPiece < 0) hitPiece = c.bone;
                        break;
                    }
                }
                if (tOut < tIn) continue; // missed

                const ImpactParams& imp = look.impact;
                const float invSweep = sweep > 1e-6f ? 1.f / sweep : 0.f;
                const float travel[3] = {(cur[side][0] - pv[0]) * invSweep,
                                         (cur[side][1] - pv[1]) * invSweep,
                                         (cur[side][2] - pv[2]) * invSweep};
                // HOW SQUARE THE HIT IS: 1 driven straight down the normal, 0
                // skidding along the skin. Everything R22 does is graded by it.
                float square = 0.f;
                for (int k = 0; k < 3; k++) square -= travel[k] * nrm[k];
                square = std::min(std::max(square, 0.f), 1.f);

                // ---- R22: the fist skids, it does not bore ----
                // Push the mitt back out along the surface normal by
                // `glance * penetration`. The punch arc keeps moving it
                // TANGENTIALLY, so what was a fist driving through a body
                // becomes a fist riding its surface.
                //
                // SET, NOT ACCUMULATED, and that distinction is the whole
                // stability of it. Adding a displacement per frame is a
                // velocity wearing a length's clothes: it would grow without
                // bound while a fist stayed buried and it would grow FASTER at
                // a higher frame rate. Assigned instead, the offset is a
                // positional response with a fixed point — `cur` is read off a
                // piece this offset already moved, so an animated drive of p0
                // settles at o = glance*p0/(1 + glance) and the fist really
                // penetrates p0/(1 + glance), whatever the frame rate.
                //
                // No push-out is applied to the carve below. The mitt itself
                // has moved, so the fist path IS the glanced path and the wound
                // follows it for free; adding the offset a second time at the
                // edit would count it twice and cut a wound shallower than the
                // fist that made it.
                if (imp.glance > 0.f) {
                    const float want = imp.glance * maxPen;
                    float have = 0.f;
                    for (int k = 0; k < 3; k++) {
                        have += att.glance[side][k] * att.glance[side][k];
                    }
                    // Deepest contact of the frame wins the deflection, the
                    // same rule reportContact applies to the shove: two targets
                    // must not push one mitt twice.
                    if (want * want > have) {
                        for (int k = 0; k < 3; k++) att.glance[side][k] = nrm[k] * want;
                    }
                    if (std::getenv("CLAYFRAY_DEBUG_IMPACT")) {
                        std::printf("[glance] p%d side %d pen=%.4f square=%.2f "
                                    "offset=%.4f had=%.4f\n",
                                    atk, side, maxPen, square, want,
                                    std::sqrt(have));
                        std::fflush(stdout);
                    }
                }

                // M-PHYS. Reported BEFORE the bounds test below, and that is
                // deliberate: the fist is physically inside the target whether
                // or not the carve was accepted, and an edit dropped at the
                // volume boundary (trap 5) must not also silently drop the
                // knockback. Contact is about the capsules; the edit is about
                // the volume.
                reportContact(atk, ti, side, maxPen, travel, sweep * 60.f, nrm);

                float wA[3], wB[3];
                for (int k = 0; k < 3; k++) {
                    const float d = cur[side][k] - pv[k];
                    wA[k] = pv[k] + d * tIn;
                    wB[k] = pv[k] + d * tOut;
                }
                // world -> the TARGET'S REST volume (trap 6), through the piece
                // the fist actually met
                float invPiece[16];
                matIdentity(invPiece);
                if (hitPiece >= 0 && (size_t)hitPiece < tgt.pieces.size()) {
                    matInvAffine(tgt.pieces[hitPiece].xform, invPiece);
                }
                // The point on the SKIN under the deepest part of the fist: the
                // fist centre, walked back along the outward normal by the part
                // of it that is still outside.
                float contactW[3];
                for (int k = 0; k < 3; k++) {
                    contactW[k] = deepP[k] - nrm[k] * (fistR - maxPen);
                }

                // ---- R20: the dent ----
                // Most of a punch is displacement, and displacement is free on
                // the ledger by construction — no gob, no debt, no dribble. It
                // goes in FIRST so the rupture below cuts a surface that has
                // already moved, which is the order the two happen physically.
                //
                // The axis is the inward SURFACE NORMAL, not the punch
                // direction: the profile's volume cancels exactly for a surface
                // perpendicular to the axis, and a glancing blow displacing
                // along its own travel would mostly shove clay sideways.
                if (imp.dentDepth > 0.f) {
                    BrickEdit d;
                    d.mode = 3;
                    d.player = tgt.vol.slot();
                    d.radius = std::max(fistR * imp.dentRadius, 1e-4f);
                    const float halfLen =
                        std::max(d.radius * imp.dentLength, 1e-4f);
                    float aW[3], bW[3];
                    for (int k = 0; k < 3; k++) {
                        aW[k] = contactW[k] + nrm[k] * halfLen; // outside the skin
                        bW[k] = contactW[k] - nrm[k] * halfLen; // into the body
                    }
                    matTransformPoint(invPiece, aW, d.pos);
                    matTransformPoint(invPiece, bW, d.posB);
                    // The amplitude is authored in world metres but applied to a
                    // rest-space field, and the body affine carries a squish.
                    // The axis segment went through the same transform, so its
                    // length ratio is that scale along the direction that
                    // matters.
                    float restLen = 0.f;
                    for (int k = 0; k < 3; k++) {
                        const float dd = d.posB[k] - d.pos[k];
                        restLen += dd * dd;
                    }
                    restLen = std::sqrt(restLen);
                    const float scale = halfLen > 1e-6f ? restLen / (2.f * halfLen) : 1.f;
                    // Graded by PENETRATION, not by squareness. A fist barely
                    // grazing the skin presses shallowly; one buried to its
                    // equator presses all the way. maxPen/fistR is the stable
                    // form of that — the per-frame -dot(travel, normal) version
                    // collapsed to zero at the apex of every punch, which is
                    // exactly where the deepest dent belongs. Scaling the
                    // AMPLITUDE is safe for conservation: the profile's shape is
                    // untouched and its integral was zero at every amplitude.
                    const float press = std::min(1.f, maxPen / std::max(fistR, 1e-4f));
                    d.dentAmp = imp.dentDepth * scale * press;
                    std::memcpy(d.worldPos, contactW, sizeof(d.worldPos));
                    std::memcpy(d.outDir, nrm, sizeof(d.outDir));
                    if (tgt.vol.editInBounds(d)) tgt.vol.queueEdit(d);
                }

                // ---- the rupture ----
                // What is left of the punch after the dent took the bulk of it:
                // a smaller carve that does eject a chunk, and therefore is what
                // M-DEATH counts as damage. impact.punchCarve is the dial.
                BrickEdit e;
                e.mode = 1; // carve
                e.radius = fistR * std::max(imp.punchCarve, 0.f);
                e.segment = true;
                matTransformPoint(invPiece, wA, e.pos);
                matTransformPoint(invPiece, wB, e.posB);
                for (int k = 0; k < 3; k++) e.worldPos[k] = (wA[k] + wB[k]) * 0.5f;
                std::memcpy(e.outDir, nrm, sizeof(e.outDir));
                e.player = tgt.vol.slot();
                e.fromWeapon = true; // one chunk, not a dribble

                // ---- R21: the bruise ----
                // Queued whether or not the rupture lands, because a punch that
                // was too glancing to break the skin still leaves a mark — and
                // an albedo stamp costs no allocation, no JFA and no ledger.
                if (imp.bruise > 0.f) {
                    BrickEdit b;
                    b.mode = 4;
                    b.player = tgt.vol.slot();
                    b.radius = std::max(fistR * imp.bruiseRadius, 1e-4f);
                    b.paint = imp.bruise;
                    // A SEGMENT, not a spot: the fist skidded, so the stain
                    // it leaves is a streak along the skin. The slide is the
                    // travel over the contact span with the normal component
                    // taken out — i.e. what actually rubbed — and it costs
                    // nothing, because mode 4 already carries two endpoints and
                    // paint allocates nothing, moves no field and never reaches
                    // the JFA. A dead-square punch has no tangential component
                    // and degenerates back to the spot it always was.
                    float slide[3];
                    for (int k = 0; k < 3; k++) {
                        slide[k] = (cur[side][k] - pv[k]) * (tOut - tIn);
                    }
                    float sn = 0.f;
                    for (int k = 0; k < 3; k++) sn += slide[k] * nrm[k];
                    float skidA[3], skidB[3];
                    for (int k = 0; k < 3; k++) {
                        slide[k] -= nrm[k] * sn;
                        skidA[k] = contactW[k] - slide[k] * 0.5f;
                        skidB[k] = contactW[k] + slide[k] * 0.5f;
                    }
                    matTransformPoint(invPiece, skidA, b.pos);
                    matTransformPoint(invPiece, skidB, b.posB);
                    std::memcpy(b.color, imp.bruiseColor, sizeof(b.color));
                    std::memcpy(b.worldPos, contactW, sizeof(b.worldPos));
                    if (tgt.vol.editInBounds(b)) tgt.vol.queueEdit(b);
                }

                if (e.radius <= 0.f || !tgt.vol.editInBounds(e)) continue;
                tgt.vol.queueEdit(e);
                // the chunk leaves from where the fist was DEEPEST, along the
                // punch — clay knocked off the far side of the impact
                std::memcpy(sliceExit_, wB, sizeof(sliceExit_));
                std::memcpy(sliceNrm_, nrm, sizeof(sliceNrm_));
                // NOT e.srcColor: queueEdit stamps the copy it keeps, so
                // this local still holds BrickEdit's fallback and the chunk
                // flew the wrong colour. Measured 5 of 96 gobs in carve-duel.
                std::memcpy(sliceCol_, fighters_[clampPlayer(e.player)].vol.bodyColor(),
                            sizeof(sliceCol_));
                std::memcpy(sliceSweep_, travel, sizeof(sliceSweep_));
                sliceOpen_ = true;
                sliceCutStep_ = true;
                slicePending_++;
                sliceWait_ = 0;
            }
        }
    }
}

void Renderer::updateBladeCut(const LookParams& look) {
    // Player 0 wields the sword; anyone else on the field is a target.
    if (!swordWorld_.enabled || capsules_.empty()) {
        haveBlade_ = false;
        return;
    }
    float hilt[3], tip[3], gA[3], gB[3];
    swordGeometry(swordWorld_, hilt, tip, gA, gB);
    if (!haveBlade_) {
        std::memcpy(prevTip_, tip, sizeof(prevTip_));
        std::memcpy(prevHilt_, hilt, sizeof(prevHilt_));
        haveBlade_ = true;
        return;
    }
    // Only a SWINGING blade cuts. Resting the sword against the opponent
    // would otherwise carve a hole through it one edit per frame.
    float sweep = 0.f;
    for (int a = 0; a < 3; a++) {
        float d = tip[a] - prevTip_[a];
        sweep += d * d;
    }
    sweep = std::sqrt(sweep);
    // KEEP the previous blade as geometry before advancing it — the substeps
    // below interpolate from it. Overwriting first made every substep land on
    // the current blade, so a fast swing bored one tunnel per frame with gaps
    // between instead of one continuous slot.
    float pH[3], pT[3];
    std::memcpy(pH, prevHilt_, sizeof(pH));
    std::memcpy(pT, prevTip_, sizeof(pT));
    std::memcpy(prevTip_, tip, sizeof(prevTip_));
    std::memcpy(prevHilt_, hilt, sizeof(prevHilt_));
    if (sweep < 0.012f) return; // ~0.7 m/s at 60 Hz

    // The cut is the BLADE, not a ball at its deepest point: carve the span of
    // the blade that lies within the opponent, as a capsule. A sphere centred
    // on maximum penetration sits entirely inside the body and hollows it out
    // without ever breaking the surface — 1.4 litres removed, nothing visible.
    // capsules posed by the opponent's OWN skeleton (root already folded in),
    // so the hit test tracks it while it animates
    // Blade tests happen in WORLD space, so the opponent's rest capsules have
    // to be posed first. Under the brush rig `bone` is a PIECE index into the
    // foe's own piece table — falling through to identity here would leave the
    // foe's hitboxes sitting at the origin while the foe itself stands
    // elsewhere, and the sword would cut empty air.
    const bool brushRig = skeletonFree();
    if (std::getenv("CLAYFRAY_DEBUG_BLADE")) {
        std::printf("[blade] sweep=%.4f brushRig=%d caps=%zu "
                    "hilt=(%.2f %.2f %.2f) tip=(%.2f %.2f %.2f)\n",
                    sweep, (int)brushRig, capsules_.size(), hilt[0], hilt[1],
                    hilt[2], tip[0], tip[1], tip[2]);
        std::fflush(stdout);
    }

    // ONE TARGET. Called for every fighter but the wielder, so a four-way
    // brawl cuts whoever the blade actually passes through instead of only
    // ever player 1. Each one costs a single fused op now (R19), so a brawl
    // cannot exhaust the frame's edit budget the way six-substeps-per-target
    // could.
    auto cut = [&](Fighter& tgt) {
    // ---- M-DEATH: cut clean through the middle and you are done ----
    // Runs before the carve and inside the sweep gate above, so it takes a
    // real cutting STROKE — a sword parked through someone is not a bisection,
    // it is a sword parked through someone. Tested against the BODY capsule
    // only: severing a mitt is a wound, not a bisection.
    if (look.death.enabled && look.death.bisect && !tgt.dead && !tgt.bisected) {
        for (const BoneCapsule& bc : capsules_) {
            if (bc.bone != 0) continue; // piece 0 is the body
            float a2[3], b2[3], cr;
            posedCapsule(tgt, bc, a2, b2, cr);
            const int kS = 32;
            int firstIn = -1, lastIn = -1;
            float minAxis = 1e9f;
            float cutW[3] = {0, 0, 0};
            for (int i = 0; i <= kS; i++) {
                const float t = (float)i / (float)kS;
                float p[3];
                for (int k = 0; k < 3; k++) p[k] = hilt[k] + (tip[k] - hilt[k]) * t;
                float ab[3] = {b2[0] - a2[0], b2[1] - a2[1], b2[2] - a2[2]};
                float ap[3] = {p[0] - a2[0], p[1] - a2[1], p[2] - a2[2]};
                const float len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
                float u = len2 > 1e-9f ? (ap[0] * ab[0] + ap[1] * ab[1] +
                                          ap[2] * ab[2]) / len2
                                       : 0.f;
                u = std::min(std::max(u, 0.f), 1.f);
                const float q[3] = {a2[0] + ab[0] * u, a2[1] + ab[1] * u,
                                    a2[2] + ab[2] * u};
                const float d[3] = {p[0] - q[0], p[1] - q[1], p[2] - q[2]};
                // distance to the AXIS, not to the surface: "through the centre
                // of mass" is a statement about the spine, and the surface
                // distance is zero anywhere inside.
                const float dist =
                    std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (dist < cr) {
                    if (firstIn < 0) firstIn = i;
                    lastIn = i;
                }
                if (dist < minAxis) {
                    minAxis = dist;
                    // the point ON THE SPINE the blade passed closest to: that
                    // is the cut plane's height, and the only sample of it we
                    // will get — the blade has moved on by the next frame.
                    std::memcpy(cutW, q, sizeof(cutW));
                }
            }
            // BOTH ends outside. firstIn > 0 puts the hilt end in clear air and
            // lastIn < kS the tip, so the blade genuinely spans the body rather
            // than being buried in it — a sword whose hilt is inside the target
            // has not passed through, it has been shoved in.
            const bool spans = firstIn > 0 && lastIn >= 0 && lastIn < kS;
            const float rad = std::min(std::max(look.death.bisectRadius, 0.f), 1.f);
            if (spans && minAxis <= cr * rad) {
                tgt.bisected = true;
                beginSever(tgt, cutW, tip, hilt, look);
                std::printf("[death] player %d BISECTED (blade passed %.0f mm "
                            "from the axis, body r=%.0f mm)\n",
                            tgt.vol.slot(), minAxis * 1e3f, cr * 1e3f);
                std::fflush(stdout);
            }
            break;
        }
    }
    // Sweep in TIME as well as along the blade, and emit ONE CAPSULE PER
    // SUBSTEP rather than collapsing the whole sweep into a single capsule.
    // Testing only the current hilt->tip segment samples an instant: at swing
    // speed the blade crosses several centimetres per frame, so the wound came
    // out as disconnected slices. Collapsing the sweep into one capsule was no
    // better — it cut the cloud's diagonal, not the blade's path. Substeps
    // spaced under a brush radius apart union into one clean swept slot.
    // substeps spaced well under a brush radius: at exactly one radius the
    // union scallops along its edge, at ~0.6 it reads as one clean slot
    const float step = std::max(swordWorld_.radius * 1.8f, 0.022f) * 0.6f;
    int subs = (int)std::ceil(sweep / step);
    // R19: bounded by how many capsules ONE brush may union, which is a memory
    // number. It used to be bounded by kOpsPerFrame — a queue-drain budget —
    // and that made a rendering constant decide how fast a sword may swing.
    subs = std::min(std::max(subs, 1), kMaxBrushCaps);

    // R19: every substep that connects becomes a capsule of ONE edit, unioned
    // by a min in the shader. Six separate soft-carves scalloped along their
    // seams (each smin'd against a field the last had already moved), cost six
    // dispatches and six ledger readbacks, and the substep count was capped by
    // an unrelated budget. This is the true swept union, measured once.
    BrickEdit e;
    e.mode = 1; // carve
    e.radius = std::max(swordWorld_.radius * 1.8f, 0.022f);
    e.segment = true;
    e.capCount = 0;
    float exitW[3] = {0.f, 0.f, 0.f};   // world, where the blade last left clay
    float lastNrm[3] = {0.f, 1.f, 0.f};
    for (int si = 1; si <= subs; si++) {
        float a = (float)si / (float)subs;
        float h0[3], t0s[3];
        for (int k = 0; k < 3; k++) {
            h0[k] = pH[k] + (hilt[k] - pH[k]) * a;
            t0s[k] = pT[k] + (tip[k] - pT[k]) * a;
        }
        // the span of THIS instant's blade that lies inside the opponent
        float tIn = 2.f, tOut = -1.f, nrm[3] = {0, 1, 0};
        int hitBone = -1;
        bool haveNrm = false;
        const int kSamples = 20;
        for (int i = 0; i <= kSamples; i++) {
            float t = (float)i / (float)kSamples;
            float p[3] = {h0[0] + (t0s[0] - h0[0]) * t, h0[1] + (t0s[1] - h0[1]) * t,
                          h0[2] + (t0s[2] - h0[2]) * t};
            for (const BoneCapsule& c : capsules_) {
                float a2[3], b2[3], cr;
                posedCapsule(tgt, c, a2, b2, cr);
                float ab[3] = {b2[0] - a2[0], b2[1] - a2[1], b2[2] - a2[2]};
                float ap[3] = {p[0] - a2[0], p[1] - a2[1], p[2] - a2[2]};
                float len2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
                float u = len2 > 1e-9f
                              ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / len2
                              : 0.f;
                u = std::min(std::max(u, 0.f), 1.f);
                float q[3] = {a2[0] + ab[0] * u, a2[1] + ab[1] * u, a2[2] + ab[2] * u};
                float d[3] = {p[0] - q[0], p[1] - q[1], p[2] - q[2]};
                float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (dist >= cr) continue;
                tIn = std::min(tIn, t);
                tOut = std::max(tOut, t);
                if (hitBone < 0) hitBone = c.bone;
                if (!haveNrm) {
                    float inv = dist > 1e-6f ? 1.f / dist : 0.f;
                    nrm[0] = d[0] * inv;
                    nrm[1] = d[1] * inv;
                    nrm[2] = d[2] * inv;
                    haveNrm = true;
                }
                break;
            }
        }
        if (tOut < tIn) continue; // this instant misses

        float dirW[3] = {t0s[0] - h0[0], t0s[1] - h0[1], t0s[2] - h0[2]};
        float bladeLen =
            std::sqrt(dirW[0] * dirW[0] + dirW[1] * dirW[1] + dirW[2] * dirW[2]);
        // M-PHYS: metres of BLADE inside this target. Reported before the
        // bounds test below, for the same reason the fist's is — the blade is
        // in the body whether or not the carve landed. reportContact keeps the
        // deepest of the frame, so the substep loop shoves once, not six times.
        {
            const float inv = sweep > 1e-6f ? 1.f / sweep : 0.f;
            const float sweepDir[3] = {(tip[0] - pT[0]) * inv, (tip[1] - pT[1]) * inv,
                                       (tip[2] - pT[2]) * inv};
            reportContact(0, tgt.vol.slot(), -1, (tOut - tIn) * bladeLen, sweepDir,
                          sweep * 60.f, nrm);
        }
        // push past entry and exit so the cut breaks the surface both sides
        float over = bladeLen > 1e-6f ? (e.radius * 0.9f) / bladeLen : 0.f;
        float t0 = std::max(0.f, tIn - over), t1 = std::min(1.f, tOut + over);
        float wA[3], wB[3];
        for (int k = 0; k < 3; k++) {
            wA[k] = h0[k] + dirW[k] * t0;
            wB[k] = h0[k] + dirW[k] * t1;
        }
        // world -> the TARGET'S REST volume (trap 6: edits are rest space),
        // through the piece the blade actually met
        float invBone[16];
        matIdentity(invBone);
        if (brushRig) {
            if (hitBone >= 0 && (size_t)hitBone < tgt.pieces.size()) {
                matInvAffine(tgt.pieces[hitBone].xform, invBone);
            }
        } else if (hitBone >= 0 && (size_t)(hitBone * 16 + 16) <= tgt.skinMats.size()) {
            matInvAffine(&tgt.skinMats[hitBone * 16], invBone);
        }
        float rA[3], rB[3];
        matTransformPoint(invBone, wA, rA);
        matTransformPoint(invBone, wB, rB);
        // Bounds-tested PER CAPSULE rather than on the finished brush: an
        // edit dropped at the volume boundary (trap 5) must cost only the
        // substep that strayed, not the rest of the swing that was fine.
        // Every capsule passing here is what makes the queueEdit below
        // unconditional, which is what keeps slicePending_ counting exactly
        // the ops that will be measured.
        if (!tgt.vol.capsuleInBounds(rA, rB, e.radius)) continue;
        if (e.capCount == 0) {
            std::memcpy(e.pos, rA, sizeof(rA));
            std::memcpy(e.posB, rB, sizeof(rB));
            e.capCount = 1;
        } else if (e.capCount < kMaxBrushCaps) {
            std::memcpy(e.capA[e.capCount - 1], rA, sizeof(rA));
            std::memcpy(e.capB[e.capCount - 1], rB, sizeof(rB));
            e.capCount++;
        }
        // Each substep overwrites these, so after the loop they name the LAST
        // place the blade was still cutting — which is the exit, and where the
        // slice gob comes from. worldPos is the wound the dribble path spawns
        // at, and it is the same per-substep midpoint it always was.
        for (int k = 0; k < 3; k++) e.worldPos[k] = (wA[k] + wB[k]) * 0.5f;
        std::memcpy(e.outDir, nrm, sizeof(e.outDir)); // gobs spray off the cut
        std::memcpy(exitW, wB, sizeof(exitW));
        std::memcpy(lastNrm, nrm, sizeof(lastNrm));
    }

    if (e.capCount == 0) return; // the whole sweep missed, or fell out of bounds
    e.player = tgt.vol.slot(); // whose clay this is, for the ledger/snapshot
    e.fromWeapon = true; // pool this into the slice gob, don't dribble it
    tgt.vol.queueEdit(e);
    // The gob spawns where the blade LEAVES the body, so latch the tip-side end
    // of the last span inside the opponent rather than the midpoint the edit
    // carries for the dribble path.
    std::memcpy(sliceExit_, exitW, sizeof(sliceExit_));
    std::memcpy(sliceNrm_, lastNrm, sizeof(sliceNrm_));
    // Same as the rupture path: the fighter's clay, not this local edit's
    // fallback — see the note there.
    std::memcpy(sliceCol_, fighters_[clampPlayer(e.player)].vol.bodyColor(),
                sizeof(sliceCol_));
    // blade travel this frame, unit — `sweep` is exactly |tip - pT|
    const float invSweep = sweep > 1e-6f ? 1.f / sweep : 0.f;
    for (int k = 0; k < 3; k++) sliceSweep_[k] = (tip[k] - pT[k]) * invSweep;
    sliceOpen_ = true;
    sliceCutStep_ = true;
    slicePending_++;
    sliceWait_ = 0;

    // R21: the smear. The same capsules one size up, stamped as albedo only —
    // no allocation, no JFA, no ledger — so the cut's lips carry the mark of
    // having been cut. It rides the fused brush for free: the capsules are
    // already in the target's rest space and already bounds-checked, and the
    // wider radius only reaches further into clay that is certainly there.
    if (look.impact.smear > 0.f) {
        BrickEdit s = e;
        s.mode = 4;
        s.fromWeapon = false; // paint is not clay; it must not pool into a gob
        s.radius = e.radius * std::max(look.impact.smearRadius, 1.f);
        s.paint = look.impact.smear;
        std::memcpy(s.color, look.impact.bruiseColor, sizeof(s.color));
        if (tgt.vol.editInBounds(s)) tgt.vol.queueEdit(s);
    }
    };

    for (int ti = 1; ti < kMaxPlayers; ti++) {
        Fighter& tgt = fighters_[ti];
        if (!tgt.enabled || tgt.dead) continue; // nothing left to cut
        // Bail only when there is NO posing data at all. This guard used to
        // read `skinMats.empty()` alone, which is ALWAYS true once the
        // armature is gone — so the sword silently stopped cutting while UI
        // carving still worked, because the return fired before the brush-rig
        // path could ever run.
        if (brushRig ? tgt.pieces.empty() : tgt.skinMats.empty()) continue;
        cut(tgt);
    }
}

// ---- M-DEATH ----
//
// A body that has lost `threshold` of its clay stops holding together: what is
// left of it is handed to the ledger as debt, the eyes come off, and the slot
// goes quiet until it respawns.
//
// Which piece a rest-space point rides once the body is in two. Everything
// that is CARRIED by the body — the eye beads, and the loose beads the
// collapse throws — has to ask this, or it follows the wrong half: the beads
// sit at the crown, and piece 0 is the half below the cut.
const float* Renderer::bodyXformFor(const Fighter& f, const float rest[3]) const {
    if (f.pieces.empty()) return nullptr;
    if (f.bisected && (int)f.pieces.size() > BrickSystem::kBrushPieces &&
        rest[1] >= f.bisectRestY) {
        return f.pieces[BrickSystem::kPiecesPerFighter - 1].xform;
    }
    return f.pieces[0].xform;
}

// ---- B-SEVER: a body cut in two falls as two solids ----
//
// The whole feature is three numbers per half (a translation, a tumble, and a
// pivot) and ONE extra piece. It creates no volume, no brush and no gob: the
// body brush is simply drawn twice, each copy clipped to one side of the cut
// by the rest AABB every piece already carries. brick_read.wgsl computes
// `max(charDistRest(q), boxDist)`, so the sliced face IS the box plane —
// perfectly flat, exactly at the cut, and costing nothing to produce.
//
// The cut plane is AXIS-ALIGNED IN REST SPACE, which is the one approximation
// here and it is deliberate. The flourish sweeps horizontally at chest height,
// so a horizontal plane is what the blade actually made; an oblique cut would
// need the clip to be a half-space rather than a box, and that is a shader
// change for a case the sword does not currently produce.
void Renderer::beginSever(Fighter& f, const float cutWorld[3], const float tip[3],
                          const float hilt[3], const LookParams& look) {
    f.bisectT = std::max(look.death.bisectLinger, 0.f);
    f.sev[0] = Fighter::Severed{};
    f.sev[1] = Fighter::Severed{};
    if (f.pieces.empty()) return;

    // World cut point -> REST, through the body piece's own transform. It has
    // to go through the affine and not just subtract the root: the body is
    // squished, sheared and yawed, so a world height is not a rest height.
    float inv[16];
    matInvAffine(f.pieces[0].xform, inv);
    float rest[3];
    matTransformPoint(inv, cutWorld, rest);
    const float lo = f.pieces[0].lo[1], hi = f.pieces[0].hi[1];
    // Keep both halves non-degenerate: a cut at the very crown produces an
    // empty top piece that still costs a march step at every sample.
    const float pad = (hi - lo) * 0.12f;
    f.bisectRestY = std::min(std::max(rest[1], lo + pad), hi - pad);

    // The blade's own direction decides which way the top half goes, so the
    // half travels the way the sword was travelling rather than in some
    // arbitrary authored direction.
    float dir[3] = {tip[0] - hilt[0], 0.f, tip[2] - hilt[2]};
    float dl = std::sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
    if (dl < 1e-4f) {
        dir[0] = 1.f;
        dir[2] = 0.f;
        dl = 1.f;
    }
    dir[0] /= dl;
    dir[2] /= dl;
    // Push the top half along the blade and tumble it about the horizontal
    // axis perpendicular to that push — which is what topples it onto its cut
    // face, the whole point of the exercise.
    Fighter::Severed& top = f.sev[1];
    top.vel[0] = dir[0] * look.death.bisectPush;
    top.vel[1] = 0.f;
    top.vel[2] = dir[2] * look.death.bisectPush;
    top.axis[0] = -dir[2];
    top.axis[1] = 0.f;
    top.axis[2] = dir[0];
    top.angVel = look.death.bisectSpin;
    // Turn about the cut itself, not the body origin, or the top half swings
    // through the bottom one on its way over.
    std::memcpy(top.pivot, cutWorld, sizeof(top.pivot));

    // The bottom half keeps its feet: it only sags the other way, which reads
    // as the legs buckling rather than as a second projectile.
    Fighter::Severed& bot = f.sev[0];
    bot.axis[0] = dir[2];
    bot.axis[1] = 0.f;
    bot.axis[2] = -dir[0];
    bot.angVel = look.death.bisectSpin * 0.22f;
    std::memcpy(bot.pivot, f.disp.pos, sizeof(bot.pivot));
    bot.pivot[1] = f.disp.pos[1];
}

// Ballistic, then it lies there. Same shape as the loose eyes above, and for
// the same reason: this runs inside the deterministic tick, so no RNG and no
// wall clock — a replay has to cut the same body into the same two halves and
// land them in the same place.
void Renderer::stepSevered(Fighter& f, const LookParams& look, float dt) {
    if (f.pieces.empty()) return;
    const float g = std::max(look.death.bisectGravity, 0.f);
    // How far each half's own centre sits above the body's feet, so a half
    // stops when IT touches the floor rather than when the root does.
    const float lo = f.pieces[0].lo[1], hi = f.pieces[0].hi[1];
    const float mid[2] = {(lo + f.bisectRestY) * 0.5f, (f.bisectRestY + hi) * 0.5f};
    for (int h = 0; h < 2; h++) {
        Fighter::Severed& sv = f.sev[h];
        if (sv.landed) continue;
        sv.vel[1] -= g * dt;
        for (int k = 0; k < 3; k++) sv.pos[k] += sv.vel[k] * dt;
        sv.ang += sv.angVel * dt;
        // Floor contact on the half's own centre height. Approximate on
        // purpose — the halves are tumbling blobs, and a real contact solve
        // would be a rigid-body engine for two objects that exist for two
        // seconds.
        const float restH = mid[h] * 0.45f;
        if (f.disp.pos[1] + mid[h] + sv.pos[1] <= restH) {
            sv.pos[1] = restH - f.disp.pos[1] - mid[h];
            sv.vel[0] *= 0.25f;
            sv.vel[1] = 0.f;
            sv.vel[2] *= 0.25f;
            sv.angVel *= 0.25f;
            if (std::fabs(sv.angVel) < 0.25f) {
                sv.angVel = 0.f;
                sv.landed = true;
            }
        }
    }
}

// Rewrite this frame's pieces as two clipped halves plus the mitts, each
// carrying its half's motion. Called AFTER updateBrushRig, so the rig stays
// one code path and knows nothing about any of this.
void Renderer::applySever(Fighter& f) const {
    if (!f.bisected || f.pieces.size() < BrickSystem::kBrushPieces) return;
    // The fourth slot is the upper half. It is appended only while a body is
    // in two pieces, so a standing fighter still reports three and the march
    // loop does exactly the work it always did (brick.h kBrushPieces).
    if ((int)f.pieces.size() < BrickSystem::kPiecesPerFighter) {
        f.pieces.push_back(f.pieces[0]);
    }
    AffinePiece& bot = f.pieces[0];
    AffinePiece& top = f.pieces[BrickSystem::kPiecesPerFighter - 1];
    top = bot; // same brush, same volume region — only the clip differs
    bot.hi[1] = f.bisectRestY;
    top.lo[1] = f.bisectRestY;

    auto rigid = [](const Fighter::Severed& sv, float out[16]) {
        // T(pos) * T(pivot) * R(axis, ang) * T(-pivot)
        const float c = std::cos(sv.ang), s1 = std::sin(sv.ang), t = 1.f - c;
        const float x = sv.axis[0], y = sv.axis[1], z = sv.axis[2];
        float R[16] = {t * x * x + c,     t * x * y + s1 * z, t * x * z - s1 * y, 0,
                       t * x * y - s1 * z, t * y * y + c,     t * y * z + s1 * x, 0,
                       t * x * z + s1 * y, t * y * z - s1 * x, t * z * z + c,     0,
                       0,                  0,                  0,                 1};
        // column-major, matching matMul/matTransformPoint in anim.cpp
        float pre[16], post[16], tmp[16];
        matIdentity(pre);
        matIdentity(post);
        for (int k = 0; k < 3; k++) {
            pre[12 + k] = -sv.pivot[k];
            post[12 + k] = sv.pivot[k] + sv.pos[k];
        }
        matMul(R, pre, tmp);
        matMul(post, tmp, out);
    };
    float M[16], composed[16];
    rigid(f.sev[0], M);
    matMul(M, bot.xform, composed);
    std::memcpy(bot.xform, composed, sizeof(composed));
    rigid(f.sev[1], M);
    matMul(M, top.xform, composed);
    std::memcpy(top.xform, composed, sizeof(composed));

    // The mitts ride whichever half they were attached to, by their own
    // height. A hand left hanging in the air where the body used to be is the
    // one thing that would give the trick away.
    for (int i = 1; i <= 2 && i < (int)f.pieces.size() - 1; i++) {
        const float cy = (f.pieces[i].lo[1] + f.pieces[i].hi[1]) * 0.5f;
        rigid(f.sev[cy >= f.bisectRestY ? 1 : 0], M);
        matMul(M, f.pieces[i].xform, composed);
        std::memcpy(f.pieces[i].xform, composed, sizeof(composed));
    }
}

// The collapse deliberately spawns NO gobs itself. Adding the remaining mass to
// carved+debt in the same breath leaves `carved == deposited + inFlight + debt`
// true at that instant, and the dribble spawner in updateConservation then
// drains the debt over the next few pose steps — up to twelve blobs at a time,
// which is exactly the "it comes apart" read we want, out of code that already
// exists and is already conservation-correct. A bespoke burst would have been a
// second way to create gobs and a second way to leak clay.
void Renderer::collapseFighter(int i, const LookParams& look) {
    Fighter& f = fighters_[i];
    if (f.dead) return;
    f.dead = true;
    f.respawnT = std::max(look.death.respawn, 0.f);

    // What is still standing, handed to the ledger. Clamped at zero because the
    // mesh volume and the voxel occupancy agree closely but not exactly, and a
    // negative remainder would MINT clay.
    const float remaining = std::max(0.f, fighterVolume_ - f.carved);
    if (look.conserveClay && remaining > 0.f) {
        // It has left the body: on the ledger as carved, and owed. Everything
        // below only MOVES that debt into gobs and splats, so the invariant
        // holds at every line rather than only at the end.
        sploot_.carved += remaining;
        sploot_.debt += remaining;

        // THIS FIGHTER'S clay, not the last wound's. It used to take
        // woundCol_/sliceCol_, which is whatever was cut most recently
        // ANYWHERE — so a green fighter collapsing after the blue one was hit
        // shed 46 litres of blue.
        const float* col = bodyColorFor(look, i);
        const float cx = f.disp.pos[0], cz = f.disp.pos[2];

        // 1. the burst: a few chunky gobs thrown clear
        const int want = std::max(0, look.death.burstGobs);
        const int room = std::max(0, kMaxGobs - (int)gobs_.size());
        const int ng = std::min(want, room);
        float thrown = 0.f;
        if (ng > 0 && look.death.burstFrac > 0.f) {
            const float each = remaining * std::min(look.death.burstFrac, 1.f) / (float)ng;
            for (int k = 0; k < ng && sploot_.debt >= each; k++) {
                Gob g{};
                g.vol = each;
                g.radius = std::cbrt(each * 3.f / (4.f * 3.14159265f));
                // Golden-angle fan, no RNG: this runs inside the sim, and the
                // gob dribble's seeded stream must stay bit-identical for
                // scenes where nothing dies.
                const float a = 2.39996323f * (float)k;
                g.pos[0] = cx + std::cos(a) * 0.10f;
                g.pos[1] = f.disp.pos[1] + 0.30f;
                g.pos[2] = cz + std::sin(a) * 0.10f;
                g.vel[0] = std::cos(a) * look.death.burstSpeed;
                g.vel[1] = look.death.burstLift;
                g.vel[2] = std::sin(a) * look.death.burstSpeed;
                std::memcpy(g.col, col, sizeof(g.col));
                std::memcpy(g.disp, g.pos, sizeof(g.disp));
                g.grace = 0.3f;
                gobs_.push_back(g);
                sploot_.debt -= each;
                thrown += each;
            }
        }

        // 2. the heap: the rest deposited straight onto the ground where it
        // stood, spread over enough splats that none of them stacks a spike.
        float pool = std::min(remaining - thrown, sploot_.debt);
        if (pool > 0.f) {
            const float per = std::max(look.death.splatMax, 1e-4f);
            const int ns = std::min(std::max((int)std::ceil(pool / per), 1), 32);
            const float v = pool / (float)ns;
            for (int k = 0; k < ns; k++) {
                // sunflower spiral: even area coverage, deterministic, and no
                // two points share a radius so the heap has no visible rings
                const float a = 2.39996323f * (float)k;
                const float rr = look.death.splatSpread *
                                 std::sqrt(((float)k + 0.5f) / (float)ns);
                ground_.splat(cx + std::cos(a) * rr, cz + std::sin(a) * rr, v, col);
                sploot_.deposited += v;
                sploot_.debt -= v;
            }
        }
        // Whatever is still owed (the gob array was full, or conserveClay
        // raced) stays as debt and the dribble drains it. Slow, never lost.
    }

    // The eyes outlive the body. Their world positions come from the body
    // piece, which is still holding this frame's transform — after this the
    // fighter stops being packed at all, so it has to happen here.
    for (const MarbleProp& m : marbles_) {
        if ((int)loose_.size() >= kMaxMarbles) break;
        LooseMarble lm;
        float p[3] = {m.pos[0], m.pos[1], m.pos[2]};
        if (const float* bx = bodyXformFor(f, m.pos)) matTransformPoint(bx, m.pos, p);
        std::memcpy(lm.pos, p, sizeof(lm.pos));
        std::memcpy(lm.disp, p, sizeof(lm.disp));
        lm.radius = m.radius;
        std::memcpy(lm.col, m.color, sizeof(lm.col));
        // Kick outward from the body's axis so the pair scatters instead of
        // dropping straight down in a neat little stack. Direction comes from
        // the bead's own offset — no RNG, because this runs inside the sim.
        float d[3] = {p[0] - f.disp.pos[0], 0.f, p[2] - f.disp.pos[2]};
        float dl = std::sqrt(d[0] * d[0] + d[2] * d[2]);
        if (dl < 1e-3f) {
            d[0] = 1.f;
            d[2] = 0.f;
            dl = 1.f;
        }
        lm.vel[0] = d[0] / dl * look.death.eyeSpeed;
        lm.vel[1] = look.death.eyeLift;
        lm.vel[2] = d[2] / dl * look.death.eyeSpeed;
        loose_.push_back(lm);
    }

    std::printf("[death] player %d collapsed%s (%.0f of %.0f ml carved), %.0f ml "
                "sploots, respawn in %.1fs\n",
                i, f.bisected ? " BISECTED" : "", f.carved * 1e6f,
                fighterVolume_ * 1e6f, remaining * 1e6f, f.respawnT);
}

void Renderer::respawnFighter(int i, const LookParams& look) {
    Fighter& f = fighters_[i];
    // A full re-import rebuilds the volume from the mesh — encodeImport clears
    // the indirection grid and the allocator first, so a body minced to lace
    // comes back whole rather than accumulating the old holes.
    f.vol.requestBake();
    f.carved = 0.f;
    f.dead = false;
    f.bisected = false; // or it dies again on its first frame back
    f.bisectT = 0.f;
    f.sev[0] = Fighter::Severed{};
    f.sev[1] = Fighter::Severed{};
    f.sevBase.clear(); // the frozen pose goes with the old body
    f.respawnT = 0.f;

    // Come back at a distance. An opponent reappears on the far side of the
    // arena from the hero — respawning inside arm's reach is a free hit for
    // whoever just killed it — and the hero, having no one to be far from,
    // comes back at the middle.
    const float r = std::max(look.death.spawnRadius, 0.f);
    if (i == 0) {
        f.pose.pos[0] = 0.f;
        f.pose.pos[2] = 0.f;
    } else {
        const float hx = fighters_[0].pose.pos[0], hz = fighters_[0].pose.pos[2];
        const float hl = std::sqrt(hx * hx + hz * hz);
        // Directly opposite the hero as seen from the centre; if the hero IS
        // at the centre there is no "opposite", so take a fixed bearing.
        const float ux = hl > 1e-3f ? -hx / hl : 1.f;
        const float uz = hl > 1e-3f ? -hz / hl : 0.f;
        f.pose.pos[0] = ux * r;
        f.pose.pos[2] = uz * r;
        f.pose.yaw = std::atan2(hx - f.pose.pos[0], hz - f.pose.pos[2]);
    }
    f.pose.pos[1] = 0.f;
    f.pose.lean = 0.f;
    f.pose.moving = false;
    f.pose.guard = false;
    f.pose.punch = 0.f;
    f.disp = f.pose;
    // The hopper itself is the sim's (Body::stepHop) and is reset there with
    // the rest of the body's velocities; all that resets here is the DISPLAY
    // latch, so a corpse's last squash does not draw for one pose step on the
    // fighter that replaces it.
    f.hopLatch = 0.f;
    f.pose.hopU = 0.f;
    f.disp.hopU = 0.f;
    // The mitts are somewhere else entirely now; a stale "where the fist was"
    // would read that teleport as a strike (updatePunchCut's own guard would
    // catch it, but relying on a magnitude threshold for a known discontinuity
    // is how the blade got its teleport bug in the first place).
    f.haveHandPrev = false;
    haveBlade_ = false;
    respawned_[i] = true;
    std::printf("[death] player %d respawned at (%.2f, %.2f)\n", i, f.pose.pos[0],
                f.pose.pos[2]);
}

// M-MASS: what is LEFT of a fighter, as a fraction of the clay it imported
// with. The damage side of this is M-DEATH's own number — `carved`, billed off
// the same measurements that feed the arena ledger — so a body's weight and its
// death threshold can never disagree about how hurt it is.
//
// 1 when there is nothing to be a fraction of (the analytic blob has no mesh
// volume) and 1 for a corpse, which has no body to shove.
const float* Renderer::bodyColorFor(const LookParams& look, int player) {
    const int p = player < 0 ? 0 : (player > 3 ? 3 : player);
    return look.bodyColor[p];
}

float Renderer::massFrac(int i) const {
    if (i < 0 || i >= kMaxPlayers || fighterVolume_ <= 0.f) return 1.f;
    const Fighter& f = fighters_[i];
    if (f.dead) return 1.f;
    const float lost = std::min(std::max(f.carved / fighterVolume_, 0.f), 1.f);
    return 1.f - lost;
}

void Renderer::updateDeaths(const LookParams& look, const FrameInfo& frame, float dt) {
    (void)frame;
    if (!look.death.enabled || fighterVolume_ <= 0.f) return;
    const float thresh = std::min(std::max(look.death.threshold, 0.05f), 1.f);
    for (int i = 0; i < kMaxPlayers; i++) {
        Fighter& f = fighters_[i];
        if (!playerEnabled(i)) continue;
        if (f.dead) {
            f.respawnT -= dt;
            if (f.respawnT <= 0.f) respawnFighter(i, look);
            continue;
        }
        // Two ways to die, one collapse. A bisection skips the ledger test
        // entirely — that is the point of it — but everything downstream is the
        // same path, so the remaining mass is conserved identically whether a
        // fighter went at 50% or at 2%.
        // A bisected body does not sploot on the frame it was cut: it falls in
        // two solid halves first, and only when that has played out does the
        // ordinary collapse run. Conservation does not care about the delay —
        // nothing has left the body yet, so the ledger just resolves later.
        if (f.bisected && look.death.bisectFall && f.bisectT > 0.f) {
            f.bisectT -= dt;
            stepSevered(f, look, dt);
            continue;
        }
        if (f.bisected || f.carved >= fighterVolume_ * thresh) {
            collapseFighter(i, look);
        }
    }
}

// Loose eyes: the only rigid bodies in the game, and they exist for about two
// seconds after a fighter comes apart.
//
// Ballistic, then rolling. The floor is taken as the flat arena plane rather
// than the pebble field — a bead is 2 cm across and the pebbles are a 4 cm
// mosaic, so bouncing off the real height field would make them jitter on
// contact for a visual difference nobody can see at this size.
void Renderer::updateLooseMarbles(const LookParams& look, const FrameInfo& frame,
                                  float dt, bool poseStep) {
    if (loose_.empty()) return;
    const float kGravity = 9.81f;
    const float kFloor = 0.f;
    for (size_t i = 0; i < loose_.size();) {
        LooseMarble& m = loose_[i];
        m.vel[1] -= kGravity * dt;
        for (int k = 0; k < 3; k++) m.pos[k] += m.vel[k] * dt;
        const float rest = kFloor + m.radius;
        if (m.pos[1] <= rest) {
            m.pos[1] = rest;
            if (m.vel[1] < 0.f) m.vel[1] = -m.vel[1] * look.death.eyeBounce;
            if (m.vel[1] < 0.35f) m.vel[1] = 0.f; // stop micro-bouncing
            // rolling friction, applied to the horizontal only
            float hs = std::sqrt(m.vel[0] * m.vel[0] + m.vel[2] * m.vel[2]);
            const float drop = look.death.eyeRoll * dt;
            if (hs <= drop) {
                m.vel[0] = 0.f;
                m.vel[2] = 0.f;
                hs = 0.f;
            } else if (hs > 1e-5f) {
                const float s = (hs - drop) / hs;
                m.vel[0] *= s;
                m.vel[2] *= s;
                hs -= drop;
            }
            // Settled and still: retire it. Eyes are litter, and litter that
            // never expires is a slow leak of uniform slots that the NEXT
            // collapse needs.
            if (hs < 0.02f && m.vel[1] == 0.f) {
                m.rest += dt;
                if (m.rest > 6.f) {
                    loose_.erase(loose_.begin() + (long)i);
                    continue;
                }
            } else {
                m.rest = 0.f;
            }
        }
        // trap 4: it MOVES, so what is drawn steps on the 12 Hz pose grid even
        // though it simulates at frame rate — same rule the gobs obey.
        if (poseStep) std::memcpy(m.disp, m.pos, sizeof(m.disp));
        i++;
    }
    (void)frame;
}

void Renderer::updateConservation(const LookParams& look, const FrameInfo& frame) {
    absorbMeasured();
    // conservation off = pre-M4.6 vanish behavior: stop spawning and shed the
    // debt (so a later re-enable doesn't erupt a backlog of gobs at once)
    if (!look.conserveClay) {
        sploot_.debt = 0.f;
        haveWound_ = false;
        // the reservation is a claim on debt that no longer exists
        sliceVol_ = 0.f;
        sliceOpen_ = false;
        sliceWait_ = 0;
    }

    float dt = 0.f;
    if (lastSimTime_ >= 0.f)
        dt = std::min(std::max(frame.time - lastSimTime_, 0.f), 0.05f);
    lastSimTime_ = frame.time;
    bool poseStep = frame.poseTime != lastPoseTime_;
    lastPoseTime_ = frame.poseTime;

    // R22: the mitts' deflection bleeds back toward the pose the rig wants.
    // Runs before updateBrushRig below, so a hand that is no longer touching
    // anything is already on its way home this frame.
    //
    // LINEAR, in m/s, like Body::knock and for the same reason: an exponential
    // return asymptotes and never quite arrives, and a mitt that never quite
    // comes back is a fighter whose idle pose drifts for the rest of the match.
    // It is deliberately NOT quantized to the pose grid — the punch arc it
    // rides is not either (a strike is carved between consecutive frames, so a
    // display that jumped 12 times a second would cut in stripes).
    if (dt > 0.f) {
        const float drop = std::max(look.impact.glanceDecay, 0.f) * dt;
        for (Fighter& f : fighters_) {
            for (int side = 0; side < 2; side++) {
                float* g = f.glance[side];
                const float len =
                    std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
                if (len <= drop || len < 1e-6f) {
                    g[0] = g[1] = g[2] = 0.f;
                } else {
                    const float sc = (len - drop) / len;
                    for (int k = 0; k < 3; k++) g[k] *= sc;
                }
            }
        }
    }

    // M-DEATH, and it must run HERE — after absorbMeasured, before the dribble.
    // A collapse works by handing the remaining mass to `debt`, so it has to
    // see this frame's measurements (or it kills on stale damage) and the
    // dribble below has to see the debt it just added (or the body vanishes a
    // frame before anything falls out of it).
    updateDeaths(look, frame, dt);
    updateLooseMarbles(look, frame, dt, poseStep);

    // spawn gobs while the ledger owes clay: 2..35 ml each (r ~ 8..20 mm)
    const float kMinGob = 2.0e-6f, kMaxGob = 3.5e-5f;
    auto rnd = [this]() {
        gobSeed_ = gobSeed_ * 1664525u + 1013904223u;
        return (float)(gobSeed_ >> 8) * (1.f / 16777216.f);
    };
    // ---- sword slice: ONE gob carrying the whole cut ----
    // The dribble below is right for a brush (many small taps), and wrong for
    // a blade: a slice that takes 300 ml should throw a 41 mm blob, not 12
    // pellets. So blade volume is WITHHELD from the dribble while the cut runs
    // and leaves in a single gob when the blade exits.
    if (!look.sword.sliceGob) {
        // toggled off mid-slice: release the hold, the debt dribbles as before
        sliceVol_ = 0.f;
        sliceOpen_ = false;
        sliceWait_ = 0;
    }
    // The reservation can never exceed the debt it claims — a gob re-stick
    // reconciling smin over-fill can push debt down underneath it.
    if (sliceVol_ > sploot_.debt) sliceVol_ = sploot_.debt;

    const bool cutStep = sliceCutStep_;
    if (poseStep) sliceCutStep_ = false;
    // A slice ENDS at the first pose step that queued no blade edit: the blade
    // left the body, or dropped under the cutting speed, or the swing stopped.
    // Watching for the absence of edits (rather than for a swing state) is what
    // makes an INTERRUPTED swing behave — nothing has to signal an ending.
    if (poseStep && sliceOpen_ && !cutStep) {
        sliceWait_++;
        // Wait for the measurements too, or the "whole" slice would be only
        // the part the GPU had reported by then (they land 1-2 frames late).
        // But bound the wait: pollVolumes DROPS a measurement whose volume
        // generation a snapshot load replaced, so slicePending_ can stall
        // forever. After kSliceWait pose steps, eject what we have.
        const int kSliceWait = 8; // ~0.67 s at 12 Hz
        const bool timedOut = sliceWait_ >= kSliceWait;
        if (slicePending_ <= 0 || timedOut) {
            bool done = true;
            if (sliceVol_ > kMinGob && (int)gobs_.size() < kMaxGobs) {
                float v = std::min(sliceVol_, sploot_.debt);
                sploot_.debt -= v;
                Gob g{};
                g.vol = v;
                g.radius = std::cbrt(v * 3.f / (4.f * 3.14159265f));
                // Launch: mostly the blade's own sweep (clay thrown off a
                // moving edge), part straight out of the wound so it clears
                // the body. NO random spray — one blob reads as a thing that
                // was cut off, and skipping rnd() leaves the dribble's seeded
                // stream bit-identical for scenes with no sword.
                float f = std::min(std::max(look.sword.sliceOut, 0.f), 1.f);
                float d[3], len = 0.f;
                for (int i = 0; i < 3; i++) {
                    d[i] = sliceSweep_[i] * (1.f - f) + sliceNrm_[i] * f;
                    len += d[i] * d[i];
                }
                len = std::sqrt(len);
                if (len > 1e-5f) {
                    for (int i = 0; i < 3; i++) d[i] /= len;
                } else {
                    d[0] = sliceNrm_[0];
                    d[1] = sliceNrm_[1];
                    d[2] = sliceNrm_[2];
                }
                for (int i = 0; i < 3; i++) {
                    // spawn at the EXIT end of the cut, pushed clear along the
                    // wound normal by its own radius (the dribble's rule, and
                    // a 41 mm blob needs it more than an 8 mm one)
                    g.pos[i] = sliceExit_[i] + sliceNrm_[i] * (g.radius + 0.02f);
                    g.vel[i] = d[i] * look.sword.sliceSpeed;
                }
                g.vel[1] += look.sword.sliceLift;
                std::memcpy(g.col, sliceCol_, sizeof(g.col));
                std::memcpy(g.disp, g.pos, sizeof(g.disp));
                // bigger blob, longer clearance before body collision arms
                g.grace = 0.3f;
                gobs_.push_back(g);
            } else if (sliceVol_ > kMinGob && !timedOut) {
                done = false; // array full — hold the reservation, retry next step
            }
            if (done) {
                // Closing does NOT move volume: anything not ejected is still
                // sploot_.debt and the dribble drains it. That is the whole
                // safety story — a slice can fail to fire, never leak.
                sliceVol_ = 0.f;
                sliceOpen_ = false;
                sliceWait_ = 0;
                slicePending_ = 0;
            }
        }
    }
    // Debt still owed to an open slice is invisible to the dribble, so a brush
    // carve during a swing still dribbles normally off its own volume.
    const float sliceHold = sliceOpen_ ? sliceVol_ : 0.f;

    // Spawning changes the in-flight count, which the tracer reads, so it
    // belongs on the pose grid too — otherwise the array refills at 60 Hz as
    // fast as landings drain it and reuse never recovers.
    while (poseStep && haveWound_ && sploot_.debt - sliceHold > kMinGob &&
           (int)gobs_.size() < 12) {
        float v = std::min(sploot_.debt - sliceHold, kMaxGob);
        sploot_.debt -= v;
        Gob g{};
        g.vol = v;
        g.radius = std::cbrt(v * 3.f / (4.f * 3.14159265f));
        for (int i = 0; i < 3; i++)
            g.pos[i] = lastWound_[i] + woundDir_[i] * (g.radius + 0.02f);
        float s = 0.5f + 0.45f * rnd();
        g.vel[0] = woundDir_[0] * s + (rnd() - 0.5f) * 0.35f;
        g.vel[1] = woundDir_[1] * s + 0.5f + (rnd() - 0.5f) * 0.2f;
        g.vel[2] = woundDir_[2] * s + (rnd() - 0.5f) * 0.35f;
        std::memcpy(g.col, woundCol_, sizeof(g.col));
        std::memcpy(g.disp, g.pos, sizeof(g.disp));
        g.grace = 0.22f; // clear the wound before body collision arms
        gobs_.push_back(g);
    }

    // posed shadow capsules double as gob colliders
    struct PosedCap {
        float a[3], b[3], r;
        int bone; // inverts back to rest space when a gob sticks here
    };
    const bool brushRig = skeletonFree();
    std::vector<PosedCap> caps;
    caps.reserve(capsules_.size());
    for (const BoneCapsule& c : capsules_) {
        PosedCap pc;
        std::memcpy(pc.a, c.a, sizeof(pc.a));
        std::memcpy(pc.b, c.b, sizeof(pc.b));
        pc.r = c.r;
        pc.bone = c.bone;
        // Gobs fly in WORLD space, so the colliders have to be posed. Under
        // the brush rig `bone` is a PIECE index and the pose comes from that
        // piece's transform; refit from its current brush box for the same
        // reason packUniforms does (a grab hand's box lives 0.98 m away).
        if (brushRig) {
            if (c.bone >= 0 && (size_t)c.bone < fighters_[0].pieces.size()) {
                const AffinePiece& ap = fighters_[0].pieces[c.bone];
                float ca[3], cb[3];
                capsuleFromBox(ap.lo, ap.hi, ca, cb, pc.r);
                matTransformPoint(ap.xform, ca, pc.a);
                matTransformPoint(ap.xform, cb, pc.b);
            }
        } else if (c.bone >= 0 && (size_t)(c.bone * 16 + 16) <= fighters_[0].skinMats.size()) {
            matTransformPoint(&fighters_[0].skinMats[c.bone * 16], c.a, pc.a);
            matTransformPoint(&fighters_[0].skinMats[c.bone * 16], c.b, pc.b);
        }
        caps.push_back(pc);
    }
    // Sticking writes rest-space voxels. The bone rig could only do that on an
    // un-posed body; the brush rig CAN invert its piece transforms exactly
    // (they are affine and non-degenerate), so a posed fighter still catches
    // gobs — which is why `bones_.empty()` must not be read as "un-posed" here.
    // That reading is precisely CLAUDE.md trap 6: it would deposit clay at a
    // world position used as a rest address, and a walked-away fighter would
    // grow wounds in the wrong place (or silently drop them, trap 5).
    bool resting = brushRig ? true : (!look.animPlay || bones_.empty());

    for (auto it = gobs_.begin(); it != gobs_.end();) {
        Gob& g = *it;
        g.grace -= dt;
        g.vel[1] -= 5.5f * dt; // hair under g: reads better at tabletop scale
        for (int i = 0; i < 3; i++) g.pos[i] += g.vel[i] * dt;

        // back wall: clay doesn't bounce, it smears down
        if (g.pos[2] - g.radius < -2.3f + 0.10f) {
            g.pos[2] = -2.3f + 0.10f + g.radius;
            if (g.vel[2] < 0.f) g.vel[2] = 0.f;
        }

        bool gone = false;
        if (g.grace <= 0.f) {
            for (const PosedCap& pc : caps) {
                float ab[3] = {pc.b[0] - pc.a[0], pc.b[1] - pc.a[1], pc.b[2] - pc.a[2]};
                float ap[3] = {g.pos[0] - pc.a[0], g.pos[1] - pc.a[1],
                               g.pos[2] - pc.a[2]};
                float abLen2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
                float t = abLen2 > 1e-9f ? (ap[0] * ab[0] + ap[1] * ab[1] +
                                            ap[2] * ab[2]) / abLen2
                                         : 0.f;
                t = std::min(std::max(t, 0.f), 1.f);
                float cp[3] = {pc.a[0] + ab[0] * t, pc.a[1] + ab[1] * t,
                               pc.a[2] + ab[2] * t};
                float d[3] = {g.pos[0] - cp[0], g.pos[1] - cp[1], g.pos[2] - cp[2]};
                float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (dist >= g.radius + pc.r) continue;
                // same pose-grid rule as the floor landing below: a deposit
                // queues an edit, which bumps a volume generation
                if (resting && poseStep) {
                    BrickEdit e;
                    e.mode = 2;
                    e.fromGob = true;
                    e.gobVol = g.vol;
                    e.radius = g.radius * 1.1f;
                    // the gob collided in WORLD space against posed capsules,
                    // but it has to be deposited in the volume's rest space:
                    // invert the skin matrix of the bone it landed on
                    std::memcpy(e.worldPos, g.pos, sizeof(e.worldPos));
                    float rest[3] = {g.pos[0], g.pos[1], g.pos[2]};
                    if (brushRig) {
                        // world -> the BRUSH region this piece samples, which
                        // is the volume address the edit must carry (trap 6).
                        if (pc.bone >= 0 && (size_t)pc.bone < fighters_[0].pieces.size()) {
                            float inv[16];
                            matInvAffine(fighters_[0].pieces[pc.bone].xform, inv);
                            matTransformPoint(inv, g.pos, rest);
                        }
                    } else if (pc.bone >= 0 &&
                               (size_t)(pc.bone * 16 + 16) <= fighters_[0].skinMats.size()) {
                        float inv[16];
                        matInvAffine(&fighters_[0].skinMats[pc.bone * 16], inv);
                        matTransformPoint(inv, g.pos, rest);
                    }
                    std::memcpy(e.pos, rest, sizeof(e.pos));
                    std::memcpy(e.color, g.col, sizeof(e.color));
                    if (fighters_[0].vol.editInBounds(e)) {
                        fighters_[0].vol.queueEdit(e);
                        gone = true; // ledger settles when its measurement lands
                        break;
                    }
                }
                // deflect: push out, kill the normal velocity, slide off
                float inv = dist > 1e-6f ? 1.f / dist : 0.f;
                float n[3] = {d[0] * inv, d[1] * inv, d[2] * inv};
                float push = g.radius + pc.r - dist;
                for (int i = 0; i < 3; i++) g.pos[i] += n[i] * push;
                float vn = g.vel[0] * n[0] + g.vel[1] * n[1] + g.vel[2] * n[2];
                if (vn < 0.f)
                    for (int i = 0; i < 3; i++) g.vel[i] -= n[i] * vn;
            }
        }
        if (gone) {
            it = gobs_.erase(it);
            continue;
        }

        // Landing: floor mosaic or an existing pile (coarse CPU mirror).
        // ON POSE STEPS ONLY, like every other visible event. The gob FLIES at
        // 60 Hz — trajectories need it — but touching down splats the ground
        // and drops the gob from the array, and both are traced inputs. Landing
        // whenever a gob happened to cross the floor made the ground clay
        // generation and the in-flight count change at frame rate, so the ~2 s
        // of clay raining down after a hit re-traced EVERY frame: 60 traces a
        // second instead of 12, right after the hit that most wants the frames.
        // Deferring costs nothing visually — the drawn position (g.disp) is
        // already frozen between pose steps, so a gob that dips under the floor
        // mid-step was never drawn there.
        float top = ground_.approxTopAt(g.pos[0], g.pos[2]);
        if (poseStep && ((g.pos[1] - 0.4f * g.radius < top && g.vel[1] <= 0.f) ||
                         g.pos[1] < -0.1f)) {
            ground_.splat(g.pos[0], g.pos[2], g.vol, g.col);
            sploot_.deposited += g.vol;
            it = gobs_.erase(it);
            continue;
        }
        if (poseStep) std::memcpy(g.disp, g.pos, sizeof(g.disp));
        ++it;
    }

    sploot_.inFlight = 0.f;
    for (const Gob& g : gobs_) sploot_.inFlight += g.vol;
    sploot_.gobs = (int)gobs_.size();
}

void Renderer::render(const OrbitCamera& cam, const LookParams& look,
                      const FrameInfo& frame, wgpu::TextureView swapchainView,
                      const std::function<void(wgpu::RenderPassEncoder&)>& uiCallback) {
    // trap 4 applied to the ROOT: latch the sim pose onto the 12 Hz grid and
    // draw THAT. Everything below (sword, skeleton root, capsules, uniforms)
    // reads the latched pose, so the whole fighter steps together instead of
    // a 12 Hz body sliding on a 60 Hz root.
    if (!look.motion.stepRoot) {
        // 60 Hz slide (pre-M-PERF behaviour)
        for (Fighter& f : fighters_) f.disp = f.pose;
    } else if (frame.poseTime != dispPoseTime_) {
        dispPoseTime_ = frame.poseTime;
        for (Fighter& f : fighters_) f.disp = f.pose;
    }

    // M-SPRING: LATCH the hopper onto the pose grid (trap 4). The sim already
    // integrated it this tick — it has to, since travel depends on it — so
    // there is nothing to advance here, only a sample to take. Taking it is
    // what keeps a traced uniform from moving at 60 Hz: a breath rings the
    // spring for ~3 s, and drawing that ring at frame rate would make a
    // STANDING fighter re-trace every frame and hand back the idle frame rate
    // reuse buys.
    //
    // The write-back is OUTSIDE the pose-step guard on purpose. `f.disp` is
    // overwritten wholesale every frame while motion.stepRoot is off (just
    // above), so a latch applied only on pose steps would be clobbered on the
    // next four frames and the height would slide at 60 Hz after all.
    const bool affine = affineOn(look);
    if (affine && frame.poseTime != springPoseTime_) {
        springPoseTime_ = frame.poseTime;
        for (Fighter& f : fighters_) f.hopLatch = f.pose.hopU;
    }
    for (Fighter& f : fighters_) f.disp.hopU = f.hopLatch;

    // pose the skeleton at the quantized clock; paused = rest pose
    resolveSword(look);

    // ---- M-RIG: drive the brush rig ----
    // The pose index is DISCRETE and latched on the 12 Hz pose grid, exactly
    // like everything else that moves (trap 4). Holding the sword selects the
    // grab brush; letting go selects the rest brush. There is deliberately no
    // transition: interpolating between the two would mean blending two SDF
    // regions per sample, which is the per-sample blending this whole rig
    // exists to delete, and a hand that eases into a grip does not read as
    // stop-motion — it should pop.
    if (skeletonFree()) {
        const bool holding = swordWorld_.enabled && look.hands.ik;
        if (frame.poseTime != handPoseTime_) {
            handPoseTime_ = frame.poseTime;
            for (int i = 0; i < kMaxPlayers; i++) {
                Fighter& f = fighters_[i];
                // Only player 0 carries the sword, so only player 0 grips.
                // Everyone else — and the hero with the sword put down —
                // chooses between the loose idle hand and a closed fist by
                // whether they are squaring up to someone.
                int pose = kHandIdle;
                if (i == 0 && holding) {
                    pose = kHandGrab;
                } else if (f.disp.guard || f.disp.punch > 0.f) {
                    pose = kHandFist;
                }
                f.handPose[0] = f.handPose[1] = pose;
            }
        }
        for (int i = 0; i < kMaxPlayers; i++) {
            Fighter& f = fighters_[i];
            if (i > 0 && !f.enabled) continue;
            const SwordParams* grip =
                (i == 0 && swordWorld_.enabled && look.hands.ik) ? &swordWorld_
                                                                 : nullptr;
            updateBrushRig(f.pieces, f.handPose, f.disp, look,
                           grip, frame.poseTime, f.glance);
            // After the rig, never inside it: a cut body is the rig's output
            // with a rigid motion laid over it, so the rig stays one path.
            if (f.bisected && look.death.bisectFall) applySever(f);
        }
    }

    if (!bones_.empty()) {
        if (affine) {
            // ONE matrix for the whole body. evalPose with a null clip yields
            // identity per bone (the rest pose by construction), so
            // premultiplying the affine leaves every body bone holding exactly
            // A — and the blob, the eye beads, the shadow capsules and the COM
            // all ride the same deformation with no further plumbing. The hand
            // IK below then lifts the two mitt subtrees off it.
            //
            // The 'bounce'/'idle' clips are simply not sampled here. They stay
            // in the asset and still drive the 13-piece path (affineRig off),
            // which is what keeps the A/B comparing like with like; the shape
            // they described — squash, spring, hop, lean — is what the affine
            // reproduces procedurally, at three pieces instead of thirteen.
            evalPose(bones_, nullptr, 0.f, fighters_[0].skinMats);
            float A[16];
            bodyAffine(fighters_[0].disp, look.rig, A);
            for (size_t b = 0; b * 16 + 16 <= fighters_[0].skinMats.size(); b++) {
                float tmp[16];
                matMul(A, &fighters_[0].skinMats[b * 16], tmp);
                std::memcpy(&fighters_[0].skinMats[b * 16], tmp, sizeof(tmp));
            }
        } else {
        // locomotion picks the clip: bounce while travelling, idle at rest.
        // Falls back to the first clip when the asset has neither name.
        int ci = -1;
        if (look.animPlay && !clips_.empty()) {
            ci = clipIndex(fighters_[0].disp.moving ? "bounce" : "idle");
            if (ci < 0) ci = 0;
        }
        const AnimClip* clip = (ci >= 0 && clips_[ci].duration > 0.f) ? &clips_[ci] : nullptr;
        if (clip) {
            fighters_[0].animT = std::fmod(frame.poseTime * look.animSpeed, clip->duration);
        }
        evalPose(bones_, clip, fighters_[0].animT, fighters_[0].skinMats);

        // root: lean about world X (before yaw), then yaw, then translate.
        // Applied to every skin matrix, so the clip stays authored in
        // character space and everything downstream is already world.
        {
            const float cy = std::cos(fighters_[0].disp.yaw), sy = std::sin(fighters_[0].disp.yaw);
            const float cl = std::cos(fighters_[0].disp.lean), sl = std::sin(fighters_[0].disp.lean);
            // R = yaw(Y) * lean(X), column-major
            float R[16] = {cy,        0.f, -sy,       0.f,
                           sy * sl,   cl,  cy * sl,   0.f,
                           sy * cl,   -sl, cy * cl,   0.f,
                           fighters_[0].disp.pos[0], fighters_[0].disp.pos[1], fighters_[0].disp.pos[2], 1.f};
            for (size_t b = 0; b * 16 + 16 <= fighters_[0].skinMats.size(); b++) {
                float tmp[16];
                matMul(R, &fighters_[0].skinMats[b * 16], tmp);
                std::memcpy(&fighters_[0].skinMats[b * 16], tmp, sizeof(tmp));
            }
        }
        }

        // M4.7: sword is master, hands follow. The mitts are detached, so IK
        // does not bend a chain — it teleports each hand onto its grip, held
        // back only by the reach ball about the blob's centre of mass. The
        // body stays on the clip. Marbles/capsules below read the IK'd
        // fighters_[0].skinMats.
        if (swordWorld_.enabled && look.hands.ik && !handIk_.empty()) {
            float hilt[3], tip[3], gA[3], gB[3];
            swordGeometry(swordWorld_, hilt, tip, gA, gB);
            float blade[3] = {tip[0] - hilt[0], tip[1] - hilt[1], tip[2] - hilt[2]};
            float bl = std::sqrt(blade[0] * blade[0] + blade[1] * blade[1] +
                                 blade[2] * blade[2]);
            if (bl > 1e-6f) { blade[0] /= bl; blade[1] /= bl; blade[2] /= bl; }
            // sideways axis for the two-handed grip: the fighter's own +X,
            // NOT cross(blade, up) — the sword rests in a VERTICAL guard, so
            // a blade-derived lateral axis degenerates exactly in the pose the
            // hands spend most of their time in.
            const float cyG = std::cos(fighters_[0].disp.yaw), syG = std::sin(fighters_[0].disp.yaw);
            const float rightW[3] = {cyG, 0.f, -syG};
            for (HandIkChain& c : handIk_) {
                const std::string& nm = bones_[c.wrist].name;
                bool right = nm.size() >= 2 && nm.compare(nm.size() - 2, 2, ".r") == 0;
                const float* g = right ? gA : gB;
                // offset each palm onto its own face of the blade
                const float off = look.hands.gripSpread * swordWorld_.radius *
                                  (right ? 1.f : -1.f);
                for (int k = 0; k < 3; k++) c.target[k] = g[k] + rightW[k] * off;
                // fingers run along the blade, so both mitts stack on the
                // handle the way two hands on a hilt actually sit
                std::memcpy(c.aim, blade, sizeof(c.aim));
                c.roll = look.hands.gripRoll;
                // Fingers never curl, and that is not a TODO.
                //
                // A `hands.gripCurl` used to feed this, mirrored per hand. It
                // had to be ZERO under the affine rig: the mitt is one piece
                // there, sampled through a single transform, so a per-digit
                // rotation would move the shadow capsules and the COM to a
                // grip the clay never adopts — a curled proxy over straight
                // fingers. Since the affine rig is the only rig, the slider
                // was always multiplied out to nothing, so it is gone.
                // Getting curl back is what a discrete grip SHAPE is for (the
                // grab brush already does exactly this), not a joint rotation.
                c.curl = 0.f;
                c.palmLen = c.restSpan * look.hands.palmFrac;
            }
            float com[3];
            evalBodyCom(bodyCom_, fighters_[0].skinMats, com);
            float reach = look.hands.reach > 0.f
                              ? look.hands.reach
                              : autoReach_ * look.hands.reachScale;
            applyHandIk(bones_, handIk_, com, reach, look.hands.orient, fighters_[0].skinMats);
        }

        // gaze last: it only touches the eye leaves, and it wants the head
        // wherever the clip finally left it. The camera is re-sampled on the
        // pose grid only (lastPoseTime_ is still the PREVIOUS step here —
        // updateConservation advances it below), so the eyes step at 12 Hz
        // with the rest of the character instead of gliding at frame rate.
        if (look.gaze.track && !gaze_.empty()) {
            if (frame.poseTime != lastPoseTime_) {
                Vec3 cp = cam.pos();
                gazeTarget_[0] = cp.x; gazeTarget_[1] = cp.y; gazeTarget_[2] = cp.z;
            }
            applyGaze(bones_, gaze_, gazeTarget_, look.gaze.maxAngle, fighters_[0].skinMats);
        }
    }

    // Every opponent poses ITSELF: its own clip clock, its own skeleton, its
    // own root. They stand and idle rather than following the hero's
    // animation. Rigged assets only — with the brush rig there is no skeleton
    // and updateBrushRig above has already done the work.
    for (int i = 1; i < kMaxPlayers && !bones_.empty(); i++) {
        Fighter& f = fighters_[i];
        if (!f.enabled) continue;
        if (affine) {
            // same collapse as the hero, driven by ITS pose and ITS hopper
            evalPose(bones_, nullptr, 0.f, f.skinMats);
            float A[16];
            bodyAffine(f.disp, look.rig, A);
            for (size_t b = 0; b * 16 + 16 <= f.skinMats.size(); b++) {
                float tmp[16];
                matMul(A, &f.skinMats[b * 16], tmp);
                std::memcpy(&f.skinMats[b * 16], tmp, sizeof(tmp));
            }
            continue;
        }
        int ci = clipIndex(f.disp.moving ? "bounce" : "idle");
        if (ci < 0) ci = 0;
        const AnimClip* clip =
            (look.animPlay && !clips_.empty() && clips_[ci].duration > 0.f)
                ? &clips_[ci]
                : nullptr;
        if (clip) {
            // Offset each fighter's phase so identical bodies don't breathe in
            // lockstep — that reads as one puppet duplicated, not N actors.
            // Scaled by index so a third and fourth are not in step either.
            f.animT = std::fmod(frame.poseTime * look.animSpeed + 0.37f * (float)i,
                                clip->duration);
        }
        evalPose(bones_, clip, f.animT, f.skinMats);
        const float cy = std::cos(f.disp.yaw), sy = std::sin(f.disp.yaw);
        const float cl = std::cos(f.disp.lean), sl = std::sin(f.disp.lean);
        float R[16] = {cy,      0.f, -sy,     0.f,
                       sy * sl, cl,  cy * sl, 0.f,
                       sy * cl, -sl, cy * cl, 0.f,
                       f.disp.pos[0], f.disp.pos[1], f.disp.pos[2], 1.f};
        for (size_t b = 0; b * 16 + 16 <= f.skinMats.size(); b++) {
            float tmp[16];
            matMul(R, &f.skinMats[b * 16], tmp);
            std::memcpy(&f.skinMats[b * 16], tmp, sizeof(tmp));
        }
    }

    // Weapons cut their targets before conservation, so a fresh wound is on
    // the ledger the same frame it is made. Both read the piece transforms
    // updateBrushRig wrote above, so they must sit after it and before the
    // pack — a fist tested against last frame's pieces would land its wound a
    // pose step behind the mitt that made it.
    // M-PHYS: contacts are a per-FRAME report with no history, so clear before
    // the weapons run rather than after the sim reads them. A stale contact
    // surviving one frame is a shove that keeps pushing after the fist has
    // withdrawn, which reads as the target being magnetically repelled.
    // Cheap, and it keeps a `set look.bodyColor...` from ctl reaching the clay
    // a carve exposes without a re-import.
    applyBodyColors(look);

    contacts_.clear();
    updateBladeCut(look);
    updatePunchCut(look);

    // conservation runs before packing so this frame's uniforms carry fresh
    // gob positions and the ground bound
    updateConservation(look, frame);

    float uniforms[kUniformSlots][4];
    // Latch what the eyes look at, on the pose grid, INDEPENDENT of whether a
    // skeleton exists. The bone-chain gaze below is gated on gaze_ (the eye
    // bone leaves), which is empty under the brush rig — and that gate was
    // also freezing this target, so the pupils aimed at its initial value and
    // stared at a fixed world point no matter where the camera went. The
    // pupil-rotation gaze in packUniforms reads this.
    if (look.gaze.track && frame.poseTime != gazePoseTime_) {
        gazePoseTime_ = frame.poseTime;
        const Vec3 cp = cam.pos();
        gazeTarget_[0] = cp.x;
        gazeTarget_[1] = cp.y;
        gazeTarget_[2] = cp.z;
    }
    packUniforms(cam, look, frame, uniforms);
    gpu_->queue.WriteBuffer(uniformBuf_, 0, uniforms, sizeof(uniforms));

    wgpu::CommandEncoder encoder = gpu_->device.CreateCommandEncoder();
    // Each fighter's own import/edit/JFA/redistance passes, into its own slice.
    for (Fighter& f : fighters_) f.vol.encode(encoder);
    ground_.encode(encoder);

    // ---- 12 Hz frame reuse ----
    // Fold the volume generations in: a carve or a landed gob changes what the
    // tracer would see without changing a single uniform. The generations are
    // read AFTER encode() above, so an edit queued this frame re-traces this
    // frame rather than a frame late.
    uint64_t digest = traceInputDigest(uniforms);
    for (const Fighter& f : fighters_) {
        digest = (digest ^ f.vol.generation()) * 1099511628211ull;
    }
    digest = (digest ^ ground_.generation()) * 1099511628211ull;
    const bool reuse = reuseEnabled_ && traceValid_ && digest == traceDigest_;
    framesPresented_++;

    // Why did this frame re-trace? Frame reuse is the difference between
    // motion costing one trace per pose step and one per frame, so when it
    // drops to 0% the useful question is which input refuses to settle.
    // Names the first differing uniform component, or the volume whose
    // generation moved (i.e. something queued an edit).
    static const bool dbgReuse = std::getenv("CLAYFRAY_DEBUG_REUSE") != nullptr;
    // A re-trace ON a pose step is the floor, not a fault — the image is
    // supposed to change 12 times a second. Reporting those buries the ones
    // that matter, and worse, misattributes them: the report names the first
    // differing slot in index order, so a pose step also carrying a camera
    // move gets blamed on camPos (slot 0) rather than the pose clock (slot
    // 3.3). Stay quiet unless a frame re-traced BETWEEN pose steps, which is
    // the only kind that costs anything.
    const bool poseStepFrame = uniforms[3][3] != prevUniforms_[3][3];
    if (dbgReuse && !reuse && traceValid_ && !poseStepFrame) {
        bool blamed = false;
        for (int i = 0; i <= kMaxPlayers; i++) {
            const uint32_t g = (i < kMaxPlayers) ? fighters_[i].vol.generation()
                                                 : ground_.generation();
            if (g == prevGens_[i]) continue;
            char name[32];
            if (i < kMaxPlayers) std::snprintf(name, sizeof(name), "player %d volume", i);
            else std::snprintf(name, sizeof(name), "ground clay");
            std::printf("[reuse] re-trace: %s generation %u -> %u\n", name,
                        prevGens_[i], g);
            blamed = true;
        }
        for (int s = 0; s < kUniformSlots && !blamed; s++) {
            for (int c = 0; c < 4; c++) {
                if (!digestIncludes(s, c)) continue;
                if (uniforms[s][c] == prevUniforms_[s][c]) continue;
                std::printf("[reuse] re-trace: %s (slot %d.%d)  %.9g -> %.9g\n",
                            uniformSlotName(s), s, c,
                            (double)prevUniforms_[s][c], (double)uniforms[s][c]);
                blamed = true;
                break;
            }
        }
        if (!blamed) std::printf("[reuse] re-trace between pose steps: no input changed?\n");
        std::fflush(stdout);
    }
    if (dbgReuse) {
        std::memcpy(prevUniforms_, uniforms, sizeof(prevUniforms_));
        for (int i = 0; i < kMaxPlayers; i++)
            prevGens_[i] = fighters_[i].vol.generation();
        prevGens_[kMaxPlayers] = ground_.generation();
    }

    if (!reuse) {
        framesTraced_++;
        traceDigest_ = digest;
        traceValid_ = true;
    }

    if (!reuse) {
        wgpu::PassTimestampWrites tsw{};
        wgpu::ComputePassDescriptor passDesc{};
        if (querySet_) {
            tsw.querySet = querySet_;
            tsw.beginningOfPassWriteIndex = 0;
            tsw.endOfPassWriteIndex = 1;
            passDesc.timestampWrites = &tsw;
        }
        wgpu::ComputePassEncoder pass = encoder.BeginComputePass(&passDesc);
        // B5, in the order the two passes MUST run: the periphery over the
        // whole frame first (a ray per NxN block, block-replicated), then the
        // focus region per texel on top. Dispatches inside one compute pass
        // are ordered, so the second overwrites the first rather than racing
        // it. n == 1 means foveation is off and the full-res pass owns the
        // frame alone — the untouched pre-foveation path.
        const int n = coarseFactor(look);
        if (n > 1) {
            const int cw = (width_ + n - 1) / n, ch = (height_ + n - 1) / n;
            pass.SetPipeline(traceCoarsePipeline_);
            pass.SetBindGroup(0, traceCoarseBind_);
            pass.SetBindGroup(1, traceCoarseBrickBind_);
            pass.DispatchWorkgroups((cw + 7) / 8, (ch + 7) / 8);
        }
        pass.SetPipeline(tracePipeline_);
        pass.SetBindGroup(0, traceBind_);
        pass.SetBindGroup(1, traceBrickBind_);
        pass.DispatchWorkgroups((width_ + 7) / 8, (height_ + 7) / 8);
        pass.End();
    }
    bool doPick = swapchainView != nullptr || alwaysPick_;
    if (doPick) encodePick(encoder);
    {
        wgpu::PassTimestampWrites tsw{};
        wgpu::RenderPassColorAttachment att{};
        att.view = presentView_;
        att.loadOp = wgpu::LoadOp::Clear;
        att.storeOp = wgpu::StoreOp::Store;
        att.clearValue = {0, 0, 0, 1};
        wgpu::RenderPassDescriptor desc{};
        desc.colorAttachmentCount = 1;
        desc.colorAttachments = &att;
        if (querySet_) {
            tsw.querySet = querySet_;
            tsw.beginningOfPassWriteIndex = 2;
            tsw.endOfPassWriteIndex = 3;
            desc.timestampWrites = &tsw;
        }
        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&desc);
        pass.SetPipeline(postPipeline_);
        pass.SetBindGroup(0, postBind_);
        pass.Draw(3);
        pass.End();
    }
    if (swapchainView && blitPipeline_) {
        wgpu::RenderPassColorAttachment att{};
        att.view = swapchainView;
        att.loadOp = wgpu::LoadOp::Clear;
        att.storeOp = wgpu::StoreOp::Store;
        att.clearValue = {0, 0, 0, 1};
        wgpu::RenderPassDescriptor desc{};
        desc.colorAttachmentCount = 1;
        desc.colorAttachments = &att;
        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&desc);
        pass.SetPipeline(blitPipeline_);
        pass.SetBindGroup(0, blitBind_);
        pass.Draw(3);
        if (uiCallback) uiCallback(pass);
        pass.End();
    }

    bool queryThisFrame = querySet_ && !queryMapPending_;
    if (queryThisFrame) {
        encoder.ResolveQuerySet(querySet_, 0, 4, queryResolve_, 0);
        encoder.CopyBufferToBuffer(queryResolve_, 0, queryRead_, 0, 4 * 8);
    }
    wgpu::CommandBuffer cmd = encoder.Finish();
    gpu_->queue.Submit(1, &cmd);
    // EVERY fighter: each imports and is carveable, so each has its own pool
    // partition to overflow. The foe's poll used to be armed and never
    // finished, which left capPollArmed_ stuck true and made a foe-side
    // overflow unreportable — a loop cannot forget the way a hand-written
    // second call did.
    for (Fighter& f : fighters_) {
        f.vol.finishCapacityPoll();
        f.vol.pollVolumes();
    }
    if (queryThisFrame) {
        queryMapPending_ = true;
        auto alive = alive_;
        queryRead_.MapAsync(wgpu::MapMode::Read, 0, 4 * 8,
                            wgpu::CallbackMode::AllowSpontaneous,
                            [this, alive](wgpu::MapAsyncStatus status, wgpu::StringView) {
                                if (!*alive) return;
                                if (status == wgpu::MapAsyncStatus::Success) {
                                    const uint64_t* t =
                                        (const uint64_t*)queryRead_.GetConstMappedRange(
                                            0, 4 * 8);
                                    float tr = (float)((t[1] - t[0]) * 1e-6);
                                    float po = (float)((t[3] - t[2]) * 1e-6);
                                    traceMs_ = traceMs_ * 0.9f + tr * 0.1f;
                                    postMs_ = postMs_ * 0.9f + po * 0.1f;
                                    queryRead_.Unmap();
                                }
                                queryMapPending_ = false;
                            });
    }
    if (doPick) pollPick();
}

bool Renderer::screenshot(const std::string& path) {
#if !CLAYFRAY_DEV_TOOLS
    // Blocking texture readback + a PNG written to a filesystem the user can
    // never open. Web capture is the browser's job, not ours.
    (void)path;
    return false;
#else
    const uint32_t bytesPerRow = ((uint32_t)width_ * 4 + 255) & ~255u;
    const uint64_t bufSize = (uint64_t)bytesPerRow * height_;

    wgpu::BufferDescriptor bufDesc{};
    bufDesc.size = bufSize;
    bufDesc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer readback = gpu_->device.CreateBuffer(&bufDesc);

    wgpu::CommandEncoder encoder = gpu_->device.CreateCommandEncoder();
    wgpu::TexelCopyTextureInfo src{};
    src.texture = presentTex_;
    wgpu::TexelCopyBufferInfo dst{};
    dst.buffer = readback;
    dst.layout.bytesPerRow = bytesPerRow;
    dst.layout.rowsPerImage = (uint32_t)height_;
    wgpu::Extent3D extent = {(uint32_t)width_, (uint32_t)height_, 1};
    encoder.CopyTextureToBuffer(&src, &dst, &extent);
    wgpu::CommandBuffer cmd = encoder.Finish();
    gpu_->queue.Submit(1, &cmd);

    // shared, not `&ok`: the callback can fire after we return. See snapshot.cpp.
    auto ok = std::make_shared<bool>(false);
    wgpu::Future f = readback.MapAsync(
        wgpu::MapMode::Read, 0, bufSize, wgpu::CallbackMode::WaitAnyOnly,
        [ok](wgpu::MapAsyncStatus status, wgpu::StringView msg) {
            *ok = status == wgpu::MapAsyncStatus::Success;
            if (!*ok)
                std::fprintf(stderr, "readback map failed: %.*s\n", (int)msg.length,
                             msg.data);
        });
    if (!gpuBlockOn(gpu_->instance, f, "screenshot readback")) return false;
    if (!*ok) return false;

    const uint8_t* data = (const uint8_t*)readback.GetConstMappedRange(0, bufSize);
    std::vector<uint8_t> pixels((size_t)width_ * height_ * 4);
    for (int y = 0; y < height_; y++) {
        std::memcpy(pixels.data() + (size_t)y * width_ * 4, data + (size_t)y * bytesPerRow,
                    (size_t)width_ * 4);
    }
    readback.Unmap();

    int rc = stbi_write_png(path.c_str(), width_, height_, 4, pixels.data(), width_ * 4);
    if (rc) std::printf("wrote %s (%dx%d)\n", path.c_str(), width_, height_);
    return rc != 0;
#endif
}

void Renderer::syncMeasurements() {
#if !CLAYFRAY_DEV_TOOLS
    // A sleep-spin on the main thread is a blocking wait wearing a different
    // hat, and it would hang the tab just as hard: the map callbacks it waits
    // for are delivered by the JS event loop this loop is refusing to return
    // to. Only snapshots and the headless loops need the ledger pinned to a
    // frame boundary, and both are desktop-only.
    return;
#else
    // Copies were submitted with the frame; they only need the event pump,
    // not more renders. Bounded wait so a lost map can't hang the app.
    for (int guard = 0; !fighters_[0].vol.measurementsIdle() && guard < 20000; guard++) {
        gpu_->processEvents();
        fighters_[0].vol.pollVolumes();
        std::this_thread::sleep_for(std::chrono::microseconds(250));
    }
    if (!fighters_[0].vol.measurementsIdle()) {
        std::fprintf(stderr, "[snap] warning: volume measurements still in flight\n");
    }
#endif
}

#if !CLAYFRAY_DEV_TOOLS
// Snapshots are desktop-only (see the header of snapshot.cpp). Kept as
// definitions rather than #if'd at the call sites, so ctl's `snap` verb and
// main's --load need no platform knowledge.
bool Renderer::saveSnapshot(const std::string&, double, const std::string&) {
    return false;
}
bool Renderer::loadSnapshot(const std::string&, double*, const std::string&) {
    return false;
}
#else

namespace {
// CPU-side conservation/pose state (RCPU section). Same-build raw bytes,
// like every snapshot section.
struct RenderSnapCpu {
    float carved, deposited, debt, inFlight;
    int32_t gobCount;
    float lastWound[3], woundDir[3], woundCol[3];
    uint32_t haveWound, gobSeed;
    float animT;
    uint32_t pad;
    double simT;
};
} // namespace

bool Renderer::saveSnapshot(const std::string& path, double simT,
                            const std::string& charPath) {
    // settle the ledger first: an unread measurement would be lost to the
    // file (carve visible in the volume, never counted as carved)
    syncMeasurements();
    absorbMeasured();

    SnapWriter w;
    if (!w.open(path)) {
        std::fprintf(stderr, "[snap] cannot write %s\n", path.c_str());
        return false;
    }
    // Player 0's volume only, as before the slice refactor. Extra fighters
    // need their own tagged sections (the format is per-fighter-clean — the
    // saved indirection carries partition-LOCAL brick indices — so this is a
    // section-naming job, not a format one). Until then a snapshot restores
    // the hero's clay and leaves the others as imported.
    if (!fighters_[0].vol.save(w) || !ground_.save(w)) {
        w.close();
        return false;
    }
    RenderSnapCpu rc{};
    rc.carved = sploot_.carved;
    rc.deposited = sploot_.deposited;
    rc.debt = sploot_.debt;
    rc.inFlight = sploot_.inFlight;
    rc.gobCount = (int32_t)gobs_.size();
    std::memcpy(rc.lastWound, lastWound_, sizeof(rc.lastWound));
    std::memcpy(rc.woundDir, woundDir_, sizeof(rc.woundDir));
    std::memcpy(rc.woundCol, woundCol_, sizeof(rc.woundCol));
    rc.haveWound = haveWound_ ? 1 : 0;
    rc.gobSeed = gobSeed_;
    rc.animT = fighters_[0].animT;
    rc.simT = simT;
    w.section("RCPU", &rc, sizeof(rc));
    // NEITHER "RRIG" NOR "RBAG" is written any more, and v9 is where both went.
    // RRIG held the affine body's squish spring while the RENDERER owned one;
    // M-SPRING moved that state into the sim (Body::stepHop), which this file
    // does not serialise at all — no snapshot has ever carried `vel` or `knock`
    // either, and the hopper is the same kind of thing. RBAG held the M-BAG
    // wobble, which v8 deleted outright in favour of the R20 dent (a real
    // displacement of the field, so it lives in the VOLUME and is saved with
    // it). A restore therefore lands a fighter standing with its bounce
    // starting fresh, exactly as it already landed one not walking.
    w.section("RGOB", gobs_.data(), gobs_.size() * sizeof(Gob));
    w.section("CHRP", charPath.data(), charPath.size());
    bool ok = w.close();
    std::printf("[snap] %s %s\n", ok ? "saved" : "FAILED to save", path.c_str());
    return ok;
}

bool Renderer::loadSnapshot(const std::string& path, double* simT,
                            const std::string& charPath) {
    SnapReader r;
    if (!r.open(path)) {
        std::fprintf(stderr, "[snap] cannot read %s\n", path.c_str());
        return false;
    }
    std::vector<uint8_t> cp = r.blob("CHRP");
    std::string savedChar((const char*)cp.data(), cp.size());
    if (savedChar != charPath) {
        std::fprintf(stderr,
                     "[snap] warning: snapshot character '%s' != loaded '%s' — "
                     "voxels won't match the rig\n",
                     savedChar.c_str(), charPath.c_str());
    }
    RenderSnapCpu rc{};
    if (!r.read("RCPU", &rc, sizeof(rc))) {
        std::fprintf(stderr, "[snap] missing RCPU section\n");
        return false;
    }
    if (!fighters_[0].vol.load(r) || !ground_.load(r)) return false;
    gobs_.assign((size_t)std::max(rc.gobCount, 0), Gob{});
    if (rc.gobCount > 0 &&
        !r.read("RGOB", gobs_.data(), gobs_.size() * sizeof(Gob))) {
        gobs_.clear();
    }
    sploot_.carved = rc.carved;
    sploot_.deposited = rc.deposited;
    sploot_.debt = rc.debt;
    sploot_.inFlight = rc.inFlight;
    sploot_.gobs = (int)gobs_.size();
    std::memcpy(lastWound_, rc.lastWound, sizeof(lastWound_));
    std::memcpy(woundDir_, rc.woundDir, sizeof(woundDir_));
    std::memcpy(woundCol_, rc.woundCol, sizeof(woundCol_));
    haveWound_ = rc.haveWound != 0;
    gobSeed_ = rc.gobSeed;
    fighters_[0].animT = rc.animT;
    // Drop any in-progress slice rather than serialising it. sliceVol_ is only
    // a hold on debt, and rc.debt above is the authoritative figure, so letting
    // the restored debt dribble is correct and exactly balanced — carrying a
    // stale reservation across a load could instead withhold more debt than
    // the restored ledger has. Measurements from the pre-load volume are
    // dropped by snapGen_, so slicePending_ must be cleared with it.
    sliceVol_ = 0.f;
    sliceOpen_ = false;
    sliceCutStep_ = false;
    slicePending_ = 0;
    sliceWait_ = 0;
    // No "RRIG" to read any more (see saveSnapshot): the hopper is sim state.
    // Only the display latch is cleared, so the first drawn frame after a
    // restore takes its height from the sim rather than from whatever body was
    // on screen before the load.
    for (Fighter& f : fighters_) {
        f.hopLatch = 0.f;
        f.pose.hopU = 0.f;
        f.disp.hopU = 0.f;
    }
    // fresh dt baseline: the restored clock may sit anywhere on the timeline
    lastSimTime_ = -1.f;
    lastPoseTime_ = -1.f;
    springPoseTime_ = -1.f;
    // Force the hand pose index to re-latch on the next pose step. It is a
    // pure function of the sword-grip state, so it needs no snapshot section —
    // but the LATCH CLOCK does have to be invalidated or a restored scene can
    // hold a stale grip until the pose time happens to move.
    handPoseTime_ = -1.f;
    // Same story for the punch tracker: a restore relocates every mitt, and a
    // stale "where the fist was last frame" would read that jump as a strike.
    for (Fighter& f : fighters_) f.haveHandPrev = false;
    haveBlade_ = false;
    if (simT) *simT = rc.simT;
    std::printf("[snap] loaded %s (t=%.2fs, %zu gob(s))\n", path.c_str(), rc.simT,
                gobs_.size());
    return true;
}
#endif // CLAYFRAY_DEV_TOOLS

int Renderer::addPlayer(const FighterPose& at) {
    if (playerCount_ >= kMaxPlayers) {
        std::fprintf(stderr,
                     "addPlayer: %d fighters is the store's slice count "
                     "(BrickSystem::kMaxFighters); raising it costs pool "
                     "memory, not shader bindings\n",
                     kMaxPlayers);
        return -1;
    }
    const int idx = playerCount_++;
    fighters_[idx].pose = at;
    fighters_[idx].disp = at;
    fighters_[idx].enabled = true;
    // (The breath/bounce phase stagger that used to be set here moved to
    // GameState's constructor with the hopper — it is Body state now.)
    return idx;
}
