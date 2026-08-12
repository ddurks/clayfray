// Narrow-band redistancing over dirty bricks. Soft-CSG edits leave the
// field accurate at the new crease but increasingly wrong away from it;
// stacked strokes compound. This restores |grad d| = 1 while FREEZING every
// voxel adjacent to a sign change — the sculpted surface itself is never
// moved, only the distances around it heal.
//
// compact: scan the grid, gather dirty allocated cells -> dirtyList
// prepArgs: dirty count -> indirect dispatch args
// redistance (indirect, 1 workgroup per dirty brick): load 10^3 apron into
//   shared memory, K Jacobi-Godunov iterations, write back own 8^3
// clearDirty (indirect): drop the dirty bits

const GRID: i32 = 74;
const BAND: f32 = 12.0;
const DIRTY_CAP: u32 = 65535u; // indirect dispatch x-dim limit

const IND_ALLOC: u32 = 0x80000000u;
const IND_INSIDE: u32 = 0x40000000u;
const IND_DIRTY: u32 = 0x10000000u;
const IND_IDX_MASK: u32 = 0x000FFFFFu;

@group(0) @binding(0) var<storage, read_write> bIndirection: array<u32>;
@group(0) @binding(1) var<storage, read_write> bDist: array<u32>;
@group(0) @binding(2) var<storage, read_write> dirtyList: array<u32>;
@group(0) @binding(3) var<storage, read_write> counters: array<atomic<u32>>; // [2] = dirty count
@group(0) @binding(4) var<storage, read_write> indirectArgs: array<u32>;

fn cellIndex(c: vec3i) -> u32 {
  return u32(c.x + c.y * GRID + c.z * GRID * GRID);
}
fn decodeCell(i: u32) -> vec3i {
  return vec3i(i32(i) % GRID, (i32(i) / GRID) % GRID, i32(i) / (GRID * GRID));
}

@compute @workgroup_size(64)
fn compact(@builtin(global_invocation_id) gid: vec3u) {
  let ci = gid.x;
  if (ci >= u32(GRID * GRID * GRID)) {
    return;
  }
  let e = bIndirection[ci];
  if ((e & IND_ALLOC) != 0u && (e & IND_DIRTY) != 0u) {
    let slot = atomicAdd(&counters[2], 1u);
    if (slot < DIRTY_CAP) {
      dirtyList[slot] = ci;
    }
  }
}

@compute @workgroup_size(1)
fn prepArgs() {
  let n = min(atomicLoad(&counters[2]), DIRTY_CAP);
  indirectArgs[0] = n; // redistance: one workgroup per dirty brick
  indirectArgs[1] = 1u;
  indirectArgs[2] = 1u;
  indirectArgs[3] = (n + 63u) / 64u; // clearDirty: 64-wide workgroups
  indirectArgs[4] = 1u;
  indirectArgs[5] = 1u;
}

// Read a voxel by WORLD voxel coordinate through the indirection (any owner
// brick is fine: overlapping copies agree). Unallocated space: band-clamped
// signed default.
fn readWorldVoxel(wv: vec3i) -> f32 {
  let cc = clamp(vec3i(vec3f(wv) / 7.0), vec3i(0), vec3i(GRID - 1));
  let cellc = clamp(cc, vec3i(0), vec3i(GRID - 1));
  let lv = wv - cellc * 7;
  if (any(lv < vec3i(0)) || any(lv > vec3i(7))) {
    return BAND; // outside grid entirely
  }
  let e = bIndirection[cellIndex(cellc)];
  if ((e & IND_ALLOC) == 0u) {
    return select(BAND, -BAND, (e & IND_INSIDE) != 0u);
  }
  let vi = u32(lv.x + lv.y * 8 + lv.z * 64);
  let pair = unpack2x16float(bDist[(e & IND_IDX_MASK) * 256u + vi / 2u]);
  return select(pair.x, pair.y, (vi & 1u) == 1u);
}

fn godunov(ain: f32, bin: f32, cin: f32) -> f32 {
  // sorted eikonal update, h = 1 voxel
  var a = ain;
  var b = bin;
  var c = cin;
  if (a > b) { let t = a; a = b; b = t; }
  if (b > c) { let t = b; b = c; c = t; }
  if (a > b) { let t = a; a = b; b = t; }
  var t = a + 1.0;
  if (t > b) {
    t = 0.5 * ((a + b) + sqrt(max(2.0 - (a - b) * (a - b), 0.0)));
    if (t > c) {
      let q = a + b + c;
      let disc = q * q - 3.0 * (a * a + b * b + c * c - 1.0);
      t = (q + sqrt(max(disc, 0.0))) / 3.0;
    }
  }
  return t;
}

