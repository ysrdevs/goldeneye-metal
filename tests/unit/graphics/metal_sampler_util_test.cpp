#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/metal/msl_compiler.h>
#include <rex/graphics/metal/sampler_util.h>

namespace {

using rex::graphics::metal::ResolveProbeSamplerFiltering;
using rex::graphics::metal::GetProbeSamplerKey;
using rex::graphics::metal::ProbeSamplerFiltering;
using rex::graphics::metal::ProbeSamplerSlot;
using rex::graphics::xenos::AnisoFilter;
using rex::graphics::xenos::TextureFilter;

TEST_CASE("Metal anisotropic override maps eligible sampler strengths",
          "[graphics][metal][sampler]") {
  auto resolve = [](int32_t override_value) {
    return ResolveProbeSamplerFiltering(TextureFilter::kLinear, TextureFilter::kLinear,
                                        TextureFilter::kLinear, AnisoFilter::kDisabled,
                                        /*has_mips=*/true, override_value);
  };

  const auto disabled = resolve(0);
  CHECK(disabled.max_anisotropy == 1);
  CHECK(disabled.mag_linear == 1);
  CHECK(disabled.min_linear == 1);
  CHECK(disabled.mip_linear == 1);

  const auto game_default_disabled = resolve(-1);
  CHECK(game_default_disabled.max_anisotropy == 1);
  CHECK(game_default_disabled.mag_linear == 1);
  CHECK(game_default_disabled.min_linear == 1);
  CHECK(game_default_disabled.mip_linear == 1);
  CHECK(resolve(1).max_anisotropy == 1);
  CHECK(resolve(2).max_anisotropy == 2);
  CHECK(resolve(3).max_anisotropy == 4);
  CHECK(resolve(4).max_anisotropy == 8);
  CHECK(resolve(5).max_anisotropy == 16);
}

TEST_CASE("Metal anisotropic override preserves ineligible sampler behavior",
          "[graphics][metal][sampler]") {
  const auto no_mips =
      ResolveProbeSamplerFiltering(TextureFilter::kLinear, TextureFilter::kLinear,
                                   TextureFilter::kLinear, AnisoFilter::kDisabled,
                                   /*has_mips=*/false, /*anisotropic_override=*/5);
  CHECK(no_mips.max_anisotropy == 1);

  const auto base_map =
      ResolveProbeSamplerFiltering(TextureFilter::kLinear, TextureFilter::kLinear,
                                   TextureFilter::kBaseMap, AnisoFilter::kDisabled,
                                   /*has_mips=*/true, /*anisotropic_override=*/5);
  CHECK(base_map.max_anisotropy == 1);
  CHECK(base_map.mip_linear == 0);

  const auto point_sampled =
      ResolveProbeSamplerFiltering(TextureFilter::kPoint, TextureFilter::kPoint,
                                   TextureFilter::kLinear, AnisoFilter::kDisabled,
                                   /*has_mips=*/true, /*anisotropic_override=*/5);
  CHECK(point_sampled.max_anisotropy == 1);
  CHECK(point_sampled.mag_linear == 0);
  CHECK(point_sampled.min_linear == 0);
}

TEST_CASE("Metal native anisotropy and one-to-one override force linear filtering",
          "[graphics][metal][sampler]") {
  const auto native =
      ResolveProbeSamplerFiltering(TextureFilter::kPoint, TextureFilter::kPoint,
                                   TextureFilter::kPoint, AnisoFilter::kMax_8_1,
                                   /*has_mips=*/true, /*anisotropic_override=*/-1);
  CHECK(native.max_anisotropy == 8);
  CHECK(native.mag_linear == 1);
  CHECK(native.min_linear == 1);
  CHECK(native.mip_linear == 1);

  const auto one_to_one =
      ResolveProbeSamplerFiltering(TextureFilter::kLinear, TextureFilter::kLinear,
                                   TextureFilter::kPoint, AnisoFilter::kDisabled,
                                   /*has_mips=*/true, /*anisotropic_override=*/1);
  CHECK(one_to_one.max_anisotropy == 1);
  CHECK(one_to_one.mag_linear == 1);
  CHECK(one_to_one.min_linear == 1);
  CHECK(one_to_one.mip_linear == 1);
}

TEST_CASE("Metal live filtering changes produce distinct sampler cache keys",
          "[graphics][metal][sampler]") {
  auto make_slot = [](const ProbeSamplerFiltering& filtering) {
    ProbeSamplerSlot slot;
    slot.min_linear = filtering.min_linear;
    slot.mag_linear = filtering.mag_linear;
    slot.mip_linear = filtering.mip_linear;
    slot.max_anisotropy = filtering.max_anisotropy;
    return slot;
  };
  auto resolve = [](int32_t override_value) {
    return ResolveProbeSamplerFiltering(TextureFilter::kLinear, TextureFilter::kLinear,
                                        TextureFilter::kPoint, AnisoFilter::kDisabled,
                                        /*has_mips=*/true, override_value);
  };

  const uint64_t off_key = GetProbeSamplerKey(make_slot(resolve(0)));
  const uint64_t one_x_key = GetProbeSamplerKey(make_slot(resolve(1)));
  const uint64_t four_x_key = GetProbeSamplerKey(make_slot(resolve(3)));
  const uint64_t eight_x_key = GetProbeSamplerKey(make_slot(resolve(4)));
  const uint64_t sixteen_x_key = GetProbeSamplerKey(make_slot(resolve(5)));
  CHECK(off_key != one_x_key);
  CHECK(one_x_key != four_x_key);
  CHECK(four_x_key != eight_x_key);
  CHECK(eight_x_key != sixteen_x_key);
}

}  // namespace
