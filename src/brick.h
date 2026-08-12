#pragma once
#include <string>
#include <vector>

#include "asset.h"
#include "gpu.h"

// Sparse brickmap for the carveable character body. Owns the GPU volume
// (indirection grid + brick pools + freelist) and the bake/edit/JFA passes.
// One edit op is consumed per frame (uniform buffer contents are per-submit).
struct BrickEdit {
    int mode = 1; // 0 bake, 1 carve, 2 add
    float pos[3] = {0, 0, 0};
    float radius = 0.05f;
    float color[3] = {0.8f, 0.5f, 0.5f};
};

class BrickSystem {
  public:
    static constexpr int kGrid = 74;
    static constexpr float kVoxel = 0.0027344f;
    static constexpr float kSpan = 0.0191406f;
    static constexpr uint32_t kMaxBricks = 49152;

    bool init(Gpu& gpu, const std::string& editSrc, const std::string& jfaSrc,
              const std::string& redistSrc, const std::string& voxelizeSrc);
    // Replaces the analytic bake with a voxelized mesh. CPU side (binning,
    // watertight parity) happens here; GPU passes run on the next encode.
    void requestImport(CharacterAsset asset);
    // Drops edits that would cross the volume boundary (a clipped blob
    // renders as corruption).
    void queueEdit(const BrickEdit& e);
    void requestBake() { bakePending_ = true; }
    bool hasPendingWork() const { return bakePending_ || !pending_.empty(); }
    // Encodes at most one op (bake or oldest queued edit) plus the JFA
    // refresh. Call once per frame before the trace pass.
    void encode(wgpu::CommandEncoder& enc);

    wgpu::Buffer indirection, distPool, albedoPool;
    wgpu::Buffer weightPool; // per-voxel packed top-2 bone weights (M4 reads)
    wgpu::Buffer seeds;  // canonical JFA output (jfaA), read by tracer/pick
    wgpu::Buffer coarse; // per-cell signed coarse distance, read by tracer

    // Blocking readbacks; debug only.
    void debugStats(const char* label);
    void debugScanField();

    // Async pool watermark check; call after queue submit.
    void finishCapacityPoll();

  private:
    void encodeOp(wgpu::CommandEncoder& enc, const BrickEdit& e);
    void encodeJfa(wgpu::CommandEncoder& enc);

    Gpu* gpu_ = nullptr;
    wgpu::ComputePipeline classify_, fill_, jfaInit_, jfaStep_, jfaResolve_, jfaRelax_;
    wgpu::Buffer coarseB_;
    wgpu::BindGroup jfaRelaxG_[4];
    void encodeRedistance(wgpu::CommandEncoder& enc);
    wgpu::ComputePipeline rdCompact_, rdPrep_, rdRedist_, rdClear_;
    wgpu::Buffer dirtyList_, indirectArgs_;
    wgpu::BindGroup rdCompactG_, rdPrepG_, rdRedistG_, rdClearG_;
    static constexpr int kJfaSteps = 9; // 7 pow2 rounds + JFA+2 refinement
    wgpu::Buffer editParams_, counters_, freelist_, jfaA_, jfaB_;
    wgpu::Buffer jfaStepParams_[kJfaSteps], jfaResolveParams_;
    wgpu::BindGroup classifyG0_, classifyG1_, classifyG2_;
    wgpu::BindGroup fillG0_, fillG1_, fillG2_;
    wgpu::BindGroup jfaInitG_, jfaStepG_[kJfaSteps], jfaResolveG_;
    std::vector<BrickEdit> pending_;
    bool bakePending_ = true;

    void encodeImport(wgpu::CommandEncoder& enc);
    wgpu::ShaderModule voxelizeMod_;
    wgpu::ComputePipeline vxClassify_, vxFill_, vxParity_;
    wgpu::BindGroup vxG0_, vxG1_, vxG2_;
    wgpu::Buffer vxPos_, vxIdx_, vxCol_, vxSkin_, vxTris_, vxParityBuf_, vxInside_;
    bool importPending_ = false;
    wgpu::Buffer capRead_;
    int editsSinceCap_ = 0;
    bool capPollArmed_ = false, capMapPending_ = false, capWarned_ = false;
};
