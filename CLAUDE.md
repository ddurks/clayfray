# CLAUDE.md — agent onboarding for clayfray

Read `PLAN.md` first: it holds the architecture, the locked decisions, and the
per-milestone "do not regress" notes (M2 look recovery, M4.6 conservation).
This file is the short list of traps that have actually bitten and the commands
to verify a change.

## Build & run

```sh
cmake --build build                 # configures via FetchContent on first run
./build/clayfray                     # windowed (sculpt + orbit + ImGui panel)
./build/clayfray --screenshot out.png --frames 8 --size 960x540 --aa 2
```

Headless flags: `--screenshot PATH --size WxH --frames N --time T --aa N`,
`--cam AZ,EL,DIST` (inspect from another angle without serve mode),
`--character file.glb`, `--carve-test` (scripted carve/add exercise),
`--exit-after N` (windowed smoke test), `--serve` (headless ctl session),
`--replay f.journal` (deterministic scenario replay), `--load NAME`
(restore a snapshot at launch).

## Players (M5)

`Renderer::addPlayer(pose)` adds a fighter and returns its index; player 0 is
the hero. **The cap is 2** — each body needs its own volume and WGSL bindings
are static, so 3+ requires the per-player stride in PLAN.md ("fighters are
SLICES"). That cap is now enforced by the API, not taste: a third fighter's
3 bindings would put `trace` at 10, over the 8 storage buffers per stage that
core WebGPU guarantees (trap 8). ctl exposes `foe.enabled`, `foe.pos`,
`foe.yaw`.

Both fighters bill to ONE conservation ledger: clay off either body becomes
the same gobs on the same arena.

## The affine rig (M-PERF) — how the fighter animates now

`look.affineRig` (default ON, `CLAYFRAY_NO_AFFINE=1` to pin it off) collapses
articulation from 13 inverse-LBS pieces to **three**: an affine body plus one
rigid transform per mitt. It exists because `charDistI` resolved skinning PER
SAMPLE — ~66 field evaluations per shaded pixel, ~900M warps a second, 65% of
the frame — to articulate a rig that is 10/15 finger bones holding a static
grip.

- **The body is one matrix**: non-uniform scale (squish) + shear (lean) + yaw +
  hop, all about the feet. `Renderer::bodyAffine`. The squish comes from a
  spring in `RigParams`, integrated **on the 12 Hz pose grid** — not the frame
  clock, because the squish is a traced uniform and a 60 Hz one would make a
  standing fighter re-trace every frame.
- **There are no clips to sample.** The asset ships none; the walk/idle shape
  is entirely procedural (the spring above). `animPlay`/`evalPose`/`animSpeed`
  survive only for a hypothetical rigged asset and no-op on this one.
- **BOX CLIPPING, NOT OWNERSHIP**, decides which piece draws a region:
  `max(field, boxDist)` against the piece's own rest AABB. The dominant-bone
  ownership test (and the `cellW` field it read) is DELETED — there are no
  bones. An earlier AABB experiment sliced flat bands across the body; that
  was when pieces clipped a shared, OVERLAPPING body region, so the box cut
  through solid clay. The three brushes are now disjoint and ~0.12 m apart, so
  each box contains its own brush entirely and cuts nothing. **Do not
  reinstate ownership.**
- `Piece.aabbLo.w` is the transform's **smallest singular value**, not a min
  column norm: a shear can have unit-length columns and a much smaller
  sigma_min, and trusting the column norm makes the march step through the
  skin. For the two hands it is exactly 1 — a mirror has det -1 but
  `M^T M = I`, so it is distance-preserving and needs no special case.
- **The grip is a discrete brush swap.** Holding the sword selects the grab
  brush, releasing selects the rest brush, latched on the 12 Hz pose grid and
  NEVER interpolated: blending two brushes would reintroduce exactly the
  per-sample blending this rig deletes, and a snap on a pose step is the
  stop-motion idiom.
- **Not carried over from the bone rig**: eye gaze (the eye bones are gone —
  the eyes are fixed beads riding the body affine) and per-digit finger curl
  (`hands.gripCurl`, `hands.gripRoll` are unused; the grab morph replaces
  them). A wound carved into a hand belongs to the POSE it was made in, since
  the two hand poses are separate volume regions.

## Windowed test harness (M5)

`WASD` walks the fighter (camera-relative — forward is always away from the
camera), it turns to face travel and leans into it, squishes and hops on its
procedural spring (there are no clips), and the orbit target follows it. `SPACE`
swings. The sword rests in a VERTICAL guard with a slight bob (quantized to
the 12 Hz pose grid, trap 4); the swing is a three-beat flourish over 0.80 s —
wind up high, flatten to horizontal and sweep across the front at chest
height, then recover to guard. The slow flourish is deliberate: it keeps the
per-frame blade sweep inside what `BrickSystem::kOpsPerFrame` (6) cut substeps
can bridge, so a faster swing would need that budget raised.
`1/2/3` still switch orbit/carve/add.

Headless has no keyboard, so locomotion there is whatever `fighter.pos`,
`fighter.yaw`, `fighter.lean` and `fighter.moving` are set to via ctl/replay.

## Agent dev loop (PREFER THIS over rebuild-relaunch cycles)

A running clayfray — windowed or `--serve` — polls `ctl/in/` every frame.
Drive it with `tools/ctl.sh` (Windows: `tools\ctl.ps1`, same arguments; one
argument per command; response printed):

```sh
./build/clayfray --serve &            # headless sim console, 960x540 aa1
tools/ctl.sh "set look.aoStrength 0.8" "shot lookdev/probe.png"
tools/ctl.sh stats                    # one-line JSON: time/ledger/gobs/fps
tools/ctl.sh "edit carve 0.02 0.42 0.12 0.07"   # scripted sculpt, no mouse
tools/ctl.sh pause "step 3"           # freeze; advance 12 Hz pose steps
tools/ctl.sh "snap save mystate"      # snapshots/mystate.snap (~80 MB)
tools/ctl.sh "snap load mystate"      # exact restore, byte-identical render
tools/ctl.sh quit
```

Commands: `get NAME|*`, `set NAME V..` (any LookParams/sword/cam/brush field,
names = struct paths like `sword.pitch`), `edit carve|add x y z r [rgb [dir
srcRGB]]`, `bake`, `shot PATH`, `stats`, `probe`/`pickuv u v`, `pause`,
`resume`, `step [N]`, `timescale F`, `snap save|load NAME`, `record
PATH|stop`, `break ledger TOL_ML|off`, `quit`.

Iteration rules of thumb:
- **Don't render screenshots to check your own work — build it and hand it
  over.** A `--screenshot`/`--replay` pass costs a startup plus the frame
  run, and a verification sweep of several burns minutes of the user's time
  waiting on you. Finish at `cmake --build build` and say what to look at.
  Render only when asked, or for a gate with no manual equivalent (the
  `--carve-test` conservation exit code, replay determinism) — and say why
  first. For perf work, quote benchmark numbers instead of re-rendering to
  eyeball.
- **A pixel diff cannot see SHAPE.** imgdiff answers "did values move", not
  "does it still look right", and the difference has already shipped a
  regression: a skin-gather change measured 133 changed pixels — inside
  imgdiff's own tolerance — while visibly wrecking the mitts' silhouette at
  the 4-bone junction. Anything touching skinning, the warp, silhouettes or
  the joints is judged by a human in the running app, not by a number.
- Tuning/look/pose/sword work: NEVER rebuild — `set` + `shot` on a live
  instance. Shaders hot-load on save (~30-frame poll), also no rebuild.
- C++ changes: `snap save` first, rebuild, relaunch `--serve --load NAME` —
  back in the exact scene in seconds.
- Benchmarking: **run at the SHIPPING config or the number is fiction.** A
  1280x720 window at `resScale` 0.5 traces **640x360**, which is now the
  headless `--size` default, so a timing run without `--size` measures what the
  game runs. Benchmarking at 1280x720 overstated the frame by ~2.7x (57.8 vs
  21.4 ms). Measure WALL CLOCK per presented frame, differencing two frame
  counts: GPU pass timestamps (`CLAYFRAY_TS`) only fire on TRACED frames, so
  they are meaningless once frame reuse is on, and they misreport small
  dispatches. Always burn a warm-up run first — a shader edit forces a cold
  pipeline compile on the next launch only, and differencing against that
  yields negative ms/frame. The subtler form BURNED A SESSION: when you
  difference a short run against a long one, the compile lands in the SHORT
  run, shrinks the difference, and reports ~13 ms/frame too FAST — a plausible
  looking win, not an obvious error. It fires only for a shader the pipeline
  cache has never seen, so the committed shader measures clean and every
  experiment measures fast. Warm up after EVERY shader edit, and never take
  the min across repeated passes: that picks the contaminated one. Two
  ablations "worth" 16 and 12 ms were worth 3 and 0 once measured warm.
  `CLAYFRAY_NO_REUSE=1` gives the MOVING cost (what governs frame rate
  whenever anything is in motion); without it you get the idle cost.
- Env-var ablations (`CLAYFRAY_NO_PIECES`, `_AO`, `_SHADOWK`) don't touch
  shader source, so they never hit the cold-compile trap. Prefer them for
  cost attribution; reach for a shader edit only to test a fix.
- **Conditional skips do not pay here.** Several "skip the expensive path
  when X" experiments came out image-identical and SLOWER (58.5 -> 59.7,
  60.1, 60.7). A branch only helps if every lane in the wavefront takes it,
  and neighbouring pixels sit in different regions of the body; the skipped
  work still gets executed, plus the test. Removing work for EVERY lane is
  what moves the number. Small edits also swing the frame by ~10% in either
  direction via register pressure — measure, never assume.
