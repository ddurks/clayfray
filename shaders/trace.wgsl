// clayfray sphere tracer: whole scene is one SDF, no triangles.
// Arena is analytic; the character bodies live in the sparse brickmap
// (carveable). Eyes stay analytic rigid beads. Writes linear HDR.

// The generated geometry block comes FIRST because the Uniforms struct below
// is sized by it: PIECES_PER_FIGHTER and MAX_FIGHTERS come from src/brick.h,
// so the CPU and GPU sides of the fighter array cannot drift (trap 2 is about
// hand-mirrored layouts; these two dimensions are no longer hand-mirrored).
//#constants

// One articulated region of a fighter's rest volume, posed by invSkin.
//   aabbLo.w  the transform's smallest SINGULAR value (a shear can have
//             unit-length columns and a much smaller sigma_min; trusting a
//             column norm makes the march step through the skin)
//   capB.w    BITMASK of the bones whose clay this piece carries — dead with
//             the skeleton, kept only by the legacy path
struct Piece {
  invSkin: mat4x4f, // world -> rest, rigid (posed inverse skin matrix)
  skin: mat4x4f,    // rest -> world, for the round-trip consistency check
  aabbLo: vec4f,    // rest-space tight bound of the piece's zero set
  aabbHi: vec4f,
  capA: vec4f,      // rest capsule end a, w = swollen radius
  capB: vec4f,      // rest capsule end b
}

// One fighter: everything the tracer needs to sample its body. Its CLAY is not
// here — that is slice `f` of the three storage buffers (brick_read.wgsl).
//
// This replaced a hero/foe pair of hand-written fields (`pieces` at slot 66,
// `foePieces` at 293, each a 16-piece array). Two fighters that way cost 384
// of 488 uniform slots and had no room for a third; an array of 3-piece
// fighters costs 38 slots each, so FOUR of them are smaller than the old TWO.
struct Fighter {
  info: vec4f,      // x = enabled, y = piece count, z = posed bound radius
  center: vec4f,    // xyz = posed bound centre, world
  pieces: array<Piece, PIECES_PER_FIGHTER>,
}

struct Uniforms {
  camPos: vec4f,    // xyz, w = fovY
  camRight: vec4f,  // xyz, w = aspect
  camUp: vec4f,     // xyz, w = time
  camFwd: vec4f,    // xyz, w = poseTime (12 Hz quantized)
  res: vec4f,       // xy = resolution, z = grainFrame, w = aaSamples
  keyPos: vec4f,    // xyz, w = intensity
  keyColor: vec4f,  // rgb, w = falloff
  rimDir: vec4f,    // xyz, w = intensity
  rimColor: vec4f,  // rgb
  ambient: vec4f,   // rgb, w = aoStrength
  material: vec4f,  // x = detail, y = boil, z = shadow k, w = sheen
  post: vec4f,
  post2: vec4f,
  mouse: vec4f,          // slot 13 (used by pick)
  marbles: array<vec4f, 32>, // 16 x {pos,radius},{color,-} — 4 beads/fighter
  marbleMeta: vec4f,     // x = count
  sceneMeta: vec4f,      // x = live fighter count, y = joint smin k,
                         // z = piece box margin
  capsMeta: vec4f,       // x = capsule count, y = bounding radius
  capsCenter: vec4f,     // xyz = bounding center (posed)
  capsules: array<vec4f, 32>, // 16 x {a,radius},{b,-}, posed per 12 Hz step
  gobMeta: vec4f,        // x = in-flight gob count (M4.6 conservation)
  gobs: array<vec4f, 24>, // 12 x {pos, radius}, {color, -}; 12 Hz stepped
  groundMeta: vec4f,     // w = clay top bound y (0 = field empty)
  swordA: vec4f,         // M4.7: hilt xyz, w = blade radius (0 = no sword)
  swordB: vec4f,         // tip xyz
  swordCol: vec4f,       // emissive rgb
  fighters: array<Fighter, MAX_FIGHTERS>,
}
@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var hdrOut: texture_storage_2d<rgba16float, write>;

//#include scene_common.wgsl
//#include brick_read.wgsl
//#include ground_read.wgsl

