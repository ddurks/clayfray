# clayfray — POC plan

A limited-scope 1v1 fighting game where both fighters are realistic clay that
dents and slices, built on a custom C++ engine written from the ground up.
Art direction: *The Trap Door* (1984) — see [`reference/ART_DIRECTION.md`](reference/ART_DIRECTION.md).

## Locked decisions

| Decision | Choice | Why |
|---|---|---|
| Language | C++ | Write-once cross-platform; user requirement |
| Engine | Custom, minimal — no ECS, no scene graph, no asset pipeline | A 1v1 fighter has 2 fighters, 1 arena, 1 camera; "custom engine" means small, not general |
| Platform layer | SDL3 | Windowing, input, gamepads (fighter = input-critical) |
| GPU API | Dawn (WebGPU, C++) with a thin RHI wrapper | Native Metal on macOS dev machine (Xcode GPU capture works), D3D12/Vulkan/Web elsewhere; prior WGSL experience transfers; everything this project needs is compute + storage buffers + 32-bit atomics, all present. RHI wrapper keeps a raw-Vulkan escape hatch |
| Shape representation | Sparse brickmap SDF (8³ bricks, surface bricks only) + per-voxel albedo | Carve/dent/slice are field edits; per-voxel color enables clay contamination smears |
| Rendering | Direct sphere-trace of the global SDF, full screen, **no triangle pipeline** | Claybook-proven; edits visible instantly, SDF AO/soft shadows nearly free, chunks/fighters/arena unify in one code path. TAA instead of MSAA |
| Simulation split | Deterministic 60 Hz CPU gameplay core + non-deterministic GPU visual sim. Hitboxes are a **damage-stateful capsule skeleton**: per-bone integer mass notches (blunt hits shrink radius) and length notches (full/stub/gone from slices) | All inputs to deformation (who hit whom, where, how hard) already live in the deterministic tick — so shape change CAN be gameplay, only the GPU field can't. Integer notches keep it deterministic and legible |
| Sever authority | **Gameplay decides, visuals conform.** CPU capsule-vs-swept-blade test rules on severs; the visual slice CSG snaps to the ruled notch boundary; GPU island detection is cosmetic cleanup only | No GPU state ever feeds a gameplay decision → replays and future netcode survive. Two-tier rule: bigger than a fist = gameplay tier (deterministic, notch-quantized); smaller = cosmetic tier (GPU-only, collides with nothing but the arena) |
| Full-body physics | None. MLS-MPM (elastoplastic) **only for severed chunks** | Fully squishy fighters are unreadable and unplayable; chunks give the money shot cheaply |
| Animation look | Render 60 fps, visual poses quantized to 12 Hz, no motion blur; attack frame data authored on the 12 Hz grid | Measured from the show (~12.5 poses/sec on 2s); aligning active frames to pose steps keeps melee reads honest |
| Characters | Original designs in the Trap Door *style* | Style is fair game; Berk/Boni/Drutt are not. Keep the mkv out of any public git history |
| Boil is sacred | The per-pose-step (12 Hz) reseed of surface detail is a protected feature | User-loved ("the slight deformations like real claymation — we need to preserve that"). Perf work must never cache/freeze the detail reseed; it IS the claymation life |
| Eyes are marbles | Eyes are rigid glass props, never clay: no carve, no boil, no detail normals, hard glint (all true in the renderer today). When dislodged they're RIGID BODIES — sphere-vs-arena, they roll and bounce; never MPM particles | "They should behave like little marbles for effect." Also the stop-motion truth: real claymation eyes are glass beads pressed into plasticine |
| Platform reach | macOS now; Windows/Linux next; **mobile is a declared target** | Dawn runs Metal-on-iOS and Vulkan-on-Android, SDL3 covers both; the tracer's cost scales with traced pixels, so the resolution-scale lever + M3 query diet are the path. Phone-class GPUs ran Claybook-likes; feasible if we stay disciplined |
| Content pipeline | Blender → glTF → import into the engine (mesh + armature + clips) | Generalize beyond the hand-authored blob: voxelize any mesh into the brickmap, drive it with any armature. See "Blender pipeline" under M4 |
| Gameplay shape (2026-08-13) | Third-person over-the-shoulder duel; **1 s simultaneous-commit ticks** (both pick Move/Block/Slice hidden, resolve together — it's about *predicting* the opponent); **slice = mouse-drag → sword swing**, arc clamped to arm reach | Turn-by-turn-ish cadence makes it a mind-game, not an execution test. The drag-authored swing *is* the swept-blade the locked sever design already wanted. Simultaneous commit keeps deterministic replay/netcode. See M4.7 (enabling IK) + revised M5 |

Dependencies (vendored, full list): SDL3, Dawn, GLM, Dear ImGui, Tracy, stb, miniaudio.
No physics engine — chunk-vs-floor collision is small enough to write ourselves.

## Frame architecture

```
[1] Gameplay core   CPU, deterministic, fixed 60 Hz tick
                    input buffering → state machines → damage-stateful capsule
                    skeleton (hit/hurtboxes + per-bone mass/length notches)
[2] Command stream  tick emits: skeleton poses, dent events, slice events
[3] GPU sim graph   apply field edits → redistance dirty bricks (JFA, narrow band)
                    → MPM substeps for live chunks → splat particles into field
                    (only GPU→CPU readback: async island-detection results)
[4] Render          one full-screen sphere-trace pass → TAA → film-look post → UI
```

---

## Milestone 0 — Toolchain skeleton

*Goal: validate the entire stack on macOS in one weekend.*

- [x] CMake project (FetchContent for deps), builds on macOS; CI stub for Win/Linux later
- [x] SDL3 window + event loop + gamepad hotplug logging
- [x] Dawn device init, swapchain, clear color (surface path smoke-tested via `--exit-after`)
- [x] First WGSL compute shader writing to a screen texture
- [x] Dear ImGui overlay (look-dev panel wired to all params)
- [ ] Tracy: CMake option `CLAYFRAY_TRACY` exists, zone macros not yet instrumented (do with M2 profiling)
- [x] Fixed-timestep loop scaffold (60 Hz tick / uncapped render, interpolation stub)

## Milestone 1 — Look-dev: sphere tracer + clay shading  ⛔ GATE

*Goal: prove the look. This gate decides the project.*

- [x] Full-screen sphere trace of an analytic SDF test scene (WGSL, compute)
- [x] Orbit camera (left-drag orbit, wheel zoom, panel sliders)
- [x] Clay material: matte wrap BRDF, procedural tool-mark detail normals, per-pose-step boil (verified: 29.8 dB PSNR between adjacent 12 Hz steps)
- [x] SDF ambient occlusion + soft shadows from distance queries
- [x] Theatrical spot key (cone aimed at stage center makes the pool of light) + cool rim, crushing to warm black
- [x] Film-look post pass: grain stepped at 25 Hz, warm-black vignette, gate weave, mild bloom
- [x] Pebble-mosaic test arena: floor + back wall of blended ellipsoid stones, palette from the measured contact-sheet hues
- [x] **Gate check: passed 2026-08-12 on `lookdev/r11.png`** (user notes folded in across several iterations; intermediate renders since pruned)

## Milestone 2 — Sparse brickmap + GPU carve

*Goal: the field becomes editable. A sculpting toy, fun in its own right.*

- [x] Brickmap data structure in storage buffers (8³ bricks / 7³ unique voxels, 74³ cells ≈ 518³ effective, f16 ±12-voxel band + rgba8 albedo)
- [x] Sparse allocation/free of bricks on GPU (atomic bump + freelist; reuse verified via readback stats)
- [x] Character baked into the field; tracer marches brickmap + analytic arena through one query (JFA+2 Euclidean seeds for empty space, eikonal-relaxed coarse distance for AO/penumbra)
- [x] Per-voxel albedo channel, trilinear-sampled (added clay keeps its color — contamination works)
- [x] Carve (soft CSG subtract) and add-clay (soft min + color stamp): mouse modes in windowed, scripted `--carve-test` headless
- [ ] Dirty-brick tracking is region-dispatch per edit; the indirect dispatch chain moves to M3 where redistancing needs it
- [x] Debug tooling grown along the way: `CLAYFRAY_DEBUG_STATS` (allocation readback + field scan + coarse histogram), `CLAYFRAY_DEBUG_NORMALS` / `_FLAT` / `_FLATALBEDO` isolation renders

M2 look recovery (user: "it does not look good" / "looks corrupted"): the
brickmap's approximated distances band under every shading term that samples
them. Final architecture, do not regress:
- Primary march: conservative brick field (accurate, safe).
- AO: point queries on the loose field, from the GEOMETRIC normal (detail
  normals into AO stamp dark pits at every fake dimple).
- Soft shadows: GEOMETRIC (field-independent) stepping — stepping by the
  field imprints its crease pattern into the penumbra — with occlusion from
  the smooth ANALYTIC body proxy. This is the M4-forward plan anyway: the
  posed capsule skeleton casts the shadows; craters/dents don't cast.
- Detail normals are lighting-only; fine grain is decorrelated hash jitter
  (any smooth band-limited noise gradient reads as fingerprint iso-bands).
User-approved look: `lookdev/m2_approved.png` / `m2_solid_carve.png`
(smoother than r11 — "already an ideal amount of bumpiness").
Solid-clay rules (user: "is he hollow?"): fresh interior bricks get body
albedo on allocation (color goes all the way through), and the shadow
occluder is max(analytic proxy, real field) so carved cavities receive
light. Craters read as lit clay bowls, not voids.

M2 perf note: ~130 ms/frame at 720p on the M2 Mac — fine for a sculpting toy,
not for the game. Tracy instrumentation + a march-query diet (arena pebbles
evaluated per step, AO still double-samples) is the first order of business
in M3. First lever shipped: internal resolution scale (panel → render →
resolution scale; window DPI already logical, not retina-native). Low-res +
grain is also an aesthetic candidate — evaluate ~0.5 for the stop-motion
feel before assuming full res is the target.

## Milestone 3 — Redistancing

*Goal: edits stop degrading the field.*

- [x] Narrow-band re-distance of dirty bricks: edits stamp a dirty bit → compact pass gathers a dirty list → **indirect dispatch** (the deferred M2 item) of Godunov eikonal iterations in workgroup shared memory (10³ apron per brick), surface voxels frozen so the sculpted shape never moves, 3 rounds carry healing across brick seams
- [x] Correctness harness: `CLAYFRAY_DEBUG_GRAD=1` renders a |∇d|−1 heatmap (green = true SDF, blue/red = under/over). Verified: stacked-stroke damage heals to green; the bake's smin-blend under-gradient is inherent and benign (conservative)
- [x] Edit-to-clean latency: redistance runs inside the edit's own submit — same-frame, no measurable frame-time cost at test edit sizes
- [x] Perf pass: conservative floor/wall bounds skip the pebble evaluation for far queries — **137 → 37 ms/frame at 720p full res (3.7×)**; at the game's 0.5 internal scale ≈ 9 ms ≈ 100+ fps sculpting
- [ ] GPU timestamps: implemented but **opt-in only** (`CLAYFRAY_TS=1`) — Dawn's Metal pass-boundary timestamps drop command buffers under heavy multi-pass frames on Apple Silicon (blank-frame bug, cost a debugging session) and report dubious values. Wall-clock is the Mac tool; timestamps should work on D3D12/Vulkan. Tracy zones remain future work

## Milestone 3.5 — Blender content pipeline (NEW)

*Goal: model and rig anything in Blender, play it in the engine.*

- [x] glTF import (cgltf): mesh, vertex colors, skin weights, skeleton, marbles (animation clips parse with M4, which consumes them)
- [x] Mesh → SDF voxelization: CPU tri-binning per cell + watertight raycast-parity inside/outside; GPU exact closest-triangle distance (pseudonormal sign near surface, parity deep), all in the existing brick pipeline (JFA/coarse reuse)
- [x] Albedo: barycentric vertex color × the engine's clay mottle → per-voxel color (COLOR_0 is linear — the build script pre-linearizes)
- [x] Per-voxel top-2 bone weights, quantized u8, in a new brick pool — populated and awaiting M4's query-warp
- [ ] Auto-derived capsule proxy from armature bones → moves to M4 (imported characters currently borrow the hand-coded analytic proxy for shadows; right shape for the test fighter, wrong in general)
- [ ] `.clay` cooked format → backlog; import is a few seconds at launch, acceptable for now
- [x] `marble_*` convention: rigid analytic props with material color, never voxelized; eyes are data-driven marbles engine-wide now (hardcoded eyes removed)
- [x] **Acceptance passed** (`lookdev/m35_final.png` / `m35_final_carve.png`): fighter authored by checked-in headless script `assets/build_fighter.py` (real-sphere union + voxel remesh + smooth — NOT metaballs, whose falloff lies about sizes), round-tripped, carve-tested. The character is content now, not code
- Honest constraint: voxelization accepts ANY mesh, but the clay LOOK and the
  skinning warp both favor chunky forms — thin fingers/capes will misbehave.
  Trap Door proportions aren't just style, they're load-bearing.

## Milestone 4 — Skeleton-driven fighter  🔬 RESEARCH RISK

*Goal: the genuinely novel part — a skinned humanoid SDF that traces correctly.*

**Architecture (shipped, replaces the query-warp sketch):** the ONE
rest-space brickmap articulates through per-bone pieces sampled at N-bone
(up to 4) inverse-LBS warped points. A per-cell skin field (bCellW: 4 joints
+ 4 weights, distance-weighted gather at import, flood-filled volume-wide)
drives the blend; a convex combination of rigid warps is a contraction, so
sphere tracing stays safe with no conservative-step hack. A region renders
once, by the piece whose interpolated influence is near-maximal; a forward
round-trip check (re-map the sampled rest point by ITS OWN weights, reject
if it lands > 5 cm from the query) kills vacated-space ghosts; an argmax
fallback guarantees coverage; plain-min union (dual samples are identical
points — smin only bulged seam rings). Squash/stretch = per-piece min-scale
distance correction. Pose = 12 Hz-quantized CPU clip eval (STEP/LINEAR,
nlerp). Carves live in rest space and ride bones structurally. No voxel
data is touched at pose steps: pose cost is O(bones).

- [x] Clip pipeline: glTF animation parse, 12 Hz pose eval, loop + speed + rest-pose toggle in the panel
- [x] N-bone chunk articulation of the carveable brickmap (validated: rest pose exact; 80° arm droop clean incl. hands/armpits; heatmap sub-1)
- [x] Squash & stretch via scale channels (min-scale Lipschitz correction)
- [x] Rigid pristine eyes: marbles bound to nearest bone, radius fixed
- [x] Auto-derived capsule shadow proxy from bones, posed per step (M3.5 leftover)
- [x] Boiling reseed per pose step (was already keyed to poseTime)
- [x] Pose quantization: 12 Hz skeleton, smooth camera
- [ ] Carve routing while posed: world hit → rest frame of the owning piece(s) (edits currently land in rest coords; pause anim to sculpt precisely)
- [ ] Shape keys: per-piece morph distance layers, mix() blending (Lipschitz-1), weights from animation channels
- [~] Perf pass (profiled + first trim; deeper work deferred):
  - PROFILED (wall-clock, GPU timestamps unreliable on this Metal driver):
    articulation ~doubles trace cost — 960×540 aa1, `CLAYFRAY_NO_PIECES`
    bypass, ~70 ms/frame → ~136 ms/frame (+66 ms, ×1.9). Confirms the 2-3×
    estimate. At the game's 0.5 internal scale that's ~34 → ~66 ms/frame.
  - TRIMMED: `charDist` ran the fixed-point warp refinement (2nd ~17-load
    cell gather) on every contributing piece every march step. Gated it on
    warp displacement (>0.006 m ≈ 0.3 span) — near-rigid regions converge in
    one pass, bent joints keep both. ~10% render win at/near rest (the common
    stance per the rig note), neutral when bent. Verified: posed body clean,
    |∇d| heatmap still green (`lookdev/perf_grad.png`).
  - DEFERRED: the remaining ~1.8× is structural (per-step cell-field gathers).
    Real wins need Xcode GPU-capture profiling or an algorithmic change
    (cache warps across steps / coarser influence field). Do before two
    fighters at 1080p, not required for single-fighter M6 work.
- [ ] Two fighters + arena in one traced scene at 60 fps, 1080p-class res on the dev Mac
- Rig craft note: keep rest poses near the resting stance (warp does least where the player stares longest); set Inherit Scale consistently — the glTF exporter drops inherit-scale-off compensation tracks

## Milestone 4.6 — Clay conservation ("sploot") (NEW, shipped)

*Goal: carved clay goes somewhere. Total clay in the scene is conserved.*

Architecture: measure → ledger → gobs → deposit.
- [x] **Measure**: the edit fill pass accumulates per-voxel |occupancy delta|
  (1-voxel linear ramp, unique 7³ voxels only; whole-cell swallows counted in
  classify) into `counters[3]`, fixed-point ×1024. Copied per-op into a
  grow-on-demand readback pool (CPU can run ~100 frames ahead of the GPU
  headless; a dropped measurement is leaked clay). ~1–2 frame latency.
- [x] **Ledger** (renderer, visual-sim side): carves add debt; debt spawns
  gobs (2–35 ml each, ≤12 in flight); every deposit subtracts. Gob adds onto
  the body reconcile through their own measurement (smin over/under-fill goes
  back on the ledger). `--carve-test` balances exactly: carved 2294.4 ml =
  landed 2294.4 ml.
- [x] **Gobs**: ballistic CPU sim at frame rate, *display positions quantized
  to the 12 Hz pose grid* (smooth droplets against stepped poses would break
  the stop-motion frame). Launch along the pick normal wearing charAlbedo at
  the wound (pick pass now returns pos+normal+material+albedo). Clay never
  bounces: capsule hits deflect-and-slide (or stick as a mode-2 add when the
  body is at rest — same rest-space rule the carve tool lives with), floor
  contact splats.
- [x] **Deposit**: `GroundClay` heightfield (512², x,z ∈ ±1.75 m) — clay
  thickness ON TOP of a bisection-precomputed pebble-surface base (nothing
  hides in mosaic cracks), per-texel color (carved cyan lands cyan), quartic
  bump kernels with closed-form integral so deposited volume is exact by
  construction; radius swells with local pile height (slump, not towers).
  splat(pos, vol, color) is the contract — M7 chunk settling can swap the
  backing store for a world brickmap without touching the ledger or gobs.
- [x] Tracer: heightfield unioned into map/mapLoose/mapPenumbra (piles get
  AO + shadows), body-style boil/detail normals + mottle, empty-field
  sentinel short-circuits to zero cost pre-carve. ~8% frame cost with a
  2.3 L splooted scene at 960×540.
- [x] Lighting regression fixed (the M2 lesson, relearned): the MARCH gets
  the conservative (Lipschitz-scaled, arenaFloor-style gated) distance;
  AO/penumbra get a SMOOTH unscaled variant — feeding them the scaled one
  reads as phantom occlusion and bands under the penumbra's stepping. And
  the field's top bound must be the true tallest pile (a per-splat sum hit
  the 0.5 m cap and became an invisible occluder plane at chest height:
  scene-wide darkening + black scanlines where rays grazed it).
