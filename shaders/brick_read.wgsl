// Read-side brickmap: sparse SDF volume for the character body.
// Bricks are 8^3 voxels covering 7^3 unique cells (1-voxel overlap for
// seamless trilinear sampling). Distances stored as f16 pairs in voxel
// units, clamped to a +-4 voxel narrow band. Empty cells carry a coarse
// Chebyshev distance (in brick units) maintained by JFA after every edit.
//
// Includers must not redeclare group(1).

// GRID / BRICK_USABLE / VOXEL / SPAN / VOL_ORIGIN / BAND / MAX_BRICKS come
// from the //#constants block in the including ROOT (trace.wgsl, pick.wgsl),
// generated from src/brick.h. This file is an include, so it must NOT declare
// them itself — WGSL rejects the duplicate.
// BAND is a half-width ~ one cell diagonal, so allocated cells report TRUE
// distance everywhere and AO/shadow taps never hit a clamp plateau.

const IND_ALLOC: u32 = 0x80000000u;
const IND_INSIDE: u32 = 0x40000000u;
const IND_IDX_MASK: u32 = 0x000FFFFFu;

// ---------- THREE BINDINGS, ANY NUMBER OF FIGHTERS ----------
// Two levels of packing stack here, and both exist for the same reason: core
// WebGPU guarantees a shader stage only 8 storage buffers (CLAUDE.md trap 8),
// and the tracer has to fit every fighter plus the ground inside that.
//
//  1. The three per-cell arrays (indirection, JFA seeds, coarse distance) are
//     ONE buffer, addressed through the CELL_* base indices in the
//     //#constants block. (There was a fourth region, the per-cell skin
//     field; M-RIG deleted it along with the skeleton.)
//  2. Every FIGHTER is a further slice of those same three buffers, at stride
//     VOL_STRIDE (cells) and MAX_BRICKS (pool). So N fighters cost 3 bindings,
//     not 3N — this is PLAN.md's "fighters are SLICES, not systems".
//
// The write passes never learn about (2): each fighter's BrickSystem binds its
// own slice as a bind-group SUB-RANGE, so edit/voxelize/jfa/redistance still
// index from zero and MAX_BRICKS still means "this fighter's pool". Only the
// read side below adds a base, and only because the tracer binds whole buffers.
@group(1) @binding(0) var<storage, read> bCells: array<u32>;
@group(1) @binding(1) var<storage, read> bDist: array<u32>;    // 256 u32 per brick (512 f16)
@group(1) @binding(2) var<storage, read> bAlbedo: array<u32>;  // 512 u32 per brick

// Which body the sampling functions are currently reading. These are BASE
// INDICES, not an id to branch on: the two-fighter version of this file
// selected the volume with `if (gFighter == 0u)` inside every accessor, which
// does not extend (four bodies would be a four-way chain in the hottest code
// in the frame) and cost real time even at two — see the note in
// charDistAffine about fetching each bound field once. An add is free.
var<private> gFighter: u32 = 0u;
var<private> gCellBase: u32 = 0u;  // first u32 of this fighter's volume slice
var<private> gDistBase: u32 = 0u;  // first u32 of its distance-pool partition
var<private> gAlbBase: u32 = 0u;   // first u32 of its albedo-pool partition
var<private> gPieceCount: i32 = 0;
var<private> gFarCenter: vec3f = vec3f(0.0);
var<private> gFarR: f32 = 0.0;

fn usePlayer(f: u32) {
  gFighter = f;
  gCellBase = f * VOL_STRIDE;
  gDistBase = f * MAX_BRICKS * 256u;
  gAlbBase = f * MAX_BRICKS * 512u;
  gPieceCount = i32(u.fighters[f].info.y);
  gFarCenter = u.fighters[f].center.xyz;
  gFarR = u.fighters[f].info.z;
}
// one piece of whichever player is selected
fn pieceAt(i: i32) -> Piece {
  return u.fighters[gFighter].pieces[i];
}

fn rdIndir(i: u32) -> u32 {
  return bCells[gCellBase + CELL_IND + i];
}
fn rdDist(i: u32) -> u32 {
  return bDist[gDistBase + i];
}
fn rdAlb(i: u32) -> u32 {
  return bAlbedo[gAlbBase + i];
}
fn rdSeed(i: u32) -> u32 {
  return bCells[gCellBase + CELL_SEED + i];
}
// The coarse region is f32 data in the shared u32 buffer.
fn rdCoarse(i: u32) -> f32 {
  return bitcast<f32>(bCells[gCellBase + CELL_COARSE + i]);
}
// M-RIG: rdCellW() read the per-cell skin field (a fourth region of the volume
// buffer). It was the last consumer of a bone weight anywhere in the tracer.
// The field, the region and this accessor are all gone — see cellOwnedBy's
// replacement below for what took over piece separation.

