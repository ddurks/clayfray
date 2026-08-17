// Brickmap bake + edit passes. One pipeline pair used for everything:
//   classify: one thread per cell in the region -> allocate fresh bricks
//   fill:     one workgroup per cell -> write voxels, free emptied bricks
// Modes: 0 = bake analytic character, 1 = carve (CSG subtract brush),
//        2 = add clay (min with brush, stamps brush albedo),
//        3 = dent (volume-conserving displacement — no clay leaves the body),
//        4 = paint (albedo only — no allocation, no JFA, no ledger).
//
// 3 and 4 are the CHEAP modes and they are cheap for the same reason: neither
// creates surface where there was none, so neither runs classify at all (the
// CPU skips the dispatch, this file early-returns as a backstop). 4 does not
// move the field either, so a batch of nothing but paint also skips the JFA and
// the redistance pass.

//#include scene_common.wgsl

//#constants

const IND_ALLOC: u32 = 0x80000000u;
const IND_FRESH: u32 = 0x20000000u;
const IND_INSIDE: u32 = 0x40000000u;
const IND_DIRTY: u32 = 0x10000000u; // edited since last redistance
const IND_IDX_MASK: u32 = 0x000FFFFFu;

struct EditParams {
  regionMin: vec4i,   // xyz = first cell of region
  regionDims: vec4i,  // xyz = region size in cells
  brush: vec4f,       // xyz = position, w = radius
  color: vec4f,       // rgb brush albedo, w = mode
  brushB: vec4f,      // xyz = second capsule endpoint (== brush.xyz for a sphere)
  // x = capsule count, y = dent amplitude (m), z = paint strength
  parms: vec4f,
  // R19: capsules 1..count-1 of a fused brush, as (A, -) (B, -) pairs sharing
  // brush.w. SIZED OFF THE GENERATED CONSTANT — a literal here that disagrees
  // with the CPU struct fails every bind group against this layout, and the app
  // renders black while still exiting 0 (trap 2).
  caps: array<vec4f, (MAX_BRUSH_CAPS - 1u) * 2u>,
}

@group(0) @binding(0) var<uniform> ep: EditParams;
@group(1) @binding(0) var<storage, read_write> bIndirection: array<u32>;
@group(1) @binding(1) var<storage, read_write> bDist: array<u32>;
@group(1) @binding(2) var<storage, read_write> bAlbedo: array<u32>;
// 0 allocTop, 1 freeTop, 2 dirty, 3 volFp, 4 pool spill — the full map is on
// BrickSystem::kCounterBytes in src/brick.h.
@group(2) @binding(0) var<storage, read_write> counters: array<atomic<u32>>;
@group(2) @binding(1) var<storage, read_write> freelist: array<u32>;

fn cellIndex(c: vec3i) -> u32 {
  return u32(c.x + c.y * GRID + c.z * GRID * GRID);
}
fn cellCenter(c: vec3i) -> vec3f {
  return VOL_ORIGIN + (vec3f(c) + 0.5) * SPAN;
}
fn segDist(w: vec3f, a: vec3f, b: vec3f) -> f32 {
  let ab = b - a;
  let l2 = dot(ab, ab);
  var t = 0.0;
  if (l2 > 1e-12) {
    t = clamp(dot(w - a, ab) / l2, 0.0, 1.0);
  }
  return length(w - (a + ab * t));
}
// Capsule brush: a swept sphere from brush to brushB. Equal endpoints give
// exactly the old sphere, so every existing edit is bit-identical.
//
// R19: with parms.x > 1 it is the UNION of that many capsules, all of radius
// brush.w. A hard min, not a smin, and that is the point — min of SDFs is the
// exact union, so a swing's substeps fuse into one clean swept slot. Emitting
// them as separate ops instead applied a soft carve per substep against a field
// the previous one had already moved, and the seam between two of those reads
// as scalloping along the cut. parms.x == 1 skips the loop entirely.
fn brushSdf(w: vec3f) -> f32 {
  var d = segDist(w, ep.brush.xyz, ep.brushB.xyz) - ep.brush.w;
  let n = u32(max(ep.parms.x, 1.0));
  for (var i = 1u; i < n; i++) {
    let j = (i - 1u) * 2u;
    d = min(d, segDist(w, ep.caps[j].xyz, ep.caps[j + 1u].xyz) - ep.brush.w);
  }
  return d;
}

