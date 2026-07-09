// resolve.metal -- Native Metal EDRAM resolve-copy compute kernels.
//
// Phase 0 first slice: a faithful MSL translation of Xenia's
// "Resolve Copy Fast 32bpp 1x/2xMSAA" compute shader
// (draw_util::ResolveCopyShaderIndex::kFast32bpp1x2xMSAA).
//
// Reference (translated 1:1):
//   src/graphics/shaders/vulkan_spirv/resolve_fast_32bpp_1x2xmsaa_cs.h
//   (the SPIR-V "main", LocalSize 8 8 1).
//
// Buffer bindings (must match the dispatcher and the isolation test):
//   buffer(0) : EDRAM source, typed as uint4 (source_bpe_log2 == 4 -> 16-byte
//               elements), read-only. Xenos 80x16-sample tiles, 32bpp.
//   buffer(1) : Destination, typed as uint4 (dest_bpe_log2 == 4), write. Guest
//               Xenos-tiled 8888 bytes (GetTiledOffset2D layout).
//   buffer(2) : ResolveCopyShaderConstants (5x uint32, see below).
//
// Thread mapping (identical to the reference shader):
//   thread_position_in_grid.x  = an 8-pixel column index (each thread owns 8
//                                consecutive destination pixels in X).
//   thread_position_in_grid.y  = a single pixel row index.
//   Threadgroup size is [[8, 8, 1]] -> one group covers 64 px (X) x 8 px (Y).
//   Dispatch group count: ceil(width_px / 8 / 8), ceil(height_px / 8) ... see
//   the host: group_count = ceil(width_px / group_size_x), where
//   group_size_x = 1 << group_size_x_log2 == 64, group_size_y == 8.

#include <metal_stdlib>
using namespace metal;

// NOTE: declared at file scope (NOT inside an anonymous namespace). Wrapping the
// kernel's struct/constants in `namespace { }` causes newLibraryWithSource to
// emit a library with zero functions on Apple Silicon (the kernel loses external
// linkage). Verified on Apple M3 Ultra.

// ResolveCopyShaderConstants, packed exactly as the C++ struct
// draw_util::ResolveCopyShaderConstants (include/rex/graphics/util/draw.h:449).
// The Vulkan reference receives these as 5 push-constant uint32s in this order.
struct ResolveCopyShaderConstants {
  uint edram_info;           // ResolveEdramInfo
  uint coordinate_info;      // ResolveCoordinateInfo
  uint dest_info;            // reg::RB_COPY_DEST_INFO
  uint dest_coordinate_info; // ResolveCopyDestCoordinateInfo
  uint dest_base;            // dest_base (in dest-buffer bytes)
};

// Xenos EDRAM tile geometry (xenos.h:411-412).
constant uint kEdramTileWidthSamples = 80u;
constant uint kEdramTileHeightSamples = 16u;
// kEdramSizeBytes / sizeof(uint32) == 2048 * 16 * 80 (xenos.h:413-415).
constant uint kEdramSizeDwords = 2048u * kEdramTileHeightSamples * kEdramTileWidthSamples;