- Scenario regression: `record` a journal (or hand-write one: `<poseTick>
  <ctl command>` per line, see `scenarios/`), then
  `./build/clayfray --replay scenarios/X.journal --screenshot out.png
  --frames 240 --size 640x360 --aa 1`. Exit 3 = conservation violation.
  Ledger is run-to-run exact; images match to ~0.02% of pixels (redistance
  apron healing is dispatch-order sensitive) — compare with
  `python3 tools/imgdiff.py a.png b.png`, NOT `cmp`.
- `break ledger 5` arms an auto-pause on ledger imbalance: the app freezes
  for inspection (stats/shot/snap) instead of exiting.

Gotchas: `stats` reports values as of the last completed frame — poll it
twice if you just mutated state. `--serve` steps a fixed 1/60 s per frame
regardless of wall clock (deterministic, GPU-bound ≈ slow motion). Gobs only
SPAWN during rendered frames; a paused carve banks its volume as `debt_ml`
until resume/step. Snapshots are same-build raw memory — don't ship them.

### Extending the dev loop (invariants that keep it flexible)

- New tunable → one `addF/addB/addI` line in `CtlServer::buildRegistry`
  (src/ctl.cpp), name = struct path. Forgetting self-diagnoses as
  "err unknown param".
- New mutating ctl verb → add to `execute()` AND call `record(tick, line)`;
  replay support then comes free (replay runs through the same execute()).
  Query verbs (stats/probe/shot) must NOT record.
