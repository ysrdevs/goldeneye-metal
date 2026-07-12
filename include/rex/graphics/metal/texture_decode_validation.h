#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace rex::graphics::metal {

// Fully checked byte layout consumed by the Metal diagnostic/probe texture
// decoder. source_span_bytes is the exclusive source extent from base; output
// sizes describe the tightly packed RGBA destination.
struct TextureDecodeMemoryLayout {
  uint64_t source_row_pitch_bytes = 0;
  uint64_t source_span_bytes = 0;
  size_t output_row_pitch_bytes = 0;
  size_t output_size_bytes = 0;
};

constexpr bool CheckedTextureDecodeMultiply(uint64_t left, uint64_t right, uint64_t& result) {
  if (left && right > std::numeric_limits<uint64_t>::max() / left) {
    return false;
  }
  result = left * right;
  return true;
}

constexpr bool CheckedTextureDecodeAdd(uint64_t left, uint64_t right, uint64_t& result) {
  if (right > std::numeric_limits<uint64_t>::max() - left) {
    return false;
  }
  result = left + right;
  return true;
}

// Validates all spans before a guest pointer is translated or a destination
// is allocated. tiled_source_span_bytes must be the exclusive upper bound from
// the Xenos tiled-address calculation; it is ignored for linear textures.
constexpr bool ValidateTextureDecodeMemoryLayout(uint32_t base_physical, uint32_t width,
                                                 uint32_t height, uint32_t pitch_texels,
                                                 uint32_t bytes_per_block, bool tiled,
                                                 uint64_t tiled_source_span_bytes,
                                                 uint64_t physical_memory_size,
                                                 TextureDecodeMemoryLayout& layout_out) {
  layout_out = {};
  if (!width || !height || !pitch_texels || !bytes_per_block || pitch_texels < width ||
      uint64_t(base_physical) >= physical_memory_size) {
    return false;
  }

  uint64_t output_row_pitch = 0;
  uint64_t output_size = 0;
  uint64_t source_row_pitch = 0;
  if (!CheckedTextureDecodeMultiply(width, bytes_per_block, output_row_pitch) ||
      !CheckedTextureDecodeMultiply(output_row_pitch, height, output_size) ||
      !CheckedTextureDecodeMultiply(pitch_texels, bytes_per_block, source_row_pitch) ||
      output_row_pitch > source_row_pitch ||
      source_row_pitch > std::numeric_limits<size_t>::max() ||
      output_size > std::numeric_limits<size_t>::max()) {
    return false;
  }

  uint64_t source_span = tiled_source_span_bytes;
  if (!tiled) {
    uint64_t preceding_rows_size = 0;
    if (!CheckedTextureDecodeMultiply(uint64_t(height - 1), source_row_pitch,
                                      preceding_rows_size) ||
        !CheckedTextureDecodeAdd(preceding_rows_size, output_row_pitch, source_span)) {
      return false;
    }
  }
  if (!source_span || source_span > physical_memory_size - uint64_t(base_physical) ||
      source_span > std::numeric_limits<size_t>::max()) {
    return false;
  }

  layout_out.source_row_pitch_bytes = source_row_pitch;
  layout_out.source_span_bytes = source_span;
  layout_out.output_row_pitch_bytes = size_t(output_row_pitch);
  layout_out.output_size_bytes = size_t(output_size);
  return true;
}

}  // namespace rex::graphics::metal