- [x] Second lighting fix — whole-footprint tint on first sploot: a 2 mm
  "skirt below the floor" was meant to hide the empty field, but after the
  0.3× march scaling + hit-epsilon it still registered, so the instant the
  first gob landed the ENTIRE floor shaded clay-cyan + darkened. Replaced
  with a real thickness gate (`groundThicknessAt < G_THMIN` → yield to the
  arena floor); the cutoff sits at a sub-mm pile lip, invisible. Rule: the
  ground field must render clay ONLY where clay was deposited, never a
  floor-wide sheet. Verify by eye: carve-test floor colors at --frames 8
  (field off) must match --frames 60/300 (field on) everywhere except the
  piles.
- [x] Panel: sploot section (conserve toggle + live ledger readout);
  `CLAYFRAY_DEBUG_LEDGER=1` prints per-op measurements + final balance.
- [ ] Carving LANDED clay back up (heightfield subtract + pick routing) —
  pairs with M8's chunk-reattach economy question
- [ ] Wall-face sticking (V1: wall hits smear down to the floor at its base)
- Note: `--carve-test` coordinates were retargeted to the imported fighter's
  aabb — the old set was authored for the taller analytic blob and carved
  air, which the ledger immediately exposed (measured 0 ml).

## Milestone 4.7 — IK arm rig + sword prop (NEW — enabling tech for M5)

