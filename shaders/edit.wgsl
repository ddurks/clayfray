// Brickmap bake + edit passes. One pipeline pair used for everything:
//   classify: one thread per cell in the region -> allocate fresh bricks
//   fill:     one workgroup per cell -> write voxels, free emptied bricks
// Modes: 0 = bake analytic character, 1 = carve (CSG subtract sphere),
//        2 = add clay (min with sphere, stamps brush albedo).

//#include scene_common.wgsl

const GRID: i32 = 74;
const BRICK_USABLE: f32 = 7.0;
const VOXEL: f32 = 0.0027344;
const SPAN: f32 = 0.0191406;
const VOL_ORIGIN: vec3f = vec3f(-0.7082, -0.1582, -0.7082);
const BAND: f32 = 12.0; // must match brick_read.wgsl
const MAX_BRICKS: u32 = 49152u;

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
}

@group(0) @binding(0) var<uniform> ep: EditParams;
@group(1) @binding(0) var<storage, read_write> bIndirection: array<u32>;
@group(1) @binding(1) var<storage, read_write> bDist: array<u32>;
@group(1) @binding(2) var<storage, read_write> bAlbedo: array<u32>;
@group(2) @binding(0) var<storage, read_write> counters: array<atomic<u32>>; // 0 allocTop, 1 freeTop
@group(2) @binding(1) var<storage, read_write> freelist: array<u32>;

fn cellIndex(c: vec3i) -> u32 {
  return u32(c.x + c.y * GRID + c.z * GRID * GRID);
}
fn cellCenter(c: vec3i) -> vec3f {
  return VOL_ORIGIN + (vec3f(c) + 0.5) * SPAN;
}
fn brushSdf(w: vec3f) -> f32 {
  return length(w - ep.brush.xyz) - ep.brush.w;
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

// ---------- classify: allocate cells the op will cut a surface through ----------
@compute @workgroup_size(4, 4, 4)
fn classify(@builtin(global_invocation_id) gid: vec3u) {
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
    return; // pool exhausted; skip (logged CPU-side via counter readback later)
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
    if ((vi & 1u) == 0u) {
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
      } else {
        dNew2 = softAdd(dOld2, brushSdf(w2) / VOXEL);
      }
      dNew2 = clamp(dNew2, -BAND, BAND);
      bDist[brick * 256u + vi / 2u] = pack2x16float(vec2f(dNew, dNew2));
      atomicMin(&wgMin, i32(floor(min(dNew, dNew2) * 100.0)));
      atomicMax(&wgMax, i32(ceil(max(dNew, dNew2) * 100.0)));
      // ledger: both pair voxels measured here (the odd thread never writes).
      // Soft carve only grows d, soft add only shrinks it, so abs() is the
      // op's one-sided volume.
      if (mode != 0) {
        if (all(v.yz < vec2i(7))) { // v.x even -> always < 7
          volLocal += abs(occ01(dNew) - occ01(dOld));
        }
        if (v.x + 1 < 7 && all(v.yz < vec2i(7))) {
          volLocal += abs(occ01(dNew2) - occ01(dOld2));
        }
      }
    }
    if (writeAlbedo) {
      bAlbedo[brick * 512u + vi] = pack4x8unorm(vec4f(albedo, 1.0));
    }
  }
  if (alive && volLocal > 0.0) {
    atomicAdd(&wgVol, u32(volLocal * VOL_FP + 0.5));
  }
  workgroupBarrier();

  // brick emptied of surface? free it and restore an empty-cell entry
  if (alive && li == 0u) {
    let wv = atomicLoad(&wgVol);
    if (wv > 0u) {
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