// ---------- mode 3: the dent (R20) ----------
//
// A punch should push clay around, not shed pellets. This displaces the field
// along the punch axis by a radial profile whose net volume change is ZERO BY
// CONSTRUCTION, so the ledger balances without gobs or debt to chase.
//
// The profile is delta(rho) = amp * (1-u^2)^2 * (1-4u^2), u = rho/R, where rho
// is the distance to the PUNCH AXIS (not to its centre). Positive delta pushes
// the surface inward, so the core u < 0.5 dents and the rim 0.5 < u < 1 bulges
// back out, reaching zero with a double root at the brush edge.
//
// Why THAT polynomial: the volume a normal displacement moves is the integral
// of delta over the surface, and for any plane the surface integral reduces to
// a constant times the integral of delta(rho)*rho drho — so the single
// condition that integral(delta*rho, 0..R) == 0 makes the op volume-neutral for
// a plane at ANY tilt and ANY offset along the axis, not just a head-on one.
// (1-u^2)^2*(1-k*u^2) satisfies it at exactly k = 4.
//
// THE WINDOW THAT LOCALIZES IT IS ON THE FIELD, NOT ON THE AXIS, and that is
// the whole difference between a dent that conserves and one that does not.
// The displacement has to fade out somewhere or it runs the length of an
// infinite cylinder and dents the far side of the body too. Fading it along the
// AXIS is the obvious choice and it breaks the integral above: the window then
// varies across a contact patch that is tilted or off-centre, so it no longer
// factors out. Measured on a voxel grid, replaying this exact occupancy sum: an
// axial window drifts to -7% of the displaced volume at 25 mm of offset and
// -30% at 45 deg of tilt, and NEGATIVE is the bad sign — it means the punch
// created clay and healed its target.
//
// Fading on dOld instead makes the window identically 1 everywhere ON the
// surface, which is exactly where the volume integral lives, so tilt and offset
// stop mattering: <=1.1% across +-30 deg and +-20 mm, and ~3% against a body
// as curved as this one, always POSITIVE (billed as damage, never as healing).
//
// The window is evaluated at dOld while the surface ends up at dOld = -shift,
// so a narrow window damps the deep core more than the shallow rim and drifts
// the balance. That is what sets its width: at 8 voxels the residual is 3%, at
// 16 it is 0.9%, and past that curvature dominates anyway.
const DENT_WIN: f32 = 16.0; // voxels, half-width
// Keeps the field MONOTONIC along the normal. max|dW/dd| is 1.54/DENT_WIN, so a
// shift past DENT_WIN/1.54 folds the field back on itself and buys a second
// zero crossing — a shell floating off the skin. 0.3 leaves 3x margin, and
// caps a cranked impact.dentDepth at ~19 mm rather than letting it corrupt.
const DENT_MAX: f32 = 0.3;
//
// brush/brushB carry the AXIS here, not a swept brush: the segment runs from
// centre - n*halfLen to centre + n*halfLen, which is also exactly the region
// the influence occupies, so the bounds test and the dispatch region need no
// special case.
//
// Takes dOld in VOXELS and returns the shift in VOXELS.
fn dentShift(w: vec3f, dOld: f32) -> f32 {
  let a = ep.brush.xyz;
  let b = ep.brushB.xyz;
  let ax = b - a;
  let len = length(ax);
  if (len < 1e-6) {
    return 0.0;
  }
  let n = ax / len;
  let halfLen = len * 0.5;
  let rel = w - (a + b) * 0.5;
  let s = dot(rel, n);
  let rho = length(rel - n * s);
  let R = max(ep.brush.w, 1e-5);
  if (rho >= R || abs(s) >= halfLen || abs(dOld) >= DENT_WIN) {
    return 0.0;
  }
  let u2 = (rho * rho) / (R * R);
  let radial = (1.0 - u2) * (1.0 - u2) * (1.0 - 4.0 * u2);
  let t = dOld / DENT_WIN;
  let window = (1.0 - t * t) * (1.0 - t * t);
  let amp = clamp(ep.parms.y / VOXEL, -DENT_WIN * DENT_MAX, DENT_WIN * DENT_MAX);
  return amp * radial * window;
}

// ---------- mode 4: the albedo stamp (R21) ----------
// Opacity across the brush, smoothstepped so the edge of a bruise is not a
// disc. Metres in, 0..1 out.
fn paintWeight(w: vec3f) -> f32 {
  let R = max(ep.brush.w, 1e-5);
  let t = clamp(-brushSdf(w) / R, 0.0, 1.0);
  return clamp(ep.parms.z, 0.0, 1.0) * t * t * (3.0 - 2.0 * t);
}
// Soft CSG (voxel units): hard max/min leave a gradient discontinuity at the
// crease that sampled fields render as black pits — and real carved clay has
// soft rims anyway.
const CSG_SOFT: f32 = 1.5;
fn softCarve(dOld: f32, ds: f32) -> f32 { return -smin(-dOld, ds, CSG_SOFT); }
fn softAdd(dOld: f32, ds: f32) -> f32 { return smin(dOld, ds, CSG_SOFT); }

