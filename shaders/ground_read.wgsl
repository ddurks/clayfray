// Read-side ground clay heightfield (see ground.wgsl for the writer).
// Clay top = pebble base + deposited thickness; the union into the scene is
// a vertical distance scaled to stay conservative under kernel + drape
// slopes. Includers must already have declared the Uniforms `u`
// (groundMeta: x = origin, y = texel, z = N, w = clay top bound).

// The three ground arrays (floor drape, deposited thickness, clay color) are
// ONE buffer here, addressed through the G_* base indices in the //#constants
// block. That is not packing for its own sake: core WebGPU guarantees a shader
// stage only EIGHT storage buffers, and two fighters at three bindings each
// plus a ground trio needs nine — a mobile browser reporting the guaranteed
// minimum would fail to create the trace pipeline outright. See the region map
// in src/ground.h.
//
// G_N / G_ORIGIN / G_TEXEL / G_BASE / G_HEIGHT / G_COLOR come from the
// //#constants block in the including ROOT (trace.wgsl), generated from
// src/ground.h. They used to be hand-copied here; don't reintroduce that.
@group(1) @binding(7) var<storage, read> gField: array<u32>;

// Base and height are f32 data in the shared u32 buffer; color is already u32.
fn gBase(i: u32) -> f32 { return bitcast<f32>(gField[G_BASE + i]); }
fn gHeight(i: u32) -> f32 { return bitcast<f32>(gField[G_HEIGHT + i]); }
fn gColor(i: u32) -> u32 { return gField[G_COLOR + i]; }

// vertical-distance Lipschitz guard: tolerates surface slope up to ~3
const G_KLIP: f32 = 0.3;

fn gTexIdx(t: vec2i) -> u32 {
  let c = clamp(t, vec2i(0), vec2i(G_N - 1));
  return u32(c.x) + u32(c.y) * u32(G_N);
}

// Clay only exists where clay was deposited. A skirt-below-floor trick can't
// hide the empty footprint: after the G_KLIP scaling and the march epsilon a
// 2 mm skirt still reads as a hit, so bare floor tinted clay-cyan the instant
// the field switched on. Instead, gate on ACTUAL thickness: below G_THMIN the
// texel has no clay and the field yields to the real arena floor. The cutoff
// sits at a sub-mm-thickness contour (the very lip of a pile), where the clay
// contributes no visible silhouette/occlusion, so the discontinuity is unseen.
const G_THMIN: f32 = 0.0015;

// deposited clay thickness (m), bilinear, >= 0
fn groundThicknessAt(xz: vec2f) -> f32 {
  let g = (xz - vec2f(G_ORIGIN)) / G_TEXEL - 0.5;
  let t0 = vec2i(floor(g));
  let f = g - floor(g);
  var h: array<f32, 4>;
  for (var k = 0; k < 4; k++) {
    h[k] = gHeight(gTexIdx(t0 + vec2i(k & 1, k >> 1)));
  }
  return mix(mix(h[0], h[1], f.x), mix(h[2], h[3], f.x), f.y);
}

// clay top y = floor drape + deposited thickness, bilinear
fn groundTopAt(xz: vec2f) -> f32 {
  let g = (xz - vec2f(G_ORIGIN)) / G_TEXEL - 0.5;
  let t0 = vec2i(floor(g));
  let f = g - floor(g);
  var top: array<f32, 4>;
  for (var k = 0; k < 4; k++) {
    let idx = gTexIdx(t0 + vec2i(k & 1, k >> 1));
    top[k] = gBase(idx) + gHeight(idx);
  }
  return mix(mix(top[0], top[1], f.x), mix(top[2], top[3], f.x), f.y);
}

