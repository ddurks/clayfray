# AUDIT.md — the multi-agent engine review, and what is left of it

Three agents read the tree independently in August 2026 — a rendering/simulation
architecture review, a C++ correctness audit, and a structure/maintainability
pass — and their findings were consolidated into the tiers below. This file is
the LIVE status. The detail behind the `R`-numbers is
[`docs/engine-review.md`](docs/engine-review.md), kept verbatim and unedited.

**Read `docs/engine-review.md` section 0 before proposing any optimisation.**
Its most valuable content is not the recommendations, it is the table of
measured NEGATIVE results: the 3D-texture SDF (−0.5%), over-relaxed tracing
(reverted on look), the mip pyramid (~0), the posed-volume bake (8 fps in the
real app), per-sample conditional skips (*slower*). Most standard advice has
already been tried here and is dead. Re-proposing it costs a session.

Status legend: **SHIPPED** · **OPEN** · **SUPERSEDED**

---

## Tier A — silent-failure insurance · SHIPPED (`d08590d`)

Everything here shared one failure mode: *the app boots, prints a healthy
banner, and lies* — the one category that has actually shipped twice.

| # | Item | Status |
|---|---|---|
| A1 | `capsules[32]` / `gobs[24]` derive from `wgslConstants()` (= **R16**) | SHIPPED |
| A2 | `-Wall -Wextra -Wshadow` on `src/` only, deps `SYSTEM`, `CLAYFRAY_ASAN` | SHIPPED |
| A3 | GPU health is an exit code (**4**), not a grep — `gpuHealthExit()` | SHIPPED |
| A4 | `look.traceW/H` registered outside `if (refs_.brush)` so `--serve` can pin it | SHIPPED |
| A5 | Copy ctors deleted on `Renderer`/`BrickSystem`/`CtlServer`/`SnapWriter`/`SnapReader` | SHIPPED |
| A6 | `WaitAnyOnly` readbacks no longer capture `&ok` across a failing `gpuBlockOn` (5 sites) | SHIPPED |
| A7 | `wgslConstants` snprintf truncation guard | SHIPPED |
| A8 | `AGENTS.md` deleted in favour of `CLAUDE.md` | SHIPPED |

Enabling A2's warnings immediately paid for itself: the compiler independently
found two dead-code findings the human audit had already flagged (an unused
`kOrigin` alias, an unused `look` parameter on `updateBladeCut`). That `look`
parameter is **back and live** — R21's blade smear reads `look.impact`.

---

## Clay expressiveness — SHIPPED

Was "when M6 opens"; landed early because R19 had to precede any swing-timing
work. See CLAUDE.md's "Edit modes: the fused brush, the dent, the stamp".

| # | Item | Status |
|---|---|---|
| R19 | Fuse the swing's substeps into ONE multi-capsule op | SHIPPED |
| R20 | Volume-conserving displacement brush (`edit.wgsl` mode 3) | SHIPPED |
| R21 | Albedo-only edit mode (`edit.wgsl` mode 4) | SHIPPED |

One correction worth carrying forward, because the review does not contain it:
R20's "inward core + outward rim designed to cancel" is **necessary but not
sufficient**. The profile cancels only if its localizing window is applied to
the OLD FIELD rather than to the axial coordinate — an axial window drifts to
−7% of displaced volume at 25 mm of offset and −30% at 45° of tilt, and
negative means the punch CREATED clay and healed its target. Measured by
replaying the shader's own occupancy sum on a voxel grid; see `dentShift` in
`shaders/edit.wgsl`.

| R22 | Don't scale gobs by growing the uniform array (design note) | OPEN — advisory |

---

## Tier B — perf, and the order matters · OPEN

**Every cheap wall-clock lever has already been pulled.** Do these in order;
B1 in particular may invalidate a whole class of work below it.

- **B1 — One Xcode/Metal GPU capture of a traced frame.** The arithmetic that
  motivates it: 6.0 ms of `shade()` over 230k pixels is ~94,000
  ALU-op-equivalents *per pixel*, two orders of magnitude off what `shade()`
  actually computes. So the trace is almost certainly **latency/occupancy-bound,
  not ALU-bound** — which would explain the 3D-texture null result and why
  conditional skips measured slower, and would invalidate every ALU
  micro-optimisation. PLAN.md M4 asked for this and it never happened. Immune to
  both the throttling trap and the cold-compile trap.
- **B2 — Decompose `R` = 6.1 ms**, 28% of the frame and entirely unattributed.
  Prime suspect is the pick pass. Two `--exit-after` runs, no shader edits.
- **B3 — Gate the pick pass.** `pick.wgsl` is `@workgroup_size(1)` +
  `DispatchWorkgroups(1)` — one thread marching 200 steps, every frame, and in
  orbit mode the result is never read. One line. Worth proportionally most at idle.
- **B4 — Measure `spike/cone-prepass`.** Best expected value in the tree: code
  written, correctness established (3600 tiles, zero rays exceeding true hit),
  and it attacks the documented #1 cost (the march, 45%) by removing *steps* —
  the one axis not yet shown dead. Needs a rebase past the slice refactor.