// Conservation ledger: counters[3] accumulates |occupancy delta| of the op
// in fixed-point voxels (x1024). Occupancy is a 1-voxel linear ramp across
// the surface — consistent between carve and add, so removed == re-deposited
// for mirror-image ops. Unique voxels only (v < 7): the overlap plane is the
// neighbor brick's voxel 0.
const VOL_FP: f32 = 1024.0;
const CELL_VOX: u32 = 343u; // 7^3 unique voxels per whole-cell swallow
fn occ01(d: f32) -> f32 { return clamp(0.5 - d, 0.0, 1.0); }
// A carve only grows d and an add only shrinks it, so abs() is the op's
// one-sided volume. A DENT does both on purpose (R20) and its two halves are
// meant to cancel — taking abs() there would report roughly twice the clay it
// displaced instead of the ~0 it actually moved. Signed values ride the same
// u32 counter as two's complement (atomicAdd wraps, which IS signed addition)
// and only the CPU read has to know.
fn volDelta(dNew: f32, dOld: f32, mode: i32) -> f32 {
  let d = occ01(dNew) - occ01(dOld);
  if (mode == 3) {
    return d;
  }
  return abs(d);
}

// ---------- classify: allocate cells the op will cut a surface through ----------
@compute @workgroup_size(4, 4, 4)
fn classify(@builtin(global_invocation_id) gid: vec3u) {
  // Neither of the cheap modes allocates: a dent moves the skin by a fraction
  // of the narrow band it already lives in, and paint does not move it at all.
  // The CPU skips this dispatch for both (encodeOp); this is the backstop that
  // keeps the two facts in one place. A dent cranked past the allocated shell
  // CLIPS rather than allocating — a tuning limit, not corruption.
  if (i32(ep.color.w) >= 3) {
    return;
  }
  // classify dispatch rounds up to workgroup multiples: cells past the fill
  // region must not allocate or they'd never be filled
  if (any(vec3i(gid) >= ep.regionDims.xyz)) {
    return;
  }
  let c = ep.regionMin.xyz + vec3i(gid);
  if (any(c < vec3i(0)) || any(c >= vec3i(GRID))) {
    return;
  }
  let ci = cellIndex(c);
  let e = bIndirection[ci];
  if ((e & IND_ALLOC) != 0u) {
    return;
  }
  let mode = i32(ep.color.w);
  let center = cellCenter(c);
  var needs = false;
  var inside = false;
  if (mode == 0) {
    let d = charBodyAnalytic(center);
    needs = abs(d) < 2.1 * SPAN;
    inside = d < 0.0;
    if (!needs) {
      // deep interior/exterior cells still need their sign recorded — an
      // unflagged interior reads as positive distance and rays that slip
      // past the skin tunnel through the whole body
      bIndirection[ci] = select(1u, IND_INSIDE | 1u, inside);
      return;
    }
  } else if (mode == 1) {
    // carving only creates new surface inside existing clay
    let ds = brushSdf(center);
    needs = ((e & IND_INSIDE) != 0u) && abs(ds) < 2.1 * SPAN;
    inside = true;
    if (!needs) {
      if (ds < -0.87 * SPAN) {
        if ((e & IND_INSIDE) != 0u) {
          atomicAdd(&counters[3], CELL_VOX * u32(VOL_FP)); // solid cell -> air
        }
        bIndirection[ci] = 1u; // cell swallowed whole by the carve -> air
      }
      return;
    }
  } else {
    // adding creates surface in empty-outside space
    let ds = brushSdf(center);
    needs = ((e & IND_INSIDE) == 0u) && abs(ds) < 2.1 * SPAN;
    inside = false;
    if (!needs) {
      if (ds < -0.87 * SPAN && (e & IND_ALLOC) == 0u) {
        if ((e & IND_INSIDE) == 0u) {
          atomicAdd(&counters[3], CELL_VOX * u32(VOL_FP)); // air cell -> solid
        }
        bIndirection[ci] = IND_INSIDE | 1u; // swallowed by the add -> clay
      }
      return;
    }
  }
  // pop freelist, else bump
  var brick: u32;
  let freeTop = atomicLoad(&counters[1]);
  if (freeTop > 0u) {
    let slot = atomicSub(&counters[1], 1u);
    if (slot > 0u && slot <= arrayLength(&freelist)) {
      brick = freelist[slot - 1u];
    } else {
      atomicAdd(&counters[1], 1u); // lost the race; fall through to bump
      brick = atomicAdd(&counters[0], 1u);
    }
  } else {
    brick = atomicAdd(&counters[0], 1u);
  }
  if (brick >= MAX_BRICKS) {
    atomicSub(&counters[0], 1u);
    // Pool exhausted. Leave bIndirection[ci] ALONE: the cell keeps whatever it
    // said before, so a carve that cannot allocate simply leaves that cell
    // reporting solid rather than writing a half-built brick. Graceful, but it
    // must never be silent — counters[4] is the spill count the CPU polls and
    // warns on (BrickSystem::finishCapacityPoll).
    atomicAdd(&counters[4], 1u);
    return;
  }
  bIndirection[ci] = IND_ALLOC | IND_FRESH | (select(0u, IND_INSIDE, inside)) | brick;
}

