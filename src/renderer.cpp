#include "renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include "snapshot.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

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
// volume geometry block generated from src/brick.h.
//
// //#constants goes in each compiled ROOT, never in an include: brick_read is
// pulled into both trace and pick, and WGSL rejects a duplicate const.
std::string loadShader(const char* name) {
    std::string src = readFileRaw(shaderPath(name));
    std::stringstream out;
    std::stringstream in(src);
    std::string line;
    while (std::getline(in, line)) {
        const std::string tag = "//#include ";
        if (line.rfind("//#constants", 0) == 0) {
            out << BrickSystem::wgslConstants();
        } else if (line.rfind(tag, 0) == 0) {
            std::string inc = line.substr(tag.size());
            while (!inc.empty() && (inc.back() == ' ' || inc.back() == '\r')) inc.pop_back();
            out << loadShader(inc.c_str()) << "\n";
        } else {
            out << line << "\n";
        }
    }
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
    return out.str();
}

// Newest mtime across the shader directory; drives hot reload.
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

    if (!brick_.init(gpu, loadShader("edit.wgsl"), loadShader("jfa.wgsl"),
                     loadShader("redistance.wgsl"), loadShader("voxelize.wgsl")))
        return false;
    // fighter 1's volume. A whole second BrickSystem, so every pass it owns
    // (voxelize, edit, JFA, redistance) works on it unchanged and its clay is
    // physically separate from the hero's.
    if (!foe_.init(gpu, loadShader("edit.wgsl"), loadShader("jfa.wgsl"),
                   loadShader("redistance.wgsl"), loadShader("voxelize.wgsl")))
        return false;
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
    shaderDirStamp_ = shaderDirStamp();
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
        tracePipeline_ = dev.CreateComputePipeline(&desc);
        lap("trace");
    }
    {
        wgpu::ShaderModule mod = makeModule(dev, pickSrc, "pick");
        wgpu::ComputePipelineDescriptor desc{};
        desc.label = "pick";
        desc.compute.module = mod;
        desc.compute.entryPoint = "cs";
        pickPipeline_ = dev.CreateComputePipeline(&desc);
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
        blitPipeline_ = dev.CreateRenderPipeline(&desc);
    }
    return tracePipeline_ && postPipeline_ && pickPipeline_;
}

void Renderer::buildBindGroups() {
    wgpu::Device& dev = gpu_->device;
    {
        wgpu::BindGroupEntry entries[2] = {};
        entries[0].binding = 0;
        entries[0].buffer = uniformBuf_;
        entries[1].binding = 1;
        entries[1].textureView = hdrView_;
        wgpu::BindGroupDescriptor desc{};
        desc.layout = tracePipeline_.GetBindGroupLayout(0);
        desc.entryCount = 2;
        desc.entries = entries;
        traceBind_ = dev.CreateBindGroup(&desc);
    }
    {
        // One binding covers indirection + seeds + coarse + cell weights: they
        // are regions of brick_.volume, and Metal allows a stage only 10
        // storage buffers (brick.h). Six here plus fighter 1's three = 9.
        wgpu::BindGroupEntry entries[6] = {};
        entries[0].binding = 0;
        entries[0].buffer = brick_.volume;
        entries[1].binding = 1;
        entries[1].buffer = brick_.distPool;
        entries[2].binding = 2;
        entries[2].buffer = brick_.albedoPool;
        entries[3].binding = 7;
        entries[3].buffer = ground_.base;
        entries[4].binding = 8;
        entries[4].buffer = ground_.height;
        entries[5].binding = 9;
        entries[5].buffer = ground_.color;
        wgpu::BindGroupDescriptor desc{};
        desc.layout = tracePipeline_.GetBindGroupLayout(1);
        desc.entryCount = 6;
        desc.entries = entries;
        traceBrickBind_ = dev.CreateBindGroup(&desc);
    }
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
        // Same three as trace, minus the ground: pick marches the body only.
        // (Before the volume regions merged, trace and pick needed different
        // entry counts here — pick's auto layout dropped the coarse field it
        // never reads. One binding now carries every per-cell array, so both
        // layouts agree.)
        wgpu::BindGroupEntry entries[3] = {};
        entries[0].binding = 0;
        entries[0].buffer = brick_.volume;
        entries[1].binding = 1;
        entries[1].buffer = brick_.distPool;
        entries[2].binding = 2;
        entries[2].buffer = brick_.albedoPool;
        wgpu::BindGroupDescriptor desc{};
        desc.layout = pickPipeline_.GetBindGroupLayout(1);
        desc.entryCount = 3;
        desc.entries = entries;
        pickBrickBind_ = dev.CreateBindGroup(&desc);
    }
    {
        // group(2): fighter 1's volume, laid out exactly like fighter 0's.
        // Bound for BOTH pipelines because brick_read.wgsl references these
        // statically, so Tint keeps the bindings alive even on paths pick
        // never takes.
        wgpu::BindGroupEntry entries[3] = {};
        entries[0].binding = 0;
        entries[0].buffer = foe_.volume;
        entries[1].binding = 1;
        entries[1].buffer = foe_.distPool;
        entries[2].binding = 2;
        entries[2].buffer = foe_.albedoPool;
        wgpu::BindGroupDescriptor desc{};
        desc.entries = entries;
        desc.entryCount = 3;
        desc.layout = tracePipeline_.GetBindGroupLayout(2);
        traceFoeBind_ = dev.CreateBindGroup(&desc);
        desc.layout = pickPipeline_.GetBindGroupLayout(2);
        pickFoeBind_ = dev.CreateBindGroup(&desc);
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
    long stamp = shaderDirStamp();
    if (stamp == shaderDirStamp_) return false;
    shaderDirStamp_ = stamp;
    std::printf("reloading shaders\n");
    ground_.rebuildPipelines(loadShader("ground.wgsl"));
    if (buildPipelines()) buildBindGroups();
    traceValid_ = false; // recompiled shaders may trace differently
    // character source may have changed; rebuild the volume from it
    brick_.requestBake();
    return true;
}

