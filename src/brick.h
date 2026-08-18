#pragma once
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "asset.h"
#include "gpu.h"

class SnapWriter;
class SnapReader;

class BrickSystem;

// The three big GPU buffers, shared by EVERY fighter — this is PLAN.md's
// "fighters are SLICES, not systems", and the reason it exists is CLAUDE.md
// trap 8: core WebGPU guarantees a shader stage only 8 storage buffers, and at
// one set of three per fighter the tracer ran out at two (7 of 8, ground
// included). Sliced, N fighters cost the same 3 bindings as one, and trace
// sits at 4 of 8 no matter how many fight.
//
// Each BrickSystem owns SLICE `slot` of all three. It binds its own slice as a
// bind-group sub-range for every write pass, so edit/voxelize/jfa/redistance
// index from zero and never learn that they are one of several — only the
// tracer, which binds the whole buffers, adds a base index.
//
// What is NOT here: the allocator counters, freelist, dirty list and JFA
// scratch. Those stay per-fighter because nothing in the trace pass binds
// them, so they cost no bindings, and keeping them separate keeps each
// fighter's allocation independent (~1.3 MB each).
struct BrickStore {
    wgpu::Buffer volume;     // kMaxFighters x kVolumeBytes
    wgpu::Buffer distPool;   // kMaxFighters x kMaxBricks x 256 x 4
    wgpu::Buffer albedoPool; // kMaxFighters x kMaxBricks x 512 x 4
    bool init(Gpu& gpu);
};

// R19: capsules ONE edit's brush may union together.
//
// This — not BrickSystem::kOpsPerFrame — is what bounds how far a weapon may
// travel in a frame, which is the whole point of the change: kOpsPerFrame is a
// queue-drain budget and it had no business setting how fast a sword swings.
// 16 capsules at the sword's ~0.013 m substep spacing covers ~0.21 m of tip
// travel per frame (~12.7 m/s), against 0.079 m (~4.7 m/s) at the old cap of 6.
//
// It costs uniform bytes and nothing else: EditParams grows by
// (kMaxBrushCaps-1) * 32 B, and the shader loop runs `capCount` iterations, not
// this many.
inline constexpr int kMaxBrushCaps = 16;

// Sparse brickmap for one carveable character body: a slice of a BrickStore
// (indirection grid + brick pool partition) plus this fighter's own freelist
// and the bake/edit/JFA passes. Up to kOpsPerFrame edits are consumed per
// frame (uniform buffer contents are per-submit).
struct BrickEdit {
    // 0 bake, 1 carve, 2 add, 3 dent (volume-conserving displacement),
    // 4 paint (albedo only). See edit.wgsl for what each one does to the field;
    // what matters up here is that 3 and 4 are the two CHEAP modes — 3 never
    // allocates a brick and 4 skips classify, the JFA and the redistance pass
    // outright.
    int mode = 1;
    // WHICH fighter's volume this cuts. Explicit, and defaulting to the hero,
    // because `pos` below is REST space: the same coordinates name different
    // clay in every fighter's slice, so an edit without an owner is ambiguous
    // the moment there is more than one body. Routing this off the live pick
    // instead would have quietly redirected every SCRIPTED edit (ctl `edit`,
    // --carve-test, replay journals — all authored against player 0) to
    // whichever fighter happened to be under the cursor.
    int player = 0;
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
    // R19: the brush is the UNION of `capCount` capsules, all sharing `radius`.
    // pos/posB are capsule 0; these are 1..capCount-1. capCount == 1 leaves the
    // brush bit-identical to the single-capsule one, which is what every call
    // site but the sword means.
    //
    // This exists because a swing used to emit one EDIT per substep and
    // kOpsPerFrame (a queue-drain budget) therefore capped how fast a sword was
    // allowed to move. Six sequential soft-carves also scalloped along their
    // union, because each one smin'd against a field the previous one had
    // already moved. A min over the capsule array is the true swept union,
    // evaluated once, and it is measured by ONE readback instead of six.
    //
    // For mode 3 the pair pos/posB means something else entirely — see
    // `dentAmp` — and capCount is ignored.
    int capCount = 1;
    float capA[kMaxBrushCaps - 1][3] = {};
    float capB[kMaxBrushCaps - 1][3] = {};
    // Mode 3 only: metres of inward displacement at the dent's core. The
    // profile is authored to cancel (edit.wgsl dentDelta), so this scales a
    // shape whose net volume change is ~0 rather than an amount of clay.
    float dentAmp = 0.f;
    // Mode 4 only: 0..1 opacity of the albedo stamp at the brush core.
    float paint = 1.f;
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
    // Cut by a WEAPON — a swinging blade or a travelling fist — rather than by
    // the brush. Rides the measurement back so the ledger can pool a whole
    // strike into ONE gob instead of dribbling it
    // (Renderer::updateConservation). `segment` is NOT a stand-in for this —
    // that is the capsule brush shape, which the UI may want one day.
    // (Named `fromBlade` until the fist arrived; the pooling was never
    // blade-specific, only the caller was.)
    bool fromWeapon = false;
};

