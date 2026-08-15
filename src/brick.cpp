#include "brick.h"

#include "snapshot.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

// Volume geometry lives in BrickSystem (brick.h) — the members below are the
// same constants this file used to keep a private copy of.
using K = BrickSystem;
constexpr const float* kOrigin = K::kOrigin;

wgpu::ShaderModule makeModule(wgpu::Device& device, const std::string& src,
                              const char* label) {
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = src.c_str();
    wgpu::ShaderModuleDescriptor desc{};
    desc.nextInChain = &wgsl;
    desc.label = label;
    return device.CreateShaderModule(&desc);
}

wgpu::ComputePipeline makePipeline(wgpu::Device& device, wgpu::ShaderModule mod,
                                   const char* entry) {
    wgpu::ComputePipelineDescriptor desc{};
    desc.label = entry;
    desc.compute.module = mod;
    desc.compute.entryPoint = entry;
    auto t0 = std::chrono::steady_clock::now();
    wgpu::ComputePipeline p = device.CreateComputePipeline(&desc);
    double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (s > 0.5) {
        std::printf("[startup] %s pipeline: %.1fs\n", entry, s);
        std::fflush(stdout);
    }
    return p;
}

wgpu::Buffer makeStorage(wgpu::Device& device, uint64_t size, const char* label,
                         bool uniform = false) {
    wgpu::BufferDescriptor desc{};
    desc.label = label;
    desc.size = size;
    desc.usage = (uniform ? wgpu::BufferUsage::Uniform : wgpu::BufferUsage::Storage) |
                 wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc;
    return device.CreateBuffer(&desc);
}

// A bind group entry that may point at a SUB-RANGE of a buffer. The per-cell
// arrays are regions of one `volume` buffer (see brick.h), so each pass binds
// only the region it touches and the write shaders keep their own bindings.
struct BindBuf {
    uint32_t binding;
    wgpu::Buffer buf;
    uint64_t offset = 0;
    uint64_t size = wgpu::kWholeSize;
};

wgpu::BindGroup makeBindGroup(wgpu::Device& device, wgpu::ComputePipeline& pipe,
                              uint32_t group, const std::vector<BindBuf>& entries) {
    std::vector<wgpu::BindGroupEntry> e(entries.size());
    for (size_t i = 0; i < entries.size(); i++) {
        e[i].binding = entries[i].binding;
        e[i].buffer = entries[i].buf;
        e[i].offset = entries[i].offset;
        e[i].size = entries[i].size;
    }
    wgpu::BindGroupDescriptor desc{};
    desc.layout = pipe.GetBindGroupLayout(group);
    desc.entryCount = e.size();
    desc.entries = e.data();
    return device.CreateBindGroup(&desc);
}

struct EditParamsCpu {
    int32_t regionMin[4];
    int32_t regionDims[4];
    float brush[4];
    float color[4];
    float brushB[4];
};

} // namespace

// Emitted verbatim into each shader root by the renderer's `//#constants`
// directive. %.9g round-trips a float exactly, so the GPU sees bit-identical
// values to the CPU — which matters: a cell index computed from a slightly
// different SPAN lands in a different brick.
std::string BrickSystem::wgslConstants() {
    char buf[2048];
    std::snprintf(
        buf, sizeof(buf),
        "// GENERATED from BrickSystem in src/brick.h — do not edit here, and\n"
        "// do not hand-copy these into a shader. Change kGrid and rebuild.\n"
        "const GRID: i32 = %d;\n"
        "const BRICK_USABLE: f32 = %d.0;\n"
        "const VOXEL: f32 = %.9g;\n"
        "const SPAN: f32 = %.9g;\n"
        "const VOL_ORIGIN: vec3f = vec3f(%.9g, %.9g, %.9g);\n"
        "const BAND: f32 = %.9g;\n"
        "const MAX_BRICKS: u32 = %uu;\n"
        "const DIRTY_CAP: u32 = %uu;\n"
        "const CELLS: u32 = %uu;\n"
        "const AXIS_VOX: i32 = %d;\n"
        "const ROW_WORDS: u32 = %uu;\n"
        "const WARP_RESID_TOL: f32 = %.9g;\n"
        "// Base index (in u32 elements) of each per-cell array inside the one\n"
        "// `volume` storage buffer the tracer binds — see brick.h.\n"
        "const CELL_IND: u32 = %uu;\n"
        "const CELL_SEED: u32 = %uu;\n"
        "const CELL_COARSE: u32 = %uu;\n"
        "const CELL_W: u32 = %uu;\n",
        kGrid, kBrickUsable, (double)kVoxel, (double)kSpan, (double)kOrigin[0],
        (double)kOrigin[1], (double)kOrigin[2], (double)kBand, kMaxBricks,
        kDirtyCap, (uint32_t)kGrid * kGrid * kGrid, (int)kAxisVox, kRowWords,
        (double)(kWarpResidSpans * kSpan), (uint32_t)(kIndOff / 4),
        (uint32_t)(kSeedOff / 4), (uint32_t)(kCoarseOff / 4),
        (uint32_t)(kCellWOff / 4));
    return buf;
}