// Digest of everything the TRACE pass reads. Deliberately excludes the three
// things that change every frame without changing the traced image:
//   camUp.w   frame.time  — not read by any shader
//   res.z     grainFrame  — post only (so grain still animates over a reuse)
//   post.xyzw, post2.xyz  — post only; post2.w is debugMode and IS traced
//   mouse     pick uv     — pick runs every frame regardless, on its own pass
// Volume CONTENTS are not in the uniform buffer at all, so the caller folds in
// the brick/ground generation counters separately.
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
        if (s == 13) continue;               // pick uv
        if (s == 11) continue;               // post
        for (int c = 0; c < 4; c++) {
            if (s == 2 && c == 3) continue;  // frame.time
            if (s == 4 && c == 2) continue;  // grainFrame
            if (s == 12 && c != 3) continue; // post2: keep only debugMode
            mix(u[s][c]);
        }
    }
    return h;
}

void Renderer::packUniforms(const OrbitCamera& cam, const LookParams& look,
                            const FrameInfo& frame, float out[kUniformSlots][4]) const {
    Vec3 fwd, right, up;
    cam.basis(fwd, right, up);
    Vec3 pos = cam.pos();
    float aspect = (float)width_ / (float)height_;

    float packed[14][4] = {
        {pos.x, pos.y, pos.z, cam.fovY},
        {right.x, right.y, right.z, aspect},
        {up.x, up.y, up.z, frame.time},
        {fwd.x, fwd.y, fwd.z, frame.poseTime},
        {(float)width_, (float)height_, frame.grainFrame, (float)frame.aaSamples},
        {look.keyPos[0], look.keyPos[1], look.keyPos[2], look.keyIntensity},
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
    // Beads for EVERY player, packed into the one 8-slot array: 4 eyes each
    // for two fighters fills it exactly. Each set rides its own skeleton, so
    // the opponent's eyes track its own pose.
    int n = 0;
    auto packMarbles = [&](const std::vector<float>& mats) {
        for (const MarbleProp& m : marbles_) {
            if (n >= 8) break;
            float* slotA = out[14 + n * 2];
            float* slotB = out[15 + n * 2];
            float pos[3] = {m.pos[0], m.pos[1], m.pos[2]};
            // props ride their bone rigidly; radius stays fixed (glass beads
            // don't breathe with torso scale)
            if (m.bone >= 0 && (size_t)(m.bone * 16 + 16) <= mats.size()) {
                matTransformPoint(&mats[m.bone * 16], m.pos, pos);
            }
            slotA[0] = pos[0]; slotA[1] = pos[1]; slotA[2] = pos[2];
            slotA[3] = m.radius;
            slotB[0] = m.color[0]; slotB[1] = m.color[1]; slotB[2] = m.color[2];
            n++;
        }
    };
    packMarbles(skinMats_);
    if (foeEnabled_ && !foeSkinMats_.empty()) packMarbles(foeSkinMats_);
    out[30][0] = (float)n;

    // posed capsule shadow proxy (empty for the analytic fallback character)
    int cn = std::min((int)capsules_.size(), 16);
    float center[3] = {0.f, 0.f, 0.f};
    for (int i = 0; i < cn; i++) {
        const BoneCapsule& c = capsules_[i];
        float a[3] = {c.a[0], c.a[1], c.a[2]};
        float b[3] = {c.b[0], c.b[1], c.b[2]};
        if (c.bone >= 0 && (size_t)(c.bone * 16 + 16) <= skinMats_.size()) {
            matTransformPoint(&skinMats_[c.bone * 16], c.a, a);
            matTransformPoint(&skinMats_[c.bone * 16], c.b, b);
        }
        float* slotA = out[33 + i * 2];
        float* slotB = out[34 + i * 2];
        slotA[0] = a[0]; slotA[1] = a[1]; slotA[2] = a[2]; slotA[3] = c.r;
        slotB[0] = b[0]; slotB[1] = b[1]; slotB[2] = b[2];
        for (int k = 0; k < 3; k++) center[k] += (a[k] + b[k]) * 0.5f;
    }
    if (cn > 0) {
        for (int k = 0; k < 3; k++) center[k] /= (float)cn;
        float radius = 0.f;
        for (int i = 0; i < cn; i++) {
            for (int e = 0; e < 2; e++) {
                const float* p = out[33 + i * 2 + e];
                float dx = p[0] - center[0], dy = p[1] - center[1], dz = p[2] - center[2];
                radius = std::max(radius, std::sqrt(dx * dx + dy * dy + dz * dz) +
                                              out[33 + i * 2][3]);
            }
        }
        out[31][1] = radius + 0.05f;
        out[32][0] = center[0]; out[32][1] = center[1]; out[32][2] = center[2];
    }
    out[31][0] = (float)cn;

    // M4-P1 chunks: the traced body = smin-union over pieces of
    // max(restField(invSkin * p), restCapsule(invSkin * p)). Rigid warps
    // preserve the distance metric; max/min/smin keep Lipschitz <= 1.
    static const bool noPieces = std::getenv("CLAYFRAY_NO_PIECES") != nullptr;
    int pn = noPieces ? 0 : cn;
    float bodyBoundR = 0.f;
    for (int i = 0; i < pn; i++) {
        const BoneCapsule& c = capsules_[i];
        float inv[16];
        if (c.bone >= 0 && (size_t)(c.bone * 16 + 16) <= skinMats_.size()) {
            matInvAffine(&skinMats_[c.bone * 16], inv);
        } else {
            matIdentity(inv);
        }
        float* base = out[66 + i * 12];
        std::memcpy(base, inv, 16 * sizeof(float)); // 4 slots: column-major mat4
        // forward skin matrix: the trace warp round-trips its blended rest
        // sample through the forward blend and rejects inconsistent samples
        // (vacated-space ghosts)
        if (c.bone >= 0 && (size_t)(c.bone * 16 + 16) <= skinMats_.size()) {
            std::memcpy(out[66 + i * 12 + 4], &skinMats_[c.bone * 16],
                        16 * sizeof(float));
        } else {
            float ident[16];
            matIdentity(ident);
            std::memcpy(out[66 + i * 12 + 4], ident, 16 * sizeof(float));
        }
        float* lo = out[66 + i * 12 + 8];
        float* hi = out[66 + i * 12 + 9];
        float* ca = out[66 + i * 12 + 10];
        float* cb = out[66 + i * 12 + 11];
        // squash/stretch (bone scale channels) makes the warp non-rigid:
        // rest distances overestimate world distances by up to the smallest
        // scale factor. Store min column norm of the skin matrix; the shader
        // multiplies sampled distances by it to restore Lipschitz <= 1.
        float sMin = 1.f;
        if (c.bone >= 0 && (size_t)(c.bone * 16 + 16) <= skinMats_.size()) {
            const float* sm = &skinMats_[c.bone * 16];
            sMin = 1e9f;
            for (int col = 0; col < 3; col++) {
                float len = std::sqrt(sm[col * 4] * sm[col * 4] +
                                      sm[col * 4 + 1] * sm[col * 4 + 1] +
                                      sm[col * 4 + 2] * sm[col * 4 + 2]);
                sMin = std::min(sMin, len);
            }
            sMin = std::min(std::max(sMin, 0.25f), 1.f); // never inflate steps
        }
        lo[3] = sMin;
        lo[0] = c.lo[0]; lo[1] = c.lo[1]; lo[2] = c.lo[2];
        hi[0] = c.hi[0]; hi[1] = c.hi[1]; hi[2] = c.hi[2];
        ca[0] = c.a[0]; ca[1] = c.a[1]; ca[2] = c.a[2]; ca[3] = c.rPiece;
        cb[0] = c.b[0]; cb[1] = c.b[1]; cb[2] = c.b[2];
        cb[3] = (float)c.bone; // weight-ownership gate compares joint ids
        // posed far bound: piece content stays within rPiece + box diagonal
        // slack of its posed capsule endpoints
        const float* pa = out[33 + i * 2];
        const float* pb = out[34 + i * 2];
        for (int e = 0; e < 2; e++) {
            const float* p = e ? pb : pa;
            float dx = p[0] - center[0], dy = p[1] - center[1], dz = p[2] - center[2];
            bodyBoundR = std::max(bodyBoundR,
                                  std::sqrt(dx * dx + dy * dy + dz * dz) + c.rPiece);
        }
    }
    out[65][0] = (float)pn;
    // joint smin k: with exclusive dominance regions (no double-render) the
    // blend only bridges hairline handoff cracks; ~2 voxels seals them
    // without re-introducing ring bulges. VOXEL-relative, not metres — as a
    // fixed 0.008 m it shrank to 1.5 voxels at kGrid=37 and the cracks
    // reopened at the shoulders (see kJointSminVoxels in brick.h).
    out[65][1] = BrickSystem::kJointSminVoxels * BrickSystem::kVoxel;
    // box test margin: sample within, bound outside. Span-relative for the
    // same reason.
    out[65][2] = BrickSystem::kBoxMarginSpans * BrickSystem::kSpan;
    out[65][3] = bodyBoundR + 0.05f;

    // Player 1's pieces (slots 293..484, count at 485). Identical packing to
    // the hero's above but driven by ITS skeleton, which is what lets it play
    // its own clip instead of standing rigid.
    int fpn = 0;
    if (foeEnabled_ && !noPieces && foeSkinMats_.size() >= bones_.size() * 16) {
        fpn = std::min((int)capsules_.size(), 16);
        for (int i = 0; i < fpn; i++) {
            const BoneCapsule& c = capsules_[i];
            const bool ok =
                c.bone >= 0 && (size_t)(c.bone * 16 + 16) <= foeSkinMats_.size();
            float inv[16];
            if (ok) matInvAffine(&foeSkinMats_[c.bone * 16], inv);
            else matIdentity(inv);
            float* base = out[293 + i * 12];
            std::memcpy(base, inv, 16 * sizeof(float));
            if (ok) {
                std::memcpy(out[293 + i * 12 + 4], &foeSkinMats_[c.bone * 16],
                            16 * sizeof(float));
            } else {
                float ident[16];
                matIdentity(ident);
                std::memcpy(out[293 + i * 12 + 4], ident, 16 * sizeof(float));
            }
            float* lo = out[293 + i * 12 + 8];
            float* hi = out[293 + i * 12 + 9];
            float* ca = out[293 + i * 12 + 10];
            float* cb = out[293 + i * 12 + 11];
            float sMin = 1.f;
            if (ok) {
                const float* sm = &foeSkinMats_[c.bone * 16];
                sMin = 1e9f;
                for (int col = 0; col < 3; col++) {
                    float len = std::sqrt(sm[col * 4] * sm[col * 4] +
                                          sm[col * 4 + 1] * sm[col * 4 + 1] +
                                          sm[col * 4 + 2] * sm[col * 4 + 2]);
                    sMin = std::min(sMin, len);
                }
                sMin = std::min(std::max(sMin, 0.25f), 1.f);
            }
            lo[3] = sMin;
            lo[0] = c.lo[0]; lo[1] = c.lo[1]; lo[2] = c.lo[2];
            hi[0] = c.hi[0]; hi[1] = c.hi[1]; hi[2] = c.hi[2];
            ca[0] = c.a[0]; ca[1] = c.a[1]; ca[2] = c.a[2]; ca[3] = c.rPiece;
            cb[0] = c.b[0]; cb[1] = c.b[1]; cb[2] = c.b[2];
            cb[3] = (float)c.bone;
        }
    }
    out[485][0] = (float)fpn;

    // M4.6 conservation: in-flight gobs (12 Hz-stepped positions) + the
    // ground field's clay top bound (0 disables the field in the tracer).
    // These slot indices are hand-mirrored in trace.wgsl AND pick.wgsl —
    // this static_assert catches only the C++ side overrunning the buffer;
    // the WGSL side has no compile-time link, so CLAUDE.md flags the mirror.
    static_assert(kUniformSlots >= 486,
                  "gobs reach out[282], groundMeta out[283], sword out[284-286], "
                  "foePieces out[293-485]; keep kUniformSlots and the Uniforms "
                  "struct in trace.wgsl + pick.wgsl in sync");
    int gn = std::min((int)gobs_.size(), 12);
    for (int i = 0; i < gn; i++) {
        const Gob& g = gobs_[i];
        float* slotA = out[259 + i * 2];
        float* slotB = out[260 + i * 2];
        slotA[0] = g.disp[0]; slotA[1] = g.disp[1]; slotA[2] = g.disp[2];
        slotA[3] = g.radius;
        slotB[0] = g.col[0]; slotB[1] = g.col[1]; slotB[2] = g.col[2];
    }
    out[258][0] = (float)gn;
    out[283][0] = GroundClay::kOrigin;
    out[283][1] = GroundClay::kTexel;
    out[283][2] = (float)GroundClay::kN;
    out[283][3] = ground_.maxTopY();

    // M4.7 sword: emissive blade endpoints (radius 0 = inactive). memset above
    // already zeroed the slots, so the disabled case needs no write.
    if (swordWorld_.enabled && !bones_.empty()) {
        float hilt[3], tip[3], gA[3], gB[3];
        swordGeometry(swordWorld_, hilt, tip, gA, gB);
        out[284][0] = hilt[0]; out[284][1] = hilt[1]; out[284][2] = hilt[2];
        out[284][3] = swordWorld_.radius;
        out[285][0] = tip[0]; out[285][1] = tip[1]; out[285][2] = tip[2];
        out[286][0] = swordWorld_.color[0];
        out[286][1] = swordWorld_.color[1];
        out[286][2] = swordWorld_.color[2];
    }

    // M5 fighter 1: world -> its rest volume (slots 287..290), then the
    // bounding sphere the tracer rejects against (291..292). Rigid, so the
    // inverse is just the transpose-rotation plus the moved origin, and the
    // sampled distance needs no Lipschitz rescale.
    {
        float cy = std::cos(foeDisp_.yaw), sy = std::sin(foeDisp_.yaw);
        // forward: R = yaw(Y), t = foe pos. inverse: R^T, -R^T t
        float inv[16] = {cy, 0.f, sy, 0.f,
                         0.f, 1.f, 0.f, 0.f,
                         -sy, 0.f, cy, 0.f,
                         0.f, 0.f, 0.f, 1.f};
        const float* t = foeDisp_.pos;
        inv[12] = -(cy * t[0] + sy * t[2]);
        inv[13] = -t[1];
        inv[14] = -(-sy * t[0] + cy * t[2]);
        std::memcpy(out[287], inv, 16 * sizeof(float));
        out[291][0] = foeEnabled_ ? 1.f : 0.f;
        out[291][1] = foeBoundR_;
        // bound centre in WORLD: rotate the rest centre and translate
        out[292][0] = t[0] + cy * foeCenterRest_[0] + sy * foeCenterRest_[2];
        out[292][1] = t[1] + foeCenterRest_[1];
        out[292][2] = t[2] - sy * foeCenterRest_[0] + cy * foeCenterRest_[2];
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
    float cy = std::cos(fighterDisp_.yaw), sy = std::sin(fighterDisp_.yaw);
    const float* p = look.sword.pos;
    // lean tips the held sword with the body: rotate about the travel-perp
    // axis, which in character space is +X (forward is +Z)
    float cl = std::cos(fighterDisp_.lean), sl = std::sin(fighterDisp_.lean);
    float ly = p[1] * cl - p[2] * sl;
    float lz = p[1] * sl + p[2] * cl;
    swordWorld_.pos[0] = fighterDisp_.pos[0] + cy * p[0] + sy * lz;
    swordWorld_.pos[1] = fighterDisp_.pos[1] + ly;
    swordWorld_.pos[2] = fighterDisp_.pos[2] - sy * p[0] + cy * lz;
    swordWorld_.yaw = look.sword.yaw + fighterDisp_.yaw;
    swordWorld_.pitch = look.sword.pitch + fighterDisp_.lean;
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
    evalPose(bones_, nullptr, 0.f, skinMats_); // identity: rest pose

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
    autoReach_ = 0.f;
    if (!bodyCom_.bone.empty()) {
        float com[3];
        evalBodyCom(bodyCom_, skinMats_, com); // skinMats_ = rest pose here
        for (const HandIkChain& c : handIk_) {
            float w[3];
            restOrigin(c.wrist, w);
            float dx = w[0] - com[0], dy = w[1] - com[1], dz = w[2] - com[2];
            autoReach_ = std::max(autoReach_,
                                  std::sqrt(dx * dx + dy * dy + dz * dz));
        }
    }
    std::printf("anim: %zu bones, %zu clip(s), %zu shadow capsules, %zu hand(s), "
                "rest reach %.3f m\n",
                bones_.size(), clips_.size(), capsules_.size(), handIk_.size(),
                autoReach_);
    // fighter 1 is the same character in its own volume — "identical second
    // fighter". Rest capsules are shared (same mesh), and the bound sphere
    // comes off the mesh so the tracer's cheap reject is tight.
    foeCaps_ = capsules_;
    {
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (uint32_t v = 0; v < asset.vertexCount(); v++) {
            for (int a = 0; a < 3; a++) {
                lo[a] = std::min(lo[a], asset.positions[v * 3 + a]);
                hi[a] = std::max(hi[a], asset.positions[v * 3 + a]);
            }
        }
        float r2 = 0.f;
        for (int a = 0; a < 3; a++) {
            foeCenterRest_[a] = (lo[a] + hi[a]) * 0.5f;
            float h = (hi[a] - lo[a]) * 0.5f;
            r2 += h * h;
        }
        foeBoundR_ = std::sqrt(r2) + 0.05f;
    }
    // Both fighters are the same character, so the mesh-derived half of the
    // import (bins, watertight parity, smooth normals, per-cell skin field,
    // and the read-only GPU uploads) is computed ONCE and shared. Doing it
    // per BrickSystem cost ~2.5 s of startup and a duplicate ~32 MB upload
    // for byte-identical results.
    std::shared_ptr<BrickSystem::MeshImport> mesh =
        BrickSystem::prepareImport(*gpu_, asset);
    foe_.requestImport(mesh);
    brick_.requestImport(std::move(mesh));
}

void Renderer::encodePick(wgpu::CommandEncoder& enc) {
    wgpu::ComputePassEncoder pass = enc.BeginComputePass();
    pass.SetPipeline(pickPipeline_);
    pass.SetBindGroup(0, pickBind_);
    pass.SetBindGroup(1, pickBrickBind_);
    pass.SetBindGroup(2, pickFoeBind_);
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
                               pickRead_.Unmap();
                               // world vs rest at the cursor: the gap between
                               // them IS the articulation, and edits must use
                               // rest. Silent divergence here is what makes a
                               // walked-away fighter stop carving.
                               static const bool dbg =
                                   std::getenv("CLAYFRAY_DEBUG_PICK") != nullptr;
                               if (dbg && pickValid_) {
                                   std::printf("[pick] world (%.4f %.4f %.4f) rest "
                                               "(%.4f %.4f %.4f) mat %.1f\n",
                                               pickPos_[0], pickPos_[1], pickPos_[2],
                                               pickRest_[0], pickRest_[1], pickRest_[2],
                                               pickMat_);
                                   std::fflush(stdout);
                               }
                           }
                           pickMapPending_ = false;
                       });
}

