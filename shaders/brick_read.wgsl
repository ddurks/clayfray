// Read-side brickmap: sparse SDF volume for the character body.
// Bricks are 8^3 voxels covering 7^3 unique cells (1-voxel overlap for
// seamless trilinear sampling). Distances stored as f16 pairs in voxel
// units, clamped to a +-4 voxel narrow band. Empty cells carry a coarse
// Chebyshev distance (in brick units) maintained by JFA after every edit.
//
// Includers must not redeclare group(1).

const GRID: i32 = 74;              // brick cells per axis
const BRICK_USABLE: f32 = 7.0;     // unique voxels per brick per axis
const VOXEL: f32 = 0.0027344;      // meters
const SPAN: f32 = 0.0191406;       // BRICK_USABLE * VOXEL
const VOL_ORIGIN: vec3f = vec3f(-0.7082, -0.1582, -0.7082);
const BAND: f32 = 12.0;            // band half-width ~ cell diagonal: allocated
                                   // cells report TRUE distance everywhere, so
                                   // AO/shadow taps never hit a clamp plateau

const IND_ALLOC: u32 = 0x80000000u;
const IND_INSIDE: u32 = 0x40000000u;
const IND_IDX_MASK: u32 = 0x000FFFFFu;
const IND_CHEB_MASK: u32 = 0x000000FFu;

@group(1) @binding(0) var<storage, read> bIndirection: array<u32>;
@group(1) @binding(1) var<storage, read> bDist: array<u32>;    // 256 u32 per brick (512 f16)
@group(1) @binding(2) var<storage, read> bAlbedo: array<u32>;  // 512 u32 per brick
@group(1) @binding(3) var<storage, read> bSeeds: array<u32>;   // nearest surface cell (JFA)
@group(1) @binding(4) var<storage, read> bCoarse: array<f32>;  // per-cell signed distance (m)
@group(1) @binding(6) var<storage, read> bCellW: array<u32>;    // per-cell 4-slot skin: [joints u8x4][weights u8x4]

fn cellIndex(c: vec3i) -> u32 {
  return u32(c.x + c.y * GRID + c.z * GRID * GRID);
}

fn brickVoxel(brick: u32, v: vec3i) -> f32 {
  let vi = u32(v.x + v.y * 8 + v.z * 64);
  let word = bDist[brick * 256u + vi / 2u];
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
  let e = bIndirection[cellIndex(cell)];
  if ((e & IND_ALLOC) != 0u) {
    let lf = v - vec3f(cell) * BRICK_USABLE;
    return brickSample(e & IND_IDX_MASK, lf) * VOXEL;
  }
  // continuous conservative distance: Euclidean to the nearest surface
  // cell's center minus its half-diagonal (surface lies somewhere inside)
  // step floor matches the 2.1-span classify margin: any unallocated point
  // is >= ~0.6 span from the surface, so a 0.5-span floor can't overshoot
  // AND stays safely above the march hit epsilon (no phantom hits)
  let s = bSeeds[cellIndex(cell)];
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
  let e = bIndirection[cellIndex(cell)];
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
  let d000 = bCoarse[cellIndex(c0)];
  let d100 = bCoarse[cellIndex(min(c0 + vec3i(1, 0, 0), vec3i(GRID - 1)))];
  let d010 = bCoarse[cellIndex(min(c0 + vec3i(0, 1, 0), vec3i(GRID - 1)))];
  let d110 = bCoarse[cellIndex(min(c0 + vec3i(1, 1, 0), vec3i(GRID - 1)))];
  let d001 = bCoarse[cellIndex(min(c0 + vec3i(0, 0, 1), vec3i(GRID - 1)))];
  let d101 = bCoarse[cellIndex(min(c0 + vec3i(1, 0, 1), vec3i(GRID - 1)))];
  let d011 = bCoarse[cellIndex(min(c0 + vec3i(0, 1, 1), vec3i(GRID - 1)))];
  let d111 = bCoarse[cellIndex(min(c0 + vec3i(1, 1, 1), vec3i(GRID - 1)))];
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
  return unpack4x8unorm(bAlbedo[brick * 512u + vi]).rgb;
}

