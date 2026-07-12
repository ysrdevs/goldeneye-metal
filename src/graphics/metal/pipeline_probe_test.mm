// pipeline_probe_test.mm -- Metal producer-path integration test.
//
// Exercises two bindings used by translated Xenos draws through the real
// RenderPipelineProbeToContext implementation:
//   * vertex positions supplied by an existing (externally owned) MTLBuffer;
//   * a single-layer MTLTextureType2DArray sampled by the fragment shader.
//
// The shader sources intentionally have no generated geometry or constant
// fallback color. A visible triangle with the texture's sentinel color proves
// both resources reached the draw.

#import <Metal/Metal.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <rex/graphics/metal/msl_compiler.h>

namespace {

using rex::graphics::metal::ClearPipelineProbeContext;
using rex::graphics::metal::CreateHostRenderTargetContext;
using rex::graphics::metal::CreateMslLibrary;
using rex::graphics::metal::CreatePipelineProbeContext;
using rex::graphics::metal::CreateRenderPipelineState;
using rex::graphics::metal::GetPipelineProbeContextPendingSubmissionCount;
using rex::graphics::metal::GetPipelineProbeContextUploadStats;
using rex::graphics::metal::PipelineProbeUploadStats;
using rex::graphics::metal::ProbeColorTargetState;
using rex::graphics::metal::ProbeCullMode;
using rex::graphics::metal::ProbeDepthStencilState;
using rex::graphics::metal::ProbeIndexBuffer;
using rex::graphics::metal::ProbeRasterizationState;
using rex::graphics::metal::ProbeSamplerSlot;
using rex::graphics::metal::ProbeTextureSlot;
using rex::graphics::metal::ProbeTiledResolveTarget;
using rex::graphics::metal::QueuePipelineProbeContextClearRect;
using rex::graphics::metal::ReadPipelineProbeContext;
using rex::graphics::metal::ReadPipelineProbeContextRect;
using rex::graphics::metal::ReleaseMslLibrary;
using rex::graphics::metal::ReleasePipelineProbeContext;
using rex::graphics::metal::ReleaseRenderPipelineState;
using rex::graphics::metal::RenderPipelineProbeToContext;
using rex::graphics::metal::ResetPipelineProbeContext;
using rex::graphics::metal::ResolvePipelineProbeContextToXenosTiled;
using rex::graphics::metal::WaitPipelineProbeContext;

constexpr uint32_t kWidth = 40;
constexpr uint32_t kHeight = 40;

constexpr char kVertexMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexOutput {
  float4 position [[position]];
  float2 uv [[user(locn0)]];
};

vertex VertexOutput main0(device const float2* positions [[buffer(3)]],
                          uint vertex_id [[vertex_id]]) {
  VertexOutput output;
  float2 position = positions[vertex_id];
  output.position = float4(position, 0.0, 1.0);
  output.uv = position * 0.5 + 0.5;
  return output;
}
)MSL";

constexpr char kFragmentMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct FragmentInput {
  float2 uv [[user(locn0)]];
};

fragment float4 main0(FragmentInput input [[stage_in]],
                      texture2d_array<float> source [[texture(0)]],
                      sampler source_sampler [[sampler(0)]]) {
  return source.sample(source_sampler, input.uv, 0u);
}
)MSL";

constexpr char kDepthVertexMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexOutput {
  float4 position [[position]];
  float2 uv [[user(locn0)]];
};

vertex VertexOutput main0(device const packed_float3* positions [[buffer(3)]],
                          uint vertex_id [[vertex_id]]) {
  VertexOutput output;
  float3 position = float3(positions[vertex_id]);
  output.position = float4(position, 1.0);
  output.uv = position.xy * 0.5 + 0.5;
  return output;
}
)MSL";

bool PixelNear(const uint8_t* bgra, const std::array<uint8_t, 4>& expected, uint8_t tolerance = 1) {
  for (size_t i = 0; i < expected.size(); ++i) {
    int difference = int(bgra[i]) - int(expected[i]);
    if (difference < -int(tolerance) || difference > int(tolerance)) {
      return false;
    }
  }
  return true;
}

void StoreExpectedTiledPixel(uint8_t* destination, const uint8_t* bgra, uint32_t endian) {
  switch (endian) {
    case 1:
      destination[0] = bgra[1];
      destination[1] = bgra[2];
      destination[2] = bgra[3];
      destination[3] = bgra[0];
      break;
    case 2:
      destination[0] = bgra[3];
      destination[1] = bgra[0];
      destination[2] = bgra[1];
      destination[3] = bgra[2];
      break;
    case 3:
      destination[0] = bgra[0];
      destination[1] = bgra[3];
      destination[2] = bgra[2];
      destination[3] = bgra[1];
      break;
    default:
      destination[0] = bgra[2];
      destination[1] = bgra[1];
      destination[2] = bgra[0];
      destination[3] = bgra[3];
      break;
  }
}

uint32_t GetExpectedTiledRgba8Offset(uint32_t x, uint32_t y, uint32_t pitch) {
  pitch = (pitch + 31u) & ~31u;
  uint32_t macro = ((x >> 5u) + (y >> 5u) * (pitch >> 5u)) << 9u;
  uint32_t micro = ((x & 7u) + ((y & 14u) << 2u)) << 2u;
  uint32_t offset = macro + ((micro & ~15u) << 1u) + (micro & 15u) + ((y & 1u) << 4u);
  return ((offset & ~511u) << 3u) + ((y & 16u) << 7u) + ((offset & 448u) << 2u) +
         (((((y & 8u) >> 2u) + (x >> 3u)) & 3u) << 6u) + (offset & 63u);
}

bool ApplyExpectedTiledRect(std::vector<uint8_t>& expected, size_t buffer_offset,
                            uint32_t destination_pitch, uint32_t destination_x,
                            uint32_t destination_y, const uint8_t* source_bgra,
                            uint32_t source_width, uint32_t copy_width, uint32_t copy_height,
                            uint32_t endian) {
  for (uint32_t y = 0; y < copy_height; ++y) {
    for (uint32_t x = 0; x < copy_width; ++x) {
      uint32_t tiled_offset =
          GetExpectedTiledRgba8Offset(destination_x + x, destination_y + y, destination_pitch);
      size_t target_offset = buffer_offset + tiled_offset;
      if (target_offset > expected.size() || expected.size() - target_offset < 4) {
        return false;
      }
      const uint8_t* source = source_bgra + (size_t(y) * source_width + x) * 4;
      StoreExpectedTiledPixel(expected.data() + target_offset, source, endian);
    }
  }
  return true;
}

