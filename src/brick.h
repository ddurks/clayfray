#pragma once
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "asset.h"
#include "gpu.h"

class SnapWriter;
class SnapReader;

// Sparse brickmap for the carveable character body. Owns the GPU volume
// (indirection grid + brick pools + freelist) and the bake/edit/JFA passes.
// One edit op is consumed per frame (uniform buffer contents are per-submit).
struct BrickEdit {
    int mode = 1; // 0 bake, 1 carve, 2 add
    float pos[3] = {0, 0, 0};
    float radius = 0.05f;
    float color[3] = {0.8f, 0.5f, 0.5f};
    // Conservation metadata (rides along; the GPU op ignores it). outDir and
    // srcColor snapshot the pick normal/albedo at queue time so gobs fly
    // outward from the wound wearing its color.
    float outDir[3] = {0.f, 1.f, 0.f};
    float srcColor[3] = {0.024f, 0.19f, 0.25f}; // linear body cyan fallback
    bool fromGob = false;  // deposit of a landed gob (ledger reconciles)
    float gobVol = 0.f;    // the landed gob's intended volume, m^3
};

// One edit op's measured |volume change| (carve: removed, add: created),
// delivered a frame or two after the op ran.
struct MeasuredEdit {
    BrickEdit edit;
    float volume = 0.f; // m^3
};

class BrickSystem {
  public:
    static constexpr int kGrid = 74;
    static constexpr float kVoxel = 0.0027344f;
    static constexpr float kSpan = 0.0191406f;
    static constexpr uint32_t kMaxBricks = 49152;

    // Flip the alive sentinel so any MapAsync callback still queued at
    // teardown bails before it touches destroyed buffers/members.
    ~BrickSystem() { *alive_ = false; }

    bool init(Gpu& gpu, const std::string& editSrc, const std::string& jfaSrc,
              const std::string& redistSrc, const std::string& voxelizeSrc);
    // Replaces the analytic bake with a voxelized mesh. CPU side (binning,
    // watertight parity) happens here; GPU passes run on the next encode.
    void requestImport(CharacterAsset asset);
    // Drops edits that would cross the volume boundary (a clipped blob
    // renders as corruption).
    void queueEdit(const BrickEdit& e);
    // Same clip test queueEdit applies, without queueing: lets gob landings
    // pick "stick" vs "deflect" before committing.
    bool editInBounds(const BrickEdit& e) const;
    // Rebuild the volume from its source. With an imported character the
    // mesh buffers persist on the GPU, so re-run the import passes; the
    // analytic bake would silently replace the fighter with the blob.
    void requestBake() {
        if (vxTris_) importPending_ = true;
        else bakePending_ = true;
    }
    bool hasPendingWork() const { return bakePending_ || !pending_.empty(); }
    // Encodes at most one op (bake or oldest queued edit) plus the JFA
    // refresh. Call once per frame before the trace pass.
    void encode(wgpu::CommandEncoder& enc);

    wgpu::Buffer indirection, distPool, albedoPool;
    wgpu::Buffer weightPool; // per-voxel packed top-2 bone weights (M4 reads)
    // per-cell nearest-surface bone weights, flood-filled volume-wide: the
    // chunk warp needs weight guidance OUTSIDE the narrow band too (a rigid
    // warp guess at a big joint angle lands far from the surface)
    wgpu::Buffer cellWeights;
    wgpu::Buffer seeds;  // canonical JFA output (jfaA), read by tracer/pick
    wgpu::Buffer coarse; // per-cell signed coarse distance, read by tracer

    // Blocking readbacks; debug only.
    void debugStats(const char* label);
    void debugScanField();

    // Async pool watermark check; call after queue submit.
    void finishCapacityPoll();

    // Conservation: arm MapAsync on completed volume copies (call after
    // submit, like finishCapacityPoll) and drain finished measurements.
    void pollVolumes();
    bool takeMeasured(MeasuredEdit& out);
    // No volume copies queued or mapping (measured_ may still hold results).
    bool measurementsIdle() const;

    // Snapshot the full carveable state (indirection, pools up to the brick
    // high-water mark, allocator, JFA seeds, coarse field, queued edits).
    // Load restores it and cancels any pending bake/import — the snapshot IS
    // the volume. Blocking; dev tooling only.
    bool save(SnapWriter& w);
    bool load(SnapReader& r);

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

    // Volume readback pool: one op measures per frame, but maps only
    // complete once the GPU catches up — headless runs queue ~100 frames
    // ahead, so the pool grows on demand (deque: stable element addresses
    // for the MapAsync callbacks; slots are reused, never erased).
    struct VolSlot {
        wgpu::Buffer buf;
        BrickEdit edit;
        bool copied = false;  // encoded a copy this submit, map not yet armed
        bool mapping = false; // MapAsync in flight
        uint32_t gen = 0;     // snapshot generation the copy belongs to
    };
    std::deque<VolSlot> volSlots_;
    std::vector<MeasuredEdit> measured_;
    // Bumped by load(): measurements from the pre-load volume complete
    // against a state that no longer exists, so their callbacks drop them.
    uint32_t snapGen_ = 0;

    // Shared with in-flight MapAsync callbacks; false once this object is
    // gone. Heap-owned, so the flag outlives `this` for any late callback.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
