#pragma once

// Runtime-compiled because the presenter is part of the generic UI runtime and
// doesn't otherwise require an offline Metal shader build step. Keep this in a
// separate header so the standalone pipeline probe compiles the exact shader
// source used by MetalPresenter.
namespace rex::ui::metal::shaders {

inline constexpr char kGuestPresentMetalSource[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
  float4 position [[position]];
  float2 uv [[user(locn0)]];
};

// Scalars only. This layout intentionally matches MetalPresentParameters on
// the C++ side without relying on vector padding rules.
struct PresentParameters {
  uint fxaa_quality;       // 0 = off, 1 = FXAA, 2 = FXAA Extreme
  uint postfx_enabled;
  uint output_filter;      // 0 = bilinear, 1 = bounded sharp reconstruction
  float source_inv_width;
  float source_inv_height;
  float brightness;
  float contrast;
  float saturation;
  float vibrance;
  float temperature;
  float gamma;
  float tint_r;
  float tint_g;
  float tint_b;
  float tint_strength;
};

vertex VertexOut guest_present_vs(uint vertex_id [[vertex_id]]) {
  constexpr float2 positions[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0),
  };
  VertexOut out;
  float2 position = positions[vertex_id];
  out.position = float4(position, 0.0, 1.0);
  out.uv = float2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);
  return out;
}

float select_guest_component(float4 color, uint selector) {
  switch (selector) {
    case 0: return color.r;
    case 1: return color.g;
    case 2: return color.b;
    case 3: return color.a;
    case 5: return 1.0;
    default: return 0.0;
  }
}

float4 sample_guest_raw(texture2d<float> guest_texture, sampler guest_sampler,
                        float2 uv, uint guest_swizzle) {
  float4 color = guest_texture.sample(guest_sampler, uv);
  return float4(select_guest_component(color, (guest_swizzle >> 0) & 7),
                select_guest_component(color, (guest_swizzle >> 3) & 7),
                select_guest_component(color, (guest_swizzle >> 6) & 7),
                select_guest_component(color, (guest_swizzle >> 9) & 7));
}

// A bounded five-tap unsharp reconstruction keeps HUD text and geometry edges
// crisp after scaling. Clamping to the sampled neighbourhood prevents halos
// from escaping the source's local colour range.
float4 sample_guest_sharp(texture2d<float> guest_texture, sampler guest_sampler,
                          float2 uv, uint guest_swizzle,
                          constant PresentParameters& parameters) {
  float2 texel = float2(parameters.source_inv_width,
                        parameters.source_inv_height);
  float4 center = sample_guest_raw(guest_texture, guest_sampler, uv,
                                   guest_swizzle);
  float3 north = sample_guest_raw(guest_texture, guest_sampler,
                                  uv - float2(0.0, texel.y),
                                  guest_swizzle).rgb;
  float3 south = sample_guest_raw(guest_texture, guest_sampler,
                                  uv + float2(0.0, texel.y),
                                  guest_swizzle).rgb;
  float3 west = sample_guest_raw(guest_texture, guest_sampler,
                                 uv - float2(texel.x, 0.0),
                                 guest_swizzle).rgb;
  float3 east = sample_guest_raw(guest_texture, guest_sampler,
                                 uv + float2(texel.x, 0.0),
                                 guest_swizzle).rgb;
  float3 neighbourhood_min =
      min(center.rgb, min(min(north, south), min(west, east)));
  float3 neighbourhood_max =
      max(center.rgb, max(max(north, south), max(west, east)));
  float3 cross_average = (north + south + west + east) * 0.25;
  float3 sharpened = center.rgb + (center.rgb - cross_average) * 0.22;
  return float4(clamp(sharpened, neighbourhood_min, neighbourhood_max),
                center.a);
}

float4 sample_guest(texture2d<float> guest_texture, sampler guest_sampler,
                    float2 uv, uint guest_swizzle,
                    constant PresentParameters& parameters) {
  return parameters.output_filter == 1
             ? sample_guest_sharp(guest_texture, guest_sampler, uv,
                                  guest_swizzle, parameters)
             : sample_guest_raw(guest_texture, guest_sampler, uv,
                                guest_swizzle);
}