int RunPipelineProbeTest() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      std::fprintf(stderr, "[metal_pipeline_probe_test] FAIL: no MTLDevice\n");
      return 1;
    }
    std::fprintf(stdout, "[metal_pipeline_probe_test] device: %s\n", device.name.UTF8String);

    std::string error;
    void* vertex_library = CreateMslLibrary(device, kVertexMsl, &error);
    if (!vertex_library) {
      std::fprintf(stderr, "[metal_pipeline_probe_test] FAIL: vertex MSL: %s\n", error.c_str());
      return 1;
    }
    void* fragment_library = CreateMslLibrary(device, kFragmentMsl, &error);
    if (!fragment_library) {
      std::fprintf(stderr, "[metal_pipeline_probe_test] FAIL: fragment MSL: %s\n", error.c_str());
      ReleaseMslLibrary(vertex_library);
      return 1;
    }
    void* pipeline_state =
        CreateRenderPipelineState(device, vertex_library, fragment_library, &error);
    ProbeColorTargetState alpha_blend_target_state;
    alpha_blend_target_state.write_mask = 0xF;
    // RB_BLENDCONTROL0: source alpha / inverse source alpha, add, for both
    // color and alpha. This is the title's dominant translucent-draw mode.
    alpha_blend_target_state.blend_control = 0x07060706;
    void* alpha_blend_pipeline_state =
        pipeline_state ? CreateRenderPipelineState(device, vertex_library, fragment_library, &error,
                                                   &alpha_blend_target_state)
                       : nullptr;
    ProbeColorTargetState partial_write_target_state;
    // Xenos mask bits are R/G/B/A. Writing only red and blue makes a mask-order
    // error visible in every channel of the BGRA8 readback.
    partial_write_target_state.write_mask = 0x5;
    partial_write_target_state.blend_control = 0x00010001;
    void* partial_write_pipeline_state =
        alpha_blend_pipeline_state
            ? CreateRenderPipelineState(device, vertex_library, fragment_library, &error,
                                        &partial_write_target_state)
            : nullptr;
    void* depth_vertex_library =
        partial_write_pipeline_state ? CreateMslLibrary(device, kDepthVertexMsl, &error) : nullptr;
    void* depth_pipeline_state =
        depth_vertex_library
            ? CreateRenderPipelineState(device, depth_vertex_library, fragment_library, &error)
            : nullptr;
    ReleaseMslLibrary(depth_vertex_library);
    ReleaseMslLibrary(vertex_library);
    ReleaseMslLibrary(fragment_library);
    auto release_pipeline_states = [&]() {
      ReleaseRenderPipelineState(depth_pipeline_state);
      ReleaseRenderPipelineState(partial_write_pipeline_state);
      ReleaseRenderPipelineState(alpha_blend_pipeline_state);
      ReleaseRenderPipelineState(pipeline_state);
    };
    if (!pipeline_state || !alpha_blend_pipeline_state || !partial_write_pipeline_state ||
        !depth_pipeline_state) {
      std::fprintf(stderr, "[metal_pipeline_probe_test] FAIL: render pipeline: %s\n",
                   error.c_str());
      release_pipeline_states();
      return 1;
    }

    void* context = CreatePipelineProbeContext(device, &error);
    if (!context) {
      std::fprintf(stderr, "[metal_pipeline_probe_test] FAIL: probe context: %s\n", error.c_str());
      release_pipeline_states();
      return 1;
    }

    // A centered triangle leaves the corners clear. No CPU vertex-data pointer
    // is supplied to the probe: the shader can obtain these positions only
    // through the externally allocated MTLBuffer at buffer(3).
    constexpr std::array<float, 6> kPositions = {
        -0.8f, -0.8f, 0.8f, -0.8f, 0.0f, 0.8f,
    };
    id<MTLBuffer> position_buffer = [device newBufferWithBytes:kPositions.data()
                                                        length:sizeof(kPositions)
                                                       options:MTLResourceStorageModeShared];
    if (!position_buffer) {
      std::fprintf(stderr, "[metal_pipeline_probe_test] FAIL: external position buffer\n");
      ReleasePipelineProbeContext(context);
      release_pipeline_states();
      return 1;
    }

    // RGBA source bytes. ReadPipelineProbeContext returns the BGRA8 render
    // target's bytes, so the corresponding expected value is B,G,R,A.
    constexpr std::array<uint8_t, 4> kTextureRgba = {31, 173, 89, 255};
    constexpr std::array<uint8_t, 4> kExpectedBgra = {89, 173, 31, 255};
    ProbeTextureSlot texture;
    texture.rgba = kTextureRgba.data();
    texture.width = 1;
    texture.height = 1;
    texture.array_length = 1;
    texture.bytes_per_row = kTextureRgba.size();
    texture.bytes_per_image = kTextureRgba.size();

    ProbeSamplerSlot sampler;
    sampler.min_linear = 0;
    sampler.mag_linear = 0;
    sampler.mip_linear = 0;

    constexpr std::array<uint32_t, 4> kUnusedSystemConstants = {};
    bool rendered = RenderPipelineProbeToContext(
        context, pipeline_state, kUnusedSystemConstants.data(), sizeof(kUnusedSystemConstants),
        /*float_constants=*/nullptr, /*float_constants_size=*/0,
        /*fetch_constants=*/nullptr, /*fetch_constants_size=*/0,
        /*shared_memory=*/nullptr, /*shared_memory_size=*/0,
        /*shared_memory_metal_buffer=*/position_buffer,
        /*vertex_textures=*/nullptr, /*vertex_texture_count=*/0,
        /*vertex_sampler_count=*/0, &texture, /*fragment_texture_count=*/1,
        /*fragment_sampler_count=*/1,
        /*primitive_type=TriangleList=*/4, /*vertex_count=*/3, kWidth, kHeight, &error,
        /*vertex_shared_memory_buffer_index=*/3,
        /*vertex_float_constants_buffer_index=*/UINT32_MAX,
        /*vertex_fetch_constants_buffer_index=*/UINT32_MAX,
        /*fragment_float_constants=*/nullptr, /*fragment_float_constants_size=*/0,
        /*fragment_float_constants_buffer_index=*/UINT32_MAX,
        /*fragment_fetch_constants_buffer_index=*/UINT32_MAX,
        /*vertex_samplers=*/nullptr, &sampler,
        /*vertex_data=*/nullptr, /*vertex_data_size=*/0,
        /*vertex_data_buffer_index=*/UINT32_MAX,
        /*bool_loop_constants=*/nullptr, /*bool_loop_constants_size=*/0,
        /*vertex_bool_loop_constants_buffer_index=*/UINT32_MAX,
        /*fragment_bool_loop_constants_buffer_index=*/UINT32_MAX);
    uint32_t pending_after_render = GetPipelineProbeContextPendingSubmissionCount(context);

    std::vector<uint8_t> bgra;
    bool read = rendered && ReadPipelineProbeContext(context, kWidth, kHeight, bgra, &error);
    uint32_t pending_after_read = GetPipelineProbeContextPendingSubmissionCount(context);

    [position_buffer release];

    if (!rendered || pending_after_render != 1 || !read || pending_after_read != 0) {
      std::fprintf(stderr,
                   "[metal_pipeline_probe_test] FAIL: asynchronous probe draw/readback: %s "
                   "rendered=%d pending_before_read=%u read=%d pending_after_read=%u\n",
                   error.c_str(), int(rendered), pending_after_render, int(read),
                   pending_after_read);
      ReleasePipelineProbeContext(context);
      release_pipeline_states();
      return 1;
    }
    if (bgra.size() != size_t(kWidth) * kHeight * 4) {
      std::fprintf(stderr, "[metal_pipeline_probe_test] FAIL: readback size %zu, expected %zu\n",
                   bgra.size(), size_t(kWidth) * kHeight * 4);
      ReleasePipelineProbeContext(context);
      release_pipeline_states();
      return 1;
    }

    size_t textured_pixels = 0;
    size_t clear_pixels = 0;
    constexpr std::array<uint8_t, 4> kClearBgra = {0, 0, 0, 255};
    for (size_t pixel = 0; pixel < size_t(kWidth) * kHeight; ++pixel) {
      const uint8_t* value = bgra.data() + pixel * 4;
      textured_pixels += PixelNear(value, kExpectedBgra) ? 1 : 0;
      clear_pixels += PixelNear(value, kClearBgra) ? 1 : 0;
    }

    const uint8_t* center = bgra.data() + (size_t(kHeight / 2) * kWidth + kWidth / 2) * 4;
    const uint8_t* corner = bgra.data();
    if (!PixelNear(center, kExpectedBgra) || !PixelNear(corner, kClearBgra) ||
        textured_pixels < 100 || clear_pixels < 100) {
      std::fprintf(stderr,
                   "[metal_pipeline_probe_test] FAIL: center=%u,%u,%u,%u corner=%u,%u,%u,%u "
                   "textured=%zu clear=%zu\n",
                   center[0], center[1], center[2], center[3], corner[0], corner[1], corner[2],
                   corner[3], textured_pixels, clear_pixels);
      ReleasePipelineProbeContext(context);
      release_pipeline_states();
      return 1;
    }

    constexpr uint32_t kRegionX = 5;
    constexpr uint32_t kRegionY = 7;
    constexpr uint32_t kRegionWidth = 17;
    constexpr uint32_t kRegionHeight = 13;
    std::vector<uint8_t> region_bgra;
    bool region_read =
        ReadPipelineProbeContextRect(context, kWidth, kHeight, kRegionX, kRegionY, kRegionWidth,
                                     kRegionHeight, region_bgra, &error);
    bool region_matches =
        region_read && region_bgra.size() == size_t(kRegionWidth) * kRegionHeight * 4;
    for (uint32_t row = 0; row < kRegionHeight && region_matches; ++row) {
      const uint8_t* expected = bgra.data() + (size_t(kRegionY + row) * kWidth + kRegionX) * 4;
      const uint8_t* actual = region_bgra.data() + size_t(row) * kRegionWidth * 4;
      region_matches = std::memcmp(actual, expected, size_t(kRegionWidth) * 4) == 0;
    }
    if (!region_matches) {
      std::fprintf(stderr,
                   "[metal_pipeline_probe_test] FAIL: rectangular readback read=%d bytes=%zu: %s\n",
                   int(region_read), region_bgra.size(), error.c_str());
      ReleasePipelineProbeContext(context);
      release_pipeline_states();
      return 1;
    }

    bool queued_clear = QueuePipelineProbeContextClearRect(context, kWidth, kHeight, 0, 0, 4, 4,
                                                           1.0, 0.25, 0.5, 1.0, &error);
    uint32_t queued_clear_pending = GetPipelineProbeContextPendingSubmissionCount(context);
    std::vector<uint8_t> queued_clear_bgra;
    bool queued_clear_read = queued_clear && ReadPipelineProbeContext(context, kWidth, kHeight,
                                                                      queued_clear_bgra, &error);
    uint32_t queued_clear_pending_after = GetPipelineProbeContextPendingSubmissionCount(context);
    constexpr std::array<uint8_t, 4> kQueuedClearBgra = {128, 64, 255, 255};
    bool queued_clear_ok = queued_clear && queued_clear_pending == 1 && queued_clear_read &&
                           queued_clear_pending_after == 0 &&
                           queued_clear_bgra.size() == size_t(kWidth) * kHeight * 4 &&
                           PixelNear(queued_clear_bgra.data(), kQueuedClearBgra) &&
                           std::memcmp(queued_clear_bgra.data() + (size_t(10) * kWidth + 10) * 4,
                                       bgra.data() + (size_t(10) * kWidth + 10) * 4, 4) == 0;
    if (!queued_clear_ok) {
      std::fprintf(stderr,
                   "[metal_pipeline_probe_test] FAIL: queued clear queued=%d pending=%u "
                   "read=%d pending_after=%u bytes=%zu: %s\n",
                   int(queued_clear), queued_clear_pending, int(queued_clear_read),
                   queued_clear_pending_after, queued_clear_bgra.size(), error.c_str());
      ReleasePipelineProbeContext(context);
      release_pipeline_states();
      return 1;
    }

    // Exercise the exact converted-triangle-fan route used by guest draws:
    // four source vertices become a six-index triangle list. A non-indexed
    // submission would read two vertices beyond this buffer.
    constexpr std::array<float, 8> kFanPositions = {
        -0.8f, -0.8f, 0.8f, -0.8f, 0.8f, 0.8f, -0.8f, 0.8f,
    };
    constexpr std::array<uint16_t, 6> kFanIndices = {1, 2, 0, 2, 3, 0};
    id<MTLBuffer> fan_position_buffer = [device newBufferWithBytes:kFanPositions.data()
                                                            length:sizeof(kFanPositions)
                                                           options:MTLResourceStorageModeShared];
    if (!fan_position_buffer) {
      std::fprintf(stderr, "[metal_pipeline_probe_test] FAIL: indexed position buffer\n");
      ReleasePipelineProbeContext(context);
      release_pipeline_states();
      return 1;
    }
    ProbeIndexBuffer fan_index_buffer;
    fan_index_buffer.data = kFanIndices.data();
    fan_index_buffer.size = sizeof(kFanIndices);
    fan_index_buffer.index_size = sizeof(uint16_t);
    id<MTLBuffer> fan_metal_index_buffer = [device newBufferWithBytes:kFanIndices.data()
                                                               length:sizeof(kFanIndices)
                                                              options:MTLResourceStorageModeShared];
    if (!fan_metal_index_buffer) {
      std::fprintf(stderr, "[metal_pipeline_probe_test] FAIL: external index buffer\n");
      [fan_position_buffer release];
      ReleasePipelineProbeContext(context);
      release_pipeline_states();
      return 1;
    }
    ProbeIndexBuffer external_fan_index_buffer;
    external_fan_index_buffer.metal_buffer = fan_metal_index_buffer;
    external_fan_index_buffer.size = sizeof(kFanIndices);
    external_fan_index_buffer.index_size = sizeof(uint16_t);
    ResetPipelineProbeContext(context);
    bool indexed_rendered = RenderPipelineProbeToContext(
        context, pipeline_state, kUnusedSystemConstants.data(), sizeof(kUnusedSystemConstants),
        nullptr, 0, nullptr, 0, nullptr, 0, fan_position_buffer, nullptr, 0, 0, &texture, 1, 1,
        /*primitive_type=TriangleList=*/4, uint32_t(kFanIndices.size()), kWidth, kHeight, &error,
        /*vertex_shared_memory_buffer_index=*/3, UINT32_MAX, UINT32_MAX, nullptr, 0, UINT32_MAX,
        UINT32_MAX, nullptr, &sampler, nullptr, 0, UINT32_MAX, nullptr, 0, UINT32_MAX, UINT32_MAX,
        &fan_index_buffer);
    std::vector<uint8_t> indexed_bgra;
    bool indexed_read = indexed_rendered &&
                        ReadPipelineProbeContext(context, kWidth, kHeight, indexed_bgra, &error);
    size_t indexed_textured_pixels = 0;
    if (indexed_read) {
      for (size_t pixel = 0; pixel < size_t(kWidth) * kHeight; ++pixel) {
        indexed_textured_pixels +=
            PixelNear(indexed_bgra.data() + pixel * 4, kExpectedBgra) ? 1 : 0;
      }
    }
    const uint8_t* indexed_center =
        indexed_read ? indexed_bgra.data() + (size_t(kHeight / 2) * kWidth + kWidth / 2) * 4
                     : nullptr;
    bool indexed_ok = indexed_read && indexed_center && PixelNear(indexed_center, kExpectedBgra) &&
                      indexed_textured_pixels > 500;
    ProbeRasterizationState rasterization_state;
    rasterization_state.viewport_width = kWidth;
    rasterization_state.viewport_height = kHeight;
    rasterization_state.scissor_width = kWidth / 2;
    rasterization_state.scissor_height = kHeight;
    ResetPipelineProbeContext(context);
    bool scissored_rendered =
        indexed_ok &&
        RenderPipelineProbeToContext(
            context, pipeline_state, kUnusedSystemConstants.data(), sizeof(kUnusedSystemConstants),
            nullptr, 0, nullptr, 0, nullptr, 0, fan_position_buffer, nullptr, 0, 0, &texture, 1, 1,
            /*primitive_type=TriangleList=*/4, uint32_t(kFanIndices.size()), kWidth, kHeight,
            &error,
            /*vertex_shared_memory_buffer_index=*/3, UINT32_MAX, UINT32_MAX, nullptr, 0, UINT32_MAX,
            UINT32_MAX, nullptr, &sampler, nullptr, 0, UINT32_MAX, nullptr, 0, UINT32_MAX,
            UINT32_MAX, &external_fan_index_buffer, &rasterization_state);
    std::vector<uint8_t> scissored_bgra;
    bool scissored_read = scissored_rendered && ReadPipelineProbeContext(context, kWidth, kHeight,
                                                                         scissored_bgra, &error);
    const uint8_t* scissored_left =
        scissored_read ? scissored_bgra.data() + (size_t(kHeight / 2) * kWidth + 10) * 4 : nullptr;
    const uint8_t* scissored_right =
        scissored_read ? scissored_bgra.data() + (size_t(kHeight / 2) * kWidth + 30) * 4 : nullptr;
    bool scissored_ok = scissored_left && scissored_right &&
                        PixelNear(scissored_left, kExpectedBgra) &&
                        PixelNear(scissored_right, kClearBgra);

    auto render_indexed_fan_to_context = [&](void* draw_context, void* draw_pipeline_state,
                                             const ProbeTextureSlot* draw_texture,
                                             uint32_t draw_width, uint32_t draw_height) {
      return RenderPipelineProbeToContext(
          draw_context, draw_pipeline_state, kUnusedSystemConstants.data(),
          sizeof(kUnusedSystemConstants), nullptr, 0, nullptr, 0, nullptr, 0, fan_position_buffer,
          nullptr, 0, 0, draw_texture, 1, 1,
          /*primitive_type=TriangleList=*/4, uint32_t(kFanIndices.size()), draw_width, draw_height,
          &error,
          /*vertex_shared_memory_buffer_index=*/3, UINT32_MAX, UINT32_MAX, nullptr, 0, UINT32_MAX,
          UINT32_MAX, nullptr, &sampler, nullptr, 0, UINT32_MAX, nullptr, 0, UINT32_MAX, UINT32_MAX,
          &fan_index_buffer);
    };
    auto render_indexed_fan = [&](void* draw_pipeline_state, const ProbeTextureSlot* draw_texture) {
      return render_indexed_fan_to_context(context, draw_pipeline_state, draw_texture, kWidth,
                                           kHeight);
    };
    auto render_uploaded_indexed_fan = [&](const std::array<float, 8>& positions) {
      return RenderPipelineProbeToContext(
          context, pipeline_state, kUnusedSystemConstants.data(), sizeof(kUnusedSystemConstants),
          nullptr, 0, nullptr, 0, nullptr, 0, nullptr, nullptr, 0, 0, &texture, 1, 1,
          /*primitive_type=TriangleList=*/4, uint32_t(kFanIndices.size()), kWidth, kHeight, &error,
          /*vertex_shared_memory_buffer_index=*/UINT32_MAX, UINT32_MAX, UINT32_MAX, nullptr, 0,
          UINT32_MAX, UINT32_MAX, nullptr, &sampler, positions.data(), sizeof(positions),
          /*vertex_data_buffer_index=*/3, nullptr, 0, UINT32_MAX, UINT32_MAX, &fan_index_buffer);
    };

    // The fan is counter-clockwise in clip space. Verify both Metal cull modes
    // and both PA_SU_SC_MODE_CNTL::face conventions, resetting the target for
    // each case so a culled draw can't pass by preserving earlier pixels.
    auto render_cull_case = [&](ProbeCullMode cull_mode, bool front_face_clockwise,
                                bool expect_visible) {
      ProbeRasterizationState cull_state;
      cull_state.viewport_width = kWidth;
      cull_state.viewport_height = kHeight;
      cull_state.scissor_width = kWidth;
      cull_state.scissor_height = kHeight;
      cull_state.cull_mode = cull_mode;
      cull_state.front_face_clockwise = front_face_clockwise;
      ResetPipelineProbeContext(context);
      bool cull_rendered = RenderPipelineProbeToContext(
          context, pipeline_state, kUnusedSystemConstants.data(), sizeof(kUnusedSystemConstants),
          nullptr, 0, nullptr, 0, nullptr, 0, fan_position_buffer, nullptr, 0, 0, &texture, 1, 1,
          /*primitive_type=TriangleList=*/4, uint32_t(kFanIndices.size()), kWidth, kHeight, &error,
          /*vertex_shared_memory_buffer_index=*/3, UINT32_MAX, UINT32_MAX, nullptr, 0, UINT32_MAX,
          UINT32_MAX, nullptr, &sampler, nullptr, 0, UINT32_MAX, nullptr, 0, UINT32_MAX, UINT32_MAX,
          &external_fan_index_buffer, &cull_state);
      std::vector<uint8_t> cull_bgra;
      bool cull_read =
          cull_rendered && ReadPipelineProbeContext(context, kWidth, kHeight, cull_bgra, &error);
      const uint8_t* cull_center =
          cull_read ? cull_bgra.data() + (size_t(kHeight / 2) * kWidth + kWidth / 2) * 4 : nullptr;
      bool visible = cull_center && PixelNear(cull_center, kExpectedBgra);
      return cull_read && visible == expect_visible;
    };
    bool cull_back_ccw_ok =
        render_cull_case(ProbeCullMode::kBack, /*front_face_clockwise=*/false, true);
    bool cull_front_ccw_ok =
        render_cull_case(ProbeCullMode::kFront, /*front_face_clockwise=*/false, false);
    bool cull_back_cw_ok =
        render_cull_case(ProbeCullMode::kBack, /*front_face_clockwise=*/true, false);
    bool cull_front_cw_ok =
        render_cull_case(ProbeCullMode::kFront, /*front_face_clockwise=*/true, true);
    bool cull_winding_ok =
        cull_back_ccw_ok && cull_front_ccw_ok && cull_back_cw_ok && cull_front_cw_ok;

    // Verify that the context-owned depth/stencil texture survives across
    // multiple draws and that changing only the dynamic test/reference state
    // doesn't require a new render pipeline or render pass.
    constexpr std::array<float, 12> kNearDepthPositions = {
        -0.8f, -0.8f, 0.25f, 0.8f, -0.8f, 0.25f, 0.8f, 0.8f, 0.25f, -0.8f, 0.8f, 0.25f,
    };
    constexpr std::array<float, 12> kFarDepthPositions = {
        -0.8f, -0.8f, 0.75f, 0.8f, -0.8f, 0.75f, 0.8f, 0.8f, 0.75f, -0.8f, 0.8f, 0.75f,
    };
    constexpr std::array<uint8_t, 4> kGreenRgba = {32, 200, 80, 255};
    constexpr std::array<uint8_t, 4> kRedRgba = {220, 40, 20, 255};
    constexpr std::array<uint8_t, 4> kBlueRgba = {15, 60, 230, 255};
    constexpr std::array<uint8_t, 4> kGreenBgra = {80, 200, 32, 255};
    constexpr std::array<uint8_t, 4> kRedBgra = {20, 40, 220, 255};
    auto make_solid_texture = [](const std::array<uint8_t, 4>& rgba) {
      ProbeTextureSlot slot;
      slot.rgba = rgba.data();
      slot.width = 1;
      slot.height = 1;
      slot.bytes_per_row = 4;
      slot.bytes_per_image = 4;
      return slot;
    };
    ProbeTextureSlot green_texture = make_solid_texture(kGreenRgba);
    ProbeTextureSlot red_texture = make_solid_texture(kRedRgba);
    ProbeTextureSlot blue_texture = make_solid_texture(kBlueRgba);
    ProbeRasterizationState depth_rasterization_state;
    depth_rasterization_state.viewport_width = kWidth;
    depth_rasterization_state.viewport_height = kHeight;
    depth_rasterization_state.scissor_width = kWidth;
    depth_rasterization_state.scissor_height = kHeight;
    auto render_depth_quad = [&](const std::array<float, 12>& positions,
                                 const ProbeTextureSlot& draw_texture,
                                 const ProbeDepthStencilState& depth_stencil_state) {
      return RenderPipelineProbeToContext(
          context, depth_pipeline_state, kUnusedSystemConstants.data(),
          sizeof(kUnusedSystemConstants), nullptr, 0, nullptr, 0, nullptr, 0, nullptr, nullptr, 0,
          0, &draw_texture, 1, 1,
          /*primitive_type=TriangleList=*/4, uint32_t(kFanIndices.size()), kWidth, kHeight, &error,
          /*vertex_shared_memory_buffer_index=*/UINT32_MAX, UINT32_MAX, UINT32_MAX, nullptr, 0,
          UINT32_MAX, UINT32_MAX, nullptr, &sampler, positions.data(), sizeof(positions),
          /*vertex_data_buffer_index=*/3, nullptr, 0, UINT32_MAX, UINT32_MAX, &fan_index_buffer,
          &depth_rasterization_state, &depth_stencil_state);
    };
    auto read_center = [&](std::vector<uint8_t>& output) {
      bool read_ok = ReadPipelineProbeContext(context, kWidth, kHeight, output, &error);
      return read_ok && output.size() == size_t(kWidth) * kHeight * 4
                 ? output.data() + (size_t(kHeight / 2) * kWidth + kWidth / 2) * 4
                 : nullptr;
    };

    ProbeDepthStencilState depth_less_write;
    depth_less_write.depth_test_enabled = true;
    depth_less_write.depth_write_enabled = true;
    depth_less_write.depth_compare_function = 1;  // CompareFunction::kLess.
    bool depth_cleared =
        ClearPipelineProbeContext(context, kWidth, kHeight, 0.0, 0.0, 0.0, 1.0, &error);
    bool depth_near_rendered =
        depth_cleared && render_depth_quad(kNearDepthPositions, green_texture, depth_less_write);
    bool depth_far_rendered =
        depth_near_rendered && render_depth_quad(kFarDepthPositions, red_texture, depth_less_write);
    std::vector<uint8_t> depth_bgra;
    const uint8_t* depth_center = depth_far_rendered ? read_center(depth_bgra) : nullptr;
    bool depth_persistence_ok = depth_center && PixelNear(depth_center, kGreenBgra);

    ProbeDepthStencilState depth_less_no_write = depth_less_write;
    depth_less_no_write.depth_write_enabled = false;
    bool no_write_cleared =
        depth_persistence_ok &&
        ClearPipelineProbeContext(context, kWidth, kHeight, 0.0, 0.0, 0.0, 1.0, &error);
    bool no_write_near_rendered =
        no_write_cleared &&
        render_depth_quad(kNearDepthPositions, green_texture, depth_less_no_write);
    bool no_write_far_rendered =
        no_write_near_rendered &&
        render_depth_quad(kFarDepthPositions, red_texture, depth_less_write);
    std::vector<uint8_t> no_write_bgra;
    const uint8_t* no_write_center = no_write_far_rendered ? read_center(no_write_bgra) : nullptr;
    bool depth_write_mask_ok = no_write_center && PixelNear(no_write_center, kRedBgra);

    ProbeDepthStencilState stencil_replace;
    stencil_replace.stencil_test_enabled = true;
    stencil_replace.front.compare_function = 7;              // CompareFunction::kAlways.
    stencil_replace.front.depth_stencil_pass_operation = 2;  // StencilOp::kReplace.
    stencil_replace.front.reference = 0x5A;
    stencil_replace.back = stencil_replace.front;
    ProbeDepthStencilState stencil_equal = stencil_replace;
    stencil_equal.front.compare_function = 2;  // CompareFunction::kEqual.
    stencil_equal.front.depth_stencil_pass_operation = 0;
    stencil_equal.front.write_mask = 0;
    stencil_equal.back = stencil_equal.front;
    ProbeDepthStencilState stencil_reject = stencil_equal;
    stencil_reject.front.reference = 0x33;
    stencil_reject.back.reference = 0x33;
    bool stencil_cleared =
        depth_write_mask_ok &&
        ClearPipelineProbeContext(context, kWidth, kHeight, 0.0, 0.0, 0.0, 1.0, &error);
    bool stencil_written =
        stencil_cleared && render_depth_quad(kNearDepthPositions, green_texture, stencil_replace);
    bool stencil_matched =
        stencil_written && render_depth_quad(kNearDepthPositions, red_texture, stencil_equal);
    bool stencil_rejected =
        stencil_matched && render_depth_quad(kNearDepthPositions, blue_texture, stencil_reject);
    std::vector<uint8_t> stencil_bgra;
    const uint8_t* stencil_center = stencil_rejected ? read_center(stencil_bgra) : nullptr;
    bool stencil_persistence_ok = stencil_center && PixelNear(stencil_center, kRedBgra);
    bool depth_stencil_ok = depth_persistence_ok && depth_write_mask_ok && stencil_persistence_ok;

    // Host render targets use private Metal storage. Their regional readback
    // must enqueue the blit behind pending draws and fence both with one wait.
    void* private_context = CreateHostRenderTargetContext(device, &error);
    bool private_rendered =
        private_context &&
        render_indexed_fan_to_context(private_context, pipeline_state, &texture, kWidth, kHeight);
    uint32_t private_pending_before =
        GetPipelineProbeContextPendingSubmissionCount(private_context);
    std::vector<uint8_t> private_region_bgra;
    bool private_region_read =
        private_rendered && ReadPipelineProbeContextRect(private_context, kWidth, kHeight, 18, 18,
                                                         4, 4, private_region_bgra, &error);
    uint32_t private_pending_after = GetPipelineProbeContextPendingSubmissionCount(private_context);
    bool private_region_ok = private_region_read && private_pending_before == 1 &&
                             private_pending_after == 0 && private_region_bgra.size() == 4 * 4 * 4;
    for (size_t pixel = 0; pixel < 16 && private_region_ok; ++pixel) {
      private_region_ok = PixelNear(private_region_bgra.data() + pixel * 4, kExpectedBgra);
    }

    // Resolve the private render target directly into an external shared Metal
    // buffer. Compare the entire buffer, not just written texels, so source and
    // destination origins, Xenos tiled addressing, every endian mapping, and
    // untouched-byte preservation are all checked together.
    constexpr uint32_t kTiledPitch = 64;
    constexpr uint32_t kTiledHeight = 64;
    constexpr size_t kTiledBufferSize = 32 * 1024;
    constexpr size_t kFullTiledBufferOffset = 512;
    constexpr size_t kPartialTiledBufferOffset = 256;
    id<MTLBuffer> tiled_buffer = [device newBufferWithLength:kTiledBufferSize
                                                     options:MTLResourceStorageModeShared];
    bool tiled_rendered =
        private_region_ok && tiled_buffer &&
        render_indexed_fan_to_context(private_context, pipeline_state, &texture, kWidth, kHeight);
    uint32_t tiled_pending_before = GetPipelineProbeContextPendingSubmissionCount(private_context);
    uint32_t tiled_pending_after_first = UINT32_MAX;
    uint32_t tiled_case_count = 0;
    bool tiled_resolve_ok = tiled_rendered && tiled_pending_before == 1;
    for (uint32_t endian = 0; endian < 4 && tiled_resolve_ok; ++endian) {
      uint8_t full_sentinel = uint8_t(0xC0 + endian);
      std::memset([tiled_buffer contents], full_sentinel, kTiledBufferSize);
      ProbeTiledResolveTarget full_target;
      full_target.metal_buffer = tiled_buffer;
      full_target.buffer_offset = kFullTiledBufferOffset;
      full_target.pitch = kTiledPitch;
      full_target.height = kTiledHeight;
      full_target.endian = endian;
      std::vector<uint8_t> full_bgra;
      bool full_resolved = ResolvePipelineProbeContextToXenosTiled(
          private_context, kWidth, kHeight, 0, 0, kWidth, kHeight, full_target, &full_bgra, &error);
      if (endian == 0) {
        tiled_pending_after_first = GetPipelineProbeContextPendingSubmissionCount(private_context);
      }
      std::vector<uint8_t> full_expected(kTiledBufferSize, full_sentinel);
      bool full_expected_valid =
          full_resolved && full_bgra == indexed_bgra &&
          ApplyExpectedTiledRect(full_expected, kFullTiledBufferOffset, kTiledPitch, 0, 0,
                                 full_bgra.data(), kWidth, kWidth, kHeight, endian);
      bool full_matches =
          full_expected_valid &&
          std::memcmp([tiled_buffer contents], full_expected.data(), kTiledBufferSize) == 0;

      uint8_t partial_sentinel = uint8_t(0xA0 + endian);
      std::memset([tiled_buffer contents], partial_sentinel, kTiledBufferSize);
      ProbeTiledResolveTarget partial_target;
      partial_target.metal_buffer = tiled_buffer;
      partial_target.buffer_offset = kPartialTiledBufferOffset;
      partial_target.pitch = kTiledPitch;
      partial_target.height = kTiledHeight;
      partial_target.x = 7;
      partial_target.y = 9;
      partial_target.endian = endian;
      bool partial_resolved = ResolvePipelineProbeContextToXenosTiled(
          private_context, kWidth, kHeight, 18, 18, 4, 4, partial_target,
          /*bgra_out=*/nullptr, &error);
      std::vector<uint8_t> partial_expected(kTiledBufferSize, partial_sentinel);
      const uint8_t* partial_source = indexed_bgra.data() + (size_t(18) * kWidth + 18) * 4;
      bool partial_expected_valid =
          partial_resolved &&
          ApplyExpectedTiledRect(partial_expected, kPartialTiledBufferOffset, kTiledPitch, 7, 9,
                                 partial_source, kWidth, 4, 4, endian);
      bool partial_matches =
          partial_expected_valid &&
          std::memcmp([tiled_buffer contents], partial_expected.data(), kTiledBufferSize) == 0;
      tiled_resolve_ok = full_matches && partial_matches;
      tiled_case_count += full_matches ? 1 : 0;
      tiled_case_count += partial_matches ? 1 : 0;
    }
    tiled_resolve_ok = tiled_resolve_ok && tiled_pending_after_first == 0 && tiled_case_count == 8;

    // Invalid metadata is still a synchronization boundary: pending render
    // work must retire without changing the destination or returning stale CPU
    // output.
    bool invalid_tiled_rendered =
        tiled_resolve_ok &&
        render_indexed_fan_to_context(private_context, pipeline_state, &texture, kWidth, kHeight);
    uint32_t invalid_tiled_pending_before =
        GetPipelineProbeContextPendingSubmissionCount(private_context);
    constexpr uint8_t kInvalidTiledSentinel = 0x7D;
    if (tiled_buffer) {
      std::memset([tiled_buffer contents], kInvalidTiledSentinel, kTiledBufferSize);
    }
    ProbeTiledResolveTarget invalid_target;
    invalid_target.metal_buffer = tiled_buffer;
    invalid_target.buffer_offset = kPartialTiledBufferOffset;
    invalid_target.pitch = kTiledPitch;
    invalid_target.height = kTiledHeight;
    invalid_target.x = kTiledPitch - 1;
    std::vector<uint8_t> invalid_tiled_bgra(16, 0xFF);
    std::string invalid_tiled_error;
    bool invalid_tiled_rejected =
        invalid_tiled_rendered && !ResolvePipelineProbeContextToXenosTiled(
                                      private_context, kWidth, kHeight, 18, 18, 4, 4,
                                      invalid_target, &invalid_tiled_bgra, &invalid_tiled_error);
    uint32_t invalid_tiled_pending_after =
        GetPipelineProbeContextPendingSubmissionCount(private_context);
    const uint8_t* invalid_tiled_bytes =
        tiled_buffer ? static_cast<const uint8_t*>([tiled_buffer contents]) : nullptr;
    bool invalid_tiled_unchanged = invalid_tiled_bytes != nullptr;
    for (size_t i = 0; i < kTiledBufferSize && invalid_tiled_unchanged; ++i) {
      invalid_tiled_unchanged = invalid_tiled_bytes[i] == kInvalidTiledSentinel;
    }
    bool invalid_tiled_ok = invalid_tiled_rendered && invalid_tiled_pending_before == 1 &&
                            invalid_tiled_rejected && invalid_tiled_pending_after == 0 &&
                            invalid_tiled_bgra.empty() && !invalid_tiled_error.empty() &&
                            invalid_tiled_unchanged;
    tiled_resolve_ok = tiled_resolve_ok && invalid_tiled_ok;
    if (tiled_buffer) {
      [tiled_buffer release];
    }
    ReleasePipelineProbeContext(private_context);
    if (!private_region_ok || !tiled_resolve_ok) {
      std::fprintf(stderr,
                   "[metal_pipeline_probe_test] FAIL: private regional readback rendered=%d "
                   "pending=%u read=%d pending_after=%u bytes=%zu; "
                   "tiled=(rendered=%d pending=%u pending_after=%u cases=%u) "
                   "invalid=(rendered=%d pending=%u rejected=%d pending_after=%u bytes=%zu "
                   "unchanged=%d error=%s): %s\n",
                   int(private_rendered), private_pending_before, int(private_region_read),
                   private_pending_after, private_region_bgra.size(), int(tiled_rendered),
                   tiled_pending_before, tiled_pending_after_first, tiled_case_count,
                   int(invalid_tiled_rendered), invalid_tiled_pending_before,
                   int(invalid_tiled_rejected), invalid_tiled_pending_after,
                   invalid_tiled_bgra.size(), int(invalid_tiled_unchanged),
                   invalid_tiled_error.c_str(), error.c_str());
      [fan_metal_index_buffer release];
      [fan_position_buffer release];
      ReleasePipelineProbeContext(context);
      release_pipeline_states();
      return 1;
    }

    // Blend a half-transparent source over a nontrivial destination. Using the
    // same factors for color and alpha exercises every field in 0x07060706.
    constexpr std::array<uint8_t, 4> kBlendSourceRgba = {224, 128, 32, 128};
    constexpr std::array<uint8_t, 4> kBlendDestinationRgba = {32, 64, 128, 192};
    // round(src * 128 / 255 + dst * 127 / 255), in BGRA order.
    constexpr std::array<uint8_t, 4> kExpectedBlendedBgra = {80, 96, 128, 160};
    ProbeTextureSlot blend_texture;
    blend_texture.rgba = kBlendSourceRgba.data();
    blend_texture.width = 1;
    blend_texture.height = 1;
    blend_texture.array_length = 1;
    blend_texture.bytes_per_row = kBlendSourceRgba.size();
    blend_texture.bytes_per_image = kBlendSourceRgba.size();
    ProbeTextureSlot blend_destination_texture;
    blend_destination_texture.rgba = kBlendDestinationRgba.data();
    blend_destination_texture.width = 1;
    blend_destination_texture.height = 1;
    blend_destination_texture.array_length = 1;
    blend_destination_texture.bytes_per_row = kBlendDestinationRgba.size();
    blend_destination_texture.bytes_per_image = kBlendDestinationRgba.size();
    // Submit two draws without a CPU wait between them. Both are encoded in the
    // same render pass, and the second draw must blend over the first's output.
    bool blend_cleared =
        ClearPipelineProbeContext(context, kWidth, kHeight, 0.0, 0.0, 0.0, 0.0, &error);
    bool blend_destination_rendered =
        blend_cleared && render_indexed_fan(pipeline_state, &blend_destination_texture);
    bool blend_rendered = blend_destination_rendered &&
                          render_indexed_fan(alpha_blend_pipeline_state, &blend_texture);
    uint32_t blend_pending_before_read = GetPipelineProbeContextPendingSubmissionCount(context);
    std::vector<uint8_t> blend_bgra;
    bool blend_read =
        blend_rendered && ReadPipelineProbeContext(context, kWidth, kHeight, blend_bgra, &error);
    uint32_t blend_pending_after_read = GetPipelineProbeContextPendingSubmissionCount(context);
    const uint8_t* blend_center =
        blend_read ? blend_bgra.data() + (size_t(kHeight / 2) * kWidth + kWidth / 2) * 4 : nullptr;
    const uint8_t* blend_corner = blend_read ? blend_bgra.data() : nullptr;
    bool blend_ok = blend_center && blend_corner &&
                    PixelNear(blend_center, kExpectedBlendedBgra, 2) &&
                    PixelNear(blend_corner, std::array<uint8_t, 4>{0, 0, 0, 0}) &&
                    blend_pending_before_read == 2 && blend_pending_after_read == 0;

    // Opaque replacement with only Xenos R and B enabled. G and A must retain
    // the clear value; this specifically catches a direct (unreversed) mapping
    // from Xenos mask bits to Metal's opposite-order MTLColorWriteMask bits.
    constexpr std::array<uint8_t, 4> kMaskSourceRgba = {201, 151, 101, 51};
    constexpr std::array<uint8_t, 4> kMaskDestinationRgba = {17, 71, 113, 199};
    constexpr std::array<uint8_t, 4> kMaskDestinationBgra = {113, 71, 17, 199};
    constexpr std::array<uint8_t, 4> kExpectedMaskedBgra = {101, 71, 201, 199};
    ProbeTextureSlot mask_texture;
    mask_texture.rgba = kMaskSourceRgba.data();
    mask_texture.width = 1;
    mask_texture.height = 1;
    mask_texture.array_length = 1;
    mask_texture.bytes_per_row = kMaskSourceRgba.size();
    mask_texture.bytes_per_image = kMaskSourceRgba.size();
    bool mask_cleared = ClearPipelineProbeContext(
        context, kWidth, kHeight, double(kMaskDestinationRgba[0]) / 255.0,
        double(kMaskDestinationRgba[1]) / 255.0, double(kMaskDestinationRgba[2]) / 255.0,
        double(kMaskDestinationRgba[3]) / 255.0, &error);
    bool mask_rendered =
        mask_cleared && render_indexed_fan(partial_write_pipeline_state, &mask_texture);
    std::vector<uint8_t> mask_bgra;
    bool mask_read =
        mask_rendered && ReadPipelineProbeContext(context, kWidth, kHeight, mask_bgra, &error);
    const uint8_t* mask_center =
        mask_read ? mask_bgra.data() + (size_t(kHeight / 2) * kWidth + kWidth / 2) * 4 : nullptr;
    const uint8_t* mask_corner = mask_read ? mask_bgra.data() : nullptr;
    bool mask_ok = mask_center && mask_corner && PixelNear(mask_center, kExpectedMaskedBgra) &&
                   PixelNear(mask_corner, kMaskDestinationBgra);

    // Clear is an explicit synchronization boundary, even when the previous
    // render is still pending.
    bool clear_drain_rendered = render_indexed_fan(pipeline_state, &texture);
    uint32_t clear_pending_before = GetPipelineProbeContextPendingSubmissionCount(context);
    bool clear_drain_cleared =
        clear_drain_rendered &&
        ClearPipelineProbeContext(context, kWidth, kHeight, 0.0, 0.0, 0.0, 1.0, &error);
    uint32_t clear_pending_after = GetPipelineProbeContextPendingSubmissionCount(context);
    bool clear_drain_ok = clear_drain_rendered && clear_pending_before == 1 &&
                          clear_drain_cleared && clear_pending_after == 0;

    // Draw 64 closes the first shared encoder / command buffer without losing
    // submission metadata. Four committed 64-draw buffers (256 draws total)
    // recycle the oldest completed upload arena while leaving newer buffers in
    // flight. Positions and indices are supplied as CPU
    // data here, exercising three upload-arena suballocations per draw (system
    // constants, vertices, indices). Each command buffer draws a different
    // quadrant so recycling an arena before GPU completion is visible.
    constexpr std::array<std::array<float, 8>, 4> kUploadQuadrants = {{
        {-0.9f, -0.9f, -0.1f, -0.9f, -0.1f, -0.1f, -0.9f, -0.1f},
        {0.1f, -0.9f, 0.9f, -0.9f, 0.9f, -0.1f, 0.1f, -0.1f},
        {-0.9f, 0.1f, -0.1f, 0.1f, -0.1f, 0.9f, -0.9f, 0.9f},
        {0.1f, 0.1f, 0.9f, 0.1f, 0.9f, 0.9f, 0.1f, 0.9f},
    }};
    PipelineProbeUploadStats upload_stats_before;
    bool upload_stats_before_ok = GetPipelineProbeContextUploadStats(context, &upload_stats_before);
    bool cap_submissions_ok = clear_drain_ok && upload_stats_before_ok;
    for (uint32_t i = 0; i < 63 && cap_submissions_ok; ++i) {
      cap_submissions_ok = render_uploaded_indexed_fan(kUploadQuadrants[0]);
    }
    uint32_t pending_after_63 = GetPipelineProbeContextPendingSubmissionCount(context);
    bool submission_64_ok = cap_submissions_ok && render_uploaded_indexed_fan(kUploadQuadrants[0]);
    uint32_t pending_after_64 = GetPipelineProbeContextPendingSubmissionCount(context);
    bool submission_65_ok = submission_64_ok && render_uploaded_indexed_fan(kUploadQuadrants[1]);
    uint32_t pending_after_65 = GetPipelineProbeContextPendingSubmissionCount(context);
    bool submissions_to_255_ok = submission_65_ok;
    for (uint32_t submitted = 65; submitted < 255 && submissions_to_255_ok; ++submitted) {
      submissions_to_255_ok = render_uploaded_indexed_fan(kUploadQuadrants[submitted / 64]);
    }
    uint32_t pending_after_255 = GetPipelineProbeContextPendingSubmissionCount(context);
    bool submission_256_ok =
        submissions_to_255_ok && render_uploaded_indexed_fan(kUploadQuadrants[3]);
    uint32_t pending_after_256 = GetPipelineProbeContextPendingSubmissionCount(context);
    std::vector<uint8_t> upload_quadrants_bgra;
    bool upload_quadrants_read =
        submission_256_ok &&
        ReadPipelineProbeContext(context, kWidth, kHeight, upload_quadrants_bgra, &error);
    bool upload_quadrants_ok =
        upload_quadrants_read && upload_quadrants_bgra.size() == size_t(kWidth) * kHeight * 4;
    for (uint32_t y : {uint32_t(10), uint32_t(30)}) {
      for (uint32_t x : {uint32_t(10), uint32_t(30)}) {
        upload_quadrants_ok =
            upload_quadrants_ok &&
            PixelNear(upload_quadrants_bgra.data() + (size_t(y) * kWidth + x) * 4, kExpectedBgra);
      }
    }
    bool submission_257_ok = upload_quadrants_ok && render_uploaded_indexed_fan(kFanPositions);
    uint32_t pending_after_257 = GetPipelineProbeContextPendingSubmissionCount(context);
    uint32_t explicitly_waited = UINT32_MAX;
    bool explicit_wait_ok =
        submission_257_ok && WaitPipelineProbeContext(context, &error, &explicitly_waited);
    uint32_t pending_after_explicit_wait = GetPipelineProbeContextPendingSubmissionCount(context);
    PipelineProbeUploadStats upload_stats_after;
    bool upload_stats_after_ok = GetPipelineProbeContextUploadStats(context, &upload_stats_after);
    uint64_t upload_buffer_allocation_delta =
        upload_stats_after.buffer_allocation_count - upload_stats_before.buffer_allocation_count;
    uint64_t upload_suballocation_delta =
        upload_stats_after.suballocation_count - upload_stats_before.suballocation_count;
    bool upload_reuse_ok = upload_stats_after_ok && upload_buffer_allocation_delta <= 4 &&
                           upload_suballocation_delta == 257 * 3;
    bool cap_ok = cap_submissions_ok && pending_after_63 == 63 && submission_64_ok &&
                  pending_after_64 == 64 && submission_65_ok && pending_after_65 == 65 &&
                  submissions_to_255_ok && pending_after_255 == 255 && submission_256_ok &&
                  pending_after_256 == 192 && submission_257_ok && pending_after_257 == 1 &&
                  explicit_wait_ok && explicitly_waited == 1 && pending_after_explicit_wait == 0 &&
                  upload_quadrants_ok && upload_reuse_ok;

    // Read is a fence even when its requested size is wrong and no pixels can
    // be returned. This catches validation-before-drain ordering regressions.
    bool invalid_read_rendered = cap_ok && render_indexed_fan(pipeline_state, &texture);
    uint32_t invalid_read_pending_before = GetPipelineProbeContextPendingSubmissionCount(context);
    std::vector<uint8_t> invalid_read_bgra;
    std::string invalid_read_error;
    bool invalid_read_rejected =
        invalid_read_rendered && !ReadPipelineProbeContext(context, kWidth + 1, kHeight,
                                                           invalid_read_bgra, &invalid_read_error);
    uint32_t invalid_read_pending_after = GetPipelineProbeContextPendingSubmissionCount(context);
    bool invalid_read_fence_ok = invalid_read_rendered && invalid_read_pending_before == 1 &&
                                 invalid_read_rejected && invalid_read_pending_after == 0 &&
                                 !invalid_read_error.empty();

    // Resizing drains work targeting the old texture before replacing it, then
    // leaves the new-size draw pending until readback.
    constexpr uint32_t kResizedWidth = 32;
    constexpr uint32_t kResizedHeight = 32;
    bool resize_old_rendered =
        invalid_read_fence_ok && render_indexed_fan(pipeline_state, &texture);
    uint32_t resize_old_pending = GetPipelineProbeContextPendingSubmissionCount(context);
    bool resize_new_rendered =
        resize_old_rendered && render_indexed_fan_to_context(context, pipeline_state, &texture,
                                                             kResizedWidth, kResizedHeight);
    uint32_t resize_new_pending = GetPipelineProbeContextPendingSubmissionCount(context);
    std::vector<uint8_t> resized_bgra;
    bool resize_read =
        resize_new_rendered &&
        ReadPipelineProbeContext(context, kResizedWidth, kResizedHeight, resized_bgra, &error);
    uint32_t resize_pending_after_read = GetPipelineProbeContextPendingSubmissionCount(context);
    const uint8_t* resized_center =
        resize_read ? resized_bgra.data() +
                          (size_t(kResizedHeight / 2) * kResizedWidth + kResizedWidth / 2) * 4
                    : nullptr;
    bool resize_ok = resize_old_rendered && resize_old_pending == 1 && resize_new_rendered &&
                     resize_new_pending == 1 && resize_read && resize_pending_after_read == 0 &&
                     resized_center && PixelNear(resized_center, kExpectedBgra);

    // Release must safely drain without requiring an explicit caller wait.
    std::string release_context_error;
    void* release_context = CreatePipelineProbeContext(device, &release_context_error);
    bool release_rendered =
        release_context &&
        render_indexed_fan_to_context(release_context, pipeline_state, &texture, kWidth, kHeight);
    uint32_t release_pending = GetPipelineProbeContextPendingSubmissionCount(release_context);
    bool release_drain_ok = release_context && release_rendered && release_pending == 1;
    ReleasePipelineProbeContext(release_context);

    std::string null_wait_error;
    uint32_t null_waited = UINT32_MAX;
    bool null_wait_ok = !WaitPipelineProbeContext(nullptr, &null_wait_error, &null_waited) &&
                        null_waited == 0 && !null_wait_error.empty() &&
                        GetPipelineProbeContextPendingSubmissionCount(nullptr) == 0;

    [fan_metal_index_buffer release];
    [fan_position_buffer release];
    ReleasePipelineProbeContext(context);
    release_pipeline_states();
    if (!indexed_ok) {
      std::fprintf(stderr,
                   "[metal_pipeline_probe_test] FAIL: indexed fan draw/readback: %s "
                   "textured=%zu\n",
                   error.c_str(), indexed_textured_pixels);
      return 1;
    }
    if (!scissored_ok) {
      std::fprintf(stderr, "[metal_pipeline_probe_test] FAIL: scissor delivery: %s\n",
                   error.c_str());
      return 1;
    }
    if (!cull_winding_ok) {
      std::fprintf(stderr,
                   "[metal_pipeline_probe_test] FAIL: cull/front-face delivery: %s "
                   "back_ccw=%d front_ccw=%d back_cw=%d front_cw=%d\n",
                   error.c_str(), int(cull_back_ccw_ok), int(cull_front_ccw_ok),
                   int(cull_back_cw_ok), int(cull_front_cw_ok));
      return 1;
    }
    if (!depth_stencil_ok) {
      std::fprintf(
          stderr,
          "[metal_pipeline_probe_test] FAIL: persistent depth/stencil: %s "
          "depth=(clear=%d near=%d far=%d center=%u,%u,%u,%u) "
          "no_write=(clear=%d near=%d far=%d center=%u,%u,%u,%u) "
          "stencil=(clear=%d write=%d match=%d reject=%d center=%u,%u,%u,%u)\n",
          error.c_str(), int(depth_cleared), int(depth_near_rendered), int(depth_far_rendered),
          depth_center ? depth_center[0] : 0, depth_center ? depth_center[1] : 0,
          depth_center ? depth_center[2] : 0, depth_center ? depth_center[3] : 0,
          int(no_write_cleared), int(no_write_near_rendered), int(no_write_far_rendered),
          no_write_center ? no_write_center[0] : 0, no_write_center ? no_write_center[1] : 0,
          no_write_center ? no_write_center[2] : 0, no_write_center ? no_write_center[3] : 0,
          int(stencil_cleared), int(stencil_written), int(stencil_matched), int(stencil_rejected),
          stencil_center ? stencil_center[0] : 0, stencil_center ? stencil_center[1] : 0,
          stencil_center ? stencil_center[2] : 0, stencil_center ? stencil_center[3] : 0);
      return 1;
    }
    if (!blend_ok) {
      std::fprintf(stderr,
                   "[metal_pipeline_probe_test] FAIL: source-alpha blend: %s "
                   "clear=%d destination=%d draw=%d pending=%u read=%d pending_after=%u "
                   "center=%u,%u,%u,%u corner=%u,%u,%u,%u\n",
                   error.c_str(), int(blend_cleared), int(blend_destination_rendered),
                   int(blend_rendered), blend_pending_before_read, int(blend_read),
                   blend_pending_after_read, blend_center ? blend_center[0] : 0,
                   blend_center ? blend_center[1] : 0, blend_center ? blend_center[2] : 0,
                   blend_center ? blend_center[3] : 0, blend_corner ? blend_corner[0] : 0,
                   blend_corner ? blend_corner[1] : 0, blend_corner ? blend_corner[2] : 0,
                   blend_corner ? blend_corner[3] : 0);
      return 1;
    }
    if (!mask_ok) {
      std::fprintf(stderr,
                   "[metal_pipeline_probe_test] FAIL: partial color write mask: %s "
                   "clear=%d draw=%d read=%d center=%u,%u,%u,%u corner=%u,%u,%u,%u\n",
                   error.c_str(), int(mask_cleared), int(mask_rendered), int(mask_read),
                   mask_center ? mask_center[0] : 0, mask_center ? mask_center[1] : 0,
                   mask_center ? mask_center[2] : 0, mask_center ? mask_center[3] : 0,
                   mask_corner ? mask_corner[0] : 0, mask_corner ? mask_corner[1] : 0,
                   mask_corner ? mask_corner[2] : 0, mask_corner ? mask_corner[3] : 0);
      return 1;
    }
    if (!clear_drain_ok || !cap_ok || !invalid_read_fence_ok || !resize_ok || !release_drain_ok ||
        !null_wait_ok) {
      std::fprintf(
          stderr,
          "[metal_pipeline_probe_test] FAIL: async lifecycle: %s "
          "clear=(%d,%u,%d,%u) batch=(%d,%u,%d,%u,%d,%u) "
          "cap=(%d,%u,%d,%u,%d,%u,%d,%u,%u) "
          "upload=(quadrants=%d reuse=%d buffers=%llu suballocations=%llu) "
          "invalid_read=(%d,%u,%d,%u,%s) "
          "resize=(%d,%u,%d,%u,%d,%u) release=(%d,%u,%s) null_wait=(%d,%u,%s)\n",
          error.c_str(), int(clear_drain_rendered), clear_pending_before, int(clear_drain_cleared),
          clear_pending_after, int(cap_submissions_ok), pending_after_63, int(submission_64_ok),
          pending_after_64, int(submission_65_ok), pending_after_65, int(submissions_to_255_ok),
          pending_after_255, int(submission_256_ok), pending_after_256, int(submission_257_ok),
          pending_after_257, int(explicit_wait_ok), explicitly_waited, pending_after_explicit_wait,
          int(upload_quadrants_ok), int(upload_reuse_ok),
          static_cast<unsigned long long>(upload_buffer_allocation_delta),
          static_cast<unsigned long long>(upload_suballocation_delta), int(invalid_read_rendered),
          invalid_read_pending_before, int(invalid_read_rejected), invalid_read_pending_after,
          invalid_read_error.c_str(), int(resize_old_rendered), resize_old_pending,
          int(resize_new_rendered), resize_new_pending, int(resize_read), resize_pending_after_read,
          int(release_rendered), release_pending, release_context_error.c_str(), int(null_wait_ok),
          null_waited, null_wait_error.c_str());
      return 1;
    }

    std::fprintf(stdout,
                 "[metal_pipeline_probe_test] PASS: external MTLBuffer rasterized %zu sentinel "
                 "pixels; indexed fan remap rasterized %zu; scissor clipped its right half "
                 "(%zu clear); all four cull/winding combinations plus persistent depth, depth "
                 "write masking, and stencil replace/equal/reject matched; ordered multi-draw "
                 "batch, R/B color write mask, 64-draw command buffers, 256-draw oldest-buffer "
                 "retirement, four upload-arena lifetimes, %llu reusable buffers for %llu "
                 "suballocations, resize, "
                 "explicit wait, and release drains matched; %u GPU tiled resolve cases plus "
                 "invalid-input fencing matched\n",
                 textured_pixels, indexed_textured_pixels, clear_pixels,
                 static_cast<unsigned long long>(upload_buffer_allocation_delta),
                 static_cast<unsigned long long>(upload_suballocation_delta), tiled_case_count);
    return 0;
  }
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
  return RunPipelineProbeTest();
}