*Goal: the fighter holds a sword and the arms follow it via IK. No gameplay
logic yet — this is the rig the dueling core drives.*

The control scheme inverts today's flow: clips pose the arms (FK) and LBS
renders the SDF around them; here the SWORD is master (driven by the swing or
a hold pose) and the arms follow to grip it. IK is just another pose source
writing into the same `skinMats_` the chunk articulation already reads.

Rig reality (from the .glb, NOT the stale build script): a real 3-joint arm
chain per side — `humerus.<s>` → `radius.<s>` → `hand.<s>` → `fingertips`,
thumb also under radius. So true 2-bone IK (shoulder=humerus, elbow=radius,
wrist=hand) works; `applyArmIk` rotates the two subtrees so the hand reaches.

- [x] 2-bone analytic IK per arm (anim.cpp `applyArmIk`): reconstruct world
  from `skinMats_`, law-of-cosines elbow, rigidly rotate the shoulder subtree
  about S then the elbow subtree about the solved E' (no per-bone locals
  needed), rewrite affected skin mats. Elbow-bend plane taken from the FK pose
  (falls back to a down/back hint). Existing M4 LBS renders it — no new SDF
  path; |∇d| heatmap stays green (`lookdev/ik_grad.png`)
- [x] Grip-target data model: `ArmIkChain` per arm with subtree lists; the
  sword carries 2 grips (grip0/grip1 along the blade), hands assigned by
  `.l`/`.r`. One-prop-two-grips now; per-hand props (dual-wield) drop in
  without IK changes