// One edit op's measured |volume change| (carve: removed, add: created),
// delivered a frame or two after the op ran.
struct MeasuredEdit {
    BrickEdit edit;
    float volume = 0.f; // m^3
};

class BrickSystem {
  public:
    // ---- how many fighters share one store ----
    // A MEMORY choice, not a binding one: the tracer costs the same 3 bindings
    // at any N (see BrickStore), so this only multiplies the pool budget below
    // and the uniform block's fighter array.
    //
    // Held at 2 deliberately. The slices are allocated up front whether or not
    // a fighter is live, so 4 costs 157 MB of GPU buffers against 104 MB for
    // 2 — fine on desktop, not obviously fine in a phone browser, and the web
    // target ships. Going back to 4 is this line PLUS dropping kMaxBricks to
    // 12288 (see its table); the static_assert below is the backstop if you
    // forget, and it is deliberately strict rather than exact-at-the-limit.
    static constexpr int kMaxFighters = 2;
    // Articulated regions per fighter: the brush rig is body + two mitts
    // (trap 7). This sizes the `pieces` array inside each Fighter in the
    // uniform block, so it is emitted into the shaders rather than
    // hand-mirrored. The dead 13-piece inverse-LBS path wanted 16; a rigged
    // asset would have to raise this (and pay 12 uniform slots per piece per
    // fighter) before it could pose more than three.
    // FOUR is a CAPACITY, not a live count. The brush rig is three (body +
    // two mitts) and a standing fighter still reports three, so the march loop
    // — `for i < gPieceCount`, the hottest loop in the frame — does exactly the
    // work it always did. The fourth slot exists for the moment a body is cut
    // in half: the body brush becomes two pieces with the same volume clipped
    // to opposite sides of the cut, each carrying its own rigid fall. Raising
    // this costs kPieceSlots uniform slots per fighter and nothing per step.
    static constexpr int kPiecesPerFighter = 4;
    // What the brush rig itself uses. Kept separate so `pieces` is sized by
    // the rig rather than by the capacity — sizing it by the capacity would
    // hand the tracer a fourth piece to march through on every standing body.
    static constexpr int kBrushPieces = 3;

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
    // ---- brick pool capacity: SIZED FROM THE GEOMETRY, not from taste ----
    // This is the single biggest number in the app's memory budget: it sizes
    // distPool (kMaxBricks * 256 * 4) and albedoPool (kMaxBricks * 512 * 4)
    // PER FIGHTER, i.e. 3 MiB per 1k bricks per fighter, doubled by the foe.
    // It was 49152 — a guess inherited from M0, never re-derived after kGrid
    // fell from 74 to 50 — which is why the pools were ~7x oversized.
    //
    // A brick exists only for a cell whose CENTRE is within 2.1 SPAN of the
    // surface (voxelize.wgsl meshClassify / edit.wgsl classify). So the
    // allocated set is a shell ~4.2 cells thick wrapped around the skin, and
    // the count is surface-area driven: ~4.2 * A / SPAN^2.
    //
    // Measured against assets/fighter.glb at kGrid=50 by replaying the
    // classify rule on the CPU (exact point-triangle distance to every cell
    // centre, all three brushes: body + hand.rest + hand.grab):
    //
    //   mesh surface area          1.2601 m^2
    //   SPAN^2                     8.0248e-4 m^2
    //   analytic 4.2*A/SPAN^2      6,595 bricks
    //   ACTUAL cells within 2.1 SPAN   6,748 bricks   <- the import floor
    //   solid (inside) cells           3,978
    //   alloc UNION inside             8,363 bricks   <- see below
    //
    // 8,363 is a HARD ceiling for carving. A carve only ever moves the surface
    // into clay that is already there, so every cell it newly allocates was
    // either in the band already or was solid interior; in the limit where the
    // fighter is minced to swiss cheese at cell scale, the allocated set is
    // exactly that union. Carving alone therefore CANNOT overflow the pool.
    //
    // Adds are the open-ended direction: they create surface in air, so they
    // are bounded only by the volume box. The worst case on record is
    // CLAYFRAY_TEST_ADDSTRESS (60 adds of r=0.04 marching along an arc): each
    // deposit allocates a ball of radius 0.04 + 2.1*SPAN ~ 0.0995 m, the arc
    // steps ~0.057 m, so it sweeps ~pi*0.0995^2*0.057 / SPAN^3 ~ 78 new cells
    // per step, ~4.7k total, landing near ~11.5k with the import floor.
    //
    // This is PER FIGHTER, and the pools are PARTITIONED, not pooled: fighter
    // f owns bricks [0, kMaxBricks) of its own slice, its indirection words
    // store indices LOCAL to that partition, and the tracer adds the base.
    // A shared free-list across fighters would use the memory better (an
    // untouched opponent would lend its unused bricks to a minced one), but it
    // would also let one fighter's carving starve another's, and it would
    // break save()'s "dense prefix up to the high-water mark" — partitioning
    // keeps each fighter's clay genuinely private, which is what PLAN.md
    // promised. Overflow is graceful (finishCapacityPoll and the spill counter
    // in brick.cpp), never corrupting.
    //
    // THIS NUMBER IS PAIRED WITH kMaxFighters. The tracer binds each pool
    // WHOLE, so the albedo binding is kMaxBricks * 2 KiB * kMaxFighters, and
    // core WebGPU only guarantees maxStorageBufferBindingSize = 128 MiB:
    //
    //           per fighter   x2 fighters   x4 fighters
    //   16384    48 MiB        96 MiB        192 MiB   <- albedo lands on
    //                                                     EXACTLY 128 MiB:
    //                                                     legal, but zero
    //                                                     margin on a
    //                                                     guaranteed minimum
    //   12288    36 MiB        72 MiB        144 MiB   <- albedo 96 MiB
    //
    // At kMaxFighters = 2 shrinking this buys nothing, so it is back at the
    // value derived for the one-volume-per-fighter design: 2.4x the import
    // floor, 2.0x the hard carve ceiling (which carving CANNOT exceed, see
    // above), and 1.4x the worst case on record (CLAYFRAY_TEST_ADDSTRESS,
    // ~11.5k). At 12288 that last margin was 7% — measured 10273/12288, i.e.
    // 83.6% of pool, which is a lot closer to spilling than it sounds.
    //
    // RE-MEASURE THIS if kGrid changes, if the artist re-exports a
    // significantly larger fighter, or before trusting it on a long play
    // session: bricks scale with kGrid^2, so kGrid=74 would want ~4x this.
    //   CLAYFRAY_DEBUG_STATS=1 ./build/clayfray --carve-test ...   -> allocTop
    //   CLAYFRAY_TEST_ADDSTRESS=1 with the same flags               -> the peak
    static constexpr uint32_t kMaxBricks = 16384;
    static constexpr uint64_t kDistBytes = (uint64_t)kMaxBricks * 256 * 4;
    static constexpr uint64_t kAlbedoBytes = (uint64_t)kMaxBricks * 512 * 4;
    // Blowing this fails to create the trace bind group on a conformant mobile
    // device while working perfectly on desktop — the same silent-black-screen
    // shape as trap 8's binding count. STRICTLY less than the limit, not equal:
    // 16384 x 4 fighters lands on 128 MiB to the byte, so an exact-fit assert
    // would wave through the one combination most likely to bite.
    static_assert(kAlbedoBytes * kMaxFighters < 134217728ull,
                  "albedo pool needs the whole 128 MiB core storage-binding "
                  "limit or more - drop kMaxBricks (see its table) or "
                  "kMaxFighters");
    // The brick index is stored in the low 20 bits of an indirection word
    // (IND_IDX_MASK = 0x000FFFFF), so the encoding caps out at 1,048,575 —
    // shrinking the pool only uses less of that range. There is also a
    // structural ceiling of kCellCount (one brick per cell, at most), which
    // no value above 125,000 could ever reach at kGrid=50.
    static_assert(kMaxBricks <= 0x000FFFFF, "brick index must fit IND_IDX_MASK");
    static constexpr uint32_t kDirtyCap = 65535; // indirect dispatch x-dim limit
    // Watertight-parity lattice: one bit per voxel along x, packed into rows.
    static constexpr uint32_t kAxisVox = kGrid * kBrickUsable + 1;
    static constexpr uint32_t kRowWords = (kAxisVox + 31) / 32;