- New sim state → new tagged section in the matching save/load pair.
  Additive sections keep old snapshots loadable. If a SERIALIZED struct
  (BrickEdit, Gob, RenderSnapCpu) or a size constant (kGrid/kMaxBricks/kN)
  changes, bump `kVersion` in snapshot.cpp so stale .snap files refuse
  cleanly. Snapshots are scratch; JOURNALS are the durable form of a
  scenario — when a snapshot dies to a struct change, replay the journal
  and re-save.
- New sim systems stay replay-deterministic by: fixed-step dt only, seeded
  RNG only (no wall clock / rand() in sim), and pinning any GPU readback
  that feeds gameplay the way `syncMeasurements` pins the volume ledger.
- Deferred until needed (each ~5 lines): `--ctl-dir` flag for running two
  app instances (ctl/ is hardcoded), per-fighter snapshot tag prefixes when
  a second BrickSystem lands.

## Traps (each has cost a debugging session)

1. **Shaders hot-load; the binary does not.** WGSL is read from `shaders/` at
   runtime, so editing a shader + its C++ side and running the STALE binary
   gives wgpu bind-group / `minBindingSize` validation spew. Always
   `cmake --build build` before diagnosing wgpu errors after a shader change.

2. **The `Uniforms` struct is hand-mirrored in THREE files**: `src/renderer.h`
   (`kUniformSlots`, and the hardcoded slot writes in `packUniforms`),
   `shaders/trace.wgsl`, and `shaders/pick.wgsl`. Change one → change all three.
   A `static_assert` in `packUniforms` catches only C++-side buffer overrun;
   the WGSL side has no compile-time link, so a mismatch shows up as the same
   validation spew as trap 1. When you add a uniform field, append it (don't
   reorder) and bump `kUniformSlots`.

