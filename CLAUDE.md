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
SLICES"). That cap is now enforced by hardware, not taste: a third fighter's
3 bindings would put `trace` at 12 of Metal's 10 (trap 8). ctl exposes
`foe.enabled`, `foe.pos`, `foe.yaw`.

Both fighters bill to ONE conservation ledger: clay off either body becomes
the same gobs on the same arena.

## Windowed test harness (M5)

`WASD` walks the fighter (camera-relative — forward is always away from the
camera), it turns to face travel and leans into it, plays the `bounce` clip
while moving and `idle` at rest, and the orbit target follows it. `SPACE`
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

7. **The character is EVERY mesh bound to the armature, not one node.** The
   fighter is authored as separate Blender objects (blob body + the two
   floating mitts) sharing one skin; `CharacterAsset::load` merges all of
   them. It used to pick a single node, which silently imported whichever
   came last — a fighter made of nothing but hands, with no error. If the
   body vanishes after a re-export, check the vertex count on the `asset:`
   line against the sum of the mesh objects before suspecting the renderer.
   A mesh bound to a SECOND skin is skipped with a warning (joint indices
   would not line up).

8. **A shader stage gets 10 storage buffers on Metal, and `trace` uses 9.**
   Metal gives a function 31 buffer slots; Dawn spends one on buffer lengths
   and reserves its default uniform + vertex budget, leaving 10 — the adapter
   genuinely reports that, so requesting full limits (which `gpu.cpp` already
   does) buys nothing. M5's second fighter took `trace` to 14 bindings and the
   pipeline failed to CREATE on macOS: `CreateComputePipeline` errored and
   every frame after it was invalid, while Vulkan (effectively unbounded)
   showed nothing wrong. So each fighter's four per-cell arrays (indirection,
   JFA seeds, coarse, cell weights) are REGIONS of one `volume` buffer: write
   passes bind their own region as a sub-range, which is why the write shaders
   still declare them separately, and the tracer binds the buffer once and
   adds a base index (`CELL_*` from `wgslConstants`, accessors at the top of
   `brick_read.wgsl`). Budget: 3 per fighter + 3 ground = 9. **Adding a
   storage binding to trace.wgsl breaks macOS** — grow a region, or pack the
   ground trio the same way, instead. Keep every write-side binding of those
   regions `read_write`: Dawn rejects a buffer that is writable and read-only
   within one pass and tracks that per BUFFER, not per bound range.

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
