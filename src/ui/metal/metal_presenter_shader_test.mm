// Runtime shader and GPU-output gate for MetalPresenter's final presentation
// pass. The application compiles this MSL on first use, so a normal C++ build
// alone cannot catch Metal-language or post-process behavior regressions.

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "metal_presenter_shaders.h"

namespace {

constexpr uint32_t kWidth = 24;
constexpr uint32_t kHeight = 24;
constexpr uint32_t kIdentitySwizzle = 0x688;

struct PresentParameters {
  uint32_t fxaa_quality = 0;
  uint32_t postfx_enabled = 0;
  uint32_t output_filter = 0;
  float source_inv_width = 1.0f / float(kWidth);
  float source_inv_height = 1.0f / float(kHeight);
  float brightness = 0.0f;
  float contrast = 1.0f;
  float saturation = 1.0f;
  float vibrance = 0.0f;
  float temperature = 0.0f;
  float gamma = 1.0f;
  float tint_r = 1.0f;
  float tint_g = 1.0f;
  float tint_b = 1.0f;
  float tint_strength = 0.0f;
};
static_assert(sizeof(PresentParameters) == 15 * sizeof(uint32_t));

bool RenderFixture(id<MTLDevice> device, id<MTLCommandQueue> command_queue,
                   id<MTLRenderPipelineState> pipeline,
                   const std::vector<uint8_t>& source_bgra,
                   const PresentParameters& parameters,
                   std::vector<uint8_t>& destination_bgra,
                   const char*& error_out, uint32_t destination_width = kWidth,
                   uint32_t destination_height = kHeight) {
  if (source_bgra.size() != size_t(kWidth) * kHeight * 4) {
    error_out = "invalid source fixture size";
    return false;
  }

  MTLTextureDescriptor* source_descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:kWidth
                                                        height:kHeight
                                                     mipmapped:NO];
  source_descriptor.storageMode = MTLStorageModeShared;
  source_descriptor.usage = MTLTextureUsageShaderRead;
  id<MTLTexture> source_texture =
      [device newTextureWithDescriptor:source_descriptor];
  if (!source_texture) {
    error_out = "source texture allocation";
    return false;
  }
  [source_texture replaceRegion:MTLRegionMake2D(0, 0, kWidth, kHeight)
                    mipmapLevel:0
                      withBytes:source_bgra.data()
                    bytesPerRow:size_t(kWidth) * 4];

  MTLTextureDescriptor* destination_descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:destination_width
                                                        height:destination_height
                                                     mipmapped:NO];
  destination_descriptor.storageMode = MTLStorageModeShared;
  destination_descriptor.usage = MTLTextureUsageRenderTarget;
  id<MTLTexture> destination_texture =
      [device newTextureWithDescriptor:destination_descriptor];
  if (!destination_texture) {
    error_out = "destination texture allocation";
    return false;
  }

  MTLRenderPassDescriptor* pass =
      [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = destination_texture;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.0, 1.0, 1.0);

  id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
  id<MTLRenderCommandEncoder> encoder =
      [command_buffer renderCommandEncoderWithDescriptor:pass];
  if (!command_buffer || !encoder) {
    error_out = "command buffer or encoder allocation";
    return false;
  }
  [encoder setViewport:MTLViewport{0.0, 0.0, double(destination_width),
                                   double(destination_height), 0.0, 1.0}];
  [encoder setRenderPipelineState:pipeline];
  [encoder setFragmentTexture:source_texture atIndex:0];
  uint32_t swizzle = kIdentitySwizzle;
  [encoder setFragmentBytes:&swizzle length:sizeof(swizzle) atIndex:0];
  [encoder setFragmentBytes:&parameters length:sizeof(parameters) atIndex:1];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];
  if (command_buffer.status != MTLCommandBufferStatusCompleted) {
    error_out = command_buffer.error ? command_buffer.error.localizedDescription.UTF8String
                                     : "GPU command did not complete";
    return false;
  }

  destination_bgra.resize(size_t(destination_width) * destination_height * 4);
  [destination_texture getBytes:destination_bgra.data()
                    bytesPerRow:size_t(destination_width) * 4
                     fromRegion:MTLRegionMake2D(0, 0, destination_width,
                                               destination_height)
                    mipmapLevel:0];
  return true;
}