- [x] Sword as a rigid emissive prop (marble precedent): capsule SDF hilt→tip,
  emissive lightsaber shading (bloom does the glow), `MAT_SWORD`; uniforms
  swordA/B/Col. Model swap later
- [x] Reachable envelope: per-hand target clamped to the arm's [|L1−L2|,
  L1+L2] annulus so IK always solves and the arm reads as a real reach.
  Sword-transform clamp / torso lean for gross out-of-reach still DEFERRED
- [x] IK layered over FK: arms driven by IK to the grips, torso/head by the
  clip — verified holding the guard identically across clip frames
  (`lookdev/ik_anim.png` at t=2.4 vs rest)
- [x] Debug harness: "sword / IK" panel drives the hilt transform live;
  `lookdev/ik_test2.png` two-handed guard. Windowed smoke test clean
- [ ] DEFERRED refinements: hand ORIENTATION to grip (wrist twist to align the
  mitt with the handle — position-only for now), sword-transform reach clamp +
  torso lean, and 12 Hz quantization of the swing (smooth for debug drag now)

## Milestone DEV — Agent iteration loop (NEW, shipped 2026-08-13)

Not a game milestone: tooling that collapses the edit→rebuild→relaunch→
re-pose→screenshot loop, designed agent-first (every knob commandable, every
state dumpable, every regression exit-code gateable). Shaders already
hot-load; this adds the rest. See CLAUDE.md "Agent dev loop" for commands.

