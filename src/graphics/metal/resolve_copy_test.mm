// resolve_copy_test.mm -- Phase 0 first-slice ISOLATION TEST.
//
// Proves the native Metal EDRAM resolve-copy compute path
// (ResolveCopyShaderIndex::kFast32bpp1x2xMSAA) in isolation: no game, no draw
// path. It:
//   1. Creates a default MTLDevice + command queue.
//   2. Allocates an EDRAM MTLBuffer (xenos::kEdramSizeBytes, StorageModeShared
//      so the CPU can stage the input pattern) and a dest MTLBuffer (Shared so
//      the CPU can read results back).
//   3. Hand-packs a known 8888 pattern into EDRAM 80x16 tiles for a chosen
//      resolve region.
//   4. Compiles + dispatches resolve_copy_fast_32bpp_1x2xmsaa (the MSL kernel,
//      embedded below; byte-identical to src/graphics/metal/shaders/resolve.metal).
//   5. Reads back the dest buffer and ASSERTS it equals a CPU reference
//      implementation of the SAME untile/copy.
//
// Gate: kernel output == CPU reference  ->  exit 0 (PASS); otherwise exit 1.
//
// References:
//   src/graphics/shaders/vulkan_spirv/resolve_fast_32bpp_1x2xmsaa_cs.h (SPIR-V)
//   src/graphics/pipeline/texture/util.cpp:424 GetTiledOffset2D
//   include/rex/graphics/util/draw.h:449 ResolveCopyShaderConstants
//   include/rex/graphics/xenos.h:411-415 EDRAM tile geometry

#import <Metal/Metal.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <rex/graphics/xenos.h>

