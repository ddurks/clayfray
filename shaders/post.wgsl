// Film-look pass: 80s 16mm TV transfer. Gate weave -> bloom -> tonemap ->
// grain (stepped at 25 Hz film frames) -> warm-black vignette -> gamma.

//#constants

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
  // Post shares ONE uniform buffer with trace and pick (trap 2) and needs the
  // foveation pair the tracer wrote at the very end of it. WGSL structs cannot
  // skip members, so everything the tracer owns in between is one pad this
  // pass never reads.
  //
  // FOCUS_PAD IS GENERATED (Renderer::wgslConstants, = kSlotFocus - 13). It
  // was hand-counted in the spike, and that is a silent failure waiting to
  // happen: a stale pad still FITS the buffer, so there is no validation
  // error — post simply reads the wrong 32 bytes and puts the defocus ellipse
  // somewhere the tracer never used.
  pad: array<vec4f, FOCUS_PAD>,
  focus: vec4f,     // xy = core centre px, z = full-res boundary radius px,
                    // w = ellipse x/y aspect
  focusMeta: vec4f, // x = ramp width inside that boundary px, y = coarse block
                    // N, z = defocus radius px (0 = no defocus)
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

  // Tilt-shift defocus. TWO jobs in one blur: it sells the miniature read, and
  // it dissolves the NxN blocks the foveated tracer left in the periphery —
  // which is why focus.blur must never be tuned below focus.coarse.
  //
  // The ramp runs across the SAME ellipse the tracer used and reaches full
  // strength exactly ON the full-res boundary, ramping up INSIDE it (which is
  // why the feather costs no rays). Both sides of that boundary therefore get
  // the same blur, and that is what stops a resolution CHANGE from reading as
  // an edge: there is no colour step there to begin with — a coarse block is
  // the traced colour of its own centre ray — only a sharpness one, and by the
  // time it happens the sharpness has already been given up.
  //
  // textureSampleLevel, not textureSample: this is varying control flow, so
  // implicit derivatives are not allowed here.
  let px = 1.0 / u.res.xy;
  if (u.focusMeta.z > 0.0) {
    let d = in.pos.xy - u.focus.xy;
    let q = length(vec2f(d.x / max(u.focus.w, 1e-3), d.y));
    let w = smoothstep(u.focus.z - max(u.focusMeta.x, 1e-3), u.focus.z, q);
    if (w > 0.002) {
      let r = w * u.focusMeta.z * px;
      var acc = col;
      for (var i = 0; i < 8; i++) {
        let a = f32(i) * 0.785398 + 0.3927; // offset off the bloom ring below
        let dir = vec2f(cos(a), sin(a));
        acc += textureSampleLevel(hdrTex, samp, uv + dir * r, 0.0).rgb;
        acc += textureSampleLevel(hdrTex, samp, uv + dir * r * 0.5, 0.0).rgb;
      }
      col = acc / 17.0;
    }
  }

  // mild bloom: 8-tap threshold blur in HDR
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
