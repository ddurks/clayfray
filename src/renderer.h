#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "anim.h"
#include "brick.h"
#include "camera.h"
#include "gpu.h"
#include "ground.h"
#include "params.h"

// Frame pipeline: brick edits (if any) -> trace (compute sphere tracer ->
// HDR) -> post (film look -> rgba8 presentTex) -> blit (swapchain + ImGui).
// Screenshot mode reads presentTex back. Shaders hot-reload from source,
// stitched together by a //#include preprocessor (WGSL has none).
class Renderer {
  public:
    // Flip the alive sentinel so any MapAsync callback (pick, GPU timestamp)
    // still queued at teardown bails before touching destroyed buffers.
    ~Renderer() { *alive_ = false; }

    bool init(Gpu& gpu, int width, int height);
    void resize(int width, int height);
    bool reloadShadersIfChanged();
    void render(const OrbitCamera& cam, const LookParams& look, const FrameInfo& frame,
                wgpu::TextureView swapchainView,
                const std::function<void(wgpu::RenderPassEncoder&)>& uiCallback);
    bool screenshot(const std::string& path);

    // sculpting
    void setPickUV(float u, float v) { pickU_ = u; pickV_ = v; }
    bool pickValid() const { return pickValid_; }
    const float* pickPos() const { return pickPos_; }
    // Where the picked surface lives in the brick volume's REST space. Sculpt
    // edits MUST use this, not pickPos(): the volume is authored in rest
    // space, so a world position only addresses it correctly while the
    // fighter stands unposed at the origin.
    const float* pickRest() const { return pickRest_; }
    const float* pickNormal() const { return pickNormal_; }
    float pickMat() const { return pickMat_; }
    // headless serve mode runs the pick pass too so ctl `probe` works
    void setAlwaysPick(bool v) { alwaysPick_ = v; }
    // snapshots the pick normal/albedo into the edit so conservation gobs
    // launch outward from the wound wearing its color; returns the resolved
    // edit so the ctl journal can record it pick-independently
    BrickEdit queueBrickEdit(BrickEdit e);
    BrickSystem& brick() { return brick_; }
    void setCharacter(CharacterAsset asset);
    const SplootStats& sploot() const { return sploot_; }

    // Sim-state snapshots (M-DEV): body volume + ground field + ledger +
    // gobs + sim time. Save drains in-flight volume measurements first so
    // the ledger in the file is settled. Load cancels queued bake/import.
    bool saveSnapshot(const std::string& path, double simT,
                      const std::string& charPath);
    bool loadSnapshot(const std::string& path, double* simT,
                      const std::string& charPath);
    // Block until queued volume measurements have landed in measured_ —
    // pins their arrival frame so journal replay is deterministic.
    void syncMeasurements();
    // Fold any completed measurements into the ledger without rendering.
    // The serve loop calls this while paused/idle so ctl `stats` doesn't
    // report a stale ledger (absorption otherwise only runs in render()).
    void pumpLedger() {
        brick_.pollVolumes();
        foe_.pollVolumes();
        absorbMeasured();
    }

    int width() const { return width_; }
    int height() const { return height_; }

    // ---- 12 Hz frame reuse ----
    // The pose grid is the stop-motion clock (CLAUDE.md trap 4), so with a
    // still camera four out of every five 60 Hz frames are BIT-IDENTICAL to
    // the one before — verified by rendering consecutive frames and diffing.
    // Re-tracing them is pure waste. The trace result is kept and reused until
    // something the tracer reads actually changes; POST still runs every frame,
    // so the 25 Hz film grain and the bloom keep animating over the cached
    // trace for free.
    uint64_t framesTraced() const { return framesTraced_; }
    uint64_t framesPresented() const { return framesPresented_; }
    // Force a re-trace (resize, shader reload, anything not in the digest).
    void invalidateTrace() { traceValid_ = false; }

    // smoothed GPU pass times, ms (0 when timestamps unsupported)
    float traceMs() const { return traceMs_; }
    float postMs() const { return postMs_; }

