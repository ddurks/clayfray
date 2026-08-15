# How Claybook animates, and what it costs

Extracted from the GDC 2018 deck in this folder. This is the answer to "is
Claybook not animated?" — it is *heavily* animated. It just never skins an SDF
at sample time, and that single choice is the whole performance gap.

## Their ray-tracing budget (their slide, verbatim numbers)

| pass | Xbox One (base) @ 720p | AMD Vega @ 4K |
|---|---|---|
| cone-trace pre-pass | 0.2 ms | 0.2 ms |
| primary & AO rays | 1.5 ms | 1.6 ms |
| shadow rays | 1.7 ms | 1.9 ms |
| material & g-buffer | 0.8 ms | 1.0 ms |
| **total** | **~4.2 ms** | **~4.7 ms** |

"60 fps target on all consoles."

### Put next to clayfray

- Claybook: 4.2 ms for 720p = 921,600 pixels -> **~4.6 ns/pixel**, on an
  Xbox One, which is far weaker than an M2.
- clayfray: ~58 ms for 640x360 = 230,400 pixels -> **~252 ns/pixel**.

**We are ~55x more expensive per pixel, on better hardware, at a quarter of
the resolution.** No amount of tuning closes 55x. Something structural is
different, and it is not the sampling — that was measured at 8.8%.

Note also their shadow rays (1.7 ms) cost slightly MORE than primary + AO
(1.5 ms), which matches clayfray's own profile (shadows ~24% of frame). The
cone pre-pass is nearly free at 0.2 ms. So their distribution of work looks
like ours; only the magnitude differs.

## How the world SDF is built — the part we are missing

Claybook does NOT keep a static world volume. **It regenerates the world SDF
every frame** by compositing N "brushes":

- A brush is a small offline-baked volume texture, 32^3 to 128^3 (32 kB - 2 MB).
- Each brush has **translation, rotation and uniform scale** — rigid only.
- Combined with smooth add/cut (exponential min/max) and a layering system for
  operation ordering.
- **"Runtime performance not dependent on brush count."**

That last line is the one that matters. The generation pipeline:

1. **Generate SDF brush grid** — 64x64x32 dispatch, 4x4x4 groups. Sample each
   brush volume at the tile centre; **cull if SDF > tile bounds + 4 voxels**;
   accepted brushes are atomically appended into groupshared memory.
2. **Generate dispatch coordinates and mip masks** — 64x64x32 dispatch; read a
   brush grid cell, and if non-empty atomically append its coordinate to a
   buffer. This compacts the work list.
3. **Generate level 0 in 8x8x8 tiles (SPARSE)** — loop through the brushes
   *in groupshared memory for that tile*, sample each at the cell centre.
4. **Generate mips (sparse).**

So each tile evaluates only the handful of brushes that actually overlap it,
found by a cheap per-tile cull, and only non-empty tiles are processed at all.
That is why brush count does not drive cost.

## How the characters deform

- Clay shapes are SDFs converted to a **point cloud** — up to 16,384 surface
  particles per shape.
- Physics is **position based dynamics (PBD) on the GPU**.
- Collisions against the world SDF and between shapes, with **O(1)
  particle<->SDF collision detection** (that is the whole point of an SDF: a
  collision query is one fetch).
- **Plastic deformation** — the clay stays deformed.

So the character is a simulated blob whose shape is *authored into the volume*,
not a rigged mesh whose shape is *computed at sample time*. There is no
skeleton in the sampling path. Ever.

## The direct comparison to what clayfray does

| | Claybook | clayfray |
|---|---|---|
| when articulation is resolved | once per frame, per **tile**, during volume generation | **per sample**, ~66x per pixel |
| what a ray sample costs | one filtered volume fetch | 13-piece inverse-LBS warp + gathers |
| how many pieces a tile/sample sees | only those overlapping it (culled) | all 13, every time |
| how deformation happens | PBD particles + rigid brush transforms | linear blend skinning at sample time |

clayfray's per-sample piece loop is doing, millions of times per frame, the
work Claybook does once per tile per frame — and without the per-tile cull
that makes it cheap.

## Corroboration from outside Claybook

**Unreal Engine does not skin distance fields either.** Mesh Distance Fields
are generated offline and "do not know about deformations caused by skeletal
animation", so the Global Distance Field only casts shadows from *rigid*
meshes; skeletal meshes fall back to **capsule** shadows and capsule-based
DFAO. The industry answer to "animated character in an SDF pipeline" is a
rigid proxy, not a skinned field.

clayfray already has that proxy — `charProxy()` and the 13 fitted bone
capsules used for soft shadows.

## What this implies for clayfray, concretely

The posed bake was the right instinct; it was built without the two things
that make Claybook's version cheap.

1. **Composite rigid pieces into a volume, per pose step.** Each bone chunk is
   exactly a Claybook "brush": a rigid transform of a piece of the rest
   volume. Smooth-min them together, which is already what `kJointSminVoxels`
   does.
2. **Cull per tile.** Build a coarse tile grid over the posed body; for each
   tile, list only the bone chunks whose posed bounds overlap it; then each
   tile evaluates 1-2 pieces instead of 13. This is the step the earlier spike
   lacked, and it is where "not dependent on brush count" comes from.
3. **Keep it sparse.** Only generate tiles that are non-empty, via the
   compaction pass above — the earlier spike flattened the whole lattice
   (43.2M voxels, ~150 ms), which is why it froze.
4. **Bake in fighter-local space**, applying the rigid root inverse per
   sample, so a 60 Hz sliding root does not force a re-bake. One matrix
   multiply per sample versus the current 13-piece loop.

## Source code

There is none — Claybook is a commercial game and was never open sourced.
What exists publicly: this deck, the talk video, and Sebastian Aaltonen's
subsequent public work (github.com/sebbbi), including a Rust+Vulkan test
project rendering 1M SDF cubes. His GDC/SIGGRAPH material and forum writing
is the primary source.

Adjacent prior art worth reading: Media Molecule's *Dreams* (Alex Evans,
"Learning from Failure", SIGGRAPH 2015) — CSG edit trees evaluated to SDFs,
then rendered as dense multi-resolution **point clouds** rather than by
sphere tracing, which is a different answer to the same problem.