3. **March fields must be CONSERVATIVE; AO/penumbra fields must be SMOOTH.**
   Feeding a conservative (Lipschitz-scaled / clamped) distance into AO or soft
   shadows reads as phantom occlusion and bands. Every field has two variants
   for this reason (`charDist`/`charDistLoose`, `groundClayDist`/
   `groundClaySmooth`). See PLAN.md M2 + M4.6. Don't collapse them.

4. **12 Hz stop-motion is sacred.** Anything that moves visually quantizes its
   DISPLAY position to the pose grid (`frame.poseTime`), even if it simulates
   at frame rate (see gobs in `updateConservation`). Never cache/freeze the
   per-pose-step boil — it IS the claymation life.

5. **Edits near the volume boundary are silently dropped** (`queueEdit` /
   `editInBounds`) to avoid clipped-blob corruption. Carving at the very edge
   of the character therefore no-ops; that's intended, not a bug.

6. **`BrickEdit.pos` is REST space, not world.** The brick volume is authored
   in the character's rest frame, so a world position only addresses it while
   the fighter stands unposed at the origin — which was true until M5 gave it
   a root transform, and is why carving silently stopped the moment he walked
   (out-of-bounds edits are dropped, see trap 5). Anything turning a world
   point into an edit must map back through the articulation first:
   `Renderer::pickRest()` for screen picks (the pick shader returns it via
   `charRestPoint`), or the inverse skin matrix of the bone it hit for gob
   re-sticks. `BrickEdit.worldPos` carries the arena-space wound alongside,
   because gobs must SPAWN in world. `CLAYFRAY_DEBUG_PICK=1` prints both at
   the cursor — if they differ while the fighter is unposed at the origin,
   something is wrong.

7. **The character is THREE BRUSHES selected by node name — and there is no
   armature.** The asset has no skins, no animations and no bones: three
   meshes authored in place (`body`, `hand`, `eye`). `CharacterAsset::load`
   keeps them SEPARATE (it used to merge every mesh bound to the armature)
   and imports the hand TWICE — once as authored, once with its `grab` morph
   target at full weight, translated by `kGrabBrushOffset` into the empty
   negative-x half of the rest volume. Three disjoint regions, one volume, no
   new GPU resources. `eye` is consumed as marble beads via its `marble_*`
   materials and the right eye/hand are MIRRORED in x at runtime, because the
   artist authors one side only.

   The old failure mode (a fighter made of nothing but hands, with no error)
   is still the one to watch for after a re-export. The `asset: brush '...'`
   lines print each brush's triangle count and AABB, and
   `tools/verify_brush_layout.py` re-derives the whole layout from the .glb
   and `src/brick.h` — run it if the artist moves anything. A brush that
   leaves the volume box, or two brushes closer than the narrow band, breaks
   the AABB clip that separates the pieces and both are reported.

8. **Core WebGPU guarantees a stage only 8 storage buffers, and `trace` uses
   7.** Two limits stack here, and the SMALLER one is now the binding budget:

   - **Core WebGPU: 8** (`maxStorageBuffersPerShaderStage`). This is the
     number that matters, because it is what a conformant implementation may
     report at minimum. Desktop Chrome reports the adapter's real (larger)
     limit, so a 9-binding `trace` ran fine there — but a mobile browser
     reporting the guaranteed minimum would fail to create the trace pipeline
     outright, which is the same silent-black-screen failure as below.
   - **Metal: 10.** Metal gives a function 31 buffer slots; Dawn spends one on
     buffer lengths and reserves its default uniform + vertex budget, leaving
     10 — the adapter genuinely reports that, so requesting full limits (which
     `gpu.cpp` already does) buys nothing. M5's second fighter took `trace` to
     14 and the pipeline failed to CREATE on macOS: `CreateComputePipeline`
     errored and every frame after it was invalid, while Vulkan (effectively
     unbounded) showed nothing wrong.

   Two rounds of packing got `trace` from 14 to 7. Each fighter's per-cell
   arrays (indirection, JFA seeds, coarse — there was a fourth, cell weights,
   until the skeleton went) are REGIONS of one `volume` buffer, and the
   ground's base/height/color are REGIONS of one `field` buffer. In both
   cases: write passes bind their own region as a sub-range, which is why the
   write shaders still declare them separately and need no index arithmetic,
   and the tracer binds each buffer once and adds a base index (`CELL_*` from
   `BrickSystem::wgslConstants` with accessors atop `brick_read.wgsl`; `G_*`
   from `GroundClay::wgslConstants` with accessors atop `ground_read.wgsl`).
   Region maps live in `src/brick.h` and `src/ground.h`.

   Budget: **3 per fighter + 1 ground = 7 of 8**, i.e. ONE spare. **Adding a
   storage binding to trace.wgsl costs the last of the mobile headroom** —
   grow an existing region instead. Fighter #3 needs the per-fighter stride in
   PLAN.md ("fighters are SLICES"), not three more bindings.

   Keep every write-side binding of those regions `read_write`: Dawn rejects a
   buffer that is writable and read-only within one pass and tracks that per
   BUFFER, not per bound range — so one `read` binding on any region breaks
   every pass that writes another. (`jfa.wgsl`'s `bDistRO` is declared
   `read_write` purely for this reason.) The tracer's read-only bindings are
   safe only because tracing is a different pass from every write.