const MAT_FLOOR: f32 = 1.0;
const MAT_WALL: f32 = 2.0;
// Fighter f is MAT_BODY + 0.1*f, i.e. 3.0, 3.1, 3.2, 3.3 — all of them under
// the 3.5 clay cutoff on purpose, so every `m < 3.5` predicate in this file
// (grain, AO, rim scale, the marbles-only glint rule) keeps classifying every
// fighter as clay with no extra tests. Only albedo has to recover the index.
// This generalises M5's MAT_FOE = 3.2 hack, which was the same trick for one
// hardcoded opponent. MAT_FIGHTER_STEP * MAX_FIGHTERS must stay under 0.5.
const MAT_BODY: f32 = 3.0;
const MAT_FIGHTER_STEP: f32 = 0.1;
const MAT_EYE: f32 = 4.0;   // + marble index (4..19)
const MAT_PUPIL: f32 = 5.0;
const MAT_GCLAY: f32 = 20.0;
const MAT_GOB: f32 = 21.0;  // + gob index (21..32)
const MAT_SWORD: f32 = 50.0; // M4.7: rigid emissive blade, never clay

// ---------- marbles: rigid glass beads, data-driven (never clay) ----------
fn marblesDist(p: vec3f) -> f32 {
  var d = 1e9;
  let n = i32(u.marbleMeta.x);
  for (var i = 0; i < n; i++) {
    let m = u.marbles[i * 2];
    d = min(d, length(p - m.xyz) - m.w);
  }
  return d;
}

// ---------- scene ----------
fn map(p: vec3f) -> vec2f {
  var d = arenaFloor(p);
  var m = MAT_FLOOR;
  let wd = arenaWall(p);
  if (wd < d) {
    d = wd;
    m = MAT_WALL;
  }
  // every live fighter, nearest wins (brick_read.wgsl owns the loop)
  let fh = fightersNearest(p, d);
  if (fh.y >= 0.0) {
    d = fh.x;
    m = MAT_BODY + MAT_FIGHTER_STEP * fh.y;
  }
  let n = i32(u.marbleMeta.x);
  for (var i = 0; i < n; i++) {
    let mb = u.marbles[i * 2];
    let md = length(p - mb.xyz) - mb.w;
    if (md < d) {
      d = md;
      m = MAT_EYE + f32(i);
    }
  }
  let dg = groundClayDist(p);
  if (dg < d) {
    d = dg;
    m = MAT_GCLAY;
  }
  let gn = i32(u.gobMeta.x);
  for (var i = 0; i < gn; i++) {
    let gb = u.gobs[i * 2];
    let gd = length(p - gb.xyz) - gb.w;
    if (gd < d) {
      d = gd;
      m = MAT_GOB + f32(i);
    }
  }
  if (u.swordA.w > 0.0) {
    let sd = sdCapsule(p, u.swordA.xyz, u.swordB.xyz, u.swordA.w);
    if (sd < d) {
      d = sd;
      m = MAT_SWORD;
    }
  }
  return vec2f(d, m);
}

fn march(ro: vec3f, rd: vec3f) -> vec2f {
  var t = 0.0;
  for (var i = 0; i < 256; i++) {
    let dm = map(ro + rd * t);
    // tight epsilon: grazing rays on the voxelized surface otherwise
    // register hits a hair off-silhouette that light up as a rim fringe
    if (dm.x < 0.00025 * (1.0 + t)) {
      return vec2f(t, dm.y);
    }
    t += dm.x * 0.85;
    if (t > 14.0) {
      break;
    }
  }
  return vec2f(-1.0, 0.0);
}

// Shading variant of the scene: same arena, loose (mid-estimate) character
// distance. Marching stays on the conservative map().
fn mapLoose(p: vec3f) -> f32 {
  var d = min(arenaFloor(p), arenaWall(p));
  d = fightersDistLoose(p, d);
  d = min(d, marblesDist(p));
  d = min(d, groundClaySmooth(p)); // piles shade/occlude like set dressing
  return d;
}