fn cellIndex(c: vec3i) -> u32 {
  return u32(c.x + c.y * GRID + c.z * GRID * GRID);
}

fn brickVoxel(brick: u32, v: vec3i) -> f32 {
  let vi = u32(v.x + v.y * 8 + v.z * 64);
  let word = rdDist(brick * 256u + vi / 2u);
  let pair = unpack2x16float(word);
  return select(pair.x, pair.y, (vi & 1u) == 1u);
}

// Trilinear sample inside one allocated brick; lf in [0,7].
fn brickSample(brick: u32, lf: vec3f) -> f32 {
  let clamped = clamp(lf, vec3f(0.0), vec3f(6.999));
  let i0 = vec3i(floor(clamped));
  let f = clamped - vec3f(i0);
  let d000 = brickVoxel(brick, i0);
  let d100 = brickVoxel(brick, i0 + vec3i(1, 0, 0));
  let d010 = brickVoxel(brick, i0 + vec3i(0, 1, 0));
  let d110 = brickVoxel(brick, i0 + vec3i(1, 1, 0));
  let d001 = brickVoxel(brick, i0 + vec3i(0, 0, 1));
  let d101 = brickVoxel(brick, i0 + vec3i(1, 0, 1));
  let d011 = brickVoxel(brick, i0 + vec3i(0, 1, 1));
  let d111 = brickVoxel(brick, i0 + vec3i(1, 1, 1));
  return mix(mix(mix(d000, d100, f.x), mix(d010, d110, f.x), f.y),
             mix(mix(d001, d101, f.x), mix(d011, d111, f.x), f.y), f.z);
}

// Conservative signed distance to the REST-SPACE brickmap body. Accurate
// inside the narrow band; coarse-but-safe steps elsewhere. charDist() below
// articulates this through the posed chunk union.
fn charDistRest(p: vec3f) -> f32 {
  let volMax = VOL_ORIGIN + vec3f(f32(GRID) * SPAN);
  let toBox = max(VOL_ORIGIN - p, p - volMax);
  let outBox = length(max(toBox, vec3f(0.0)));
  if (outBox > 0.0) {
    return outBox + 0.5 * SPAN;
  }
  let v = (p - VOL_ORIGIN) / VOXEL;
  let cell = clamp(vec3i(floor(v / BRICK_USABLE)), vec3i(0), vec3i(GRID - 1));
  let e = rdIndir(cellIndex(cell));
  if ((e & IND_ALLOC) != 0u) {
    let lf = v - vec3f(cell) * BRICK_USABLE;
    return brickSample(e & IND_IDX_MASK, lf) * VOXEL;
  }
  // continuous conservative distance: Euclidean to the nearest surface
  // cell's center minus its half-diagonal (surface lies somewhere inside)
  // step floor matches the 2.1-span classify margin: any unallocated point
  // is >= ~0.6 span from the surface, so a 0.5-span floor can't overshoot
  // AND stays safely above the march hit epsilon (no phantom hits)
  let s = rdSeed(cellIndex(cell));
  var step = 0.5 * SPAN;
  if (s != 0xFFFFFFFFu) {
    let sc = vec3i(i32(s) % GRID, (i32(s) / GRID) % GRID, i32(s) / (GRID * GRID));
    let seedCenter = VOL_ORIGIN + (vec3f(sc) + 0.5) * SPAN;
    step = max(length(p - seedCenter) - 0.87 * SPAN, 0.5 * SPAN);
  }
  return select(step, -step, (e & IND_INSIDE) != 0u);
}