  private:
    bool buildPipelines();
    void buildTargets();
    void buildBindGroups();
    // 13 look + mouse + 8 marbles x2 + marbleMeta + capsMeta + capsCenter +
    // 16 capsules x2 + boneMeta + 16 pieces x12 (invSkin mat4, forward skin
    // mat4, aabb lo/hi, rest capsule a/b) + gobMeta + 12 gobs x2 + groundMeta
    // + swordA/swordB/swordCol.
    // MUST match the Uniforms struct in trace.wgsl AND pick.wgsl — and a
    // mismatch only bites after a rebuild (shaders hot-load, binaries don't).
    // 287..292 foeInv/foeMeta/foeCenter, 293..484 foePieces, 485 foeBoneMeta
    static constexpr int kUniformSlots = 488;
    void packUniforms(const OrbitCamera& cam, const LookParams& look,
                      const FrameInfo& frame, float out[kUniformSlots][4]) const;
    std::vector<MarbleProp> marbles_;
    // P0 animation: skeleton + clips kept CPU-side; pose sampled at the 12 Hz
    // quantized clock; marbles and the capsule shadow proxy ride the skin
    // matrices. (The voxel body stays in rest pose until M4-P1.)
    std::vector<AssetBone> bones_;
    std::vector<AnimClip> clips_;
    std::vector<BoneCapsule> capsules_;
    std::vector<float> skinMats_;
    float animT_ = 0.f;
    // M4.7: floating-hand IK chains (built at setCharacter from bone names),
    // the blob's centre-of-mass decomposition that tethers them, and the
    // computed sword geometry for a frame (hilt/tip/two grips, world).
    std::vector<HandIkChain> handIk_;
    BodyCom bodyCom_;
    // stands off to the front, turned to face the hero
    FighterPose foePose_{{1.15f, 0.f, 0.25f}, 3.14159f, 0.f, false};
    bool foeEnabled_ = false; // no opponent until addPlayer() makes one
    int playerCount_ = 1;     // the hero always exists
    std::vector<BoneCapsule> foeCaps_;  // rest capsules, for blade hit tests
    float foeBoundR_ = 0.6f;            // rest bound radius about foe origin
    float foeCenterRest_[3] = {0.f, 0.35f, 0.f};
    float autoReach_ = 0.f; // rest COM->wrist distance; hands.reach 0 uses it
    // M4.8 gaze: eye bones, and the camera position LATCHED at the last pose
    // step. Sampling the live camera would slide the eyes at frame rate and
    // break the 12 Hz stop-motion the rest of the character obeys.
    // player 1 runs its own pose clock and its own posed skeleton, so it can
    // play idle while the hero does something else entirely
    float foeAnimT_ = 0.f;
    std::vector<float> foeSkinMats_;
    std::vector<GazeChain> gaze_;
    float gazeTarget_[3] = {0.f, 0.6f, 3.f};
    // pose step the gaze target was last latched on; separate from
    // lastPoseTime_, which updateConservation advances at a different point
    float gazePoseTime_ = -1.f;
    // ---- display poses (trap 4 applied to the ROOT) ----
    // The animation clip was already quantised to the pose grid, but the root
    // translation/yaw/lean was not: the fighter SLID at 60 Hz under a body
    // that stepped at 12. These latch the sim pose at each pose step and are
    // what the renderer actually draws. Two consequences, both wanted: the
    // walk reads as stop-motion instead of gliding, and because the drawn
    // pose stops changing between pose steps, walking no longer invalidates
    // frame reuse (13.8 -> 54 fps while moving).
    FighterPose fighterDisp_, foeDisp_;
    float dispPoseTime_ = -1.f;

