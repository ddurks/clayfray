// Single-thread scene raycast for brush placement. Marches the same
// arena analytics + brickmap the tracer uses, writes hit to a 16B buffer.

//#include scene_common.wgsl

// mirror of trace.wgsl's layout (shared uniform buffer + brick_read include)
struct Piece {
  invSkin: mat4x4f,
  skin: mat4x4f,    // forward, for the round-trip consistency check
  aabbLo: vec4f,
  aabbHi: vec4f,
  capA: vec4f,
  capB: vec4f,
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
  marbles: array<vec4f, 16>,
  marbleMeta: vec4f,
  capsMeta: vec4f,
  capsCenter: vec4f,
  capsules: array<vec4f, 32>,
  boneMeta: vec4f,
  pieces: array<Piece, 16>,
}
@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var<storage, read_write> pickOut: array<vec4f>;

//#include brick_read.wgsl

fn mapPick(p: vec3f) -> f32 {
  var d = min(arenaFloor(p), arenaWall(p));
  d = min(d, charDist(p));
  let n = i32(u.marbleMeta.x);
  for (var i = 0; i < n; i++) {
    let mb = u.marbles[i * 2];
    d = min(d, length(p - mb.xyz) - mb.w);
  }
  return d;
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
  for (var i = 0; i < 200; i++) {
    let d = mapPick(ro + rd * t);
    if (d < 0.002) {
      hit = 1.0;
      break;
    }
    t += d * 0.85;
    if (t > 14.0) {
      break;
    }
  }
  let p = ro + rd * t;
  pickOut[0] = vec4f(p, hit);
}