// Mid-estimate distance for AO taps (point queries, never marched):
// trilinear over the per-cell coarse field — smooth, no seed-contour
// banding, no clamp plateaus. Falls back to the accurate brick data
// inside allocated cells near the surface. Rest space.
fn charLooseRest(p: vec3f) -> f32 {
  let volMax = VOL_ORIGIN + vec3f(f32(GRID) * SPAN);
  let toBox = max(VOL_ORIGIN - p, p - volMax);
  let outBox = length(max(toBox, vec3f(0.0)));
  if (outBox > 0.0) {
    return outBox + 0.5 * SPAN;
  }
  let v = (p - VOL_ORIGIN) / VOXEL;
  let cell = clamp(vec3i(floor(v / BRICK_USABLE)), vec3i(0), vec3i(GRID - 1));
  let e = rdIndir(cellIndex(cell));
  if ((e & IND_ALLOC) != 0u) {
    let lf = v - vec3f(cell) * BRICK_USABLE;
    return brickSample(e & IND_IDX_MASK, lf) * VOXEL;
  }
  return coarseTrilinear(v);
}

// Cell-scale trilinear over the relaxed coarse scalars: smooth everywhere,
// no voxel-level gradient kinks.
fn coarseTrilinear(v: vec3f) -> f32 {
  let g = clamp(v / BRICK_USABLE - 0.5, vec3f(0.0), vec3f(f32(GRID) - 1.001));
  let c0 = vec3i(floor(g));
  let f = g - floor(g);
  let d000 = rdCoarse(cellIndex(c0));
  let d100 = rdCoarse(cellIndex(min(c0 + vec3i(1, 0, 0), vec3i(GRID - 1))));
  let d010 = rdCoarse(cellIndex(min(c0 + vec3i(0, 1, 0), vec3i(GRID - 1))));
  let d110 = rdCoarse(cellIndex(min(c0 + vec3i(1, 1, 0), vec3i(GRID - 1))));
  let d001 = rdCoarse(cellIndex(min(c0 + vec3i(0, 0, 1), vec3i(GRID - 1))));
  let d101 = rdCoarse(cellIndex(min(c0 + vec3i(1, 0, 1), vec3i(GRID - 1))));
  let d011 = rdCoarse(cellIndex(min(c0 + vec3i(0, 1, 1), vec3i(GRID - 1))));
  let d111 = rdCoarse(cellIndex(min(c0 + vec3i(1, 1, 1), vec3i(GRID - 1))));
  return mix(mix(mix(d000, d100, f.x), mix(d010, d110, f.x), f.y),
             mix(mix(d001, d101, f.x), mix(d011, d111, f.x), f.y), f.z);
}

fn albedoVoxel(brick: u32, v: vec3i) -> vec3f {
  let vi = u32(v.x + v.y * 8 + v.z * 64);
  return unpack4x8unorm(rdAlb(brick * 512u + vi)).rgb;
}

fn charAlbedoRest(p: vec3f) -> vec3f {
  // trilinear: nearest-voxel sampling posterizes the baked mottle into
  // contour swirls
  let v = (p - VOL_ORIGIN) / VOXEL;
  let cell = clamp(vec3i(floor(v / BRICK_USABLE)), vec3i(0), vec3i(GRID - 1));
  let e = rdIndir(cellIndex(cell));
  if ((e & IND_ALLOC) == 0u) {
    // blended chunk samples can land a hair outside the band; pull the
    // nearest surface brick's color via the JFA seed instead of debug grey
    let sIdx = rdSeed(cellIndex(cell));
    if (sIdx != 0xFFFFFFFFu) {
      let sc = vec3i(i32(sIdx) % GRID, (i32(sIdx) / GRID) % GRID,
                     i32(sIdx) / (GRID * GRID));
      let se = rdIndir(cellIndex(sc));
      if ((se & IND_ALLOC) != 0u) {
        return albedoVoxel(se & IND_IDX_MASK, vec3i(3, 3, 3));
      }
    }
    return vec3f(0.1);
  }
  let brick = e & IND_IDX_MASK;
  let clamped = clamp(v - vec3f(cell) * BRICK_USABLE, vec3f(0.0), vec3f(6.999));
  let i0 = vec3i(floor(clamped));
  let f = clamped - vec3f(i0);
  let c000 = albedoVoxel(brick, i0);
  let c100 = albedoVoxel(brick, i0 + vec3i(1, 0, 0));
  let c010 = albedoVoxel(brick, i0 + vec3i(0, 1, 0));
  let c110 = albedoVoxel(brick, i0 + vec3i(1, 1, 0));
  let c001 = albedoVoxel(brick, i0 + vec3i(0, 0, 1));
  let c101 = albedoVoxel(brick, i0 + vec3i(1, 0, 1));
  let c011 = albedoVoxel(brick, i0 + vec3i(0, 1, 1));
  let c111 = albedoVoxel(brick, i0 + vec3i(1, 1, 1));
  return mix(mix(mix(c000, c100, f.x), mix(c010, c110, f.x), f.y),
             mix(mix(c001, c101, f.x), mix(c011, c111, f.x), f.y), f.z);
}

