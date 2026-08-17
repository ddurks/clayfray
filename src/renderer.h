#pragma once
#include <array>
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
    // How many fighters can be on the field. One slice of the shared
    // BrickStore each, so this is a MEMORY knob (see the capacity table on
    // BrickSystem::kMaxBricks), not the shader-binding wall it used to be.
    // Declared first because the uniform layout below is derived from it.
    static constexpr int kMaxPlayers = BrickSystem::kMaxFighters;

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
    // WHICH fighter pickRest() addresses. A rest point is meaningless without
    // it once there is more than one volume: the same coordinates name a
    // different body's clay in each slice. -1 when the ray hit no clay.
    int pickPlayer() const { return pickPlayer_; }
    const float* pickNormal() const { return pickNormal_; }
    float pickMat() const { return pickMat_; }
    // headless serve mode runs the pick pass too so ctl `probe` works
    void setAlwaysPick(bool v) { alwaysPick_ = v; }
    // snapshots the pick normal/albedo into the edit so conservation gobs
    // launch outward from the wound wearing its color; returns the resolved
    // edit so the ctl journal can record it pick-independently
    // The edit is routed to the fighter the pick landed on (pickPlayer()), so
    // any body can be sculpted, not just the hero.
    BrickEdit queueBrickEdit(BrickEdit e);
    // Player 0's volume. Callers that mean "the fighter being sculpted" want
    // playerBrick(pickPlayer()) instead.
    BrickSystem& brick() { return fighters_[0].vol; }
    BrickSystem& playerBrick(int i) { return fighters_[clampPlayer(i)].vol; }
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
        for (Fighter& f : fighters_) f.vol.pollVolumes();
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

    // ---- M-PHYS: one frame of weapon-in-body contact ----
    // The renderer resolves WHERE weapons are — it owns the piece transforms
    // and the posed capsules — so it is the only place that can say how deep
    // one is. It deliberately does not ACT on that: knockback, hitstun and
    // resistance are gameplay, and gameplay is the deterministic 60 Hz tick in
    // main.cpp. So this is a pure report, rebuilt from scratch every frame and
    // read once by the sim on the next.
    //
    // `bite` is metres of weapon inside the target's proxy, and it means
    // slightly different things per weapon on purpose: for a blade it is the
    // LENGTH OF BLADE buried (a sword half-through a body is fighting far more
    // than one that nicked it), for a fist it is how far the fist is past the
    // surface. Each has its own drag coefficient, so they never have to be the
    // same quantity.
    struct StrikeContact {
        int attacker = 0;
        int target = 0;
        int side = -1;    // -1 = the blade, 0/1 = which mitt
        float bite = 0.f; // m
        float dir[3] = {0.f, 0.f, 1.f}; // weapon travel, unit
        float speed = 0.f;              // m/s
    };
    const std::vector<StrikeContact>& contacts() const { return contacts_; }

  private:
    bool buildPipelines();
    void buildTargets();
    void buildBindGroups();
    // ---- the uniform block's layout, DERIVED rather than counted ----
    // MUST match the Uniforms struct in trace.wgsl AND pick.wgsl (trap 2), and
    // a mismatch only bites after a rebuild (shaders hot-load, binaries don't).
    // These used to be bare literals scattered through packUniforms — `out[65]`,
    // `out[293 + i * 12]` — which is why appending a field meant re-deriving
    // every later index by hand and the struct comment listed slot ranges.
    // Naming each start makes inserting a field a one-line edit here.
    static constexpr int kMaxMarbles = 4 * kMaxPlayers; // 2 eyes + 2 pupils each
    static constexpr int kSlotMarbles = 14;             // after the 14 look slots
    static constexpr int kSlotMarbleMeta = kSlotMarbles + kMaxMarbles * 2;
    static constexpr int kSlotSceneMeta = kSlotMarbleMeta + 1;
    static constexpr int kSlotCapsMeta = kSlotSceneMeta + 1;
    static constexpr int kSlotCapsCenter = kSlotCapsMeta + 1;
    static constexpr int kSlotCapsules = kSlotCapsCenter + 1;
    static constexpr int kSlotGobMeta = kSlotCapsules + 32; // 16 capsules x2
    static constexpr int kSlotGobs = kSlotGobMeta + 1;
    static constexpr int kSlotGroundMeta = kSlotGobs + 24; // 12 gobs x2
    static constexpr int kSlotSwordA = kSlotGroundMeta + 1;
    static constexpr int kSlotFighters = kSlotSwordA + 3; // hilt, tip, colour
    // One Piece: invSkin mat4 + aabb lo/hi. It was 12 — a forward skin mat4
    // and a rest capsule a/b rode along, packed every frame and read by no
    // shader (the round-trip check and the capsule test that wanted them both
    // died with the armature).
    static constexpr int kPieceSlots = 6;
    // One Fighter: meta + center + its pieces. WGSL requires a uniform array's
    // element stride to be a multiple of 16 bytes, i.e. a whole number of
    // slots — which this is by construction.
    static constexpr int kFighterSlots =
        2 + kPieceSlots * BrickSystem::kPiecesPerFighter;
    static constexpr int kUniformSlots =
        kSlotFighters + kFighterSlots * kMaxPlayers;
    void packUniforms(const OrbitCamera& cam, const LookParams& look,
                      const FrameInfo& frame, float out[kUniformSlots][4]) const;
    // First slot of fighter i's block.
    static constexpr int fighterSlot(int i) {
        return kSlotFighters + i * kFighterSlots;
    }
    std::vector<MarbleProp> marbles_;
    // ---- CHARACTER-level: one copy, shared by every fighter ----
    // These come off the ASSET and are identical for all bodies: the skeleton
    // (if any), its clips, and the rest capsules the shadow proxy is fitted
    // from. Everything POSED lives per fighter, in Fighter below — that split
    // is what let the hero/foe member pairs collapse into an array.
    std::vector<AssetBone> bones_;
    std::vector<AnimClip> clips_;
    std::vector<BoneCapsule> capsules_;
    // M4.7: floating-hand IK chains (built at setCharacter from bone names),
    // the blob's centre-of-mass decomposition that tethers them, and the
    // computed sword geometry for a frame (hilt/tip/two grips, world).
    std::vector<HandIkChain> handIk_;
    BodyCom bodyCom_;
    int playerCount_ = 1;   // the hero always exists
    float autoReach_ = 0.f; // rest COM->wrist distance; hands.reach 0 uses it
    // Rest-space bounding sphere of the character, measured off the asset at
    // import. Every fighter re-derives its POSED reject sphere from this each
    // frame (packUniforms); these two are only the un-rigged fallback.
    float bodyCenterRest_[3] = {0.f, 0.35f, 0.f};
    float bodyBoundRest_ = 0.6f;
    // M4.8 gaze: the camera position LATCHED at the last pose step. Sampling
    // the live camera would slide the eyes at frame rate and break the 12 Hz
    // stop-motion the rest of the character obeys.
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
    // frame reuse (13.8 -> 54 fps while moving). Per fighter: Fighter::disp.
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
    // conservative cull they were under the bone rig. (A `boneMask` lived here
    // for the shader's dominant-bone ownership test; that test is deleted and
    // must not come back — see brick_read.wgsl.)
    struct AffinePiece {
        float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
        int srcBone = -1;
        float xform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    };
    // Static description of the brushes, measured off the asset at import and
    // then constant. `canonLo/canonHi` are each hand brush's bounds mapped back
    // onto the authored (rest-hand) frame, so every pose is placed by the same
    // grip maths; `ofs` is the rest-space translation that brush carries in the
    // volume (zero for the rest hand, kHandBrushOffset[pose] otherwise).
    //
    // Indexed by HandBrush, so a pose the asset did not ship simply repeats the
    // rest entry and selecting it samples the rest brush.
    struct BrushRig {
        bool valid = false;
        float bodyLo[3], bodyHi[3];
        float bodyCenter[3] = {0.f, 0.35f, 0.f}; // the reach ball's origin
        float handLo[kHandBrushCount][3], handHi[kHandBrushCount][3]; // in-volume
        float canonLo[kHandBrushCount][3], canonHi[kHandBrushCount][3];
        float ofs[kHandBrushCount][3];
        float palm[kHandBrushCount][3]; // grip point per pose, canonical frame
    };
    BrushRig brush_{};
    float handPoseTime_ = -1.f;

    // Procedural squish: one scalar per fighter. q < 0 is compressed, q > 0
    // stretched; the overshoot back through zero IS the bounce. Stepped on the
    // 12 Hz pose grid only (trap 4 + frame reuse), fixed substeps, no RNG.
    struct BodySpring {
        float q = 0.f, v = 0.f, gait = 0.f;
    };
    float springPoseTime_ = -1.f;

    // M-BAG: the punching-bag wobble. Same integrator, same 12 Hz grid, but the
    // squash is about a HORIZONTAL axis the hit chose rather than about the
    // vertical, so it needs that axis carried with it. The axis is latched on
    // contact and then held for the whole ring-out: recomputing it as the dent
    // springs back would swing the deformation around while it is recovering.
    struct ImpactWobble {
        float q = 0.f, v = 0.f;   // q < 0 = dented along the axis
        float ax = 0.f, az = 1.f; // world xz, unit
    };

  public:
    // ---- ONE FIGHTER ----
    // Every field here used to exist twice, as a `foo_`/`foeFoo_` pair, and
    // that is exactly why the player cap was 2: adding a third body meant a
    // third copy of a dozen members plus a third `else if` at every use.
    // Player index is now the only thing that varies.
    //
    // `vol` is this fighter's slice of the renderer's one BrickStore — three
    // storage bindings serve all of them (see brick.h), which is what lifted
    // the cap in the first place.
    struct Fighter {
        BrickSystem vol;
        FighterPose pose;
        // What is actually DRAWN: the sim pose latched onto the 12 Hz grid.
        FighterPose disp;
        BodySpring spring;
        ImpactWobble wobble;
        // M-BAG: the deepest bite anything has taken out of this body SINCE THE
        // LAST POSE STEP, with the direction it came in on. Accumulated every
        // frame in reportContact and drained by stepImpact.
        //
        // The peak is kept per frame rather than sampled on the pose step
        // because a jab's contact can begin and end BETWEEN two steps: sampling
        // at 12 Hz would miss the fastest punches entirely, which are precisely
        // the ones that should wobble hardest.
        float hitPeak = 0.f;
        float hitDir[3] = {0.f, 0.f, 0.f};
        // Posed skin matrices; empty under the brush rig (there is no skeleton).
        std::vector<float> skinMats;
        // Posed brush pieces — body affine + one rigid transform per mitt.
        // Each fighter needs its OWN, since its own pose drives them.
        std::vector<AffinePiece> pieces;
        // Which HandBrush each mitt is sampling, LATCHED on the 12 Hz grid.
        // Discrete, never interpolated — see brick_read.wgsl.
        int handPose[2] = {kHandRest, kHandRest};
        // Where each mitt's fist centre was LAST frame, world. A punch cuts the
        // swept segment between the two, which is the same trick the blade uses
        // (updateBladeCut) and for the same reason: at strike speed a fist
        // crosses several centimetres per frame, so testing only where it is
        // now leaves a wound made of disconnected bites.
        float handPrev[2][3] = {{0, 0, 0}, {0, 0, 0}};
        bool haveHandPrev = false;
        // Its own clip clock, so two identical bodies don't breathe in
        // lockstep — that reads as one puppet duplicated, not two actors.
        float animT = 0.f;
        // Player 0 is always live; the rest switch on via addPlayer().
        bool enabled = false;

        // ---- M-DEATH ----
        // Clay carved off THIS body, m^3, net of anything that stuck back on.
        // Accumulated from the same measurements that feed the arena ledger
        // (absorbMeasured), because damage IS the conservation number — there
        // is no separate health pool that could drift out of step with it.
        float carved = 0.f;
        // Collapsed: no clay, no beads, no hitbox, nothing to punch. Distinct
        // from `enabled`, which means "this player slot is in play at all" and
        // is permanently true for the hero — the hero has to be able to die.
        bool dead = false;
        float respawnT = 0.f; // s left face-down
    };

  private:
    std::array<Fighter, kMaxPlayers> fighters_;
    // The hero, named for the many places that only ever mean player 0.
    Fighter& hero() { return fighters_[0]; }
    const Fighter& hero() const { return fighters_[0]; }

    bool affineOn(const LookParams& look) const;
    // True when the asset arrived without an armature and the brush rig is the
    // only rig there is.
    bool skeletonFree() const {
        return bones_.empty() && !fighters_[0].pieces.empty();
    }
    static void stepSpring(BodySpring& s, const RigParams& r, bool moving, float dt);
    // M-BAG: one pose step of the impact wobble. Consumes the fighter's
    // accumulated hit peak, so it must run exactly once per pose step.
    static void stepImpact(Fighter& f, const RigParams& r, float dt);
    // Shadow-proxy capsule fitted to a brush's rest AABB (there are no bones
    // left for deriveCapsules to fit to).
    static void capsuleFromBox(const float lo[3], const float hi[3], float a[3],
                               float b[3], float& r);
    void bodyAffine(const FighterPose& disp, const BodySpring& s,
                    const ImpactWobble& w, const RigParams& r,
                    float out[16]) const;
    // Recomputes one fighter's three piece transforms and brush boxes for the
    // current pose. `grip` is the world-space sword geometry to hold, or null
    // to place the mitts from look.handPose (idle / guard / punch).
    // `poseTime` is the 12 Hz clock the hand bob steps on.
    void updateBrushRig(std::vector<AffinePiece>& pieces, const int handPose[2],
                        const FighterPose& disp, const BodySpring& s,
                        const ImpactWobble& w, const LookParams& look,
                        const SwordParams* grip, float poseTime) const;
    // Palm position and orientation for ONE unarmed mitt, in CHARACTER space
    // (before the body affine and the per-side mirror). Split out because the
    // punch cut needs the same answer the rig draws, and re-deriving it would
    // be two places to get the guard height wrong.
    void unarmedHand(const FighterPose& disp, const LookParams& look, int side,
                     float poseTime, float outPos[3], float outRot[16]) const;
    // How far a mitt may stray from the body's centre. Under the brush rig
    // there is no skeleton to measure a wrist off, so this is the rest
    // distance from the body box's centre to the authored palm — the same
    // quantity autoReach_ held under the bone rig, taken from the boxes.
    float handReach(const LookParams& look) const;
    // Writes fighter `idx`'s pieces into its block and returns how many it
    // wrote. Capped at BrickSystem::kPiecesPerFighter — the brush rig uses
    // exactly three, and a piece that does not fit would silently vanish, so
    // this also warns once if an asset asks for more.
    int packAffinePieces(float out[kUniformSlots][4], int idx,
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
    void setFighter(const FighterPose& f) { fighters_[0].pose = f; }
    const FighterPose& fighter() const { return fighters_[0].pose; }
    // index of a clip by name, -1 if absent (locomotion picks bounce/idle)
    int clipIndex(const char* name) const;
    // ---- players ----
    // Player 0 is the hero (articulated, holds the sword); every later player
    // is a carveable body of the same character, placed by its own root and
    // running its own pose clock.
    //
    // The cap USED to be 2 and it was a shader limit: each extra volume was
    // another set of static WGSL bindings, and two fighters plus the ground
    // already sat at 7 of the 8 storage buffers core WebGPU guarantees. Now
    // every fighter is a SLICE of one store (brick.h) and the tracer costs 4
    // bindings at any N, so kMaxPlayers is purely a memory knob — see the
    // capacity table on BrickSystem::kMaxBricks.
    int playerCount() const { return playerCount_; }
    // Returns the new player's index, or -1 if the volume budget is spent.
    int addPlayer(const FighterPose& at);
    FighterPose& player(int i) { return fighters_[clampPlayer(i)].pose; }
    const FighterPose& player(int i) const { return fighters_[clampPlayer(i)].pose; }
    void setPlayerEnabled(int i, bool on) {
        // player 0 is the hero and cannot be switched off
        if (i > 0 && i < kMaxPlayers) fighters_[i].enabled = on;
    }
    bool playerEnabled(int i) const {
        return i <= 0 || (i < kMaxPlayers && fighters_[i].enabled);
    }
    // M-DEATH: in play AND still standing. This is the predicate gameplay
    // wants — a collapsed fighter has no body to collide with, steer at, or
    // punch — while playerEnabled() stays "is this slot in play at all".
    bool playerAlive(int i) const {
        return playerEnabled(i) && i >= 0 && i < kMaxPlayers && !fighters_[i].dead;
    }
    // Consumes a one-shot "this fighter just respawned" flag.
    //
    // It exists because a respawn TELEPORTS a body, and for the hero that body
    // belongs to the caller: the renderer can move fighters_[0].pose, but
    // main.cpp overwrites it from GameState every frame via setFighter, so the
    // new position would be gone before it was ever drawn. The caller has to
    // be told, and it has to drop the old velocity too — otherwise the fighter
    // arrives at the spawn point still running the direction it died going.
    bool takeRespawn(int i) {
        if (i < 0 || i >= kMaxPlayers || !respawned_[i]) return false;
        respawned_[i] = false;
        return true;
    }
    // 0 = untouched, 1 = at the collapse threshold. For a UI/telemetry read;
    // the collapse itself is decided inside updateDeaths.
    float playerDamage(int i) const {
        if (i < 0 || i >= kMaxPlayers || fighterVolume_ <= 0.f) return 0.f;
        return fighters_[i].carved / fighterVolume_;
    }
    // exposed so ctl/replay can place and toggle each opponent by index
    FighterPose* playerPosePtr(int i) { return &fighters_[clampPlayer(i)].pose; }
    bool* playerEnabledPtr(int i) { return &fighters_[clampPlayer(i)].enabled; }

  private:
    static int clampPlayer(int i) {
        return i < 0 ? 0 : (i >= kMaxPlayers ? kMaxPlayers - 1 : i);
    }
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

    // ---- M-DEATH ----
    // One fighter's worth of clay, m^3: the body brush plus two mitts, summed
    // from the mesh volumes measured at import. The denominator of the damage
    // fraction, and 0 for the analytic blob (which therefore never dies).
    float fighterVolume_ = 0.f;
    void updateDeaths(const LookParams& look, const FrameInfo& frame, float dt);
    bool respawned_[kMaxPlayers] = {}; // one-shot, drained by takeRespawn
    void collapseFighter(int i, const LookParams& look);
    void respawnFighter(int i, const LookParams& look);
    // The eyes, once nothing is wearing them. Four beads per collapse, and a
    // dead fighter frees exactly the four marble slots its corpse used to
    // occupy — so this rides in the SAME uniform array at the same size and
    // costs no layout change (trap 2 stays untouched).
    struct LooseMarble {
        float pos[3], vel[3], disp[3]; // disp = 12 Hz-stepped display position
        float radius = 0.02f;
        float col[3] = {0.8f, 0.8f, 0.8f};
        float rest = 0.f; // s spent stationary; retired once it settles
    };
    std::vector<LooseMarble> loose_;
    void updateLooseMarbles(const LookParams& look, const FrameInfo& frame, float dt,
                            bool poseStep);
    // M5: a MOVING blade that overlaps fighter 1 carves a channel along its
    // sweep. Gated on blade speed so a sword merely resting against the
    // opponent doesn't eat it.
    void updateBladeCut(const LookParams& look);
    float prevTip_[3] = {0, 0, 0}, prevHilt_[3] = {0, 0, 0};
    bool haveBlade_ = false;
    // M-FIST: the same idea one weapon down. A closed fist travelling fast
    // enough carves a fist-sized channel out of any OTHER fighter it passes
    // through, and the wound is pooled into one gob like a sword slice rather
    // than dribbled — a punch takes a chunk, it does not sneeze.
    void updatePunchCut(const LookParams& look);
    // A capsule of `owner`'s, posed into world. Under the brush rig the stored
    // capsule is fitted to ONE brush's box, so it has to be refitted from the
    // piece's CURRENT box before it is transformed — the piece's transform
    // undoes the selected brush's rest-space offset, so a capsule still
    // expressed in another brush's frame lands a metre across the arena. This
    // was invisible while every target held rest hands and became real the
    // moment opponents started closing their fists.
    void posedCapsule(const Fighter& owner, const BoneCapsule& c, float a[3],
                      float b[3], float& r) const;
    // Rebuilt every frame by the two cut paths, before either queues an edit.
    std::vector<StrikeContact> contacts_;
    // Contact is reported per (attacker, target, weapon) with the DEEPEST bite
    // of the frame, not once per substep or per sample — a blade emits up to
    // kOpsPerFrame capsule carves as it sweeps, and six separate shoves off one
    // swing would multiply knockback by the substep count.
    void reportContact(int attacker, int target, int side, float bite,
                       const float dir[3], float speed);

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
    // The three big buffers every fighter's volume is a slice of. Created
    // before any BrickSystem, because each one binds sub-ranges of these.
    BrickStore store_;
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
    // group(1) is now the WHOLE store plus the ground — one bind group for
    // every fighter, where M5 needed a second group per extra body.
    wgpu::BindGroup traceBind_, traceBrickBind_, postBind_, blitBind_;
    wgpu::BindGroup pickBind_, pickBrickBind_;
    wgpu::Buffer pickOut_, pickRead_;
    bool pickMapPending_ = false;
    bool alwaysPick_ = false;
    float pickU_ = 0.5f, pickV_ = 0.5f;
    bool pickValid_ = false;
    float pickPos_[3] = {0, 0, 0};
    float pickRest_[3] = {0, 0, 0};
    int pickPlayer_ = -1;
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
    // one per fighter volume, plus the ground field
    uint32_t prevGens_[kMaxPlayers + 1] = {};

    // Shared with in-flight MapAsync callbacks; false once destroyed.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};
