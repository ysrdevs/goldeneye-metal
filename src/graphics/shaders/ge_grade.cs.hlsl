// ge - colour-grade post-process compute shader.
//
// Runs in-place over the final guest-output image (R10G10B10A2_UNORM) right
// after gamma/FXAA in the D3D12 swap. Applies a per-pixel grade chain:
// temperature -> brightness -> contrast -> tint -> saturation -> vibrance ->
// gamma. All neutral defaults = identity (no visible change).
//
// Scalars only in the cbuffer to keep a tight, straddle-free 32-bit-constant
// layout (12 dwords) matching GradeConstants on the C++ side.

cbuffer push_consts : register(b0) {
  uint2 xe_grade_size;      // 0
  float xe_brightness;      // 8   (-1..1, added)
  float xe_contrast;        // 12  (0..2, 1=none)
  float xe_saturation;      // 16  (0..2, 1=none)
  float xe_vibrance;        // 20  (-1..1)
  float xe_temperature;     // 24  (-1..1, warm/cool)
  float xe_gamma;           // 28  (0.3..3, 1=none)
  float xe_tint_r;          // 32
  float xe_tint_g;          // 36
  float xe_tint_b;          // 40
  float xe_tint_strength;   // 44  (0..1)
};

RWTexture2D<float4> xe_dest : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 xe_thread_id : SV_DispatchThreadID) {
  [branch] if (xe_thread_id.x >= xe_grade_size.x || xe_thread_id.y >= xe_grade_size.y) {
    return;
  }
  int2 coord = int2(xe_thread_id.xy);
  float3 c = xe_dest[coord].rgb;

  // Temperature: push warm (R up / B down) or cool (opposite).
  c.r += xe_temperature * 0.10;
  c.b -= xe_temperature * 0.10;

  // Brightness (additive) then contrast about mid-grey.
  c += xe_brightness;
  c = (c - 0.5) * xe_contrast + 0.5;

  // Tint: multiply toward the tint colour by strength.
  float3 tint = float3(xe_tint_r, xe_tint_g, xe_tint_b);
  c = lerp(c, c * tint, xe_tint_strength);

  // Saturation about luma.
  float luma = dot(max(c, 0.0), float3(0.2126, 0.7152, 0.0722));
  c = lerp(float3(luma, luma, luma), c, xe_saturation);

  // Vibrance: boost less-saturated pixels more.
  float mx = max(c.r, max(c.g, c.b));
  float mn = min(c.r, min(c.g, c.b));
  float sat = mx - mn;
  c = lerp(float3(luma, luma, luma), c, 1.0 + xe_vibrance * (1.0 - saturate(sat)));

  // Gamma.
  c = pow(max(c, 0.0001), 1.0 / xe_gamma);

  xe_dest[coord] = float4(saturate(c), 1.0);
}