size_t CountChangedRgbPixels(const std::vector<uint8_t>& first,
                             const std::vector<uint8_t>& second,
                             uint8_t tolerance = 1) {
  if (first.size() != second.size()) {
    return SIZE_MAX;
  }
  size_t changed = 0;
  for (size_t offset = 0; offset < first.size(); offset += 4) {
    bool pixel_changed = false;
    for (size_t channel = 0; channel < 3; ++channel) {
      int difference = int(first[offset + channel]) - int(second[offset + channel]);
      pixel_changed |= std::abs(difference) > int(tolerance);
    }
    changed += pixel_changed ? 1 : 0;
  }
  return changed;
}

bool PixelNear(const uint8_t* actual, const std::array<uint8_t, 4>& expected,
               uint8_t tolerance = 2) {
  for (size_t channel = 0; channel < expected.size(); ++channel) {
    if (std::abs(int(actual[channel]) - int(expected[channel])) > int(tolerance)) {
      return false;
    }
  }
  return true;
}

float Mix(float from, float to, float amount) {
  return from + (to - from) * amount;
}

std::array<uint8_t, 4> GradeExpectedBgra(const std::array<uint8_t, 4>& source_bgra,
                                         const PresentParameters& parameters) {
  std::array<float, 3> color = {
      float(source_bgra[2]) / 255.0f,
      float(source_bgra[1]) / 255.0f,
      float(source_bgra[0]) / 255.0f,
  };
  color[0] += parameters.temperature * 0.10f;
  color[2] -= parameters.temperature * 0.10f;
  for (float& channel : color) {
    channel += parameters.brightness;
    channel = (channel - 0.5f) * parameters.contrast + 0.5f;
  }
  const std::array<float, 3> tint = {
      parameters.tint_r, parameters.tint_g, parameters.tint_b};
  for (size_t channel = 0; channel < 3; ++channel) {
    color[channel] = Mix(color[channel], color[channel] * tint[channel],
                         parameters.tint_strength);
  }
  float luma = std::max(color[0], 0.0f) * 0.2126f +
               std::max(color[1], 0.0f) * 0.7152f +
               std::max(color[2], 0.0f) * 0.0722f;
  for (float& channel : color) {
    channel = Mix(luma, channel, parameters.saturation);
  }
  float maximum = std::max(color[0], std::max(color[1], color[2]));
  float minimum = std::min(color[0], std::min(color[1], color[2]));
  float chroma = maximum - minimum;
  float vibrance_amount =
      1.0f + parameters.vibrance * (1.0f - std::clamp(chroma, 0.0f, 1.0f));
  for (float& channel : color) {
    channel = Mix(luma, channel, vibrance_amount);
    channel = std::pow(std::max(channel, 0.0001f), 1.0f / parameters.gamma);
    channel = std::clamp(channel, 0.0f, 1.0f);
  }
  return {
      uint8_t(std::lround(color[2] * 255.0f)),
      uint8_t(std::lround(color[1] * 255.0f)),
      uint8_t(std::lround(color[0] * 255.0f)),
      255,
  };
}

std::vector<uint8_t> MakeUniformFixture(const std::array<uint8_t, 4>& bgra) {
  std::vector<uint8_t> result(size_t(kWidth) * kHeight * 4);
  for (size_t offset = 0; offset < result.size(); offset += 4) {
    std::copy(bgra.begin(), bgra.end(), result.begin() + offset);
  }
  return result;
}

std::vector<uint8_t> MakeDiagonalFixture(uint8_t dark, uint8_t light) {
  std::vector<uint8_t> result(size_t(kWidth) * kHeight * 4);
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      uint8_t value = x > y ? light : dark;
      size_t offset = (size_t(y) * kWidth + x) * 4;
      result[offset + 0] = value;
      result[offset + 1] = value;
      result[offset + 2] = value;
      result[offset + 3] = 255;
    }
  }
  return result;
}

std::vector<uint8_t> MakeCheckerFixture(uint8_t dark, uint8_t light) {
  std::vector<uint8_t> result(size_t(kWidth) * kHeight * 4);
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      uint8_t value = ((x ^ y) & 1) ? light : dark;
      size_t offset = (size_t(y) * kWidth + x) * 4;
      result[offset + 0] = value;
      result[offset + 1] = value;
      result[offset + 2] = value;
      result[offset + 3] = 255;
    }
  }
  return result;
}