// ---------- fill: write voxels for every allocated cell in the region ----------
var<workgroup> wgMin: atomic<i32>;
var<workgroup> wgMax: atomic<i32>;
var<workgroup> wgVol: atomic<u32>; // fixed-point |occupancy delta| this brick

@compute @workgroup_size(8, 8, 4)
fn fill(@builtin(workgroup_id) wid: vec3u, @builtin(local_invocation_id) lid: vec3u,
        @builtin(local_invocation_index) li: u32) {
  // no early returns: workgroupBarrier demands uniform control flow, so all
  // gating happens through `alive` (uniform per-workgroup by construction)
  let c = ep.regionMin.xyz + vec3i(wid);
  var alive = !(any(c < vec3i(0)) || any(c >= vec3i(GRID)));
  var ci = 0u;
  var e = 0u;
  if (alive) {
    ci = cellIndex(c);
    e = bIndirection[ci];
    alive = (e & IND_ALLOC) != 0u;
  }
  if (li == 0u) {
    atomicStore(&wgMin, 100000);
    atomicStore(&wgMax, -100000);
    atomicStore(&wgVol, 0u);
  }
  workgroupBarrier();

  let brick = e & IND_IDX_MASK;
  let fresh = (e & IND_FRESH) != 0u;
  let wasInside = (e & IND_INSIDE) != 0u;
  let mode = i32(ep.color.w);
  let baseVoxel = vec3f(c) * BRICK_USABLE;

  // each thread does voxels (x,y,z) and (x,y,z+4)
  var volLocal = 0.0; // this thread's |occupancy delta|, unique voxels only
  for (var zi = 0u; zi < 2u; zi++) {
    if (!alive) {
      break;
    }
    let v = vec3i(vec3u(lid.x, lid.y, lid.z + zi * 4u));
    let vi = u32(v.x + v.y * 8 + v.z * 64);
    let w = VOL_ORIGIN + (baseVoxel + vec3f(v)) * VOXEL;

    var dOld: f32;
    if (fresh) {
      dOld = select(BAND, -BAND, wasInside);
    } else {
      let word = bDist[brick * 256u + vi / 2u];
      let pair = unpack2x16float(word);
      dOld = select(pair.x, pair.y, (vi & 1u) == 1u);
    }

    var dNew = dOld;
    var writeAlbedo = false;
    var albedo = ep.color.rgb;
    if (fresh) {
      // solid clay is colored all the way through: freshly allocated
      // interior bricks must carry body color or deep carves expose
      // unpainted (black) voxels
      writeAlbedo = true;
      if (mode != 2) {
        albedo = charBodyAlbedo(w);
      }
    }
    if (mode == 0) {
      dNew = charBodyAnalytic(w) / VOXEL;
      writeAlbedo = true;
      albedo = charBodyAlbedo(w);
    } else if (mode == 1) {
      let ds = brushSdf(w) / VOXEL;
      dNew = softCarve(dOld, ds);
    } else if (mode == 3) {
      // displacement, not CSG: the field is SHIFTED along its own normal, so
      // the surface moves and the clay it passes over is neither added nor
      // removed. |grad d| takes a knock doing it, which is exactly what the
      // redistance pass this op marks the cell dirty for is there to heal.
      dNew = dOld + dentShift(w, dOld);
    } else if (mode == 4) {
      // albedo only — dNew stays dOld and nothing below writes bDist.
      // `fresh` cannot happen (paint never allocates), but a fresh brick's
      // albedo is undefined until its owning op writes it, so blending against
      // it would smear garbage.
      let a = paintWeight(w);
      if (a > 0.0 && !fresh) {
        writeAlbedo = true;
        let prev = unpack4x8unorm(bAlbedo[brick * 512u + vi]).rgb;
        albedo = mix(prev, ep.color.rgb, a);
      }
    } else {
      let ds = brushSdf(w) / VOXEL;
      if (ds < dOld) {
        writeAlbedo = true;
      }
      dNew = softAdd(dOld, ds);
    }
    dNew = clamp(dNew, -BAND, BAND);

    // f16 pair packing: both halves of the u32 are written by the same
    // thread (vi and vi+1 with vi even are handled by threads lid.x even/odd
    // pairs)... they are NOT - so pack via per-voxel gather: thread with
    // even vi writes the word using its neighbor's value computed the same
    // way. To keep it race-free each even-vi thread computes both voxels.
    // Paint writes no distances, so it skips the pair packing entirely — which
    // is the other half of why an albedo edit is cheap.
    if (mode != 4 && (vi & 1u) == 0u) {
      // recompute neighbor voxel (x+1) with identical logic
      let v2 = v + vec3i(1, 0, 0);
      let vi2 = vi + 1u;
      let w2 = VOL_ORIGIN + (baseVoxel + vec3f(v2)) * VOXEL;
      var dOld2: f32;
      if (fresh) {
        dOld2 = select(BAND, -BAND, wasInside);
      } else {
        let word2 = bDist[brick * 256u + vi2 / 2u];
        dOld2 = unpack2x16float(word2).y;
      }
      var dNew2 = dOld2;
      if (mode == 0) {
        dNew2 = charBodyAnalytic(w2) / VOXEL;
      } else if (mode == 1) {
        dNew2 = softCarve(dOld2, brushSdf(w2) / VOXEL);
      } else if (mode == 3) {
        dNew2 = dOld2 + dentShift(w2, dOld2);
      } else {
        dNew2 = softAdd(dOld2, brushSdf(w2) / VOXEL);
      }
      dNew2 = clamp(dNew2, -BAND, BAND);
      bDist[brick * 256u + vi / 2u] = pack2x16float(vec2f(dNew, dNew2));
      atomicMin(&wgMin, i32(floor(min(dNew, dNew2) * 100.0)));
      atomicMax(&wgMax, i32(ceil(max(dNew, dNew2) * 100.0)));
      // ledger: both pair voxels measured here (the odd thread never writes).
      if (mode != 0) {
        if (all(v.yz < vec2i(7))) { // v.x even -> always < 7
          volLocal += volDelta(dNew, dOld, mode);
        }
        if (v.x + 1 < 7 && all(v.yz < vec2i(7))) {
          volLocal += volDelta(dNew2, dOld2, mode);
        }
      }
    }
    if (writeAlbedo) {
      bAlbedo[brick * 512u + vi] = pack4x8unorm(vec4f(albedo, 1.0));
    }
  }
  if (alive && volLocal != 0.0) {
    // via i32: a dent's contribution can be negative, and converting a negative
    // float straight to u32 is not defined to wrap the way the counter needs.
    atomicAdd(&wgVol, bitcast<u32>(i32(round(volLocal * VOL_FP))));
  }
  workgroupBarrier();

  // brick emptied of surface? free it and restore an empty-cell entry. Paint
  // moved nothing, so none of this applies to it — and running the free test
  // over a field it did not write would be reading someone else's result.
  if (alive && li == 0u && mode != 4) {
    let wv = atomicLoad(&wgVol);
    if (wv != 0u) {
      atomicAdd(&counters[3], wv);
    }
    let lo = atomicLoad(&wgMin);
    let hi = atomicLoad(&wgMax);
    if (lo > i32((BAND - 0.1) * 100.0)) {
      // everything far outside: free as empty-outside
      let slot = atomicAdd(&counters[1], 1u);
      freelist[slot] = brick;
      bIndirection[ci] = 1u; // cheb refreshed by JFA
    } else if (hi < -i32((BAND - 0.1) * 100.0)) {
      let slot = atomicAdd(&counters[1], 1u);
      freelist[slot] = brick;
      bIndirection[ci] = IND_INSIDE | 1u;
    } else {
      var outE = e & ~IND_FRESH;
      if (mode != 0) {
        outE |= IND_DIRTY; // bake writes exact distances; edits need healing
      }
      bIndirection[ci] = outE;
    }
  }
}
