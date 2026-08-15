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

// The four per-cell arrays (indirection, JFA seeds, coarse distance, cell
// skin weights) are ONE buffer here, addressed through the CELL_* base
// indices in the //#constants block. That is not packing for its own sake:
// a Metal shader stage gets 10 storage buffers, and two fighters plus the
// ground at one binding per array needs 14 — the trace pipeline would not
// create at all. See the region map in src/brick.h.
@group(1) @binding(0) var<storage, read> bCells: array<u32>;
@group(1) @binding(1) var<storage, read> bDist: array<u32>;    // 256 u32 per brick (512 f16)
@group(1) @binding(2) var<storage, read> bAlbedo: array<u32>;  // 512 u32 per brick

// ---------- fighter 1 (the opponent) ----------
// A second body, in its own volume. Same layout, same code: every read below
// goes through an accessor that picks the volume by `gFighter`, so one set of
// sampling functions serves both fighters and nothing here is duplicated.
// Fighter 1 is RIGID (it stands and gets cut, it does not articulate), so its
// cell-weight region is allocated but never read.
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
// Skin weights: fighter 0 only (see above), so no gFighter branch.
fn rdCellW(i: u32) -> u32 { return bCells[CELL_W + i]; }

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
  out.jw = rdCellW(cellIndex(cellN) * 2u);
  let g = clamp(v / BRICK_USABLE - 0.5, vec3f(0.0), vec3f(f32(GRID) - 1.001));
  let c0 = vec3i(floor(g));
  out.f = g - floor(g);
  for (var k = 0; k < 8; k++) {
    let c = min(c0 + vec3i(k & 1, (k >> 1) & 1, (k >> 2) & 1), vec3i(GRID - 1));
    let ci = cellIndex(c) * 2u;
    out.cw[k] = vec2u(rdCellW(ci), rdCellW(ci + 1u));
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
  // `self`'s influence is one of the candidate influences computed below —
  // the loop already evaluates skinInfl for the slot where cand == bone, so
  // capture it there instead of re-running that 8x4 gather afterwards.
  var inflSelf = 0.0;
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
      inflSelf = infl;
    }
    if (cand < 16u && (*boneToPiece)[cand] >= 0) {
      acc += infl * (*qs)[(*boneToPiece)[cand]];
      wsum += infl;
    }
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
  let jw = rdCellW(ci);
  let ww = rdCellW(ci + 1u);
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
      back += f32(w8) * (pieceAt(pi).skin * vec4f(qh, 1.0)).xyz;
      wsum += f32(w8);
    }
  }
  if (wsum < 1.0) {
    return 0.0; // no data: don't reject
  }
  return length(back / wsum - p);
}