int RunTests(id<MTLDevice> device, id<MTLCommandQueue> command_queue,
             id<MTLRenderPipelineState> pipeline) {
  const char* render_error = nullptr;
  constexpr std::array<uint8_t, 4> kUniformBgra = {64, 112, 176, 255};
  std::vector<uint8_t> uniform = MakeUniformFixture(kUniformBgra);
  std::vector<uint8_t> baseline;
  PresentParameters parameters;
  if (!RenderFixture(device, command_queue, pipeline, uniform, parameters,
                     baseline, render_error)) {
    std::fprintf(stderr, "[metal_presenter_shader_test] FAIL: baseline: %s\n",
                 render_error);
    return 1;
  }
  if (!PixelNear(baseline.data() + (size_t(kHeight / 2) * kWidth + kWidth / 2) * 4,
                 kUniformBgra)) {
    std::fprintf(stderr,
                 "[metal_presenter_shader_test] FAIL: no-effect baseline is not identity\n");
    return 1;
  }

  parameters.postfx_enabled = 1;
  std::vector<uint8_t> neutral_grade;
  if (!RenderFixture(device, command_queue, pipeline, uniform, parameters,
                     neutral_grade, render_error) ||
      CountChangedRgbPixels(baseline, neutral_grade) != 0) {
    std::fprintf(stderr,
                 "[metal_presenter_shader_test] FAIL: neutral grade is not identity%s%s\n",
                 render_error ? ": " : "", render_error ? render_error : "");
    return 1;
  }

  parameters.brightness = 0.03f;
  parameters.contrast = 1.12f;
  parameters.saturation = 0.82f;
  parameters.vibrance = 0.28f;
  parameters.temperature = 0.18f;
  parameters.gamma = 0.92f;
  parameters.tint_r = 0.90f;
  parameters.tint_g = 1.00f;
  parameters.tint_b = 0.78f;
  parameters.tint_strength = 0.30f;
  std::vector<uint8_t> representative_grade;
  if (!RenderFixture(device, command_queue, pipeline, uniform, parameters,
                     representative_grade, render_error)) {
    std::fprintf(stderr,
                 "[metal_presenter_shader_test] FAIL: representative grade: %s\n",
                 render_error);
    return 1;
  }
  std::array<uint8_t, 4> expected_grade = GradeExpectedBgra(kUniformBgra, parameters);
  const uint8_t* graded_center =
      representative_grade.data() + (size_t(kHeight / 2) * kWidth + kWidth / 2) * 4;
  if (!PixelNear(graded_center, expected_grade, 3)) {
    std::fprintf(stderr,
                 "[metal_presenter_shader_test] FAIL: representative grade output "
                 "BGRA=(%u,%u,%u,%u), expected approximately (%u,%u,%u,%u)\n",
                 graded_center[0], graded_center[1], graded_center[2], graded_center[3],
                 expected_grade[0], expected_grade[1], expected_grade[2], expected_grade[3]);
    return 1;
  }

  std::vector<uint8_t> high_contrast = MakeDiagonalFixture(8, 247);
  PresentParameters no_fxaa;
  std::vector<uint8_t> high_none;
  if (!RenderFixture(device, command_queue, pipeline, high_contrast, no_fxaa,
                     high_none, render_error)) {
    std::fprintf(stderr, "[metal_presenter_shader_test] FAIL: high edge baseline: %s\n",
                 render_error);
    return 1;
  }
  if (CountChangedRgbPixels(high_contrast, high_none) != 0) {
    std::fprintf(stderr,
                 "[metal_presenter_shader_test] FAIL: no-effect spatial baseline "
                 "is not identity\n");
    return 1;
  }

  constexpr uint32_t kUpscaledWidth = kWidth * 2;
  constexpr uint32_t kUpscaledHeight = kHeight * 2;
  std::vector<uint8_t> checker = MakeCheckerFixture(8, 247);
  PresentParameters bilinear_upscale;
  std::vector<uint8_t> high_bilinear_upscaled;
  PresentParameters sharp_upscale;
  sharp_upscale.output_filter = 1;
  std::vector<uint8_t> high_sharp_upscaled;
  if (!RenderFixture(device, command_queue, pipeline, checker,
                     bilinear_upscale, high_bilinear_upscaled, render_error,
                     kUpscaledWidth, kUpscaledHeight) ||
      !RenderFixture(device, command_queue, pipeline, checker,
                     sharp_upscale, high_sharp_upscaled, render_error,
                     kUpscaledWidth, kUpscaledHeight) ||
      CountChangedRgbPixels(high_bilinear_upscaled, high_sharp_upscaled) == 0) {
    std::fprintf(stderr,
                 "[metal_presenter_shader_test] FAIL: sharp upscale did not differ "
                 "from bilinear%s%s\n",
                 render_error ? ": " : "", render_error ? render_error : "");
    return 1;
  }
  std::vector<uint8_t> uniform_sharp_upscaled;
  if (!RenderFixture(device, command_queue, pipeline, uniform, sharp_upscale,
                     uniform_sharp_upscaled, render_error, kUpscaledWidth,
                     kUpscaledHeight) ||
      !PixelNear(uniform_sharp_upscaled.data() +
                     (size_t(kUpscaledHeight / 2) * kUpscaledWidth +
                      kUpscaledWidth / 2) *
                         4,
                 kUniformBgra)) {
    std::fprintf(stderr,
                 "[metal_presenter_shader_test] FAIL: sharp upscale changed a "
                 "uniform image%s%s\n",
                 render_error ? ": " : "", render_error ? render_error : "");
    return 1;
  }
  PresentParameters standard_fxaa;
  standard_fxaa.fxaa_quality = 1;
  std::vector<uint8_t> high_standard;
  if (!RenderFixture(device, command_queue, pipeline, high_contrast, standard_fxaa,
                     high_standard, render_error) ||
      CountChangedRgbPixels(high_none, high_standard) == 0) {
    std::fprintf(stderr,
                 "[metal_presenter_shader_test] FAIL: standard FXAA did not change "
                 "the diagonal edge%s%s\n",
                 render_error ? ": " : "", render_error ? render_error : "");
    return 1;
  }

  // This contrast is below standard's 1/12 floor and above extreme's 0.0312
  // floor, proving that the two existing quality presets are distinct.
  std::vector<uint8_t> low_contrast = MakeDiagonalFixture(102, 123);
  std::vector<uint8_t> low_standard;
  PresentParameters extreme_fxaa;
  extreme_fxaa.fxaa_quality = 2;
  std::vector<uint8_t> low_extreme;
  if (!RenderFixture(device, command_queue, pipeline, low_contrast, standard_fxaa,
                     low_standard, render_error) ||
      !RenderFixture(device, command_queue, pipeline, low_contrast, extreme_fxaa,
                     low_extreme, render_error) ||
      CountChangedRgbPixels(low_standard, low_extreme) == 0) {
    std::fprintf(stderr,
                 "[metal_presenter_shader_test] FAIL: FXAA Extreme did not differ "
                 "from standard%s%s\n",
                 render_error ? ": " : "", render_error ? render_error : "");
    return 1;
  }

  std::fprintf(stdout,
               "[metal_presenter_shader_test] PASS: identity, neutral/representative "
               "grade, sharp upscale, FXAA and FXAA Extreme GPU fixtures\n");
  return 0;
}

}  // namespace

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      std::fprintf(stderr, "[metal_presenter_shader_test] FAIL: no Metal device\n");
      return 1;
    }

    NSError* library_error = nil;
    NSString* source = [NSString
        stringWithUTF8String:rex::ui::metal::shaders::kGuestPresentMetalSource];
    id<MTLLibrary> library = [device newLibraryWithSource:source
                                                  options:nil
                                                    error:&library_error];
    if (!library) {
      std::fprintf(stderr, "[metal_presenter_shader_test] FAIL: shader compile: %s\n",
                   library_error ? library_error.localizedDescription.UTF8String
                                 : "unknown error");
      return 1;
    }

    id<MTLFunction> vertex_function =
        [library newFunctionWithName:@"guest_present_vs"];
    id<MTLFunction> fragment_function =
        [library newFunctionWithName:@"guest_present_ps"];
    if (!vertex_function || !fragment_function) {
      std::fprintf(stderr,
                   "[metal_presenter_shader_test] FAIL: presentation entry points\n");
      return 1;
    }

    MTLRenderPipelineDescriptor* descriptor =
        [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = vertex_function;
    descriptor.fragmentFunction = fragment_function;
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    NSError* pipeline_error = nil;
    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:descriptor
                                               error:&pipeline_error];
    if (!pipeline) {
      std::fprintf(stderr, "[metal_presenter_shader_test] FAIL: pipeline: %s\n",
                   pipeline_error ? pipeline_error.localizedDescription.UTF8String
                                  : "unknown error");
      return 1;
    }
    id<MTLCommandQueue> command_queue = [device newCommandQueue];
    if (!command_queue) {
      std::fprintf(stderr,
                   "[metal_presenter_shader_test] FAIL: command queue allocation\n");
      return 1;
    }

    int result = RunTests(device, command_queue, pipeline);
    if (result == 0) {
      std::fprintf(stdout,
                   "[metal_presenter_shader_test] device: %s\n",
                   device.name.UTF8String);
    }
    return result;
  }
}
