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
    // REST space — the volume's own frame. A world position only addresses
    // the volume correctly while the fighter stands unposed at the origin.
    float pos[3] = {0, 0, 0};
    float radius = 0.05f;
    // Segment brush: the edit is a CAPSULE from pos to posB. `segment` off
    // (the default) leaves it a sphere, which is what every pre-M5 call site
    // means. A sword cut is a segment — a sphere at the blade's deepest point
    // hollows the interior without ever breaking the surface.
    bool segment = false;
    float posB[3] = {0, 0, 0};
    float color[3] = {0.8f, 0.5f, 0.5f};
    // Conservation metadata (rides along; the GPU op ignores it). outDir and
    // srcColor snapshot the pick normal/albedo at queue time so gobs fly
    // outward from the wound wearing its color. worldPos is where that wound
    // actually is in the arena — gobs spawn there, and it is NOT `pos` once
    // the fighter has walked away from the origin.
    float worldPos[3] = {0, 0, 0};
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
    // ---- volume geometry: THE one place these are authored ----
    // kGrid is the only free knob; everything below derives from it, and
    // wgslConstants() emits the whole block into the shaders. These used to be
    // hand-copied into six .wgsl files with two silently grid-derived
    // companions (ROW_WORDS, rowWords) — don't reintroduce a second copy.
    //
    // Cost model: bricks allocated track SURFACE area, ~(1/kVoxel)^2, so they
    // fall off fast as kGrid drops. The per-cell passes (JFA, redistance
    // compact, the indirection/coarse/seed buffers) are kGrid^3.
    static constexpr int kGrid = 50;
    static constexpr int kBrickUsable = 7; // unique voxels per brick per axis
    // Volume extent in metres. FIXED across resolutions: the character is
    // voxelized into this box and VOL_ORIGIN centres it, so changing kGrid
    // must not move the box (it would invalidate every authored position).
    static constexpr float kExtent = 1.4164044f;
    static constexpr float kSpan = kExtent / kGrid;         // cell size, m
    static constexpr float kVoxel = kSpan / kBrickUsable;   // voxel size, m
    // Narrow-band half-width in VOXELS. ~= a cell diagonal (7*sqrt(3) = 12.1):
    // a cell within one diagonal of the surface must be allocated, so this is
    // structural, not a smoothing knob — lowering it drops needed bricks.
    static constexpr float kBand = 12.f;
    // Volume is centred in x/z; y sits the feet on the ground plane.
    //
    // The half-voxel nudge is LOAD-BEARING, not cosmetic. A plain -kExtent/2
    // puts world x=z=0 exactly kGrid/2 cells from the origin — i.e. exactly on
    // a cell boundary whenever kGrid is even. The fighter is centred on that
    // axis, and `floor()` on an exact boundary drops into the neighbouring
    // cell, which may be unallocated: the march then reads empty space,
    // overshoots, and the render streaks down the character and the props.
    // Offsetting half a voxel keeps the world axes off every cell plane.
    // (The old hand-written -0.7082 satisfied this by luck at 24.99991 cells;
    // deriving it "cleanly" is what broke it. Verified: without the nudge,
    // kGrid=50 streaks; with it, the render matches the reference.)
    static constexpr float kOriginNudge = kVoxel * 0.5f;
    static constexpr float kOrigin[3] = {-kExtent / 2 + kOriginNudge, -0.1582f,
                                         -kExtent / 2 + kOriginNudge};
    static constexpr uint32_t kMaxBricks = 49152;
    static constexpr uint32_t kDirtyCap = 65535; // indirect dispatch x-dim limit
    // Watertight-parity lattice: one bit per voxel along x, packed into rows.
    static constexpr uint32_t kAxisVox = kGrid * kBrickUsable + 1;
    static constexpr uint32_t kRowWords = (kAxisVox + 31) / 32;

    // ---- the four per-cell arrays share ONE buffer ----
    // A Metal shader stage gets 10 storage buffers: 31 Metal buffer slots,
    // less the one Dawn reserves for buffer lengths and its default uniform
    // and vertex-buffer budget. The tracer samples TWO fighters plus the
    // ground, and at one binding per array that came to 14 — the trace
    // pipeline failed to create at all and the app drew nothing on macOS.
    // (Vulkan's limit is effectively unbounded, which is why M5's second
    // fighter looked fine on the Windows box.)
    //
    // So indirection, JFA seeds and the coarse field live at fixed offsets
    // inside `volume`. A pass that WRITES one binds just its own region as a
    // sub-range, so the write shaders are untouched; the tracer binds the whole
    // buffer once and adds a base index on the read (the CELL_* constants in
    // wgslConstants(), used by brick_read.wgsl).
    //
    // Every write-side binding of these regions is read_write ON PURPOSE:
    // Dawn rejects a buffer that is writable and read-only within one pass,
    // and it tracks that per BUFFER, not per bound range — so a `read`
    // binding on any region would break every pass that writes another.
    //
    // M-RIG: there used to be a FOURTH region here, `cellW` — a per-cell
    // 4-slot bone-weight field (kCellCount * 8 bytes) that existed solely to
    // steer the inverse-LBS chunk warp and, later, the affine rig's
    // dominant-bone ownership test. The asset has no armature and the brush
    // rig separates pieces by disjoint rest-space AABBs, so nothing reads a
    // bone weight anywhere in the pipeline any more. Dropping the region is
    // 1 MB of the volume buffer per fighter and, more usefully, deletes the
    // volume-wide flood fill from import.
    static constexpr uint64_t kCellCount = (uint64_t)kGrid * kGrid * kGrid;
    static constexpr uint64_t kCellBytes = kCellCount * 4;
    // Region starts must satisfy minStorageBufferOffsetAlignment (256).
    static constexpr uint64_t kAlign = 256;
    static constexpr uint64_t kRegionStride = (kCellBytes + kAlign - 1) & ~(kAlign - 1);
    static constexpr uint64_t kIndOff = 0;                     // indirection
    static constexpr uint64_t kSeedOff = kRegionStride;        // JFA seeds (jfaA)
    static constexpr uint64_t kCoarseOff = kRegionStride * 2;  // coarse distance
    static constexpr uint64_t kVolumeBytes =
        kCoarseOff + ((kCellBytes + kAlign - 1) & ~(kAlign - 1));

    // ---- piece thresholds: RESOLUTION-RELATIVE, never metres ----
    // These were authored as fixed distances tuned at kGrid=50. A fixed
    // distance silently TIGHTENS as kGrid falls, because the errors they gate
    // on are all set by cell/voxel size — which is how dropping to kGrid=37
    // opened holes in the character's sides. The multipliers below reproduce
    // the tuned values exactly at kGrid=50 and track the grid from there.
    //
    // M-RIG removed a third, kWarpResidSpans: the inverse-LBS round-trip
    // rejection tolerance that forwardResid() compared against. Both it and
    // forwardResid went with the skeleton.
    //
    // Joint blend width, in VOXELS: bridges hairline cracks where two pieces
    // hand off, without re-introducing ring bulges at the joint.
    static constexpr float kJointSminVoxels = 1.977f;     // 0.008 m at kGrid=50
    // Box test margin, in CELL SPANS: within it a piece is sampled, outside it
    // contributes a conservative bound instead. A perf knob, not a
    // correctness one — the bound path is safe, just slower to march.
    static constexpr float kBoxMarginSpans = 2.118f;      // 0.06 m at kGrid=50

    // The block above as WGSL. Stitched into every shader root that samples
    // the brickmap by the renderer's `//#constants` directive, so the GPU
    // side cannot drift from the CPU side.
    static std::string wgslConstants();

    // Flip the alive sentinel so any MapAsync callback still queued at
    // teardown bails before it touches destroyed buffers/members.
    ~BrickSystem() { *alive_ = false; }

    bool init(Gpu& gpu, const std::string& editSrc, const std::string& jfaSrc,
              const std::string& redistSrc, const std::string& voxelizeSrc);

    // Replaces the analytic bake with a voxelized mesh. CPU side (binning,
    // watertight parity) happens here; GPU passes run on the next encode.
    void requestImport(CharacterAsset asset);

    // ---- shared mesh preprocessing ----
    // Everything requestImport() derives from the asset — triangle bins,
    // watertight parity, smooth normals — is a pure function of the
    // CharacterAsset, and so are the read-only GPU buffers it uploads. Two
    // fighters of the same character were computing byte-identical results
    // twice: ~2.5 s of a 7.8 s startup, plus a duplicate ~32 MB upload.
    // Only the OUTPUT volume (indirection, seeds, coarse, pools) is
    // per-fighter.
    //
    // Call prepareImport() once per character and hand the result to every
    // fighter. Refcounted so the buffers outlive whichever BrickSystem dies
    // first.
    struct MeshImport {
        wgpu::Buffer pos;    // interleaved position + smooth normal
        wgpu::Buffer idx, col;
        wgpu::Buffer tris;   // merged [cell offsets][triangle ids]
        wgpu::Buffer inside; // per-cell watertight parity bits
        uint32_t triCount = 0;
    };
    static std::shared_ptr<MeshImport> prepareImport(Gpu& gpu,
                                                     const CharacterAsset& asset);
    void requestImport(std::shared_ptr<MeshImport> mesh);
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
        if (mesh_) importPending_ = true;
        else bakePending_ = true;
    }
    bool hasPendingWork() const { return bakePending_ || !pending_.empty(); }
    // Bumped whenever encode() emits work that changes the volume (import,
    // bake, or an edit). The volume's CONTENTS are not in the uniform buffer,
    // so this is what tells the renderer's frame reuse that a carve happened.
    uint32_t generation() const { return gen_; }
    // Encodes at most one op (bake or oldest queued edit) plus the JFA
    // refresh. Call once per frame before the trace pass.
    void encode(wgpu::CommandEncoder& enc);

    // The per-cell arrays, packed at the kIndOff/kSeedOff/kCoarseOff offsets
    // above:
    //   ind    — indirection grid
    //   seed   — canonical JFA output (jfaA), read by tracer/pick
    //   coarse — per-cell signed coarse distance, read by tracer
    wgpu::Buffer volume;
    // NOTE (mobile memory): these two are sized by kMaxBricks (49152) but
    // measured usage is ~12.4k bricks, so both are roughly 4x oversized. Left
    // alone here on purpose — shrinking kMaxBricks has capacity-overflow
    // consequences and is its own change.
    wgpu::Buffer distPool, albedoPool;

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

  public:
    // Ops drained per frame. A sword sweep is substepped into several capsule
    // carves, and at one op per frame the wound would unzip over half a second
    // after the swing. They share one JFA/redistance at the end of the frame,
    // so the extra cost is the carve dispatches, not the refresh.
    static constexpr int kOpsPerFrame = 6;

  private:
    void encodeOp(wgpu::CommandEncoder& enc, const BrickEdit& e, int slot);
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
    // jfaB_ is the JFA's ping-pong scratch; the canonical side (jfaA) is the
    // seed region of `volume`, since the tracer reads it.
    wgpu::Buffer editParams_[kOpsPerFrame], counters_, freelist_, jfaB_;
    wgpu::Buffer jfaStepParams_[kJfaSteps], jfaResolveParams_;
    wgpu::BindGroup classifyG0_[kOpsPerFrame], classifyG1_, classifyG2_;
    wgpu::BindGroup fillG0_[kOpsPerFrame], fillG1_, fillG2_;
    wgpu::BindGroup jfaInitG_, jfaStepG_[kJfaSteps], jfaResolveG_;
    std::vector<BrickEdit> pending_;
    bool bakePending_ = true;
    uint32_t gen_ = 0;

    void encodeImport(wgpu::CommandEncoder& enc);
    wgpu::ShaderModule voxelizeMod_;
    wgpu::ComputePipeline vxClassify_, vxFill_, vxParity_;
    wgpu::BindGroup vxG0_, vxG1_, vxG2_;
    // Mesh inputs, shared with every other fighter of the same character.
    std::shared_ptr<MeshImport> mesh_;
    // Per-fighter scratch: the voxel parity lattice is written by this
    // volume's own import pass, so it is NOT shared.
    wgpu::Buffer vxParityBuf_;
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