- **B5 — Re-measure `spike/foveation-tiltshift`.** −27%, but measured
  pre-affine-rig and pre-`fairbench.sh`. Should survive where the 3D texture did
  not, because it cuts *ray count* rather than per-sample cost — and tilt-shift
  defocus is the miniature read, so this is the rare perf lever that may improve
  the look.

Cheap exact wins that need measuring rather than arguing: **R8** (algebraic skip
in `mapPenumbra`, `max(a,b) ≥ a`, provably image-identical) · **R6** (per-fighter
head bound over the 4 eye beads — currently 8 sphere tests × ~60 steps per pixel
for four glass beads) · **R13** (redistance copy-back → parity buffers:
bit-identical, and halves workgroup storage, which *doubles* mobile occupancy).

---

## Tier C — mobile, now that it is the target · OPEN

**The honest number: 21.2 ms on an M2 → 100–300 ms on a mid-range phone
(3–10 fps).** No single item closes that. The ladder that plausibly does:
320×180 traced + one fighter on the low tier + foveation + cone pre-pass
≈ 0.16× today's work ≈ 20–40 fps.

- **C1 — Coarsen the triangle bins** (= **R14**). Best item in the tier.
  Refs-per-bin is *independent of bin size*, so query cost is unchanged, but
  total refs scale as `binSize⁻³`: at 3× cell size, **8.0M refs → ~260k, i.e.
  1 MiB instead of 61 MiB**. Wasm memory never shrinks, so this transient is
  pinned for the whole session and is what sets `-sINITIAL_MEMORY`. (Deleting
  the `merged` copy, per trap 10, is the four-line half of it.)
- **C2 — Albedo pool**: 32 MiB/fighter, RGBA8 per voxel, twice the size of the
  distance pool it accompanies. RGB565, or per-brick base + delta.
- **R15 — Be honest about the mobile gap and plan a tier ladder.**

---

## Tier D — structure · OPEN

Ranked by payoff:

1. **F7 — split `Renderer::simulate()` from `draw()`.** A zero-behaviour-change
   cut. Makes frame count stop being a sim input, which is why `--serve` needs
   its `warm = 3` hack.
2. **F13 — a test target.** `anim.cpp`'s math and, once F7 lands, the whole
   conservation ledger become GPU-free testable. The one machine-checkable
   invariant this project has currently needs a 300-frame GPU run.
3. **F1 — generate the WGSL uniform struct.** The `//#constants` generator
   already exists and already killed this bug once for `MAX_FIGHTERS`;
   extending it retires trap 2 outright.
4. **F5 — shared brick pipelines** (20 compilations for 10 distinct programs).
5. **R9 — static-arena reprojection.** **The biggest single number available:
   ~4–6 ms of a 21 ms moving frame**, and the answer to PLAN.md's open "walking
   cannot reuse frames". It reprojects *geometry only* and re-shades every pixel
   every frame — categorically **not** TAA, so the boil survives. L effort, and
   currently blocked by the reject-sphere coupling below.

---

## Tier E — before `kMaxFighters` goes to 4 · OPEN

It is not one line. Six things do not derive:

- `kSpots[3]` — player 4 stands inside player 1
- gobs collide and stick to player 0 only
- **`syncMeasurements` pins player 0 only**, so journals editing player ≥ 1 are
  *not* run-to-run exact — which contradicts a documented guarantee
- shader reload rebakes player 0 only
- **material codes have 0.5 of headroom at 4 fighters and break at 5**
  (`MAT_EYE 4+15 = 19.0` against the ground-clay test at 19.5) (= **R17**)
- the brick pool has **7% headroom** over the measured add-stress peak

The `syncMeasurements` and `reloadShadersIfChanged` fixes are one-line loops and
are worth doing now, independently of the cap.

**R18 — give each sim subsystem its own RNG stream** belongs here too: it
protects determinism as the sim grows.

---

## Consolidated "do not do"

No ECS or scene graph · no virtual dispatch in the frame path (`Renderer`'s size
is a file-organisation problem, not an architecture one) · no RHI over WebGPU ·
**no TAA** — it smears the pose-step boil reseed; use exact geometric
reprojection instead · don't collapse the conservative/smooth field pairs
(trap 3) · don't reinstate dominant-bone ownership · **don't add a storage
binding reachable from `meshFill`** (8 of 8 — works on your desktop, fails to
create the voxelizer on a conformant phone) · don't retry over-relaxation or the
3D texture · **don't chase ALU micro-optimisation until B1 says it is
ALU-bound** · don't judge silhouette-touching changes by imgdiff.

---

## Two things worth knowing before picking anything

**The reject sphere is a global SHADING input.** `charLooseAffine` returns
`length(p - gFarCenter) - gFarR` for far points, and that feeds AO and penumbra
*everywhere* — 38,295 px moved when its derivation changed. This makes the bound
geometry un-refactorable without repainting the image, and it **blocks R9**.
Clamping each AO tap at zero (standard Quilez) decouples it and costs nothing,
but it is a look change and needs a human eye.

**What was never verified: every ms estimate in Tier B and C.** Those are the
agents' estimates, not measurements. This machine throttles harder than most of
the effects being hunted — use `tools/fairbench.sh` and a warm-up run, or the
numbers are fiction.
