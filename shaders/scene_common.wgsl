// Shared noise, SDF primitives, and the analytic character body.
// Pulled in via the renderer's //#include preprocessor (WGSL has none).

fn hash12(p: vec2f) -> f32 {
  var p3 = fract(vec3f(p.xyx) * 0.1031);
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.x + p3.y) * p3.z);
}
fn hash22(p: vec2f) -> vec2f {
  var p3 = fract(vec3f(p.xyx) * vec3f(0.1031, 0.1030, 0.0973));
  p3 += dot(p3, p3.yzx + 33.33);
  return fract((p3.xx + p3.yz) * p3.zy);
}
fn hash13(pin: vec3f) -> f32 {
  var p3 = fract(pin * 0.1031);
  p3 += dot(p3, p3.zyx + 31.32);
  return fract((p3.x + p3.y) * p3.z);
}
fn vnoise(p: vec3f) -> f32 {
  let i = floor(p);
  let f = fract(p);
  let w = f * f * (3.0 - 2.0 * f);
  return mix(
    mix(mix(hash13(i + vec3f(0.0, 0.0, 0.0)), hash13(i + vec3f(1.0, 0.0, 0.0)), w.x),
        mix(hash13(i + vec3f(0.0, 1.0, 0.0)), hash13(i + vec3f(1.0, 1.0, 0.0)), w.x), w.y),
    mix(mix(hash13(i + vec3f(0.0, 0.0, 1.0)), hash13(i + vec3f(1.0, 0.0, 1.0)), w.x),
        mix(hash13(i + vec3f(0.0, 1.0, 1.0)), hash13(i + vec3f(1.0, 1.0, 1.0)), w.x), w.y),
    w.z);
}
fn fbm(p: vec3f) -> f32 {
  var v = 0.0;
  var a = 0.5;
  var q = p;
  for (var i = 0; i < 3; i++) {
    v += a * vnoise(q);
    q = q * 2.13;
    a *= 0.5;
  }
  return v;
}

fn sdSphere(p: vec3f, r: f32) -> f32 { return length(p) - r; }
fn sdEllipsoid(p: vec3f, r: vec3f) -> f32 {
  let k0 = length(p / r);
  let k1 = length(p / (r * r));
  return k0 * (k0 - 1.0) / max(k1, 1e-6);
}
fn sdCapsule(p: vec3f, a: vec3f, b: vec3f, r: f32) -> f32 {
  let pa = p - a;
  let ba = b - a;
  let h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
  return length(pa - ba * h) - r;
}
fn smin(a: f32, b: f32, k: f32) -> f32 {
  let h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
  return mix(b, a, h) - k * h * (1.0 - h);
}
fn toLinear(c: vec3f) -> vec3f { return pow(c, vec3f(2.2)); }

// ---------- arena: pebble-mosaic floor + wall ----------
const WALL_Z: f32 = -2.3;
const FLOOR_CELL: f32 = 0.34;
const WALL_CELL: f32 = 0.85;
const FLOOR_SEED: f32 = 7.0;
const WALL_SEED: f32 = 3.0;
const FLOOR_SQUASH: f32 = 0.20;
const WALL_SQUASH: f32 = 0.12;

