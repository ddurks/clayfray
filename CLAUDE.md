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

### Web target (Emscripten) — STAGE 2: it RUNS

```sh
emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release
cmake --build build-web        # -> build-web/clayfray.{html,js,wasm,data}
python3 -m http.server -d build-web 8000   # then open localhost:8000/clayfray.html
```

**It must be SERVED, not opened as a file.** WebGPU requires a secure origin,
so `file://` gives "no adapter" and a black canvas. `localhost` counts as
secure; a bare LAN IP does not.

Dependencies come from Emscripten's own ports (`--use-port=sdl3
--use-port=emdawnwebgpu`), not the FetchContent'd native Dawn — a wasm module
has no native Dawn to link, and emdawnwebgpu is Dawn's browser-facing
implementation. The ports pin their own SDL3/Dawn revisions; the desktop pins
are untouched by that branch and only `__EMSCRIPTEN__` code ever sees the port
headers.

`src/platform.h` is the whole platform boundary, and `CLAYFRAY_DEV_TOOLS` is
the switch. Web drops: the ctl server, snapshots, shader hot reload, journal
record/replay, screenshots, `debugStats`/`debugScanField`, and every headless
mode. Web keeps: the renderer, the sim, the ImGui panel (imgui's WebGPU
backend speaks emdawnwebgpu — keep `IMGUI_IMPL_WEBGPU_BACKEND_DAWN`, the
`_WGPU` spelling is wgpu-native), and the runtime reads of `shaders/*.wgsl`
and `assets/fighter.glb`. Those reads are UNCHANGED: the linker preloads both
into MEMFS at the same paths, cwd is `/`, and only `CLAYFRAY_SHADER_DIR`
differs (`/shaders`, since the source tree's absolute path means nothing
inside the module). A NEW asset needs a `--preload-file` line in CMakeLists or
it is silently missing on web.

**Startup is inverted on web, and that is the whole shape of stage 2.** Two
things had to stop being straight-line code:

- `Gpu::init` blocked on the adapter and device futures. It is now split into
  stages (`createInstance` / `requestAdapter` / `reportAdapter` /
  `requestDevice` / `finishInit`) that BOTH platforms call in the same order.
  Native drives them with `gpuBlockOn` exactly as before; web drives them with
  `AllowSpontaneous` continuations via `Gpu::initAsync`. Native's `initAsync`
  just calls `init` and invokes the callback immediately, so `runWindowed` has
  ONE startup shape on both targets.
- `runWindowed` owned a `while` loop. The body is now `frameOnce(AppState&)`
  over a heap-allocated `AppState`; native keeps its `while`, web hands
  `frameOnce` to `emscripten_set_main_loop_arg` (fps 0 = rAF) and RETURNS.
  Falling off the end of `main()` on web is correct — the registered callback
  keeps the module alive. `AppState` is heap-allocated because `CtlRefs`
  stores raw pointers into it and the browser callback holds a `void*`.

Everything after the device callback (renderer init, asset load, uiInit) lives
in `appStartAfterGpu`, because on web it runs a browser task or two after
`runWindowed` has already returned.

Canvas sizing: the window is created at the `#canvas` CSS size, not 1280x720,
and re-polled every 10 frames against the last size we set (never against the
current window size — comparing those oscillates). DPR is REPORTED in the
`[res]` line but deliberately not multiplied in, matching the desktop's
no-`HIGH_PIXEL_DENSITY` choice; `--res`/`look.traceW/traceH` still win.

Headless flags: `--screenshot PATH --size WxH --frames N --time T --aa N`,
`--cam AZ,EL,DIST` (inspect from another angle without serve mode),
`--character file.glb`, `--carve-test` (scripted carve/add exercise),
`--exit-after N` (windowed smoke test), `--serve` (headless ctl session),
`--replay f.journal` (deterministic scenario replay), `--load NAME`
(restore a snapshot at launch).

## Players — fighters are SLICES (M5, sliced M-SLICE)

`Renderer::addPlayer(pose)` adds a fighter and returns its index; player 0 is
the hero. **The cap is `BrickSystem::kMaxFighters`, currently 2** — and it is
now a MEMORY choice rather than a shader one. The slice machinery is sized by
that constant and has been exercised at 4; it sits at 2 because every slice is
allocated up front whether or not a fighter is live, so 4 costs 157 MB of GPU
buffers against 104 MB, and the web target ships. Going to 4 is that one line
PLUS dropping `kMaxBricks` to 12288 (its comment has the table; a
`static_assert` catches you if you forget).

Every fighter is a SLICE of one `BrickStore`: one `volume`, one `distPool`,
one `albedoPool`, each cut into `kMaxFighters` partitions. So **the tracer
costs 3 brick bindings + 1 ground = 4 of 8 AT ANY N** (it was 3N+1, which hit
the wall at two — see trap 8). Raising the cap costs pool memory and uniform
slots, nothing else.

The split that makes it work, and the reason the write shaders were untouched:

- **Write side binds SUB-RANGES.** Each `BrickSystem` binds its own slice as a
  bind-group offset, so `edit.wgsl` / `voxelize.wgsl` / `jfa.wgsl` /
  `redistance.wgsl` still index from zero and `MAX_BRICKS` still means "this
  fighter's pool". Miss an offset in `brick.cpp` and a fighter silently edits
  its neighbour's clay — every one goes through `volBase()`/`distBase()`/
  `albBase()`.
- **Read side adds a base.** The tracer binds the buffers whole, so
  `brick_read.wgsl`'s accessors add `gCellBase`/`gDistBase`/`gAlbBase`, set by
  `usePlayer(f)`. That replaced a per-accessor `if (gFighter == 0u)` branch,
  which at four bodies would have been a four-way chain in the hottest code in
  the frame.
- **Pools are PARTITIONED, not pooled.** Indirection words carry brick indices
  local to the fighter's partition, so one fighter's carving can never starve
  or corrupt another's — and `save()`'s dense-prefix format still works.
- **Materials carry the index**: fighter f is `MAT_BODY + 0.1*f` (3.0…3.3, all
  under the 3.5 clay cutoff so every `m < 3.5` predicate still classifies every
  fighter as clay). `pick.wgsl` returns it in `pickOut[3].w`, which is what
  lets the mouse sculpt any body.

Per-fighter renderer state lives in ONE `Renderer::Fighter` struct in an array.
Every field there used to exist twice as a `foo_`/`foeFoo_` pair — that
duplication *was* the cap, since a third body meant a third copy of a dozen
members plus a third `else if` at each use.

**A rest-space point is meaningless without its fighter.** The same
coordinates name different clay in every slice, so `BrickEdit::player` is
explicit (default 0 = the hero, which is what every journal and `--carve-test`
assumes). Routing an edit off the live pick instead would silently redirect
every SCRIPTED edit to whatever is under the cursor.

ctl exposes `p1.*` … `p<kMaxPlayers-1>.*` (`enabled`/`pos`/`yaw`/`lean`/
`moving`) — the registry is built by a loop, so raising the cap adds the names
automatically. `foe.*` is kept as an alias for player 1 so recorded journals
still replay. The `edit` verb takes an optional trailing player index.

EVERY fighter bills to ONE conservation ledger: clay off any body becomes the
same gobs on the same arena.

**The trace reject sphere is a SHADING input, not just an early-out.**
`charLooseAffine` RETURNS `length(p - gFarCenter) - gFarR` for points outside
it, and that value feeds AO and the penumbra term — so moving a fighter's
bound repaints shading on surfaces it is nowhere near, including ones where
the fighter is off-screen entirely. Measured at 640x360 aa2 vs origin/main:
deriving the hero's centre from the transformed rest centre instead of the
posed capsule average moved **38,295 px, max delta 59**. Every fighter now
uses the capsule average (the hero's derivation), which makes the hero **4 px,
max delta 1**. If you touch `packUniforms`' centre/radius, re-run that A/B.

**Not done:** snapshots still save player 0's volume only (the format is
per-fighter-clean, so it is a section-naming job, not a format one); the
capsule shadow proxy in `mapPenumbra` is still player 0's alone (generalising
it would change the hero's penumbra); and the opponent's reject sphere moved
onto the hero's derivation, which is the whole residual against origin/main
(~4% of pixels, max 17, almost all ±1 LSB of AO on ground near it).

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
- **The grip is a discrete brush swap.** There are FOUR hand poses — `rest`
  (as authored), `grab`, `idle`, `fist` — each its own brush in its own region
  of the volume, selected whole and latched on the 12 Hz pose grid, NEVER
  interpolated: blending two brushes would reintroduce exactly the per-sample
  blending this rig deletes, and a snap on a pose step is the stop-motion
  idiom. Selection is one predicate in `Renderer::render`: sword in hand →
  `grab`, squaring up → `fist`, otherwise → `idle`. `rest` is now only the
  canonical frame and the fallback for an asset shipping fewer shape keys.
- **Not carried over from the bone rig**: eye gaze as a bone rotation (the
  eye bones are gone — the eyes are beads riding the body affine, and gaze
  spins each pupil about its own eyeball) and per-digit finger curl
  (`hands.gripCurl` is DELETED — it had to be zero under the affine rig, so
  under the only rig there is it was a slider multiplied out to nothing; the
  grab brush is how a grip shape changes now). `hands.gripRoll` IS live: it
  spins the whole mitt about the blade. A wound carved into a hand belongs to
  the POSE it was made in, since the two hand poses are separate volume
  regions.

## Fists, idle hands, and the opponent AI (M-FIST)

Three things landed together because they are one feature: the mitts got
somewhere to be when they are not holding the sword, and something to do there.

**Hands are PLACED now, not left where the artist modelled them.** The old
no-sword path was `xform = A * Mirror` with the brush's authored offset intact,
i.e. "the mitt rides the body wherever it happens to sit". That was fine while
"unarmed" meant "the sword is switched off"; it is not a pose. Both branches of
`updateBrushRig` now centre the PALM and differ only in where they put it —
`unarmedHand()` returns a character-space palm position plus a rotation, the
mirror sits between it and the body affine, so the right mitt is the exact
reflection (position, droop and knuckle roll flip together). Everything about
that placement is a judgement call you make by looking, so it lives in ctl:

```sh
tools/ctl.sh "set handpose.idlePos 0.28 0.29 0.04" "set handpose.fistYaw -0.95"
```

Two numbers to know before dialling them: **the body is x ±0.21, z ±0.21,
y 0..0.69, and the palm is the mitt's CENTRE**, so a placement much inside those
bounds buries the hand in the torso. A little overlap is wanted — it is what
makes a detached blob read as attached rather than as floating nearby.

**The bob is the sword's bob, moved.** It used to exist only in
`GameState::swordOffset`, riding the hilt, so putting the sword down killed the
only motion the hands had. `HandPoseParams` now carries the same expression
(lift on `sin(rate)`, roll on `sin(rate/2)`, same constants, same 12 Hz clock)
and applies it to any unarmed mitt. Both mitts bob IN PHASE, because the sword
bobs one hilt and two hands ride it — a phase offset would make putting the
sword down change how the fighter idles.

**A punch is the sword slice one weapon down**, and `updatePunchCut` is
deliberately `updateBladeCut`'s shape. The differences are all consequences of
a fist being a ball rather than a blade: the swept volume of a ball IS a
capsule, so one edit covers the frame's whole motion and there is no substep
loop; the hit test inflates the target's capsules by the fist radius, because
what has to overlap is two solids; and there is no reach test, because the rig
already applied one. Both weapons set `BrickEdit::fromWeapon` (was `fromBlade`
— the pooling was never blade-specific, only the caller was), so a punch ejects
ONE chunk on the same ledger path a slice does.

**THE FIST'S WORLD POSITION IS NOT THE PIECE'S TRANSLATION COLUMN.** That is
the tempting one-liner and it is wrong: `xform = M * T(pre)`, so its
translation is `M` applied to `pre` — the world position of the brush-space
ORIGIN, which is most of a metre from the palm. It shipped once and the symptom
was punches landing visibly high on the target while the dents appeared low on
its body, with a ledger that cheerfully reported the volume. Run `palm + ofs`
through the piece instead. `CLAYFRAY_DEBUG_PUNCH=1` prints the fist position
and sweep per frame, which is the only way to tell that failure from the three
others that also read as 0.0 ml (wrong brush, too slow, edit out of bounds).

**The reach ball is real now.** `HandParams::reach` claimed the mitts were held
to an armless body by a max distance and nothing enforced it under the brush
rig — `autoReach_` was derived from bones and stayed 0. It is now the rest
distance from the body box's centre to the authored palm (0.566 m on this
asset, printed in the `rig:` banner), and `unarmedHand` clamps to it. So a
punch cannot out-reach a sword thrust: same ball, same radius.

**The opponent AI (`AiParams`, `OpponentAi` in main.cpp) runs in EVERY path,
headless included.** It is deterministic by the house rules — fixed 60 Hz tick,
seeded RNG, no wall clock — so `--replay` reproduces a brawl exactly, and the
sword bob is the precedent: a headless render that disagrees with the running
game is what look-dev and the GIF read from. It wanders, locks on inside
`lockRange`, closes to `standoff`, jabs inside `strikeRange`, and gives up
outside `breakRange`.

It is **slower than the hero on purpose**: `chaseSpeed` 0.92 against the hero's
1.1 m/s, with lower `accel` and `turnRate` too, so the player can always break
away by running and corners tighter. An opponent matching the hero's top speed
is glued to their back and there is no way out of a fight. Spawn spots are
likewise all OUTSIDE `lockRange` — inside it, an opponent was locked on at
frame zero, walked straight out of the dark and started punching before the
player touched a key, so nobody ever saw it wander or saw the idle hand pose.

A journal that places opponents by hand must turn it off first, or the AI walks
them off the marks it set:

```sh
tools/ctl.sh "set ai.enabled 0" "set p1.pos -0.95 0 0.35" "set p1.guard 1"
```

`scenarios/walk.journal` and `res-probe.journal` do exactly that — not for
repeatability (it is deterministic either way) but to isolate what they
measure. `carve-duel.journal` leaves it ON, so the conservation gate now
exercises the punch path.

## Physics (M-PHYS) — resistance, knockback, bodies

Three mechanics, one struct (`PhysicsParams`), and no solver. A fighter is a
point with a velocity on a plane, so everything here is a force, a position
constraint, or a rate multiplier on a strike's own clock.

**THE INPUT IS THE CAPSULE TEST, NOT THE CARVED VOLUME.** Resistance scales
with how much weapon is inside a body, and the tempting source is the measured
carved volume — it is right there in the ledger and it is the physically honest
quantity. It must not be used. That volume is a GPU readback landing one or two
frames late, whose arrival frame is pinned only under `--replay`'s
`syncMeasurements`, and blocking for it is illegal on web (trap 9). Feeding it
into the sim would make how a swing FEELS depend on GPU scheduling, and make a
replay disagree with the run that recorded it. The capsule hit test both cut
paths already run is exact, free, and one frame earlier.

So `Renderer::StrikeContact` is a **pure report**: the renderer resolves where
weapons are (it owns the piece transforms and the posed capsules) and says how
deep they are; it does not act on it. Knockback, hitstun and resistance are
gameplay, and gameplay is the deterministic 60 Hz tick in main.cpp. The report
is rebuilt every frame and read once, on the next — a one-frame lag by
construction, invisible at 60 Hz.

- **`bite` means different things per weapon on purpose.** Blade: metres of
  BLADE buried (a sword half through a body is fighting far more than one that
  nicked it). Fist: how far the fist is past the surface — *not* the swept span
  `(tOut - tIn)`, which is how far it travelled while inside and is near zero
  exactly when resistance should be highest. Each has its own drag coefficient,
  so they never have to be the same quantity.
- **Contact is reported per (attacker, target, weapon), deepest wins.** A blade
  emits up to `kOpsPerFrame` capsule carves as it sweeps, and six separate
  shoves off one swing would multiply knockback by the substep count.
- **Reported BEFORE the bounds test**, in both paths. The weapon is physically
  inside the body whether or not the carve was accepted, and an edit dropped at
  the volume boundary (trap 5) must not silently drop the knockback too.
- **Resistance is a rate multiplier on the strike's own clock**
  (`swingT`/`punchT`), `1 / (1 + drag * bite)` floored at `minRate`. Every beat
  of the flourish and the whole punch curve read off that clock, so a buried
  blade visibly labours and then snaps back to speed as it exits — no extra
  animation, no state. The floor is not just a stall guard: `updateBladeCut`
  stops cutting below a minimum sweep, so a swing dragged far enough would stop
  cutting, lose contact, speed up, cut again, and buzz.
- **Knockback is a FORCE, not an impulse.** Contact persists across frames
  while a weapon ploughs through, so it integrates over the strike; an impulse
  per frame would scale the shove with the frame rate.
- **Knock velocity is SEPARATE from locomotion velocity**, summed only at
  integration. Folding a shove into `vel` puts it in the path of the
  accel-toward-desired model that drives walking, and at `kAccel` 7 m/s² a
  2 m/s knock is cancelled by the victim's own steering in under a third of a
  second — the hit reads as nothing happening. Kept apart, it decays on its own
  LINEAR schedule (an exponential one asymptotes and never quite stops, which
  is how a fighter drifts out of the arena with nothing touching it).
- **Hitstun** (`stagger`) is the one place the game takes the controls away, so
  it is deliberately brief. Without it the shove is cancelled by the victim's
  own accel and there is no hit to feel.

**Bodies do not interpenetrate** — a pairwise xz position constraint, run after
every fighter has integrated. That ordering is why `stepWorld` exists: bodies
used to integrate and confine themselves inside their own `tick()`, and there
was no moment when every fighter had moved but none had been corrected.
Collision is a *relation*, not a property, and it cannot be written at all
while each fighter's velocity is private to whatever drives it — which is why
`GameState::vel` and `OpponentAi::vel` collapsed into one `Body bodies[]`.
Correction is symmetric (half each); pushing only the "second" body would make
collision depend on player index and let the hero bulldoze.

## Death and respawn (M-DEATH)

A fighter that has lost `death.threshold` (0.5) of its clay comes apart.

**The threshold is measured against the CARVED LEDGER, not a hit-point pool.**
Damage is literally the clay that left your body — the same measurements
`absorbMeasured` bills to the arena ledger, billed per fighter at the same
time. There is no second health model that could drift out of step, and a
re-stick *heals*: clay that sticks back on subtracts from the damage, or a
fighter could be visibly whole and still drop dead.

The denominator is the **mesh volume** of its brushes (body + two mitts),
computed by the divergence theorem at import and printed per brush in the
`asset:` banner. Voxel occupancy is the more honest number and is unavailable:
it needs a blocking readback (`debugStats`, dev-tools only). The two agree
closely because the voxelizer is a watertight parity fill — measured 85,744 ml
for the body, matching an independent check on the .glb to the millilitre.

**A fighter is ~91 litres, so 50% is ~46 litres — about twenty connecting
punches.** That is the spec, and it is a long fight; `death.threshold` is the
knob if you want it shorter.

**The collapse cannot use the dribble spawner.** It moves at most twelve 35 ml
gobs per pose step, so 46 litres would leak as a thin stream for FIFTEEN
SECONDS. Same reasoning as the sword's slice gob one scale up — what comes off
has to match what was removed. So the mass splits: a burst of chunky gobs that
fly, and the rest deposited straight into the ground field as a heap where the
body stood, spread over enough splats that none stacks a spike (`splat()`
clamps its radius at 0.28 m, past which volume stops widening and starts
building a tower taller than the fighter was). Anything that will not fit falls
back to debt and the dribble drains it — slow, never lost.

**Conservation holds across all of it**, and `scenarios/death.journal` is the
gate: the remaining mass is added to `carved` and `debt` in the same breath and
every line after only *moves* that debt into gobs and splats. Respawn then
re-imports a whole body, which does put new clay in the world — the ledger's
invariant is that CARVED clay is conserved, not that total scene mass is.

**The eyes outlive the body.** On collapse the four beads stop riding the
corpse and become `loose_` marbles that fall, bounce and roll — the only rigid
bodies in the game. They take the uniform slots the corpse just gave up (four
freed, four spawned), so `kMaxMarbles` is unchanged and trap 2's hand-mirrored
layout never moves. They are deliberately excluded from the gaze pass, which
pairs a pupil to an eyeball by proximity: a bead on the floor has no eyeball to
look out of, and leaving it in the search lets a live pupil pick a corpse's eye
as its partner.

**`dead` is not `enabled`.** `playerEnabled` means "this slot is in play at
all" and is permanently true for the hero — who has to be able to die.
`playerAlive` is the predicate gameplay wants. A respawn is a teleport decided
in the renderer (it owns the ledger that triggers it) but every fighter's
DRIVER lives in main.cpp, so `takeRespawn()` is a one-shot the sim drains: it
adopts the new pose (GameState's copy of the hero is authoritative and
`setFighter` would overwrite the renderer's every frame) and drops the old
velocity, or the fighter arrives still running the direction it died going.

## There is no wall

The arena used to end at a pebble-mosaic backdrop plane at `z = -2.3`
(`arenaWall`, gone from scene_common.wgsl). It did what a lit backdrop always
does: gave the scene a visible edge and caught enough key light to read as a
surface rather than as depth.

Nothing replaced it. The floor is an infinite plane and the key is a close
point light with quadratic falloff, so the ground simply runs out of light and
meets the near-black `background()` with no seam, corner or texture to
recognise. `MAT_WALL`'s id (2.0) is left unused rather than reclaimed — every
material test downstream is a `<` against a threshold, so renumbering to close
the gap would move boundaries the rest of `shade()` is written around.

The only wall left is invisible: `MotionParams::arenaRadius` (2.6 m) clamps
every fighter's position and kills the outward part of its velocity, so a body
SLIDES along the boundary instead of grinding into it. It sits where the light
has already gone, which is what makes it invisible rather than a barrier you
can watch yourself bump into. `set motion.arenaRadius 0` removes it; the light
will not follow you out there.

## The look-dev panel is deliberately small

ImGui carries knobs whose right value is still a judgement call you make by
looking. Anything settled has been cut — the mitt alignment (reach, palm,
orient, grip axis/roll/spread), the sword's placement (hilt pos/yaw/pitch/
length/radius/glow, all driven by the harness's guard-and-swing pose so
dragging them fought the animation), the clip play/speed controls for clips
this asset does not have, and the camera sliders that drag-orbit does better.

**Nothing was lost.** ctl still exposes every `LookParams` field by struct
path, which is the better home for a value set once and never touched:

```sh
tools/ctl.sh "set hands.gripRoll 0.4" "set sword.pitch 0.35"
```

Add a knob back to the panel when you are actively tuning it, not because it
exists.

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

**`SPACE` throws a JAB instead when the sword is away** (`set sword.enabled 0`).
The same key, because the sword's own state decides which it is — there is no
separate mode to keep in sync — and the hero's fists come up the moment the
sword goes down. The punch arc is NOT quantized to the pose grid, and neither
is the sword's swing (only its idle bob is): a strike is carved as a swept
capsule between consecutive frames, so a display that jumped 12 times a second
would cut in stripes.

Headless has no keyboard, so the HERO's locomotion there is whatever
`fighter.pos`, `fighter.yaw`, `fighter.lean`, `fighter.moving` — and now
`fighter.guard` / `fighter.punch` — are set to via ctl/replay. Opponents are
different: the AI drives them in headless too (see M-FIST above), so a journal
that wants them still has to `set ai.enabled 0`.

### Touch controls (`src/touch.cpp`) — the mobile web hands

A phone has no WASD and no SPACE, so the web build grew the same two inputs as
an overlay: a dynamic left-thumb stick and one `SWING` button bottom-right.
`CLAYFRAY_TOUCH=1` forces them on for desktop layout work, `=0` off; otherwise
web probes `maxTouchPoints > 0 && (pointer: coarse)` at startup (a plain
`maxTouchPoints` check fires on every convertible laptop) and ANY platform
latches them on the first finger event.

**The stick is real touch; the camera is still synthetic mouse.** SDL3
synthesises mouse events from the primary finger, which is what already makes
drag-orbit and the whole ImGui panel work under a thumb for free — so
`touch.cpp` claims fingers only inside its two zones and `engaged()` tells
`frameOnce` to ignore the synthetic mouse for orbit and sculpt while a control
is held. Reimplementing orbit on finger events would have thrown that away.
The price, and it is deliberate: **you cannot orbit while walking**, because
SDL synthesises from the primary finger only and a second finger elsewhere
produces no mouse motion at all.

Two more things that are load-bearing rather than decoration:

- **Layout is in ImGui screen points**, converted from the normalized finger
  coords at the event boundary, and sized off the SHORT window edge. Hit test
  and overlay therefore cannot disagree about where the button is, on any DPR
  or in either orientation.
- **The panel wins where it overlaps.** `uiPanelRect()` reports the look-dev
  window's rect and both zones yield inside it, so a slider under a thumb moves
  instead of walking the fighter. `uiSetCompact(touch.active)` then starts the
  panel COLLAPSED on touch devices, because open it is most of a phone screen
  and sits exactly where the left thumb wants to be.

`web/shell.html` is the other half: `touch-action: none` +
`overscroll-behavior: none` (a thumb on the stick would otherwise rubber-band
the page and a two-finger drag would pinch-zoom the app), `100dvh` (plain
`100vh` is the viewport with the URL bar HIDDEN, so the bottom of the canvas —
where both controls live — hides under browser chrome), and the log toggle
moved to the TOP right because a DOM element over the canvas eats the finger
before SDL sees it, and bottom-right is the swing button.

Movement is direction-only, not analog: `GameState::tick` normalizes the move
vector to `kMaxSpeed` anyway, so the thumb and WASD produce literally the same
input. Analog walk speed is a `GameState` change, not a touch one.

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
names = struct paths like `sword.pitch`; plus `handpose.*` for the unarmed mitt
placement and its bob, `ai.*` for opponent behaviour, `phys.*` for knockback /
resistance / body collision, `death.*` for the collapse threshold and how the
mass leaves, `motion.arenaRadius` for the invisible wall, and `p1.*`… per
opponent, now including `guard`/`punch`/`punchSide`),
`edit carve|add x y z r [rgb [dir srcRGB [player]]]`, `bake`, `shot PATH`,
`stats`, `probe`/`pickuv u v`, `pause`, `resume`, `step [N]`, `timescale F`,
`snap save|load NAME`, `record PATH|stop`, `break ledger TOL_ML|off`, `quit`.

Placing and carving an opponent in ITS OWN slice (the trailing `1` is the
player index; drop it and the edit means the hero, which is what every
existing journal assumes):

```sh
tools/ctl.sh "set p1.pos -0.95 0 0.35" "set p1.yaw 1.4"
tools/ctl.sh "edit carve 0.02 0.45 0.14 0.085 0.72 0.45 0.4 0 0 1 .15 .4 .45 1"
```

Every slice is imported at startup, so enabling a player is instant — but
`playerCount()` only counts the ones `addPlayer()` made, and only `main.cpp`
calls that (once, for player 1).

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
- Env-var ablations don't touch shader source, so they never hit the
  cold-compile trap — but **know which ones remove WORK and which only remove
  the LOOK**, because the difference reads as "this term is free":
  - `CLAYFRAY_AO` / `_DETAIL` scale the RESULT, not the cost. `calcAO` runs
    all five `mapLoose` taps whatever `u.ambient.w` is, and the grain
    evaluates its noise gradients before multiplying by `u.material.x`. They
    measure ~0% and tell you nothing about price.
  - `CLAYFRAY_DEBUG_NORMALS=1` DOES: it skips `shade()` entirely, so
    base-minus-this is the whole shading bill (AO + soft shadow + albedo +
    lighting + grain).
  - `CLAYFRAY_NO_PIECES=1` is not a clean articulation ablation either — it
    draws the rest volume UNPOSED, i.e. three brushes side by side, which is
    more clay on screen. It measures SLOWER than posed.
  - `CLAYFRAY_SHADOWK` only moves `softShadow`'s early-out; at k=64 it was
    within noise, so it will not split AO from shadows. That split needs a
    shader edit.
  These are applied by `applyLookEnv()` and reach BOTH the windowed and
  headless paths. They used to be read inside `runHeadless` only, which made
  every ablation silently a no-op under `--exit-after` — the very flag
  `tools/fairbench.sh` drives.
- **THIS MACHINE THROTTLES, AND IT IS BIGGER THAN ANY EFFECT YOU ARE HUNTING.**
  Measured across one session: an identical `moving` baseline drifted 22.0 ->
  23.5 -> 23.6 -> 30.2 ms, and inside a single run the samples went 21.6 ->
  31.0. A 40 s cooldown was not enough to recover. So: never compare a number
  from one invocation against a number from another, quote absolutes only
  from a cold first run, and treat the WITHIN-RUN interleaved delta as the
  only trustworthy figure. `tools/fairbench.sh` exists for exactly this.
- **Where the frame actually goes** (M2, 640x360, `--res 640x360`, cold
  machine, windowed loop). Idle vs moving differ only by the trace, so the
  reuse ratio splits them: `idle = R + 0.2T`, `moving = R + T`.

  | | ms | share of a moving frame |
  |---|---|---|
  | moving (every frame traces) | **22.0** (45 fps) | |
  | idle (reuse on, 80% skipped) | 9.3 (108 fps) | |
  | -> trace `T` | 15.9 | 72% |
  | ---- of which `shade()` | ~6.0 | 27% |
  | ---- of which march + normals | ~9.9 | 45% |
  | -> everything else `R` | 6.1 | 28% |

  `R` is post + blit + ImGui + present + sim + CPU, and 6.1 ms of it at this
  resolution is the surprise — it is a bigger line item than shading.
  **The march is the single biggest cost**, not the shadows: the old note in
  trace.wgsl calling `softShadow` "the single most expensive term at 25.9 of
  70 ms" predates the affine rig, which deleted the per-sample skinning that
  made every field evaluation expensive. Articulation is now free.

  Idle's 108 fps is not 108 images: the pose grid is 12 Hz, so a still camera
  produces 12 unique frames a second and ~96 duplicates.
- **Conditional skips do not pay here.** Several "skip the expensive path
  when X" experiments came out image-identical and SLOWER (58.5 -> 59.7,
  60.1, 60.7). A branch only helps if every lane in the wavefront takes it,
  and neighbouring pixels sit in different regions of the body; the skipped
  work still gets executed, plus the test. Removing work for EVERY lane is
  what moves the number. Small edits also swing the frame by ~10% in either
  direction via register pressure — measure, never assume.
- `scenarios/death.journal` is the M-DEATH gate: it drops `death.threshold` to
  3% (at the shipping 50% a fighter is ~46 litres and it would be a 30-second
  journal), carves one body past it, and exits 3 if the collapse leaks clay.
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
   (`kUniformSlots` and the `kSlot*` layout constants `packUniforms` writes
   through), `shaders/trace.wgsl`, and `shaders/pick.wgsl`. Change one → change
   all three. A `static_assert` in `packUniforms` catches only C++-side buffer
   overrun; the WGSL side has no compile-time link, so a mismatch shows up as
   the same validation spew as trap 1. When you add a uniform field, append it
   (don't reorder).

   **Any array sized by a C++ constant must be DERIVED in the WGSL, never
   retyped as a literal.** This has already bitten once, and it is nastier
   than it looks. `marbles` was `array<vec4f, 32>` in both shaders while the
   C++ computed `4 * kMaxPlayers` beads — which agreed *by coincidence* at
   `kMaxPlayers = 4`. Dropping the cap to 2 made the CPU buffer 2736 B against
   a shader `minBindingSize` of 2992, and the failure is not local: **every**
   bind group against that layout fails to create, so the app boots, imports,
   prints a healthy asset banner, renders a black screen, and carves 0.0 ml
   while `--carve-test` still exits 0 (0 == 0 balances). The fix is to size it
   off the generated block — `array<vec4f, MAX_FIGHTERS * 8>` — which is why
   `wgslConstants()` emits `MAX_FIGHTERS` and `PIECES_PER_FIGHTER` and why
   `//#constants` sits ABOVE the struct in both roots.

   So after ANY change to the uniform block or to `kMaxFighters`, grep the run
   for validation spew rather than trusting an exit code:

   ```sh
   ./build/clayfray --screenshot /tmp/x.png --frames 4 --size 320x180 --aa 1 \
     2>&1 | grep -c 'wgpu error'      # must print 0
   ```

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

7. **The character is FIVE BRUSHES selected by node name — and there is no
   armature.** The asset has no skins, no animations and no bones: three
   meshes authored in place (`body`, `hand`, `eye`). `CharacterAsset::load`
   keeps them SEPARATE (it used to merge every mesh bound to the armature)
   and imports the hand FOUR TIMES — once as authored (`rest`), then once per
   shape key (`grab`, `idle`, `fist`) at full weight, each translated by
   `kHandBrushOffset[pose]` into a part of the rest volume the body never
   uses. Five disjoint regions, one volume, no new GPU resources. `eye` is
   consumed as marble beads via its `marble_*` materials and the right
   eye/hand are MIRRORED in x at runtime, because the artist authors one side
   only.

   **Shape keys are matched BY NAME**, not by index — glTF carries them in the
   mesh's `extras.targetNames`, cgltf parses that, and both `asset.cpp` and
   `verify_brush_layout.py` key on it. Reordering shape keys in Blender would
   otherwise silently swap two poses, which renders as a fighter gripping its
   hilt with an open palm and logs nothing at all. A key the asset does not
   have is imported as NOTHING and `handPart()` falls back to `rest`, so an
   older .glb still rigs — it just has fewer distinct grips.

   The old failure mode (a fighter made of nothing but hands, with no error)
   is still the one to watch for after a re-export. The `asset: brush '...'`
   lines print each brush's triangle count and AABB, and
   `tools/verify_brush_layout.py` re-derives the whole layout from the .glb
   and `src/brick.h` — **run it after adding a shape key**, not just after
   moving something: a new pose lands in a region nothing else validated. A
   brush that leaves the volume box, or two brushes closer than the narrow
   band, breaks the AABB clip that separates the pieces and both are reported.

   Adding a brush is not free anywhere else either. It is a whole extra mesh
   through the voxelizer's triangle binner, so it moves the number trap 10 is
   about: the two fist/idle brushes took `import:` from 8.0M to 11.0M refs and
   the web startup transient from ~70 to ~95 MiB, straight past the old
   `-sINITIAL_MEMORY`. Re-read the `import:` line and re-check CMakeLists.

8. **Core WebGPU guarantees a stage only 8 storage buffers, and `trace` uses
   4.** Two limits stack here, and the SMALLER one is the binding budget:

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

   THREE rounds of packing got `trace` from 14 to 4, and all three use the
   same trick — **write passes bind a SUB-RANGE, the tracer binds the whole
   buffer and adds a base index** — so no write shader has ever needed index
   arithmetic for any of them:

   1. Each fighter's per-cell arrays (indirection, JFA seeds, coarse — there
      was a fourth, cell weights, until the skeleton went) are REGIONS of one
      `volume` buffer. `CELL_*` from `BrickSystem::wgslConstants`, accessors
      atop `brick_read.wgsl`, region map in `src/brick.h`.
   2. The ground's base/height/colour are REGIONS of one `field` buffer.
      `G_*` from `GroundClay::wgslConstants`, accessors atop
      `ground_read.wgsl`, region map in `src/ground.h`.
   3. Every FIGHTER is a slice of those same three brick buffers, at stride
      `VOL_STRIDE` / `MAX_BRICKS`. This is the one that removes N from the
      equation entirely.

   Budget: **3 for all fighters + 1 ground = 4 of 8**, i.e. FOUR spare, and it
   does not grow with the player count. It was 3N+1, which is why M5's second
   fighter (7 of 8) left no room for a third. Prefer growing an existing
   region anyway — a new binding is a permanent tax on every future body.

   **Count per ENTRY POINT, not per module — and `voxelize.wgsl` is why.** It
   declares ELEVEN `var<storage>` across 3 groups, which looks like an
   instant violation and is not. Every pipeline here uses an AUTO layout
   (`desc.layout` left unset, then `GetBindGroupLayout(n)`), so Dawn derives
   the layout from what that entry point STATICALLY REACHES through its call
   graph. The full audit, cross-validated against the bind groups the C++
   actually builds:

   | entry point | reachable storage bindings |
   |---|---|
   | `voxelize.wgsl` `meshFill` | **8 of 8 — AT THE LIMIT** |
   | `trace.wgsl` `cs` | 7 |
   | `pick.wgsl` `cs` | 7 |
   | `voxelize.wgsl` `meshClassify` | 6 |
   | `redistance.wgsl` `apron` | 5 |
   | everything else | ≤ 5 |

   So the tree is portable to a core-minimum device — but `meshFill` has
   **ZERO spare**, which was never written down and is the tighter constraint
   than trace's one. `brick.cpp` already depends on this ("classify's auto
   layout skips bindings it doesn't touch; fill uses all"). Adding ONE storage
   binding reachable from `meshFill` breaks the voxelizer on any conformant
   mobile browser while working perfectly on desktop.

   The old `gpu.cpp` comment claiming "the voxelizer needs 10 storage buffers
   per stage" was true when written and went stale at commit 3eee63d: the
   skeleton-free rig deleted `mSkin` and `bWeights`, which `meshFill` used to
   reach. Nothing in the tree needs raised limits now.

   A rejected pipeline is otherwise SILENT — Dawn returns an invalid object
   that every later pass no-ops on. `GpuPipelineScope` (gpu.h) wraps each
   creation in a validation error scope so the failure names the pipeline, and
   startup prints a banner if any failed.

   Keep every write-side binding of those regions `read_write`: Dawn rejects a
   buffer that is writable and read-only within one pass and tracks that per
   BUFFER, not per bound range — so one `read` binding on any region breaks
   every pass that writes another. (`jfa.wgsl`'s `bDistRO` is declared
   `read_write` purely for this reason.) The tracer's read-only bindings are
   safe only because tracing is a different pass from every write.

9. **A browser main thread cannot block, and emdawnwebgpu makes that an
   ABORT, not a hang.** `wgpuInstanceWaitAny` is a bare `abort()` in a
   non-Asyncify wasm module at EVERY timeout **including 0** — the port's
   `library_webgpu.js` has literally one `abort()` line there — so there is no
   poll-once fallback to write, and a blocking wait takes the tab down with a
   stack trace pointing at Dawn. Requesting
   `InstanceFeatureName::TimedWaitAny` is the same trap wearing a disguise:
   `CreateInstance` returns NULL and it surfaces as "failed to create
   instance" with no hint why.

   So every blocking wait goes through `gpuBlockOn()` (src/gpu.h) and there is
   exactly ONE `.WaitAny(` left in the tree, inside it:

   ```sh
   grep -rn '\.WaitAny(' src/            # must print exactly one line
   grep -c emwgpuWaitAny build-web/clayfray.js   # must print 0
   ```

   The second check is the real one — Emscripten only links the WaitAny JS
   glue if something references it, so 0 proves the web build cannot reach the
   abort. (It prints 1 under `-DCLAYFRAY_WEB_JSPI=ON`, which is the escape
   hatch that makes blocking legal again.) Adding a bare `WaitAny` back
   compiles clean on both targets and fails only in a browser.

   Blocking also hides in non-GPU clothing: `Renderer::syncMeasurements`
   sleep-spins for map callbacks that only the JS event loop can deliver, so
   it is compiled out too. Any new "wait until X arrives" loop belongs behind
   `CLAYFRAY_DEV_TOOLS`, not in the frame path.

   As of stage 2 `gpuBlockOn` has **no callers left on web at all** — the
   adapter and device requests were the last two, and they are now
   continuations (see the Web target section). It stays because native still
   uses it and because it is the one place a future blocking wait must go.

10. **The voxelizer's triangle binning is the app's biggest CPU allocation,
    by two orders of magnitude — and nothing about the mesh predicts it.**
    `import: 20480 tris binned (10975582 refs)` at startup: each triangle is
    binned into every cell its band-dilated AABB touches, and `kBand` = 12
    voxels fans 20.5k triangles out to **11 million** cell refs, ~536 each.
    That is 41.9 MiB for `ids` plus another 42.3 MiB for `merged` (a full
    copy of it, alive simultaneously), i.e. a **~95 MiB startup transient**
    on a scene whose steady state is about 5 MiB.

    **It moves with the BRUSH count, not just with the mesh.** M-FIST's two
    extra hand shape keys took tris 14,848 → 20,480 (+38%), refs 8.0M → 11.0M,
    and the transient ~70 → ~95 MiB — straight past the 96 MiB
    `-sINITIAL_MEMORY` of the day, which is now 128 MiB.

    Desktop never noticed. The browser must reserve for it, which is what
    `-sINITIAL_MEMORY` in CMakeLists is sized from — and wasm memory never
    shrinks, so the module holds the transient peak for the whole session.
    Estimating this from vertex counts is hopeless; read the `import:` line.
    Halving it is easy and unclaimed: `merged` exists only to upload offsets
    and ids as one buffer, and two `WriteBuffer` calls at offsets 0 and
    `offsets.size()*4` are byte-identical.

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
| `CLAYFRAY_DEBUG_BLADE` / `_DEBUG_PUNCH` | per-frame weapon sweep + position. A strike that visibly connects and carves nothing has four indistinguishable causes from the ledger alone (which just reads 0.0 ml): wrong brush selected, under the cutting speed, missed the target's capsules, or the rest-space edit fell outside the volume |
| `CLAYFRAY_TOUCH=1` / `=0` | force the on-screen touch stick + swing button on/off, overriding the coarse-pointer probe (desktop layout work) |
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