float guest_luma(float3 color) {
  return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// A compact FXAA quality pass derived from the public FXAA 3.x edge search.
// It runs in source-image texel space, before drawable scaling and grading.
// Extreme lowers the edge threshold and permits a longer edge search.
float4 apply_fxaa(texture2d<float> guest_texture, sampler guest_sampler,
                  float2 uv, uint guest_swizzle,
                  constant PresentParameters& parameters) {
  float4 center = sample_guest(guest_texture, guest_sampler, uv, guest_swizzle,
                               parameters);
  if (parameters.fxaa_quality == 0) {
    return center;
  }

  float2 texel = float2(parameters.source_inv_width,
                        parameters.source_inv_height);
  float3 rgb_nw = sample_guest(guest_texture, guest_sampler,
                               uv + texel * float2(-1.0, -1.0),
                               guest_swizzle, parameters).rgb;
  float3 rgb_ne = sample_guest(guest_texture, guest_sampler,
                               uv + texel * float2( 1.0, -1.0),
                               guest_swizzle, parameters).rgb;
  float3 rgb_sw = sample_guest(guest_texture, guest_sampler,
                               uv + texel * float2(-1.0,  1.0),
                               guest_swizzle, parameters).rgb;
  float3 rgb_se = sample_guest(guest_texture, guest_sampler,
                               uv + texel * float2( 1.0,  1.0),
                               guest_swizzle, parameters).rgb;

  float luma_nw = guest_luma(rgb_nw);
  float luma_ne = guest_luma(rgb_ne);
  float luma_sw = guest_luma(rgb_sw);
  float luma_se = guest_luma(rgb_se);
  float luma_m = guest_luma(center.rgb);
  float luma_min = min(luma_m, min(min(luma_nw, luma_ne), min(luma_sw, luma_se)));
  float luma_max = max(luma_m, max(max(luma_nw, luma_ne), max(luma_sw, luma_se)));
  float luma_range = luma_max - luma_min;
  // Match the existing D3D12 / Vulkan quality presets exactly.
  float edge_threshold = parameters.fxaa_quality >= 2 ? 0.063 : 0.166;
  float edge_threshold_min = parameters.fxaa_quality >= 2 ? 0.0312 : 0.0833;
  if (luma_range < max(edge_threshold_min, luma_max * edge_threshold)) {
    return center;
  }

  float2 direction;
  direction.x = -((luma_nw + luma_ne) - (luma_sw + luma_se));
  direction.y =  ((luma_nw + luma_sw) - (luma_ne + luma_se));
  float direction_reduce =
      max((luma_nw + luma_ne + luma_sw + luma_se) * (0.25 * 0.125),
          1.0 / 128.0);
  float inverse_direction_min =
      1.0 / (min(abs(direction.x), abs(direction.y)) + direction_reduce);
  float span_max = parameters.fxaa_quality >= 2 ? 12.0 : 8.0;
  direction = clamp(direction * inverse_direction_min,
                    float2(-span_max), float2(span_max)) * texel;

  float3 rgb_a = 0.5 * (
      sample_guest(guest_texture, guest_sampler,
                   uv + direction * (1.0 / 3.0 - 0.5), guest_swizzle,
                   parameters).rgb +
      sample_guest(guest_texture, guest_sampler,
                   uv + direction * (2.0 / 3.0 - 0.5), guest_swizzle,
                   parameters).rgb);
  float3 rgb_b = rgb_a * 0.5 + 0.25 * (
      sample_guest(guest_texture, guest_sampler,
                   uv + direction * -0.5, guest_swizzle, parameters).rgb +
      sample_guest(guest_texture, guest_sampler,
                   uv + direction *  0.5, guest_swizzle, parameters).rgb);
  float luma_b = guest_luma(rgb_b);
  float3 filtered = (luma_b < luma_min || luma_b > luma_max) ? rgb_a : rgb_b;
  return float4(filtered, center.a);
}

float3 apply_color_grade(float3 color,
                         constant PresentParameters& parameters) {
  float3 graded = color;

  // Temperature: warm raises red and lowers blue; cool does the reverse.
  graded.r += parameters.temperature * 0.10;
  graded.b -= parameters.temperature * 0.10;

  graded += parameters.brightness;
  graded = (graded - 0.5) * parameters.contrast + 0.5;

  float3 tint = float3(parameters.tint_r, parameters.tint_g,
                       parameters.tint_b);
  graded = mix(graded, graded * tint, parameters.tint_strength);

  float luma = dot(max(graded, 0.0), float3(0.2126, 0.7152, 0.0722));
  graded = mix(float3(luma), graded, parameters.saturation);

  float maximum = max(graded.r, max(graded.g, graded.b));
  float minimum = min(graded.r, min(graded.g, graded.b));
  float chroma = maximum - minimum;
  graded = mix(float3(luma), graded,
               1.0 + parameters.vibrance * (1.0 - saturate(chroma)));

  graded = pow(max(graded, 0.0001), 1.0 / parameters.gamma);
  return saturate(graded);
}

fragment float4 guest_present_ps(
    VertexOut in [[stage_in]],
    texture2d<float> guest_texture [[texture(0)]],
    constant uint& guest_swizzle [[buffer(0)]],
    constant PresentParameters& parameters [[buffer(1)]]) {
  constexpr sampler guest_sampler(coord::normalized, address::clamp_to_edge,
                                  filter::linear);
  float4 color = apply_fxaa(guest_texture, guest_sampler, in.uv,
                            guest_swizzle, parameters);
  if (parameters.postfx_enabled != 0) {
    color.rgb = apply_color_grade(color.rgb, parameters);
  }
  return color;
}
)MSL";

}  // namespace rex::ui::metal::shaders