var<workgroup> shA: array<f32, 1000>; // 10^3 apron
var<workgroup> shB: array<f32, 1000>;

fn apronIdx(a: vec3i) -> u32 {
  return u32((a.x + 1) + (a.y + 1) * 10 + (a.z + 1) * 100);
}

@compute @workgroup_size(8, 8, 4)
fn redistance(@builtin(workgroup_id) wid: vec3u,
              @builtin(local_invocation_index) li: u32,
              @builtin(local_invocation_id) lid: vec3u) {
  let ci = dirtyList[wid.x];
  let e = bIndirection[ci];
  let alive = (e & IND_ALLOC) != 0u;
  let cell = decodeCell(ci);
  let brick = e & IND_IDX_MASK;
  let baseW = cell * 7;

  // cooperative apron load: 1000 values by 256 threads
  for (var k = li; k < 1000u; k += 256u) {
    let az = i32(k / 100u);
    let ay = i32((k / 10u) % 10u);
    let ax = i32(k % 10u);
    var v = BAND;
    if (alive) {
      v = readWorldVoxel(baseW + vec3i(ax - 1, ay - 1, az - 1));
    }
    shA[k] = v;
    shB[k] = v;
  }
  workgroupBarrier();

  // freeze mask per own voxel: adjacent to a sign change = surface, fixed
  // (recomputed cheaply per iteration from the ORIGINAL values in shB's
  // first load... signs never change during iteration, so shA signs work)

  for (var it = 0; it < 10; it++) {
    // Jacobi: read shA, write shB (own 8^3 only; apron stays fixed)
    for (var zi = 0u; zi < 2u; zi++) {
      let v = vec3i(vec3u(lid.x, lid.y, lid.z + zi * 4u));
      let idx = apronIdx(v);
      let dv = shA[idx];
      let sgn = select(1.0, -1.0, dv < 0.0);
      let nxm = shA[apronIdx(v + vec3i(-1, 0, 0))];
      let nxp = shA[apronIdx(v + vec3i(1, 0, 0))];
      let nym = shA[apronIdx(v + vec3i(0, -1, 0))];
      let nyp = shA[apronIdx(v + vec3i(0, 1, 0))];
      let nzm = shA[apronIdx(v + vec3i(0, 0, -1))];
      let nzp = shA[apronIdx(v + vec3i(0, 0, 1))];
      let frozen = dv * nxm <= 0.0 || dv * nxp <= 0.0 || dv * nym <= 0.0 ||
                   dv * nyp <= 0.0 || dv * nzm <= 0.0 || dv * nzp <= 0.0;
      var out = dv;
      if (alive && !frozen) {
        let ax = min(abs(nxm), abs(nxp));
        let ay = min(abs(nym), abs(nyp));
        let az = min(abs(nzm), abs(nzp));
        out = clamp(sgn * godunov(ax, ay, az), -BAND, BAND);
      }
      shB[idx] = out;
    }
    workgroupBarrier();
    // swap: copy B back to A for the next iteration (own voxels only)
    for (var zi = 0u; zi < 2u; zi++) {
      let v = vec3i(vec3u(lid.x, lid.y, lid.z + zi * 4u));
      let idx = apronIdx(v);
      shA[idx] = shB[idx];
    }
    workgroupBarrier();
  }

  // write back own 8^3 (even-vi thread packs the f16 pair)
  for (var zi = 0u; zi < 2u; zi++) {
    let v = vec3i(vec3u(lid.x, lid.y, lid.z + zi * 4u));
    let vi = u32(v.x + v.y * 8 + v.z * 64);
    if (alive && (vi & 1u) == 0u) {
      let d0 = shA[apronIdx(v)];
      let d1 = shA[apronIdx(v + vec3i(1, 0, 0))];
      bDist[brick * 256u + vi / 2u] = pack2x16float(vec2f(d0, d1));
    }
  }
}

@compute @workgroup_size(64)
fn clearDirty(@builtin(global_invocation_id) gid: vec3u) {
  if (gid.x >= min(atomicLoad(&counters[2]), DIRTY_CAP)) {
    return;
  }
  let ci = dirtyList[gid.x];
  bIndirection[ci] = bIndirection[ci] & ~IND_DIRTY;
}