// ---------- M-RIG: what used to live here ----------
// This block held the M4-P2 skinned articulation: CellSkin / cellSkinAt /
// skinInfl / chunkWarp / forwardResid — an up-to-4-bone inverse-LBS warp
// driven by the per-cell skin field, with a fixed-point refinement pass and a
// forward round-trip rejection test, run PER SAMPLE (~66 field evaluations per
// shaded pixel).
//
// It is gone because the thing it inverted is gone: the asset has no armature,
// no skin and no joint weights, so there is no linear blend to undo. Nothing
// below reads a bone id or a bone weight. See the brush rig immediately below.

// ---------- M-RIG: the skeleton-free brush rig ----------
// The asset has NO armature: no skin, no joints, no weights, no clips. Three
// meshes authored in place (body, one hand with a "grab" morph target, one
// eye). So articulation is no longer "undo a skin" — it is Claybook's brush
// model (docs/claybook/ANIMATION-AND-BUDGET.md): a small number of RIGID
// transforms of baked SDF regions, composited per frame.
//
// The trick that costs nothing: the rest volume box spans x,z in about
// [-0.708, +0.708], and the authored geometry only occupies x in
// [-0.209, +0.664] — the whole NEGATIVE-X HALF IS EMPTY. So all three brushes
// fit in the ONE existing volume, at three DISJOINT rest-space regions:
//
//   brush A  body       as authored,        x [-0.209, +0.209]
//   brush B  hand REST  as authored,        x [+0.398, +0.664]
//   brush C  hand GRAB  base + 1.0*morph, TRANSLATED into the empty half,
//                                           x [-0.586, -0.329]
//
// Zero new buffers, zero new bindings, zero new textures — which is how this
// satisfies CLAUDE.md trap 8 trivially (trace.wgsl's storage-buffer count is
// untouched, and it had no headroom to give).
//
// Runtime pieces, all sampling that one volume:
//   piece 0  BODY        the affine (spring squish -> shear lean -> yaw ->
//                        translate/hop about the feet). Unchanged.
//   piece 1  LEFT hand   rigid; samples brush B or C by POSE INDEX.
//   piece 2  RIGHT hand  rigid AND MIRRORED in x; samples brush B or C.
//
// The mirror is safe, and this was CONFIRMED rather than assumed: a mirror has
// determinant -1 but M^T M = I, so every singular value is exactly 1 and it is
// distance-preserving. mat3MinSingular() forms M^T M and takes the smallest
// eigenvalue, so it returns 1 for a mirror with no sign trouble, and
// matInvAffine() is a full signed-cofactor inverse (it never assumes
// orthonormality). Nothing in the Lipschitz rescale needs a special case.
//
// POSE SELECTION IS DISCRETE AND SNAPPED TO THE 12 Hz GRID, NEVER INTERPOLATED.
// Blending brush B toward brush C would reintroduce exactly the per-sample
// blending this design deletes, and a hand that eases into a grip is not what
// stop-motion looks like — it pops between held shapes. The CPU picks one
// brush per hand per pose step and writes its AABB and matrix; the shader
// never sees a weight.
//
// TRAP 3 SURVIVES UNCHANGED. The split is between the two REST samplers and
// both are still here: the march path calls charDistRest (conservative) and
// the AO/penumbra path calls charLooseRest (smooth). The rig only changes
// WHERE the rest field is sampled, never WHICH field.
//
// TRAP 4 SURVIVES TOO: every input (root, lean, spring squish, and now the
// pose index) is latched on the 12 Hz pose grid by the renderer, so the
// fighter steps rather than slides and frame reuse still fires between steps.