// Conservative distance for MARCHING only (map()). Gated arenaFloor-style:
// the early-out branch always returns > its threshold, so there is no
// near-zero shell at the bound plane for grazing rays to phantom-hit.
fn groundClayDist(p: vec3f) -> f32 {
  let maxTop = u.groundMeta.w;
  if (maxTop <= 0.0) {
    return 1e9; // nothing has landed yet: free
  }
  let bound = p.y - maxTop;
  if (bound > 0.05) {
    return bound;
  }
  // no clay in this texel -> yield to the real floor (a positive value that
  // clears the march epsilon, so the ground never wins the hit here)
  if (groundThicknessAt(p.xz) < G_THMIN) {
    return max(bound, 0.006);
  }
  var d = (p.y - groundTopAt(p.xz)) * G_KLIP;
  // beyond the field footprint no clay can exist: lateral distance rules
  let lat = max(abs(p.x), abs(p.z)) + G_ORIGIN;
  if (lat > 0.0) {
    d = max(d, lat);
  }
  return d;
}

// Smooth mid-estimate for AO / penumbra. Those terms band under
// conservative underestimates (M2 lesson: a scaled distance reads as
// phantom occlusion), so this is the UNSCALED vertical distance — smooth
// everywhere the shading terms sample it, never spuriously small.
// This is the SHADING HOT PATH: 5 AO taps + up to 44 penumbra steps per lit
// pixel. The early-outs skip the fetch; the thickness gate keeps the empty
// footprint from occluding the floor (the whole-arena darkening bug).
// What a clay-free column reports as its distance to clay. Large enough that
// the penumbra term (min(res, k*d/t)) leaves bare floor alone, which is the
// whole point of the branch that uses it.
const G_CLAY_FAR: f32 = 1.0;

fn groundClaySmooth(p: vec3f) -> f32 {
  let maxTop = u.groundMeta.w;
  if (maxTop <= 0.0) {
    return 1e9;
  }
  // lateral: outside the field footprint the nearest clay is at least this
  // far horizontally — smooth and monotone, no fetch. Catches low samples
  // (penumbra rays skimming the floor, distant AO taps) the vertical gate
  // can't, since their bound is negative.
  let lat = max(abs(p.x), abs(p.z)) + G_ORIGIN;
  if (lat > 0.05) {
    return lat;
  }
  let bound = p.y - maxTop;
  if (bound > 0.15) {
    return bound;
  }
  // No clay in THIS column: report far, so bare floor keeps its own AO and
  // shadows. This used to `return max(bound, 0.05)`, which is the opposite of
  // what the comment says and is what made shadows creep across the scene as
  // gobs piled up:
  //
  //   `bound` is p.y - maxTop, and maxTop only ever GROWS (ground.cpp keeps
  //   maxH_ = max(maxH_, ...), so a pile raises it permanently). Once any pile
  //   is tall, `bound` is negative for everything near the floor, the cheap
  //   `bound > 0.15` exit above stops firing over more and more of the scene,
  //   and all of it lands here and reports a CONSTANT 5 cm to clay. The
  //   penumbra term is min(res, k * d / t), so a fixed tiny d darkens
  //   everything — and because maxH_ never falls, it never recovers.
  //
  // The honest answer for a clay-free column is "no clay here", and this is
  // the SMOOTH field (trap 3) — AO and penumbra only, never marched — so it
  // may report far without any risk of overstepping a surface. The cost is
  // that a pile no longer shadows the bare floor immediately beside it; a
  // lateral distance-to-clay field would fix that properly and is the right
  // follow-up if the contact shadow is missed.
  if (groundThicknessAt(p.xz) < G_THMIN) {
    return max(bound, G_CLAY_FAR);
  }
  return p.y - groundTopAt(p.xz);
}

fn groundAlbedo(p: vec3f) -> vec3f {
  let g = (p.xz - vec2f(G_ORIGIN)) / G_TEXEL - 0.5;
  let c = unpack4x8unorm(gColor(gTexIdx(vec2i(round(g))))).rgb;
  // same handled-plasticine mottle the body wears
  return c * (0.82 + 0.32 * fbm(p * 9.0));
}
