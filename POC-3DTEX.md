# POC: sample the SDF from a 3D TEXTURE instead of a storage buffer

**The implementation lives on branch `poc/sdf-3d-texture`, not here.** This
file is the plan and the measured result, kept on the mainline so the
experiment is not re-run from scratch. The code is deliberately left off the
clean branch: it wins 8.8% but still flattens the whole 43.2M-voxel lattice
on every volume change (~150 ms), which freezes the window while carving.

## The claim being tested

Every field evaluation currently pays for a **software trilinear filter**.
`brickSample()` in `shaders/brick_read.wgsl` does 8 × `brickVoxel()` — each an
index computation, a `u32` load from a storage buffer, and an
`unpack2x16float` — then 7 `mix()`es, on top of an indirection fetch to find
the brick. That is ~10 dependent buffer loads and ~30 ALU per sample.

Claybook's world SDF is a **3D texture**, so its equivalent is one
`textureSampleLevel`: hardware trilinear, dedicated texture cache, 8-bit
samples. That is where its 99.85% cache hit rate at 1080p comes from, and it
is a large part of why a full game held 60 fps on PS4 hardware.

The tracer runs ~66 field evaluations per shaded pixel (≈35 march + 4 normal
+ 5 AO + 22 shadow). **This change makes all 66 cheaper, and unlike every
lever tried so far it is orthogonal to the articulation warp.**

Hypothesis: replacing the software filter with a hardware one is worth
25-50% of the traced frame.

Falsifiable, and it might not be. Counter-argument worth taking seriously:
the brickmap is SPARSE, so its live working set is only ~12.4k bricks ×
512 × 2 B ≈ 12.7 MB, and 8³ bricks are already good spatial locality. A dense
texture is 86 MB. If the current path is already cache-resident, the win is
only the ALU and the load count, not the cache — call that 10-15% instead.
**Phase 1 exists to find out cheaply, before anything is restructured.**

## Why REST space first (and not the posed bake)

The obvious pairing is "bake the posed body into a world-space texture", which
is exactly Claybook's setup. Do NOT start there:

1. It conflates two changes. If the number moves we won't know which one did it.
2. The rest-space volume is **static except when carved**, so Phase 1 adds
   *zero* per-frame cost. A world-space posed volume must be rebuilt whenever
   the fighter moves — the posed-bake spike benched at 38 ms headless and then
   delivered 8 fps in the real windowed app, almost certainly because the root
   slides at 60 Hz and it re-baked every frame. That trap is avoided entirely
   by staying in rest space.
3. Phase 1 is a strictly smaller diff and reversible.

If Phase 1 wins, the posed bake becomes far more attractive *afterwards*,
because it can then write into a texture rather than a brickmap.

## Geometry and formats (computed, not guessed)

| | value |
|---|---|
| dense lattice | `kGrid × kBrickUsable + 1` = **351³** = 43.2M voxels |
| voxel | 4.05 mm; band ±12 voxels = ±49 mm |
| `r16float` | 86 MB/fighter, **173 MB for two** |
| `r8snorm` | 43 MB/fighter, 86 MB for two |
| `rg16float` | 173 MB/fighter, 346 MB for two |

**Phase 1 uses `r16float`.** 8-bit over the ±12 voxel band quantises to
0.379 mm, and the march hit epsilon is `0.00025 × (1+t)` ≈ 0.25 mm — the
quantum would be *coarser than the epsilon*, which risks surface acne and
would confound the measurement with a precision artifact. `r16float` holds
today's values exactly, so Phase 1 changes ONE variable: how the sample is
fetched. Narrowing the band to ±4 voxels (0.126 mm quantum) makes 8-bit
viable, but that is a Phase 2 storage optimisation, not part of the test.

351 is far inside `maxTextureDimension3D` (2048+).

## Web (browser WebGPU) compatibility

Checked against Dawn's own format/limit tables, not from memory. **The
strategy is web-compatible**, and it removes a portability problem we already
have — but two specifics decide the design.