fn charAlbedoRest(p: vec3f) -> vec3f {
  // trilinear: nearest-voxel sampling posterizes the baked mottle into
  // contour swirls
  let v = (p - VOL_ORIGIN) / VOXEL;
  let cell = clamp(vec3i(floor(v / BRICK_USABLE)), vec3i(0), vec3i(GRID - 1));
  let e = bIndirection[cellIndex(cell)];
  if ((e & IND_ALLOC) == 0u) {
    // blended chunk samples can land a hair outside the band; pull the
    // nearest surface brick's color via the JFA seed instead of debug grey
    let sIdx = bSeeds[cellIndex(cell)];
    if (sIdx != 0xFFFFFFFFu) {
      let sc = vec3i(i32(sIdx) % GRID, (i32(sIdx) / GRID) % GRID,
                     i32(sIdx) / (GRID * GRID));
      let se = bIndirection[cellIndex(sc)];
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

// ---------- M4-P2: chunk articulation with an N-bone smooth warp ----------
// The posed body is a union over per-bone pieces of the ONE rest-space
// brickmap, each sampled at a warped point. The warp is up-to-4-bone inverse
// LBS driven by a per-cell skin field (bCellW, distance-weighted gather at
// import, flood-filled volume-wide): at a piece's rigidly inverse-posed
// point, gather the governing bones and their trilinear influences, and
// blend the candidate rigid warps by those influences. A convex combination
// of rigid warps is a contraction, so sampled distances stay safe bounds.
// The piece whose influence is (near-)maximal renders the region — no fixed
// thresholds, so 4-bone junctions (hands) resolve as well as simple joints.
// boneMeta: x = piece count (0 = un-rigged: rest field directly),
//           y = joint smin k, z = box test margin, w = posed bound radius.

struct CellSkin {
  cw: array<vec2u, 8>, // 8 cell corners: x = joints word, y = weights word
  f: vec3f,            // trilinear fractions
  jw: u32,             // nearest cell's joints word (candidate set)
}

fn cellSkinAt(q: vec3f) -> CellSkin {
  let v = (q - VOL_ORIGIN) / VOXEL;
  var out: CellSkin;
  let cellN = clamp(vec3i(floor(v / BRICK_USABLE)), vec3i(0), vec3i(GRID - 1));
  out.jw = bCellW[cellIndex(cellN) * 2u];
  let g = clamp(v / BRICK_USABLE - 0.5, vec3f(0.0), vec3f(f32(GRID) - 1.001));
  let c0 = vec3i(floor(g));
  out.f = g - floor(g);
  for (var k = 0; k < 8; k++) {
    let c = min(c0 + vec3i(k & 1, (k >> 1) & 1, (k >> 2) & 1), vec3i(GRID - 1));
    let ci = cellIndex(c) * 2u;
    out.cw[k] = vec2u(bCellW[ci], bCellW[ci + 1u]);
  }
  return out;
}

// trilinear influence of one bone over the gathered corners
fn skinInfl(sk: CellSkin, bone: u32) -> f32 {
  var acc: array<f32, 8>;
  for (var k = 0; k < 8; k++) {
    let jw = sk.cw[k].x;
    let ww = sk.cw[k].y;
    var w = 0.0;
    for (var sl = 0u; sl < 4u; sl++) {
      if (((jw >> (sl * 8u)) & 0xFFu) == bone) {
        w = max(w, f32((ww >> (sl * 8u)) & 0xFFu) / 255.0);
      }
    }
    acc[k] = w;
  }
  return mix(mix(mix(acc[0], acc[1], sk.f.x), mix(acc[2], acc[3], sk.f.x), sk.f.y),
             mix(mix(acc[4], acc[5], sk.f.x), mix(acc[6], acc[7], sk.f.x), sk.f.y),
             sk.f.z);
}

struct WarpOut {
  q: vec3f,      // blended rest-space sample point
  inflSelf: f32, // this bone's influence there
  inflMax: f32,  // strongest candidate influence (render if self is near it)
  resid: f32,    // forward round-trip error |skinBlend(q) - p| (m)
}

// N-bone inverse blend at rest-guess q for piece `self`. Candidates come
// from the nearest cell's joint slots.
fn chunkWarp(p: vec3f, q: vec3f, selfIdx: i32, bone: u32,
             qs: ptr<function, array<vec3f, 16>>,
             boneToPiece: ptr<function, array<i32, 16>>) -> WarpOut {
  let sk = cellSkinAt(q);
  var acc = vec3f(0.0);
  var wsum = 0.0;
  var inflMax = 0.0;
  var seenBone = false;
  var cands: array<u32, 4>;
  var infls: array<f32, 4>;
  var nc = 0;
  for (var sl = 0u; sl < 4u; sl++) {
    let cand = (sk.jw >> (sl * 8u)) & 0xFFu;
    // skip duplicate slots (padding repeats joints)
    var dup = false;
    for (var m = 0u; m < sl; m++) {
      if (((sk.jw >> (m * 8u)) & 0xFFu) == cand) {
        dup = true;
      }
    }
    if (dup) {
      continue;
    }
    let infl = skinInfl(sk, cand);
    if (infl <= 0.0) {
      continue;
    }
    inflMax = max(inflMax, infl);
    if (cand == bone) {
      seenBone = true;
    }
    if (cand < 16u && (*boneToPiece)[cand] >= 0) {
      acc += infl * (*qs)[(*boneToPiece)[cand]];
      wsum += infl;
      cands[nc] = cand;
      infls[nc] = infl;
      nc++;
    }
  }
  var inflSelf = 0.0;
  if (seenBone) {
    inflSelf = skinInfl(sk, bone);
  }
  var out: WarpOut;
  out.inflSelf = inflSelf;
  out.inflMax = max(inflMax, 1e-4);
  out.resid = 0.0;
  if (wsum > 1e-4) {
    out.q = acc / wsum;
  } else {
    out.q = (*qs)[selfIdx];
  }
  return out;
}

// Forward round-trip using the weights AT the sampled rest point — i.e.
// where the game's LBS would actually place that piece of surface. Genuine
// samples land back within ~a couple cm even in hard blend bands (inverse
// approximation error); conjured samples (world air where a limb was)
// forward-map onto the POSED limb, tens of cm away. One generous threshold
// separates them without starving armpit/shoulder coverage.
fn forwardResid(p: vec3f, qh: vec3f,
                boneToPiece: ptr<function, array<i32, 16>>) -> f32 {
  let v = (qh - VOL_ORIGIN) / VOXEL;
  let cell = clamp(vec3i(floor(v / BRICK_USABLE)), vec3i(0), vec3i(GRID - 1));
  let ci = cellIndex(cell) * 2u;
  let jw = bCellW[ci];
  let ww = bCellW[ci + 1u];
  var back = vec3f(0.0);
  var wsum = 0.0;
  for (var sl = 0u; sl < 4u; sl++) {
    let w8 = (ww >> (sl * 8u)) & 0xFFu;
    if (w8 == 0u) {
      continue;
    }
    let j = (jw >> (sl * 8u)) & 0xFFu;
    if (j < 16u && (*boneToPiece)[j] >= 0) {
      let pi = (*boneToPiece)[j];
      back += f32(w8) * (u.pieces[pi].skin * vec4f(qh, 1.0)).xyz;
      wsum += f32(w8);
    }
  }
  if (wsum < 1.0) {
    return 0.0; // no data: don't reject
  }
  return length(back / wsum - p);
}

fn charDist(p: vec3f) -> f32 {
  let n = i32(u.boneMeta.x);
  if (n == 0) {
    return charDistRest(p);
  }
  let far = length(p - u.capsCenter.xyz) - u.boneMeta.w;
  if (far > 0.1) {
    return far;
  }
  var qs: array<vec3f, 16>;
  var boneToPiece: array<i32, 16>;
  for (var i = 0; i < 16; i++) {
    boneToPiece[i] = -1;
  }
  for (var i = 0; i < n; i++) {
    qs[i] = (u.pieces[i].invSkin * vec4f(p, 1.0)).xyz;
    let b = u32(u.pieces[i].capB.w);
    if (b < 16u) {
      boneToPiece[b] = i;
    }
  }
  var d = 1e9;
  var bestInfl = -1.0;
  var bestPd = 1e9;
  var rendered = false;
  for (var i = 0; i < n; i++) {
    // aabbLo.w = min scale of the skin matrix: converts rest-space distance
    // to a safe world-space bound under squash/stretch
    let s = u.pieces[i].aabbLo.w;
    let toBox = max(u.pieces[i].aabbLo.xyz - qs[i], qs[i] - u.pieces[i].aabbHi.xyz);
    let boxDist = length(max(toBox, vec3f(0.0)));
    if (boxDist > u.boneMeta.z) {
      // outside the test shell: the tight box bounds the piece's zero set,
      // so its distance is a safe lower bound (and >= margin: no stall)
      d = min(d, boxDist * s);
      continue;
    }
    let bone = u32(u.pieces[i].capB.w);
    // fixed-point refinement: influences read at the rigid guess are off by
    // the warp displacement, so re-evaluate at the blended location. But the
    // correction is proportional to that displacement — near-rigid regions
    // (small joint angle, the bulk of the body most frames) converge in one
    // pass, so skip the second ~17-load cell gather where the blend barely
    // moved the sample. Bent joints (large displacement) keep both passes.
    var w = chunkWarp(p, qs[i], i, bone, &qs, &boneToPiece);
    if (distance(w.q, qs[i]) > 0.006) { // ~0.3 cell spans
      w = chunkWarp(p, w.q, i, bone, &qs, &boneToPiece);
    }
    if (forwardResid(p, w.q, &boneToPiece) > 0.05) {
      continue; // vacated-space sample: LBS puts that surface elsewhere
    }
    let pd = charDistRest(w.q) * s;
    if (w.inflSelf > bestInfl) {
      bestInfl = w.inflSelf;
      bestPd = pd;
    }
    if (w.inflSelf < w.inflMax - 0.06) {
      continue; // a stronger bone's piece renders this region
    }
    rendered = true;
    d = min(d, pd);
  }
  // coverage guarantee for spots where refinement flips every piece away;
  // gated so blended samples can't conjure surface far from any influence
  if (!rendered && bestInfl >= 0.3) {
    d = min(d, bestPd);
  }
  return d;
}

fn charDistLoose(p: vec3f) -> f32 {
  let n = i32(u.boneMeta.x);
  if (n == 0) {
    return charLooseRest(p);
  }
  let far = length(p - u.capsCenter.xyz) - u.boneMeta.w;
  if (far > 0.1) {
    return far;
  }
  var qs: array<vec3f, 16>;
  var boneToPiece: array<i32, 16>;
  for (var i = 0; i < 16; i++) {
    boneToPiece[i] = -1;
  }
  for (var i = 0; i < n; i++) {
    qs[i] = (u.pieces[i].invSkin * vec4f(p, 1.0)).xyz;
    let b = u32(u.pieces[i].capB.w);
    if (b < 16u) {
      boneToPiece[b] = i;
    }
  }
  var d = 1e9;
  var bestInfl = -1.0;
  var bestPd = 1e9;
  var rendered = false;
  for (var i = 0; i < n; i++) {
    let s = u.pieces[i].aabbLo.w;
    let toBox = max(u.pieces[i].aabbLo.xyz - qs[i], qs[i] - u.pieces[i].aabbHi.xyz);
    let boxDist = length(max(toBox, vec3f(0.0)));
    if (boxDist > u.boneMeta.z) {
      d = min(d, boxDist * s);
      continue;
    }
    let bone = u32(u.pieces[i].capB.w);
    // single-pass warp: AO/penumbra tolerate the coarser estimate, and this
    // path runs 40+ times per shadow ray
    let w = chunkWarp(p, qs[i], i, bone, &qs, &boneToPiece);
    if (forwardResid(p, w.q, &boneToPiece) > 0.05) {
      continue;
    }
    let pd = charLooseRest(w.q) * s;
    if (w.inflSelf > bestInfl) {
      bestInfl = w.inflSelf;
      bestPd = pd;
    }
    if (w.inflSelf < w.inflMax - 0.06) {
      continue;
    }
    rendered = true;
    d = min(d, pd);
  }
  if (!rendered && bestInfl >= 0.3) {
    d = min(d, bestPd);
  }
  return d;
}

fn charAlbedo(p: vec3f) -> vec3f {
  let n = i32(u.boneMeta.x);
  if (n == 0) {
    return charAlbedoRest(p);
  }
  var qs: array<vec3f, 16>;
  var boneToPiece: array<i32, 16>;
  for (var i = 0; i < 16; i++) {
    boneToPiece[i] = -1;
  }
  for (var i = 0; i < n; i++) {
    qs[i] = (u.pieces[i].invSkin * vec4f(p, 1.0)).xyz;
    let b = u32(u.pieces[i].capB.w);
    if (b < 16u) {
      boneToPiece[b] = i;
    }
  }
  // albedo follows the nearest candidate surface, sampled at the same
  // blended point charDist uses
  var best = 1e9;
  var q = p;
  for (var i = 0; i < n; i++) {
    let toBox = max(u.pieces[i].aabbLo.xyz - qs[i], qs[i] - u.pieces[i].aabbHi.xyz);
    if (length(max(toBox, vec3f(0.0))) > u.boneMeta.z) {
      continue;
    }
    let bone = u32(u.pieces[i].capB.w);
    var w = chunkWarp(p, qs[i], i, bone, &qs, &boneToPiece);
    w = chunkWarp(p, w.q, i, bone, &qs, &boneToPiece);
    if (forwardResid(p, w.q, &boneToPiece) > 0.05) {
      continue;
    }
    let d = charDistRest(w.q) * u.pieces[i].aabbLo.w - w.inflSelf * 0.001;
    if (d < best) {
      best = d;
      q = w.q;
    }
  }
  return charAlbedoRest(q);
}
