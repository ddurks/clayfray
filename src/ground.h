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

    // ---- base + height + color share ONE buffer ----
    // Core WebGPU guarantees a shader stage only EIGHT storage buffers
    // (maxStorageBuffersPerShaderStage). `trace` bound nine: three per fighter
    // (brick.h packs each fighter's per-cell arrays for the same reason) plus
    // this trio. Desktop Chrome reports the adapter's real limit so it worked,
    // but a mobile browser reporting the guaranteed minimum would fail to
    // CREATE the trace pipeline — the exact failure mode that made macOS draw
    // nothing when Metal's 10 was exceeded (CLAUDE.md trap 8). Packing the
    // trio takes trace to seven, inside the guaranteed minimum.
    //
    // Same technique as brick.h: a pass that WRITES a region binds just that
    // region as a sub-range, so ground.wgsl keeps its own three bindings and
    // needs no index arithmetic; the tracer binds the whole buffer once and
    // adds a base index (G_BASE/G_HEIGHT/G_COLOR from wgslConstants(), via the
    // accessors at the top of ground_read.wgsl).
    //
    // Every write-side binding of these regions is read_write ON PURPOSE:
    // Dawn rejects a buffer that is writable and read-only within one pass and
    // tracks that per BUFFER, not per bound range — so a single `read` binding
    // on any region would break every pass that writes another. The tracer's
    // read-only binding is safe only because it is a different pass.
    //
    // Region map (kN = 512, so a region is exactly 1 MiB and already 256-byte
    // aligned; kAlign guards the invariant if kN ever stops being a power of
    // two):
    //     0x000000  base    512*512 f32   floor drape height, baked once
    //     0x100000  height  512*512 f32   deposited clay thickness
    //     0x200000  color   512*512 u32   packed rgba8 clay color
    //     0x300000  = kFieldBytes (3 MiB)
    static constexpr uint64_t kTexels = (uint64_t)kN * kN;
    static constexpr uint64_t kTexelBytes = kTexels * 4;
    static constexpr uint64_t kAlign = 256; // minStorageBufferOffsetAlignment
    static constexpr uint64_t kRegionStride = (kTexelBytes + kAlign - 1) & ~(kAlign - 1);
    static constexpr uint64_t kBaseOff = 0;
    static constexpr uint64_t kHeightOff = kRegionStride;
    static constexpr uint64_t kColorOff = kRegionStride * 2;
    // 0x300000  coarse  64*64 f32   metres to the nearest clay-bearing tile
    //
    // A FOURTH region, and it is a march accelerator rather than scene data.
    // groundClayDist has to return something POSITIVE for a column with no
    // clay in it, or the field wins the hit on bare floor — and it used to
    // return a 6 mm constant, which is safe and catastrophic: the march steps
    // by 0.85*d, so 6 mm caps a ray at 256 * 5.1 mm = 1.3 m of travel. A
    // grazing ray needs several metres to reach the floor and simply died,
    // falling through to background() — black. This region is what lets that
    // column say "the nearest clay is 40 cm away" instead.
    static constexpr int kCoarse = 64; // == kMirror; the DT is built from it
    static constexpr float kCoarseTexel = 3.5f / kCoarse;
    static constexpr uint64_t kCoarseBytes = (uint64_t)kCoarse * kCoarse * 4;
    static constexpr uint64_t kCoarseStride = (kCoarseBytes + kAlign - 1) & ~(kAlign - 1);
    static constexpr uint64_t kCoarseOff = kRegionStride * 3;
    static constexpr uint64_t kFieldBytes = kCoarseOff + kCoarseStride;

    // The region map above as WGSL, stitched into every shader root by the
    // renderer's `//#constants` directive alongside BrickSystem's block. The
    // read side must NOT hand-copy these — kN is a snapshot-versioned size
    // constant, and a drifted copy addresses the wrong region.
    static std::string wgslConstants();

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

    // Bumped whenever encode() emits work that changes the field. The
    // renderer folds this into its frame-reuse digest: the field's CONTENTS
    // are not in the uniform buffer, so nothing else would notice a splat.
    uint32_t generation() const { return gen_; }

    // base + height + color packed at kBaseOff/kHeightOff/kColorOff above.
    // One buffer, one trace binding.
    wgpu::Buffer field;

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
    uint32_t gen_ = 0;
    float maxH_ = 0.f;
    // Lateral distance (metres, conservative) from each coarse tile to the
    // nearest tile holding clay. Rebuilt from mirror_ whenever clay lands, so
    // it needs no snapshot section of its own — GMIR already persists mirror_.
    void rebuildCoarse();
    float coarseDist_[kCoarse * kCoarse] = {};
    bool coarseDirty_ = true;
    // 64x64 thickness mirror so gobs land on top of existing piles
    static constexpr int kMirror = 64;
    float mirror_[kMirror * kMirror] = {};
};