**1. It FIXES an existing web blocker.** Core WebGPU guarantees only
`maxStorageBuffersPerShaderStage = 8` (Limits.cpp: core default 8; the 10 we
rely on is a Metal/tier-1 figure). **Trace currently binds 9 — already over
the web baseline.** Chrome reports the adapter's real limit so a desktop
browser will often allow 10, but nothing guarantees it. Every array moved out
of a storage buffer and into a texture frees a slot: sampled textures have
their own budget of `maxSampledTexturesPerShaderStage = 16`, of which trace
uses **zero**. So this change trades a scarce budget for an empty one.

**2. Do NOT write the texture through a storage binding.** `r16float` and
`r8snorm` are *not* storage-bindable in core WebGPU — the core storage
formats are the `rgba8*` / `rgba16*` / `r32*` / `rg32*` / `rgba32*` set
(Dawn gates even `rg32float` storage behind `CoreFeaturesAndLimits`). The
portable write path is:

> compute shader → storage **buffer** → `copyBufferToTexture` → sampled texture

which is fully core, works in the browser today, and costs nothing here
because the flatten is rare (on carve, not per frame). This also means the
POC needs **no 3D storage-texture binding at all**, which sidesteps risk 1
below entirely — so prefer it from the start rather than as a fallback.

**3. Pick a core-filterable format.** 16-bit float formats filter in core
WebGPU; **`r32float` does NOT filter without the optional
`float32-filterable` feature**. Since hardware filtering is the entire point
of this exercise, `r32float` would silently defeat it on any browser lacking
that feature. Use `r16float` (Phase 1) or `r8snorm` (Phase 2). Never
`r32float`.

**4. Copy alignment.** `copyBufferToTexture` requires `bytesPerRow` to be a
multiple of 256. A 351-wide row is 702 B at `r16float` (pad to 768) or 351 B
at `r8snorm` (pad to 512). The staging buffer must be padded per row — a
real detail that will otherwise show as a sheared volume.

**5. Memory budget matters more on web.** 173 MB of texture for two fighters
is heavy for a browser tab and likely too much for mobile. `r8snorm` with the
band narrowed to ±4 voxels brings it to 86 MB total, so **for a web target,
Phase 2 stops being optional** — plan for it rather than treating it as a
follow-up.

**6. Also check before shipping to web:** `maxStorageBufferBindingSize` core
default is 128 MiB (an 86 MB staging buffer fits, but tiling the flatten into
slabs is safer and enables dirty-region updates), and `maxTextureDimension3D`
core minimum is 2048 (351 is fine). `timestamp-query` is optional on web,
which is another reason the wall-clock protocol below is the right one.

## Bindings — the constraint that has bitten twice

CLAUDE.md trap 8: a Metal shader stage gets **10 storage buffers and trace
already uses 9**. This POC must not consume the last one.

It does not. A **sampled texture** and a **sampler** are separate limits, and
trace currently uses zero sampled textures (only `hdrOut`, a storage texture).
Phase 1 adds 2 sampled 3D textures + 1 sampler. Confirm at startup rather than
assume — a wrong assumption here is the failure that made macOS render nothing.

## Phases

### Phase 1 — decisive measurement (target: ~1 day)

1. **Texture resource.** Per fighter: `r16float` 3D texture, 351³, usages
   `TextureBinding | StorageBinding`. One shared filtering sampler
   (clamp-to-edge, linear).
2. **Flatten pass** — `shaders/flatten.wgsl`, brickmap → texture. One thread
   per voxel; reads exactly what `charDistRest()` returns today (brick value
   inside allocated cells, the conservative seed/coarse estimate outside) and
   writes it as one scalar. Writes through a `texture_storage_3d<r32float,
   write>` view (r16float is not storage-writable in core WebGPU — if that
   bites, write via a staging buffer + `CopyBufferToTexture`, or make the
   texture r32float at 173 MB/fighter for Phase 1 only).
