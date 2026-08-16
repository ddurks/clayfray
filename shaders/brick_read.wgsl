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
const IND_CHEB_MASK: u32 = 0x000000FFu;

// The three per-cell arrays (indirection, JFA seeds, coarse distance) are ONE
// buffer here, addressed through the CELL_* base indices in the //#constants
// block. That is not packing for its own sake: a Metal shader stage gets 10
// storage buffers, and two fighters plus the ground at one binding per array
// needs 14 — the trace pipeline would not create at all. See the region map in
// src/brick.h. (There was a fourth region, the per-cell skin field; M-RIG
// deleted it along with the skeleton.)
@group(1) @binding(0) var<storage, read> bCells: array<u32>;
@group(1) @binding(1) var<storage, read> bDist: array<u32>;    // 256 u32 per brick (512 f16)
@group(1) @binding(2) var<storage, read> bAlbedo: array<u32>;  // 512 u32 per brick

// ---------- fighter 1 (the opponent) ----------
// A second body, in its own volume. Same layout, same code: every read below
// goes through an accessor that picks the volume by `gFighter`, so one set of
// sampling functions serves both fighters and nothing here is duplicated.
// Fighter 1 runs the same brush rig as the hero, driven by its own pose.
@group(2) @binding(0) var<storage, read> fCells: array<u32>;
@group(2) @binding(1) var<storage, read> fDist: array<u32>;
@group(2) @binding(2) var<storage, read> fAlbedo: array<u32>;

// Which body the sampling functions are currently reading, and its pose data.
// Set them, call, set them back — see foeDist/foeAlbedo at the bottom. This is
// what lets ONE copy of the warp serve every player.
var<private> gFighter: u32 = 0u;
var<private> gPieceCount: i32 = 0;
var<private> gFarCenter: vec3f = vec3f(0.0);
var<private> gFarR: f32 = 0.0;

fn usePlayer0() {
  gFighter = 0u;
  gPieceCount = i32(u.boneMeta.x);
  gFarCenter = u.capsCenter.xyz;
  gFarR = u.boneMeta.w;
}
fn usePlayer1() {
  gFighter = 1u;
  gPieceCount = i32(u.foeBoneMeta.x);
  gFarCenter = u.foeCenter.xyz;
  gFarR = u.foeMeta.y;
}
// one piece of whichever player is selected
fn pieceAt(i: i32) -> Piece {
  if (gFighter == 0u) { return u.pieces[i]; }
  return u.foePieces[i];
}

fn rdIndir(i: u32) -> u32 {
  if (gFighter == 0u) { return bCells[CELL_IND + i]; }
  return fCells[CELL_IND + i];
}
fn rdDist(i: u32) -> u32 {
  if (gFighter == 0u) { return bDist[i]; }
  return fDist[i];
}
fn rdAlb(i: u32) -> u32 {
  if (gFighter == 0u) { return bAlbedo[i]; }
  return fAlbedo[i];
}
fn rdSeed(i: u32) -> u32 {
  if (gFighter == 0u) { return bCells[CELL_SEED + i]; }
  return fCells[CELL_SEED + i];
}
// The coarse region is f32 data in the shared u32 buffer.
fn rdCoarse(i: u32) -> f32 {
  if (gFighter == 0u) { return bitcast<f32>(bCells[CELL_COARSE + i]); }
  return bitcast<f32>(fCells[CELL_COARSE + i]);
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

// Penumbra flavor: smoothness beats accuracy. The trilinear brick data has
// per-voxel gradient kinks that paint fingerprint bands into soft shadows;
// the coarse cell field is smooth everywhere.
fn charDistSmooth(p: vec3f) -> f32 {
  let volMax = VOL_ORIGIN + vec3f(f32(GRID) * SPAN);
  let toBox = max(VOL_ORIGIN - p, p - volMax);
  let outBox = length(max(toBox, vec3f(0.0)));
  if (outBox > 0.0) {
    return outBox + 0.5 * SPAN;
  }
  return coarseTrilinear((p - VOL_ORIGIN) / VOXEL);
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
    if (boxDist > u.boneMeta.z) {
      // outside the test shell: the tight box bounds this brush's zero set,
      // so its distance is a safe lower bound (and >= margin, so no stall)
      d = min(d, boxDist * s);
      continue;
    }
    // min, not smin. The brushes are separated by ~0.12 m of empty rest space
    // and the pieces land in the world as a blob plus two DETACHED mitts
    // (CLAUDE.md trap 7), so there is no seam to bridge; a smin at the packed
    // joint width (u.boneMeta.y, ~2 voxels) would be an exact no-op that still
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
    if (boxDist > u.boneMeta.z) {
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
    if (boxDist > u.boneMeta.z) {
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
// The hero is player 0; each call re-selects it because the opponent's
// sampling leaves the private selectors pointing elsewhere.
fn charDist(p: vec3f) -> f32 {
  usePlayer0();
  return charDistI(p);
}
fn charDistLoose(p: vec3f) -> f32 {
  usePlayer0();
  return charDistLooseI(p);
}
fn charAlbedo(p: vec3f) -> vec3f {
  usePlayer0();
  return charAlbedoI(p);
}
fn charRestPoint(p: vec3f) -> vec3f {
  usePlayer0();
  return charRestPointI(p);
}

// Player 1: same character, same warp, its OWN volume and its own pose. The
// root transform is folded into its piece matrices exactly like the hero's,
// so it animates rather than standing rigid.
fn foeActive() -> bool {
  return u.foeMeta.x > 0.5;
}
fn foeDist(p: vec3f) -> f32 {
  if (!foeActive() || u.foeBoneMeta.x < 0.5) {
    return 1e9;
  }
  usePlayer1();
  let d = charDistI(p);
  usePlayer0();
  return d;
}
// smooth twin for AO/penumbra — same split the hero's field keeps (trap 3)
fn foeDistLoose(p: vec3f) -> f32 {
  if (!foeActive() || u.foeBoneMeta.x < 0.5) {
    return 1e9;
  }
  usePlayer1();
  let d = charDistLooseI(p);
  usePlayer0();
  return d;
}
fn foeAlbedo(p: vec3f) -> vec3f {
  usePlayer1();
  let c = charAlbedoI(p);
  usePlayer0();
  return c;
}