fn calcNormal(p: vec3f) -> vec3f {
  let e = 0.0045; // ~1.6 voxels: smooths trilinear gradient kinks
  let k = vec2f(1.0, -1.0);
  let g = k.xyy * map(p + k.xyy * e).x + k.yyx * map(p + k.yyx * e).x +
          k.yxy * map(p + k.yxy * e).x + k.xxx * map(p + k.xxx * e).x;
  let len = length(g);
  if (len < 1e-6) {
    return vec3f(0.0, 1.0, 0.0);
  }
  return g / len;
}

fn calcAO(p: vec3f, n: vec3f) -> f32 {
  var occ = 0.0;
  var sca = 1.0;
  for (var i = 0; i < 5; i++) {
    let h = 0.012 + 0.11 * f32(i);
    occ += (h - mapLoose(p + n * h)) * sca;
    sca *= 0.85;
  }
  return clamp(1.0 - u.ambient.w * 1.6 * occ, 0.0, 1.0);
}

// Smooth character occluder proxy: imported characters use bone capsules
// (fitted at import, posed rigidly per 12 Hz step); the analytic blob
// remains the fallback. smin-joined so the penumbra term sees no creases.
fn charProxy(p: vec3f) -> f32 {
  let n = i32(u.capsMeta.x);
  if (n == 0) {
    return charBodyAnalytic(p);
  }
  let bound = length(p - u.capsCenter.xyz) - u.capsMeta.y;
  if (bound > 0.3) {
    return bound;
  }
  var d = 1e9;
  for (var i = 0; i < n; i++) {
    let ca = u.capsules[i * 2];
    let cb = u.capsules[i * 2 + 1];
    d = smin(d, sdCapsule(p, ca.xyz, cb.xyz, ca.w), 0.05);
  }
  return d;
}


// Shadow STEPS need the conservative field (an overestimate tunnels through
// the surface) but the PENUMBRA term needs the smooth one — the crease-y
// seed-based estimates paint organic swirls across every lit surface.
// Penumbra scene. The character occluder is max(smooth proxy, real field):
// the max opens carved cavities to light (real field says air where the
// proxy still says clay — without this every crater renders as an unlit
// void, reading as hollow) while the smooth proxy floors the field's
// downward errors, which are what band under a penumbra term.
fn mapPenumbra(p: vec3f) -> f32 {
  var d = min(arenaFloor(p), arenaWall(p));
  // The max() with the real field is NOT optional: the fitted capsules bulge
  // past the true surface, so using the proxy alone as the occluder buries
  // anything sitting proud of the body — the marble eyes render black. Speed
  // comes from the conservative early-out inside the loose field instead,
  // which is safe because it never REPLACES the field, only skips evaluating
  // it where a bound already proves the surface is far.
  //
  // The capsule proxy is FIGHTER 0's ONLY: capsules are fitted per character
  // at import and packed once (u.capsules is not per-fighter), and every other
  // fighter has always taken the plain loose field here — M5's opponent
  // included. Keeping that split is what makes this refactor a no-op on the
  // rendered image; generalising the proxy would change the hero's penumbra,
  // which is a look change and belongs in its own pass with a human looking.
  d = min(d, max(charProxy(p), fighterDistLoose(0u, p)));
  for (var f = 1u; f < u32(u.sceneMeta.x); f++) {
    d = min(d, fighterDistLoose(f, p));
  }
  d = min(d, marblesDist(p));
  d = min(d, groundClaySmooth(p));
  return d;
}

// Geometric (field-independent) stepping: stepping by the field itself
// decides WHERE the min-samples land, which imprints the field's crease
// pattern into the penumbra no matter how smooth the sampled values are.
// Step count and growth are a pair: both walk t from 0.02 out to ~4.5 m, which
// is past any occluder in the arena. 44 steps at 1.13 oversampled that range —
// the penumbra term weights by 1/t, so the far samples it spent most of its
// steps on barely move the result. 22 at 1.28 covers the same distance for
// half as many mapPenumbra() calls.
//
// This is the single most expensive term in the frame: measured at 25.9 ms of
// 70 ms (37%), because it is ~half of all field evaluations per shaded pixel
// (22 shadow vs ~35 march + 4 normal + 5 AO) and mapPenumbra is the heaviest
// variant — it adds charProxy's 16-capsule smin on top of the two bodies.
fn softShadow(ro: vec3f, rd: vec3f, tmax: f32) -> f32 {
  var res = 1.0;
  var t = 0.02;
  for (var i = 0; i < 22; i++) {
    res = min(res, u.material.z * mapPenumbra(ro + rd * t) / t);
    t *= 1.28;
    if (res < 0.004 || t > tmax) {
      break;
    }
  }
  return clamp(res, 0.0, 1.0);
}

