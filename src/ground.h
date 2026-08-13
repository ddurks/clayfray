#pragma once
#include <string>
#include <vector>

#include "gpu.h"

class SnapWriter;
class SnapReader;

// Where carved clay lands (M4.6 conservation). V1 backing store is a floor
// heightfield: thickness on top of a precomputed pebble-surface base, plus a
// per-texel color map. splat() is the deposit contract — M7 chunk settling
// can swap the store for a world brickmap without touching callers.
// Deposits are closed-form kernels, so the ledger is exact by construction.
class GroundClay {
  public:
    static constexpr int kN = 512;
    static constexpr float kOrigin = -1.75f; // field spans [-1.75, 1.75] in x,z
    static constexpr float kTexel = 3.5f / kN;
    static constexpr float kBaseTop = 0.058f; // tallest pebble the base can hold

    bool init(Gpu& gpu, const std::string& src);
    bool rebuildPipelines(const std::string& src); // shader hot-reload
    // Deposit volumeM3 of clay centered at (x, z), linear color.
    void splat(float x, float z, float volumeM3, const float color[3]);
    // Encode the one-time base bake and any pending splats.
    void encode(wgpu::CommandEncoder& enc);

    // Upper bound of the clay top surface (0 while the field is empty:
    // the tracer skips the field entirely).
    float maxTopY() const { return maxH_ > 0.f ? kBaseTop + maxH_ : 0.f; }
    // Approximate landing height for gob collision: coarse CPU mirror of the
    // deposited thickness over a mid-pebble base guess.
    float approxTopAt(float x, float z) const;

    // Snapshot the deposited field (GPU maps + CPU mirror). Blocking.
    bool save(SnapWriter& w);
    bool load(SnapReader& r);

    wgpu::Buffer base, height, color;

  private:
    struct Op {
        float x, z, radius, amp;
        float col[3];
    };
    void buildBindGroups();
    float thicknessAt(float x, float z) const;

    Gpu* gpu_ = nullptr;
    std::vector<Op> pending_;
    wgpu::ComputePipeline initPipe_, splatPipe_;
    static constexpr int kOpsPerFrame = 16;
    wgpu::Buffer params_[kOpsPerFrame];
    wgpu::BindGroup initG_, splatG_[kOpsPerFrame];
    bool basePending_ = true;
    float maxH_ = 0.f;
    // 64x64 thickness mirror so gobs land on top of existing piles
    static constexpr int kMirror = 64;
    float mirror_[kMirror * kMirror] = {};
};