bool BrickSystem::init(Gpu& gpu, const std::string& editSrc, const std::string& jfaSrc,
                       const std::string& redistSrc, const std::string& voxelizeSrc) {
    gpu_ = &gpu;
    wgpu::Device& dev = gpu.device;
    const uint32_t cells = kGrid * kGrid * kGrid;

    // One buffer for indirection + seeds + coarse + cell weights (see the
    // region map in brick.h — Metal's 10-storage-buffer stage cap). Created
    // once at full size: import fills the weights in place, so bind groups
    // built against it never go stale.
    volume = makeStorage(dev, kVolumeBytes, "brick volume");
    distPool = makeStorage(dev, kMaxBricks * 256ull * 4ull, "brick dist pool");
    albedoPool = makeStorage(dev, kMaxBricks * 512ull * 4ull, "brick albedo pool");
    weightPool = makeStorage(dev, kMaxBricks * 512ull * 4ull, "brick weight pool");
    counters_ = makeStorage(dev, 16, "brick counters");
    freelist_ = makeStorage(dev, kMaxBricks * 4ull, "brick freelist");
    jfaB_ = makeStorage(dev, cells * 4ull, "jfa B");
    coarseB_ = makeStorage(dev, cells * 4ull, "coarse dist B");
    static_assert(sizeof(EditParamsCpu) == 80, "must match WGSL EditParams");
    for (int i = 0; i < kOpsPerFrame; i++)
        editParams_[i] = makeStorage(dev, sizeof(EditParamsCpu), "edit params", true);

    // 7 pow2 JFA rounds + two refinement rounds (JFA+2: kills the classic
    // 1-JFA wrong-seed errors that overstate safe steps), alternating source
    // buffer; resolve reads the final target (B after an odd step count).
    const int steps[kJfaSteps] = {64, 32, 16, 8, 4, 2, 1, 2, 1};
    for (int i = 0; i < kJfaSteps; i++) {
        jfaStepParams_[i] = makeStorage(dev, 16, "jfa step params", true);
        int32_t data[4] = {steps[i], (i % 2 == 0) ? 1 : 0, 0, 0};
        gpu.queue.WriteBuffer(jfaStepParams_[i], 0, data, sizeof(data));
    }
    jfaResolveParams_ = makeStorage(dev, 16, "jfa resolve params", true);
    int32_t resolveData[4] = {1, 0, 0, 0};
    gpu.queue.WriteBuffer(jfaResolveParams_, 0, resolveData, sizeof(resolveData));

    dirtyList_ = makeStorage(dev, (uint64_t)kDirtyCap * 4, "dirty list");
    {
        wgpu::BufferDescriptor desc{};
        desc.label = "redist indirect args";
        desc.size = 6 * 4;
        desc.usage = wgpu::BufferUsage::Indirect | wgpu::BufferUsage::Storage;
        indirectArgs_ = dev.CreateBuffer(&desc);
    }

    wgpu::ShaderModule editMod = makeModule(dev, editSrc, "edit");
    wgpu::ShaderModule jfaMod = makeModule(dev, jfaSrc, "jfa");
    wgpu::ShaderModule redistMod = makeModule(dev, redistSrc, "redistance");
    voxelizeMod_ = makeModule(dev, voxelizeSrc, "voxelize");
    vxClassify_ = makePipeline(dev, voxelizeMod_, "meshClassify");
    vxFill_ = makePipeline(dev, voxelizeMod_, "meshFill");
    vxParity_ = makePipeline(dev, voxelizeMod_, "voxelParity");
    classify_ = makePipeline(dev, editMod, "classify");
    fill_ = makePipeline(dev, editMod, "fill");
    jfaInit_ = makePipeline(dev, jfaMod, "init");
    jfaStep_ = makePipeline(dev, jfaMod, "step");
    jfaResolve_ = makePipeline(dev, jfaMod, "resolve");
    jfaRelax_ = makePipeline(dev, jfaMod, "relax");
    rdCompact_ = makePipeline(dev, redistMod, "compact");
    rdPrep_ = makePipeline(dev, redistMod, "prepArgs");
    rdRedist_ = makePipeline(dev, redistMod, "redistance");
    rdClear_ = makePipeline(dev, redistMod, "clearDirty");
    if (!classify_ || !fill_ || !jfaInit_ || !jfaStep_ || !jfaResolve_ || !jfaRelax_ ||
        !rdCompact_ || !rdPrep_ || !rdRedist_ || !rdClear_)
        return false;

    auto bind = [&](wgpu::ComputePipeline& pipe, uint32_t group,
                    std::vector<BindBuf> entries) {
        return makeBindGroup(gpu_->device, pipe, group, entries);
    };
    // The `volume` regions these passes write, each at the binding its shader
    // declares for it (indirection is always 0, seeds 1, coarse 5).
    const BindBuf vInd{0, volume, kIndOff, kCellBytes};
    const BindBuf vSeed{1, volume, kSeedOff, kCellBytes};
    const BindBuf vCoarse{5, volume, kCoarseOff, kCellBytes};

    for (int i = 0; i < kOpsPerFrame; i++)
        classifyG0_[i] = bind(classify_, 0, {{0, editParams_[i]}});
    classifyG1_ = bind(classify_, 1, {vInd});
    classifyG2_ = bind(classify_, 2, {{0, counters_}, {1, freelist_}});
    for (int i = 0; i < kOpsPerFrame; i++)
        fillG0_[i] = bind(fill_, 0, {{0, editParams_[i]}});
    fillG1_ = bind(fill_, 1, {vInd, {1, distPool}, {2, albedoPool}});
    fillG2_ = bind(fill_, 2, {{0, counters_}, {1, freelist_}});
    jfaInitG_ = bind(jfaInit_, 0, {vInd, vSeed});
    for (int i = 0; i < kJfaSteps; i++) {
        jfaStepG_[i] = bind(jfaStep_, 0,
                            {vSeed, {2, jfaB_}, {3, jfaStepParams_[i]}});
    }
    jfaResolveG_ = bind(jfaResolve_, 0,
                        {vInd, vSeed, {2, jfaB_}, {3, jfaResolveParams_},
                         {4, distPool}, vCoarse});
    rdCompactG_ = bind(rdCompact_, 0, {vInd, {2, dirtyList_}, {3, counters_}});
    rdPrepG_ = bind(rdPrep_, 0, {{3, counters_}, {4, indirectArgs_}});
    rdRedistG_ = bind(rdRedist_, 0, {vInd, {1, distPool}, {2, dirtyList_}});
    rdClearG_ = bind(rdClear_, 0, {vInd, {2, dirtyList_}, {3, counters_}});
    // 4 relax rounds alternating direction; even count ends back in `coarse`
    for (int i = 0; i < 4; i++) {
        jfaRelaxG_[i] = bind(jfaRelax_, 0,
                             {{3, jfaStepParams_[i % 2 == 0 ? 6 : 5]}, // srcIsA: 1,0,1,0
                              vCoarse,
                              {6, coarseB_}});
    }
    return true;
}

