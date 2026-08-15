# Claybook (GDC 2018) — the closest published prior art to clayfray

Sebastian Aaltonen (Second Order), *GPU-Based Clay Simulation and Ray-Tracing
Tech in 'Claybook'*. A shipped game whose world AND characters are SDFs,
ray-traced at 60 fps on PS4-era hardware.

- `Aaltonen_GDC2018_Claybook.pdf` — the deck.
  Source: <https://media.gdcvault.com/gdc2018/presentations/Aaltonen_Sebastian_GPU_Based_Clay.pdf>
  Session: <https://www.gdcvault.com/play/1025316/Advanced-Graphics-Techniques-Tutorial-GPU>
  Talk video: <https://www.youtube.com/watch?v=Xpf7Ua3UqOA>
- `slide-text-excerpts.txt` — slide text pulled out of the PDF and filtered to
  the rendering material, because the PDF itself is not greppable.

## What they do that we act on

- **World SDF is a 3D volume texture**: 1024x1024x512, 8-bit signed, 586 MB
  with 5 mip levels, distances over [-4, +4] voxels (1/32 voxel precision).
  A ray sample is ONE hardware-filtered fetch. -> `POC-3DTEX.md`; measured
  -8.8% on this codebase.
- **Mips**: "max step distance (world space) doubled per mip level", data
  scaling log8 (100%, 12.5%, 1.6%, 0.2%). AO rays sample a LOW mip. They
  measured a 1080p frame touching 8 MB of that 586 MB volume at a **99.85%
  cache hit rate**.
- **Coarse cone-trace pre-pass** over 8x8 pixel outer bounding cones, with an
  analytic step costing one extra instruction: precompute `C = sqrt(a^2 + 1)`,
  `A = C/(C - a)`, then in shader `t = (t + D) * A`.
- **Soft shadows**: the demoscene cone-coverage approximation
  `c = min(c, light_size * SDF(P) / t)`, improved by triangulating current and
  previous samples (less banding), plus jittered rays and temporal
  accumulation.
- **AO**: cone along the surface normal, random variation, temporal
  accumulation, low mip.

## What the talk lists as FAILED — read before re-inventing

clayfray independently reproduced both of these before finding them here:

- **Overstepping** (over-relaxed sphere tracing): "reduces sampling cache
  locality (random rollback), SDF(P) more noisy with our mipmapped approach,
  bloats VGPR count and adds ALU." Tried here: 15.9% of pixels changed to buy
  ~5%. Reverted.
- **Load balancing** by wave ballot refilling finished ray slots: "ray setup
  code runs for unfinished rays, volume texture sampling is less cache local.
  Coarse cone-trace is simpler and does the job better."

## The difference that still separates us

Claybook never articulates per sample — its characters are simulated INTO the
world volume, so a sample is just a sample. clayfray runs an inverse-LBS warp
over ~13 pieces per sample, measured at 65% of the frame. **That, not how a
sample is fetched, is the remaining structural gap.** Closing it means baking
the posed field into a volume once per pose step; the spike that tried it is
in this session's notes, and the fix it needs is to bake in FIGHTER-LOCAL
space so the 60 Hz root stops forcing a re-bake every frame.
