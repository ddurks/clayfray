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
// Slot -> what lives there, so a re-trace report reads as "gobs" rather than
// "slot 271". Mirrors the Uniforms layout (trap 2); boundaries match the
// hardcoded writes in packUniforms.
const char* Renderer::uniformSlotName(int s) {
    static const char* head[14] = {"camPos",   "camRight", "camUp",    "camFwd",
                                   "res",      "keyPos",   "keyColor", "rimDir",
                                   "rimColor", "ambient",  "material", "post",
                                   "post2",    "mouse"};
    if (s < 14) return head[s];
    if (s < 30) return "marbles (eyes)";
    if (s == 30) return "marbleMeta";
    if (s == 31) return "capsMeta";
    if (s == 32) return "capsCenter";
    if (s < 65) return "capsules (hero shadow proxy)";
    if (s == 65) return "boneMeta";
    if (s < 258) return "pieces (hero articulation)";
    if (s == 258) return "gobMeta (in-flight count)";
    if (s < 283) return "gobs (flying clay)";
    if (s == 283) return "groundMeta (clay top bound)";
    if (s == 284) return "swordA (hilt)";
    if (s == 285) return "swordB (tip)";
    if (s == 286) return "swordCol";
    if (s < 291) return "foeInv";
    if (s == 291) return "foeMeta";
    if (s == 292) return "foeCenter";
    if (s < 485) return "pieces (foe articulation)";
    if (s == 485) return "foeBoneMeta";
    return "rigMeta (affine rig select)";
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
    if (affinePieces_.empty()) return false;
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

void Renderer::updateBrushRig(std::vector<AffinePiece>& pieces, const int handPose[2],
                              const FighterPose& disp, const BodySpring& s,
                              const LookParams& look, const SwordParams* grip) const {
    if (!brush_.valid || pieces.size() < 3) return;
    float A[16];
    bodyAffine(disp, s, look.rig, A);
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
        const int b = (handPose[side] != 0) ? 1 : 0;
        for (int k = 0; k < 3; k++) {
            ap.lo[k] = brush_.handLo[b][k];
            ap.hi[k] = brush_.handHi[b][k];
        }
        // Mirror in x for the right hand. det = -1, but M^T M = I so every
        // singular value is 1: distance-preserving, and mat3MinSingular (which
        // forms M^T M) returns exactly 1 for it. Confirmed, not assumed.
        const float mir = (side == 1) ? -1.f : 1.f;

        // Pre-translation applied in BRUSH space, before the mirror.
        //   holding: brush -> canonical -> palm at the origin, so the grip
        //            transform below lands the palm exactly on the hilt.
        //   riding:  brush -> canonical ONLY. The hand keeps its authored
        //            offset from the body, and mirroring about x=0 (the body's
        //            own axis) puts the other one on the other side.
        float pre[3];
        for (int k = 0; k < 3; k++) {
            pre[k] = -brush_.ofs[b][k] - (grip ? brush_.palm[b][k] : 0.f);
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
            float col0[3] = {offs[0], offs[1], offs[2]};
            float col1[3] = {blade[0], blade[1], blade[2]};
            float col2[3] = {col0[1] * col1[2] - col0[2] * col1[1],
                             col0[2] * col1[0] - col0[0] * col1[2],
                             col0[0] * col1[1] - col0[1] * col1[0]};
            // columns (fingerDir, bladeDir, fingerDir x bladeDir) — orthonormal
            // and det +1, so the ONLY reflection in the piece is the explicit
            // mirror below.
            M[0] = col0[0] * mir; M[1] = col0[1] * mir; M[2] = col0[2] * mir; M[3] = 0.f;
            M[4] = col1[0];       M[5] = col1[1];       M[6] = col1[2];       M[7] = 0.f;
            M[8] = col2[0];       M[9] = col2[1];       M[10] = col2[2];      M[11] = 0.f;
            for (int k = 0; k < 3; k++) M[12 + k] = g[k] + offs[k] * lateral;
            M[15] = 1.f;
        } else {
            // No sword: the mitt just rides the body, mirrored, where authored.
            float Mir[16];
            matIdentity(Mir);
            Mir[0] = mir;
            matMul(A, Mir, M);
        }
        // fold the pre-translation in: xform = M * T(pre)
        float T[16];
        matIdentity(T);
        T[12] = pre[0]; T[13] = pre[1]; T[14] = pre[2];
        matMul(M, T, ap.xform);
    }
}