void BrickSystem::debugStats(const char* label) {
    const uint32_t cells = kGrid * kGrid * kGrid;
    wgpu::BufferDescriptor desc{};
    desc.size = cells * 4ull + 16;
    desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer rb = gpu_->device.CreateBuffer(&desc);
    wgpu::CommandEncoder enc = gpu_->device.CreateCommandEncoder();
    enc.CopyBufferToBuffer(volume, kIndOff, rb, 0, cells * 4ull);
    enc.CopyBufferToBuffer(counters_, 0, rb, cells * 4ull, 16);
    wgpu::CommandBuffer cmd = enc.Finish();
    gpu_->queue.Submit(1, &cmd);
    bool ok = false;
    wgpu::Future f = rb.MapAsync(wgpu::MapMode::Read, 0, desc.size,
                                 wgpu::CallbackMode::WaitAnyOnly,
                                 [&ok](wgpu::MapAsyncStatus s, wgpu::StringView) {
                                     ok = s == wgpu::MapAsyncStatus::Success;
                                 });
    gpu_->instance.WaitAny(f, UINT64_MAX);
    if (!ok) return;
    const uint32_t* d = (const uint32_t*)rb.GetConstMappedRange(0, desc.size);
    uint32_t alloc = 0, fresh = 0, inside = 0, maxIdx = 0;
    for (uint32_t i = 0; i < cells; i++) {
        uint32_t e = d[i];
        if (e & 0x80000000u) {
            alloc++;
            if (e & 0x20000000u) fresh++;
            maxIdx = std::max(maxIdx, e & 0x000FFFFFu);
        } else if (e & 0x40000000u) {
            inside++;
        }
    }
    std::printf("[brick %s] alloc=%u freshLeft=%u insideEmpty=%u maxIdx=%u "
                "allocTop=%u freeTop=%u volFp=%u\n",
                label, alloc, fresh, inside, maxIdx, d[cells], d[cells + 1],
                d[cells + 3]);
    rb.Unmap();
}

// Scan the distance pool for anomalous bricks (zero words / NaN / values
// outside the band) and print their cell coordinates.
void BrickSystem::debugScanField() {
    const uint32_t cells = kGrid * kGrid * kGrid;
    const uint64_t distBytes = (uint64_t)kMaxBricks * 256 * 4;
    wgpu::BufferDescriptor desc{};
    desc.size = cells * 4ull + distBytes;
    desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer rb = gpu_->device.CreateBuffer(&desc);
    wgpu::CommandEncoder enc = gpu_->device.CreateCommandEncoder();
    enc.CopyBufferToBuffer(volume, kIndOff, rb, 0, cells * 4ull);
    enc.CopyBufferToBuffer(distPool, 0, rb, cells * 4ull, distBytes);
    wgpu::CommandBuffer cmd = enc.Finish();
    gpu_->queue.Submit(1, &cmd);
    bool ok = false;
    wgpu::Future f = rb.MapAsync(wgpu::MapMode::Read, 0, desc.size,
                                 wgpu::CallbackMode::WaitAnyOnly,
                                 [&ok](wgpu::MapAsyncStatus s, wgpu::StringView) {
                                     ok = s == wgpu::MapAsyncStatus::Success;
                                 });
    gpu_->instance.WaitAny(f, UINT64_MAX);
    if (!ok) return;
    const uint32_t* ind = (const uint32_t*)rb.GetConstMappedRange(0, desc.size);
    const uint16_t* dist = (const uint16_t*)(ind + cells);
    auto halfToFloat = [](uint16_t h) -> float {
        uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
        if (exp == 0) return (sign ? -1.f : 1.f) * man * (1.f / 16777216.f);
        if (exp == 31) return man ? NAN : (sign ? -INFINITY : INFINITY);
        float v = std::ldexp(1.f + man / 1024.f, (int)exp - 15);
        return sign ? -v : v;
    };
    int reported = 0;
    for (uint32_t ci = 0; ci < cells && reported < 12; ci++) {
        uint32_t e = ind[ci];
        if (!(e & 0x80000000u)) continue;
        uint32_t brick = e & 0x000FFFFFu;
        int zeroWords = 0, outOfBand = 0, nans = 0;
        for (int v = 0; v < 512; v++) {
            uint16_t h = dist[(uint64_t)brick * 512 + v];
            float fv = halfToFloat(h);
            if (h == 0) zeroWords++;
            if (std::isnan(fv)) nans++;
            else if (fv < -12.5f || fv > 12.5f) outOfBand++;
        }
        if (zeroWords > 40 || nans > 0 || outOfBand > 0) {
            int x = ci % kGrid, y = (ci / kGrid) % kGrid, z = ci / (kGrid * kGrid);
            std::printf("  suspicious cell (%d,%d,%d) brick=%u zeros=%d nan=%d "
                        "outOfBand=%d world=(%.3f,%.3f,%.3f)\n",
                        x, y, z, brick, zeroWords, nans, outOfBand,
                        -0.7082f + (x + 0.5f) * kSpan, -0.1582f + (y + 0.5f) * kSpan,
                        -0.7082f + (z + 0.5f) * kSpan);
            reported++;
        }
    }
    if (reported == 0) std::printf("  field scan: no anomalous bricks\n");
    rb.Unmap();

    // coarse-field histogram
    wgpu::BufferDescriptor cdesc{};
    cdesc.size = cells * 4ull;
    cdesc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer crb = gpu_->device.CreateBuffer(&cdesc);
    wgpu::CommandEncoder cenc = gpu_->device.CreateCommandEncoder();
    cenc.CopyBufferToBuffer(volume, kCoarseOff, crb, 0, cells * 4ull);
    wgpu::CommandBuffer ccmd = cenc.Finish();
    gpu_->queue.Submit(1, &ccmd);
    bool cok = false;
    wgpu::Future cf = crb.MapAsync(wgpu::MapMode::Read, 0, cdesc.size,
                                   wgpu::CallbackMode::WaitAnyOnly,
                                   [&cok](wgpu::MapAsyncStatus s, wgpu::StringView) {
                                       cok = s == wgpu::MapAsyncStatus::Success;
                                   });
    gpu_->instance.WaitAny(cf, UINT64_MAX);
    if (cok) {
        const float* cd = (const float*)crb.GetConstMappedRange(0, cdesc.size);
        int neg = 0, tiny = 0, nan = 0;
        float mn = 1e9f, mx = -1e9f;
        for (uint32_t i = 0; i < cells; i++) {
            float v = cd[i];
            if (std::isnan(v)) { nan++; continue; }
            if (v < 0) neg++;
            if (std::fabs(v) < 0.005f) tiny++;
            mn = std::min(mn, v);
            mx = std::max(mx, v);
        }
        std::printf("  coarse: neg=%d tiny=%d nan=%d min=%.4f max=%.4f\n", neg, tiny,
                    nan, mn, mx);
        crb.Unmap();
    }
}

// Legacy single-fighter entry point: prepare and use in one step.
void BrickSystem::requestImport(CharacterAsset asset) {
    requestImport(prepareImport(*gpu_, asset));
}