fn pebbleField(pl: vec3f, cell: f32, seed: f32, squash: f32) -> f32 {
  var d = 1e9;
  let id = floor(pl.xz / cell);
  for (var i = -1; i <= 1; i++) {
    for (var j = -1; j <= 1; j++) {
      let cid = id + vec2f(f32(i), f32(j));
      let h = hash22(cid + seed);
      let c = (cid + 0.5 + (h - 0.5) * 0.55) * cell;
      let rr = cell * (0.62 + 0.22 * hash12(cid * 1.71 + seed));
      let q = vec3f(pl.x - c.x, pl.y, pl.z - c.y);
      let e = sdEllipsoid(q, vec3f(rr, rr * squash, rr * (0.8 + 0.4 * hash12(cid * 2.7 + seed))));
      d = min(d, e);
    }
  }
  return d;
}
fn pebbleCell(pl: vec3f, cell: f32, seed: f32, squash: f32) -> vec2f {
  var best = 1e9;
  var bestId = vec2f(0.0);
  let id = floor(pl.xz / cell);
  for (var i = -1; i <= 1; i++) {
    for (var j = -1; j <= 1; j++) {
      let cid = id + vec2f(f32(i), f32(j));
      let h = hash22(cid + seed);
      let c = (cid + 0.5 + (h - 0.5) * 0.55) * cell;
      let rr = cell * (0.62 + 0.22 * hash12(cid * 1.71 + seed));
      let q = vec3f(pl.x - c.x, pl.y, pl.z - c.y);
      let e = sdEllipsoid(q, vec3f(rr, rr * squash, rr * (0.8 + 0.4 * hash12(cid * 2.7 + seed))));
      if (e < best) {
        best = e;
        bestId = cid;
      }
    }
  }
  return bestId;
}

// Conservative height/depth bounds skip the 9-ellipsoid loops for queries
// far from the floor/wall planes — which is nearly all of them for rays,
// AO taps, and shadow samples up in the scene.
fn arenaFloor(p: vec3f) -> f32 {
  let bound = p.y - 0.058; // pebbles never rise above y = 0.058
  if (bound > 0.08) {
    return bound;
  }
  return min(p.y + 0.045, pebbleField(p, FLOOR_CELL, FLOOR_SEED, FLOOR_SQUASH));
}
fn arenaWall(p: vec3f) -> f32 {
  let bound = p.z - WALL_Z - 0.086; // wall pebbles protrude at most 0.086
  if (bound > 0.08) {
    return bound;
  }
  return min(p.z - WALL_Z + 0.045,
             pebbleField(vec3f(p.x, p.z - WALL_Z, p.y), WALL_CELL, WALL_SEED, WALL_SQUASH));
}

// ---------- analytic character body (bake source; eyes stay live in trace) ----------
fn charBodyAnalytic(p: vec3f) -> f32 {
  let bound = length(p - vec3f(0.0, 0.55, 0.0)) - 1.35;
  if (bound > 0.35) {
    return bound;
  }
  let px = vec3f(abs(p.x), p.y, p.z);
  var d = sdEllipsoid(p - vec3f(0.0, 0.40, 0.0), vec3f(0.31, 0.38, 0.28));
  d = smin(d, sdEllipsoid(p - vec3f(0.0, 0.82, 0.03), vec3f(0.235, 0.215, 0.205)), 0.10);
  d = smin(d, sdCapsule(px, vec3f(0.25, 0.60, 0.02), vec3f(0.45, 0.30, 0.13), 0.075), 0.055);
  // mitten hand: flat paddle palm with a thumb lobe on the inner edge
  var hand = sdEllipsoid(px - vec3f(0.47, 0.235, 0.16), vec3f(0.085, 0.115, 0.055));
  hand = smin(hand, sdEllipsoid(px - vec3f(0.388, 0.27, 0.20), vec3f(0.042, 0.06, 0.042)), 0.035);
  d = smin(d, hand, 0.045);
  d = smin(d, sdEllipsoid(px - vec3f(0.15, 0.05, 0.11), vec3f(0.13, 0.055, 0.17)), 0.04);
  // handworked lumpiness: real geometry — bump shading, silhouette, and
  // self-occlusion that normal tricks can't fake. (Once suspected of Moiré
  // against the voxel grid; the actual culprit was shadow-step banding.)
  d += (fbm(p * 5.0) - 0.5) * 0.022;
  return d;
}
fn charBodyAlbedo(p: vec3f) -> vec3f {
  // dusky clay cyan, mottled like handled plasticine
  return toLinear(vec3f(0.15, 0.47, 0.53)) * (0.82 + 0.32 * fbm(p * 9.0));
}
