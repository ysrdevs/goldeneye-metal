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
#include <string>
#include <vector>

#include <rex/graphics/metal/msl_compiler.h>

namespace {

using rex::graphics::metal::CreateMslLibrary;
using rex::graphics::metal::CreatePipelineProbeContext;
using rex::graphics::metal::CreateRenderPipelineState;
using rex::graphics::metal::ProbeIndexBuffer;
using rex::graphics::metal::ProbeRasterizationState;
using rex::graphics::metal::ProbeSamplerSlot;
using rex::graphics::metal::ProbeTextureSlot;
using rex::graphics::metal::ReadPipelineProbeContext;
using rex::graphics::metal::ReleaseMslLibrary;
using rex::graphics::metal::ReleasePipelineProbeContext;
using rex::graphics::metal::ReleaseRenderPipelineState;
using rex::graphics::metal::RenderPipelineProbeToContext;
using rex::graphics::metal::ResetPipelineProbeContext;

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

bool PixelNear(const uint8_t* bgra, const std::array<uint8_t, 4>& expected, uint8_t tolerance = 1) {
  for (size_t i = 0; i < expected.size(); ++i) {
    int difference = int(bgra[i]) - int(expected[i]);
    if (difference < -int(tolerance) || difference > int(tolerance)) {
      return false;
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
    ReleaseMslLibrary(vertex_library);
    ReleaseMslLibrary(fragment_library);
    if (!pipeline_state) {
      std::fprintf(stderr, "[metal_pipeline_probe_test] FAIL: render pipeline: %s\n",
                   error.c_str());
      return 1;
    }

    void* context = CreatePipelineProbeContext(device, &error);
    if (!context) {
      std::fprintf(stderr, "[metal_pipeline_probe_test] FAIL: probe context: %s\n", error.c_str());
      ReleaseRenderPipelineState(pipeline_state);
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
      ReleaseRenderPipelineState(pipeline_state);
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

    std::vector<uint8_t> bgra;
    bool read = rendered && ReadPipelineProbeContext(context, kWidth, kHeight, bgra, &error);

    [position_buffer release];

    if (!rendered || !read) {
      std::fprintf(stderr, "[metal_pipeline_probe_test] FAIL: probe draw/readback: %s\n",
                   error.c_str());
      ReleasePipelineProbeContext(context);
      ReleaseRenderPipelineState(pipeline_state);
      return 1;
    }
    if (bgra.size() != size_t(kWidth) * kHeight * 4) {
      std::fprintf(stderr, "[metal_pipeline_probe_test] FAIL: readback size %zu, expected %zu\n",
                   bgra.size(), size_t(kWidth) * kHeight * 4);
      ReleasePipelineProbeContext(context);
      ReleaseRenderPipelineState(pipeline_state);
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
      ReleaseRenderPipelineState(pipeline_state);
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
      ReleaseRenderPipelineState(pipeline_state);
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
      ReleaseRenderPipelineState(pipeline_state);
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
    [fan_metal_index_buffer release];
    [fan_position_buffer release];
    ReleasePipelineProbeContext(context);
    ReleaseRenderPipelineState(pipeline_state);
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

    std::fprintf(stdout,
                 "[metal_pipeline_probe_test] PASS: external MTLBuffer rasterized %zu sentinel "
                 "pixels; indexed fan remap rasterized %zu; scissor clipped its right half "
                 "(%zu clear)\n",
                 textured_pixels, indexed_textured_pixels, clear_pixels);
    return 0;
  }
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
  return RunPipelineProbeTest();
}