// CLIPPING, NOT OWNERSHIP — and do not put ownership back.
//
// This used to call cellOwnedBy(), which read the per-cell DOMINANT BONE out
// of the skin field and compared it against a bitmask of the bones a piece
// carried. There are no bones, so there is nothing to compare; the skin field
// is deleted.
//
// Its replacement is max(field, boxDist) against the piece's own rest AABB.
// An earlier experiment did exactly that and produced visible FLAT BANDS, so
// the reason it is correct NOW is worth stating precisely, because the old
// objection reads like it should still apply:
//
//   The bands appeared when every piece clipped a shared, OVERLAPPING body
//   region. A box drawn around one bone's share of a continuous blob CUTS
//   THROUGH SOLID CLAY — the clay carries on past the box face — so max()
//   sliced an axis-aligned plane across a round surface.
//
//   Here the three brushes are DISJOINT regions of rest space, separated by
//   ~0.12 m of empty volume (far more than the ~0.049 m narrow band). Each
//   brush's AABB therefore CONTAINS that brush's clay entirely and contains
//   no other brush's clay at all. max() never cuts anything: inside the box it
//   is the field verbatim, outside it is a lower bound on a surface that is
//   genuinely elsewhere. Exact, and no face ever grazes a surface.
//
// The clip is what MAKES the pieces separable now, so it is load-bearing, not
// an optimisation: without it every piece would draw every brush.
fn brushDist(q: vec3f, boxDist: f32) -> f32 {
  return max(charDistRest(q), boxDist);
}
fn brushLoose(q: vec3f, boxDist: f32) -> f32 {
  return max(charLooseRest(q), boxDist);
}

// Conservative (march) field under the brush rig.
fn charDistAffine(p: vec3f) -> f32 {
  let n = gPieceCount;
  let far = length(p - gFarCenter) - gFarR;
  if (far > 0.1) {
    return far;
  }
  var d = 1e9;
  for (var i = 0; i < n; i++) {
    // Fetch each bound field ONCE — pieceAt() carries a dynamic branch over
    // the two players' uniform arrays and re-fetching measured as a real cost.
    let pLo = pieceAt(i).aabbLo;
    let pHi = pieceAt(i).aabbHi;
    // aabbLo.w = the piece transform's SMALLEST SINGULAR VALUE. The body is a
    // shear plus a non-uniform scale, and a shear can have unit-length columns
    // while its smallest singular value is well under 1, so a column norm
    // would OVERESTIMATE world distances and the march would tunnel. For the
    // two hands this is exactly 1 (rigid, mirror included).
    let s = pLo.w;
    let q = (pieceAt(i).invSkin * vec4f(p, 1.0)).xyz;
    let toBox = max(pLo.xyz - q, q - pHi.xyz);
    let boxDist = length(max(toBox, vec3f(0.0)));
    if (boxDist > u.sceneMeta.z) {
      // outside the test shell: the tight box bounds this brush's zero set,
      // so its distance is a safe lower bound (and >= margin, so no stall)
      d = min(d, boxDist * s);
      continue;
    }
    // min, not smin. The brushes are separated by ~0.12 m of empty rest space
    // and the pieces land in the world as a blob plus two DETACHED mitts
    // (CLAUDE.md trap 7), so there is no seam to bridge; a smin at the packed
    // joint width (u.sceneMeta.y, ~2 voxels) would be an exact no-op that still
    // costs every lane. Attach the mitts to the body on some future rig and
    // this is the one line to change.
    d = min(d, brushDist(q, boxDist) * s);
  }
  return d;
}

// Smooth (AO/penumbra) twin — trap 3. Same pieces, same clip, but sampling
// charLooseRest: the relaxed per-cell coarse field instead of the conservative
// seed steps, because a conservative distance fed to AO or soft shadows reads
// as phantom occlusion and bands.
fn charLooseAffine(p: vec3f) -> f32 {
  let n = gPieceCount;
  let far = length(p - gFarCenter) - gFarR;
  if (far > 0.1) {
    return far;
  }
  var d = 1e9;
  for (var i = 0; i < n; i++) {
    let pLo = pieceAt(i).aabbLo;
    let pHi = pieceAt(i).aabbHi;
    let s = pLo.w;
    let q = (pieceAt(i).invSkin * vec4f(p, 1.0)).xyz;
    let toBox = max(pLo.xyz - q, q - pHi.xyz);
    let boxDist = length(max(toBox, vec3f(0.0)));
    if (boxDist > u.sceneMeta.z) {
      d = min(d, boxDist * s);
      continue;
    }
    d = min(d, brushLoose(q, boxDist) * s);
  }
  return d;
}