    // ---- M-RIG: the three-piece brush rig ----
    // Articulation is an affine body plus two rigid mitts, each sampling a
    // DISJOINT region of the one rest volume. See the block comment in
    // shaders/brick_read.wgsl for the design; this is the CPU-side table.
    //
    // Two ways a piece gets its rest->world transform:
    //   srcBone >= 0  the posed skin matrix of that bone (legacy rigged asset)
    //   srcBone <  0  `xform`, written directly each frame by updateBrushRig
    // The fighter asset has no armature, so it always takes the second.
    //
    // lo/hi are the piece's rest-space AABB. Under the brush rig they are the
    // selected BRUSH's box, they change when the pose index changes, and the
    // shader clips to them — so they are load-bearing geometry, not just the
    // conservative cull they were under the bone rig. `boneMask` is dead
    // (the ownership test it fed is gone); kept only for the legacy path.
    struct AffinePiece {
        uint32_t boneMask = 0;
        float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
        int srcBone = -1;
        float xform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    };
    std::vector<AffinePiece> affinePieces_;
    // The opponent needs its OWN piece matrices (its own pose drives them), so
    // it cannot share the hero's table the way the bone rig did via skinMats_.
    std::vector<AffinePiece> foeAffinePieces_;

    // Static description of the three brushes, measured off the asset at import
    // and then constant. `canonLo/canonHi` are each hand brush's bounds mapped
    // back onto the authored (rest-hand) frame, so both poses are placed by the
    // same grip maths; `ofs` is the rest-space translation that brush carries
    // in the volume (zero for the rest hand, kGrabBrushOffset for the grab).
    struct BrushRig {
        bool valid = false;
        float bodyLo[3], bodyHi[3];
        float handLo[2][3], handHi[2][3];   // [0] = rest, [1] = grab, in-volume
        float canonLo[2][3], canonHi[2][3]; // the same, mapped to the rest frame
        float ofs[2][3];
        float palm[2][3]; // grip point per pose, canonical frame
    };
    BrushRig brush_{};
    // Pose index per hand, LATCHED on the 12 Hz grid: 0 = rest brush, 1 = grab
    // brush. Discrete and never interpolated — see brick_read.wgsl.
    int handPose_[2] = {0, 0};
    int foeHandPose_[2] = {0, 0};
    float handPoseTime_ = -1.f;

    // Procedural squish: one scalar per fighter. q < 0 is compressed, q > 0
    // stretched; the overshoot back through zero IS the bounce. Stepped on the
    // 12 Hz pose grid only (trap 4 + frame reuse), fixed substeps, no RNG.
    struct BodySpring {
        float q = 0.f, v = 0.f, gait = 0.f;
    };
    BodySpring spring_;
    // phase-offset so two identical fighters don't breathe in lockstep — that
    // reads as one puppet duplicated, not two actors (the clip path offsets
    // its own clock by the same 0.37 for the same reason)
    BodySpring foeSpring_{0.f, 0.f, 0.37f};
    float springPoseTime_ = -1.f;
    bool affineOn(const LookParams& look) const;
    // True when the asset arrived without an armature and the brush rig is the
    // only rig there is.
    bool skeletonFree() const { return bones_.empty() && !affinePieces_.empty(); }
    static void stepSpring(BodySpring& s, const RigParams& r, bool moving, float dt);
    // Shadow-proxy capsule fitted to a brush's rest AABB (there are no bones
    // left for deriveCapsules to fit to).
    static void capsuleFromBox(const float lo[3], const float hi[3], float a[3],
                               float b[3], float& r);
    void bodyAffine(const FighterPose& disp, const BodySpring& s,
                    const RigParams& r, float out[16]) const;
    // Recomputes one fighter's three piece transforms and brush boxes for the
    // current pose. `grip` is the world-space sword geometry to hold, or null
    // to leave the mitts riding the body.
    void updateBrushRig(std::vector<AffinePiece>& pieces, const int handPose[2],
                        const FighterPose& disp, const BodySpring& s,
                        const LookParams& look, const SwordParams* grip) const;
    // Writes the three pieces at `base` (66 for the hero, 293 for the foe) and
    // returns how many it wrote.
    int packAffinePieces(float out[kUniformSlots][4], int base,
                         const std::vector<AffinePiece>& pieces,
                         const std::vector<float>& mats) const;
    // Tight posed bound over those pieces: each piece's rest AABB corners run
    // through its own transform. The 13-piece path derives this from posed
    // capsule endpoints + rPiece; with three pieces the boxes ARE the clay
    // bounds, so the corners give a tighter sphere for free — and it has to be
    // recomputed rather than inherited, because a squish can push the body
    // past a bound measured on the rest mesh.
    float affineBoundR(const std::vector<AffinePiece>& pieces,
                       const std::vector<float>& mats, const float center[3]) const;

