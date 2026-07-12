#include <rex/graphics/metal/texture_decode_validation.h>

#include <cstdio>
#include <limits>

namespace {
using rex::graphics::metal::TextureDecodeMemoryLayout;
using rex::graphics::metal::ValidateTextureDecodeMemoryLayout;

constexpr uint64_t kPhysicalMemorySize = UINT64_C(0x20000000);

constexpr bool ValidLinearFramebuffer() {
  TextureDecodeMemoryLayout layout;
  return ValidateTextureDecodeMemoryLayout(UINT32_C(0x1EC30000), 1280, 720, 1280, 4, false, 0,
                                           kPhysicalMemorySize, layout) &&
         layout.source_row_pitch_bytes == 5120 && layout.source_span_bytes == 3686400 &&
         layout.output_size_bytes == 3686400;
}

constexpr bool ValidExactEndTiledSpan() {
  TextureDecodeMemoryLayout layout;
  return ValidateTextureDecodeMemoryLayout(UINT32_C(0x10000000), 8192, 8192, 8192, 4, true,
                                           UINT64_C(0x10000000), kPhysicalMemorySize, layout) &&
         layout.source_span_bytes == UINT64_C(0x10000000);
}

constexpr bool RejectDamBrokenBloomSpan() {
  TextureDecodeMemoryLayout layout;
  return !ValidateTextureDecodeMemoryLayout(UINT32_C(0x1EBFE000), 8192, 8191, 8192, 4, true,
                                            UINT64_C(0x10000000), kPhysicalMemorySize, layout);
}

constexpr bool RejectLinearLastRowOverrun() {
  TextureDecodeMemoryLayout layout;
  return !ValidateTextureDecodeMemoryLayout(UINT32_C(0x1FFFF000), 1024, 2, 1024, 4, false, 0,
                                            kPhysicalMemorySize, layout);
}

constexpr bool RejectArithmeticOverflow() {
  TextureDecodeMemoryLayout layout;
  return !ValidateTextureDecodeMemoryLayout(
      0, std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max(),
      std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max(), false, 0,
      kPhysicalMemorySize, layout);
}

static_assert(ValidLinearFramebuffer());
static_assert(ValidExactEndTiledSpan());
static_assert(RejectDamBrokenBloomSpan());
static_assert(RejectLinearLastRowOverrun());
static_assert(RejectArithmeticOverflow());
}  // namespace

int main() {
  if (!ValidLinearFramebuffer() || !ValidExactEndTiledSpan() || !RejectDamBrokenBloomSpan() ||
      !RejectLinearLastRowOverrun() || !RejectArithmeticOverflow()) {
    std::fprintf(stderr, "metal_texture_decode_validation_test: FAIL\n");
    return 1;
  }
  std::fprintf(stderr, "metal_texture_decode_validation_test: PASS\n");
  return 0;
}
