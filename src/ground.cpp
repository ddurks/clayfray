#include "ground.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "snapshot.h"

namespace {

wgpu::ShaderModule makeModule(wgpu::Device& device, const std::string& src,
                              const char* label) {
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = src.c_str();
    wgpu::ShaderModuleDescriptor desc{};
    desc.nextInChain = &wgsl;
    desc.label = label;
    return device.CreateShaderModule(&desc);
}

struct SplatParamsCpu {
    float splat[4]; // xz center, y radius, w amp
    float color[4];
    int32_t tile[4]; // xy first texel, zw dims
};

} // namespace

bool GroundClay::init(Gpu& gpu, const std::string& src) {
    gpu_ = &gpu;
    wgpu::Device& dev = gpu.device;
    const uint64_t texels = (uint64_t)kN * kN;

    auto storage = [&](uint64_t size, const char* label) {
        wgpu::BufferDescriptor desc{};
        desc.label = label;
        desc.size = size;
        desc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst |
                     wgpu::BufferUsage::CopySrc; // CopySrc: snapshot readback
        return dev.CreateBuffer(&desc);
    };
    base = storage(texels * 4, "ground base");
    height = storage(texels * 4, "ground height");
    color = storage(texels * 4, "ground color");
    for (int i = 0; i < kOpsPerFrame; i++) {
        wgpu::BufferDescriptor desc{};
        desc.label = "ground splat params";
        desc.size = sizeof(SplatParamsCpu);
        desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        params_[i] = dev.CreateBuffer(&desc);
    }
    static_assert(sizeof(SplatParamsCpu) == 48, "must match WGSL SplatParams");
    return rebuildPipelines(src);
}

bool GroundClay::rebuildPipelines(const std::string& src) {
    wgpu::Device& dev = gpu_->device;
    wgpu::ShaderModule mod = makeModule(dev, src, "ground");
    wgpu::ComputePipelineDescriptor desc{};
    desc.compute.module = mod;
    desc.label = "ground initBase";
    desc.compute.entryPoint = "initBase";
    initPipe_ = dev.CreateComputePipeline(&desc);
    desc.label = "ground splat";
    desc.compute.entryPoint = "splat";
    splatPipe_ = dev.CreateComputePipeline(&desc);
    if (!initPipe_ || !splatPipe_) return false;
    buildBindGroups();
    return true;
}

void GroundClay::buildBindGroups() {
    auto bind = [&](wgpu::ComputePipeline& pipe,
                    std::vector<std::pair<uint32_t, wgpu::Buffer>> entries) {
        std::vector<wgpu::BindGroupEntry> e(entries.size());
        for (size_t i = 0; i < entries.size(); i++) {
            e[i].binding = entries[i].first;
            e[i].buffer = entries[i].second;
        }
        wgpu::BindGroupDescriptor desc{};
        desc.layout = pipe.GetBindGroupLayout(0);
        desc.entryCount = e.size();
        desc.entries = e.data();
        return gpu_->device.CreateBindGroup(&desc);
    };
    initG_ = bind(initPipe_, {{1, base}});
    for (int i = 0; i < kOpsPerFrame; i++) {
        splatG_[i] = bind(splatPipe_, {{0, params_[i]}, {2, height}, {3, color}});
    }
}

void GroundClay::splat(float x, float z, float volumeM3, const float col[3]) {
    if (volumeM3 <= 0.f) return;
    // pile shape: peak ~0.38 R reads as a slumped gob (0.55 stacked into
    // meringue spikes); radius from volume, then amp recomputed exactly so
    // clamping never cheats the ledger
    float r = std::cbrt(3.f * volumeM3 / (0.38f * 3.14159265f));
    // slump: gobs funneled onto the same spot widen instead of stacking a
    // tower — clay sheds sideways as the pile grows
    r *= 1.f + 5.f * thicknessAt(x, z);
    r = std::min(std::max(r, 2.5f * kTexel), 0.28f);
    float amp = 3.f * volumeM3 / (3.14159265f * r * r);
    Op op;
    // keep the whole kernel inside the field so no volume is clipped away
    op.x = std::min(std::max(x, kOrigin + r + kTexel), -kOrigin - r - kTexel);
    op.z = std::min(std::max(z, kOrigin + r + kTexel), -kOrigin - r - kTexel);
    op.radius = r;
    op.amp = amp;
    op.col[0] = col[0];
    op.col[1] = col[1];
    op.col[2] = col[2];
    pending_.push_back(op);

    // coarse mirror update so later gobs land on this pile
    const float mTexel = -2.f * kOrigin / kMirror;
    for (int j = 0; j < kMirror; j++) {
        float mz = kOrigin + (j + 0.5f) * mTexel;
        if (std::fabs(mz - op.z) > r) continue;
        for (int i = 0; i < kMirror; i++) {
            float mx = kOrigin + (i + 0.5f) * mTexel;
            float r2 = (mx - op.x) * (mx - op.x) + (mz - op.z) * (mz - op.z);
            if (r2 >= r * r) continue;
            float u = 1.f - r2 / (r * r);
            mirror_[i + j * kMirror] += amp * u * u;
        }
    }
    // top bound for the tracer = the actual tallest pile, not a per-splat
    // sum: an inflated bound is an invisible occluder plane that darkens
    // AO/penumbra across the whole scene and phantom-hits grazing rays.
    // Small pad covers coarse-mirror underestimation of the true peak.
    maxH_ = std::max(maxH_, thicknessAt(op.x, op.z) + 0.15f * amp + 0.003f);
}