- [x] **ctl port**: file inbox `ctl/in/` polled per frame (windowed AND
  `--serve` headless); `tools/ctl.sh` client. set/get over a registry of all
  LookParams/SwordParams/camera/brush fields, `edit` (scripted carves/adds),
  `shot`, `stats` JSON, `probe`/`pickuv`, pause/resume/`step N` (12 Hz pose
  steps)/timescale, `snap save|load`, `record`, `break ledger`, `quit`
- [x] **snapshots**: full sim state (brick pools to high-water mark +
  allocator + JFA/coarse, ground field + mirror, ledger, gobs, sim time) in a
  tagged-section file, `snapshots/<name>.snap` (~80 MB). In-session round
  trip is byte-identical (`rt_A/B.png` cmp equal); `--load NAME` restores at
  launch cross-process. Load poisons in-flight volume measurements (snapGen)
- [x] **journal replay**: `record` captures tick-stamped set/edit/bake lines
  (brush strokes recorded pick-resolved); `--replay f.journal` re-runs them
  headless on the fixed 60 Hz step with measurement arrival pinned
  (syncMeasurements) — ledger is run-to-run EXACT, conservation gate applies
  (exit 3), screenshots match to ~0.02% of pixels (redistance apron healing
  is dispatch-order sensitive; that's the pre-existing noise floor). Gate
  images with `python3 tools/imgdiff.py a.png b.png` (exit 1 outside tol)
- [x] **break-on-condition**: `break ledger TOL` pauses the clock on residual
  instead of exiting — state stays inspectable via stats/shot/snap
- [ ] Later if needed: C++ dylib hot reload (deliberately skipped — snapshots
  make rebuild+relaunch+`--load` ~10 s and always ABI-correct), param-file
  watch, pause-on-NaN

## Milestone 5 — Dueling core (REVISED — was "Gameplay core")

*Goal: two fighters, 1 s predictive ticks, sword slicing. Playable-if-rough,
capsule-debug visuals. Parallel track, but now gated on M4.7 for the sword.*

Locked control scheme (2026-08-13, see top table): third-person
over-the-shoulder; **1 s decision tick = 60 sim ticks = 12 pose steps**;
**simultaneous commit** (both pick hidden, resolve together); three actions.

- [ ] Deterministic 60 Hz core + 1 s commit cadence; one action committed per
  tick, both revealed at tick start (replay/netcode-friendly by construction)
- [ ] Three actions: MOVE (fixed step, auto-facing the opponent — toward/away/
  around a dueling ring), BLOCK (raise sword to a guard arc via M4.7 IK;
  negates slices whose swept arc crosses it), SLICE (mouse-drag → swing)
- [ ] Slice authoring: capture the screen-space drag, project to a swing arc
  CLAMPED to the arm-reach envelope (M4.7), sample on the 12 Hz grid; the
  sword sweeps that arc over the tick
- [ ] Swept-blade sever (PULLED FORWARD from M6 — it's the core verb now): CPU
  capsule-vs-swept-blade test against where the opponent ACTUALLY moved this
  tick → deterministic ruling, notch-quantized; bigger-than-fist = gameplay
  tier (per the locked sever decision)
- [ ] Block resolution: guard arc vs incoming swept blade → full/partial
  negation; chip/stagger tuned in playtest
- [ ] Simultaneous resolution: both actions animate on the 60 Hz core; a slice
  tests against the opponent's tick-end capsule path (did they step into it?
  did they block the arc you chose?) — this IS the mind-game
- [ ] Per-bone damage state (unchanged from the original M5): mass notches
  (0–7, blunt shrinks capsule radius) + length notches (full/stub/gone),
  integers only; severed bone + children leave the hurtbox set
- [ ] Damage consequences: lighter = faster but weaker; severed arm disables
  that side's moves (and, with M4.7, that hand's grip → sword drops / one-hand)