  public:
    // M5: where the fighter stands. Set from the gameplay tick; the renderer
    // premultiplies the whole skeleton by it each frame.
    //
    // Note the sim pose and the DISPLAY pose are different things: the sim
    // moves at 60 Hz, but what gets drawn snaps to the 12 Hz pose grid like
    // everything else that moves (trap 4). See fighterDisp_.
    void setFighter(const FighterPose& f) { fighter_ = f; }
    const FighterPose& fighter() const { return fighter_; }
    // index of a clip by name, -1 if absent (locomotion picks bounce/idle)
    int clipIndex(const char* name) const;
    // ---- players ----
    // Player 0 is the hero (articulated, holds the sword); every later player
    // is a carveable body with its own volume, placed by its own root.
    //
    // The cap is a SHADER limit, not a design one: WGSL bindings are static,
    // so each extra volume today means another bound array. Lifting it means
    // giving the per-cell arrays a per-player stride
    // (`bIndirection[player * CELLS + cell]`) so one binding serves all — see
    // PLAN.md "fighters are SLICES". addPlayer() keeps this signature when
    // that lands; only its body changes.
    static constexpr int kMaxPlayers = 2;
    int playerCount() const { return playerCount_; }
    // Returns the new player's index, or -1 if the volume budget is spent.
    int addPlayer(const FighterPose& at);
    FighterPose& player(int i) { return i <= 0 ? fighter_ : foePose_; }
    const FighterPose& player(int i) const { return i <= 0 ? fighter_ : foePose_; }
    void setPlayerEnabled(int i, bool on) {
        if (i > 0) foeEnabled_ = on;
    }
    bool playerEnabled(int i) const { return i <= 0 ? true : foeEnabled_; }
    // exposed so ctl/replay can place and toggle the opponent
    FighterPose* foePosePtr() { return &foePose_; }
    bool* foeEnabledPtr() { return &foeEnabled_; }

  private:
    FighterPose fighter_;
    // the sword resolved into WORLD space for this frame (carry mode folds in
    // the fighter root). Everything downstream — grips, IK, uniforms — reads
    // this, never look.sword directly.
    SwordParams swordWorld_;
    void resolveSword(const LookParams& look);
    void swordGeometry(const SwordParams& s, float hilt[3], float tip[3],
                       float gripA[3], float gripB[3]) const;
    void encodePick(wgpu::CommandEncoder& enc);
    void pollPick();

    // M4.6 conservation: carved volume -> ledger -> ballistic gobs ->
    // ground splats / body re-sticks. Visual sim, non-deterministic side.
    struct Gob {
        float pos[3], vel[3], disp[3]; // disp = 12 Hz-stepped display pos
        float radius, vol, grace;      // grace: skip body collision at launch
        float col[3];
    };
    void updateConservation(const LookParams& look, const FrameInfo& frame);
    void absorbMeasured(); // drain brick measurements into the ledger
    // M5: a MOVING blade that overlaps fighter 1 carves a channel along its
    // sweep. Gated on blade speed so a sword merely resting against the
    // opponent doesn't eat it.
    void updateBladeCut(const LookParams& look);
    float prevTip_[3] = {0, 0, 0}, prevHilt_[3] = {0, 0, 0};
    bool haveBlade_ = false;