namespace {

// The EDRAM constants below live in rex::graphics::xenos.
using namespace rex::graphics;

// ---------------------------------------------------------------------------
// EDRAM geometry (xenos.h:411-415).
// ---------------------------------------------------------------------------
constexpr uint32_t kEdramTileWidthSamples = xenos::kEdramTileWidthSamples;               // 80
constexpr uint32_t kEdramTileHeightSamples = xenos::kEdramTileHeightSamples;             // 16
constexpr uint32_t kEdramTileDwords = kEdramTileWidthSamples * kEdramTileHeightSamples;  // 1280
constexpr uint32_t kEdramSizeBytes = xenos::kEdramSizeBytes;                             // 10 MiB
constexpr uint32_t kEdramSizeDwords = kEdramSizeBytes / 4u;                              // 2621440

// kFast32bpp1x2xMSAA threadgroup sizing (draw.cpp:754 -> group_size_x_log2 = 6,
// group_size_y_log2 = 3). The MSL kernel uses an 8x8 *threadgroup* where each
// thread.x owns 8 pixels -> 64 px (X) x 8 px (Y) per group, matching 6/3.
constexpr uint32_t kThreadgroupX = 8;
constexpr uint32_t kThreadgroupY = 8;
constexpr uint32_t kPixelsPerThreadX = 8;  // each thread.x handles 8 px in X
constexpr uint32_t kGroupPixelsX = kThreadgroupX * kPixelsPerThreadX;  // 64
constexpr uint32_t kGroupPixelsY = kThreadgroupY;                      // 8

// ---------------------------------------------------------------------------
// Packed constants, byte-identical to draw_util::ResolveCopyShaderConstants and
// to the MSL `ResolveCopyShaderConstants` (5 x uint32).
// ---------------------------------------------------------------------------
struct ResolveCopyShaderConstants {
  uint32_t edram_info;
  uint32_t coordinate_info;
  uint32_t dest_info;
  uint32_t dest_coordinate_info;
  uint32_t dest_base;
};
static_assert(sizeof(ResolveCopyShaderConstants) == 20, "constants must be 5 x uint32");

// ---------------------------------------------------------------------------
// Bit packers (mirror the union layouts in draw.h).
// ---------------------------------------------------------------------------
uint32_t PackEdramInfo(uint32_t pitch_tiles, uint32_t msaa_samples, uint32_t is_depth,
                       uint32_t base_tiles, uint32_t format, uint32_t format_is_64bpp,
                       uint32_t fill_half_pixel) {
  return (pitch_tiles & 0x3FFu) | ((msaa_samples & 0x3u) << 10u) | ((is_depth & 0x1u) << 12u) |
         ((base_tiles & 0x7FFu) << 13u) | ((format & 0xFu) << 24u) |
         ((format_is_64bpp & 0x1u) << 28u) | ((fill_half_pixel & 0x1u) << 29u);
}

uint32_t PackCoordinateInfo(uint32_t off_x_div_8, uint32_t off_y_div_8, uint32_t width_div_8,
                            uint32_t scale_x, uint32_t scale_y) {
  return (off_x_div_8 & 0xFu) | ((off_y_div_8 & 0x1u) << 4u) | ((width_div_8 & 0x7FFu) << 5u) |
         ((scale_x & 0x7u) << 16u) | ((scale_y & 0x7u) << 19u);
}

uint32_t PackDestCoordinateInfo(uint32_t pitch_div_32, uint32_t height_div_32, uint32_t off_x_div_8,
                                uint32_t off_y_div_8, uint32_t copy_sample_select) {
  return (pitch_div_32 & 0x3FFu) | ((height_div_32 & 0x3FFu) << 10u) |
         ((off_x_div_8 & 0xFu) << 20u) | ((off_y_div_8 & 0xFu) << 24u) |
         ((copy_sample_select & 0x7u) << 28u);
}

// ---------------------------------------------------------------------------
// CPU reference: the SAME untile/copy the kernel performs, written
// independently from the packed constants. Operates per 8-pixel run (matching
// the kernel's per-thread granularity) but the result is address-equivalent to
// a per-pixel implementation.
// ---------------------------------------------------------------------------

// EDRAM dword index for sample (sx, sy) given tile geometry. Mirrors the
// SPIR-V %8742 / %9094 / %18580 math (color path: is_depth == 0).
uint32_t EdramDwordIndex(uint32_t pitch_tiles, uint32_t base_tiles, uint32_t msaa_samples,
                         uint32_t coord_x_px, uint32_t coord_y_px, uint32_t copy_sample_select) {
  uint32_t x_shift = (msaa_samples >= 2u) ? 1u : 0u;
  uint32_t y_shift = (msaa_samples >= 1u) ? 1u : 0u;
  uint32_t sx = (coord_x_px << x_shift) + ((copy_sample_select >> 1u) & 1u);
  uint32_t sy = (coord_y_px << y_shift) + (copy_sample_select & 1u);
  uint32_t tile_x = sx / kEdramTileWidthSamples;
  uint32_t tile_y = sy / kEdramTileHeightSamples;
  uint32_t in_x = sx - tile_x * kEdramTileWidthSamples;
  uint32_t in_y = sy - tile_y * kEdramTileHeightSamples;
  uint32_t tile = base_tiles + tile_y * pitch_tiles + tile_x;
  uint32_t dword = tile * kEdramTileDwords + in_y * kEdramTileWidthSamples + in_x;
  return dword % kEdramSizeDwords;
}

// Xenos 2D tiled byte offset, 32bpp (bytes_per_block_log2 == 2).
// Identical to texture_util::GetTiledOffset2D (util.cpp:424) and the MSL.
int32_t GetTiledOffset2D32bpp(int32_t x, int32_t y, uint32_t pitch_texels) {
  // pitch already aligned to 32 by the caller (pitch_div_32 << 5).
  int32_t pitch = int32_t(pitch_texels);
  int32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (2 + 7);
  int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << 2;
  int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

uint32_t ApplyEndian(uint32_t v, uint32_t endian) {
  if (endian == 1u || endian == 2u) {
    v = ((v & 0x00FF00FFu) << 8u) | ((v & 0xFF00FF00u) >> 8u);
  }
  if (endian == 2u || endian == 3u) {
    v = (v << 16u) | (v >> 16u);
  }
  return v;
}

void CpuReference(const uint32_t* edram, const ResolveCopyShaderConstants& c, uint32_t width_px,
                  uint32_t height_px, std::vector<uint8_t>& dest_out) {
  uint32_t pitch_tiles = c.edram_info & 0x3FFu;
  uint32_t msaa_samples = (c.edram_info >> 10u) & 0x3u;
  uint32_t base_tiles = (c.edram_info >> 13u) & 0x7FFu;

  uint32_t edram_off_x = ((c.coordinate_info >> 0u) & 0xFu) << 3u;
  uint32_t edram_off_y = ((c.coordinate_info >> 4u) & 0x1u) << 3u;

  uint32_t endian = c.dest_info & 0x7u;

  uint32_t dest_pitch_texels = (c.dest_coordinate_info & 0x3FFu) << 5u;
  uint32_t dest_off_x = ((c.dest_coordinate_info >> 20u) & 0xFu) << 3u;
  uint32_t dest_off_y = ((c.dest_coordinate_info >> 24u) & 0xFu) << 3u;
  uint32_t copy_sample_select = (c.dest_coordinate_info >> 28u) & 0x7u;

  for (uint32_t y = 0; y < height_px; ++y) {
    for (uint32_t x = 0; x < width_px; ++x) {
      // EDRAM source dword.
      uint32_t edram_index = EdramDwordIndex(pitch_tiles, base_tiles, msaa_samples, x + edram_off_x,
                                             y + edram_off_y, copy_sample_select);
      uint32_t sample = edram[edram_index];
      sample = ApplyEndian(sample, endian);

      // Dest tiled byte offset.
      int32_t tiled = GetTiledOffset2D32bpp(int32_t(x + dest_off_x), int32_t(y + dest_off_y),
                                            dest_pitch_texels);
      uint32_t dest_byte = uint32_t(tiled) + c.dest_base;
      if (dest_byte + 4u <= dest_out.size()) {
        std::memcpy(dest_out.data() + dest_byte, &sample, sizeof(sample));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// MSL kernel source (byte-identical to src/graphics/metal/shaders/resolve.metal).
// Embedded so the test is self-contained with no runtime file dependency.
// ---------------------------------------------------------------------------
const char* kResolveMsl = R"MSL(
#include <metal_stdlib>
using namespace metal;

// File scope (no anonymous namespace): wrapping the kernel in `namespace { }`
// yields a library with zero functions on Apple Silicon.
struct ResolveCopyShaderConstants {
  uint edram_info;
  uint coordinate_info;
  uint dest_info;
  uint dest_coordinate_info;
  uint dest_base;
};
constant uint kEdramTileWidthSamples = 80u;
constant uint kEdramTileHeightSamples = 16u;
constant uint kEdramSizeDwords = 2048u * kEdramTileHeightSamples * kEdramTileWidthSamples;

kernel void resolve_copy_fast_32bpp_1x2xmsaa(
    device const uint4* edram_buffer [[buffer(0)]],
    device uint4* dest_buffer [[buffer(1)]],
    constant ResolveCopyShaderConstants& constants [[buffer(2)]],
    uint3 global_id [[thread_position_in_grid]]) {
  uint edram_pitch_tiles = constants.edram_info & 0x3FFu;
  uint msaa_samples = (constants.edram_info >> 10u) & 0x3u;
  bool edram_is_depth = (constants.edram_info & 0x1000u) != 0u;
  uint edram_base_tiles = (constants.edram_info >> 13u) & 0x7FFu;

  uint2 edram_offset =
      ((uint2(constants.coordinate_info, constants.coordinate_info) >> uint2(0u, 4u)) &
       uint2(0xFu, 0x1u)) << 3u;
  uint width_div_8 = (constants.coordinate_info >> 5u) & 0x7FFu;

  uint dest_endian = constants.dest_info & 0x7u;

  uint dest_pitch_texels = (constants.dest_coordinate_info & 0x3FFu) << 5u;
  uint2 dest_offset =
      ((uint2(constants.dest_coordinate_info, constants.dest_coordinate_info) >>
        uint2(20u, 24u)) &
       uint2(0xFu, 0xFu)) << 3u;
  uint copy_sample_select = (constants.dest_coordinate_info >> 28u) & 0x7u;

  if (global_id.x >= width_div_8) {
    return;
  }

  uint2 pixel = uint2(global_id.x << 3u, global_id.y);

  uint2 coord = pixel + edram_offset;
  uint2 sample_shift = select(uint2(0u, 0u), uint2(1u, 1u),
                              uint2(msaa_samples, msaa_samples) >= uint2(2u, 1u));
  uint2 coord_samples = coord << sample_shift;
  uint sel = copy_sample_select;
  uint2 sample_coord = coord_samples + uint2((sel >> 1u) & 1u, sel & 1u);

  uint2 tile_coord = sample_coord / uint2(kEdramTileWidthSamples, kEdramTileHeightSamples);
  uint edram_tile = edram_base_tiles + tile_coord.y * edram_pitch_tiles + tile_coord.x;
  uint2 in_tile =
      sample_coord - tile_coord * uint2(kEdramTileWidthSamples, kEdramTileHeightSamples);

  if (edram_is_depth) {
    int sx = int(in_tile.x);
    in_tile.x = uint(sx + (in_tile.x >= 40u ? -40 : 40));
  }

  uint edram_dword =
      edram_tile * (kEdramTileWidthSamples * kEdramTileHeightSamples) +
      in_tile.y * kEdramTileWidthSamples + in_tile.x;
  edram_dword = edram_dword % kEdramSizeDwords;
  uint edram_index = edram_dword >> 2u;

  uint4 edram_data0 = edram_buffer[edram_index];
  uint4 edram_data1 = edram_buffer[edram_index + 1u];

  if (dest_endian == 1u || dest_endian == 2u) {
    edram_data0 = ((edram_data0 & 0x00FF00FFu) << 8u) | ((edram_data0 & 0xFF00FF00u) >> 8u);
    edram_data1 = ((edram_data1 & 0x00FF00FFu) << 8u) | ((edram_data1 & 0xFF00FF00u) >> 8u);
  }
  if (dest_endian == 2u || dest_endian == 3u) {
    edram_data0 = (edram_data0 << 16u) | (edram_data0 >> 16u);
    edram_data1 = (edram_data1 << 16u) | (edram_data1 >> 16u);
  }

  int2 dxy = int2(pixel + dest_offset);
  int dpitch = int(dest_pitch_texels);
  int macro = ((dxy.x >> 5) + (dxy.y >> 5) * (dpitch >> 5)) << (2 + 7);
  int micro = ((dxy.x & 7) + ((dxy.y & 0xE) << 2)) << 2;
  int offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((dxy.y & 1) << 4);
  int tiled_byte =
      ((offset & ~0x1FF) << 3) + ((dxy.y & 16) << 7) + ((offset & 0x1C0) << 2) +
      (((((dxy.y & 8) >> 2) + (dxy.x >> 3)) & 3) << 6) + (offset & 0x3F);

  uint dest_byte = uint(tiled_byte) + constants.dest_base;
  uint dest_index = dest_byte >> 4u;

  dest_buffer[dest_index] = edram_data0;
  dest_buffer[dest_index + 2u] = edram_data1;
}
)MSL";

// ---------------------------------------------------------------------------
// Hand-pack a deterministic 8888 pattern into EDRAM for the resolve region.
// Each guest pixel (x, y) gets a unique sentinel value so any addressing error
// surfaces as a mismatch.
// ---------------------------------------------------------------------------
uint32_t PatternFor(uint32_t x, uint32_t y) {
  // 0xAARRGGBB-ish sentinel: high byte 0xC0 | low coords. Distinct per pixel.
  return 0xC0000000u | ((x & 0xFFFu) << 12u) | (y & 0xFFFu);
}

void PackEdramPattern(uint32_t* edram, const ResolveCopyShaderConstants& c, uint32_t width_px,
                      uint32_t height_px) {
  uint32_t pitch_tiles = c.edram_info & 0x3FFu;
  uint32_t msaa_samples = (c.edram_info >> 10u) & 0x3u;
  uint32_t base_tiles = (c.edram_info >> 13u) & 0x7FFu;
  uint32_t edram_off_x = ((c.coordinate_info >> 0u) & 0xFu) << 3u;
  uint32_t edram_off_y = ((c.coordinate_info >> 4u) & 0x1u) << 3u;
  uint32_t copy_sample_select = (c.dest_coordinate_info >> 28u) & 0x7u;
  for (uint32_t y = 0; y < height_px; ++y) {
    for (uint32_t x = 0; x < width_px; ++x) {
      uint32_t idx = EdramDwordIndex(pitch_tiles, base_tiles, msaa_samples, x + edram_off_x,
                                     y + edram_off_y, copy_sample_select);
      edram[idx] = PatternFor(x, y);
    }
  }
}

int RunResolveTest() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      std::fprintf(stderr, "[metal_resolve_test] FAIL: no MTLDevice\n");
      return 1;
    }
    std::fprintf(stdout, "[metal_resolve_test] device: %s\n", device.name.UTF8String);

    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!queue) {
      std::fprintf(stderr, "[metal_resolve_test] FAIL: no command queue\n");
      return 1;
    }

    // ----- Representative resolve config: 1x MSAA, 32bpp 8888, tile-aligned. -----
    // Region: 128 x 32 pixels (multiple of group sizes: 128 = 2*64, 32 = 4*8).
    const uint32_t width_px = 128;
    const uint32_t height_px = 32;
    const uint32_t width_div_8 = width_px / 8;    // 16
    const uint32_t height_div_8 = height_px / 8;  // 4

    // EDRAM source: pitch chosen to cover width in tiles; base tile 0.
    // 1x MSAA, 32bpp: surface pitch tiles = ceil(width / 80). For 128 px -> 2 tiles.
    const uint32_t edram_pitch_tiles =
        (width_px + kEdramTileWidthSamples - 1) / kEdramTileWidthSamples;  // 2
    const uint32_t edram_base_tiles = 0;
    const uint32_t msaa_samples = 0;  // k1X

    // Dest: tightly-tiled 2D 8888 surface. Pitch aligned to 32 texels.
    const uint32_t dest_pitch_texels = (width_px + 31u) & ~31u;    // 128
    const uint32_t dest_height_texels = (height_px + 31u) & ~31u;  // 32
    const uint32_t dest_pitch_div_32 = dest_pitch_texels >> 5u;    // 4
    const uint32_t dest_height_div_32 = dest_height_texels >> 5u;  // 1
    const uint32_t dest_base = 0;

    ResolveCopyShaderConstants constants;
    constants.edram_info = PackEdramInfo(edram_pitch_tiles, msaa_samples, /*is_depth=*/0,
                                         edram_base_tiles, /*format=*/0, /*format_is_64bpp=*/0,
                                         /*fill_half_pixel=*/0);
    constants.coordinate_info =
        PackCoordinateInfo(/*off_x_div_8=*/0, /*off_y_div_8=*/0, width_div_8,
                           /*scale_x=*/1, /*scale_y=*/1);
    constants.dest_info = 0;  // endian k1D (0 = none), no swap, no exp_bias, 2D.
    constants.dest_coordinate_info = PackDestCoordinateInfo(dest_pitch_div_32, dest_height_div_32,
                                                            /*off_x_div_8=*/0, /*off_y_div_8=*/0,
                                                            /*copy_sample_select=*/0);  // k0
    constants.dest_base = dest_base;

    // ----- Allocate buffers (Shared so CPU can stage in and read back). -----
    id<MTLBuffer> edram_buffer = [device newBufferWithLength:kEdramSizeBytes
                                                     options:MTLResourceStorageModeShared];
    // Dest sized to the full tiled extent of the surface (+ a guard tile).
    const uint32_t dest_size_bytes = (dest_pitch_texels * dest_height_texels + 1024u) * 4u;
    id<MTLBuffer> dest_buffer = [device newBufferWithLength:dest_size_bytes
                                                    options:MTLResourceStorageModeShared];
    if (!edram_buffer || !dest_buffer) {
      std::fprintf(stderr, "[metal_resolve_test] FAIL: buffer allocation\n");
      return 1;
    }

    // Zero both buffers, then pack the EDRAM input pattern.
    std::memset([edram_buffer contents], 0, kEdramSizeBytes);
    std::memset([dest_buffer contents], 0, dest_size_bytes);
    PackEdramPattern(reinterpret_cast<uint32_t*>([edram_buffer contents]), constants, width_px,
                     height_px);

    // ----- Compile the MSL kernel and build the compute pipeline. -----
    NSError* error = nil;
    id<MTLLibrary> library =
        [device newLibraryWithSource:[NSString stringWithUTF8String:kResolveMsl]
                             options:nil
                               error:&error];
    if (!library) {
      std::fprintf(stderr, "[metal_resolve_test] FAIL: MSL compile: %s\n",
                   error ? error.localizedDescription.UTF8String : "unknown");
      return 1;
    }
    id<MTLFunction> function = [library newFunctionWithName:@"resolve_copy_fast_32bpp_1x2xmsaa"];
    if (!function) {
      std::fprintf(stderr, "[metal_resolve_test] FAIL: kernel function not found\n");
      return 1;
    }
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function
                                                                                 error:&error];
    if (!pipeline) {
      std::fprintf(stderr, "[metal_resolve_test] FAIL: pipeline: %s\n",
                   error ? error.localizedDescription.UTF8String : "unknown");
      return 1;
    }

    // ----- Dispatch. Group counts mirror ResolveInfo::GetCopyShader (draw.cpp:1172). -----
    // group_size_x = 64 px (1<<6), group_size_y = 8 px (1<<3).
    const uint32_t group_count_x = (width_px + (kGroupPixelsX - 1)) / kGroupPixelsX;   // 2
    const uint32_t group_count_y = (height_px + (kGroupPixelsY - 1)) / kGroupPixelsY;  // 4

    id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:edram_buffer offset:0 atIndex:0];
    [encoder setBuffer:dest_buffer offset:0 atIndex:1];
    [encoder setBytes:&constants length:sizeof(constants) atIndex:2];
    [encoder dispatchThreadgroups:MTLSizeMake(group_count_x, group_count_y, 1)
            threadsPerThreadgroup:MTLSizeMake(kThreadgroupX, kThreadgroupY, 1)];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];

    if (command_buffer.status != MTLCommandBufferStatusCompleted) {
      std::fprintf(
          stderr, "[metal_resolve_test] FAIL: dispatch status %lu: %s\n",
          (unsigned long)command_buffer.status,
          command_buffer.error ? command_buffer.error.localizedDescription.UTF8String : "unknown");
      return 1;
    }

    // ----- CPU reference into a matching-size buffer. -----
    std::vector<uint8_t> cpu_dest(dest_size_bytes, 0);
    CpuReference(reinterpret_cast<const uint32_t*>([edram_buffer contents]), constants, width_px,
                 height_px, cpu_dest);

    // ----- Compare GPU dest vs CPU reference, byte for byte. -----
    const uint8_t* gpu_dest = reinterpret_cast<const uint8_t*>([dest_buffer contents]);
    uint32_t mismatches = 0;
    uint32_t first_mismatch = 0;
    for (uint32_t i = 0; i < dest_size_bytes; ++i) {
      if (gpu_dest[i] != cpu_dest[i]) {
        if (mismatches == 0) {
          first_mismatch = i;
        }
        ++mismatches;
      }
    }

    if (mismatches != 0) {
      std::fprintf(stderr,
                   "[metal_resolve_test] FAIL: %u byte mismatches (first @0x%x: gpu=0x%02x "
                   "cpu=0x%02x)\n",
                   mismatches, first_mismatch, gpu_dest[first_mismatch], cpu_dest[first_mismatch]);
      return 1;
    }

    // Sanity: ensure we actually wrote something (guard against all-zero match).
    uint32_t nonzero = 0;
    for (uint32_t i = 0; i < dest_size_bytes; ++i) {
      nonzero += (cpu_dest[i] != 0) ? 1u : 0u;
    }
    if (nonzero == 0) {
      std::fprintf(stderr, "[metal_resolve_test] FAIL: reference produced no output\n");
      return 1;
    }

    std::fprintf(stdout,
                 "[metal_resolve_test] PASS: %u px (%ux%u) resolved, %u dest bytes match CPU "
                 "reference\n",
                 width_px * height_px, width_px, height_px, nonzero);
    return 0;
  }
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
  return RunResolveTest();
}
