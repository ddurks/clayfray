// Single-thread scene raycast for brush placement. Marches the same
// arena analytics + brickmap the tracer uses, writes hit to a 16B buffer.

//#constants
//#include scene_common.wgsl

// mirror of trace.wgsl's layout (shared uniform buffer + brick_read include).
// CLAUDE.md trap 2: this struct is hand-mirrored, so any field added to
// trace.wgsl's Uniforms must be added here too — the two array DIMENSIONS
// (PIECES_PER_FIGHTER, MAX_FIGHTERS) come from the generated //#constants
// block above, so at least those cannot drift.
struct Piece {
  invSkin: mat4x4f,
  aabbLo: vec4f,
  aabbHi: vec4f,
}

struct Fighter {
  info: vec4f,      // x = enabled, y = piece count, z = posed bound radius
  center: vec4f,    // xyz = posed bound centre, world
  pieces: array<Piece, PIECES_PER_FIGHTER>,
}

struct Uniforms {
  camPos: vec4f,
  camRight: vec4f,
  camUp: vec4f,
  camFwd: vec4f,
  res: vec4f,
  keyPos: vec4f,
  keyColor: vec4f,
  rimDir: vec4f,
  rimColor: vec4f,
  ambient: vec4f,
  material: vec4f,
  post: vec4f,
  post2: vec4f,
  mouse: vec4f, // xy = uv of cursor
  marbles: array<vec4f, MAX_FIGHTERS * 8>,
  marbleMeta: vec4f,
  sceneMeta: vec4f,
  capsMeta: vec4f,
  capsCenter: vec4f,
  capsules: array<vec4f, 32>,
  gobMeta: vec4f,
  gobs: array<vec4f, 24>,
  groundMeta: vec4f,
  swordA: vec4f,
  swordB: vec4f,
  swordCol: vec4f,
  fighters: array<Fighter, MAX_FIGHTERS>,
}
@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var<storage, read_write> pickOut: array<vec4f>;

//#include brick_read.wgsl

// (d, material): 0 arena, 3 + 0.1*fighter body, 4+ marble — enough to route
// brush/albedo. The fighter index is part of the material for the same reason
// it is in trace.wgsl: a wound has to be carved into the volume it landed on,
// and until this loop existed only player 0 could be picked at all.
fn mapPick(p: vec3f) -> vec2f {
  var d = arenaFloor(p);
  var m = 0.0;
  let fh = fightersNearest(p, d);
  if (fh.y >= 0.0) {
    d = fh.x;
    m = 3.0 + 0.1 * fh.y;
  }
  let n = i32(u.marbleMeta.x);
  for (var i = 0; i < n; i++) {
    let mb = u.marbles[i * 2];
    let md = length(p - mb.xyz) - mb.w;
    if (md < d) {
      d = md;
      m = 4.0 + f32(i);
    }
  }
  return vec2f(d, m);
}

// Which fighter a body material names, for the two rest-space lookups below.
fn pickFighter(m: f32) -> u32 {
  return u32(clamp(floor((m - 3.0) / 0.1 + 0.5), 0.0, f32(MAX_FIGHTERS - 1u)));
}

@compute @workgroup_size(1)
fn cs() {
  let uv = u.mouse.xy;
  let ndc = vec2f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  let th = tan(u.camPos.w * 0.5);
  let rd = normalize(u.camFwd.xyz + ndc.x * th * u.camRight.w * u.camRight.xyz +
                     ndc.y * th * u.camUp.xyz);
  let ro = u.camPos.xyz;
  var t = 0.0;
  var hit = 0.0;
  var mat = 0.0;
  for (var i = 0; i < 200; i++) {
    let dm = mapPick(ro + rd * t);
    if (dm.x < 0.002) {
      hit = 1.0;
      mat = dm.y;
      break;
    }
    t += dm.x * 0.85;
    if (t > 14.0) {
      break;
    }
  }
  let p = ro + rd * t;
  // surface normal (central diff): gobs launch along it; brush-facing too
  let e = 0.0045;
  let k = vec2f(1.0, -1.0);
  var nrm = k.xyy * mapPick(p + k.xyy * e).x + k.yyx * mapPick(p + k.yyx * e).x +
            k.yxy * mapPick(p + k.yxy * e).x + k.xxx * mapPick(p + k.xxx * e).x;
  let len = length(nrm);
  nrm = select(vec3f(0.0, 1.0, 0.0), nrm / len, len > 1e-6);
  // carved-surface albedo: what a gob torn from here is made of
  var alb = vec3f(0.024, 0.19, 0.25);
  // rest-space address of the hit: what the sculpt tools must edit at, since
  // the brick volume is authored in rest space and the body is warped away
  // from it by pose AND by the fighter's world root
  var rest = p;
  // -1 = the ray hit no clay, so there is no volume to carve
  var who = -1.0;
  if (hit > 0.5 && mat > 2.5 && mat < 3.5) {
    let f = pickFighter(mat);
    alb = fighterAlbedo(f, p);
    rest = fighterRestPoint(f, p);
    who = f32(f);
  }
  pickOut[0] = vec4f(p, hit);
  pickOut[1] = vec4f(nrm, mat);
  pickOut[2] = vec4f(alb, 0.0);
  // w carries WHICH fighter owns `rest` — a rest point is meaningless without
  // it once there is more than one volume to address.
  pickOut[3] = vec4f(rest, who);
}