- [ ] Over-the-shoulder camera behind your fighter, opponent framed ahead;
  drag maps onto the opponent's silhouette. (Supersedes M8's "fixed tabletop
  camera" for PLAY; tabletop may return for intros/replays)
- [ ] Capsule + frame-data debug view (ImGui) + replay recording from the
  input stream (doubles as the determinism test)

## Milestone 6 — Impacts and slicing (visuals)

*Goal: hits leave marks; the sword means something.* NOTE: the gameplay-tier
swept-blade **sever ruling moved to M5** (it's the core verb). M6 is now the
VISUAL layer that conforms to M5's rulings — dents, contamination, the CSG
cut, cosmetic-tier slices, chunk labeling.

- [ ] Wire tick events → GPU edit queue (dent field edits from hit events, scaled by move weight)
- [ ] Visual dents conform to mass notches: field edit depth driven by the notch change, so silhouette tracks hitbox
- [ ] Color contamination: impact stamps attacker albedo into defender; floor contact smears floor color
- [ ] Slice visuals conform to the CPU sever ruling: CSG cut plane snaps to the ruled notch boundary (gameplay decides, visuals conform)
- [ ] Cosmetic-tier slices (smaller than a fist): free-form CSG, no hitbox effect
- [ ] GPU connected-component labeling over the fighter's bricks — cosmetic cleanup only, turns already-ruled severed geometry into chunks (no gameplay readback)
- [ ] Detach event: island voxels handed off to the chunk system (milestone 7 consumes this)

## Milestone 7 — MPM chunks

*Goal: severed clay falls, squishes, and rejoins the world.*

- [ ] MLS-MPM solver in WGSL, elastoplastic (von Mises yield), fixed-point 32-bit atomic P2G
- [ ] Seed particles from detached island voxels (position + albedo)
- [ ] Particle splat back into the global SDF each frame (chunks render through the same tracer, blend where they touch)
- [ ] Floor + wall collision vs the static arena SDF (free — it's a distance query)
- [ ] Chunk lifetime: settle → merge into static field or fade
- [ ] Dislodged eye marbles: rigid sphere vs arena SDF (roll, bounce, settle) — pointedly NOT MPM; an eye popping out and rolling across the dungeon floor is a Trap Door gag worth engineering for
- [ ] Eye dislodge trigger: when the clay socket supporting an eye is carved/severed away, the marble detaches (support check against the field around its anchor)

## Milestone 8 — It's a game

*Goal: someone who isn't us can play a round and laugh.*

- [ ] Health = total remaining mass notches; round loss on head/torso depletion or losing both arms (tune in playtest)
- [ ] **Candidate mechanic (playtest before committing): chunk reattach** — walk onto your own severed chunk to slam it back on and recover notches; makes clay mass the core resource economy
- [ ] Rounds, win screen, rematch flow
- [ ] Fixed tabletop camera with subtle framing shifts
- [ ] Trap-door stage hazard: opens mid-round (design TBD — pull, spawn, or swallow chunks)
- [ ] Sound: miniaudio, squelchy foley placeholder
- [ ] Day (amber) and night (blue) lighting rigs as stage variants
- [ ] Second platform build (Windows) to cash the write-once check

---

## Risks & open questions

- **Skinned-SDF distance distortion (M4)** — the research crux. Fallbacks if query-warp
  misbehaves: per-bone rigid brick regions, or re-splat the body into the field each
  pose step (costlier but robust).
- **Full-screen trace + edits perf on Apple Silicon (M1–M4)** — Claybook did it on a
  PS4; modern Macs are strong at compute. Verify at each gate, keep dynamic resolution
  as a lever.
- **TAA vs 12 Hz stepped poses** — TAA history hates teleporting geometry; may need
  history rejection keyed to pose-step boundaries. Investigate during M4.
- **Visual/hitbox agreement (M6)** — both layers are driven by the same hit events, but
  they won't match to the millimeter. Acceptance bar: "the silhouette I see is roughly
  the thing I can hit." If drift is noticeable in playtest, tighten the dent-conform pass.
- **Trap-door hazard design (M8)** — intentionally undecided until the game is playable.

## Order of play

M0 → M1 (gate) → M2 → M3 → M4 (research) → **M4.6 (done)** → **M4.7 (IK, next)**
→ **M5 (dueling core)** → M6 (impact visuals) → M7 → M8. The gameplay design
(2026-08-13) makes M4.7 the enabling step for M5, and pulls the gameplay sever
from M6 into M5. M5 no longer "starts anytime" — it needs the M4.7 sword rig.
Nothing after M1 is worth building if the gate render doesn't look like the show.

Also still open before layering gameplay: M4 leftovers (carve-while-posed,
shape keys), M4.6 leftovers (scoop landed clay, wall sticking), and the deeper
`charDist` perf pass — none block M4.7/M5, revisit as needed.