// TRIED AND REJECTED: a capsule-union early-out here (step by the distance to
// the inflated bone capsules and skip the warp where that bound is comfortably
// positive). It is sound as a MARCH acceleration — a union of enclosing
// capsules is a lower bound, which is a safe sphere-trace step — and it was
// worth ~6.5 ms of 71 ms. It still shifted 1.5% of pixels, because map() feeds
// calcNormal as well as the march: when the gate fires on some of the four
// gradient taps and not others, the normal is built from a mix of true-field
// and bound values and the silhouette shading changes. Making it safe needs a
// march-only map() separate from the shading one. Note also that capsules[i].w
// is an 80th-percentile fit (anim.cpp) and does NOT enclose the body — only
// pieces[i].capA.w (rPiece = max + 3.5 cm) does.
fn charDistI(p: vec3f) -> f32 {
  let n = gPieceCount;
  if (n == 0) {
    return charDistRest(p);
  }
  let far = length(p - gFarCenter) - gFarR;
  if (far > 0.1) {
    return far;
  }
  var qs: array<vec3f, 16>;
  var boneToPiece: array<i32, 16>;
  for (var i = 0; i < 16; i++) {
    boneToPiece[i] = -1;
  }
  for (var i = 0; i < n; i++) {
    qs[i] = (pieceAt(i).invSkin * vec4f(p, 1.0)).xyz;
    let b = u32(pieceAt(i).capB.w);
    if (b < 16u) {
      boneToPiece[b] = i;
    }
  }
  var d = 1e9;
  var bestInfl = -1.0;
  var bestPd = 1e9;
  var rendered = false;
  for (var i = 0; i < n; i++) {
    // Fetch the bound fields ONCE. pieceAt() carries a dynamic branch over
    // the two players' uniform arrays, and aabbLo was being re-fetched for
    // its .w and its .xyz separately — four pieceAt calls per iteration on
    // the hottest loop in the renderer.
    let pLo = pieceAt(i).aabbLo;
    let pHi = pieceAt(i).aabbHi;
    // aabbLo.w = min scale of the skin matrix: converts rest-space distance
    // to a safe world-space bound under squash/stretch
    let s = pLo.w;
    let toBox = max(pLo.xyz - qs[i], qs[i] - pHi.xyz);
    let boxDist = length(max(toBox, vec3f(0.0)));
    if (boxDist > u.boneMeta.z) {
      // outside the test shell: the tight box bounds the piece's zero set,
      // so its distance is a safe lower bound (and >= margin: no stall)
      d = min(d, boxDist * s);
      continue;
    }
    let bone = u32(pieceAt(i).capB.w);
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
    if (forwardResid(p, w.q, &boneToPiece) > WARP_RESID_TOL) {
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

fn charDistLooseI(p: vec3f) -> f32 {
  let n = gPieceCount;
  if (n == 0) {
    return charLooseRest(p);
  }
  let far = length(p - gFarCenter) - gFarR;
  if (far > 0.1) {
    return far;
  }
  // NO proxy gate here, however tempting: this is the AO/penumbra field, and
  // the gate returns a CONSERVATIVE bound. Feeding a conservative distance
  // into AO or soft shadows is CLAUDE.md trap 3 — it reads as phantom
  // occlusion and bands. Measured: gating here shifted 16% of pixels.
  // The march path (charDistI) is the one that wants a conservative bound.
  var qs: array<vec3f, 16>;
  var boneToPiece: array<i32, 16>;
  for (var i = 0; i < 16; i++) {
    boneToPiece[i] = -1;
  }
  for (var i = 0; i < n; i++) {
    qs[i] = (pieceAt(i).invSkin * vec4f(p, 1.0)).xyz;
    let b = u32(pieceAt(i).capB.w);
    if (b < 16u) {
      boneToPiece[b] = i;
    }
  }
  var d = 1e9;
  var bestInfl = -1.0;
  var bestPd = 1e9;
  var rendered = false;
  for (var i = 0; i < n; i++) {
    let pLo = pieceAt(i).aabbLo;
    let pHi = pieceAt(i).aabbHi;
    let s = pLo.w;
    let toBox = max(pLo.xyz - qs[i], qs[i] - pHi.xyz);
    let boxDist = length(max(toBox, vec3f(0.0)));
    if (boxDist > u.boneMeta.z) {
      d = min(d, boxDist * s);
      continue;
    }
    let bone = u32(pieceAt(i).capB.w);
    // single-pass warp: AO/penumbra tolerate the coarser estimate, and this
    // path runs 40+ times per shadow ray
    let w = chunkWarp(p, qs[i], i, bone, &qs, &boneToPiece);
    if (forwardResid(p, w.q, &boneToPiece) > WARP_RESID_TOL) {
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

// World point -> the REST-space point it was warped from, i.e. the inverse of
// the articulation. Everything that has to address the brick volume by a
// point picked off the SCREEN needs this: the volume is authored in rest
// space, so a world position is only a valid brick address while the fighter
// sits unposed at the origin. Carving used to rely on exactly that accident.
fn charRestPointI(p: vec3f) -> vec3f {
  let n = gPieceCount;
  if (n == 0) {
    return p;
  }
  var qs: array<vec3f, 16>;
  var boneToPiece: array<i32, 16>;
  for (var i = 0; i < 16; i++) {
    boneToPiece[i] = -1;
  }
  for (var i = 0; i < n; i++) {
    qs[i] = (pieceAt(i).invSkin * vec4f(p, 1.0)).xyz;
    let b = u32(pieceAt(i).capB.w);
    if (b < 16u) {
      boneToPiece[b] = i;
    }
  }
  // the winning piece is the one whose warped surface is nearest — the same
  // contest charDist minimises, so the point lands on the surface drawn
  var best = 1e9;
  var q = p;
  for (var i = 0; i < n; i++) {
    let pLo = pieceAt(i).aabbLo;
    let pHi = pieceAt(i).aabbHi;
    let toBox = max(pLo.xyz - qs[i], qs[i] - pHi.xyz);
    if (length(max(toBox, vec3f(0.0))) > u.boneMeta.z) {
      continue;
    }
    let bone = u32(pieceAt(i).capB.w);
    var w = chunkWarp(p, qs[i], i, bone, &qs, &boneToPiece);
    w = chunkWarp(p, w.q, i, bone, &qs, &boneToPiece);
    if (forwardResid(p, w.q, &boneToPiece) > WARP_RESID_TOL) {
      continue;
    }
    let d = charDistRest(w.q) * pLo.w - w.inflSelf * 0.001;
    if (d < best) {
      best = d;
      q = w.q;
    }
  }
  return q;
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
