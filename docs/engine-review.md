<!--
Verbatim output of the rendering & simulation architecture review (one of the
three agents behind AUDIT.md), recovered from the session that produced it.
Kept unedited ON PURPOSE: the numbered items R1-R22 are referenced by AUDIT.md,
by CLAUDE.md and by commit bodies, and its value is the measured negative
results — the record of what was tried and did NOT pay.

It is a SNAPSHOT, dated 2026-08-16, against the tree at 19c25f1. Line numbers,
timings and "current state" claims are as-of then; several items have shipped
since. AUDIT.md carries the live status, not this file.
-->

I've read CLAUDE.md, PLAN.md, every shader, the driving C++, plus `POC-3DTEX.md`, `docs/claybook/`, and the commit bodies on `main` and the five `spike/*` branches. That last part changed the shape of this report substantially — several of the obvious recommendations are already measured and dead, and two implemented spikes were never measured at all.

# clayfray — rendering & simulation architecture review

## 0. What the existing record already settles (read this before the list)

I found measured negative results for most of the "standard" advice, so I'm not repeating it:

| Idea | Verdict on record | Where |
|---|---|---|
| 3D-texture SDF + hardware trilinear | **−0.5%** post-affine-rig (was −8.8% pre-rig, and that was measuring the *warp's* cache waste, not filtering) | `POC-3DTEX.md`, branch `poc/sdf-3d-texture` |
| Over-relaxed sphere tracing | ~5% for **15.9% of pixels changed** → reverted. Claybook lists it as failed too | `docs/claybook/README.md` |
| Mip pyramid for AO/shadow taps | **~0** (58.66 vs 58.8 ms) | `spike/mip-pyramid` |
| Posed-volume bake | works, 1.25× headless, **8 fps in the real app** (60 Hz root re-bake) | `spike/posed-bake`, PLAN.md |
| Wave-ballot load balancing | Claybook: failed | `docs/claybook/` |
| Dynamic resolution while moving | shipped then **removed** — resScale is already 0.5, the switch was visible | 9e0fcac |
| Root quantisation to 12 Hz | **rejected on look**, and reuse% is explicitly the wrong scoreboard | params.h `MotionParams`, 7818e21 |
| Per-sample conditional skips | image-identical and *slower* (58.5 → 59.7/60.1/60.7) | CLAUDE.md |

The important meta-lesson already written down and worth acting on: *"an optimisation measured against an expensive baseline may be measuring that baseline's waste."* Two spikes were measured against the pre-affine frame and **have never been re-measured**: foveation (−27%) and, worse, the cone pre-pass was never measured *at all*.

Current state: **1 fighter 17.99 ms, 2 fighters 21.23 ms** at 640×360 with reuse off; budget is 16.67 ms.

---

## 1. Prioritised recommendations

### Tier 0 — instrument first (everything below depends on this)

---

**R1. Get one Xcode/Metal GPU capture of a traced frame. (S, no look risk)**

Every cheap wall-clock lever has been pulled. What is *not* known is whether the trace kernel is ALU-bound, memory-latency-bound, occupancy-bound, or spilling registers — and the four have completely different fixes. My arithmetic says it cannot be ALU: 6.0 ms of `shade()` over 230,400 pixels on an M2 (~3.6 TFLOP/s) is ~94,000 ALU-op-equivalents *per pixel*, which is off by two orders of magnitude from what `shade()` actually computes. That points hard at **dependent-load latency and occupancy**, not instruction count — which is consistent with the 3D-texture result (changing *how* a sample is fetched did nothing) and with "conditional skips do not pay" (removing ALU from some lanes does nothing when you're waiting on memory).

If that's right, it reorders everything: register pressure / occupancy and *number of dependent gathers* are the levers, and ALU micro-optimisation (analytic noise gradients, cheaper pebble maths) is worthless. PLAN.md M4 already flagged "real wins need Xcode GPU-capture profiling" and it never happened.

Get: ALU utilisation, occupancy, register/VGPR count and spill for `trace.wgsl cs`, L1/L2 hit rate, and per-pass durations. This is a one-off capture, immune to the throttling trap (counters, not wall clock) and immune to the cold-compile trap.

Verify: n/a — this *is* verification infrastructure.

---

**R2. Decompose `R` = 6.1 ms. It is 28% of the frame and entirely unattributed. (S, no look risk)**

Three specific suspects, in order of my confidence:

1. **The pick pass runs every frame, at `@workgroup_size(1)` with `DispatchWorkgroups(1)`** — one thread marching 200 steps plus 4 normal taps, every dependent load fully exposed with no other warp to hide latency (`renderer.cpp:1564` `encodePick`, called whenever `swapchainView != nullptr`). Estimate **0.2–1.0 ms per frame**, including reused frames, including frames where `brush.mode == 0` and nothing can be sculpted. Plus a `CopyBufferToBuffer` + `MapAsync` per frame.
2. **`PresentMode::Fifo` on a 60 Hz display** (`gpu.cpp:479`; the 60 Hz is stated in commit 21192b2) — yet the idle figure is 9.3 ms = **108 fps**, which Fifo cannot produce if present blocks. Either present isn't blocking in the `--exit-after` harness (window occluded/backgrounded), in which case `R` is real work but the harness doesn't reproduce shipping present behaviour; or it is, in which case `R` is inflated by a vsync floor artifact. Both matter.
3. **Blit + ImGui run at *window* resolution (1280×720)**, 4× the traced pixel count, while post runs at 640×360.

The 15-minute test, no shader edits so no cold-compile trap:
```sh
# same trace size, one with a surface/ImGui/present, one without
./build/clayfray --res 640x360 --exit-after 60   ;  --exit-after 660   # windowed
./build/clayfray --size 640x360 --frames 60      ;  --frames 660       # headless: no present/blit/ImGui
```
Difference the two per-frame numbers → that gap *is* present+blit+ImGui. Then ablate the pick pass (add an env guard) for suspect 1. Also: `CLAYFRAY_TRACY` exists as a CMake option with **zero zone macros instrumented** (PLAN.md M0) — ten `ZoneScoped` lines in `frameOnce` and `render()` would answer the CPU half of `R` permanently.

---

### Tier 1 — highest expected ms per unit of effort

---

**R3. Measure the cone pre-pass. The code exists, is proven correct, and has never been benchmarked. (S to measure / M to productionise; look risk: none if the bound is conservative)**

`spike/cone-prepass` (270775c): per-8×8-tile cone trace recording a safe start `t` for every ray in the tile, **validated on 3600 tiles with zero rays whose recorded `t` exceeded their true hit**. It failed WebGPU validation once (feeding a default pipeline layout into `CreatePipelineLayout`), was fixed, and *was never re-measured*.

This is the single best expected-value item in the tree: the code is written, correctness is established, and it attacks **the documented #1 cost** (march, ~9.9 ms / 45%) by removing *steps* rather than making samples cheaper — the one axis that hasn't been shown dead. Claybook priced their equivalent at 0.2 ms for 720p and treated it as their primary accelerator. The spike author estimated 5–15% and expected the low end because the scene is near-surface bound; that estimate predates the affine rig, when the march was a smaller share of a much bigger frame.

Files: `shaders/cone.wgsl` + `renderer.cpp` pass wiring (already on the branch). Rebase onto `main` (it branches from 7edf27e, pre-affine, pre-slice — the slice refactor will conflict in `brick_read.wgsl`).

Verify: `tools/fairbench.sh` interleaved with `CLAYFRAY_NO_REUSE=1`, both idle and moving; gate on `--carve-test` exit 0 and all three replays. Because it changes shader source, **burn a warm-up run after the rebase** (CLAUDE.md's cold-compile trap).

*Estimate: 5–15% of the traced frame, i.e. ~0.8–2.4 ms. Labelled estimate — the whole point is that nobody has measured it.*

---

**R4. Re-measure foveation + tilt-shift periphery. (S to measure / M to ship; look risk: medium, but it may be a look *improvement*)**

`spike/foveation-tiltshift` (be0e2a6): coarse full-screen pass + a full-res focus ellipse tracking the fighters; the periphery's upsampling reads as defocus. Measured **−27%** (59.4 → 43.6 ms), but with the naive A/B protocol *before* `fairbench.sh` existed, so it carries ordering bias in a known direction.

Why it should survive the affine rig where the 3D texture didn't: the texture change reduced *per-sample cost*, which is exactly what the affine rig had already made cheap. Foveation reduces **ray count**, which is orthogonal to everything the rig changed and scales linearly with whatever the traced frame currently costs. I'd expect a real, if smaller, win — call it 15–25%.

The art-direction argument is unusually strong: tilt-shift defocus is the *miniature* read, which is the same visual grammar as 12 Hz stop-motion. This is the rare perf lever that plausibly makes the look better. It also composes with R3 (fewer rays × fewer steps each).

Files (on branch): the trace entry point's ray-count logic, a coarse+fine pass pair, feather in `post.wgsl` (`hdrOut` is write-only, so the branch correctly puts the feather in post — no seam).

Verify: interleaved fairbench, idle and moving; then **a human looks at it at high resolution**, specifically at the mitts' silhouette at the focus boundary (CLAUDE.md: a pixel diff cannot see shape).

---

**R5. Gate the pick pass. (S, no look risk)**

`renderer.cpp:2522` — `doPick = swapchainView != nullptr || alwaysPick_`. Make it also require *(sculpt mode active OR the cursor moved since last frame OR `alwaysPick_`)*. In orbit mode (`brush.mode == 0`, the default and what the game will ship with) the pick result is never read.

Longer term the pass should be a workgroup, not a single thread: 64 lanes each marching a disjoint `t` interval and a ballot for the first hit, or simply run the 4 normal taps in parallel with the primary. But gating it is one line and captures most of the win.

*Estimate: 0.2–1.0 ms off **every** frame including reused ones, so it is worth proportionally more at idle (where it's up to 10% of a 9.3 ms frame). Measure with R2's ablation first.*

Verify: `probe`/`pickuv` must still work in `--serve` (that's what `alwaysPick_` is for); replay journals that assert a pick must still pass.

---

**R6. Put the eye marbles under the per-ray fighter cull. (S–M, image-identical if done as an exact bound)**

Right now `map()` loops **all 8 marbles at every march step** (`trace.wgsl:122`), `mapLoose` calls `marblesDist` (all 8) at every AO tap, and `mapPenumbra` does the same at every shadow step. That's ~60 field evaluations × 8 = **~480 sphere tests and 480 dynamically-indexed uniform loads per shaded pixel**, for four glass beads on a head — evaluated identically for a floor pixel three metres away.

Two ways, in increasing order of goodness:

- *Exact algebraic gate (S):* pack one bounding sphere **per fighter** over its 4 beads (one extra uniform slot per fighter, computed in `packUniforms` where `eyeWorld[]` already exists). Then `let hb = length(p - B.xyz) - B.w; if (hb < d) { ...loop 4 beads... }`. This is **exactly** image-identical: `B ⊇ M_i ⇒ sd_B(p) ≤ sd_i(p)` for all `p`, so `sd_B ≥ d` proves no bead can win the `min`. A *per-fighter head* bound (radius ~0.15 m) is what makes it fire; a single global marble bound would not.
- *Structural (M):* move the bead loop inside `fighterDist`, after the `far > 0.1` reject-sphere test in `charDistAffine`. Then the **already-shipped per-ray fighter mask culls the beads for free**, and rays that never enter a fighter's bound pay zero. Requires `fightersNearest` to return a marble index alongside the fighter index for `albedoFor`/`shade`'s glint rule.

The same exact-bound trick applies to `u.gobs` (≤12 spheres in `map()`), though it only matters mid-sploot.

*Estimate: 0.3–0.8 ms; more on mobile where the dynamically-indexed uniform loads are dearer. Honest caveat: CLAUDE.md's "conditional skips do not pay" applies to per-sample tests; this one is per-sample too. It differs from the failed cases in that the skipped work is 8 loads + 8 `length()` and the predicate is two live scalars — but it must be measured, not assumed.*

---

**R7. Tighten the per-ray cull from a sphere to the union of piece boxes, and make it per-piece. (M, same ±1 LSB drift class as the shipped cull)**

The just-shipped `setRayMask` (19c25f1, worth 5.32 → 3.24 ms for the opponent) tests one bound sphere per fighter. That sphere is derived by `affineBoundR` over **all three posed piece boxes**, and two of those are mitts held out on a sword — so the sphere is roughly twice the body radius and covers ~4× the projected area it needs to.

The box clip already guarantees the exact invariant you need: piece *i*'s zero set lies inside its rest AABB (`brushDist = max(charDistRest(q), boxDist)` — that's the whole "clipping, not ownership" argument). So **a ray that misses a piece's transformed OBB cannot hit that piece's surface**, by exactly the same reasoning the sphere cull already uses.

Two steps:
1. Replace the ray-sphere test with ray-vs-3-OBBs (transform ray origin+dir by `invSkin`, slab test against `aabbLo/aabbHi`). ~3× the per-pixel setup, but a much tighter mask.
2. Store **3 bits per fighter** instead of 1, and have `charDistAffine` skip non-masked pieces. Today it runs a `mat4` transform + box distance for all three pieces at **every march step**; most rays need one.

`gRayMask` already has room (4 fighters × 3 pieces = 12 bits of 32), and the set/clear discipline around `march()` is already in place and correct.

*Estimate: cuts the opponent's remaining 3.24 ms substantially and takes a bite out of the single-fighter march too. Guess 1–2 ms at two fighters.*

Verify: exactly as 19c25f1 did — ablate the mask to all-ones and diff; that commit's precedent (2.28% of pixels, 1.94% of them exactly ±1 LSB, zero above delta 32) is the acceptance bar. Plus `--carve-test` and the three replays.

---

**R8. Exact algebraic skip in `mapPenumbra`. (S, provably image-identical)**

`trace.wgsl:250`:
```wgsl
d = min(d, max(charProxy(p), fighterDistLoose(0u, p)));
```
Both terms are evaluated at every one of the 16 shadow steps. But `max(a,b) ≥ a`, so if `charProxy(p) ≥ d` the whole `min` is a no-op and `fighterDistLoose(0u, p)` — the expensive one, a full three-piece evaluation plus a brick/coarse gather — need not run at all:
```wgsl
let cp = charProxy(p);
if (cp < d) { d = min(d, max(cp, fighterDistLoose(0u, p))); }
```
This is not an approximation; it is algebra. `charProxy` is cheap and has its own `bound > 0.3` early-out. `softShadow` is 16 of the ~60 field evaluations per lit pixel, and this is the heaviest variant.

*Estimate: 0.3–0.8 ms. Same divergence caveat as R6 — measure.*

---

### Tier 2 — structural

---

**R9. Reproject the STATIC arena under camera motion; re-march only the fighter mask and the holes. (L, no risk to the boil — and that's the point)**

This is the answer to the problem PLAN.md names explicitly and then leaves open: *"walking cannot reuse frames... That is a rendering problem to fix in the renderer, not by degrading the movement."*

The observation: the fighters occupy maybe 15–25% of a 640×360 frame. The other 75–85% is floor, wall and background — **geometry that never changes**. During motion the only 60 Hz inputs are the camera chase and the fighter root; the sword, gobs, spring and pose are all already 12 Hz.

So keep a screen-space buffer of the **world-space primary hit position + material**. Each frame, reproject it into the new camera (exact for static geometry — no motion vectors needed, and moving objects are re-marched anyway). Re-march only:
- pixels whose `setRayMask` is non-zero, dilated by a pixel or two (the fighter and its silhouette),
- pixels with no reprojection source (disocclusion holes),
- everything, on a ground-clay/gob/sword generation bump.

Then run `shade()` fresh on every pixel, every frame.

**Why this is safe for the sacred part**: nothing is temporally *filtered*. A reprojected sample is an exact geometric sample of a static surface; the boil, the grain, the shadows and the AO are all recomputed from scratch each frame. This is categorically different from TAA, which would smear across the 12 Hz boil reseed and destroy the very thing that is non-negotiable.

*Estimate: march+normals is ~9.9 ms; arena pixels are perhaps 60–70% as expensive per pixel as body pixels; 75–85% of pixels. So roughly **4–6 ms of a 21 ms moving frame**. This is the largest single number available and it lands exactly where the budget hurts.* Effort is genuinely L — hole filling, silhouette dilation, and a careful invalidation digest split into "camera/arena" and "fighter" halves.

Risks: edge crawl at silhouettes if the dilation is too tight; a stale hit surviving a ground splat if the digest split is wrong. Verify with `--replay` on the walk journal plus a human looking at the fighter's outline against the wall while walking.

---

**R10. Split march from shade (G-buffer), shade per pixel not per AA sample. (L, medium look risk)**

PLAN.md priced this pre-affine at "36 → ~42 fps while moving" and deferred it as a poor ratio once dynamic resolution took the motion win — but dynamic resolution was subsequently *removed* (9e0fcac), so that argument no longer holds. Post-affine, `shade()` is 6.0 ms of 22; taking AO+shadow to half rate with a bilateral upsample is worth ~2–3 ms.

Independent of the half-res question, the split is the **enabling refactor** for four other things: (a) it unlocks the march-only `map()` that `brick_read.wgsl:423` says is required to make the capsule early-out safe (that one was worth 6.5 ms of 71 and was reverted only because `calcNormal` shares `map()`); (b) it lets the march use a screen-space-error epsilon that shading must not see; (c) it makes `workgroupBarrier` legal in the shading pass; (d) **it halves peak register pressure**, which is the thing most likely to be limiting occupancy on a phone (see R1).

Risk to the look is real: bilateral upsampling of AO/shadow at silhouettes is exactly the class of change that "a pixel diff cannot see". Ship it with the upsample disabled first (full-res shading, split passes only), measure the register/occupancy effect alone, then add half-res behind a flag.

---

**R11. Screen-space-error march epsilon. (M, medium look risk)**

`march()` terminates at `dm.x < 0.00025 * (1.0 + t)` — 0.25 mm at the camera, against a **4.05 mm voxel**. The field is trilinear over those voxels, so sub-0.06-voxel precision is meaningless data-wise; it exists (per the comment) to kill a rim fringe on grazing rays, which is precisely where it costs the most steps.

The principled version is a cone/LOD epsilon: `eps = max(eps0, kappa * t * pixelAngle)` where `pixelAngle = 2*tan(fovY/2)/resY`. At 640×360 and `t ≈ 2 m` that is ~5 mm — larger than a voxel, so the march would terminate several steps earlier on every ray, and *more* earlier on the grazing rays that currently cost the most.

This pairs naturally with R3 (the cone pre-pass already computes the per-tile cone) and requires R10's march-only `map()` to be safe against `calcNormal`.

Look risk: silhouettes soften; the rim fringe the tight epsilon was fighting may come back. Tune `kappa` by eye in the running app (it's a `ctl`-settable float, no rebuild). Note over-relaxation's precedent — 5% for 15.9% of pixels changed — as the bar this must beat.

---

**R12. Make the JFA incremental. (M–L, no look risk if done conservatively)**

`BrickSystem::encodeJfa` (`brick.cpp:954`) runs **init + 9 flood steps + resolve + 4 relax = 15 full-grid dispatches over all 125,000 cells** every frame in which *any* edit lands. During a sword swing that is every frame; the flood steps alone are ~30M neighbour reads per fighter per frame, to service an edit that touched perhaps 50 cells.

The correctness argument for scoping it: a **new** seed (a newly allocated cell) can only *decrease* the distance at another cell, and only within a radius bounded by that cell's currently-stored coarse value — so a bounded flood from new seeds is exact. A **removed** seed (a freed brick) leaves a stale seed that reports a distance *smaller* than truth, which is conservative and therefore safe for the march (just slower); it can be repaired lazily by a full JFA every N frames or on carve-idle. The 4 relax rounds only feed the **smooth** coarse field (AO/penumbra, trap 3) which has no safety requirement at all, so they can be region-scoped or amortised outright.

A cheap first cut before any of that: have `classify`/`fill` bump a counter when the allocated set actually changes, and **skip the entire JFA when it didn't** — many carve substeps inside an existing brick allocate nothing.

Verify: `CLAYFRAY_DEBUG_GRAD=1` (|∇d|−1 heatmap must stay green), `--carve-test` exit 0, and the replay journals. Matters far more on mobile than on the M2.

---

**R13. Redistance: drop the copy-back. (S, bit-identical)**

`redistance.wgsl:159-165` runs, per Jacobi iteration, a write pass into `shB` then a **second full pass copying `shB` back into `shA`**, with two `workgroupBarrier`s. Ten iterations × three rounds = 30 of these. Alternate the read/write buffers by iteration parity instead: halves the shared-memory traffic, removes one barrier per iteration, and lets you drop one of the two 4 KB `array<f32,1000>` buffers (8 KB → 4 KB of workgroup storage, which **doubles achievable occupancy on a mobile core** against `maxComputeWorkgroupStorageSize`).

Bit-identical output (Jacobi is order-independent within an iteration). *Estimate: 15–25% off the redistance pass, which is only visible during carving — but it is also the cheapest correctness-preserving win in the tree.*

---

### Tier 3 — mobile / web

---

**R14. Coarsen the triangle bins. This is the biggest mobile-memory item and it is nearly free. (M, no look risk)**

CLAUDE.md trap 10: `import: 14848 tris binned (8000190 refs)` — 30.5 MiB of `ids` plus a full 31.0 MiB copy in `merged`, a **~70 MiB startup transient** that wasm memory can never give back, and the reason `-sINITIAL_MEMORY=100663296`.

The stated fix (drop `merged`, do two `WriteBuffer` calls at offsets 0 and `offsets.size()*4`) halves it and should be done — it's four lines in `brick.cpp:574`.

But the real fix is to **bin into a coarser grid**. The ref count is `T × ((2·margin + triSize)/binSize)³` and the bin count is `(L/binSize)³`, so **refs/bin is independent of bin size** — the average triangles scanned per query in `nearestTri` does not change — while total refs scale as `binSize⁻³`. At 3× cell size: 8.0M refs → ~260k, i.e. **1 MiB instead of 61 MiB**, at identical query cost. Correctness holds if each triangle is binned into every coarse cell whose box dilated by `margin` contains it, which is what `cellRange` already does with a different divisor.

This unblocks a much smaller `INITIAL_MEMORY`, which is the difference between loading and not loading on a mid-range phone.

Verify: `--carve-test` exit 0, the `asset: brush` banner lines unchanged, `tools/verify_brush_layout.py`, and one look at the imported fighter (trap 7's failure mode is a body made entirely of hands).

---

**R15. Be honest about the mobile gap, and plan a tier ladder. (S to decide, M to implement)**

At 640×360 with two fighters the M2 is at 21.2 ms. A mid-range phone GPU is 5–15× slower at this workload, and mobile-specific penalties (lower occupancy, no unified-memory bandwidth, dynamically-indexed uniform arrays) make it worse. That is **100–300 ms/frame**, i.e. 3–10 fps. This is not a tuning gap and no item on this list closes it alone.

The realistic ladder, in the order the resolution lever should be pulled:
- **320×180 traced** (0.25× the pixels; the chunky low-res + grain aesthetic is already user-approved at 0.5 scale and PLAN.md M2 explicitly floats low-res as *aesthetic*, not just cheap) → ~5 ms-equivalent
- **one fighter** on the lowest tier
- **R4 foveation** on top (×~0.8)
- **R3 cone pre-pass** (×~0.9)
- **12 Hz reuse** already carries the still-camera case for free

Combined that's ~0.16× of today's work → roughly 25–50 ms on a mid phone → 20–40 fps, with reuse making stillness much cheaper. That's a *plausible* phone target. Anything above 480×270 with two fighters is not, and the project should say so before it costs a milestone.

Memory side: 104 MB of GPU buffers + 96 MiB wasm heap is already heavy for a phone tab. R14 fixes the heap; the albedo pool (32 MiB/fighter, RGBA8 per voxel, twice the size of the distance pool it accompanies) is the obvious halving — RGB565 or a per-brick base colour plus a low-precision per-voxel delta. Contamination gradients are the only thing that would show it.

---

### Tier 4 — latent defects and determinism

---

**R16. `capsules[32]` and `gobs[24]` are hand-mirrored literals. This is exactly the bug that already produced a black screen. (S, no look risk)**

CLAUDE.md trap 2 documents the `marbles` incident in detail — a WGSL array literal that agreed with a C++ constant *by coincidence*, and when the coincidence broke, **every** bind group against that layout failed, the app booted, printed a healthy banner, rendered black, carved 0.0 ml, and `--carve-test` still exited 0 (0 == 0 balances). The fix was to derive the dimension from `wgslConstants()`.

Two more of the same pattern are still live:
- `capsules: array<vec4f, 32>` in `trace.wgsl` and `pick.wgsl`, against `kSlotGobMeta = kSlotCapsules + 32; // 16 capsules x2` in `renderer.h` and a bare `std::min((int)capsules_.size(), 16)` in `packUniforms`.
- `gobs: array<vec4f, 24>`, against `kSlotGroundMeta = kSlotGobs + 24; // 12 gobs x2`, `std::min((int)gobs_.size(), 12)`, and `clamp(i32(m - MAT_GOB + 0.5), 0, 11)` in `albedoFor`.

Emit `MAX_CAPSULES` and `MAX_GOBS` from `BrickSystem::wgslConstants()` and derive all six sites. Cheap insurance against a failure mode that has already cost a session once.

---

**R17. The material-code space runs out at 4–5 fighters. (S to document, M to fix)**

Materials are floats: `MAT_BODY 3.0 + 0.1·f`, `MAT_EYE 4.0 + marbleIndex`, `MAT_GCLAY 20.0`, `MAT_GOB 21.0 + gobIndex`. `kMaxMarbles = 4 * kMaxPlayers`, so at `kMaxFighters = 4` the highest eye material is `4 + 15 = 19.0` — and `albedoFor`'s ground-clay test is `m > MAT_GCLAY - 0.5` = **19.5**. That is 0.5 of headroom. At `kMaxFighters = 5`, or at 4 fighters if the artist adds a fifth bead per body, an eye silently renders with a gob's albedo and loses its glint.

CLAUDE.md documents raising the cap to 4 as "that one line PLUS dropping `kMaxBricks` to 12288". It is also this. Either widen the spacing (`MAT_EYE = 100.0`) or, better, split material into an integer class plus an index — the `vec2f(d, m)` return is already there to carry it.

---

**R18. Give each sim subsystem its own RNG stream. (S, protects determinism as the sim grows)**

There is one `gobSeed_` LCG. The slice-gob path in `updateConservation` **deliberately avoids calling `rnd()`** so that "the dribble's seeded stream [stays] bit-identical for scenes with no sword" — a comment that documents exactly how fragile a single shared stream is. Every new system that draws from it will shift every existing system's sequence and break every recorded journal.

Seed per subsystem from `(poseTick, subsystemId, index)` — a stateless hash rather than a stateful LCG. Then adding splatter, dents or MPM seeding cannot perturb the gob dribble, and journals stay replayable across feature work. This is a five-line change now and a week of forensics later.

---

### Clay dynamics — scaling up expressiveness

---

**R19. Fuse the swing substeps into ONE multi-brush edit op. (M, look risk: improves the cut)**

`kOpsPerFrame = 6` is described as what constrains swing speed, and CLAUDE.md notes "residual scalloping on very fast swings is that budget". But the budget is six *dispatch pairs*, not six brushes: `updateBladeCut` emits up to six capsules along the blade's path, each running its own `classify` + `fill` over its own padded AABB, each measured separately.

Give `EditParams` a `brushCount` and an array of `(posA, posB)` — 16 capsules is 512 bytes of uniform. `brushSdf` becomes a `min` over the array; `classify`/`fill` dispatch once over the **union** AABB. Three consequences:

1. The swing-speed ceiling stops being a dispatch budget and becomes a brush-array size. A fast swing gets 16 or 32 substeps for less GPU work than 6 costs today.
2. **The scalloping disappears by construction.** Six sequential `softCarve`s each smin against the *previous* result; one `softCarve` against the true union of the swept capsules is the mathematically correct swept volume.
3. Conservation gets *more* accurate, not less — one measurement of the whole cut rather than six overlapping ones.

Determinism unaffected (fixed count, fixed order, no RNG). Files: `shaders/edit.wgsl` (`EditParams`, `brushSdf`), `src/brick.h` (`BrickEdit` grows a segment list or gains a batch form), `src/brick.cpp` (`encodeOp` region union), `renderer.cpp` (`updateBladeCut` emits one op).

Verify: `--carve-test` exit 0 with the ledger exact, `scenarios/carve-duel` replay, and a human looking at a fast swing's wound.

---

**R20. Add a volume-conserving displacement brush — this is what "plastic deformation" actually wants. (M, look risk: high upside)**

Right now the only shape verbs are carve (throw a gob) and add. A punch that dents clay should not eject material at all: it should push the surface in and raise a rim. Implement as a single brush whose distance modification is inward inside a radius and outward in a shell around it, with the two occupancy integrals designed to cancel:

```
dNew = dOld + push(dOld, brushDist)      // negative inside the core, positive in the rim shell
```

The existing measurement pass (`counters[3]`, `|occupancy delta|`) then reports ~zero net change and the ledger balances **automatically** — no gobs, no debt, no re-stick. It is one more `mode` in `edit.wgsl`, it reuses the whole classify/fill/JFA/redistance chain unchanged, and it is deterministic because it is a pure function of the edit parameters.

This is also the honest answer to "impact splatter, smearing, merging":
- **Smear**: carve at A and add at A+δ with the source albedo, in one fused op (R19). Both are measured, so the ledger balances.
- **Merge**: already works — `softAdd` is a smin.
- **Contamination**: see R21.

The Claybook precedent is exactly this — *"plastic deformation — the clay stays deformed"* — and it's what makes their clay read as clay rather than as a thing that sheds pellets.

---

**R21. Add an albedo-only edit mode. (S–M, cheap expressiveness)**

Every edit currently rewrites distances, which means `classify` (allocation), `fill`, JFA and redistance. A colour stamp needs none of that: it touches no surface, allocates no bricks, dirties nothing, changes no volume, and bills nothing to the ledger. A mode that writes only `bAlbedo` over the brush region is a fraction of the cost of a real edit and buys: impact contamination (attacker's colour stamped into defender), floor smears, bruising, mud from the arena. PLAN.md M6 asks for all of this.

It also cleanly separates the two scaling problems: *shape* changes are expensive and rate-limited; *colour* changes are cheap and can be plentiful.

---

**R22. Don't scale gobs by growing the uniform array. (design note)**

`u.gobs` is 12 spheres evaluated in `map()`, `mapLoose` and `mapPenumbra`. Splatter at the scale M6/M7 imply (hundreds of droplets) cannot go here — the cost is `N × 60` sphere tests per pixel and the uniform block is the wrong home for dynamic counts. The scalable route is: keep ~12 *rendered* gobs as hero geometry, and route the rest straight into `GroundClay::splat` (or an albedo stamp, R21) without ever existing as traced spheres. The `splat(pos, vol, color)` contract PLAN.md already identifies as the swap point is exactly right for this.

For M7 MPM specifically: keep the locked decision that GPU state never feeds gameplay. The one GPU→CPU path that does feed sim (the volume ledger) is already pinned by `syncMeasurements` for replay; anything MPM adds must go through the same discipline or stay visual-only.

---

## 2. Comparison to how modern engines do this — with verdicts

**UE5 Lumen mesh distance fields + global SDF** — Lumen keeps per-mesh SDFs in a shared atlas and composites a coarse **global SDF clipmap** each frame so a trace hits one field regardless of object count. Notably, UE does *not* skin distance fields at all: skeletal meshes fall back to capsule proxies for DFAO and shadows. clayfray independently arrived at the same answer (`charProxy` + fitted capsules). **Adopt: nothing new.** The clipmap idea would remove `N` from the per-step min, but at two fighters plus an analytic arena the fighter loop is already one ray-sphere test per pixel (R7 tightens it further); a world clipmap would cost a rebuild whenever anything moves, which is precisely the failure mode `spike/posed-bake` measured at 8 fps.

**Nanite** — **Do not adopt, and don't spend time understanding it for this.** There is no triangle pipeline here and no analogue: Nanite solves "billions of authored triangles" via cluster LOD and software raster. A 1.4 m sculptable blob has the opposite problem.

**Dreams (Media Molecule)** — CSG edit list as the authoritative representation, evaluated into a sparse multi-resolution brick tree, rendered by **point splatting** rather than ray marching, with an edit BVH so only affected regions re-evaluate. **Adopt the edit-scoping idea (already have a simplified version: dirty bricks + region dispatch); R12 is the missing half of it for the JFA.** **Do not adopt point splatting** — it exists because Dreams had to render unbounded user-authored content with soft painterly AA on a PS4; the sphere tracer already delivers the required look and swapping renderers would restart M1's gate.

**Claybook** — the closest sibling and the repo already has the deck. Their world SDF is one 3D volume texture regenerated per frame by compositing rigid brushes, with a **per-tile brush cull** ("runtime performance not dependent on brush count") and a **coarse cone pre-pass**. **Adopt the cone pre-pass — R3, and it is already written.** The per-tile brush cull is the same idea as R7 at a different granularity and is the right generalisation if fighter count ever rises. **Do not adopt** their world-volume regeneration (measured here as `spike/posed-bake`, 8 fps) or their 3D-texture sampling (measured at −0.5%). Also note their two documented *failures* — overstepping and wave-ballot load balancing — both of which this codebase independently reproduced.

**Godot SDFGI / cascaded SDF GI** — **Scope-inappropriate.** The art direction is one theatrical warm key crushing to black (PLAN.md M1, `reference/ART_DIRECTION.md`). Adding bounce GI would fill the blacks and fight the entire look. The `ambient` term is already a flat 0.016/0.012/0.009 by choice.

**Unity SDF baking / VFX Graph** — authoring-tool convenience for baking meshes to SDFs. **Nothing to adopt**; `voxelize.wgsl` + the watertight parity pass already does this better for this use case (exact closest-triangle magnitudes, parity signs).

**NanoVDB / GVDB / brickmap literature** — the mature versions of what's already here. Two ideas are genuinely transferable:
- **Cached accessors** (NanoVDB's signature): remember the last cell and its indirection word in a `var<private>`, and check it before re-decoding. Consecutive march steps overwhelmingly land in the same cell or a neighbour, so most steps would skip the indirection load entirely. Per-lane state, no divergence hazard. **Adopt — S effort, worth trying, especially if R1 confirms load-latency-bound.** Key it by `(gCellBase, cellIndex)` since fighters share buffers.
- **Morton/Z-order within a brick**: an 8-corner trilinear gather currently spans bytes 0–18 and 128–146 of the brick (i.e. two cache lines); Morton order puts all eight in one. Exact same values, guaranteed no look change, touches the index maths in `brick_read.wgsl`, `edit.wgsl`, `redistance.wgsl`, `jfa.wgsl`, `voxelize.wgsl`. **Adopt only if R1 shows cache misses** — the 3D-texture result suggests the working set may already be resident, which is the honest downside case.

GVDB's brick-atlas-in-a-3D-texture is the thing `POC-3DTEX.md` tested and rejected at −0.5%. **Do not revisit** unless a structural change makes samples hot again.

**Neural / hash-grid SDFs (Instant-NGP and descendants)** — **Categorically wrong here.** The single defining requirement is that the field is *carveable at 60 Hz from a compute shader*. A hash-grid or MLP field cannot be edited without re-fitting, has no conservative distance bound (so sphere tracing is unsafe), and its inference cost per sample is orders of magnitude above a trilinear fetch. Say no and move on.

**Raymarching literature** —
- *Hart 1996, sphere tracing*: the baseline, correctly implemented, including the Lipschitz rescale by `sigma_min` (and the `aabbLo.w` note about shear vs column norms is genuinely more careful than most implementations).
- *Keinert et al. 2014, Enhanced Sphere Tracing (over-relaxation)*: **tried, 5% for 15.9% pixel movement, reverted.** Don't retry.
- *Galin et al. 2020, Segment Tracing*: needs a cheap **local** Lipschitz bound per primitive. The analytic parts have one (`sigma_min`, `G_KLIP`); the sampled brick field does not, and inventing one would cost more than it saves. **Low priority, partial applicability at best.**
- *Keyframe-coherent reprojection*: the 12 Hz frame reuse is already the strongest possible form of this for a static camera (bit-identical, 0 error). R9 is the principled extension to motion — and the crucial constraint is that it must **reproject geometry, never accumulate shading**, because accumulation would smear across the pose-step boil reseed.

---

## 3. What NOT to do

**Do not add TAA.** PLAN.md lists it as an open question; the answer is no. TAA's history buffer is filtered across frames, and this renderer deliberately reseeds surface detail *every pose step* — the boil, which PLAN.md calls "sacred" and "user-loved". Any temporal filter either smears the reseed (destroying it) or rejects history at every pose boundary (making it useless 12 times a second). Use exact reprojection of static geometry (R9) if you need temporal coherence; never accumulate shading.

**Do not merge `spike/temporal-accumulation` as-is.** It's clever and its epoch handling is correct, but the epoch is one frame long whenever the camera or root is moving — which is exactly the case that needs help. It buys a cheaper *idle* frame (already 9.3 ms against a 16.67 ms budget) at the cost of a *worse-looking moving* frame (shadow 22→12, AO 5→3 with no recovery). Wrong direction.

**Do not reinstate dominant-bone ownership** in the piece clip. `brick_read.wgsl` explains precisely why box clipping is exact for disjoint brushes and ownership was not — and it is one of the few pieces of design in the tree that is *provably* correct rather than tuned.

**Do not collapse the conservative/smooth field pair** (trap 3). It looks like duplication; it is the difference between a valid march and phantom occlusion. Any new field needs both variants.

**Do not add a storage-buffer binding reachable from `voxelize.wgsl meshFill`.** It sits at 8 of 8 on core WebGPU and will work perfectly on your desktop while failing to create the voxelizer pipeline on a conformant phone browser. Four slots are spare in `trace` — spend them there if you must, and prefer growing an existing packed region.

**Do not chase ALU micro-optimisation** (analytic noise gradients for the boil, cheaper pebble maths, hoisting `pieceAt(i)`) until R1 says the kernel is ALU-bound. My arithmetic says `shade()` is ~94,000 ALU-op-equivalents per pixel at its measured cost, which is impossible — the time is going somewhere else, and shaving instructions will read as noise.

**Do not benchmark anything without `fairbench.sh` and a warm-up run.** The record contains a 2.8× drift across three identical passes and a "13 ms/frame win" that was a cold pipeline compile. Both of those already fooled a careful person once.

**Do not judge silhouette-touching changes by `imgdiff`.** A 133-pixel diff — inside tolerance — shipped a visibly wrecked mitt. R4, R10 and R11 all touch silhouettes.

**Do not raise `kMaxFighters` to 4 without re-deriving `kMaxBricks`.** At 12288 the measured `CLAYFRAY_TEST_ADDSTRESS` peak is 10,273 — **7% headroom**, and a pool spill manifests as holes and silently no-op carves. Also fix R17 first.

---

## 4. Risks and scaling limits — what breaks first

**In order of how soon it bites:**

1. **Mobile, on raw throughput.** 21 ms on an M2 at 640×360 is 100–300 ms on a phone. This is the largest gap in the project and no single item closes it (R15).
2. **Mobile, on memory.** The 70 MiB import transient (trap 10) permanently inflates the wasm heap, and 104 MB of GPU buffers sits on top. R14 is the fix and it is cheap.
3. **The brick pool at 4 fighters.** 7% headroom over the measured add-stress peak. Adds are the unbounded direction (carves provably cannot overflow — the derivation in `brick.h` is excellent), so a long session with heavy re-sticking is the scenario. Overflow is graceful and reported, but visible.
4. **Material-code space** (R17): 0.5 of headroom at 4 fighters; broken at 5.
5. **The `capsules[32]` / `gobs[24]` literals** (R16): a repeat of a failure that produces a black screen and a passing exit code.
6. **The shadow proxy is player 0's alone.** `u.capsules` is not per-fighter; every other body takes the plain loose field in `mapPenumbra`. Invisible today; it will become visible the first time two fighters stand close enough to shadow each other, which is the whole premise of a duel.
7. **The reject sphere is a global shading input.** `charLooseAffine` returns `length(p - gFarCenter) - gFarR` for far points, and that value feeds AO and penumbra *everywhere*, including surfaces the fighter is nowhere near. CLAUDE.md documents the measurement (38,295 px moved by changing the derivation). This makes the bound geometry un-refactorable without repainting the whole image, and it blocks any per-pixel caching (R9). Clamping each AO tap's contribution at zero (`max(h - mapLoose(...), 0)`, the standard Quilez formulation) would decouple distant fighters from arena AO and cost nothing — but it is a look change and needs a human.
8. **`kOpsPerFrame` as a gameplay constraint.** It currently dictates how fast a sword may swing, which means a *rendering* budget is setting a *design* parameter. R19 removes that coupling — do it before M5 tunes swing timing, not after.
9. **Snapshots save player 0's volume only.** Fine as scratch tooling; it means a two-fighter carve scenario cannot be checkpointed, which will hurt once M5/M6 iteration starts.

**Where there is genuine headroom:** the binding budget (4 of 8 in `trace`, and it no longer grows with fighter count — that refactor was the right call and it is done); the uniform block (~135 of a possible 4096 slots); the piece count (3 of `kPiecesPerFighter`, and the cost is 6 uniform slots each); the ground field (3 MiB, 512², trivially extensible to per-texel wetness/contamination); and the dev loop, which is the best-instrumented part of the project and is what makes any of the above measurable at all.
