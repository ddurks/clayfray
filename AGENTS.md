# AGENTS.md — agent onboarding for clayfray

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
`--character file.glb`, `--carve-test` (scripted carve/add exercise),
`--exit-after N` (windowed smoke test), `--serve` (headless ctl session),
`--replay f.journal` (deterministic scenario replay), `--load NAME`
(restore a snapshot at launch).

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
- Tuning/look/pose/sword work: NEVER rebuild — `set` + `shot` on a live
  instance. Shaders hot-load on save (~30-frame poll), also no rebuild.
- C++ changes: `snap save` first, rebuild, relaunch `--serve --load NAME` —
  back in the exact scene in seconds.
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
| `CLAYFRAY_TEST_ADDSTRESS` / `_TEST_NULLEDITS` | `--carve-test` variants (pool stress / null-edit JFA) |

## Async readback lifetime

Dawn `MapAsync` callbacks (`AllowSpontaneous`) fire on the main thread during
`gpu.processEvents()` / `WaitAny`, so members they touch aren't racing the
render loop. But a callback can still fire after its owner is destroyed at
teardown — `BrickSystem` and `Renderer` each hold a `shared_ptr<bool> alive_`
that the destructor flips and every callback checks first. New MapAsync
callbacks that capture `this` must capture a copy of `alive_` and early-return
on `!*alive`.