BrickEdit Renderer::queueBrickEdit(BrickEdit e) {
    // snapshot the wound's facing + material color for the gobs this edit
    // will tear off (measurement arrives frames later; pick moves on)
    if (pickValid_ && !e.fromGob) {
        std::memcpy(e.outDir, pickNormal_, sizeof(e.outDir));
        std::memcpy(e.worldPos, pickPos_, sizeof(e.worldPos));
        if (pickMat_ > 2.5f && pickMat_ < 3.5f) {
            std::memcpy(e.srcColor, pickAlbedo_, sizeof(e.srcColor));
        }
    } else if (e.worldPos[0] == 0.f && e.worldPos[1] == 0.f && e.worldPos[2] == 0.f) {
        // scripted edit (ctl/replay/carve-test): authored in rest space, so
        // place its wound by the fighter root. Exact while unposed, and only
        // the gob spawn point rides on it.
        float cy = std::cos(fighterDisp_.yaw), sy = std::sin(fighterDisp_.yaw);
        e.worldPos[0] = fighterDisp_.pos[0] + cy * e.pos[0] + sy * e.pos[2];
        e.worldPos[1] = fighterDisp_.pos[1] + e.pos[1];
        e.worldPos[2] = fighterDisp_.pos[2] - sy * e.pos[0] + cy * e.pos[2];
    }
    brick_.queueEdit(e);
    return e;
}

