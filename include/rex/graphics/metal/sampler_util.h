#pragma once

#include <cstdint>

#include <rex/graphics/xenos.h>

namespace rex::graphics::metal {

// Metal sampler state derived from an already-resolved Xenos sampler binding.
// Keeping this conversion independent of Metal objects makes the eligibility
// rules shared by every draw path easy to test.
struct ProbeSamplerFiltering {
  uint8_t min_linear = 0;
  uint8_t mag_linear = 0;
  uint8_t mip_linear = 0;
  uint8_t max_anisotropy = 1;
};

constexpr uint8_t ToProbeSamplerAnisotropy(xenos::AnisoFilter aniso_filter) {
  switch (aniso_filter) {
    case xenos::AnisoFilter::kMax_2_1:
      return 2;
    case xenos::AnisoFilter::kMax_4_1:
      return 4;
    case xenos::AnisoFilter::kMax_8_1:
      return 8;
    case xenos::AnisoFilter::kMax_16_1:
      return 16;
    case xenos::AnisoFilter::kMax_1_1:
    case xenos::AnisoFilter::kDisabled:
    case xenos::AnisoFilter::kUseFetchConst:
    default:
      return 1;
  }
}

constexpr ProbeSamplerFiltering ResolveProbeSamplerFiltering(
    xenos::TextureFilter mag_filter, xenos::TextureFilter min_filter,
    xenos::TextureFilter mip_filter, xenos::AnisoFilter aniso_filter, bool has_mips,
    int32_t anisotropic_override) {
  ProbeSamplerFiltering filtering;
  filtering.mag_linear = mag_filter == xenos::TextureFilter::kLinear;
  filtering.min_linear = min_filter == xenos::TextureFilter::kLinear;
  filtering.mip_linear = mip_filter == xenos::TextureFilter::kLinear;

  const bool min_mag_linear = filtering.mag_linear && filtering.min_linear;
  const bool mip_filter_bilinear_or_trilinear =
      mip_filter == xenos::TextureFilter::kPoint || mip_filter == xenos::TextureFilter::kLinear;
  const bool mip_base_map = mip_filter == xenos::TextureFilter::kBaseMap;
  if (anisotropic_override > -1 && anisotropic_override < 6 && has_mips && !mip_base_map &&
      min_mag_linear && mip_filter_bilinear_or_trilinear) {
    aniso_filter = xenos::AnisoFilter(anisotropic_override);
  }
  if (uint32_t(aniso_filter) > uint32_t(xenos::AnisoFilter::kMax_16_1)) {
    aniso_filter = xenos::AnisoFilter::kMax_16_1;
  }

  filtering.max_anisotropy = ToProbeSamplerAnisotropy(aniso_filter);
  if (aniso_filter != xenos::AnisoFilter::kDisabled) {
    // Match D3D12 and Vulkan: anisotropic filtering forces linear
    // minification, magnification and mip filtering. kMax_1_1 intentionally
    // keeps maxAnisotropy at 1 while retaining those filtering semantics.
    filtering.mag_linear = 1;
    filtering.min_linear = 1;
    filtering.mip_linear = 1;
  }
  return filtering;
}

}  // namespace rex::graphics::metal
