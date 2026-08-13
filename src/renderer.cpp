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
// include graph is a tree by construction).
std::string loadShader(const char* name) {
    std::string src = readFileRaw(shaderPath(name));
    std::stringstream out;
    std::stringstream in(src);
    std::string line;
    while (std::getline(in, line)) {
        const std::string tag = "//#include ";
        if (line.rfind(tag, 0) == 0) {
            std::string inc = line.substr(tag.size());
            while (!inc.empty() && (inc.back() == ' ' || inc.back() == '\r')) inc.pop_back();
            out << loadShader(inc.c_str()) << "\n";
        } else {
            out << line << "\n";
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
    pickDesc.size = 48; // pos+hit, normal+mat, albedo
    pickDesc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc;
    pickOut_ = gpu_->device.CreateBuffer(&pickDesc);
    wgpu::BufferDescriptor pickReadDesc{};
    pickReadDesc.size = 48;
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
    return true;
}

void Renderer::resize(int width, int height) {
    if (width == width_ && height == height_) return;
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

    wgpu::Device& dev = gpu_->device;
    {
        wgpu::ShaderModule mod = makeModule(dev, traceSrc, "trace");
        wgpu::ComputePipelineDescriptor desc{};
        desc.label = "trace";
        desc.compute.module = mod;
        desc.compute.entryPoint = "cs";
        tracePipeline_ = dev.CreateComputePipeline(&desc);
    }
    {
        wgpu::ShaderModule mod = makeModule(dev, pickSrc, "pick");
        wgpu::ComputePipelineDescriptor desc{};
        desc.label = "pick";
        desc.compute.module = mod;
        desc.compute.entryPoint = "cs";
        pickPipeline_ = dev.CreateComputePipeline(&desc);
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
        wgpu::BindGroupEntry entries[9] = {};
        entries[0].binding = 0;
        entries[0].buffer = brick_.indirection;
        entries[1].binding = 1;
        entries[1].buffer = brick_.distPool;
        entries[2].binding = 2;
        entries[2].buffer = brick_.albedoPool;
        entries[3].binding = 3;
        entries[3].buffer = brick_.seeds;
        entries[4].binding = 4;
        entries[4].buffer = brick_.coarse;
        entries[5].binding = 6;
        entries[5].buffer = brick_.cellWeights;
        entries[6].binding = 7;
        entries[6].buffer = ground_.base;
        entries[7].binding = 8;
        entries[7].buffer = ground_.height;
        entries[8].binding = 9;
        entries[8].buffer = ground_.color;
        wgpu::BindGroupDescriptor desc{};
        desc.layout = tracePipeline_.GetBindGroupLayout(1);
        desc.entryCount = 9;
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
        // pick's charDist + charAlbedo touch indirection + dist + albedo +
        // seeds + weights; auto layout excludes the unused coarse binding
        wgpu::BindGroupEntry entries[5] = {};
        entries[0].binding = 0;
        entries[0].buffer = brick_.indirection;
        entries[1].binding = 1;
        entries[1].buffer = brick_.distPool;
        entries[2].binding = 2;
        entries[2].buffer = brick_.albedoPool;
        entries[3].binding = 3;
        entries[3].buffer = brick_.seeds;
        entries[4].binding = 6;
        entries[4].buffer = brick_.cellWeights;
        wgpu::BindGroupDescriptor desc{};
        desc.layout = pickPipeline_.GetBindGroupLayout(1);
        desc.entryCount = 5;
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
    long stamp = shaderDirStamp();
    if (stamp == shaderDirStamp_) return false;
    shaderDirStamp_ = stamp;
    std::printf("reloading shaders\n");
    ground_.rebuildPipelines(loadShader("ground.wgsl"));
    if (buildPipelines()) buildBindGroups();
    // character source may have changed; rebuild the volume from it
    brick_.requestBake();
    return true;
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
    int n = std::min((int)marbles_.size(), 8);
    for (int i = 0; i < n; i++) {
        const MarbleProp& m = marbles_[i];
        float* slotA = out[14 + i * 2];
        float* slotB = out[15 + i * 2];
        float pos[3] = {m.pos[0], m.pos[1], m.pos[2]};
        // props ride their bone rigidly; radius stays fixed (glass beads
        // don't breathe with torso scale)
        if (m.bone >= 0 && (size_t)(m.bone * 16 + 16) <= skinMats_.size()) {
            matTransformPoint(&skinMats_[m.bone * 16], m.pos, pos);
        }
        slotA[0] = pos[0]; slotA[1] = pos[1]; slotA[2] = pos[2];
        slotA[3] = m.radius;
        slotB[0] = m.color[0]; slotB[1] = m.color[1]; slotB[2] = m.color[2];
    }
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
    // blend only bridges hairline handoff cracks; ~3 voxels seals them
    // without re-introducing ring bulges.
    out[65][1] = 0.008f;
    out[65][2] = 0.06f; // box test margin: sample within, bound outside
    out[65][3] = bodyBoundR + 0.05f;

    // M4.6 conservation: in-flight gobs (12 Hz-stepped positions) + the
    // ground field's clay top bound (0 disables the field in the tracer).
    // These slot indices are hand-mirrored in trace.wgsl AND pick.wgsl —
    // this static_assert catches only the C++ side overrunning the buffer;
    // the WGSL side has no compile-time link, so CLAUDE.md flags the mirror.
    static_assert(kUniformSlots >= 287,
                  "gobs reach out[282], groundMeta out[283], sword out[284-286]; "
                  "keep kUniformSlots and the Uniforms struct in trace.wgsl "
                  "+ pick.wgsl in sync");
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
    if (look.sword.enabled && !bones_.empty()) {
        float hilt[3], tip[3], gA[3], gB[3];
        swordGeometry(look.sword, hilt, tip, gA, gB);
        out[284][0] = hilt[0]; out[284][1] = hilt[1]; out[284][2] = hilt[2];
        out[284][3] = look.sword.radius;
        out[285][0] = tip[0]; out[285][1] = tip[1]; out[285][2] = tip[2];
        out[286][0] = look.sword.color[0];
        out[286][1] = look.sword.color[1];
        out[286][2] = look.sword.color[2];
    }
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
    if (!asset.marbles.empty()) marbles_ = asset.marbles;
    bones_ = asset.bones;
    clips_ = asset.clips;
    capsules_ = deriveCapsules(asset);
    evalPose(bones_, nullptr, 0.f, skinMats_); // identity: rest pose

    // M4.7: build the arm IK chains from bone names. Rig convention (see the
    // .glb): humerus.<side> -> radius.<side> -> hand.<side> -> fingertips,
    // with thumb.<side> also under radius. Subtrees carry the hand/fingers.
    armIk_.clear();
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
    for (const char* side : {"l", "r"}) {
        int sh = findBone(std::string("humerus.") + side);
        int el = findBone(std::string("radius.") + side);
        int wr = findBone(std::string("hand.") + side);
        if (sh < 0 || el < 0 || wr < 0) continue;
        ArmIkChain c;
        c.shoulder = sh; c.elbow = el; c.wrist = wr;
        subtree(sh, c.upperSubtree);
        subtree(el, c.lowerSubtree);
        c.pole[0] = 0.f; c.pole[1] = -0.5f; c.pole[2] = -1.f; // elbows bend down/back
        // palm offset: wrist -> fingertips rest length (grip sits mid-mitt, so
        // ~70% of it). Falls back to 0 (wrist on grip) if no fingertip bone.
        int ft = findBone(std::string("fingertips.") + side);
        if (ft >= 0) {
            float w[3], f[3];
            restOrigin(wr, w);
            restOrigin(ft, f);
            float dx = f[0] - w[0], dy = f[1] - w[1], dz = f[2] - w[2];
            c.handLen = 0.7f * std::sqrt(dx * dx + dy * dy + dz * dz);
        }
        armIk_.push_back(c);
    }
    std::printf("anim: %zu bones, %zu clip(s), %zu shadow capsules, %zu IK arm(s)\n",
                bones_.size(), clips_.size(), capsules_.size(), armIk_.size());
    brick_.requestImport(std::move(asset));
}

void Renderer::encodePick(wgpu::CommandEncoder& enc) {
    wgpu::ComputePassEncoder pass = enc.BeginComputePass();
    pass.SetPipeline(pickPipeline_);
    pass.SetBindGroup(0, pickBind_);
    pass.SetBindGroup(1, pickBrickBind_);
    pass.DispatchWorkgroups(1);
    pass.End();
    if (!pickMapPending_) {
        enc.CopyBufferToBuffer(pickOut_, 0, pickRead_, 0, 48);
    }
}

void Renderer::pollPick() {
    if (pickMapPending_) return;
    pickMapPending_ = true;
    auto alive = alive_;
    pickRead_.MapAsync(wgpu::MapMode::Read, 0, 48, wgpu::CallbackMode::AllowSpontaneous,
                       [this, alive](wgpu::MapAsyncStatus status, wgpu::StringView) {
                           if (!*alive) return;
                           if (status == wgpu::MapAsyncStatus::Success) {
                               const float* d =
                                   (const float*)pickRead_.GetConstMappedRange(0, 48);
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
                               pickRead_.Unmap();
                           }
                           pickMapPending_ = false;
                       });
}

BrickEdit Renderer::queueBrickEdit(BrickEdit e) {
    // snapshot the wound's facing + material color for the gobs this edit
    // will tear off (measurement arrives frames later; pick moves on)
    if (pickValid_ && !e.fromGob) {
        std::memcpy(e.outDir, pickNormal_, sizeof(e.outDir));
        if (pickMat_ > 2.5f && pickMat_ < 3.5f) {
            std::memcpy(e.srcColor, pickAlbedo_, sizeof(e.srcColor));
        }
    }
    brick_.queueEdit(e);
    return e;
}

void Renderer::absorbMeasured() {
    // Measurements from ops that ran 1-2 frames ago. Always drained — even
    // with conservation off — or a measurement queued while ON but arriving
    // while OFF would desync the ledger the next time it's toggled back on.
    // The toggle gates SPAWNING (updateConservation), not accounting.
    MeasuredEdit m;
    while (brick_.takeMeasured(m)) {
        if (std::getenv("CLAYFRAY_DEBUG_LEDGER")) {
            std::printf("[sploot] mode %d measured %.1f ml\n", m.edit.mode,
                        m.volume * 1e6f);
        }
        if (m.edit.mode == 1) {
            sploot_.carved += m.volume;
            sploot_.debt += m.volume;
            std::memcpy(lastWound_, m.edit.pos, sizeof(lastWound_));
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
    };
    std::vector<PosedCap> caps;
    caps.reserve(capsules_.size());
    for (const BoneCapsule& c : capsules_) {
        PosedCap pc;
        std::memcpy(pc.a, c.a, sizeof(pc.a));
        std::memcpy(pc.b, c.b, sizeof(pc.b));
        pc.r = c.r;
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
                    std::memcpy(e.pos, g.pos, sizeof(e.pos));
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
    // pose the skeleton at the quantized clock; paused = rest pose
    if (!bones_.empty()) {
        const AnimClip* clip =
            (look.animPlay && !clips_.empty() && clips_[0].duration > 0.f) ? &clips_[0]
                                                                           : nullptr;
        if (clip) {
            animT_ = std::fmod(frame.poseTime * look.animSpeed, clip->duration);
        }
        evalPose(bones_, clip, animT_, skinMats_);

        // M4.7: sword is master, arms follow. IK overrides the FK arm pose so
        // each hand reaches its grip on the (debug-driven) sword; torso/head
        // stay on the clip. Marbles/capsules below read the IK'd skinMats_.
        if (look.sword.enabled && !armIk_.empty()) {
            float hilt[3], tip[3], gA[3], gB[3];
            swordGeometry(look.sword, hilt, tip, gA, gB);
            for (ArmIkChain& c : armIk_) {
                const std::string& nm = bones_[c.shoulder].name;
                bool right = nm.size() >= 2 && nm.compare(nm.size() - 2, 2, ".r") == 0;
                std::memcpy(c.target, right ? gA : gB, sizeof(c.target));
            }
            applyArmIk(bones_, armIk_, skinMats_);
        }
    }

    // conservation runs before packing so this frame's uniforms carry fresh
    // gob positions and the ground bound
    updateConservation(look, frame);

    float uniforms[kUniformSlots][4];
    packUniforms(cam, look, frame, uniforms);
    gpu_->queue.WriteBuffer(uniformBuf_, 0, uniforms, sizeof(uniforms));

    wgpu::CommandEncoder encoder = gpu_->device.CreateCommandEncoder();
    brick_.encode(encoder);
    ground_.encode(encoder);

    {
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