    // ---- sword slice sploot ----
    // A slice is a CONTIGUOUS RUN OF CUTTING POSE STEPS: it opens on the first
    // blade edit and closes on the first pose step that queued none. One swing
    // can therefore eject more than one blob (the flourish enters and leaves
    // the body more than once) and each one is a real, separate wound.
    //
    // THE LEDGER INVARIANT: sliceVol_ is a RESERVATION against sploot_.debt,
    // never a second pot of clay. Blade volume is billed to carved/debt by
    // absorbMeasured exactly as before; sliceVol_ only withholds that much
    // debt from the dribble spawner until the slice closes. So
    // carved == deposited + inFlight + debt holds at every instant, and every
    // way this can go wrong (measurement dropped, gob array full, conserve
    // toggled off, snapshot loaded, app exits mid-swing) degrades to the old
    // dribble rather than stranding clay.
    float sliceVol_ = 0.f;    // debt withheld for this slice's single gob
    float sliceExit_[3] = {0, 0.5f, 0};    // world EXIT end of the last cut
    float sliceSweep_[3] = {1, 0, 0};      // blade travel there, unit
    float sliceNrm_[3] = {0, 1, 0};        // wound normal there, unit
    float sliceCol_[3] = {0.024f, 0.19f, 0.25f};
    bool sliceOpen_ = false;   // a slice is accumulating or awaiting flush
    bool sliceCutStep_ = false; // a blade edit was queued since the last pose step
    int slicePending_ = 0;     // blade edits queued, measurement not yet back
    int sliceWait_ = 0;        // pose steps spent waiting to flush

    Gpu* gpu_ = nullptr;
    BrickSystem brick_;
    // M5 fighter 1: its OWN carveable volume, so cutting it cannot touch the
    // hero's clay. Rigid — it stands and takes hits, no piece/warp path.
    BrickSystem foe_;
    GroundClay ground_;
    std::vector<Gob> gobs_;
    SplootStats sploot_;
    float lastWound_[3] = {0, 0.5f, 0};
    float woundDir_[3] = {0, 1, 0};
    float woundCol_[3] = {0.024f, 0.19f, 0.25f};
    bool haveWound_ = false;
    float lastSimTime_ = -1.f, lastPoseTime_ = -1.f;
    uint32_t gobSeed_ = 0x9e3779b9u;
    int width_ = 0, height_ = 0;

    wgpu::Texture hdrTex_, presentTex_;
    wgpu::TextureView hdrView_, presentView_;
    wgpu::Sampler sampler_;
    wgpu::Buffer uniformBuf_;

    wgpu::ComputePipeline tracePipeline_, pickPipeline_;
    wgpu::RenderPipeline postPipeline_, blitPipeline_;
    wgpu::BindGroup traceBind_, traceBrickBind_, postBind_, blitBind_;
    wgpu::BindGroup pickBind_, pickBrickBind_;
    wgpu::BindGroup traceFoeBind_, pickFoeBind_; // group(2): fighter 1's volume
    wgpu::Buffer pickOut_, pickRead_;
    bool pickMapPending_ = false;
    bool alwaysPick_ = false;
    float pickU_ = 0.5f, pickV_ = 0.5f;
    bool pickValid_ = false;
    float pickPos_[3] = {0, 0, 0};
    float pickRest_[3] = {0, 0, 0};
    float pickNormal_[3] = {0, 1, 0};
    float pickAlbedo_[3] = {0.024f, 0.19f, 0.25f};
    float pickMat_ = 0.f;

    long shaderDirStamp_ = 0;

    wgpu::QuerySet querySet_;
    wgpu::Buffer queryResolve_, queryRead_;
    bool queryMapPending_ = false;
    float traceMs_ = 0.f, postMs_ = 0.f;
    // frame reuse
    uint64_t traceDigest_ = 0;
    bool traceValid_ = false;
    bool reuseEnabled_ = true;
    uint64_t framesTraced_ = 0, framesPresented_ = 0;
    uint64_t traceInputDigest(const float u[kUniformSlots][4]) const;
    static bool digestIncludes(int slot, int comp);
    static const char* uniformSlotName(int slot);
    // CLAYFRAY_DEBUG_REUSE only: last frame's traced inputs, so a re-trace can
    // name the input that changed instead of just counting itself.
    float prevUniforms_[kUniformSlots][4] = {};
    uint32_t prevGens_[3] = {0, 0, 0};

    // Shared with in-flight MapAsync callbacks; false once destroyed.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