// World -> rest under the brush rig. The winner is the piece whose surface is
// nearest, which is the same contest charDistAffine minimises, so the returned
// point lands on the surface that was actually drawn.
//
// CARVING NOTE (CLAUDE.md trap 6): the point this returns is in the WINNING
// BRUSH's region. Carve the body and the wound lands in brush A, which is the
// body's only representation, so it persists. Carve a HAND while it is holding
// the grab pose and the wound lands in brush C — brush B, the same hand's rest
// shape, is a separate region and does not see it. A hand wound therefore
// belongs to the pose it was made in. That is inherent to one-volume-many-
// brushes and is not a bug in this function.
fn charRestPointAffine(p: vec3f) -> vec3f {
  let n = gPieceCount;
  var best = 1e9;
  var bestQ = p;
  for (var i = 0; i < n; i++) {
    let pLo = pieceAt(i).aabbLo;
    let pHi = pieceAt(i).aabbHi;
    let q = (pieceAt(i).invSkin * vec4f(p, 1.0)).xyz;
    let toBox = max(pLo.xyz - q, q - pHi.xyz);
    let boxDist = length(max(toBox, vec3f(0.0)));
    if (boxDist > u.sceneMeta.z) {
      continue;
    }
    let d = brushDist(q, boxDist) * pLo.w;
    if (d < best) {
      best = d;
      bestQ = q;
    }
  }
  return bestQ;
}

// TRIED AND REJECTED: a capsule-union early-out here (step by the distance to
// the inflated proxy capsules and skip the piece loop where that bound is
// comfortably positive). It is sound as a MARCH acceleration — a union of
// enclosing capsules is a lower bound, which is a safe sphere-trace step — and
// it was worth ~6.5 ms of 71 ms. It still shifted 1.5% of pixels, because
// map() feeds calcNormal as well as the march: when the gate fires on some of
// the four gradient taps and not others, the normal is built from a mix of
// true-field and bound values and the silhouette shading changes. Making it
// safe needs a march-only map() separate from the shading one. Note also that
// u.capsules[i].w is only a SHADOW PROXY fit and does NOT enclose the body.
fn charDistI(p: vec3f) -> f32 {
  let n = gPieceCount;
  if (n == 0) {
    // Un-rigged: the rest volume drawn where it was authored. Reachable two
    // ways — an analytic/rigless character, and the renderer's debug A/B
    // (look.affineRig off / CLAYFRAY_NO_AFFINE=1) which packs zero pieces.
    return charDistRest(p);
  }
  return charDistAffine(p);
}

fn charDistLooseI(p: vec3f) -> f32 {
  let n = gPieceCount;
  if (n == 0) {
    return charLooseRest(p);
  }
  return charLooseAffine(p);
}

// World point -> the REST-space point it was drawn from, i.e. the inverse of
// the articulation. Everything that has to address the brick volume by a point
// picked off the SCREEN needs this: the volume is authored in rest space, so a
// world position is only a valid brick address while the fighter sits unposed
// at the origin. Carving used to rely on exactly that accident (trap 6).
fn charRestPointI(p: vec3f) -> vec3f {
  let n = gPieceCount;
  if (n == 0) {
    return p;
  }
  return charRestPointAffine(p);
}

fn charAlbedoI(p: vec3f) -> vec3f {
  // albedo follows the nearest candidate surface, sampled at the same
  // blended point charDist uses
  return charAlbedoRest(charRestPointI(p));
}