void Renderer::absorbMeasured() {
    // Measurements from ops that ran 1-2 frames ago. Always drained — even
    // with conservation off — or a measurement queued while ON but arriving
    // while OFF would desync the ledger the next time it's toggled back on.
    // The toggle gates SPAWNING (updateConservation), not accounting.
    // Both fighters bill to ONE ledger: clay carved off either body becomes
    // the same gobs and lands on the same arena, so conservation is arena-wide.
    MeasuredEdit m;
    for (BrickSystem* bs : {&brick_, &foe_}) {
      while (bs->takeMeasured(m)) {
        if (std::getenv("CLAYFRAY_DEBUG_LEDGER")) {
            std::printf("[sploot] mode %d measured %.1f ml\n", m.edit.mode,
                        m.volume * 1e6f);
        }
        if (m.edit.mode == 1) {
            sploot_.carved += m.volume;
            sploot_.debt += m.volume;
            std::memcpy(lastWound_, m.edit.worldPos, sizeof(lastWound_));
            std::memcpy(woundDir_, m.edit.outDir, sizeof(woundDir_));
            std::memcpy(woundCol_, m.edit.srcColor, sizeof(woundCol_));
            haveWound_ = true;
        } else if (m.edit.mode == 2 && m.edit.fromGob) {
            // a landed gob became body clay; smin over/under-fill goes back
            // on the ledger so nothing is created or destroyed
            sploot_.deposited += m.volume;
            sploot_.debt += m.edit.gobVol - m.volume;
        }
      }
    }
}