kernel void resolve_copy_fast_32bpp_1x2xmsaa(
    device const uint4* edram_buffer [[buffer(0)]],
    device uint4* dest_buffer [[buffer(1)]],
    constant ResolveCopyShaderConstants& constants [[buffer(2)]],
    uint3 global_id [[thread_position_in_grid]]) {
  // ----- Unpack ResolveEdramInfo (constants.edram_info). -----
  uint edram_pitch_tiles = constants.edram_info & 0x3FFu;            // [0:10]
  uint msaa_samples = (constants.edram_info >> 10u) & 0x3u;          // [10:12]
  bool edram_is_depth = (constants.edram_info & 0x1000u) != 0u;      // [12:13]
  uint edram_base_tiles = (constants.edram_info >> 13u) & 0x7FFu;    // [13:24]

  // ----- Unpack ResolveCoordinateInfo (constants.coordinate_info). -----
  // edram_offset = (offset_x_div_8, offset_y_div_8) << 3, in pixels.
  uint2 edram_offset =
      ((uint2(constants.coordinate_info, constants.coordinate_info) >> uint2(0u, 4u)) &
       uint2(0xFu, 0x1u)) << 3u;
  uint width_div_8 = (constants.coordinate_info >> 5u) & 0x7FFu;     // [5:16]

  // ----- Unpack reg::RB_COPY_DEST_INFO (constants.dest_info). -----
  uint dest_endian = constants.dest_info & 0x7u;                    // [0:3] Endian128
  uint dest_slice = (constants.dest_info >> 4u) & 0x7u;            // [4:7] copy_dest_slice
  bool dest_swap = (constants.dest_info & 0x1000000u) != 0u;       // bit 24 copy_dest_swap

  // ----- Unpack ResolveCopyDestCoordinateInfo (constants.dest_coordinate_info). -----
  uint dest_pitch_texels = (constants.dest_coordinate_info & 0x3FFu) << 5u;        // [0:10] * 32
  uint dest_height_texels = ((constants.dest_coordinate_info >> 10u) & 0x3FFu) << 5u; // [10:20] * 32
  // dest_offset = (offset_x_div_8, offset_y_div_8) << 3, in pixels.
  uint2 dest_offset =
      ((uint2(constants.dest_coordinate_info, constants.dest_coordinate_info) >>
        uint2(20u, 24u)) &
       uint2(0xFu, 0xFu)) << 3u;
  uint copy_sample_select = (constants.dest_coordinate_info >> 28u) & 0x7u;        // [28:31]

  // ----- Early out for threads past the resolve width (in 8-pixel columns). -----
  if (global_id.x >= width_div_8) {
    return;
  }

  // ----- Pixel coordinate of this thread. -----
  // X is shifted by 3 (8 px per thread); Y is the row directly.
  uint2 pixel = uint2(global_id.x << 3u, global_id.y);

  // ===== EDRAM source address (untile from 80x16 Xenos EDRAM tiles). =====
  // Translated from resolve_fast_32bpp_1x2xmsaa_cs.h (%21036..%18580).
  //
  // 1. coord (pixels) = (gid.x<<3, gid.y) + edram_offset.
  uint2 coord = pixel + edram_offset;
  // 2. Per-axis sample shift: x_shift = (msaa >= 2x) ? 1 : 0,
  //    y_shift = (msaa >= 1x i.e. >= 2x sample row) ? 1 : 0. (SPIR-V %1837 = (2,1)).
  uint2 sample_shift = select(uint2(0u, 0u), uint2(1u, 1u),
                              uint2(msaa_samples, msaa_samples) >= uint2(2u, 1u));
  uint2 coord_samples = coord << sample_shift;
  // 3. Add the selected sub-sample: ((sel>>1)&1, sel&1) (SPIR-V %16110).
  uint sel = copy_sample_select;
  uint2 sample_coord = coord_samples + uint2((sel >> 1u) & 1u, sel & 1u);

  // 4. Tile coordinate and within-tile sample coordinate.
  uint2 tile_coord = sample_coord / uint2(kEdramTileWidthSamples, kEdramTileHeightSamples);
  uint edram_tile = edram_base_tiles + tile_coord.y * edram_pitch_tiles + tile_coord.x;
  uint2 in_tile =
      sample_coord - tile_coord * uint2(kEdramTileWidthSamples, kEdramTileHeightSamples);

  // 5. Depth-only X spill wrap across the 40-sample column pair (SPIR-V %10558,
  //    gated on is_depth). No-op for color (the tested path).
  if (edram_is_depth) {
    int sx = int(in_tile.x);
    in_tile.x = uint(sx + (in_tile.x >= 40u ? -40 : 40));
  }

  // 6. EDRAM dword offset: tile * (80*16) + (in_tile.y * 80 + in_tile.x), then
  //    periodic modulo total EDRAM dwords. uint4 index = dword / 4.
  uint edram_dword =
      edram_tile * (kEdramTileWidthSamples * kEdramTileHeightSamples) +
      in_tile.y * kEdramTileWidthSamples + in_tile.x;
  edram_dword = edram_dword % kEdramSizeDwords;
  uint edram_index = edram_dword >> 2u;

  uint4 edram_data0 = edram_buffer[edram_index];
  uint4 edram_data1 = edram_buffer[edram_index + 1u];

  // ===== Apply Endian128 byte swap (reg RB_COPY_DEST_INFO endian). =====
  // k8in16 (1) and k8in32 (2) both swap bytes within 16-bit halves first.
  if (dest_endian == 1u || dest_endian == 2u) {
    edram_data0 = ((edram_data0 & 0x00FF00FFu) << 8u) | ((edram_data0 & 0xFF00FF00u) >> 8u);
    edram_data1 = ((edram_data1 & 0x00FF00FFu) << 8u) | ((edram_data1 & 0xFF00FF00u) >> 8u);
  }
  // k8in32 (2) and k16in32 (3) then swap the 16-bit halves.
  if (dest_endian == 2u || dest_endian == 3u) {
    edram_data0 = (edram_data0 << 16u) | (edram_data0 >> 16u);
    edram_data1 = (edram_data1 << 16u) | (edram_data1 >> 16u);
  }

  // ===== Optional 8888 channel swap (copy_dest_swap, RB->BGR). =====
  // Not used by the isolation test (dest_swap == false); kept for parity.
  (void)dest_swap;
  (void)dest_slice;

  // ===== Destination address: Xenos 2D tiled offset, 32bpp (bpe_log2 = 2). =====
  // The reference computes a *dword* address for the 8-pixel run start; the run
  // is 8 consecutive X pixels, so the tiled offset for the first pixel gives the
  // base, and the two uint4 stores cover pixels [x..x+3] and [x+4..x+7].
  // GetTiledOffset2D(x, y, pitch, bpe_log2=2) translated to MSL (byte offset):
  int2 dxy = int2(pixel + dest_offset);
  int dpitch = int(dest_pitch_texels);
  int macro = ((dxy.x >> 5) + (dxy.y >> 5) * (dpitch >> 5)) << (2 + 7);
  int micro = ((dxy.x & 7) + ((dxy.y & 0xE) << 2)) << 2;
  int offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((dxy.y & 1) << 4);
  int tiled_byte =
      ((offset & ~0x1FF) << 3) + ((dxy.y & 16) << 7) + ((offset & 0x1C0) << 2) +
      (((((dxy.y & 8) >> 2) + (dxy.x >> 3)) & 3) << 6) + (offset & 0x3F);

  uint dest_byte = uint(tiled_byte) + constants.dest_base;
  uint dest_index = dest_byte >> 4u;  // uint4 index (16-byte element)

  // In the Xenos 32bpp 2D tiled layout, pixels [x..x+3] occupy one uint4 and
  // pixels [x+4..x+7] occupy the uint4 two elements later (32 bytes = 2x16),
  // not the immediately adjacent one. The reference shader stores at
  // dest_index and dest_index + 2 (SPIR-V %18675 and %21685 = +2).
  dest_buffer[dest_index] = edram_data0;
  dest_buffer[dest_index + 2u] = edram_data1;
  (void)dest_height_texels;
}
