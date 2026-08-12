// presentTex -> swapchain passthrough (post already wrote display-ready sRGB).

@group(0) @binding(0) var srcTex: texture_2d<f32>;
@group(0) @binding(1) var samp: sampler;

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

@fragment
fn fs(in: VOut) -> @location(0) vec4f {
  return vec4f(textureSample(srcTex, samp, in.uv).rgb, 1.0);
}