// ---------- albedo ----------
fn stonePalette(h: f32) -> vec3f {
  let i = i32(floor(h * 8.0));
  switch i {
    case 0: { return toLinear(vec3f(0.66, 0.42, 0.45)); }
    case 1: { return toLinear(vec3f(0.48, 0.29, 0.43)); }
    case 2: { return toLinear(vec3f(0.29, 0.49, 0.52)); }
    case 3: { return toLinear(vec3f(0.72, 0.63, 0.42)); }
    case 4: { return toLinear(vec3f(0.42, 0.49, 0.29)); }
    case 5: { return toLinear(vec3f(0.54, 0.29, 0.24)); }
    case 6: { return toLinear(vec3f(0.29, 0.36, 0.24)); }
    default: { return toLinear(vec3f(0.60, 0.54, 0.42)); }
  }
}
fn albedoFor(p: vec3f, m: f32) -> vec3f {
  if (m < 1.5) {
    let cid = pebbleCell(p, FLOOR_CELL, FLOOR_SEED, FLOOR_SQUASH);
    return stonePalette(hash12(cid * 3.3 + 11.0)) * (0.60 + 0.35 * fbm(p * 7.0)) *
           (0.82 + 0.36 * fbm(p * 23.0));
  } else if (m < 2.5) {
    let cid = pebbleCell(vec3f(p.x, p.z - WALL_Z, p.y), WALL_CELL, WALL_SEED, WALL_SQUASH);
    return stonePalette(hash12(cid * 5.1 + 4.0)) * (0.42 + 0.28 * fbm(p * 7.0)) *
           (0.82 + 0.36 * fbm(p * 23.0));
  } else if (m < 3.5) {
    // per-voxel color from the brickmap (carve reveals, add stamps), read out
    // of whichever fighter's volume the material index names
    let f = u32(clamp(floor((m - MAT_BODY) / MAT_FIGHTER_STEP + 0.5), 0.0,
                      f32(MAX_FIGHTERS - 1u)));
    return fighterAlbedo(f, p);
  } else if (m > MAT_GOB - 0.5) {
    // in-flight gob: the wound albedo it was torn with
    let gi = clamp(i32(m - MAT_GOB + 0.5), 0, 11);
    return u.gobs[gi * 2 + 1].rgb;
  } else if (m > MAT_GCLAY - 0.5) {
    return groundAlbedo(p);
  }
  // marble: per-prop color from the uniform (already linear)
  let idx = clamp(i32(m - MAT_EYE + 0.5), 0, 15);
  return u.marbles[idx * 2 + 1].rgb;
}

// ---------- shading ----------
fn boilSeed() -> vec3f {
  return vec3f(hash13(vec3f(u.camFwd.w * 12.0, 3.1, 7.7)) * 61.7 * u.material.y);
}