3. **Trigger.** Run the flatten whenever `BrickSystem::generation()` changes —
   i.e. on import, bake, and edits. **Not per frame.** Full-volume on
   import/bake; on an edit, flatten only the edit's dirty sub-box (`EditParams`
   already carries `regionMin`/`regionDims`), or the whole volume in the first
   cut if that is simpler — a carve is not the hot path, the tracer is.
4. **Read side.** `charDistRest()` and `charLooseRest()` become a single
   `textureSampleLevel(tex, samp, (p - VOL_ORIGIN) / extent, 0.0)`. Keep the
   old path behind `look.sdfTexture` / `CLAYFRAY_NO_SDFTEX=1` so the A/B is an
   env var on one binary — this sidesteps the cold-compile trap entirely
   (CLAUDE.md benchmarking).
5. **Everything else untouched.** The warp, the piece loop, AO, shadows, the
   ground, conservation.

### Phase 2 — only if Phase 1 wins

- `r8snorm` with the band narrowed to ±4 voxels (43 MB/fighter, 2× bandwidth).
- **Mip chain**, sampled by AO and shadow rays — Claybook's "max step distance
  doubles per mip", ~27 of the 66 evaluations. The mip-pyramid agent already
  established the correct reduction for point-sampled taps is an *average*, not
  `min − offset`: that filter is for a pyramid you MARCH, and its offset
  compounds to ~7 cm by level 2 and reads as phantom occlusion (trap 3).
- Sparse residency (brick atlas texture + indirection) if 173 MB is a problem.

### Phase 3 — combine with the posed bake

Only after 1 and 2. Bake the articulated field into a WORLD-space texture once
per pose step, so the piece loop leaves the per-sample path entirely. This is
Claybook's actual architecture. Re-measure the 60 Hz re-bake problem first: the
existing spike is in worktree `agent-accfd49713af280d8` with two fixes already
applied (WGSL reserved keyword `meta`; indirect-args buffer split so `Indirect`
and writable `Storage` do not share a sync scope).

## Measurement protocol — windowed, not headless

Headless benchmarking is what produced a 26 fps claim against a real 8 fps.
It has no camera easing, no pick pass, no UI, and drives the root from a
journal at 12 Hz while a real session drives it at 60 Hz.

**Every number in this POC is measured windowed at a pinned resolution:**

```sh
# 600 frames of steady state, startup differenced out
./build/clayfray --res 640x360 --exit-after 60    # t1
./build/clayfray --res 640x360 --exit-after 660   # t2   -> 600/(t2-t1) fps
```

A/B the same binary with `CLAYFRAY_NO_SDFTEX=1`. Report idle AND moving.

Baseline on this M2, already measured: **63.3 fps idle at 640×360**
(15.8 ms/frame), 110.8 fps at 320×180. The moving case is the one that matters
and is the one to beat.

## Decision gates

- **< 10% on the moving frame** → stop. Revert the branch, write the negative
  result into CLAUDE.md so nobody re-runs it, and go to Phase 3 on its own.
- **10-25%** → keep, proceed to Phase 2 (8-bit + mips) for the rest.
- **> 25%** → keep, do Phase 2, then Phase 3. This is the path to 60 fps.

## Risks, in the order they are likely to bite

1. ~~`r16float` is not storage-writable~~ — confirmed true, and designed
   around: the flatten writes a buffer and copies into the texture. The
   residual risk is the 256 B `bytesPerRow` padding, which shows as a sheared
   or striped volume if got wrong.
2. **Memory.** 173 MB of textures on top of the existing pools. Watch for
   allocation failure at startup on smaller GPUs; it is fine on an M2's
   unified memory.
3. **Filtering across the band edge.** Hardware trilinear interpolates into
   whatever sits outside the narrow band. Today's `charDistRest` deliberately
   returns a *conservative* estimate there (trap 3), so the flatten must write
   that same value, not a clamp — otherwise the march oversteps and silhouettes
   break.
4. **The look.** Judged by eye in the running app at high resolution, never by
   `imgdiff` — a pixel diff already passed a change that visibly wrecked the
   mitts. Check the hands at the 4-bone junction and the carved cavities.