## Verifying a conservation (M4.6) change

`--carve-test` now self-checks and **exits nonzero (3) on a conservation
violation** — carved clay must equal landed + in-flight + owed at exit:

```sh
CLAYFRAY_DEBUG_LEDGER=1 ./build/clayfray --carve-test \
  --screenshot out.png --frames 300 --size 960x540 --aa 2
echo $?    # 0 = balanced, 3 = clay leaked
```

The `[sploot] final:` line prints the ledger. `lookdev/sploot_*.png` are the
reference renders — diff against them by eye after a lighting/shading change.

## Debug env vars

| var | effect |
|---|---|
| `CLAYFRAY_DEBUG_LEDGER=1` | print per-op measured volume + final balance |
| `CLAYFRAY_DEBUG_STATS=1` | brick allocation readback + field scan |
| `CLAYFRAY_DEBUG_NORMALS` / `_FLAT` / `_FLATALBEDO` / `_GRAD` | isolation renders (grad = \|∇d\|−1 heatmap) |
| `CLAYFRAY_TS=1` | opt-in GPU timestamps (Metal drops these under load — wall-clock is the Mac tool) |
| `CLAYFRAY_NO_REDIST` / `_NO_ANIM` / `_NO_PIECES` | disable redistance / animation / chunk articulation |
| `CLAYFRAY_AO` / `_DETAIL` / `_SHADOWK` | override look params (float) |
| `CLAYFRAY_DEBUG_PICK=1` | print world vs REST position under the cursor (trap 6) |
| `CLAYFRAY_NO_AFFINE=1` | draw the rest volume UNPOSED (pieces = 0), i.e. all three brushes side by side where they are authored. Answers "is the rig wrong or is the volume wrong?" in one keystroke. It used to select the 13-piece inverse-LBS warp; that path died with the armature, so this is no longer an A/B between two rigs |
| `CLAYFRAY_DEBUG_REUSE=1` | name the input behind every re-trace that happens BETWEEN pose steps (a re-trace ON a pose step is the 12 Hz floor, so it stays quiet — silence means optimal) — a uniform (by slot NAME, e.g. "gobs (flying clay)") or the volume whose generation moved. Frame reuse is what makes motion affordable, so when the `[reuse] traced N of M` line collapses toward 0% skipped, this says which input refuses to settle |
| `CLAYFRAY_TEST_ADDSTRESS` / `_TEST_NULLEDITS` | `--carve-test` variants (pool stress / null-edit JFA) |

## Known defect: repeated readbacks stall (Windows/Vulkan, 2026-08-13)

On this box's Vulkan backend a `MapAsync` on a REUSED readback buffer
succeeds once per process and then never completes again. Consequences:
`shot` fails outright in `--serve`, and `probe`/`pickuv` return the first
pick forever (the freeze is `pickMapPending_` never clearing). The volume
ledger is unaffected — it is the one path that works. Until it is fixed,
verify renders with `--replay <journal> --screenshot`, NOT serve-mode `shot`,
and put any journal command whose pick you care about on the FIRST tick.

## Async readback lifetime

Dawn `MapAsync` callbacks (`AllowSpontaneous`) fire on the main thread during
`gpu.processEvents()` / `WaitAny`, so members they touch aren't racing the
render loop. But a callback can still fire after its owner is destroyed at
teardown — `BrickSystem` and `Renderer` each hold a `shared_ptr<bool> alive_`
that the destructor flips and every callback checks first. New MapAsync
callbacks that capture `this` must capture a copy of `alive_` and early-return
on `!*alive`.
