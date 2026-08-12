#include "brick.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

constexpr float kOrigin[3] = {-0.7082f, -0.1582f, -0.7082f};

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
    return device.CreateComputePipeline(&desc);
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

struct EditParamsCpu {
    int32_t regionMin[4];
    int32_t regionDims[4];
    float brush[4];
    float color[4];
};

} // namespace

bool BrickSystem::init(Gpu& gpu, const std::string& editSrc, const std::string& jfaSrc,
                       const std::string& redistSrc, const std::string& voxelizeSrc) {
    gpu_ = &gpu;
    wgpu::Device& dev = gpu.device;
    const uint32_t cells = kGrid * kGrid * kGrid;

    indirection = makeStorage(dev, cells * 4ull, "brick indirection");
    distPool = makeStorage(dev, kMaxBricks * 256ull * 4ull, "brick dist pool");
    albedoPool = makeStorage(dev, kMaxBricks * 512ull * 4ull, "brick albedo pool");
    weightPool = makeStorage(dev, kMaxBricks * 512ull * 4ull, "brick weight pool");
    counters_ = makeStorage(dev, 16, "brick counters");
    freelist_ = makeStorage(dev, kMaxBricks * 4ull, "brick freelist");
    jfaA_ = makeStorage(dev, cells * 4ull, "jfa A");
    jfaB_ = makeStorage(dev, cells * 4ull, "jfa B");
    seeds = jfaA_;
    coarse = makeStorage(dev, cells * 4ull, "coarse dist");
    coarseB_ = makeStorage(dev, cells * 4ull, "coarse dist B");
    static_assert(sizeof(EditParamsCpu) == 64, "must match WGSL EditParams");
    editParams_ = makeStorage(dev, sizeof(EditParamsCpu), "edit params", true);

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

    dirtyList_ = makeStorage(dev, 65535ull * 4, "dirty list");
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
                    std::vector<std::pair<uint32_t, wgpu::Buffer>> entries) {
        std::vector<wgpu::BindGroupEntry> e(entries.size());
        for (size_t i = 0; i < entries.size(); i++) {
            e[i].binding = entries[i].first;
            e[i].buffer = entries[i].second;
        }
        wgpu::BindGroupDescriptor desc{};
        desc.layout = pipe.GetBindGroupLayout(group);
        desc.entryCount = e.size();
        desc.entries = e.data();
        return gpu_->device.CreateBindGroup(&desc);
    };

    classifyG0_ = bind(classify_, 0, {{0, editParams_}});
    classifyG1_ = bind(classify_, 1, {{0, indirection}});
    classifyG2_ = bind(classify_, 2, {{0, counters_}, {1, freelist_}});
    fillG0_ = bind(fill_, 0, {{0, editParams_}});
    fillG1_ = bind(fill_, 1, {{0, indirection}, {1, distPool}, {2, albedoPool}});
    fillG2_ = bind(fill_, 2, {{0, counters_}, {1, freelist_}});
    jfaInitG_ = bind(jfaInit_, 0, {{0, indirection}, {1, jfaA_}});
    for (int i = 0; i < kJfaSteps; i++) {
        jfaStepG_[i] = bind(jfaStep_, 0,
                            {{1, jfaA_}, {2, jfaB_}, {3, jfaStepParams_[i]}});
    }
    jfaResolveG_ = bind(jfaResolve_, 0,
                        {{0, indirection}, {1, jfaA_}, {2, jfaB_}, {3, jfaResolveParams_},
                         {4, distPool}, {5, coarse}});
    rdCompactG_ = bind(rdCompact_, 0, {{0, indirection}, {2, dirtyList_}, {3, counters_}});
    rdPrepG_ = bind(rdPrep_, 0, {{3, counters_}, {4, indirectArgs_}});
    rdRedistG_ = bind(rdRedist_, 0, {{0, indirection}, {1, distPool}, {2, dirtyList_}});
    rdClearG_ = bind(rdClear_, 0, {{0, indirection}, {2, dirtyList_}, {3, counters_}});
    // 4 relax rounds alternating direction; even count ends back in `coarse`
    for (int i = 0; i < 4; i++) {
        jfaRelaxG_[i] = bind(jfaRelax_, 0,
                             {{3, jfaStepParams_[i % 2 == 0 ? 6 : 5]}, // srcIsA: 1,0,1,0
                              {5, coarse},
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
    enc.CopyBufferToBuffer(indirection, 0, rb, 0, cells * 4ull);
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
                "allocTop=%u freeTop=%u\n",
                label, alloc, fresh, inside, maxIdx, d[cells], d[cells + 1]);
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
    enc.CopyBufferToBuffer(indirection, 0, rb, 0, cells * 4ull);
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
    cenc.CopyBufferToBuffer(coarse, 0, crb, 0, cells * 4ull);
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

void BrickSystem::requestImport(CharacterAsset asset) {
    wgpu::Device& dev = gpu_->device;
    const uint32_t cells = kGrid * kGrid * kGrid;
    const uint32_t triCount = asset.triangleCount();
    const uint32_t vertCount = asset.vertexCount();
    if (triCount == 0) return;

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

    auto upload = [&](const void* data, uint64_t bytes, const char* label) {
        wgpu::Buffer buf = makeStorage(dev, std::max<uint64_t>(bytes, 4), label);
        if (bytes) gpu_->queue.WriteBuffer(buf, 0, data, bytes);
        return buf;
    };
    vxPos_ = upload(posNrm.data(), posNrm.size() * 4, "mesh pos+normal");
    vxIdx_ = upload(asset.indices.data(), asset.indices.size() * 4, "mesh idx");
    vxCol_ = upload(asset.colors.data(), asset.colors.size() * 4, "mesh col");
    vxSkin_ = upload(skin.data(), skin.size() * 4, "mesh skin");
    // merged [offsets][ids] — stays inside the 10-storage-buffer limit
    std::vector<uint32_t> merged;
    merged.reserve(offsets.size() + ids.size());
    merged.insert(merged.end(), offsets.begin(), offsets.end());
    merged.insert(merged.end(), ids.begin(), ids.end());
    vxTris_ = upload(merged.data(), merged.size() * 4, "cell tris");
    vxInside_ = upload(inside.data(), inside.size() * 4, "inside bits");
    const uint32_t axisVox = kGrid * 7 + 1, rowWords = 17;
    vxParityBuf_ = makeStorage(dev, (uint64_t)axisVox * axisVox * rowWords * 4,
                               "voxel parity");

    auto bindv = [&](wgpu::ComputePipeline& pipe, uint32_t group,
                     std::vector<std::pair<uint32_t, wgpu::Buffer>> entries) {
        std::vector<wgpu::BindGroupEntry> e(entries.size());
        for (size_t i = 0; i < entries.size(); i++) {
            e[i].binding = entries[i].first;
            e[i].buffer = entries[i].second;
        }
        wgpu::BindGroupDescriptor desc{};
        desc.layout = pipe.GetBindGroupLayout(group);
        desc.entryCount = e.size();
        desc.entries = e.data();
        return dev.CreateBindGroup(&desc);
    };
    // classify's auto layout skips bindings it doesn't touch; fill uses all.
    vxG0_ = bindv(vxFill_, 0,
                  {{0, vxPos_}, {1, vxIdx_}, {2, vxCol_}, {3, vxSkin_},
                   {4, vxTris_}, {5, vxParityBuf_}});
    vxG1_ = bindv(vxFill_, 1,
                  {{0, indirection}, {1, distPool}, {2, albedoPool}, {3, weightPool}});
    vxG2_ = bindv(vxClassify_, 2, {{0, counters_}, {1, freelist_}});

    std::printf("import: %u tris binned (%zu refs), parity done\n", triCount, ids.size());
    importPending_ = true;
    bakePending_ = false;
}

void BrickSystem::encodeImport(wgpu::CommandEncoder& enc) {
    enc.ClearBuffer(indirection, 0, (uint64_t)kGrid * kGrid * kGrid * 4);
    enc.ClearBuffer(counters_, 0, 16);
    // per-pipeline bind groups against each auto layout
    auto bindc2 = [&](wgpu::ComputePipeline& pipe, uint32_t group,
                      std::vector<std::pair<uint32_t, wgpu::Buffer>> es) {
        std::vector<wgpu::BindGroupEntry> e(es.size());
        for (size_t i = 0; i < es.size(); i++) {
            e[i].binding = es[i].first;
            e[i].buffer = es[i].second;
        }
        wgpu::BindGroupDescriptor desc{};
        desc.layout = pipe.GetBindGroupLayout(group);
        desc.entryCount = e.size();
        desc.entries = e.data();
        return gpu_->device.CreateBindGroup(&desc);
    };
    wgpu::BindGroup cg0 = bindc2(vxClassify_, 0, {{0, vxPos_}, {1, vxIdx_},
                                                  {4, vxTris_}, {6, vxInside_}});
    wgpu::BindGroup cg1 = bindc2(vxClassify_, 1, {{0, indirection}});
    wgpu::BindGroup pg0 = bindc2(vxParity_, 0,
                                 {{0, vxPos_}, {1, vxIdx_}, {5, vxParityBuf_}});

    const uint32_t wg = (kGrid + 3) / 4;
    const uint32_t axisVox = kGrid * 7 + 1;
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

void BrickSystem::queueEdit(const BrickEdit& e) {
    if (e.mode != 0) {
        // brush influence (radius + band margin) must stay inside the volume:
        // a blob clipped at the boundary renders as corruption
        const float m = e.radius + 3.0f * kSpan;
        for (int i = 0; i < 3; i++) {
            if (e.pos[i] - m < kOrigin[i] || e.pos[i] + m > kOrigin[i] + kGrid * kSpan) {
                return;
            }
        }
    }
    pending_.push_back(e);
}

void BrickSystem::finishCapacityPoll() {
    if (!capPollArmed_ || capMapPending_) return;
    capPollArmed_ = false;
    capMapPending_ = true;
    capRead_.MapAsync(wgpu::MapMode::Read, 0, 16, wgpu::CallbackMode::AllowSpontaneous,
                      [this](wgpu::MapAsyncStatus status, wgpu::StringView) {
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

void BrickSystem::encodeOp(wgpu::CommandEncoder& enc, const BrickEdit& e) {
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
            float lo = e.pos[i] - e.radius - 3.0f * kSpan - kOrigin[i];
            float hi = e.pos[i] + e.radius + 3.0f * kSpan - kOrigin[i];
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
    params.color[0] = e.color[0];
    params.color[1] = e.color[1];
    params.color[2] = e.color[2];
    params.color[3] = (float)e.mode;
    gpu_->queue.WriteBuffer(editParams_, 0, &params, sizeof(params));

    wgpu::ComputePassEncoder pass = enc.BeginComputePass();
    pass.SetPipeline(classify_);
    pass.SetBindGroup(0, classifyG0_);
    pass.SetBindGroup(1, classifyG1_);
    pass.SetBindGroup(2, classifyG2_);
    pass.DispatchWorkgroups((regionDims[0] + 3) / 4, (regionDims[1] + 3) / 4,
                            (regionDims[2] + 3) / 4);
    pass.SetPipeline(fill_);
    pass.SetBindGroup(0, fillG0_);
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
        encodeImport(enc);
        encodeJfa(enc);
        importPending_ = false;
        return;
    }
    if (bakePending_) {
        // fresh volume: zero the indirection (empty-outside) and counters
        enc.ClearBuffer(indirection, 0, kGrid * kGrid * kGrid * 4ull);
        enc.ClearBuffer(counters_, 0, 16);
        BrickEdit bake{};
        bake.mode = 0;
        encodeOp(enc, bake);
        encodeJfa(enc);
        bakePending_ = false;
        return;
    }
    if (!pending_.empty()) {
        BrickEdit e = pending_.front();
        pending_.erase(pending_.begin());
        encodeOp(enc, e);
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