fn shade(p: vec3f, rd: vec3f, m: f32) -> vec3f {
  if (m > MAT_SWORD - 0.5) {
    // rigid emissive blade: unlit and bright, the bloom pass does the glow.
    // a soft core-to-edge falloff keeps the debug line from banding.
    let core = clamp(1.0 - sdCapsule(p, u.swordA.xyz, u.swordB.xyz, 0.0) /
                             max(u.swordA.w, 1e-4), 0.0, 1.0);
    return u.swordCol.rgb * (2.2 + 2.5 * core);
  }
  // nGeo: macro shape, drives AO + shadow ray offset. n: micro detail on
  // top, drives lighting only — occluding along fake dimple normals stamps
  // dark pits over every textured surface.
  let nGeo = calcNormal(p);
  var n = nGeo;

  // gradient step must stay well under the noise wavelength (2pi/34 ~ 0.18m
  // features at ~29mm): a coarse step aliases the gradient into swirl bands
  let seed = boilSeed() * 1.37;
  let eps = 0.004;
  let g = vec3f(
    vnoise((p + vec3f(eps, 0.0, 0.0)) * 34.0 + seed) - vnoise((p - vec3f(eps, 0.0, 0.0)) * 34.0 + seed),
    vnoise((p + vec3f(0.0, eps, 0.0)) * 34.0 + seed) - vnoise((p - vec3f(0.0, eps, 0.0)) * 34.0 + seed),
    vnoise((p + vec3f(0.0, 0.0, eps)) * 34.0 + seed) - vnoise((p - vec3f(0.0, 0.0, eps)) * 34.0 + seed)) /
    (2.0 * eps);
  if (m < 2.5) {
    let seed2 = seed * 0.31;
    let e2 = 0.012;
    let g2 = vec3f(
      vnoise((p + vec3f(e2, 0.0, 0.0)) * 9.0 + seed2) - vnoise((p - vec3f(e2, 0.0, 0.0)) * 9.0 + seed2),
      vnoise((p + vec3f(0.0, e2, 0.0)) * 9.0 + seed2) - vnoise((p - vec3f(0.0, e2, 0.0)) * 9.0 + seed2),
      vnoise((p + vec3f(0.0, 0.0, e2)) * 9.0 + seed2) - vnoise((p - vec3f(0.0, 0.0, e2)) * 9.0 + seed2)) /
      (2.0 * e2);
    n = normalize(n + u.material.x * (0.016 * g + 0.007 * g2));
  } else if (m < 3.5 || m > MAT_GCLAY - 0.5) {
    // body: dry-clay grain is sub-millimeter — sub-pixel — so its honest
    // rendering is decorrelated speckle, not a smooth noise gradient (any
    // band-limited gradient reads as fingerprint iso-bands under a key
    // light). Grid-quantized hash jitter, reseeded per pose step (boil),
    // plus a gentle handworked-lump octave.
    let cellp = floor(p * 300.0 + seed * 31.0);
    let jitter = vec3f(hash13(cellp), hash13(cellp + 17.1), hash13(cellp + 41.7)) - 0.5;
    let seed3 = seed * 0.53;
    let e3 = 0.02;
    let g3 = vec3f(
      vnoise((p + vec3f(e3, 0.0, 0.0)) * 5.5 + seed3) - vnoise((p - vec3f(e3, 0.0, 0.0)) * 5.5 + seed3),
      vnoise((p + vec3f(0.0, e3, 0.0)) * 5.5 + seed3) - vnoise((p - vec3f(0.0, e3, 0.0)) * 5.5 + seed3),
      vnoise((p + vec3f(0.0, 0.0, e3)) * 5.5 + seed3) - vnoise((p - vec3f(0.0, 0.0, e3)) * 5.5 + seed3)) /
      (2.0 * e3);
    n = normalize(n + u.material.x * (0.16 * jitter + 0.03 * g3));
  }

  let ao = calcAO(p, nGeo);
  var alb = albedoFor(p, m);
  if (u.post2.w > 1.5 && m > 2.5 && m < 3.5) {
    alb = toLinear(vec3f(0.15, 0.47, 0.53)); // debug 2: flat body albedo
  }

  let toKey = u.keyPos.xyz - p;
  let dk = length(toKey);
  let lk = toKey / dk;
  let spotDir = normalize(vec3f(0.0, 0.55, 0.0) - u.keyPos.xyz);
  let cone = pow(clamp(dot(-lk, spotDir), 0.0, 1.0), 7.0);
  let att = cone * u.keyPos.w / (1.0 + u.keyColor.w * dk * dk);
  // Shadow-ray origin bias. Must clear ONE CELL, not one voxel: an
  // unallocated cell's conservative distance has a 0.5*SPAN floor and the
  // coarse field is cell-granular, so a ray starting inside a cell of the
  // surface self-intersects and the terminator breaks into ragged black
  // gashes. This was a hardcoded 0.022 = 1.15 * SPAN at kGrid=74; frozen as a
  // literal it silently became 0.57 * SPAN at kGrid=37 and the acne appeared.
  // Keep it expressed in SPAN so it tracks the resolution.
  let sh = softShadow(p + nGeo * (1.15 * SPAN), lk, dk);
  let ndl = clamp((dot(n, lk) + 0.25) / 1.25, 0.0, 1.0);
  var col = alb * u.keyColor.rgb * att * ndl * sh;

  // dry clay: nearly no sheen — broad, dim, panel-tunable (material.w)
  let hv = normalize(lk - rd);
  col += u.keyColor.rgb * att * sh * pow(clamp(dot(n, hv), 0.0, 1.0), 6.0) * u.material.w;

  // cool rim from the dark side — for the character's silhouette; the set
  // barely catches it (rim on rough stones reads as wetness)
  let rimScale = select(0.25, 1.0, m > 2.5);
  let rim = pow(clamp(1.0 + dot(n, rd), 0.0, 1.0), 2.5);
  col += u.rimColor.rgb * u.rimDir.w * rimScale * rim *
         clamp(dot(n, normalize(u.rimDir.xyz)) * 0.5 + 0.5, 0.0, 1.0) * ao;

  col += alb * u.ambient.rgb * ao;

  if (m > 3.5 && m < MAT_GCLAY - 0.5) {
    // glass glint: marbles only — landed clay stays matte
    col += u.keyColor.rgb * att * sh * pow(clamp(dot(n, hv), 0.0, 1.0), 60.0) * 0.5;
  }
  return col;
}