void Renderer::updateBladeCut(const LookParams& look) {
    if (!swordWorld_.enabled || !foeEnabled_ || foeCaps_.empty()) {
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
    if (foeSkinMats_.empty()) return;
    auto foeToWorld = [&](const BoneCapsule& c, const float* r, float* w) {
        if (c.bone >= 0 && (size_t)(c.bone * 16 + 16) <= foeSkinMats_.size()) {
            matTransformPoint(&foeSkinMats_[c.bone * 16], r, w);
        } else {
            w[0] = r[0]; w[1] = r[1]; w[2] = r[2];
        }
    };
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
    subs = std::min(std::max(subs, 1), BrickSystem::kOpsPerFrame);
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
            for (const BoneCapsule& c : foeCaps_) {
                float a2[3], b2[3];
                foeToWorld(c, c.a, a2);
                foeToWorld(c, c.b, b2);
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
                if (dist >= c.r) continue;
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

        BrickEdit e;
        e.mode = 1; // carve
        e.radius = std::max(swordWorld_.radius * 1.8f, 0.022f);
        e.segment = true;
        float dirW[3] = {t0s[0] - h0[0], t0s[1] - h0[1], t0s[2] - h0[2]};
        float bladeLen =
            std::sqrt(dirW[0] * dirW[0] + dirW[1] * dirW[1] + dirW[2] * dirW[2]);
        // push past entry and exit so the cut breaks the surface both sides
        float over = bladeLen > 1e-6f ? (e.radius * 0.9f) / bladeLen : 0.f;
        float t0 = std::max(0.f, tIn - over), t1 = std::min(1.f, tOut + over);
        float wA[3], wB[3];
        for (int k = 0; k < 3; k++) {
            wA[k] = h0[k] + dirW[k] * t0;
            wB[k] = h0[k] + dirW[k] * t1;
        }
        // world -> the opponent's REST volume (trap 6: edits are rest space),
        // through the bone the blade actually met
        float invBone[16];
        matIdentity(invBone);
        if (hitBone >= 0 && (size_t)(hitBone * 16 + 16) <= foeSkinMats_.size()) {
            matInvAffine(&foeSkinMats_[hitBone * 16], invBone);
        }
        matTransformPoint(invBone, wA, e.pos);
        matTransformPoint(invBone, wB, e.posB);
        for (int k = 0; k < 3; k++) e.worldPos[k] = (wA[k] + wB[k]) * 0.5f;
        std::memcpy(e.outDir, nrm, sizeof(e.outDir)); // gobs spray off the cut
        if (foe_.editInBounds(e)) foe_.queueEdit(e);
    }
}

void Renderer::updateConservation(const LookParams& look, const FrameInfo& frame) {
    absorbMeasured();
    // conservation off = pre-M4.6 vanish behavior: stop spawning and shed the
    // debt (so a later re-enable doesn't erupt a backlog of gobs at once)
    if (!look.conserveClay) {
        sploot_.debt = 0.f;
        haveWound_ = false;
    }

    float dt = 0.f;
    if (lastSimTime_ >= 0.f)
        dt = std::min(std::max(frame.time - lastSimTime_, 0.f), 0.05f);
    lastSimTime_ = frame.time;
    bool poseStep = frame.poseTime != lastPoseTime_;
    lastPoseTime_ = frame.poseTime;

    // spawn gobs while the ledger owes clay: 2..35 ml each (r ~ 8..20 mm)
    const float kMinGob = 2.0e-6f, kMaxGob = 3.5e-5f;
    auto rnd = [this]() {
        gobSeed_ = gobSeed_ * 1664525u + 1013904223u;
        return (float)(gobSeed_ >> 8) * (1.f / 16777216.f);
    };
    while (haveWound_ && sploot_.debt > kMinGob && (int)gobs_.size() < 12) {
        float v = std::min(sploot_.debt, kMaxGob);
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
    std::vector<PosedCap> caps;
    caps.reserve(capsules_.size());
    for (const BoneCapsule& c : capsules_) {
        PosedCap pc;
        std::memcpy(pc.a, c.a, sizeof(pc.a));
        std::memcpy(pc.b, c.b, sizeof(pc.b));
        pc.r = c.r;
        pc.bone = c.bone;
        if (c.bone >= 0 && (size_t)(c.bone * 16 + 16) <= skinMats_.size()) {
            matTransformPoint(&skinMats_[c.bone * 16], c.a, pc.a);
            matTransformPoint(&skinMats_[c.bone * 16], c.b, pc.b);
        }
        caps.push_back(pc);
    }
    // sticking writes rest-space voxels, so only an un-posed body may catch
    // gobs (same "pause to sculpt" rule the carve tool lives with)
    bool resting = !look.animPlay || bones_.empty();

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
                if (resting) {
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
                    if (pc.bone >= 0 && (size_t)(pc.bone * 16 + 16) <= skinMats_.size()) {
                        float inv[16];
                        matInvAffine(&skinMats_[pc.bone * 16], inv);
                        matTransformPoint(inv, g.pos, rest);
                    }
                    std::memcpy(e.pos, rest, sizeof(e.pos));
                    std::memcpy(e.color, g.col, sizeof(e.color));
                    if (brick_.editInBounds(e)) {
                        brick_.queueEdit(e);
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

        // landing: floor mosaic or an existing pile (coarse CPU mirror)
        float top = ground_.approxTopAt(g.pos[0], g.pos[2]);
        if ((g.pos[1] - 0.4f * g.radius < top && g.vel[1] <= 0.f) ||
            g.pos[1] < -0.1f) {
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
        fighterDisp_ = fighter_; // 60 Hz slide (pre-M-PERF behaviour)
        foeDisp_ = foePose_;
    } else if (frame.poseTime != dispPoseTime_) {
        dispPoseTime_ = frame.poseTime;
        fighterDisp_ = fighter_;
        foeDisp_ = foePose_;
    }

    // pose the skeleton at the quantized clock; paused = rest pose
    resolveSword(look);
    if (!bones_.empty()) {
        // locomotion picks the clip: bounce while travelling, idle at rest.
        // Falls back to the first clip when the asset has neither name.
        int ci = -1;
        if (look.animPlay && !clips_.empty()) {
            ci = clipIndex(fighterDisp_.moving ? "bounce" : "idle");
            if (ci < 0) ci = 0;
        }
        const AnimClip* clip = (ci >= 0 && clips_[ci].duration > 0.f) ? &clips_[ci] : nullptr;
        if (clip) {
            animT_ = std::fmod(frame.poseTime * look.animSpeed, clip->duration);
        }
        evalPose(bones_, clip, animT_, skinMats_);

        // root: lean about world X (before yaw), then yaw, then translate.
        // Applied to every skin matrix, so the clip stays authored in
        // character space and everything downstream is already world.
        {
            const float cy = std::cos(fighterDisp_.yaw), sy = std::sin(fighterDisp_.yaw);
            const float cl = std::cos(fighterDisp_.lean), sl = std::sin(fighterDisp_.lean);
            // R = yaw(Y) * lean(X), column-major
            float R[16] = {cy,        0.f, -sy,       0.f,
                           sy * sl,   cl,  cy * sl,   0.f,
                           sy * cl,   -sl, cy * cl,   0.f,
                           fighterDisp_.pos[0], fighterDisp_.pos[1], fighterDisp_.pos[2], 1.f};
            for (size_t b = 0; b * 16 + 16 <= skinMats_.size(); b++) {
                float tmp[16];
                matMul(R, &skinMats_[b * 16], tmp);
                std::memcpy(&skinMats_[b * 16], tmp, sizeof(tmp));
            }
        }

        // M4.7: sword is master, hands follow. The mitts are detached, so IK
        // does not bend a chain — it teleports each hand onto its grip, held
        // back only by the reach ball about the blob's centre of mass. The
        // body stays on the clip. Marbles/capsules below read the IK'd
        // skinMats_.
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
            const float cyG = std::cos(fighterDisp_.yaw), syG = std::sin(fighterDisp_.yaw);
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
                // fingers wrap the handle; mirrored so both curl inward
                c.curl = right ? -look.hands.gripCurl : look.hands.gripCurl;
                c.palmLen = c.restSpan * look.hands.palmFrac;
            }
            float com[3];
            evalBodyCom(bodyCom_, skinMats_, com);
            float reach = look.hands.reach > 0.f
                              ? look.hands.reach
                              : autoReach_ * look.hands.reachScale;
            applyHandIk(bones_, handIk_, com, reach, look.hands.orient, skinMats_);
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
            applyGaze(bones_, gaze_, gazeTarget_, look.gaze.maxAngle, skinMats_);
        }
    }

    // Player 1 poses itself: its own clip clock, its own skeleton, its own
    // root. It stands and idles rather than following the hero's animation.
    if (foeEnabled_ && !bones_.empty()) {
        int ci = clipIndex(foeDisp_.moving ? "bounce" : "idle");
        if (ci < 0) ci = 0;
        const AnimClip* clip =
            (look.animPlay && !clips_.empty() && clips_[ci].duration > 0.f)
                ? &clips_[ci]
                : nullptr;
        if (clip) {
            // offset the phase so two identical fighters don't breathe in
            // lockstep — that reads as one puppet duplicated, not two actors
            foeAnimT_ = std::fmod(frame.poseTime * look.animSpeed + 0.37f,
                                  clip->duration);
        }
        evalPose(bones_, clip, foeAnimT_, foeSkinMats_);
        const float cy = std::cos(foeDisp_.yaw), sy = std::sin(foeDisp_.yaw);
        const float cl = std::cos(foeDisp_.lean), sl = std::sin(foeDisp_.lean);
        float R[16] = {cy,      0.f, -sy,     0.f,
                       sy * sl, cl,  cy * sl, 0.f,
                       sy * cl, -sl, cy * cl, 0.f,
                       foeDisp_.pos[0], foeDisp_.pos[1], foeDisp_.pos[2], 1.f};
        for (size_t b = 0; b * 16 + 16 <= foeSkinMats_.size(); b++) {
            float tmp[16];
            matMul(R, &foeSkinMats_[b * 16], tmp);
            std::memcpy(&foeSkinMats_[b * 16], tmp, sizeof(tmp));
        }
    }

    // the blade cuts fighter 1 before conservation, so a fresh wound is on
    // the ledger the same frame it is made
    updateBladeCut(look);

    // conservation runs before packing so this frame's uniforms carry fresh
    // gob positions and the ground bound
    updateConservation(look, frame);

    float uniforms[kUniformSlots][4];
    packUniforms(cam, look, frame, uniforms);
    gpu_->queue.WriteBuffer(uniformBuf_, 0, uniforms, sizeof(uniforms));

    wgpu::CommandEncoder encoder = gpu_->device.CreateCommandEncoder();
    brick_.encode(encoder);
    foe_.encode(encoder); // fighter 1's own import/edit/JFA passes
    ground_.encode(encoder);

    // ---- 12 Hz frame reuse ----
    // Fold the volume generations in: a carve or a landed gob changes what the
    // tracer would see without changing a single uniform. The generations are
    // read AFTER encode() above, so an edit queued this frame re-traces this
    // frame rather than a frame late.
    uint64_t digest = traceInputDigest(uniforms);
    for (uint32_t g : {brick_.generation(), foe_.generation(), ground_.generation()}) {
        digest = (digest ^ g) * 1099511628211ull;
    }
    const bool reuse = reuseEnabled_ && traceValid_ && digest == traceDigest_;
    framesPresented_++;
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
        pass.SetPipeline(tracePipeline_);
        pass.SetBindGroup(0, traceBind_);
        pass.SetBindGroup(1, traceBrickBind_);
        pass.SetBindGroup(2, traceFoeBind_);
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
    brick_.finishCapacityPoll();
    brick_.pollVolumes();
    foe_.pollVolumes();
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

    bool ok = false;
    wgpu::Future f = readback.MapAsync(
        wgpu::MapMode::Read, 0, bufSize, wgpu::CallbackMode::WaitAnyOnly,
        [&ok](wgpu::MapAsyncStatus status, wgpu::StringView msg) {
            ok = status == wgpu::MapAsyncStatus::Success;
            if (!ok)
                std::fprintf(stderr, "readback map failed: %.*s\n", (int)msg.length,
                             msg.data);
        });
    gpu_->instance.WaitAny(f, UINT64_MAX);
    if (!ok) return false;

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
}

void Renderer::syncMeasurements() {
    // Copies were submitted with the frame; they only need the event pump,
    // not more renders. Bounded wait so a lost map can't hang the app.
    for (int guard = 0; !brick_.measurementsIdle() && guard < 20000; guard++) {
        gpu_->processEvents();
        brick_.pollVolumes();
        std::this_thread::sleep_for(std::chrono::microseconds(250));
    }
    if (!brick_.measurementsIdle()) {
        std::fprintf(stderr, "[snap] warning: volume measurements still in flight\n");
    }
}

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
    if (!brick_.save(w) || !ground_.save(w)) {
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
    rc.animT = animT_;
    rc.simT = simT;
    w.section("RCPU", &rc, sizeof(rc));
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
    if (!brick_.load(r) || !ground_.load(r)) return false;
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
    animT_ = rc.animT;
    // fresh dt baseline: the restored clock may sit anywhere on the timeline
    lastSimTime_ = -1.f;
    lastPoseTime_ = -1.f;
    if (simT) *simT = rc.simT;
    std::printf("[snap] loaded %s (t=%.2fs, %zu gob(s))\n", path.c_str(), rc.simT,
                gobs_.size());
    return true;
}

int Renderer::addPlayer(const FighterPose& at) {
    if (playerCount_ >= kMaxPlayers) {
        std::fprintf(stderr,
                     "addPlayer: %d is the volume budget (see PLAN.md "
                     "\"fighters are SLICES\" to lift it)\n",
                     kMaxPlayers);
        return -1;
    }
    foePose_ = at;
    foeEnabled_ = true;
    return playerCount_++;
}
