// Ground clay heightfield: where carved clay lands (M4.6 conservation).
// Thickness lives ON TOP of the pebble floor surface (precomputed base), so
// every deposited milliliter reads as visible pile — nothing hides in the
// cracks. Deposits are quartic bumps with a closed-form integral: the ledger
// is exact by construction, no measurement pass needed on this side.
//
// V1 backing store is this 2D field; the deposit interface (splat) is the
// contract, so M7 chunk-settling can swap in a world brickmap later.

//#include scene_common.wgsl

const GN: i32 = 512;
const GORIGIN: f32 = -1.75;          // field covers x,z in [-1.75, 1.75]
const GTEXEL: f32 = 0.0068359375;    // 3.5 / 512

struct SplatParams {
  splat: vec4f,  // xz = center, y = radius R, w = peak amplitude A
  color: vec4f,  // rgb, linear
  tile: vec4i,   // xy = first texel, zw = tile dims
}

@group(0) @binding(0) var<uniform> sp: SplatParams;
@group(0) @binding(1) var<storage, read_write> gBaseW: array<f32>;
@group(0) @binding(2) var<storage, read_write> gHeightW: array<f32>;
@group(0) @binding(3) var<storage, read_write> gColorW: array<u32>;

// ---------- one-time: floor surface height per texel ----------
// The arena floor is min(plane y=-0.045, pebble mosaic). Bisect the surface
// crossing so clay drapes over pebbles instead of floating on a plane.
@compute @workgroup_size(8, 8)
fn initBase(@builtin(global_invocation_id) gid: vec3u) {
  if (gid.x >= u32(GN) || gid.y >= u32(GN)) {
    return;
  }
  let x = GORIGIN + (f32(gid.x) + 0.5) * GTEXEL;
  let z = GORIGIN + (f32(gid.y) + 0.5) * GTEXEL;
  var lo = -0.05;  // below the base plane: field negative
  var hi = 0.09;   // above the tallest pebble: field positive
  for (var i = 0; i < 20; i++) {
    let mid = 0.5 * (lo + hi);
    let d = min(mid + 0.045,
                pebbleField(vec3f(x, mid, z), FLOOR_CELL, FLOOR_SEED, FLOOR_SQUASH));
    if (d < 0.0) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  gBaseW[gid.x + gid.y * u32(GN)] = 0.5 * (lo + hi);
}

// ---------- per-deposit: add a quartic bump of exact volume ----------
// kernel h(r) = A * (1 - (r/R)^2)^2, integral over the disk = A*pi*R^2/3.
// The CPU chooses A = 3V / (pi R^2), so the deposit carries exactly V.
@compute @workgroup_size(8, 8)
fn splat(@builtin(global_invocation_id) gid: vec3u) {
  if (any(vec2i(gid.xy) >= sp.tile.zw)) {
    return;
  }
  let t = sp.tile.xy + vec2i(gid.xy);
  if (any(t < vec2i(0)) || any(t >= vec2i(GN))) {
    return;
  }
  let x = GORIGIN + (f32(t.x) + 0.5) * GTEXEL;
  let z = GORIGIN + (f32(t.y) + 0.5) * GTEXEL;
  let r2 = (x - sp.splat.x) * (x - sp.splat.x) + (z - sp.splat.z) * (z - sp.splat.z);
  let R2 = sp.splat.y * sp.splat.y;
  if (r2 >= R2) {
    return;
  }
  let u1 = 1.0 - r2 / R2;
  let dh = sp.splat.w * u1 * u1;
  let idx = u32(t.x) + u32(t.y) * u32(GN);
  let hOld = gHeightW[idx];
  gHeightW[idx] = hOld + dh;
  // fresh clay covers old proportionally to how much it thickens the pile
  let w = dh / max(hOld + dh, 1e-6);
  let old = unpack4x8unorm(gColorW[idx]).rgb;
  gColorW[idx] = pack4x8unorm(vec4f(mix(old, sp.color.rgb, w), 1.0));
}
