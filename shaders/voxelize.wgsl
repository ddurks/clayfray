// Mesh -> brickmap voxelization (import-time). CPU pre-bins triangles into
// cells (with band margin); GPU computes exact per-voxel raycast PARITY for
// watertight signs (pseudonormal guessing speckles silhouettes — don't),
// exact closest-triangle distance magnitudes, albedo from barycentric
// vertex color x clay mottle, and packed top-2 bone weights.

//#include scene_common.wgsl

//#constants

const IND_ALLOC: u32 = 0x80000000u;
const IND_FRESH: u32 = 0x20000000u;
const IND_INSIDE: u32 = 0x40000000u;
const IND_IDX_MASK: u32 = 0x000FFFFFu;

@group(0) @binding(0) var<storage, read> mPos: array<f32>;      // xyz + smooth normal, 6/vertex
@group(0) @binding(1) var<storage, read> mIdx: array<u32>;      // 3 per tri
@group(0) @binding(2) var<storage, read> mCol: array<f32>;      // rgb per vertex
// M-RIG: group(0) binding(3) used to be `mSkin` (per-vertex joint ids +
// weights). It fed nothing but the packed per-voxel bone weights below, which
// no shader ever read back. Binding numbers of the survivors are deliberately
// LEFT AS THEY WERE — WebGPU allows a sparse set, and renumbering would mean
// touching every bind-group entry in brick.cpp for no gain.
@group(0) @binding(4) var<storage, read> cellTris: array<u32>;  // [offsets: cells+1][ids...]
@group(0) @binding(5) var<storage, read_write> voxParity: array<u32>; // per-voxel inside bits, ROW_WORDS u32/row
@group(0) @binding(6) var<storage, read> insideBits: array<u32>;      // per-cell parity (classify)

// CELLS / AXIS_VOX / ROW_WORDS are in the //#constants block (src/brick.h).

@group(1) @binding(0) var<storage, read_write> bIndirection: array<u32>;
@group(1) @binding(1) var<storage, read_write> bDist: array<u32>;
@group(1) @binding(2) var<storage, read_write> bAlbedo: array<u32>;
// M-RIG: binding(3) was `bWeights`, 512 packed top-2 bone weights per brick —
// a whole albedo-pool's worth of GPU memory PER FIGHTER (96 MiB at the then
// kMaxBricks of 49152) that was written once at import and never read by any
// shader or by the renderer. It was skeleton residue; with the armature gone
// it could never be read again.

// counters[4] is the pool-spill count — see the block comment on
// BrickSystem::kCounterBytes in src/brick.h.
@group(2) @binding(0) var<storage, read_write> counters: array<atomic<u32>>;
@group(2) @binding(1) var<storage, read_write> freelist: array<u32>;

fn cellIndex(c: vec3i) -> u32 {
  return u32(c.x + c.y * GRID + c.z * GRID * GRID);
}
fn cellInside(ci: u32) -> bool {
  return (insideBits[ci / 32u] & (1u << (ci % 32u))) != 0u;
}
fn triVert(t: u32, k: u32) -> vec3f {
  let vi = mIdx[t * 3u + k];
  return vec3f(mPos[vi * 6u], mPos[vi * 6u + 1u], mPos[vi * 6u + 2u]);
}

struct TriHit {
  d2: f32,
  tri: u32,
  bary: vec3f,
  sign: f32,
}

fn closestOnTri(p: vec3f, a: vec3f, b: vec3f, c: vec3f) -> vec3f {
  // returns barycentric coords of closest point (Ericson, RTCD)
  let ab = b - a;
  let ac = c - a;
  let ap = p - a;
  let d1 = dot(ab, ap);
  let d2v = dot(ac, ap);
  if (d1 <= 0.0 && d2v <= 0.0) { return vec3f(1.0, 0.0, 0.0); }
  let bp = p - b;
  let d3 = dot(ab, bp);
  let d4 = dot(ac, bp);
  if (d3 >= 0.0 && d4 <= d3) { return vec3f(0.0, 1.0, 0.0); }
  let vc = d1 * d4 - d3 * d2v;
  if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
    let v = d1 / (d1 - d3);
    return vec3f(1.0 - v, v, 0.0);
  }
  let cp = p - c;
  let d5 = dot(ab, cp);
  let d6 = dot(ac, cp);
  if (d6 >= 0.0 && d5 <= d6) { return vec3f(0.0, 0.0, 1.0); }
  let vb = d5 * d2v - d1 * d6;
  if (vb <= 0.0 && d2v >= 0.0 && d6 <= 0.0) {
    let w = d2v / (d2v - d6);
    return vec3f(1.0 - w, 0.0, w);
  }
  let va = d3 * d6 - d5 * d4;
  if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
    let w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return vec3f(0.0, 1.0 - w, w);
  }
  let denom = 1.0 / (va + vb + vc);
  let v = vb * denom;
  let w = vc * denom;
  return vec3f(1.0 - v - w, v, w);
}