    // ---- the three per-cell arrays share ONE buffer ----
    // Core WebGPU guarantees a shader stage only 8 storage buffers (and Metal
    // gives 10: 31 Metal buffer slots, less the one Dawn spends on buffer
    // lengths and its default uniform/vertex budget). The tracer samples every
    // fighter plus the ground, and at one binding per array TWO fighters came
    // to 14 — the trace pipeline failed to create at all and the app drew
    // nothing on macOS. (Vulkan's limit is effectively unbounded, which is why
    // the Windows box never saw it.)
    //
    // So indirection, JFA seeds and the coarse field live at fixed offsets
    // inside `volume`. A pass that WRITES one binds just its own region as a
    // sub-range, so the write shaders are untouched; the tracer binds the whole
    // buffer once and adds a base index on the read (the CELL_* constants in
    // wgslConstants(), used by brick_read.wgsl).
    //
    // The SLICE offsets stack on top of these: a region of fighter f starts at
    // `f * kVolumeBytes + kIndOff`. Same mechanism, one more term — which is
    // exactly why the slice refactor was cheap on the write side.
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

    // NON-COPYABLE for the same reason Renderer is: a copy SHARES `alive_`,
    // and the temporary's destructor kills every readback the original is
    // waiting on. This one is the likelier accident of the two — these live in
    // `std::array<Fighter, kMaxPlayers>` inside Renderer, so any `auto f =
    // fighters_[0];` or by-value parameter does it, and raising kMaxFighters
    // is exactly the kind of refactor that writes one.
    BrickSystem(const BrickSystem&) = delete;
    BrickSystem& operator=(const BrickSystem&) = delete;
    BrickSystem() = default;

