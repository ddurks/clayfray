// Jump-flood over the 50^3 brick grid: nearest allocated cell for every
// empty cell -> Chebyshev distance (brick units) stored in the indirection
// entry. Runs after every edit; whole grid costs ~3M threads total.

//#constants
const IND_ALLOC: u32 = 0x80000000u;
const IND_INSIDE: u32 = 0x40000000u;
const NO_SEED: u32 = 0xFFFFFFFFu;

struct JfaParams {
  step: i32,
  srcIsA: u32,
}

@group(0) @binding(0) var<storage, read_write> bIndirection: array<u32>;
@group(0) @binding(1) var<storage, read_write> jfaA: array<u32>;
@group(0) @binding(2) var<storage, read_write> jfaB: array<u32>;
@group(0) @binding(3) var<uniform> jp: JfaParams;
@group(0) @binding(4) var<storage, read_write> bDistRO: array<u32>;   // brick pool (read)
@group(0) @binding(5) var<storage, read_write> coarseDist: array<f32>; // per-cell true-ish distance (m)
@group(0) @binding(6) var<storage, read_write> coarseB: array<f32>;    // relaxation ping-pong

const IND_IDX_MASK: u32 = 0x000FFFFFu;

fn brickCenterValue(brick: u32) -> f32 {
  // voxel (3,3,3): near enough to the brick center for a coarse scalar
  let vi = u32(3 + 3 * 8 + 3 * 64);
  let pair = unpack2x16float(bDistRO[brick * 256u + vi / 2u]);
  return select(pair.x, pair.y, (vi & 1u) == 1u) * VOXEL;
}

fn cellIndex(c: vec3i) -> u32 {
  return u32(c.x + c.y * GRID + c.z * GRID * GRID);
}
fn decodeCell(i: u32) -> vec3i {
  let x = i32(i) % GRID;
  let y = (i32(i) / GRID) % GRID;
  let z = i32(i) / (GRID * GRID);
  return vec3i(x, y, z);
}
fn cheb(a: vec3i, b: vec3i) -> i32 {
  let d = abs(a - b);
  return max(d.x, max(d.y, d.z));
}
// comparisons must use the same metric the tracer steps with (Euclidean) or
// the "nearest" seed can overstate the safe step and rays punch through
fn dist2(a: vec3i, b: vec3i) -> i32 {
  let d = a - b;
  return d.x * d.x + d.y * d.y + d.z * d.z;
}

@compute @workgroup_size(4, 4, 4)
fn init(@builtin(global_invocation_id) gid: vec3u) {
  let c = vec3i(gid);
  if (any(c >= vec3i(GRID))) {
    return;
  }
  let ci = cellIndex(c);
  jfaA[ci] = select(NO_SEED, ci, (bIndirection[ci] & IND_ALLOC) != 0u);
}

@compute @workgroup_size(4, 4, 4)
fn step(@builtin(global_invocation_id) gid: vec3u) {
  let c = vec3i(gid);
  if (any(c >= vec3i(GRID))) {
    return;
  }
  let ci = cellIndex(c);
  var best = select(jfaB[ci], jfaA[ci], jp.srcIsA == 1u);
  var bestD = 0x7FFFFFFF;
  if (best != NO_SEED) {
    bestD = dist2(c, decodeCell(best));
  }
  for (var dz = -1; dz <= 1; dz++) {
    for (var dy = -1; dy <= 1; dy++) {
      for (var dx = -1; dx <= 1; dx++) {
        if (dx == 0 && dy == 0 && dz == 0) {
          continue;
        }
        let n = c + vec3i(dx, dy, dz) * jp.step;
        if (any(n < vec3i(0)) || any(n >= vec3i(GRID))) {
          continue;
        }
        let s = select(jfaB[cellIndex(n)], jfaA[cellIndex(n)], jp.srcIsA == 1u);
        if (s != NO_SEED) {
          let d = dist2(c, decodeCell(s));
          if (d < bestD) {
            bestD = d;
            best = s;
          }
        }
      }
    }
  }
  if (jp.srcIsA == 1u) {
    jfaB[ci] = best;
  } else {
    jfaA[ci] = best;
  }
}

@compute @workgroup_size(4, 4, 4)
fn resolve(@builtin(global_invocation_id) gid: vec3u) {
  let c = vec3i(gid);
  if (any(c >= vec3i(GRID))) {
    return;
  }
  let ci = cellIndex(c);
  let e = bIndirection[ci];
  if ((e & IND_ALLOC) != 0u) {
    // coarse scalar for allocated cells: own brick's center value
    coarseDist[ci] = brickCenterValue(e & IND_IDX_MASK);
    return;
  }
  let s = select(jfaB[ci], jfaA[ci], jp.srcIsA == 1u);
  jfaA[ci] = s; // canonical final seed buffer, read by the tracer
  var d = 255u;
  var cd = 2.0; // meters; far default
  if (s != NO_SEED) {
    d = u32(clamp(cheb(c, decodeCell(s)), 1, 255));
    // triangle-inequality coarse distance: |c -> seed center| + value there
    let sc = decodeCell(s);
    let dc = length(vec3f(c - sc)) * SPAN;
    cd = dc + abs(brickCenterValue((bIndirection[cellIndex(sc)]) & IND_IDX_MASK));
  }
  let inside = (e & IND_INSIDE) != 0u;
  coarseDist[ci] = select(cd, -cd, inside);
  bIndirection[ci] = (e & IND_INSIDE) | d;
}

// Eikonal relaxation over the per-cell scalars: the triangle-inequality
// estimate has organic ~cell-scale error that AO and shadow penumbra render
// as swirls. min(sv, neighbor + spacing) restores field consistency,
// anchored by the accurate allocated-cell centers. jp.srcIsA: 1 = read
// coarseDist write coarseB, 0 = reverse. Works on |d|; sign restored after.
@compute @workgroup_size(4, 4, 4)
fn relax(@builtin(global_invocation_id) gid: vec3u) {
  let c = vec3i(gid);
  if (any(c >= vec3i(GRID))) {
    return;
  }
  let ci = cellIndex(c);
  let sv = select(coarseB[ci], coarseDist[ci], jp.srcIsA == 1u);
  var best = abs(sv);
  for (var a = 0; a < 6; a++) {
    var off = vec3i(0);
    if (a == 0) { off.x = 1; } else if (a == 1) { off.x = -1; }
    else if (a == 2) { off.y = 1; } else if (a == 3) { off.y = -1; }
    else if (a == 4) { off.z = 1; } else { off.z = -1; }
    let n = c + off;
    if (any(n < vec3i(0)) || any(n >= vec3i(GRID))) {
      continue;
    }
    let nv = select(coarseB[cellIndex(n)], coarseDist[cellIndex(n)], jp.srcIsA == 1u);
    best = min(best, abs(nv) + SPAN);
  }
  let out = select(best, -best, sv < 0.0);
  if (jp.srcIsA == 1u) {
    coarseB[ci] = out;
  } else {
    coarseDist[ci] = out;
  }
}