std::shared_ptr<BrickSystem::MeshImport> BrickSystem::prepareImport(
    Gpu& gpu, const CharacterAsset& asset) {
    wgpu::Device& dev = gpu.device;
    const uint32_t cells = kGrid * kGrid * kGrid;
    const uint32_t triCount = asset.triangleCount();
    const uint32_t vertCount = asset.vertexCount();
    if (triCount == 0) return nullptr;
    auto tick = std::chrono::steady_clock::now();
    auto lap = [&tick](const char* what) {
        auto now = std::chrono::steady_clock::now();
        double s = std::chrono::duration<double>(now - tick).count();
        if (s > 0.5) {
            std::printf("[startup] import %s: %.1fs\n", what, s);
            std::fflush(stdout);
        }
        tick = now;
    };

    // per-vertex packed skin: x = 4x joint u8, y = 4x weight unorm8
    std::vector<uint32_t> skin(vertCount * 2);
    for (uint32_t v = 0; v < vertCount; v++) {
        uint32_t j = 0, w = 0;
        for (int s = 0; s < 4; s++) {
            j |= (std::min<uint32_t>(asset.joints[v * 4 + s], 255u)) << (s * 8);
            w |= (uint32_t)std::lround(
                     std::min(std::max(asset.weights[v * 4 + s], 0.f), 1.f) * 255.f)
                 << (s * 8);
        }
        skin[v * 2] = j;
        skin[v * 2 + 1] = w;
    }

    // bin triangles into cells (AABB + band margin): fill needs every tri
    // that could be a voxel's nearest within the stored band
    const float margin = 3.4f * kSpan;
    std::vector<uint32_t> counts(cells + 1, 0);
    auto cellRange = [&](uint32_t t, int mn[3], int mx[3]) {
        float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
        for (int k = 0; k < 3; k++) {
            const float* p = &asset.positions[asset.indices[t * 3 + k] * 3];
            for (int a = 0; a < 3; a++) {
                lo[a] = std::min(lo[a], p[a]);
                hi[a] = std::max(hi[a], p[a]);
            }
        }
        for (int a = 0; a < 3; a++) {
            mn[a] = std::max(0, (int)std::floor((lo[a] - margin - kOrigin[a]) / kSpan));
            mx[a] = std::min(kGrid - 1,
                             (int)std::floor((hi[a] + margin - kOrigin[a]) / kSpan));
        }
    };
    for (uint32_t t = 0; t < triCount; t++) {
        int mn[3], mx[3];
        cellRange(t, mn, mx);
        for (int z = mn[2]; z <= mx[2]; z++)
            for (int y = mn[1]; y <= mx[1]; y++)
                for (int x = mn[0]; x <= mx[0]; x++)
                    counts[x + y * kGrid + z * kGrid * kGrid + 1]++;
    }
    std::vector<uint32_t> offsets(cells + 1, 0);
    for (uint32_t i = 0; i < cells; i++) offsets[i + 1] = offsets[i] + counts[i + 1];
    std::vector<uint32_t> ids(offsets[cells]);
    std::vector<uint32_t> cursor(offsets.begin(), offsets.end() - 1);
    for (uint32_t t = 0; t < triCount; t++) {
        int mn[3], mx[3];
        cellRange(t, mn, mx);
        for (int z = mn[2]; z <= mx[2]; z++)
            for (int y = mn[1]; y <= mx[1]; y++)
                for (int x = mn[0]; x <= mx[0]; x++)
                    ids[cursor[x + y * kGrid + z * kGrid * kGrid]++] = t;
    }
    lap("bin");

    // watertight inside/outside per cell: +x raycast parity per (y,z) row
    std::vector<uint32_t> inside((cells + 31) / 32, 0);
    const float x0 = kOrigin[0] - 1.f;
    std::vector<float> hits;
    for (int z = 0; z < kGrid; z++) {
        for (int y = 0; y < kGrid; y++) {
            float oy = kOrigin[1] + (y + 0.5f) * kSpan;
            float oz = kOrigin[2] + (z + 0.5f) * kSpan;
            hits.clear();
            for (uint32_t t = 0; t < triCount; t++) {
                const float* a = &asset.positions[asset.indices[t * 3] * 3];
                const float* b = &asset.positions[asset.indices[t * 3 + 1] * 3];
                const float* c = &asset.positions[asset.indices[t * 3 + 2] * 3];
                // Moller-Trumbore, ray origin (x0, oy, oz), dir (1,0,0)
                float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
                float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
                // pvec = dir x e2 = (0, -e2z, e2y); det = e1 . pvec
                float det = -e1[1] * e2[2] + e1[2] * e2[1];
                if (std::fabs(det) < 1e-12f) continue;
                float inv = 1.f / det;
                float tv[3] = {x0 - a[0], oy - a[1], oz - a[2]};
                float u = (-tv[1] * e2[2] + tv[2] * e2[1]) * inv;
                if (u < 0.f || u > 1.f) continue;
                float qv[3] = {tv[1] * e1[2] - tv[2] * e1[1],
                               tv[2] * e1[0] - tv[0] * e1[2],
                               tv[0] * e1[1] - tv[1] * e1[0]};
                float v = qv[0] * inv; // dir . qvec
                if (v < 0.f || u + v > 1.f) continue;
                float thit = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
                if (thit > 0.f) hits.push_back(thit);
            }
            std::sort(hits.begin(), hits.end());
            for (int x = 0; x < kGrid; x++) {
                float cx = kOrigin[0] + (x + 0.5f) * kSpan - x0;
                size_t crossings =
                    std::lower_bound(hits.begin(), hits.end(), cx) - hits.begin();
                if (crossings & 1) {
                    uint32_t ci = x + y * kGrid + z * kGrid * kGrid;
                    inside[ci / 32] |= 1u << (ci % 32);
                }
            }
        }
    }

    lap("parity");

    // smooth area-weighted vertex normals: the voxel sign test needs them —
    // face-normal signs flip at edges/vertices and speckle the silhouette
    std::vector<float> normals(vertCount * 3, 0.f);
    for (uint32_t t = 0; t < triCount; t++) {
        const float* a = &asset.positions[asset.indices[t * 3] * 3];
        const float* b = &asset.positions[asset.indices[t * 3 + 1] * 3];
        const float* c = &asset.positions[asset.indices[t * 3 + 2] * 3];
        float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                      e1[0] * e2[1] - e1[1] * e2[0]}; // area-weighted (unnormalized)
        for (int k = 0; k < 3; k++)
            for (int ax = 0; ax < 3; ax++)
                normals[asset.indices[t * 3 + k] * 3 + ax] += n[ax];
    }
    // interleave pos+normal (6 floats/vertex): keeps us at the 10-buffer limit
    std::vector<float> posNrm(vertCount * 6);
    for (uint32_t v = 0; v < vertCount; v++) {
        float nx = normals[v * 3], ny = normals[v * 3 + 1], nz = normals[v * 3 + 2];
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len < 1e-12f) len = 1.f;
        posNrm[v * 6 + 0] = asset.positions[v * 3 + 0];
        posNrm[v * 6 + 1] = asset.positions[v * 3 + 1];
        posNrm[v * 6 + 2] = asset.positions[v * 3 + 2];
        posNrm[v * 6 + 3] = nx / len;
        posNrm[v * 6 + 4] = ny / len;
        posNrm[v * 6 + 5] = nz / len;
    }

    lap("normals");

    // per-cell 4-slot bone weights (joints word + weights word per cell):
    // the trace-side warp blends up to four bones per region, which is what
    // hand-scale junctions need. Source: distance-weighted gather over the
    // cell's binned vertices (denoised vs nearest-vertex), flood-filled so
    // the warp is defined volume-wide.
    std::vector<uint32_t> cellW(cells * 2, 0);
    {
        std::vector<int> queue;
        queue.reserve(cells);
        std::vector<float> acc(256);
        for (uint32_t ci = 0; ci < cells; ci++) {
            uint32_t lo = offsets[ci], hi = offsets[ci + 1];
            if (lo == hi) continue;
            float cx = kOrigin[0] + ((ci % kGrid) + 0.5f) * kSpan;
            float cy = kOrigin[1] + (((ci / kGrid) % kGrid) + 0.5f) * kSpan;
            float cz = kOrigin[2] + ((ci / (kGrid * kGrid)) + 0.5f) * kSpan;
            std::fill(acc.begin(), acc.end(), 0.f);
            for (uint32_t k = lo; k < hi; k++) {
                uint32_t t = ids[k];
                for (int e = 0; e < 3; e++) {
                    uint32_t v = asset.indices[t * 3 + e];
                    const float* pv = &asset.positions[v * 3];
                    float dx = pv[0] - cx, dy = pv[1] - cy, dz = pv[2] - cz;
                    float kern = 1.f / (dx * dx + dy * dy + dz * dz +
                                        kSpan * kSpan * 0.25f);
                    for (int sl = 0; sl < 4; sl++) {
                        uint32_t j = std::min<uint32_t>(asset.joints[v * 4 + sl], 255u);
                        acc[j] += asset.weights[v * 4 + sl] * kern;
                    }
                }
            }
            // top-4, renormalized to sum 255
            int top[4] = {-1, -1, -1, -1};
            for (int j = 0; j < 256; j++) {
                if (acc[j] <= 0.f) continue;
                for (int r = 0; r < 4; r++) {
                    if (top[r] < 0 || acc[j] > acc[top[r]]) {
                        for (int m = 3; m > r; m--) top[m] = top[m - 1];
                        top[r] = j;
                        break;
                    }
                }
            }
            if (top[0] < 0) continue;
            float total = 0.f;
            for (int r = 0; r < 4; r++) {
                if (top[r] >= 0) total += acc[top[r]];
            }
            uint32_t jw = 0, ww = 0;
            for (int r = 0; r < 4; r++) {
                if (top[r] < 0) break;
                uint32_t w8 = (uint32_t)std::lround(acc[top[r]] / total * 255.f);
                if (r == 0) w8 = std::max(w8, 1u); // dominant never rounds to 0
                jw |= (uint32_t)top[r] << (r * 8);
                ww |= w8 << (r * 8);
            }
            cellW[ci * 2] = jw;
            cellW[ci * 2 + 1] = ww;
            queue.push_back((int)ci);
        }
        for (size_t head = 0; head < queue.size(); head++) {
            int ci = queue[head];
            int x = ci % kGrid, y = (ci / kGrid) % kGrid, z = ci / (kGrid * kGrid);
            const int nb[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                  {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            for (auto& o : nb) {
                int nx = x + o[0], ny = y + o[1], nz = z + o[2];
                if (nx < 0 || ny < 0 || nz < 0 || nx >= kGrid || ny >= kGrid ||
                    nz >= kGrid)
                    continue;
                int ni = nx + ny * kGrid + nz * kGrid * kGrid;
                if (cellW[ni * 2 + 1] == 0) {
                    cellW[ni * 2] = cellW[ci * 2];
                    cellW[ni * 2 + 1] = cellW[ci * 2 + 1];
                    queue.push_back(ni);
                }
            }
        }
    }

    lap("cellW");

    auto upload = [&](const void* data, uint64_t bytes, const char* label) {
        wgpu::Buffer buf = makeStorage(dev, std::max<uint64_t>(bytes, 4), label);
        if (bytes) gpu.queue.WriteBuffer(buf, 0, data, bytes);
        return buf;
    };
    auto mesh = std::make_shared<MeshImport>();
    mesh->triCount = triCount;
    mesh->cellW = std::move(cellW);
    mesh->pos = upload(posNrm.data(), posNrm.size() * 4, "mesh pos+normal");
    mesh->idx = upload(asset.indices.data(), asset.indices.size() * 4, "mesh idx");
    mesh->col = upload(asset.colors.data(), asset.colors.size() * 4, "mesh col");
    mesh->skin = upload(skin.data(), skin.size() * 4, "mesh skin");
    // merged [offsets][ids] — stays inside the 10-storage-buffer limit
    std::vector<uint32_t> merged;
    merged.reserve(offsets.size() + ids.size());
    merged.insert(merged.end(), offsets.begin(), offsets.end());
    merged.insert(merged.end(), ids.begin(), ids.end());
    mesh->tris = upload(merged.data(), merged.size() * 4, "cell tris");
    mesh->inside = upload(inside.data(), inside.size() * 4, "inside bits");

    std::printf("import: %u tris binned (%zu refs), parity done\n", triCount, ids.size());
    return mesh;
}

void BrickSystem::requestImport(std::shared_ptr<MeshImport> mesh) {
    if (!mesh) return;
    wgpu::Device& dev = gpu_->device;
    mesh_ = std::move(mesh);

    // per-fighter: this volume's own cell-weight copy and parity scratch
    gpu_->queue.WriteBuffer(volume, kCellWOff, mesh_->cellW.data(),
                            mesh_->cellW.size() * 4);
    const uint32_t axisVox = kAxisVox, rowWords = kRowWords;
    vxParityBuf_ = makeStorage(dev, (uint64_t)axisVox * axisVox * rowWords * 4,
                               "voxel parity");

    auto bindv = [&](wgpu::ComputePipeline& pipe, uint32_t group,
                     std::vector<BindBuf> entries) {
        return makeBindGroup(dev, pipe, group, entries);
    };
    // classify's auto layout skips bindings it doesn't touch; fill uses all.
    vxG0_ = bindv(vxFill_, 0,
                  {{0, mesh_->pos}, {1, mesh_->idx}, {2, mesh_->col},
                   {3, mesh_->skin}, {4, mesh_->tris}, {5, vxParityBuf_}});
    vxG1_ = bindv(vxFill_, 1,
                  {{0, volume, kIndOff, kCellBytes}, {1, distPool},
                   {2, albedoPool}, {3, weightPool}});
    vxG2_ = bindv(vxClassify_, 2, {{0, counters_}, {1, freelist_}});

    importPending_ = true;
    bakePending_ = false;
}

void BrickSystem::encodeImport(wgpu::CommandEncoder& enc) {
    enc.ClearBuffer(volume, kIndOff, kCellBytes);
    enc.ClearBuffer(counters_, 0, 16);
    // per-pipeline bind groups against each auto layout
    auto bindc2 = [&](wgpu::ComputePipeline& pipe, uint32_t group,
                      std::vector<BindBuf> es) {
        return makeBindGroup(gpu_->device, pipe, group, es);
    };
    wgpu::BindGroup cg0 = bindc2(vxClassify_, 0, {{0, mesh_->pos}, {1, mesh_->idx},
                                                  {4, mesh_->tris}, {6, mesh_->inside}});
    wgpu::BindGroup cg1 = bindc2(vxClassify_, 1, {{0, volume, kIndOff, kCellBytes}});
    wgpu::BindGroup pg0 = bindc2(vxParity_, 0,
                                 {{0, mesh_->pos}, {1, mesh_->idx}, {5, vxParityBuf_}});

    const uint32_t wg = (kGrid + 3) / 4;
    const uint32_t axisVox = kAxisVox;
    wgpu::ComputePassEncoder pass = enc.BeginComputePass();
    // exact per-voxel signs first: watertight raycast parity
    pass.SetPipeline(vxParity_);
    pass.SetBindGroup(0, pg0);
    pass.DispatchWorkgroups((axisVox + 7) / 8, (axisVox + 7) / 8);
    pass.SetPipeline(vxClassify_);
    pass.SetBindGroup(0, cg0);
    pass.SetBindGroup(1, cg1);
    pass.SetBindGroup(2, vxG2_);
    pass.DispatchWorkgroups(wg, wg, wg);
    pass.SetPipeline(vxFill_);
    pass.SetBindGroup(0, vxG0_);
    pass.SetBindGroup(1, vxG1_);
    pass.DispatchWorkgroups(kGrid, kGrid, kGrid);
    pass.End();
}

bool BrickSystem::editInBounds(const BrickEdit& e) const {
    if (e.mode == 0) return true;
    // brush influence (radius + band margin) must stay inside the volume:
    // a blob clipped at the boundary renders as corruption
    const float m = e.radius + 3.0f * kSpan;
    for (int i = 0; i < 3; i++) {
        // a capsule edit must clear the boundary at BOTH ends
        float lo = std::min(e.pos[i], e.segment ? e.posB[i] : e.pos[i]);
        float hi = std::max(e.pos[i], e.segment ? e.posB[i] : e.pos[i]);
        if (lo - m < kOrigin[i] || hi + m > kOrigin[i] + kGrid * kSpan) {
            return false;
        }
    }
    return true;
}

void BrickSystem::queueEdit(const BrickEdit& e) {
    if (!editInBounds(e)) return;
    pending_.push_back(e);
}

void BrickSystem::pollVolumes() {
    for (VolSlot& s : volSlots_) {
        if (!s.copied || s.mapping) continue;
        s.copied = false;
        s.mapping = true;
        auto alive = alive_;
        s.buf.MapAsync(wgpu::MapMode::Read, 0, 4, wgpu::CallbackMode::AllowSpontaneous,
                       [this, &s, alive](wgpu::MapAsyncStatus status, wgpu::StringView) {
                           if (!*alive) return; // object gone; &s/this dangle
                           if (status == wgpu::MapAsyncStatus::Success) {
                               // stale generation: measured against a volume a
                               // snapshot load replaced — drop, don't ledger it
                               if (s.gen == snapGen_) {
                                   uint32_t fp =
                                       *(const uint32_t*)s.buf.GetConstMappedRange(0, 4);
                                   if (std::getenv("CLAYFRAY_DEBUG_LEDGER")) {
                                       std::printf("[vol] raw fp=%u mode=%d\n", fp,
                                                   s.edit.mode);
                                   }
                                   MeasuredEdit m;
                                   m.edit = s.edit;
                                   // fixed-point voxels (x1024) -> m^3
                                   m.volume = (float)((double)fp / 1024.0 *
                                                      (double)kVoxel * (double)kVoxel *
                                                      (double)kVoxel);
                                   measured_.push_back(m);
                               }
                               s.buf.Unmap();
                           }
                           s.mapping = false;
                       });
    }
}

bool BrickSystem::takeMeasured(MeasuredEdit& out) {
    if (measured_.empty()) return false;
    out = measured_.front();
    measured_.erase(measured_.begin());
    return true;
}

bool BrickSystem::measurementsIdle() const {
    for (const VolSlot& s : volSlots_) {
        if (s.copied || s.mapping) return false;
    }
    return true;
}

bool BrickSystem::save(SnapWriter& w) {
    const uint32_t cells = kGrid * kGrid * kGrid;
    std::vector<uint8_t> ind = readbackBuffer(*gpu_, volume, cells * 4ull, kIndOff);
    if (ind.empty()) return false;
    // pools are saved as a dense prefix up to the brick high-water mark:
    // alloc is counter-based so live indices are packed low, and freed holes
    // inside the prefix are rewritten in full on realloc (harmless garbage)
    const uint32_t* ip = (const uint32_t*)ind.data();
    uint32_t hi = 0;
    bool any = false;
    for (uint32_t i = 0; i < cells; i++) {
        if (ip[i] & 0x80000000u) {
            any = true;
            hi = std::max(hi, ip[i] & 0x000FFFFFu);
        }
    }
    const uint32_t bricks = any ? hi + 1 : 0;
    uint32_t meta[4] = {kGrid, kMaxBricks, bricks, (uint32_t)pending_.size()};
    w.section("BMET", meta, sizeof(meta));
    w.section("BIND", ind.data(), ind.size());

    // Sections keep their pre-merge names and contents; three of them now
    // read out of a region of `volume` rather than a buffer of their own, so
    // old snapshots still load.
    struct Sect {
        const char* tag;
        wgpu::Buffer buf;
        uint64_t size;
        uint64_t offset = 0;
    };
    const Sect sects[] = {
        {"BCLW", volume, cells * 8ull, kCellWOff},
        {"BCNT", counters_, 16},
        {"BFRE", freelist_, kMaxBricks * 4ull},
        {"BJFA", volume, cells * 4ull, kSeedOff},   // jfaB_ is per-pass scratch
        {"BCRS", volume, cells * 4ull, kCoarseOff}, // coarseB_ likewise
        {"BDST", distPool, bricks * 256ull * 4ull},
        {"BALB", albedoPool, bricks * 512ull * 4ull},
        {"BWGT", weightPool, bricks * 512ull * 4ull},
    };
    for (const Sect& s : sects) {
        if (s.size == 0) {
            w.section(s.tag, nullptr, 0);
            continue;
        }
        std::vector<uint8_t> data = readbackBuffer(*gpu_, s.buf, s.size, s.offset);
        if (data.empty()) return false;
        w.section(s.tag, data.data(), data.size());
    }
    w.section("BPND", pending_.data(), pending_.size() * sizeof(BrickEdit));
    return true;
}

bool BrickSystem::load(SnapReader& r) {
    const uint32_t cells = kGrid * kGrid * kGrid;
    uint32_t meta[4];
    if (!r.read("BMET", meta, sizeof(meta))) {
        std::fprintf(stderr, "[snap] missing/short BMET section\n");
        return false;
    }
    if (meta[0] != kGrid || meta[1] != kMaxBricks) {
        std::fprintf(stderr, "[snap] grid mismatch: file %ux%u, build %dx%u\n",
                     meta[0], meta[1], kGrid, kMaxBricks);
        return false;
    }
    const uint32_t bricks = meta[2];
    struct Sect {
        const char* tag;
        wgpu::Buffer buf;
        uint64_t size;
        uint64_t offset = 0;
    };
    const Sect sects[] = {
        {"BIND", volume, cells * 4ull, kIndOff},
        {"BCLW", volume, cells * 8ull, kCellWOff},
        {"BCNT", counters_, 16},
        {"BFRE", freelist_, kMaxBricks * 4ull},
        {"BJFA", volume, cells * 4ull, kSeedOff},
        {"BCRS", volume, cells * 4ull, kCoarseOff},
        {"BDST", distPool, bricks * 256ull * 4ull},
        {"BALB", albedoPool, bricks * 512ull * 4ull},
        {"BWGT", weightPool, bricks * 512ull * 4ull},
    };
    for (const Sect& s : sects) {
        if (s.size == 0) continue;
        std::vector<uint8_t> data = r.blob(s.tag);
        if (data.size() != s.size) {
            std::fprintf(stderr, "[snap] section %s: %zu bytes, expected %llu\n",
                         s.tag, data.size(), (unsigned long long)s.size);
            return false;
        }
        gpu_->queue.WriteBuffer(s.buf, s.offset, data.data(), data.size());
    }
    pending_.resize(meta[3]);
    if (meta[3] &&
        !r.read("BPND", pending_.data(), meta[3] * sizeof(BrickEdit))) {
        pending_.clear();
    }
    // the snapshot IS the volume: cancel any queued bake/import, and poison
    // in-flight volume measurements (they measured the replaced state)
    bakePending_ = false;
    importPending_ = false;
    measured_.clear();
    snapGen_++;
    for (VolSlot& s : volSlots_) s.copied = false;
    editsSinceCap_ = 0;
    return true;
}

void BrickSystem::finishCapacityPoll() {
    if (!capPollArmed_ || capMapPending_) return;
    capPollArmed_ = false;
    capMapPending_ = true;
    auto alive = alive_;
    capRead_.MapAsync(wgpu::MapMode::Read, 0, 16, wgpu::CallbackMode::AllowSpontaneous,
                      [this, alive](wgpu::MapAsyncStatus status, wgpu::StringView) {
                          if (!*alive) return;
                          if (status == wgpu::MapAsyncStatus::Success) {
                              const uint32_t* c =
                                  (const uint32_t*)capRead_.GetConstMappedRange(0, 16);
                              uint32_t used = c[0] - std::min(c[0], c[1]);
                              if (!capWarned_ && used > kMaxBricks * 9 / 10) {
                                  capWarned_ = true;
                                  std::fprintf(stderr,
                                               "[brick] pool nearly full: %u/%u bricks "
                                               "(sculpting may develop holes)\n",
                                               used, kMaxBricks);
                              }
                              capRead_.Unmap();
                          }
                          capMapPending_ = false;
                      });
}

void BrickSystem::encodeOp(wgpu::CommandEncoder& enc, const BrickEdit& e, int slot) {
    EditParamsCpu params{};
    int regionDims[3];
    if (e.mode == 0) {
        params.regionMin[0] = params.regionMin[1] = params.regionMin[2] = 0;
        regionDims[0] = regionDims[1] = regionDims[2] = kGrid;
    } else {
        for (int i = 0; i < 3; i++) {
            // margin = brush influence reach (band, 1.7 spans) + cell diagonal
            // (0.87) + slack: cells outside the region keep stale voxels, and a
            // too-small margin leaves seams at shared voxel planes
            float pa = e.pos[i];
            float pb = e.segment ? e.posB[i] : e.pos[i];
            float lo = std::min(pa, pb) - e.radius - 3.0f * kSpan - kOrigin[i];
            float hi = std::max(pa, pb) + e.radius + 3.0f * kSpan - kOrigin[i];
            int c0 = (int)std::floor(lo / kSpan);
            int c1 = (int)std::floor(hi / kSpan);
            params.regionMin[i] = c0;
            regionDims[i] = c1 - c0 + 1;
        }
    }
    params.regionDims[0] = regionDims[0];
    params.regionDims[1] = regionDims[1];
    params.regionDims[2] = regionDims[2];
    params.brush[0] = e.pos[0];
    params.brush[1] = e.pos[1];
    params.brush[2] = e.pos[2];
    params.brush[3] = e.radius;
    for (int i = 0; i < 3; i++) params.brushB[i] = e.segment ? e.posB[i] : e.pos[i];
    params.brushB[3] = 0.f;
    params.color[0] = e.color[0];
    params.color[1] = e.color[1];
    params.color[2] = e.color[2];
    params.color[3] = (float)e.mode;
    gpu_->queue.WriteBuffer(editParams_[slot], 0, &params, sizeof(params));

    wgpu::ComputePassEncoder pass = enc.BeginComputePass();
    pass.SetPipeline(classify_);
    pass.SetBindGroup(0, classifyG0_[slot]);
    pass.SetBindGroup(1, classifyG1_);
    pass.SetBindGroup(2, classifyG2_);
    pass.DispatchWorkgroups((regionDims[0] + 3) / 4, (regionDims[1] + 3) / 4,
                            (regionDims[2] + 3) / 4);
    pass.SetPipeline(fill_);
    pass.SetBindGroup(0, fillG0_[slot]);
    pass.SetBindGroup(1, fillG1_);
    pass.SetBindGroup(2, fillG2_);
    pass.DispatchWorkgroups(regionDims[0], regionDims[1], regionDims[2]);
    pass.End();
}

void BrickSystem::encodeJfa(wgpu::CommandEncoder& enc) {
    const uint32_t wg = (kGrid + 3) / 4;
    wgpu::ComputePassEncoder pass = enc.BeginComputePass();
    pass.SetPipeline(jfaInit_);
    pass.SetBindGroup(0, jfaInitG_);
    pass.DispatchWorkgroups(wg, wg, wg);
    pass.SetPipeline(jfaStep_);
    for (int i = 0; i < kJfaSteps; i++) {
        pass.SetBindGroup(0, jfaStepG_[i]);
        pass.DispatchWorkgroups(wg, wg, wg);
    }
    pass.SetPipeline(jfaResolve_);
    pass.SetBindGroup(0, jfaResolveG_);
    pass.DispatchWorkgroups(wg, wg, wg);
    pass.SetPipeline(jfaRelax_);
    for (int i = 0; i < 4; i++) {
        pass.SetBindGroup(0, jfaRelaxG_[i]);
        pass.DispatchWorkgroups(wg, wg, wg);
    }
    pass.End();
}

void BrickSystem::encodeRedistance(wgpu::CommandEncoder& enc) {
    const uint32_t cells = kGrid * kGrid * kGrid;
    enc.ClearBuffer(counters_, 8, 4); // dirty count = 0
    wgpu::ComputePassEncoder pass = enc.BeginComputePass();
    pass.SetPipeline(rdCompact_);
    pass.SetBindGroup(0, rdCompactG_);
    pass.DispatchWorkgroups((cells + 63) / 64);
    pass.SetPipeline(rdPrep_);
    pass.SetBindGroup(0, rdPrepG_);
    pass.DispatchWorkgroups(1);
    pass.SetPipeline(rdRedist_);
    pass.SetBindGroup(0, rdRedistG_);
    // 3 rounds: apron reloads between rounds carry healing across bricks
    for (int i = 0; i < 3; i++) {
        pass.DispatchWorkgroupsIndirect(indirectArgs_, 0);
    }
    pass.SetPipeline(rdClear_);
    pass.SetBindGroup(0, rdClearG_);
    pass.DispatchWorkgroupsIndirect(indirectArgs_, 12);
    pass.End();
}

void BrickSystem::encode(wgpu::CommandEncoder& enc) {
    if (importPending_) {
        gen_++;
        encodeImport(enc);
        encodeJfa(enc);
        importPending_ = false;
        return;
    }
    if (bakePending_) {
        // fresh volume: zero the indirection (empty-outside) and counters
        enc.ClearBuffer(volume, kIndOff, kCellBytes);
        enc.ClearBuffer(counters_, 0, 16);
        BrickEdit bake{};
        bake.mode = 0;
        gen_++;
        encodeOp(enc, bake, 0);
        encodeJfa(enc);
        bakePending_ = false;
        return;
    }
    if (!pending_.empty()) {
      // Drain several ops per frame. They are independent carves into the same
      // volume, so ONE JFA + redistance after the batch refreshes all of them —
      // the sweep substeps of a sword cut land in the same frame instead of
      // unzipping one per frame.
      gen_++;
      const int batch = std::min((int)pending_.size(), kOpsPerFrame);
      for (int op = 0; op < batch; op++) {
        BrickEdit e = pending_.front();
        pending_.erase(pending_.begin());
        enc.ClearBuffer(counters_, 12, 4); // volume delta = 0
        encodeOp(enc, e, op);
        // conservation: park the measured delta in a free pool slot; the
        // renderer maps it after submit. Pool grows to cover however far the
        // CPU runs ahead of the GPU — a dropped measurement is leaked clay.
        {
            VolSlot* slot = nullptr;
            for (VolSlot& s : volSlots_) {
                if (!s.copied && !s.mapping) {
                    slot = &s;
                    break;
                }
            }
            if (!slot) {
                volSlots_.emplace_back();
                slot = &volSlots_.back();
                wgpu::BufferDescriptor desc{};
                desc.label = "edit volume readback";
                desc.size = 4;
                desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
                slot->buf = gpu_->device.CreateBuffer(&desc);
            }
            enc.CopyBufferToBuffer(counters_, 12, slot->buf, 0, 4);
            slot->edit = e;
            slot->copied = true;
            slot->gen = snapGen_;
        }
      }
      encodeJfa(enc);
      if (!std::getenv("CLAYFRAY_NO_REDIST")) encodeRedistance(enc);
        if (++editsSinceCap_ >= 60 && !capMapPending_) {
            editsSinceCap_ = 0;
            if (!capRead_) {
                wgpu::BufferDescriptor desc{};
                desc.size = 16;
                desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
                capRead_ = gpu_->device.CreateBuffer(&desc);
            }
            enc.CopyBufferToBuffer(counters_, 0, capRead_, 0, 16);
            capPollArmed_ = true; // renderer maps it after submit
        }
    }
}