    // `slot` is which slice of the shared store this fighter owns. Everything
    // this object encodes is confined to that slice.
    bool init(Gpu& gpu, BrickStore& store, int slot, const std::string& editSrc,
              const std::string& jfaSrc, const std::string& redistSrc,
              const std::string& voxelizeSrc);
    int slot() const { return slot_; }
    // Byte offsets of this fighter's slice inside each shared buffer. Every
    // bind-group sub-range, ClearBuffer, CopyBufferToBuffer and WriteBuffer in
    // brick.cpp goes through these — miss one and a fighter silently edits
    // its neighbour's clay.
    uint64_t volBase() const { return (uint64_t)slot_ * kVolumeBytes; }
    uint64_t distBase() const { return (uint64_t)slot_ * kDistBytes; }
    uint64_t albBase() const { return (uint64_t)slot_ * kAlbedoBytes; }

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
    // Cheap and idempotent: stores the colour, and refreshes the voxelizer's
    // uniform if one exists yet. Call it BEFORE requestImport for it to reach
    // the imported skin.
    void setBodyColor(const float c[3]);
    // The colour of THIS fighter's clay. queueEdit stamps it onto the copy it
    // keeps, so a caller holding its own BrickEdit still sees the struct's
    // fallback — read it from here instead of from an edit you just queued.
    const float* bodyColor() const { return bodyColor_; }
    // Drops edits that would cross the volume boundary (a clipped blob
    // renders as corruption).
    void queueEdit(const BrickEdit& e);
    // Same clip test queueEdit applies, without queueing: lets gob landings
    // pick "stick" vs "deflect" before committing. Every capsule of a fused
    // brush has to clear the boundary, so a caller building one is better off
    // filtering with capsuleInBounds as it goes — dropping a whole swing
    // because its last substep strayed would lose the part that was fine.
    bool editInBounds(const BrickEdit& e) const;
    bool capsuleInBounds(const float a[3], const float b[3], float radius) const;
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