fn background(rd: vec3f) -> vec3f {
  let glow = clamp(dot(rd, normalize(u.keyPos.xyz)), 0.0, 1.0);
  return mix(vec3f(0.004, 0.003, 0.0025), vec3f(0.035, 0.022, 0.012), pow(glow, 3.0));
}

@compute @workgroup_size(8, 8)
fn cs(@builtin(global_invocation_id) gid: vec3u) {
  if (gid.x >= u32(u.res.x) || gid.y >= u32(u.res.y)) {
    return;
  }
  let aa = max(i32(u.res.w), 1);
  var col = vec3f(0.0);
  for (var sy = 0; sy < aa; sy++) {
    for (var sx = 0; sx < aa; sx++) {
      let off = (vec2f(f32(sx), f32(sy)) + 0.5) / f32(aa);
      let uv = (vec2f(f32(gid.x), f32(gid.y)) + off) / u.res.xy;
      let ndc = vec2f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
      let th = tan(u.camPos.w * 0.5);
      let rd = normalize(u.camFwd.xyz + ndc.x * th * u.camRight.w * u.camRight.xyz +
                         ndc.y * th * u.camUp.xyz);
      let ro = u.camPos.xyz;
      let tm = march(ro, rd);
      var c: vec3f;
      if (tm.x < 0.0) {
        c = background(rd);
      } else if (u.post2.w > 0.5 && u.post2.w < 1.5) {
        c = calcNormal(ro + rd * tm.x) * 0.5 + 0.5; // debug: raw field normals
      } else if (u.post2.w > 2.5) {
        // debug 3: |grad charDist| - 1 heatmap. green = true SDF, red =
        // gradient too steep, blue = too shallow
        let p = ro + rd * tm.x;
        let ge = VOXEL; // one voxel (brick_read.wgsl — don't hardcode)
        let gx = fighterDist(0u, p + vec3f(ge, 0.0, 0.0)) -
                 fighterDist(0u, p - vec3f(ge, 0.0, 0.0));
        let gy = fighterDist(0u, p + vec3f(0.0, ge, 0.0)) -
                 fighterDist(0u, p - vec3f(0.0, ge, 0.0));
        let gz = fighterDist(0u, p + vec3f(0.0, 0.0, ge)) -
                 fighterDist(0u, p - vec3f(0.0, 0.0, ge));
        let mag = length(vec3f(gx, gy, gz)) / (2.0 * ge);
        let err = mag - 1.0;
        c = vec3f(clamp(err * 3.0, 0.0, 1.0), 0.25, clamp(-err * 3.0, 0.0, 1.0));
      } else {
        let p = ro + rd * tm.x;
        c = shade(p, rd, tm.y);
        c = mix(c, vec3f(0.008, 0.006, 0.004), 1.0 - exp(-0.045 * tm.x * tm.x));
      }
      col += c;
    }
  }
  col /= f32(aa * aa);
  textureStore(hdrOut, vec2i(gid.xy), vec4f(col, 1.0));
}