void Renderer::stepSpring(BodySpring& s, const RigParams& r, bool moving, float dt) {
    if (dt <= 0.f) return;
    // Metronome: one impulse per footfall while walking, one slower and
    // gentler per breath at rest. The phase is the only clock — no wall
    // clock, no rand() — so replay reproduces the squish exactly.
    const float hz = moving ? r.gaitHz : r.idleHz;
    const float kick = r.squishKick * (moving ? 1.f : r.idleScale);
    const float next = s.gait + dt * hz;
    if (next >= 1.f) s.v -= kick;      // land / inhale: compress
    s.gait = next - std::floor(next);
    // Semi-implicit Euler, substepped. dt is a whole 12 Hz pose step (~83 ms)
    // and the spring's period is ~0.8 s, so one step per frame would be a
    // visibly lumpy integration; four is stable and still fixed-count, which
    // is what keeps it deterministic.
    const int kSub = 4;
    const float h = dt / (float)kSub;
    for (int i = 0; i < kSub; i++) {
        s.v += (-r.squishK * s.q - r.squishDamp * s.v) * h;
        s.q += s.v * h;
    }
    s.q = std::min(std::max(s.q, -0.45f), 0.45f);
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
void Renderer::bodyAffine(const FighterPose& disp, const BodySpring& s,
                          const RigParams& r, float out[16]) const {
    const float ky = std::min(std::max(1.f + s.q, 0.55f), 1.45f);
    // sideways bulge: squashing down pushes clay out. Not strictly volume
    // preserving — `widen` is a taste knob, and clay is not water.
    const float kxz = std::min(std::max(1.f - r.widen * s.q, 0.55f), 1.65f);
    const float t = std::tan(disp.lean);
    // the hop rides the RELEASE half of the spring only, and only when
    // travelling: an idle breath must not lift the feet off the floor
    const float hop = disp.moving ? std::max(s.q, 0.f) * r.hop : 0.f;
    const float cy = std::cos(disp.yaw), sy = std::sin(disp.yaw);
    out[0] = kxz * cy;      out[1] = 0.f;  out[2] = -kxz * sy;    out[3] = 0.f;
    out[4] = t * ky * sy;   out[5] = ky;   out[6] = t * ky * cy;  out[7] = 0.f;
    out[8] = kxz * sy;      out[9] = 0.f;  out[10] = kxz * cy;    out[11] = 0.f;
    out[12] = disp.pos[0];
    out[13] = disp.pos[1] + hop;
    out[14] = disp.pos[2];
    out[15] = 1.f;
}

int Renderer::packAffinePieces(float out[kUniformSlots][4], int base,
                               const std::vector<AffinePiece>& pieces,
                               const std::vector<float>& mats) const {
    int n = 0;
    for (const AffinePiece& ap : pieces) {
        if (n >= 16) break;
        float fwd[16];
        if (ap.srcBone >= 0 && (size_t)(ap.srcBone * 16 + 16) <= mats.size()) {
            std::memcpy(fwd, &mats[ap.srcBone * 16], sizeof(fwd));
        } else {
            // brush rig: the transform was written straight into the piece
            std::memcpy(fwd, ap.xform, sizeof(fwd));
        }
        float inv[16];
        matInvAffine(fwd, inv);
        std::memcpy(out[base + n * 12], inv, 16 * sizeof(float));
        std::memcpy(out[base + n * 12 + 4], fwd, 16 * sizeof(float));
        float* lo = out[base + n * 12 + 8];
        float* hi = out[base + n * 12 + 9];
        float* ca = out[base + n * 12 + 10];
        float* cb = out[base + n * 12 + 11];
        for (int k = 0; k < 3; k++) {
            lo[k] = ap.lo[k];
            hi[k] = ap.hi[k];
        }
        // Lipschitz rescale — the SMALLEST SINGULAR VALUE, not the min column
        // norm the 13-piece path packs here. A shear can have unit-length
        // columns and a much smaller sigmaMin; trusting the column norm would
        // overestimate world distances and the march would tunnel. Floored at
        // 0.25 so a degenerate matrix cannot stall the trace to a crawl.
        lo[3] = std::min(std::max(mat3MinSingular(fwd), 0.25f), 1.f);
        // Rest bounding sphere of this piece's clay. Unused by the affine
        // sampling path today; it is where a baked hand-pose volume would
        // declare its extent when the mitts move out of the shared volume.
        for (int k = 0; k < 3; k++) ca[k] = (ap.lo[k] + ap.hi[k]) * 0.5f;
        float hd = 0.f;
        for (int k = 0; k < 3; k++) {
            const float h = (ap.hi[k] - ap.lo[k]) * 0.5f;
            hd += h * h;
        }
        ca[3] = std::sqrt(hd);
        for (int k = 0; k < 3; k++) cb[k] = ca[k];
        // capB.w is a BONE MASK in this mode, not a bone id — the shader's
        // ownership test ANDs it with 1 << dominantBone.
        cb[3] = (float)ap.boneMask;
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
    // The eyes are the four beads: two per fighter under the bone rig, and
    // under the brush rig two per fighter as well (the artist authors the LEFT
    // eye's pupil + eyeball and the importer mirrors both across x).
    //
    // Skeleton-free, a bead has no bone to ride, so it rides the BODY piece's
    // affine — which is what carries the squish, lean, yaw and hop. Radius is
    // deliberately NOT scaled by it: glass beads don't breathe with the torso.
    const bool brushRig = skeletonFree();
    int n = 0;
    auto packMarbles = [&](const std::vector<float>& mats,
                           const std::vector<AffinePiece>& pieces) {
        for (const MarbleProp& m : marbles_) {
            if (n >= 8) break;
            float* slotA = out[14 + n * 2];
            float* slotB = out[15 + n * 2];
            float pos[3] = {m.pos[0], m.pos[1], m.pos[2]};
            if (m.bone >= 0 && (size_t)(m.bone * 16 + 16) <= mats.size()) {
                matTransformPoint(&mats[m.bone * 16], m.pos, pos);
            } else if (brushRig && !pieces.empty()) {
                matTransformPoint(pieces[0].xform, m.pos, pos);
            }
            slotA[0] = pos[0]; slotA[1] = pos[1]; slotA[2] = pos[2];
            slotA[3] = m.radius;
            slotB[0] = m.color[0]; slotB[1] = m.color[1]; slotB[2] = m.color[2];
            n++;
        }
    };
    packMarbles(skinMats_, affinePieces_);
    if (foeEnabled_ && (!foeSkinMats_.empty() || brushRig))
        packMarbles(foeSkinMats_, foeAffinePieces_);
    out[30][0] = (float)n;

    // Posed capsule shadow proxy. Under the brush rig `bone` carries the PIECE
    // index instead of a bone index (see setCharacter), so the capsule rides
    // the same transform as the clay it stands in for.
    int cn = std::min((int)capsules_.size(), 16);
    float center[3] = {0.f, 0.f, 0.f};
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
            if (c.bone >= 0 && (size_t)c.bone < affinePieces_.size()) {
                const AffinePiece& ap = affinePieces_[c.bone];
                float ca[3], cb[3], cr;
                capsuleFromBox(ap.lo, ap.hi, ca, cb, cr);
                matTransformPoint(ap.xform, ca, a);
                matTransformPoint(ap.xform, cb, b);
                float* sA = out[33 + i * 2];
                float* sB = out[34 + i * 2];
                sA[0] = a[0]; sA[1] = a[1]; sA[2] = a[2]; sA[3] = cr;
                sB[0] = b[0]; sB[1] = b[1]; sB[2] = b[2];
                for (int k = 0; k < 3; k++) center[k] += (a[k] + b[k]) * 0.5f;
                continue;
            }
        } else if (c.bone >= 0 && (size_t)(c.bone * 16 + 16) <= skinMats_.size()) {
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
    const bool affine = affineOn(look);
    // Skeleton-free with the rig switched off: pn = 0 puts the shader on its
    // un-rigged path (the rest volume drawn where it was authored, all three
    // brushes visible side by side). That is the debug A/B described in
    // affineOn — NOT a fallback anything ships with.
    int pn = (brushRig || noPieces) ? 0 : cn;
    float bodyBoundR = 0.f;
    if (affine && !noPieces) {
        // Three pieces replace the per-capsule ones. The posed bound comes off
        // the pieces' own rest boxes rather than the capsule endpoints below,
        // because a squish can push clay past any bound measured at rest.
        pn = packAffinePieces(out, 66, affinePieces_, skinMats_);
        bodyBoundR = affineBoundR(affinePieces_, skinMats_, center);
    }
    // ...otherwise the M4-P1 per-capsule pieces, one per bone.
    const int legacyPn = (affine || brushRig) ? 0 : pn;
    for (int i = 0; i < legacyPn; i++) {
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
    if (affine && foeEnabled_ && !noPieces && brushRig) {
        fpn = packAffinePieces(out, 293, foeAffinePieces_, foeSkinMats_);
    } else if (affine && foeEnabled_ && !noPieces &&
               foeSkinMats_.size() >= bones_.size() * 16) {
        fpn = packAffinePieces(out, 293, affinePieces_, foeSkinMats_);
    } else if (foeEnabled_ && !noPieces && !brushRig &&
               foeSkinMats_.size() >= bones_.size() * 16) {
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

    // M-PERF rig select (slot 486). Appended, never inserted, so no earlier
    // index moves — and mirrored into BOTH shader roots (trap 2). y is the
    // count of baked hand-pose volumes; 0 means the mitts are sampled out of
    // the shared rest volume, which is the only mode today's asset supports.
    out[486][0] = affine ? 1.f : 0.f;
    out[486][1] = 0.f;

    // M4.6 conservation: in-flight gobs (12 Hz-stepped positions) + the
    // ground field's clay top bound (0 disables the field in the tracer).
    // These slot indices are hand-mirrored in trace.wgsl AND pick.wgsl —
    // this static_assert catches only the C++ side overrunning the buffer;
    // the WGSL side has no compile-time link, so CLAUDE.md flags the mirror.
    static_assert(kUniformSlots >= 487,
                  "gobs reach out[282], groundMeta out[283], sword out[284-286], "
                  "foePieces out[293-485], rigMeta out[486]; keep kUniformSlots "
                  "and the Uniforms struct in trace.wgsl + pick.wgsl in sync");
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
    if (swordWorld_.enabled && (!bones_.empty() || brushRig)) {
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
        // Under the affine rig the yaw-only inverse above no longer describes
        // the foe's body — a squish is not a rotation — so re-derive the
        // reject sphere from its actual posed pieces. Too SMALL a sphere
        // clips the opponent out of the frame, so this has to track the
        // squish rather than reuse the rest-mesh radius.
        if (affine && fpn > 0 &&
            (brushRig || foeSkinMats_.size() >= bones_.size() * 16)) {
            const std::vector<AffinePiece>& fp =
                brushRig ? foeAffinePieces_ : affinePieces_;
            float m[16];
            if (brushRig) {
                if (fp.empty()) matIdentity(m);
                else std::memcpy(m, fp[0].xform, sizeof(m));
            } else {
                const int sb = fp.empty() ? -1 : fp[0].srcBone;
                if (sb >= 0 && (size_t)(sb * 16 + 16) <= foeSkinMats_.size()) {
                    std::memcpy(m, &foeSkinMats_[sb * 16], sizeof(m));
                } else {
                    matIdentity(m);
                }
            }
            float c[3];
            matTransformPoint(m, foeCenterRest_, c);
            out[292][0] = c[0]; out[292][1] = c[1]; out[292][2] = c[2];
            out[291][1] = affineBoundR(fp, foeSkinMats_, c) + 0.05f;
        }
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

    // ---- M-RIG: the skeleton-free brush rig ----
    // Built entirely from the imported MeshParts, so it needs no armature. Done
    // FIRST because it decides whether any of the bone machinery below runs at
    // all (on this asset none of it does: every loop is over an empty bones_).
    brush_ = BrushRig{};
    affinePieces_.clear();
    foeAffinePieces_.clear();
    if (asset.hasBrushRig()) {
        const MeshPart& body = asset.parts[asset.partBody];
        const MeshPart& hr = asset.parts[asset.partHandRest];
        const MeshPart& hg = asset.parts[asset.partHandGrab];
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
        setBox(hr, brush_.handLo[0], brush_.handHi[0]);
        setBox(hg, brush_.handLo[1], brush_.handHi[1]);
        for (int k = 0; k < 3; k++) {
            brush_.ofs[0][k] = 0.f;
            brush_.ofs[1][k] = kGrabBrushOffset[k];
        }
        for (int b = 0; b < 2; b++) {
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
        affinePieces_.resize(3);
        for (int i = 0; i < 3; i++) affinePieces_[i].srcBone = -1;
        foeAffinePieces_ = affinePieces_;

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
        // Rest-pose defaults. packUniforms REFITS the a/b/r of each of these
        // every frame from its piece's current brush box, so these values only
        // serve foeCaps_ (the rest capsules used for blade hit tests).
        addCapsule(brush_.bodyLo, brush_.bodyHi, 0);
        addCapsule(brush_.handLo[0], brush_.handHi[0], 1);
        addCapsule(brush_.handLo[0], brush_.handHi[0], 2);

        std::printf("rig: brush rig — body (%.3f %.3f %.3f)..(%.3f %.3f %.3f), "
                    "hand rest x[%.3f %.3f], hand grab x[%.3f %.3f], "
                    "%zu marbles\n",
                    brush_.bodyLo[0], brush_.bodyLo[1], brush_.bodyLo[2],
                    brush_.bodyHi[0], brush_.bodyHi[1], brush_.bodyHi[2],
                    brush_.handLo[0][0], brush_.handHi[0][0], brush_.handLo[1][0],
                    brush_.handHi[1][0], marbles_.size());
        // Fail LOUDLY rather than rendering a silently clipped fighter: a brush
        // that pokes outside the volume box, or two brushes whose padded boxes
        // touch, both break the clip's exactness (CLAUDE.md trap 5 drops edits
        // near the boundary for the same reason).
        const float vlo[3] = {BrickSystem::kOrigin[0], BrickSystem::kOrigin[1],
                              BrickSystem::kOrigin[2]};
        const float vhi[3] = {vlo[0] + BrickSystem::kExtent,
                              vlo[1] + BrickSystem::kExtent,
                              vlo[2] + BrickSystem::kExtent};
        const float* boxes[3][2] = {{brush_.bodyLo, brush_.bodyHi},
                                    {brush_.handLo[0], brush_.handHi[0]},
                                    {brush_.handLo[1], brush_.handHi[1]}};
        static const char* bnames[3] = {"body", "hand.rest", "hand.grab"};
        for (int i = 0; i < 3; i++) {
            for (int k = 0; k < 3; k++) {
                if (boxes[i][0][k] < vlo[k] || boxes[i][1][k] > vhi[k]) {
                    std::fprintf(stderr,
                                 "[rig] WARNING brush '%s' axis %d leaves the volume "
                                 "box (%.4f..%.4f vs %.4f..%.4f) — it will render "
                                 "clipped\n",
                                 bnames[i], k, boxes[i][0][k], boxes[i][1][k], vlo[k],
                                 vhi[k]);
                }
            }
            for (int j = i + 1; j < 3; j++) {
                bool overlap = true;
                for (int k = 0; k < 3; k++)
                    if (boxes[i][1][k] < boxes[j][0][k] || boxes[j][1][k] < boxes[i][0][k])
                        overlap = false;
                if (overlap) {
                    std::fprintf(stderr,
                                 "[rig] WARNING brushes '%s' and '%s' overlap — the "
                                 "AABB clip is no longer an exact separation\n",
                                 bnames[i], bnames[j]);
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
    // ---- M-PERF: the BONE-partitioned affine rig (legacy assets only) ----
    // Partition a rigged asset into an affine body plus one rigid piece per
    // mitt, by BONE. Skipped entirely when the brush rig already built the
    // pieces above — which is the case for the current fighter, and the guard
    // matters: this used to start with an unconditional clear() that would
    // silently throw the brush pieces away.
    if (!bones_.empty()) {
        affinePieces_.clear();
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
        // one bit per bone: the shader ANDs this with 1 << dominant bone of
        // the cell it is sampling. 16 bones is the shader-side cap.
        for (size_t b = 0; b < nb && b < 16; b++)
            pieces[owner[b]].boneMask |= 1u << b;
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
        // bound to) would carry an inverted box and poison the cull, so fold
        // its bones back into the body and drop it.
        for (size_t i = 1; i < pieces.size(); i++) {
            if (pieces[i].hi[0] < pieces[i].lo[0]) {
                pieces[0].boneMask |= pieces[i].boneMask;
                pieces[i].boneMask = 0;
            }
        }
        if (pieces[0].hi[0] >= pieces[0].lo[0]) {
            for (const AffinePiece& ap : pieces)
                if (ap.boneMask != 0 && ap.hi[0] >= ap.lo[0]) affinePieces_.push_back(ap);
        }
        if (bones_.size() > 16 && !affinePieces_.empty()) {
            std::fprintf(stderr,
                         "[rig] %zu bones exceeds the 16-bone ownership mask; "
                         "falling back to the 13-piece warp\n",
                         bones_.size());
        }
    }
    std::printf("anim: %zu bones, %zu clip(s), %zu shadow capsules, %zu hand(s), "
                "rest reach %.3f m, %zu affine piece(s)\n",
                bones_.size(), clips_.size(), capsules_.size(), handIk_.size(),
                autoReach_, affinePieces_.size());
    // fighter 1 is the same character in its own volume — "identical second
    // fighter". Rest capsules are shared (same mesh), and the bound sphere
    // comes off the mesh so the tracer's cheap reject is tight.
    foeCaps_ = capsules_;
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
    // Blade tests happen in WORLD space, so the opponent's rest capsules have
    // to be posed first. Under the brush rig `bone` is a PIECE index into the
    // foe's own piece table — falling through to identity here would leave the
    // foe's hitboxes sitting at the origin while the foe itself stands
    // elsewhere, and the sword would cut empty air.
    const bool brushRig = skeletonFree();
    auto foeToWorld = [&](const BoneCapsule& c, const float* r, float* w) {
        if (brushRig) {
            if (c.bone >= 0 && (size_t)c.bone < foeAffinePieces_.size()) {
                matTransformPoint(foeAffinePieces_[c.bone].xform, r, w);
                return;
            }
        } else if (c.bone >= 0 && (size_t)(c.bone * 16 + 16) <= foeSkinMats_.size()) {
            matTransformPoint(&foeSkinMats_[c.bone * 16], r, w);
            return;
        }
        w[0] = r[0]; w[1] = r[1]; w[2] = r[2];
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
        if (brushRig) {
            if (hitBone >= 0 && (size_t)hitBone < foeAffinePieces_.size()) {
                matInvAffine(foeAffinePieces_[hitBone].xform, invBone);
            }
        } else if (hitBone >= 0 && (size_t)(hitBone * 16 + 16) <= foeSkinMats_.size()) {
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
    // Spawning changes the in-flight count, which the tracer reads, so it
    // belongs on the pose grid too — otherwise the array refills at 60 Hz as
    // fast as landings drain it and reuse never recovers.
    while (poseStep && haveWound_ && sploot_.debt > kMinGob &&
           (int)gobs_.size() < 12) {
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
            if (c.bone >= 0 && (size_t)c.bone < affinePieces_.size()) {
                const AffinePiece& ap = affinePieces_[c.bone];
                float ca[3], cb[3];
                capsuleFromBox(ap.lo, ap.hi, ca, cb, pc.r);
                matTransformPoint(ap.xform, ca, pc.a);
                matTransformPoint(ap.xform, cb, pc.b);
            }
        } else if (c.bone >= 0 && (size_t)(c.bone * 16 + 16) <= skinMats_.size()) {
            matTransformPoint(&skinMats_[c.bone * 16], c.a, pc.a);
            matTransformPoint(&skinMats_[c.bone * 16], c.b, pc.b);
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
                        if (pc.bone >= 0 && (size_t)pc.bone < affinePieces_.size()) {
                            float inv[16];
                            matInvAffine(affinePieces_[pc.bone].xform, inv);
                            matTransformPoint(inv, g.pos, rest);
                        }
                    } else if (pc.bone >= 0 &&
                               (size_t)(pc.bone * 16 + 16) <= skinMats_.size()) {
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
        fighterDisp_ = fighter_; // 60 Hz slide (pre-M-PERF behaviour)
        foeDisp_ = foePose_;
    } else if (frame.poseTime != dispPoseTime_) {
        dispPoseTime_ = frame.poseTime;
        fighterDisp_ = fighter_;
        foeDisp_ = foePose_;
    }

    // M-PERF: advance the affine body's squish spring. ON THE POSE GRID, not
    // the frame clock (trap 4): the squish is a traced uniform input, so
    // integrating it at 60 Hz would make a STANDING fighter re-trace every
    // frame and hand back the idle frame rate that reuse buys. Fixed substeps,
    // no wall clock, no RNG, so --replay reproduces it exactly.
    const bool affine = affineOn(look);
    if (affine && frame.poseTime != springPoseTime_) {
        const float dt =
            (springPoseTime_ >= 0.f)
                ? std::min(std::max(frame.poseTime - springPoseTime_, 0.f), 0.25f)
                : 0.f;
        springPoseTime_ = frame.poseTime;
        stepSpring(spring_, look.rig, fighter_.moving, dt);
        stepSpring(foeSpring_, look.rig, foePose_.moving, dt);
    }

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
            handPose_[0] = handPose_[1] = holding ? 1 : 0;
            // the opponent is not holding anything yet, so it keeps rest hands
            foeHandPose_[0] = foeHandPose_[1] = 0;
        }
        updateBrushRig(affinePieces_, handPose_, fighterDisp_, spring_, look,
                       (swordWorld_.enabled && look.hands.ik) ? &swordWorld_ : nullptr);
        if (foeEnabled_) {
            updateBrushRig(foeAffinePieces_, foeHandPose_, foeDisp_, foeSpring_, look,
                           nullptr);
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
            evalPose(bones_, nullptr, 0.f, skinMats_);
            float A[16];
            bodyAffine(fighterDisp_, spring_, look.rig, A);
            for (size_t b = 0; b * 16 + 16 <= skinMats_.size(); b++) {
                float tmp[16];
                matMul(A, &skinMats_[b * 16], tmp);
                std::memcpy(&skinMats_[b * 16], tmp, sizeof(tmp));
            }
        } else {
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
                // Fingers wrap the handle; mirrored so both curl inward.
                //
                // ZERO under the affine rig, and it has to be. The mitt is one
                // piece there, sampled through the WRIST's transform alone, so
                // a per-digit rotation would move the shadow capsules and the
                // COM to a grip the clay never adopts — a curled proxy over
                // straight fingers. Zeroing it keeps the whole rig telling one
                // story: applyHandIk then moves every mitt bone by the same
                // matrix, which is exactly what the one-transform piece
                // assumes. Getting the curl BACK is what the baked hand-pose
                // volumes are for (rigMeta.y) — a discrete grip shape selected
                // by index, not a joint rotation.
                c.curl = affine ? 0.f
                                : (right ? -look.hands.gripCurl : look.hands.gripCurl);
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
    if (foeEnabled_ && !bones_.empty() && affine) {
        // same collapse as the hero, driven by ITS pose and ITS spring
        evalPose(bones_, nullptr, 0.f, foeSkinMats_);
        float A[16];
        bodyAffine(foeDisp_, foeSpring_, look.rig, A);
        for (size_t b = 0; b * 16 + 16 <= foeSkinMats_.size(); b++) {
            float tmp[16];
            matMul(A, &foeSkinMats_[b * 16], tmp);
            std::memcpy(&foeSkinMats_[b * 16], tmp, sizeof(tmp));
        }
    } else if (foeEnabled_ && !bones_.empty()) {
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
        const uint32_t gens[3] = {brick_.generation(), foe_.generation(),
                                  ground_.generation()};
        const char* gname[3] = {"hero volume", "foe volume", "ground clay"};
        bool blamed = false;
        for (int i = 0; i < 3; i++) {
            if (gens[i] != prevGens_[i]) {
                std::printf("[reuse] re-trace: %s generation %u -> %u\n", gname[i],
                            prevGens_[i], gens[i]);
                blamed = true;
            }
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
        prevGens_[0] = brick_.generation();
        prevGens_[1] = foe_.generation();
        prevGens_[2] = ground_.generation();
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
    // M-PERF spring state, as its OWN section rather than fields on
    // RenderSnapCpu: additive sections keep pre-rig snapshots loadable without
    // a kVersion bump (see the dev-loop invariants in CLAUDE.md).
    {
        const float rig[6] = {spring_.q,    spring_.v,    spring_.gait,
                              foeSpring_.q, foeSpring_.v, foeSpring_.gait};
        w.section("RRIG", rig, sizeof(rig));
    }
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
    // Spring state is optional: a snapshot taken before the affine rig landed
    // has no RRIG, and starting from neutral is a valid pose.
    {
        float rig[6] = {0, 0, 0, 0, 0, 0.37f};
        r.read("RRIG", rig, sizeof(rig));
        spring_ = BodySpring{rig[0], rig[1], rig[2]};
        foeSpring_ = BodySpring{rig[3], rig[4], rig[5]};
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