    // Handles to the SHARED store (wgpu::Buffer is a refcounted handle, so
    // these name the same GPU allocations every fighter uses). THIS fighter's
    // data starts at volBase()/distBase()/albBase().
    //
    // volume holds the per-cell arrays at the kIndOff/kSeedOff/kCoarseOff
    // offsets above:
    //   ind    — indirection grid
    //   seed   — canonical JFA output (jfaA), read by tracer/pick
    //   coarse — per-cell signed coarse distance, read by tracer
    wgpu::Buffer volume;
    // Sized by kMaxBricks: 12 MiB + 24 MiB per fighter. See the capacity
    // derivation on kMaxBricks — these two are the app's memory budget.
    wgpu::Buffer distPool, albedoPool;

    // Blocking readbacks; debug only.
    void debugStats(const char* label);
    void debugScanField();

    // Async pool watermark check; call after queue submit. Reports both the
    // high-water mark and any allocations the shaders had to DROP.
    void finishCapacityPoll();

    // The allocator's shared counter block, mirrored in edit.wgsl /
    // voxelize.wgsl / redistance.wgsl as `counters: array<atomic<u32>>`:
    //   [0] allocTop  bump pointer (also the all-time high-water mark)
    //   [1] freeTop   entries currently on the freelist
    //   [2] dirty     redistance work-list length
    //   [3] volFp     |occupancy delta| of the op being measured, fixed point
    //   [4] spill     allocations REFUSED because the pool was full
    // Slot 4 exists so a full pool cannot fail silently: the shaders drop the
    // cell (keeping its previous indirection word) and count the drop, and
    // finishCapacityPoll turns that into a warning. Sized to 32 B rather than
    // 20 so the block stays 16-byte aligned and has room for the next counter.
    static constexpr uint64_t kCounterBytes = 32;

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
    // Ops drained per frame, sharing one JFA/redistance at the end of the
    // frame. A QUEUE-DRAIN BUDGET and nothing more.
    //
    // It used to be more than that: a sword sweep was substepped into one edit
    // per capsule, so this number silently capped how fast a blade could move
    // before its wound came out as disconnected slices (R19). The sweep is one
    // multi-capsule op now — kMaxBrushCaps is that bound — and what drains
    // through here is genuinely independent ops: gob deposits, UI/ctl carves,
    // and the two or three a strike emits (dent + carve + bruise).
    static constexpr int kOpsPerFrame = 6;

  private:
    void encodeOp(wgpu::CommandEncoder& enc, const BrickEdit& e, int slot);
    void encodeJfa(wgpu::CommandEncoder& enc);

    Gpu* gpu_ = nullptr;
    int slot_ = 0; // which slice of the shared store this fighter owns
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
    // This fighter's clay colour, linear rgb. Read by BOTH write paths — the
    // voxelizer through voxColor_ at import, and every edit through
    // EditParams::bodyColor — which is what makes a wound the same colour as
    // the body around it. Defaults to the old hardcoded cyan so a caller that
    // never sets one still gets clay rather than black.
    float bodyColor_[3] = {0.024f, 0.19f, 0.25f};
    wgpu::Buffer voxColor_; // 16 B uniform; see trap 8 in voxelize.wgsl
    // Per-fighter scratch: the voxel parity lattice is written by this
    // volume's own import pass, so it is NOT shared.
    wgpu::Buffer vxParityBuf_;
    bool importPending_ = false;
    void armCapacityPoll(wgpu::CommandEncoder& enc);
    wgpu::Buffer capRead_;
    int editsSinceCap_ = 0;
    bool capPollArmed_ = false, capMapPending_ = false, capWarned_ = false;
    // Spill count already reported. A full pool keeps dropping cells every
    // frame, so the warning re-fires only when the count GROWS — one line per
    // poll at worst, never a per-frame spam.
    uint32_t capSpillSeen_ = 0;

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