float GroundClay::thicknessAt(float x, float z) const {
    const float mTexel = -2.f * kOrigin / kMirror;
    int i = (int)std::floor((x - kOrigin) / mTexel);
    int j = (int)std::floor((z - kOrigin) / mTexel);
    i = std::min(std::max(i, 0), kMirror - 1);
    j = std::min(std::max(j, 0), kMirror - 1);
    return mirror_[i + j * kMirror];
}

float GroundClay::approxTopAt(float x, float z) const {
    // mid-pebble base guess: gobs sink a hair into the mosaic before the
    // splat pops in, which reads as impact squash at 12 Hz anyway
    return 0.015f + thicknessAt(x, z);
}

bool GroundClay::save(SnapWriter& w) {
    const uint64_t texels = (uint64_t)kN * kN;
    uint32_t meta[4] = {kN, kMirror, basePending_ ? 1u : 0u, 0};
    w.section("GMET", meta, sizeof(meta));
    struct Sect {
        const char* tag;
        wgpu::Buffer buf;
    };
    const Sect sects[] = {{"GBAS", base}, {"GHGT", height}, {"GCOL", color}};
    for (const Sect& s : sects) {
        std::vector<uint8_t> data = readbackBuffer(*gpu_, s.buf, texels * 4);
        if (data.empty()) return false;
        w.section(s.tag, data.data(), data.size());
    }
    w.section("GMAX", &maxH_, sizeof(maxH_));
    w.section("GMIR", mirror_, sizeof(mirror_));
    return true;
}

bool GroundClay::load(SnapReader& r) {
    const uint64_t texels = (uint64_t)kN * kN;
    uint32_t meta[4];
    if (!r.read("GMET", meta, sizeof(meta)) || meta[0] != kN || meta[1] != kMirror) {
        std::fprintf(stderr, "[snap] ground field mismatch\n");
        return false;
    }
    struct Sect {
        const char* tag;
        wgpu::Buffer buf;
    };
    const Sect sects[] = {{"GBAS", base}, {"GHGT", height}, {"GCOL", color}};
    for (const Sect& s : sects) {
        std::vector<uint8_t> data = r.blob(s.tag);
        if (data.size() != texels * 4) {
            std::fprintf(stderr, "[snap] ground section %s wrong size\n", s.tag);
            return false;
        }
        gpu_->queue.WriteBuffer(s.buf, 0, data.data(), data.size());
    }
    if (!r.read("GMAX", &maxH_, sizeof(maxH_))) return false;
    if (!r.read("GMIR", mirror_, sizeof(mirror_))) return false;
    basePending_ = meta[2] != 0;
    pending_.clear();
    return true;
}

void GroundClay::encode(wgpu::CommandEncoder& enc) {
    if (basePending_) {
        basePending_ = false;
        gen_++;
        enc.ClearBuffer(height, 0, (uint64_t)kN * kN * 4);
        enc.ClearBuffer(color, 0, (uint64_t)kN * kN * 4);
        wgpu::ComputePassEncoder pass = enc.BeginComputePass();
        pass.SetPipeline(initPipe_);
        pass.SetBindGroup(0, initG_);
        pass.DispatchWorkgroups((kN + 7) / 8, (kN + 7) / 8);
        pass.End();
    }
    if (pending_.empty()) return;
    gen_++;
    int n = std::min((int)pending_.size(), kOpsPerFrame);
    for (int i = 0; i < n; i++) {
        const Op& op = pending_[i];
        SplatParamsCpu p{};
        p.splat[0] = op.x;
        p.splat[1] = op.radius;
        p.splat[2] = op.z;
        p.splat[3] = op.amp;
        p.color[0] = op.col[0];
        p.color[1] = op.col[1];
        p.color[2] = op.col[2];
        p.color[3] = 1.f;
        int t0 = (int)std::floor((op.x - op.radius - kOrigin) / kTexel) - 1;
        int t1 = (int)std::floor((op.z - op.radius - kOrigin) / kTexel) - 1;
        int dim = (int)std::ceil(2.f * op.radius / kTexel) + 3;
        p.tile[0] = t0;
        p.tile[1] = t1;
        p.tile[2] = dim;
        p.tile[3] = dim;
        gpu_->queue.WriteBuffer(params_[i], 0, &p, sizeof(p));
    }
    wgpu::ComputePassEncoder pass = enc.BeginComputePass();
    pass.SetPipeline(splatPipe_);
    for (int i = 0; i < n; i++) {
        const Op& op = pending_[i];
        int dim = (int)std::ceil(2.f * op.radius / kTexel) + 3;
        pass.SetBindGroup(0, splatG_[i]);
        pass.DispatchWorkgroups((dim + 7) / 8, (dim + 7) / 8);
    }
    pass.End();
    pending_.erase(pending_.begin(), pending_.begin() + n);
}