fn nearestTri(p: vec3f, ci: u32) -> TriHit {
  var hit: TriHit;
  hit.d2 = 1e12;
  hit.tri = 0xFFFFFFFFu;
  hit.bary = vec3f(1.0, 0.0, 0.0);
  hit.sign = 1.0;
  let lo = cellTris[ci];
  let hi = cellTris[ci + 1u];
  for (var k = lo; k < hi; k++) {
    let t = cellTris[CELLS + 1u + k];
    let a = triVert(t, 0u);
    let b = triVert(t, 1u);
    let c = triVert(t, 2u);
    let bary = closestOnTri(p, a, b, c);
    let q = a * bary.x + b * bary.y + c * bary.z;
    let dd = dot(p - q, p - q);
    if (dd < hit.d2) {
      hit.d2 = dd;
      hit.tri = t;
      hit.bary = bary;
    }
  }
  return hit;
}

// ---------- per-voxel parity: exact watertight signs ----------
// One thread per (y,z) voxel row: raycast +x through ALL triangles, flip a
// suffix bitmask at each crossing. Kills every pseudonormal sign-guess
// failure mode (speck chains along creases included) in one stroke.
@compute @workgroup_size(8, 8)
fn voxelParity(@builtin(global_invocation_id) gid: vec3u) {
  if (gid.x >= u32(AXIS_VOX) || gid.y >= u32(AXIS_VOX)) {
    return;
  }
  let oy = VOL_ORIGIN.y + f32(gid.x) * VOXEL;
  let oz = VOL_ORIGIN.z + f32(gid.y) * VOXEL;
  let x0 = VOL_ORIGIN.x - 1.0;
  var row: array<u32, ROW_WORDS>;
  for (var wI = 0u; wI < ROW_WORDS; wI++) {
    row[wI] = 0u;
  }
  let triCount = arrayLength(&mIdx) / 3u;
  for (var t = 0u; t < triCount; t++) {
    let a = triVert(t, 0u);
    let b = triVert(t, 1u);
    let c = triVert(t, 2u);
    let e1 = b - a;
    let e2 = c - a;
    let det = -e1.y * e2.z + e1.z * e2.y; // dir (1,0,0), pvec = (0,-e2z,e2y)
    if (abs(det) < 1e-12) {
      continue;
    }
    let inv = 1.0 / det;
    let tv = vec3f(x0 - a.x, oy - a.y, oz - a.z);
    let uu = (-tv.y * e2.z + tv.z * e2.y) * inv;
    if (uu < 0.0 || uu > 1.0) {
      continue;
    }
    let qv = cross(tv, e1);
    let vv = qv.x * inv;
    if (vv < 0.0 || uu + vv > 1.0) {
      continue;
    }
    let thit = dot(e2, qv) * inv;
    if (thit <= 0.0) {
      continue;
    }
    // flip parity for every lattice x with origin.x + k*VOXEL > x0 + thit
    let k0 = i32(ceil((x0 + thit - VOL_ORIGIN.x) / VOXEL));
    let start = clamp(k0, 0, AXIS_VOX);
    for (var wI = 0u; wI < ROW_WORDS; wI++) {
      let base = i32(wI) * 32;
      if (base + 32 <= start) {
        continue;
      }
      var mask = 0xFFFFFFFFu;
      if (start > base) {
        mask = mask << u32(start - base);
      }
      row[wI] ^= mask;
    }
  }
  let rowBase = (gid.x + gid.y * u32(AXIS_VOX)) * ROW_WORDS;
  for (var wI = 0u; wI < ROW_WORDS; wI++) {
    voxParity[rowBase + wI] = row[wI];
  }
}

fn voxelInside(wv: vec3i) -> bool {
  let rowBase = (u32(wv.y) + u32(wv.z) * u32(AXIS_VOX)) * ROW_WORDS;
  let k = u32(clamp(wv.x, 0, AXIS_VOX - 1));
  return (voxParity[rowBase + k / 32u] & (1u << (k % 32u))) != 0u;
}