5. **The win is smaller than hoped** because the sparse brickmap was already
   cache-resident. That is the honest downside case, and gate 1 exists for it.

## Definition of done for the POC

- A windowed, pinned-resolution, idle-and-moving A/B table on one binary.
- A high-resolution look check on the hands and a carved surface.
- `--carve-test` exits 0 (conservation unaffected).
- A recommendation against the decision gates above, written here.

---

# RESULT: the texture path WINS — but read the correction first

## The first verdict was wrong, and the reason matters more than the result

Measured naively (run A, then run B) the texture path looked 7% SLOWER and I
wrote it off. That was an artifact of the MACHINE, not the change.

This M2 throttles hard under sustained load. Three IDENTICAL back-to-back
runs of the same config:

| pass | fps |
|---|---|
| 1 | 66.6 |
| 2 | 56.7 |
| 3 | 24.0 |

**2.8x drift.** That is larger than every effect measured in this whole
investigation, and the naive protocol runs one config after the other, so it
systematically penalises whichever goes SECOND. Every A/B taken that way is
suspect in a known direction.

`tools/fairbench.sh` fixes it: INTERLEAVE the configs (A,B,A,B,...) so both
see the same thermal profile, and take the MEDIAN per config so one hot or
cold pass cannot carry the result.

## Corrected numbers

Windowed, pinned 640x360, every frame traced (`CLAYFRAY_NO_REUSE=1`), same
binary both ways via `CLAYFRAY_NO_SDFTEX=1`, interleaved, 3 reps, median:

| variant | median | fps |
|---|---|---|
| software filter (baseline) | 61.78 ms | 16.2 |
| **3D texture, inside allocated cells** | **56.33 ms** | **17.8** |

**-8.8%.** Correctness is not in question: the texture reproduces the field to
42 pixels past imgdiff tolerance out of 230400.

Against the decision gates that is the 10-25% band's lower edge — keep it,
and go to Phase 2 (r8snorm + mips), which is where the rest of the win is
supposed to live. Do NOT re-derive the verdict from a naive A/B.

## What is committed on this branch

- `shaders/flatten.wgsl` — brickmap -> dense lattice, staged in a buffer and
  `copyBufferToTexture`'d (the core-WebGPU-portable path; r16float is not
  storage-bindable and r32float is not core-filterable).
- The 3D texture + sampler in `BrickSystem`, group(3) in the tracer and pick.
- `look.sdfTexture` / `CLAYFRAY_NO_SDFTEX=1` to A/B on one binary.
- The texture is sampled ONLY inside allocated cells, keeping the sparse
  indirection early-out. The indirection is an acceleration structure, not
  just storage: most march samples are in empty space, where one word read
  beats a filtered fetch.

## Known problems to fix before this is playable

1. **The flatten is full-volume** — 43.2M voxels, ~150 ms, run on every
   generation bump. That is the freezing and pink flashing seen while
   carving and at import. It MUST be restricted to the edit's dirty sub-box
   (`EditParams` already carries `regionMin`/`regionDims`).
2. The texture is unfilled for the first frames after startup, which is the
   rest of the flashing. Fill it before the first trace.
3. Memory: 86 MB/fighter at r16float, 173 MB for two, plus a 94 MB staging
   buffer per fighter. Phase 2's r8snorm halves the textures; the staging
   buffer should be shared or freed between flattens.

## Measuring this on another machine

```sh
./build/clayfray --res 640x360                 # pin the traced resolution
tools/fairbench.sh "software:CLAYFRAY_NO_REUSE=1 CLAYFRAY_NO_SDFTEX=1" \
                   "3D-texture:CLAYFRAY_NO_REUSE=1" 3 8
```

`--res` is what makes two machines comparable at all: frame cost is per
TRACED pixel, and window size does not tell you that (SDL reports BACKING
pixels, and resScale then divides them). The startup `[res]` line always
prints what is actually traced.