// ---------- player entry points ----------
// Every fighter runs the same character, the same warp and the same sampling
// code; what differs is its slice of the volume and its own posed pieces. Each
// entry point states which fighter it wants, so there is no "restore the
// selector afterwards" dance to forget (the two-fighter version had to call
// usePlayer0() on the way out of every foe sampler).
//
// A DISABLED fighter must return 1e9 rather than sampling: its volume slice is
// stale clay from whenever it was last live, and the pieces uniform is zeroed,
// which would otherwise draw the rest volume unposed at the origin.
// info.x ALONE, deliberately: a live fighter with ZERO pieces is the un-rigged
// debug path (CLAYFRAY_NO_AFFINE / look.affineRig off), which draws the rest
// volume where it was authored via charDistRest. Gating on the piece count too
// would make that flag render nothing at all instead of answering "is the rig
// wrong or is the volume wrong?".
fn fighterActive(f: u32) -> bool {
  return u.fighters[f].info.x > 0.5;
}
fn fighterDist(f: u32, p: vec3f) -> f32 {
  if (!fighterActive(f)) {
    return 1e9;
  }
  usePlayer(f);
  return charDistI(p);
}
// smooth twin for AO/penumbra — the trap 3 split, per fighter
fn fighterDistLoose(f: u32, p: vec3f) -> f32 {
  if (!fighterActive(f)) {
    return 1e9;
  }
  usePlayer(f);
  return charDistLooseI(p);
}
fn fighterAlbedo(f: u32, p: vec3f) -> vec3f {
  usePlayer(f);
  return charAlbedoI(p);
}
fn fighterRestPoint(f: u32, p: vec3f) -> vec3f {
  usePlayer(f);
  return charRestPointI(p);
}
// ---------- PER-RAY FIGHTER CULLING ----------
// Which fighters the CURRENT ray can possibly reach, one bit each. All ones
// means "no culling", which is what every path other than the march wants.
//
// The problem it solves: map() takes the min over every live fighter at every
// march step, so a second body costs a point-sphere test per step per pixel
// and — for any ray entering its (generous) bound sphere — a full three-piece
// evaluation per step, whether or not the ray could ever hit it. Measured at
// 5.3 ms of a 23.3 ms frame, 23%, just for the opponent.
//
// A ray-sphere test done ONCE per pixel replaces ~35 point-sphere tests, and
// more importantly lets a ray that cannot hit a body skip its pieces outright.
//
// WHY SKIPPING IS SAFE FOR THE MARCH. Sphere tracing may only step by a
// distance that cannot overshoot a surface ON THE RAY. A fighter's clay lies
// entirely inside its bound sphere (info.z comes from affineBoundR over the
// posed piece boxes, plus slack), so a ray that never enters that sphere never
// touches that fighter's surface — omitting it from the min can return a
// LARGER distance, but there is no hit on this ray to overshoot.
//
// WHY IT IS NOT SAFE FOR ANYTHING ELSE, which is the trap that killed the last
// attempt at this (a capsule-union early-out, worth 6.5 ms of 71, reverted
// because it shifted 1.5% of pixels): calcNormal takes four taps OFF the ray,
// and AO and soft shadows march different rays entirely. A mask built from the
// primary ray is wrong for all of them. So the march sets it, and CLEARS IT
// IMMEDIATELY AFTER — see trace.wgsl's cs(). Everything downstream sees the
// full field.
//
// This is also the case where a branch actually pays, despite CLAUDE.md's
// "conditional skips do not pay here": that note is about per-SAMPLE region
// tests, where neighbouring pixels disagree and every lane runs the work
// anyway. A per-PIXEL fighter mask is spatially coherent — a tile either sees
// the opponent or it does not — so whole wavefronts take the same branch.
var<private> gRayMask: u32 = 0xFFFFFFFFu;

fn setRayMask(ro: vec3f, rd: vec3f) {
  var m = 0u;
  for (var f = 0u; f < u32(u.sceneMeta.x); f++) {
    // A fighter with no pieces is the un-rigged debug draw, where the bound
    // sphere describes the BODY brush but three brushes are drawn side by
    // side ~0.6 m apart. The sphere does not enclose them, so never cull it.
    if (u.fighters[f].info.y < 0.5) {
      m |= 1u << f;
      continue;
    }
    let oc = u.fighters[f].center.xyz - ro;
    // closest approach along the FORWARD ray; t=0 handles a ray starting
    // inside the sphere, which must never be culled
    let t = max(dot(oc, rd), 0.0);
    let q = oc - rd * t;
    let r = u.fighters[f].info.z;
    if (dot(q, q) <= r * r) {
      m |= 1u << f;
    }
  }
  gRayMask = m;
}
fn clearRayMask() { gRayMask = 0xFFFFFFFFu; }

// Nearest fighter's surface, over every live body. Returns (distance, index);
// index is -1 when no fighter is closer than `best`. This is the one place the
// fighter loop lives — map(), mapLoose(), mapPenumbra() and the pick shader all
// go through it, so adding a body cannot miss a call site.
fn fightersNearest(p: vec3f, best: f32) -> vec2f {
  var d = best;
  var hit = -1.0;
  for (var f = 0u; f < u32(u.sceneMeta.x); f++) {
    if ((gRayMask & (1u << f)) == 0u) {
      continue;
    }
    let df = fighterDist(f, p);
    if (df < d) {
      d = df;
      hit = f32(f);
    }
  }
  return vec2f(d, hit);
}
fn fightersDistLoose(p: vec3f, best: f32) -> f32 {
  var d = best;
  for (var f = 0u; f < u32(u.sceneMeta.x); f++) {
    d = min(d, fighterDistLoose(f, p));
  }
  return d;
}