// ---------- classify ----------
@compute @workgroup_size(4, 4, 4)
fn meshClassify(@builtin(global_invocation_id) gid: vec3u) {
  let c = vec3i(gid);
  if (any(c >= vec3i(GRID))) {
    return;
  }
  let ci = cellIndex(c);
  let inside = cellInside(ci);
  let lo = cellTris[ci];
  let hi = cellTris[ci + 1u];
  if (lo == hi) {
    bIndirection[ci] = select(1u, IND_INSIDE | 1u, inside);
    return;
  }
  let center = VOL_ORIGIN + (vec3f(c) + 0.5) * SPAN;
  let hit = nearestTri(center, ci);
  let d = sqrt(hit.d2);
  if (d >= 2.1 * SPAN) {
    bIndirection[ci] = select(1u, IND_INSIDE | 1u, inside);
    return;
  }
  var brick: u32;
  let freeTop = atomicLoad(&counters[1]);
  if (freeTop > 0u) {
    let slot = atomicSub(&counters[1], 1u);
    if (slot > 0u && slot <= arrayLength(&freelist)) {
      brick = freelist[slot - 1u];
    } else {
      atomicAdd(&counters[1], 1u);
      brick = atomicAdd(&counters[0], 1u);
    }
  } else {
    brick = atomicAdd(&counters[0], 1u);
  }
  if (brick >= MAX_BRICKS) {
    atomicSub(&counters[0], 1u);
    // Pool exhausted mid-IMPORT. The indirection grid was just cleared, so
    // this cell stays 0 = empty-outside and the fighter imports with a hole —
    // far more visible than an edit spill, and the reason the capacity poll is
    // armed after import too. counters[4] makes it say so instead of guessing.
    atomicAdd(&counters[4], 1u);
    return;
  }
  // no FRESH bit: import fill writes unconditionally, and a leftover FRESH
  // would make the first edit's fill discard the imported voxel data
  bIndirection[ci] = IND_ALLOC | select(0u, IND_INSIDE, inside) | brick;
}

// ---------- fill ----------
fn voxelValue(w: vec3f, wv: vec3i, ci: u32) -> f32 {
  // sign is exact watertight parity; magnitude is exact closest-triangle
  let sgn = select(1.0, -1.0, voxelInside(wv));
  let hit = nearestTri(w, ci);
  if (hit.tri == 0xFFFFFFFFu) {
    return sgn * BAND;
  }
  return clamp(sgn * sqrt(hit.d2) / VOXEL, -BAND, BAND);
}

@compute @workgroup_size(8, 8, 4)
fn meshFill(@builtin(workgroup_id) wid: vec3u, @builtin(local_invocation_id) lid: vec3u) {
  let c = vec3i(wid);
  if (any(c >= vec3i(GRID))) {
    return;
  }
  let ci = cellIndex(c);
  let e = bIndirection[ci];
  if ((e & IND_ALLOC) == 0u) {
    return;
  }
  let brick = e & IND_IDX_MASK;
  let baseVoxel = vec3f(c) * BRICK_USABLE;

  for (var zi = 0u; zi < 2u; zi++) {
    let v = vec3i(vec3u(lid.x, lid.y, lid.z + zi * 4u));
    let vi = u32(v.x + v.y * 8 + v.z * 64);
    let wv = c * 7 + v;
    let w = VOL_ORIGIN + (baseVoxel + vec3f(v)) * VOXEL;
    let hit = nearestTri(w, ci);

    // distance pair (even-vi thread writes both halves)
    if ((vi & 1u) == 0u) {
      let d0 = voxelValue(w, wv, ci);
      let w2 = VOL_ORIGIN + (baseVoxel + vec3f(v) + vec3f(1.0, 0.0, 0.0)) * VOXEL;
      let d1 = voxelValue(w2, wv + vec3i(1, 0, 0), ci);
      bDist[brick * 256u + vi / 2u] = pack2x16float(vec2f(d0, d1));
    }

    // albedo from the nearest triangle's vertices. The 12-slot bone-influence
    // accumulation that used to run here (three verts x four slots, merged and
    // sorted to a top-2) went with bWeights — it was per-VOXEL work feeding a
    // buffer nobody read.
    var albedo = vec3f(0.15, 0.47, 0.53);
    if (hit.tri != 0xFFFFFFFFu) {
      let i0 = mIdx[hit.tri * 3u];
      let i1 = mIdx[hit.tri * 3u + 1u];
      let i2 = mIdx[hit.tri * 3u + 2u];
      let c0 = vec3f(mCol[i0 * 3u], mCol[i0 * 3u + 1u], mCol[i0 * 3u + 2u]);
      let c1 = vec3f(mCol[i1 * 3u], mCol[i1 * 3u + 1u], mCol[i1 * 3u + 2u]);
      let c2 = vec3f(mCol[i2 * 3u], mCol[i2 * 3u + 1u], mCol[i2 * 3u + 2u]);
      albedo = c0 * hit.bary.x + c1 * hit.bary.y + c2 * hit.bary.z;
    }
    // clay mottle so imported flat colors still read handled
    albedo *= (0.82 + 0.32 * fbm(w * 9.0));
    bAlbedo[brick * 512u + vi] = pack4x8unorm(vec4f(clamp(albedo, vec3f(0.0), vec3f(1.0)), 1.0));
  }
}
