// Runtime capability and GPU-output gate for the MetalFX spatial scaler used
// by MetalPresenter. Unsupported devices skip cleanly because the application
// falls back to the Sharp presentation filter in the same situation.

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr uint32_t kInputWidth = 32;
constexpr uint32_t kInputHeight = 18;
constexpr uint32_t kOutputWidth = 64;
constexpr uint32_t kOutputHeight = 36;

size_t Align(size_t value, size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

std::vector<uint8_t> MakeFixture() {
  std::vector<uint8_t> fixture(size_t(kInputWidth) * kInputHeight * 4);
  for (uint32_t y = 0; y < kInputHeight; ++y) {
    for (uint32_t x = 0; x < kInputWidth; ++x) {
      size_t offset = (size_t(y) * kInputWidth + x) * 4;
      bool checker = ((x / 4) ^ (y / 3)) & 1;
      fixture[offset + 0] = checker ? 28 : uint8_t(90 + x * 3);
      fixture[offset + 1] = checker ? uint8_t(40 + y * 5) : 210;
      fixture[offset + 2] = checker ? 236 : uint8_t(20 + x * 2);
      fixture[offset + 3] = 255;
    }
  }
  return fixture;
}

}  // namespace

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {
      std::fprintf(stderr, "[metal_metalfx_scaler_test] FAIL: no Metal device\n");
      return 1;
    }
    if (@available(macOS 13.0, *)) {
      if (![MTLFXSpatialScalerDescriptor supportsDevice:device]) {
        std::fprintf(stdout,
                     "[metal_metalfx_scaler_test] SKIP: MetalFX spatial scaling "
                     "unsupported on %s\n",
                     device.name.UTF8String);
        return 0;
      }

      MTLFXSpatialScalerDescriptor* descriptor =
          [[MTLFXSpatialScalerDescriptor alloc] init];
      descriptor.colorTextureFormat = MTLPixelFormatBGRA8Unorm;
      descriptor.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
      descriptor.inputWidth = kInputWidth;
      descriptor.inputHeight = kInputHeight;
      descriptor.outputWidth = kOutputWidth;
      descriptor.outputHeight = kOutputHeight;
      descriptor.colorProcessingMode =
          MTLFXSpatialScalerColorProcessingModePerceptual;
      id<MTLFXSpatialScaler> scaler =
          [descriptor newSpatialScalerWithDevice:device];
      if (!scaler) {
        std::fprintf(stderr,
                     "[metal_metalfx_scaler_test] FAIL: scaler allocation\n");
        return 1;
      }

      MTLTextureDescriptor* input_descriptor =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                             width:kInputWidth
                                                            height:kInputHeight
                                                         mipmapped:NO];
      input_descriptor.storageMode = MTLStorageModeShared;
      input_descriptor.usage = MTLTextureUsageShaderRead | scaler.colorTextureUsage;
      id<MTLTexture> input_texture =
          [device newTextureWithDescriptor:input_descriptor];

      MTLTextureDescriptor* output_descriptor =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                             width:kOutputWidth
                                                            height:kOutputHeight
                                                         mipmapped:NO];
      output_descriptor.storageMode = MTLStorageModePrivate;
      output_descriptor.usage = MTLTextureUsageShaderRead | scaler.outputTextureUsage;
      id<MTLTexture> output_texture =
          [device newTextureWithDescriptor:output_descriptor];
      if (!input_texture || !output_texture) {
        std::fprintf(stderr,
                     "[metal_metalfx_scaler_test] FAIL: texture allocation\n");
        return 1;
      }

      std::vector<uint8_t> fixture = MakeFixture();
      [input_texture replaceRegion:MTLRegionMake2D(0, 0, kInputWidth, kInputHeight)
                       mipmapLevel:0
                         withBytes:fixture.data()
                       bytesPerRow:size_t(kInputWidth) * 4];
      scaler.inputContentWidth = kInputWidth;
      scaler.inputContentHeight = kInputHeight;
      scaler.colorTexture = input_texture;
      scaler.outputTexture = output_texture;

      id<MTLCommandQueue> queue = [device newCommandQueue];
      id<MTLCommandBuffer> command_buffer = [queue commandBuffer];
      if (!queue || !command_buffer) {
        std::fprintf(stderr,
                     "[metal_metalfx_scaler_test] FAIL: command allocation\n");
        return 1;
      }
      [scaler encodeToCommandBuffer:command_buffer];

      size_t readback_pitch = Align(size_t(kOutputWidth) * 4, 256);
      id<MTLBuffer> readback =
          [device newBufferWithLength:readback_pitch * kOutputHeight
                              options:MTLResourceStorageModeShared];
      id<MTLBlitCommandEncoder> blit = [command_buffer blitCommandEncoder];
      if (!readback || !blit) {
        std::fprintf(stderr,
                     "[metal_metalfx_scaler_test] FAIL: readback allocation\n");
        return 1;
      }
      [blit copyFromTexture:output_texture
                sourceSlice:0
                sourceLevel:0
               sourceOrigin:MTLOriginMake(0, 0, 0)
                 sourceSize:MTLSizeMake(kOutputWidth, kOutputHeight, 1)
                   toBuffer:readback
          destinationOffset:0
     destinationBytesPerRow:readback_pitch
   destinationBytesPerImage:readback_pitch * kOutputHeight];
      [blit endEncoding];
      [command_buffer commit];
      [command_buffer waitUntilCompleted];
      if (command_buffer.status != MTLCommandBufferStatusCompleted) {
        std::fprintf(stderr,
                     "[metal_metalfx_scaler_test] FAIL: GPU command: %s\n",
                     command_buffer.error
                         ? command_buffer.error.localizedDescription.UTF8String
                         : "unknown error");
        return 1;
      }

      const uint8_t* pixels = static_cast<const uint8_t*>(readback.contents);
      uint8_t minimum_rgb = 255;
      uint8_t maximum_rgb = 0;
      size_t opaque_pixels = 0;
      for (uint32_t y = 0; y < kOutputHeight; ++y) {
        const uint8_t* row = pixels + size_t(y) * readback_pitch;
        for (uint32_t x = 0; x < kOutputWidth; ++x) {
          const uint8_t* pixel = row + size_t(x) * 4;
          minimum_rgb = std::min(minimum_rgb,
                                 std::min(pixel[0], std::min(pixel[1], pixel[2])));
          maximum_rgb = std::max(maximum_rgb,
                                 std::max(pixel[0], std::max(pixel[1], pixel[2])));
          opaque_pixels += pixel[3] >= 250 ? 1 : 0;
        }
      }
      if (maximum_rgb <= minimum_rgb + 32 ||
          opaque_pixels < size_t(kOutputWidth) * kOutputHeight * 9 / 10) {
        std::fprintf(stderr,
                     "[metal_metalfx_scaler_test] FAIL: invalid output range "
                     "RGB=%u..%u opaque=%zu/%u\n",
                     minimum_rgb, maximum_rgb, opaque_pixels,
                     kOutputWidth * kOutputHeight);
        return 1;
      }
      std::fprintf(stdout,
                   "[metal_metalfx_scaler_test] PASS: %ux%u -> %ux%u on %s\n",
                   kInputWidth, kInputHeight, kOutputWidth, kOutputHeight,
                   device.name.UTF8String);
      return 0;
    }

    std::fprintf(stdout,
                 "[metal_metalfx_scaler_test] SKIP: macOS 13 is required\n");
    return 0;
  }
}
