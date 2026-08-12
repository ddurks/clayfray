// Film-look pass: 80s 16mm TV transfer. Gate weave -> bloom -> tonemap ->
// grain (stepped at 25 Hz film frames) -> warm-black vignette -> gamma.

struct Uniforms {
  camPos: vec4f,
  camRight: vec4f,
  camUp: vec4f,
  camFwd: vec4f,
  res: vec4f,     // xy = resolution, z = grainFrame
  keyPos: vec4f,
  keyColor: vec4f,
  rimDir: vec4f,
  rimColor: vec4f,
  ambient: vec4f,
  material: vec4f,
  post: vec4f,    // x = grain, y = vignette inner, z = vignette outer, w = weave px
  post2: vec4f,   // x = exposure, y = bloom amount, z = bloom threshold
}
@group(0) @binding(0) var<uniform> u: Uniforms;
@group(0) @binding(1) var hdrTex: texture_2d<f32>;
@group(0) @binding(2) var samp: sampler;

struct VOut {
  @builtin(position) pos: vec4f,
  @location(0) uv: vec2f,
}

@vertex
fn vs(@builtin(vertex_index) vi: u32) -> VOut {
  var out: VOut;
  let uvc = vec2f(f32((vi << 1u) & 2u), f32(vi & 2u));
  out.pos = vec4f(uvc * 2.0 - 1.0, 0.0, 1.0);
  out.uv = vec2f(uvc.x, 1.0 - uvc.y);
  return out;
}

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

fn aces(x: vec3f) -> vec3f {
  return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), vec3f(0.0),
               vec3f(1.0));
}

@fragment
fn fs(in: VOut) -> @location(0) vec4f {
  let frameId = u.res.z;

  // gate weave: the whole frame drifts a hair, re-rolled per film frame
  let weave = (hash22(vec2f(frameId, 3.7)) - 0.5) * u.post.w * 2.0 / u.res.xy;
  let uv = in.uv + weave;

  var col = textureSample(hdrTex, samp, uv).rgb;

  // mild bloom: 8-tap threshold blur in HDR
  let px = 1.0 / u.res.xy;
  var bloom = vec3f(0.0);
  for (var i = 0; i < 8; i++) {
    let a = f32(i) * 0.785398;
    let o = vec2f(cos(a), sin(a)) * 3.5 * px;
    bloom += max(textureSample(hdrTex, samp, uv + o).rgb - vec3f(u.post2.z), vec3f(0.0));
  }
  col += bloom / 8.0 * u.post2.y;

  col = aces(col * u.post2.x);

  // film grain, stepped at 25 Hz, strongest in midtones
  let lum = dot(col, vec3f(0.299, 0.587, 0.114));
  let g = hash13(vec3f(in.pos.xy, frameId)) - 0.5;
  col += g * u.post.x * (lum * (1.0 - lum) * 3.0 + 0.05);

  // vignette toward warm black, never neutral
  let q = (in.uv - 0.5) * vec2f(u.res.x / u.res.y, 1.0) * 2.0;
  let v = smoothstep(u.post.z, u.post.y, length(q));
  col = mix(vec3f(0.008, 0.006, 0.004), col, v);

  col = pow(max(col, vec3f(0.0)), vec3f(1.0 / 2.2));
  return vec4f(col, 1.0);
}
