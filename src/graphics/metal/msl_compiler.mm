#include <rex/graphics/metal/msl_compiler.h>

#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rex::graphics::metal {
namespace {

MTLPrimitiveType ToMetalPrimitiveType(uint32_t primitive_type) {
  switch (primitive_type) {
    case 1:  // PointList
      return MTLPrimitiveTypePoint;
    case 2:  // LineList
      return MTLPrimitiveTypeLine;
    case 3:   // LineStrip
    case 12:  // LineLoop
    case 21:  // 2DLineStrip
      return MTLPrimitiveTypeLineStrip;
    case 6:   // TriangleStrip
    case 22:  // 2DTriStrip
      return MTLPrimitiveTypeTriangleStrip;
    case 4:  // TriangleList
    case 5:  // TriangleFan, expanded later.
    case 8:  // RectangleList, expanded later.
    default:
      return MTLPrimitiveTypeTriangle;
  }
}

bool IsProbeIndexBufferValid(const ProbeIndexBuffer* index_buffer, uint32_t index_count) {
  if (!index_buffer) {
    return true;
  }
  bool has_data = index_buffer->data != nullptr;
  bool has_metal_buffer = index_buffer->metal_buffer != nullptr;
  if (has_data == has_metal_buffer ||
      (index_buffer->index_size != 2 && index_buffer->index_size != 4) ||
      index_buffer->offset % index_buffer->index_size) {
    return false;
  }
  size_t required_size = size_t(index_count) * index_buffer->index_size;
  if (index_buffer->offset > index_buffer->size ||
      required_size > index_buffer->size - index_buffer->offset) {
    return false;
  }
  return !has_metal_buffer ||
         index_buffer->offset + required_size <= [(id<MTLBuffer>)index_buffer->metal_buffer length];
}

bool IsProbeRasterizationStateValid(const ProbeRasterizationState* state, uint32_t width,
                                    uint32_t height) {
  if (!state) {
    return true;
  }
  if (!std::isfinite(state->viewport_x) || !std::isfinite(state->viewport_y) ||
      !std::isfinite(state->viewport_width) || !std::isfinite(state->viewport_height) ||
      !std::isfinite(state->viewport_z_min) || !std::isfinite(state->viewport_z_max) ||
      state->viewport_width <= 0.0 || state->viewport_height <= 0.0 ||
      !std::isfinite(state->viewport_x + state->viewport_width) ||
      !std::isfinite(state->viewport_y + state->viewport_height) || state->viewport_x < 0.0 ||
      state->viewport_y < 0.0 || state->viewport_x > double(width) ||
      state->viewport_y > double(height) ||
      state->viewport_width > double(width) - state->viewport_x ||
      state->viewport_height > double(height) - state->viewport_y || state->viewport_z_min < 0.0 ||
      state->viewport_z_min > 1.0 || state->viewport_z_max < 0.0 || state->viewport_z_max > 1.0 ||
      !state->scissor_width || !state->scissor_height || state->scissor_x > width ||
      state->scissor_y > height || state->scissor_width > width - state->scissor_x ||
      state->scissor_height > height - state->scissor_y || !std::isfinite(state->blend_red) ||
      !std::isfinite(state->blend_green) || !std::isfinite(state->blend_blue) ||
      !std::isfinite(state->blend_alpha)) {
    return false;
  }
  return true;
}

MTLBlendFactor GetMetalBlendFactor(uint32_t factor, bool alpha) {
  switch (factor & 0x1F) {
    case 1:
      return MTLBlendFactorOne;
    case 4:
    case 6:
      return alpha ? MTLBlendFactorSourceAlpha
                   : (factor == 4 ? MTLBlendFactorSourceColor : MTLBlendFactorSourceAlpha);
    case 5:
    case 7:
      return alpha ? MTLBlendFactorOneMinusSourceAlpha
                   : (factor == 5 ? MTLBlendFactorOneMinusSourceColor
                                  : MTLBlendFactorOneMinusSourceAlpha);
    case 8:
    case 10:
      return alpha
                 ? MTLBlendFactorDestinationAlpha
                 : (factor == 8 ? MTLBlendFactorDestinationColor : MTLBlendFactorDestinationAlpha);
    case 9:
    case 11:
      return alpha ? MTLBlendFactorOneMinusDestinationAlpha
                   : (factor == 9 ? MTLBlendFactorOneMinusDestinationColor
                                  : MTLBlendFactorOneMinusDestinationAlpha);
    case 12:
    case 14:
      return alpha || factor == 14 ? MTLBlendFactorBlendAlpha : MTLBlendFactorBlendColor;
    case 13:
    case 15:
      return alpha || factor == 15 ? MTLBlendFactorOneMinusBlendAlpha
                                   : MTLBlendFactorOneMinusBlendColor;
    case 16:
      return MTLBlendFactorSourceAlphaSaturated;
    default:
      return MTLBlendFactorZero;
  }
}

MTLBlendOperation GetMetalBlendOperation(uint32_t operation) {
  switch (operation & 0x7) {
    case 1:
      return MTLBlendOperationSubtract;
    case 2:
      return MTLBlendOperationMin;
    case 3:
      return MTLBlendOperationMax;
    case 4:
      return MTLBlendOperationReverseSubtract;
    default:
      return MTLBlendOperationAdd;
  }
}

id<MTLTexture> CreateProbeTexture(id<MTLDevice> device, const ProbeTextureSlot& slot) {
  if (slot.metal_texture) {
    return [(id<MTLTexture>)slot.metal_texture retain];
  }
  if (!slot.rgba || !slot.width || !slot.height || !slot.bytes_per_row) {
    return nil;
  }
  uint32_t array_length = slot.array_length ? slot.array_length : 1;
  MTLTextureDescriptor* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:slot.width
                                                        height:slot.height
                                                     mipmapped:NO];
  descriptor.textureType = MTLTextureType2DArray;
  descriptor.arrayLength = array_length;
  descriptor.usage = MTLTextureUsageShaderRead;
  descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
  if (!texture) {
    return nil;
  }
  size_t bytes_per_image =
      slot.bytes_per_image ? slot.bytes_per_image : slot.bytes_per_row * size_t(slot.height);
  MTLRegion region = MTLRegionMake2D(0, 0, slot.width, slot.height);
  for (uint32_t slice = 0; slice < array_length; ++slice) {
    [texture replaceRegion:region
               mipmapLevel:0
                     slice:slice
                 withBytes:slot.rgba + bytes_per_image * slice
               bytesPerRow:slot.bytes_per_row
             bytesPerImage:bytes_per_image];
  }
  return texture;
}

void CreateProbeTextures(id<MTLDevice> device, const ProbeTextureSlot* slots, size_t slot_count,
                         id<MTLTexture> fallback_texture,
                         std::vector<id<MTLTexture>>& textures_out) {
  textures_out.clear();
  textures_out.reserve(slot_count);
  for (size_t i = 0; i < slot_count; ++i) {
    id<MTLTexture> texture = slots ? CreateProbeTexture(device, slots[i]) : nil;
    textures_out.push_back(texture ? texture : fallback_texture);
  }
}

void BindProbeTextures(id<MTLRenderCommandEncoder> encoder,
                       const std::vector<id<MTLTexture>>& textures, bool vertex_stage) {
  for (NSUInteger i = 0; i < textures.size(); ++i) {
    if (vertex_stage) {
      [encoder setVertexTexture:textures[i] atIndex:i];
    } else {
      [encoder setFragmentTexture:textures[i] atIndex:i];
    }
  }
}

MTLSamplerAddressMode ToMetalAddressMode(uint8_t address_mode) {
  switch (address_mode) {
    case 0:
      return MTLSamplerAddressModeRepeat;
    case 1:
      return MTLSamplerAddressModeMirrorRepeat;
    case 3:
      return MTLSamplerAddressModeClampToBorderColor;
    case 2:
    default:
      return MTLSamplerAddressModeClampToEdge;
  }
}

id<MTLSamplerState> CreateProbeSampler(id<MTLDevice> device, const ProbeSamplerSlot& slot) {
  MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
  descriptor.minFilter =
      slot.min_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
  descriptor.magFilter =
      slot.mag_linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
  descriptor.mipFilter = slot.mip_linear ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
  descriptor.sAddressMode = ToMetalAddressMode(slot.address_mode_s);
  descriptor.tAddressMode = ToMetalAddressMode(slot.address_mode_t);
  descriptor.rAddressMode = ToMetalAddressMode(slot.address_mode_r);
  descriptor.borderColor = MTLSamplerBorderColorTransparentBlack;
  descriptor.maxAnisotropy = std::max<NSUInteger>(slot.max_anisotropy, 1);
  id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
  [descriptor release];
  return sampler;
}

void CreateProbeSamplers(id<MTLDevice> device, const ProbeSamplerSlot* slots, size_t slot_count,
                         id<MTLSamplerState> fallback_sampler,
                         std::vector<id<MTLSamplerState>>& samplers_out) {
  samplers_out.clear();
  samplers_out.reserve(slot_count);
  for (size_t i = 0; i < slot_count; ++i) {
    id<MTLSamplerState> sampler = slots ? CreateProbeSampler(device, slots[i]) : nil;
    samplers_out.push_back(sampler ? sampler : fallback_sampler);
  }
}

void BindProbeSamplers(id<MTLRenderCommandEncoder> encoder,
                       const std::vector<id<MTLSamplerState>>& samplers, bool vertex_stage) {
  for (NSUInteger i = 0; i < samplers.size(); ++i) {
    if (vertex_stage) {
      [encoder setVertexSamplerState:samplers[i] atIndex:i];
    } else {
      [encoder setFragmentSamplerState:samplers[i] atIndex:i];
    }
  }
}

void ReleaseOwnedProbeSamplers(std::vector<id<MTLSamplerState>>& samplers,
                               id<MTLSamplerState> fallback_sampler) {
  for (id<MTLSamplerState> sampler : samplers) {
    if (sampler && sampler != fallback_sampler) {
      [sampler release];
    }
  }
  samplers.clear();
}

void ReleaseOwnedProbeTextures(std::vector<id<MTLTexture>>& textures,
                               id<MTLTexture> fallback_texture) {
  for (id<MTLTexture> texture : textures) {
    if (texture && texture != fallback_texture) {
      [texture release];
    }
  }
  textures.clear();
}

}  // namespace

void* CreateMslLibrary(void* metal_device, const std::string& source, std::string* error_out) {
  if (!metal_device || source.empty()) {
    if (error_out) {
      *error_out = "missing Metal device or MSL source";
    }
    return nullptr;
  }

  NSError* error = nil;
  NSString* source_string = [NSString stringWithUTF8String:source.c_str()];
  id<MTLLibrary> library = [(id<MTLDevice>)metal_device newLibraryWithSource:source_string
                                                                     options:nil
                                                                       error:&error];
  if (!library) {
    if (error_out) {
      *error_out = error ? [[error localizedDescription] UTF8String] : "unknown Metal error";
    }
    return nullptr;
  }

  return library;
}

void ReleaseMslLibrary(void* metal_library) {
  if (metal_library) {
    [(id)metal_library release];
  }
}

bool ValidateMslSource(void* metal_device, const std::string& source, std::string* error_out) {
  void* library = CreateMslLibrary(metal_device, source, error_out);
  if (!library) {
    return false;
  }
  ReleaseMslLibrary(library);
  return true;
}

void* CreateRenderPipelineState(void* metal_device, void* vertex_library, void* fragment_library,
                                std::string* error_out,
                                const ProbeColorTargetState* color_target_state) {
  if (!metal_device || !vertex_library || !fragment_library) {
    if (error_out) {
      *error_out = "missing Metal device or shader library";
    }
    return nullptr;
  }

  id<MTLFunction> vertex_function = [(id<MTLLibrary>)vertex_library newFunctionWithName:@"main0"];
  id<MTLFunction> fragment_function =
      [(id<MTLLibrary>)fragment_library newFunctionWithName:@"main0"];
  if (!vertex_function || !fragment_function) {
    if (error_out) {
      *error_out = "missing main0 entry point";
    }
    if (vertex_function) {
      [vertex_function release];
    }
    if (fragment_function) {
      [fragment_function release];
    }
    return nullptr;
  }

  MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = vertex_function;
  descriptor.fragmentFunction = fragment_function;
  MTLRenderPipelineColorAttachmentDescriptor* color_attachment = descriptor.colorAttachments[0];
  color_attachment.pixelFormat = MTLPixelFormatBGRA8Unorm;
  if (color_target_state) {
    uint32_t write_mask = color_target_state->write_mask & 0xF;
    MTLColorWriteMask metal_write_mask = MTLColorWriteMaskNone;
    if (write_mask & 0x1) {
      metal_write_mask |= MTLColorWriteMaskRed;
    }
    if (write_mask & 0x2) {
      metal_write_mask |= MTLColorWriteMaskGreen;
    }
    if (write_mask & 0x4) {
      metal_write_mask |= MTLColorWriteMaskBlue;
    }
    if (write_mask & 0x8) {
      metal_write_mask |= MTLColorWriteMaskAlpha;
    }
    color_attachment.writeMask = metal_write_mask;

    uint32_t blend_control = color_target_state->blend_control & 0x1FFF1FFF;
    uint32_t color_source = blend_control & 0x1F;
    uint32_t color_operation = (blend_control >> 5) & 0x7;
    uint32_t color_destination = (blend_control >> 8) & 0x1F;
    uint32_t alpha_source = (blend_control >> 16) & 0x1F;
    uint32_t alpha_operation = (blend_control >> 21) & 0x7;
    uint32_t alpha_destination = (blend_control >> 24) & 0x1F;
    color_attachment.sourceRGBBlendFactor = GetMetalBlendFactor(color_source, false);
    color_attachment.rgbBlendOperation = GetMetalBlendOperation(color_operation);
    color_attachment.destinationRGBBlendFactor = GetMetalBlendFactor(color_destination, false);
    color_attachment.sourceAlphaBlendFactor = GetMetalBlendFactor(alpha_source, true);
    color_attachment.alphaBlendOperation = GetMetalBlendOperation(alpha_operation);
    color_attachment.destinationAlphaBlendFactor = GetMetalBlendFactor(alpha_destination, true);
    color_attachment.blendingEnabled = color_source != 1 || color_operation != 0 ||
                                       color_destination != 0 || alpha_source != 1 ||
                                       alpha_operation != 0 || alpha_destination != 0;
  }

  NSError* error = nil;
  id<MTLRenderPipelineState> pipeline_state =
      [(id<MTLDevice>)metal_device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  [descriptor release];
  [vertex_function release];
  [fragment_function release];

  if (!pipeline_state) {
    if (error_out) {
      *error_out = error ? [[error localizedDescription] UTF8String] : "unknown Metal error";
    }
    return nullptr;
  }
  return pipeline_state;
}

void ReleaseRenderPipelineState(void* pipeline_state) {
  if (pipeline_state) {
    [(id)pipeline_state release];
  }
}

namespace {

struct PipelineProbeContext {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> command_queue = nil;
  id<MTLTexture> render_texture = nil;
  id<MTLRenderPipelineState> clear_pipeline_state = nil;
  MTLStorageMode storage_mode = MTLStorageModeShared;
  uint32_t width = 0;
  uint32_t height = 0;
  bool initialized = false;
};

bool EnsureProbeContextTexture(PipelineProbeContext* context, uint32_t width, uint32_t height,
                               std::string* error_out) {
  if (!context || !context->device || !width || !height) {
    if (error_out) {
      *error_out = "missing probe context, Metal device, or target size";
    }
    return false;
  }
  if (context->render_texture && context->width == width && context->height == height) {
    return true;
  }
  if (context->render_texture) {
    [context->render_texture release];
    context->render_texture = nil;
  }
  MTLTextureDescriptor* texture_descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  texture_descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  texture_descriptor.storageMode = context->storage_mode;
  context->render_texture = [context->device newTextureWithDescriptor:texture_descriptor];
  if (!context->render_texture) {
    context->width = 0;
    context->height = 0;
    context->initialized = false;
    if (error_out) {
      *error_out = "failed to create persistent probe texture";
    }
    return false;
  }
  context->width = width;
  context->height = height;
  context->initialized = false;
  return true;
}

bool EnsureClearPipelineState(PipelineProbeContext* context, std::string* error_out) {
  if (!context || !context->device) {
    if (error_out) {
      *error_out = "missing probe context or Metal device";
    }
    return false;
  }
  if (context->clear_pipeline_state) {
    return true;
  }
  static constexpr char kClearMsl[] = R"(
#include <metal_stdlib>
using namespace metal;

struct ClearConstants {
  float4 color;
};

vertex float4 rex_clear_vertex(uint vertex_id [[vertex_id]]) {
  constexpr float2 positions[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0),
  };
  return float4(positions[vertex_id], 0.0, 1.0);
}

fragment float4 rex_clear_fragment(constant ClearConstants& constants [[buffer(0)]]) {
  return constants.color;
}
)";
  NSError* error = nil;
  id<MTLLibrary> library =
      [context->device newLibraryWithSource:[NSString stringWithUTF8String:kClearMsl]
                                    options:nil
                                      error:&error];
  if (!library) {
    if (error_out) {
      *error_out = error ? [[error localizedDescription] UTF8String] : "clear library failed";
    }
    return false;
  }
  id<MTLFunction> vertex_function = [library newFunctionWithName:@"rex_clear_vertex"];
  id<MTLFunction> fragment_function = [library newFunctionWithName:@"rex_clear_fragment"];
  if (!vertex_function || !fragment_function) {
    if (error_out) {
      *error_out = "clear shader functions not found";
    }
    if (vertex_function) {
      [vertex_function release];
    }
    if (fragment_function) {
      [fragment_function release];
    }
    [library release];
    return false;
  }
  MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = vertex_function;
  descriptor.fragmentFunction = fragment_function;
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  context->clear_pipeline_state = [context->device newRenderPipelineStateWithDescriptor:descriptor
                                                                                  error:&error];
  [descriptor release];
  [vertex_function release];
  [fragment_function release];
  [library release];
  if (!context->clear_pipeline_state) {
    if (error_out) {
      *error_out = error ? [[error localizedDescription] UTF8String] : "clear pipeline failed";
    }
    return false;
  }
  return true;
}

}  // namespace

namespace {

void* CreatePersistentRenderContext(void* metal_device, MTLStorageMode storage_mode,
                                    const char* label, std::string* error_out) {
  if (!metal_device) {
    if (error_out) {
      *error_out = "missing Metal device";
    }
    return nullptr;
  }
  auto* context = new PipelineProbeContext();
  context->device = [(id<MTLDevice>)metal_device retain];
  context->storage_mode = storage_mode;
  context->command_queue = [context->device newCommandQueue];
  if (!context->command_queue) {
    if (error_out) {
      *error_out = std::string("failed to create persistent ") + label + " command queue";
    }
    [context->device release];
    delete context;
    return nullptr;
  }
  return context;
}

}  // namespace

void* CreatePipelineProbeContext(void* metal_device, std::string* error_out) {
  return CreatePersistentRenderContext(metal_device, MTLStorageModeShared, "probe", error_out);
}

void* CreateHostRenderTargetContext(void* metal_device, std::string* error_out) {
  return CreatePersistentRenderContext(metal_device, MTLStorageModePrivate, "host render target",
                                       error_out);
}

void ResetPipelineProbeContext(void* opaque_context) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  if (context) {
    context->initialized = false;
  }
}

void ReleasePipelineProbeContext(void* opaque_context) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  if (!context) {
    return;
  }
  if (context->clear_pipeline_state) {
    [context->clear_pipeline_state release];
  }
  if (context->render_texture) {
    [context->render_texture release];
  }
  if (context->command_queue) {
    [context->command_queue release];
  }
  if (context->device) {
    [context->device release];
  }
  delete context;
}

bool ClearPipelineProbeContext(void* opaque_context, uint32_t width, uint32_t height, double red,
                               double green, double blue, double alpha, std::string* error_out) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  if (!EnsureProbeContextTexture(context, width, height, error_out)) {
    return false;
  }

  id<MTLCommandBuffer> command_buffer = [context->command_queue commandBuffer];
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = context->render_texture;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(red, green, blue, alpha);

  id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];

  bool succeeded = [command_buffer status] != MTLCommandBufferStatusError;
  if (succeeded) {
    context->initialized = true;
  } else if (error_out) {
    NSError* error = [command_buffer error];
    *error_out = error ? [[error localizedDescription] UTF8String] : "command buffer failed";
  }
  return succeeded;
}

bool ClearPipelineProbeContextRect(void* opaque_context, uint32_t width, uint32_t height,
                                   uint32_t x, uint32_t y, uint32_t clear_width,
                                   uint32_t clear_height, double red, double green, double blue,
                                   double alpha, std::string* error_out) {
  if (!clear_width || !clear_height || x >= width || y >= height) {
    return true;
  }
  clear_width = std::min(clear_width, width - x);
  clear_height = std::min(clear_height, height - y);
  if (!x && !y && clear_width == width && clear_height == height) {
    return ClearPipelineProbeContext(opaque_context, width, height, red, green, blue, alpha,
                                     error_out);
  }
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  if (!EnsureProbeContextTexture(context, width, height, error_out) ||
      !EnsureClearPipelineState(context, error_out)) {
    return false;
  }

  float clear_constants[4] = {float(red), float(green), float(blue), float(alpha)};
  id<MTLBuffer> constants_buffer =
      [context->device newBufferWithBytes:clear_constants
                                   length:sizeof(clear_constants)
                                  options:MTLResourceStorageModeShared];
  if (!constants_buffer) {
    if (error_out) {
      *error_out = "failed to create clear constants buffer";
    }
    return false;
  }

  id<MTLCommandBuffer> command_buffer = [context->command_queue commandBuffer];
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = context->render_texture;
  pass.colorAttachments[0].loadAction =
      context->initialized ? MTLLoadActionLoad : MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);

  id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
  MTLScissorRect scissor = {x, y, clear_width, clear_height};
  [encoder setScissorRect:scissor];
  [encoder setRenderPipelineState:context->clear_pipeline_state];
  [encoder setFragmentBuffer:constants_buffer offset:0 atIndex:0];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];

  bool succeeded = [command_buffer status] != MTLCommandBufferStatusError;
  if (succeeded) {
    context->initialized = true;
  } else if (error_out) {
    NSError* error = [command_buffer error];
    *error_out = error ? [[error localizedDescription] UTF8String] : "command buffer failed";
  }
  [constants_buffer release];
  return succeeded;
}

bool RenderPipelineProbeToContext(
    void* opaque_context, void* pipeline_state, const void* system_constants,
    size_t system_constants_size, const void* float_constants, size_t float_constants_size,
    const void* fetch_constants, size_t fetch_constants_size, void* shared_memory,
    size_t shared_memory_size, void* shared_memory_metal_buffer,
    const ProbeTextureSlot* vertex_textures, size_t vertex_texture_count,
    size_t vertex_sampler_count, const ProbeTextureSlot* fragment_textures,
    size_t fragment_texture_count, size_t fragment_sampler_count, uint32_t primitive_type,
    uint32_t vertex_count, uint32_t width, uint32_t height, std::string* error_out,
    uint32_t vertex_shared_memory_buffer_index, uint32_t vertex_float_constants_buffer_index,
    uint32_t vertex_fetch_constants_buffer_index, const void* fragment_float_constants,
    size_t fragment_float_constants_size, uint32_t fragment_float_constants_buffer_index,
    uint32_t fragment_fetch_constants_buffer_index, const ProbeSamplerSlot* vertex_samplers,
    const ProbeSamplerSlot* fragment_samplers, const void* vertex_data, size_t vertex_data_size,
    uint32_t vertex_data_buffer_index, const void* bool_loop_constants,
    size_t bool_loop_constants_size, uint32_t vertex_bool_loop_constants_buffer_index,
    uint32_t fragment_bool_loop_constants_buffer_index, const ProbeIndexBuffer* index_buffer,
    const ProbeRasterizationState* rasterization_state) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  if (!context || !pipeline_state || !system_constants || !system_constants_size || !width ||
      !height || !vertex_count) {
    if (error_out) {
      *error_out = "missing probe context, pipeline state, constants, or target size";
    }
    return false;
  }
  if (!IsProbeIndexBufferValid(index_buffer, vertex_count)) {
    if (error_out) {
      *error_out = "invalid probe index buffer";
    }
    return false;
  }
  if (!IsProbeRasterizationStateValid(rasterization_state, width, height)) {
    if (error_out) {
      *error_out = "invalid probe viewport or scissor";
    }
    return false;
  }
  if (!EnsureProbeContextTexture(context, width, height, error_out)) {
    return false;
  }

  id<MTLDevice> device = context->device;
  id<MTLBuffer> system_buffer = [device newBufferWithBytes:system_constants
                                                    length:system_constants_size
                                                   options:MTLResourceStorageModeShared];
  id<MTLBuffer> float_buffer = nil;
  if (float_constants && float_constants_size) {
    float_buffer = [device newBufferWithBytes:float_constants
                                       length:float_constants_size
                                      options:MTLResourceStorageModeShared];
  }
  id<MTLBuffer> fragment_float_buffer = nil;
  if (fragment_float_constants && fragment_float_constants_size) {
    fragment_float_buffer = [device newBufferWithBytes:fragment_float_constants
                                                length:fragment_float_constants_size
                                               options:MTLResourceStorageModeShared];
  }
  id<MTLBuffer> fetch_buffer = nil;
  if (fetch_constants && fetch_constants_size) {
    fetch_buffer = [device newBufferWithBytes:fetch_constants
                                       length:fetch_constants_size
                                      options:MTLResourceStorageModeShared];
  }
  id<MTLBuffer> bool_loop_buffer = nil;
  if (bool_loop_constants && bool_loop_constants_size) {
    bool_loop_buffer = [device newBufferWithBytes:bool_loop_constants
                                           length:bool_loop_constants_size
                                          options:MTLResourceStorageModeShared];
  }
  id<MTLBuffer> vertex_data_buffer = nil;
  if (vertex_data && vertex_data_size && vertex_data_buffer_index != UINT32_MAX) {
    vertex_data_buffer = [device newBufferWithBytes:vertex_data
                                             length:vertex_data_size
                                            options:MTLResourceStorageModeShared];
  }
  id<MTLBuffer> index_buffer_object = nil;
  if (index_buffer) {
    if (index_buffer->metal_buffer) {
      index_buffer_object = [(id<MTLBuffer>)index_buffer->metal_buffer retain];
    } else {
      index_buffer_object = [device newBufferWithBytes:index_buffer->data
                                                length:index_buffer->size
                                               options:MTLResourceStorageModeShared];
    }
  }
  uint32_t shared_dummy_words[4] = {};
  id<MTLBuffer> shared_memory_buffer = nil;
  if (shared_memory_metal_buffer) {
    shared_memory_buffer = [(id<MTLBuffer>)shared_memory_metal_buffer retain];
  } else if (shared_memory && shared_memory_size) {
    shared_memory_buffer = [device newBufferWithBytesNoCopy:shared_memory
                                                     length:shared_memory_size
                                                    options:MTLResourceStorageModeShared
                                                deallocator:nil];
  }
  if (!shared_memory_buffer && vertex_shared_memory_buffer_index == UINT32_MAX) {
    shared_memory_buffer = [device newBufferWithBytes:shared_dummy_words
                                               length:sizeof(shared_dummy_words)
                                              options:MTLResourceStorageModeShared];
  }
  if (!system_buffer || !shared_memory_buffer || (index_buffer && !index_buffer_object)) {
    if (system_buffer) {
      [system_buffer release];
    }
    if (float_buffer) {
      [float_buffer release];
    }
    if (fragment_float_buffer) {
      [fragment_float_buffer release];
    }
    if (fetch_buffer) {
      [fetch_buffer release];
    }
    if (bool_loop_buffer) {
      [bool_loop_buffer release];
    }
    if (vertex_data_buffer) {
      [vertex_data_buffer release];
    }
    if (index_buffer_object) {
      [index_buffer_object release];
    }
    if (shared_memory_buffer) {
      [shared_memory_buffer release];
    }
    if (error_out) {
      *error_out = index_buffer && !index_buffer_object
                       ? "failed to create persistent probe index buffer"
                       : (vertex_shared_memory_buffer_index != UINT32_MAX && !shared_memory_buffer
                              ? "required persistent shared-memory buffer is unavailable"
                              : "failed to create persistent probe argument buffers");
    }
    return false;
  }

  MTLTextureDescriptor* dummy_texture_descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:1
                                                        height:1
                                                     mipmapped:NO];
  dummy_texture_descriptor.textureType = MTLTextureType2DArray;
  dummy_texture_descriptor.arrayLength = 1;
  dummy_texture_descriptor.usage = MTLTextureUsageShaderRead;
  dummy_texture_descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> dummy_texture = [device newTextureWithDescriptor:dummy_texture_descriptor];
  uint32_t dummy_texture_pixel = 0x00000000u;
  if (dummy_texture) {
    MTLRegion region = MTLRegionMake3D(0, 0, 0, 1, 1, 1);
    [dummy_texture replaceRegion:region
                     mipmapLevel:0
                           slice:0
                       withBytes:&dummy_texture_pixel
                     bytesPerRow:4
                   bytesPerImage:4];
  }
  MTLSamplerDescriptor* sampler_descriptor = [[MTLSamplerDescriptor alloc] init];
  sampler_descriptor.minFilter = MTLSamplerMinMagFilterNearest;
  sampler_descriptor.magFilter = MTLSamplerMinMagFilterNearest;
  sampler_descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
  sampler_descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
  sampler_descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
  id<MTLSamplerState> dummy_sampler = [device newSamplerStateWithDescriptor:sampler_descriptor];
  [sampler_descriptor release];
  std::vector<id<MTLTexture>> vertex_texture_objects;
  std::vector<id<MTLTexture>> fragment_texture_objects;
  std::vector<id<MTLSamplerState>> vertex_sampler_objects;
  std::vector<id<MTLSamplerState>> fragment_sampler_objects;
  CreateProbeTextures(device, vertex_textures, vertex_texture_count, dummy_texture,
                      vertex_texture_objects);
  CreateProbeTextures(device, fragment_textures, fragment_texture_count, dummy_texture,
                      fragment_texture_objects);
  CreateProbeSamplers(device, vertex_samplers, vertex_sampler_count, dummy_sampler,
                      vertex_sampler_objects);
  CreateProbeSamplers(device, fragment_samplers, fragment_sampler_count, dummy_sampler,
                      fragment_sampler_objects);

  id<MTLCommandBuffer> command_buffer = [context->command_queue commandBuffer];
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = context->render_texture;
  pass.colorAttachments[0].loadAction =
      context->initialized ? MTLLoadActionLoad : MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

  id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
  [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline_state];
  if (rasterization_state) {
    MTLViewport viewport = {
        rasterization_state->viewport_x,     rasterization_state->viewport_y,
        rasterization_state->viewport_width, rasterization_state->viewport_height,
        rasterization_state->viewport_z_min, rasterization_state->viewport_z_max};
    [encoder setViewport:viewport];
    MTLScissorRect scissor = {rasterization_state->scissor_x, rasterization_state->scissor_y,
                              rasterization_state->scissor_width,
                              rasterization_state->scissor_height};
    [encoder setScissorRect:scissor];
    [encoder setBlendColorRed:rasterization_state->blend_red
                        green:rasterization_state->blend_green
                         blue:rasterization_state->blend_blue
                        alpha:rasterization_state->blend_alpha];
  }
  [encoder setVertexBuffer:system_buffer offset:0 atIndex:0];
  [encoder setFragmentBuffer:system_buffer offset:0 atIndex:0];
  if (fetch_buffer) {
    if (vertex_fetch_constants_buffer_index != UINT32_MAX) {
      [encoder setVertexBuffer:fetch_buffer offset:0 atIndex:vertex_fetch_constants_buffer_index];
    }
    if (fragment_fetch_constants_buffer_index != UINT32_MAX) {
      [encoder setFragmentBuffer:fetch_buffer
                          offset:0
                         atIndex:fragment_fetch_constants_buffer_index];
    }
  }
  if (bool_loop_buffer) {
    if (vertex_bool_loop_constants_buffer_index != UINT32_MAX) {
      [encoder setVertexBuffer:bool_loop_buffer
                        offset:0
                       atIndex:vertex_bool_loop_constants_buffer_index];
    }
    if (fragment_bool_loop_constants_buffer_index != UINT32_MAX) {
      [encoder setFragmentBuffer:bool_loop_buffer
                          offset:0
                         atIndex:fragment_bool_loop_constants_buffer_index];
    }
  }
  if (float_buffer) {
    if (vertex_float_constants_buffer_index != UINT32_MAX) {
      [encoder setVertexBuffer:float_buffer offset:0 atIndex:vertex_float_constants_buffer_index];
    }
  }
  id<MTLBuffer> fragment_constants_to_bind =
      fragment_float_buffer ? fragment_float_buffer : float_buffer;
  if (fragment_constants_to_bind && fragment_float_constants_buffer_index != UINT32_MAX) {
    [encoder setFragmentBuffer:fragment_constants_to_bind
                        offset:0
                       atIndex:fragment_float_constants_buffer_index];
  }
  if (vertex_shared_memory_buffer_index != UINT32_MAX) {
    [encoder setVertexBuffer:shared_memory_buffer
                      offset:0
                     atIndex:vertex_shared_memory_buffer_index];
  }
  if (vertex_data_buffer && vertex_data_buffer_index != UINT32_MAX) {
    [encoder setVertexBuffer:vertex_data_buffer offset:0 atIndex:vertex_data_buffer_index];
  }
  BindProbeTextures(encoder, vertex_texture_objects, true);
  BindProbeTextures(encoder, fragment_texture_objects, false);
  BindProbeSamplers(encoder, vertex_sampler_objects, true);
  BindProbeSamplers(encoder, fragment_sampler_objects, false);
  if (index_buffer_object) {
    [encoder drawIndexedPrimitives:ToMetalPrimitiveType(primitive_type)
                        indexCount:vertex_count
                         indexType:index_buffer->index_size == 2 ? MTLIndexTypeUInt16
                                                                 : MTLIndexTypeUInt32
                       indexBuffer:index_buffer_object
                 indexBufferOffset:index_buffer->offset];
  } else {
    [encoder drawPrimitives:ToMetalPrimitiveType(primitive_type)
                vertexStart:0
                vertexCount:vertex_count];
  }
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];

  bool succeeded = [command_buffer status] != MTLCommandBufferStatusError;
  if (succeeded) {
    context->initialized = true;
  } else if (error_out) {
    NSError* error = [command_buffer error];
    *error_out = error ? [[error localizedDescription] UTF8String] : "command buffer failed";
  }

  [system_buffer release];
  if (float_buffer) {
    [float_buffer release];
  }
  if (fragment_float_buffer) {
    [fragment_float_buffer release];
  }
  if (fetch_buffer) {
    [fetch_buffer release];
  }
  if (bool_loop_buffer) {
    [bool_loop_buffer release];
  }
  if (vertex_data_buffer) {
    [vertex_data_buffer release];
  }
  if (index_buffer_object) {
    [index_buffer_object release];
  }
  [shared_memory_buffer release];
  ReleaseOwnedProbeTextures(vertex_texture_objects, dummy_texture);
  ReleaseOwnedProbeTextures(fragment_texture_objects, dummy_texture);
  ReleaseOwnedProbeSamplers(vertex_sampler_objects, dummy_sampler);
  ReleaseOwnedProbeSamplers(fragment_sampler_objects, dummy_sampler);
  if (dummy_texture) {
    [dummy_texture release];
  }
  if (dummy_sampler) {
    [dummy_sampler release];
  }
  return succeeded;
}

bool ReadPipelineProbeContext(void* opaque_context, uint32_t width, uint32_t height,
                              std::vector<uint8_t>& bgra_out, std::string* error_out) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  if (!context || !context->render_texture || !context->initialized || !width || !height ||
      context->width != width || context->height != height) {
    if (error_out) {
      *error_out = "persistent probe texture is unavailable or has a different size";
    }
    return false;
  }
  bgra_out.resize(size_t(width) * height * 4);
  if (context->storage_mode == MTLStorageModePrivate) {
    size_t row_pitch = (size_t(width) * 4 + 255) & ~size_t(255);
    id<MTLBuffer> readback_buffer =
        [context->device newBufferWithLength:row_pitch * height
                                     options:MTLResourceStorageModeShared];
    if (!readback_buffer) {
      if (error_out) {
        *error_out = "failed to create private render target readback buffer";
      }
      return false;
    }
    id<MTLCommandBuffer> command_buffer = [context->command_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit_encoder = [command_buffer blitCommandEncoder];
    [blit_encoder copyFromTexture:context->render_texture
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(width, height, 1)
                         toBuffer:readback_buffer
                destinationOffset:0
           destinationBytesPerRow:row_pitch
         destinationBytesPerImage:row_pitch * height];
    [blit_encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    bool succeeded = [command_buffer status] != MTLCommandBufferStatusError;
    if (!succeeded) {
      if (error_out) {
        NSError* error = [command_buffer error];
        *error_out = error ? [[error localizedDescription] UTF8String]
                           : "private render target readback failed";
      }
      [readback_buffer release];
      return false;
    }
    const uint8_t* source = static_cast<const uint8_t*>([readback_buffer contents]);
    for (uint32_t y = 0; y < height; ++y) {
      std::memcpy(bgra_out.data() + size_t(y) * width * 4, source + size_t(y) * row_pitch,
                  size_t(width) * 4);
    }
    [readback_buffer release];
    return true;
  }
  MTLRegion region = MTLRegionMake2D(0, 0, width, height);
  [context->render_texture getBytes:bgra_out.data()
                        bytesPerRow:size_t(width) * 4
                         fromRegion:region
                        mipmapLevel:0];
  return true;
}

bool RenderPipelineProbe(
    void* metal_device, void* pipeline_state, const void* system_constants,
    size_t system_constants_size, const void* float_constants, size_t float_constants_size,
    const void* fetch_constants, size_t fetch_constants_size, void* shared_memory,
    size_t shared_memory_size, void* shared_memory_metal_buffer,
    const ProbeTextureSlot* vertex_textures, size_t vertex_texture_count,
    size_t vertex_sampler_count, const ProbeTextureSlot* fragment_textures,
    size_t fragment_texture_count, size_t fragment_sampler_count, uint32_t primitive_type,
    uint32_t vertex_count, uint32_t width, uint32_t height, std::vector<uint8_t>& bgra_out,
    std::string* error_out, uint32_t vertex_shared_memory_buffer_index,
    uint32_t vertex_float_constants_buffer_index, uint32_t vertex_fetch_constants_buffer_index,
    const uint8_t* initial_bgra, size_t initial_bgra_row_pitch,
    const void* fragment_float_constants, size_t fragment_float_constants_size,
    uint32_t fragment_float_constants_buffer_index, uint32_t fragment_fetch_constants_buffer_index,
    const ProbeSamplerSlot* vertex_samplers, const ProbeSamplerSlot* fragment_samplers,
    const void* vertex_data, size_t vertex_data_size, uint32_t vertex_data_buffer_index,
    const void* bool_loop_constants, size_t bool_loop_constants_size,
    uint32_t vertex_bool_loop_constants_buffer_index,
    uint32_t fragment_bool_loop_constants_buffer_index, const ProbeIndexBuffer* index_buffer,
    const ProbeRasterizationState* rasterization_state) {
  if (!metal_device || !pipeline_state || !system_constants || !system_constants_size || !width ||
      !height || !vertex_count) {
    if (error_out) {
      *error_out = "missing Metal device, pipeline state, system constants, or target size";
    }
    return false;
  }
  if (!IsProbeIndexBufferValid(index_buffer, vertex_count)) {
    if (error_out) {
      *error_out = "invalid probe index buffer";
    }
    return false;
  }
  if (!IsProbeRasterizationStateValid(rasterization_state, width, height)) {
    if (error_out) {
      *error_out = "invalid probe viewport or scissor";
    }
    return false;
  }

  id<MTLDevice> device = (id<MTLDevice>)metal_device;
  id<MTLCommandQueue> command_queue = [device newCommandQueue];
  if (!command_queue) {
    if (error_out) {
      *error_out = "failed to create command queue";
    }
    return false;
  }

  MTLTextureDescriptor* texture_descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  texture_descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  texture_descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> render_texture = [device newTextureWithDescriptor:texture_descriptor];
  if (!render_texture) {
    [command_queue release];
    if (error_out) {
      *error_out = "failed to create render texture";
    }
    return false;
  }

  id<MTLBuffer> system_buffer = [device newBufferWithBytes:system_constants
                                                    length:system_constants_size
                                                   options:MTLResourceStorageModeShared];
  id<MTLBuffer> float_buffer = nil;
  if (float_constants && float_constants_size) {
    float_buffer = [device newBufferWithBytes:float_constants
                                       length:float_constants_size
                                      options:MTLResourceStorageModeShared];
  }
  id<MTLBuffer> fragment_float_buffer = nil;
  if (fragment_float_constants && fragment_float_constants_size) {
    fragment_float_buffer = [device newBufferWithBytes:fragment_float_constants
                                                length:fragment_float_constants_size
                                               options:MTLResourceStorageModeShared];
  }
  id<MTLBuffer> fetch_buffer = nil;
  if (fetch_constants && fetch_constants_size) {
    fetch_buffer = [device newBufferWithBytes:fetch_constants
                                       length:fetch_constants_size
                                      options:MTLResourceStorageModeShared];
  }
  id<MTLBuffer> bool_loop_buffer = nil;
  if (bool_loop_constants && bool_loop_constants_size) {
    bool_loop_buffer = [device newBufferWithBytes:bool_loop_constants
                                           length:bool_loop_constants_size
                                          options:MTLResourceStorageModeShared];
  }
  id<MTLBuffer> vertex_data_buffer = nil;
  if (vertex_data && vertex_data_size && vertex_data_buffer_index != UINT32_MAX) {
    vertex_data_buffer = [device newBufferWithBytes:vertex_data
                                             length:vertex_data_size
                                            options:MTLResourceStorageModeShared];
  }
  id<MTLBuffer> index_buffer_object = nil;
  if (index_buffer) {
    if (index_buffer->metal_buffer) {
      index_buffer_object = [(id<MTLBuffer>)index_buffer->metal_buffer retain];
    } else {
      index_buffer_object = [device newBufferWithBytes:index_buffer->data
                                                length:index_buffer->size
                                               options:MTLResourceStorageModeShared];
    }
  }
  uint32_t shared_dummy_words[4] = {};
  id<MTLBuffer> shared_memory_buffer = nil;
  if (shared_memory_metal_buffer) {
    shared_memory_buffer = [(id<MTLBuffer>)shared_memory_metal_buffer retain];
  } else if (shared_memory && shared_memory_size) {
    shared_memory_buffer = [device newBufferWithBytesNoCopy:shared_memory
                                                     length:shared_memory_size
                                                    options:MTLResourceStorageModeShared
                                                deallocator:nil];
  }
  if (!shared_memory_buffer && vertex_shared_memory_buffer_index == UINT32_MAX) {
    shared_memory_buffer = [device newBufferWithBytes:shared_dummy_words
                                               length:sizeof(shared_dummy_words)
                                              options:MTLResourceStorageModeShared];
  }

  if (!system_buffer || !shared_memory_buffer || (index_buffer && !index_buffer_object)) {
    if (system_buffer) {
      [system_buffer release];
    }
    if (float_buffer) {
      [float_buffer release];
    }
    if (fragment_float_buffer) {
      [fragment_float_buffer release];
    }
    if (fetch_buffer) {
      [fetch_buffer release];
    }
    if (bool_loop_buffer) {
      [bool_loop_buffer release];
    }
    if (vertex_data_buffer) {
      [vertex_data_buffer release];
    }
    if (index_buffer_object) {
      [index_buffer_object release];
    }
    if (shared_memory_buffer) {
      [shared_memory_buffer release];
    }
    [render_texture release];
    [command_queue release];
    if (error_out) {
      *error_out = index_buffer && !index_buffer_object
                       ? "failed to create probe index buffer"
                       : (vertex_shared_memory_buffer_index != UINT32_MAX && !shared_memory_buffer
                              ? "required shared-memory buffer is unavailable"
                              : "failed to create argument buffers");
    }
    return false;
  }

  MTLTextureDescriptor* dummy_texture_descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                         width:1
                                                        height:1
                                                     mipmapped:NO];
  dummy_texture_descriptor.textureType = MTLTextureType2DArray;
  dummy_texture_descriptor.arrayLength = 1;
  dummy_texture_descriptor.usage = MTLTextureUsageShaderRead;
  dummy_texture_descriptor.storageMode = MTLStorageModeShared;
  id<MTLTexture> dummy_texture = [device newTextureWithDescriptor:dummy_texture_descriptor];
  uint32_t dummy_texture_pixel = 0x00000000u;
  if (dummy_texture) {
    MTLRegion region = MTLRegionMake3D(0, 0, 0, 1, 1, 1);
    [dummy_texture replaceRegion:region
                     mipmapLevel:0
                           slice:0
                       withBytes:&dummy_texture_pixel
                     bytesPerRow:4
                   bytesPerImage:4];
  }
  MTLSamplerDescriptor* sampler_descriptor = [[MTLSamplerDescriptor alloc] init];
  sampler_descriptor.minFilter = MTLSamplerMinMagFilterNearest;
  sampler_descriptor.magFilter = MTLSamplerMinMagFilterNearest;
  sampler_descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
  sampler_descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
  sampler_descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
  id<MTLSamplerState> dummy_sampler = [device newSamplerStateWithDescriptor:sampler_descriptor];
  [sampler_descriptor release];
  std::vector<id<MTLTexture>> vertex_texture_objects;
  std::vector<id<MTLTexture>> fragment_texture_objects;
  std::vector<id<MTLSamplerState>> vertex_sampler_objects;
  std::vector<id<MTLSamplerState>> fragment_sampler_objects;
  CreateProbeTextures(device, vertex_textures, vertex_texture_count, dummy_texture,
                      vertex_texture_objects);
  CreateProbeTextures(device, fragment_textures, fragment_texture_count, dummy_texture,
                      fragment_texture_objects);
  CreateProbeSamplers(device, vertex_samplers, vertex_sampler_count, dummy_sampler,
                      vertex_sampler_objects);
  CreateProbeSamplers(device, fragment_samplers, fragment_sampler_count, dummy_sampler,
                      fragment_sampler_objects);

  bool has_initial_bgra = initial_bgra && initial_bgra_row_pitch >= size_t(width) * 4;
  if (has_initial_bgra) {
    MTLRegion initial_region = MTLRegionMake2D(0, 0, width, height);
    [render_texture replaceRegion:initial_region
                      mipmapLevel:0
                        withBytes:initial_bgra
                      bytesPerRow:initial_bgra_row_pitch];
  }

  id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = render_texture;
  pass.colorAttachments[0].loadAction = has_initial_bgra ? MTLLoadActionLoad : MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);

  id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
  [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline_state];
  if (rasterization_state) {
    MTLViewport viewport = {
        rasterization_state->viewport_x,     rasterization_state->viewport_y,
        rasterization_state->viewport_width, rasterization_state->viewport_height,
        rasterization_state->viewport_z_min, rasterization_state->viewport_z_max};
    [encoder setViewport:viewport];
    MTLScissorRect scissor = {rasterization_state->scissor_x, rasterization_state->scissor_y,
                              rasterization_state->scissor_width,
                              rasterization_state->scissor_height};
    [encoder setScissorRect:scissor];
    [encoder setBlendColorRed:rasterization_state->blend_red
                        green:rasterization_state->blend_green
                         blue:rasterization_state->blend_blue
                        alpha:rasterization_state->blend_alpha];
  }
  [encoder setVertexBuffer:system_buffer offset:0 atIndex:0];
  [encoder setFragmentBuffer:system_buffer offset:0 atIndex:0];
  if (fetch_buffer) {
    if (vertex_fetch_constants_buffer_index != UINT32_MAX) {
      [encoder setVertexBuffer:fetch_buffer offset:0 atIndex:vertex_fetch_constants_buffer_index];
    }
    if (fragment_fetch_constants_buffer_index != UINT32_MAX) {
      [encoder setFragmentBuffer:fetch_buffer
                          offset:0
                         atIndex:fragment_fetch_constants_buffer_index];
    }
  }
  if (bool_loop_buffer) {
    if (vertex_bool_loop_constants_buffer_index != UINT32_MAX) {
      [encoder setVertexBuffer:bool_loop_buffer
                        offset:0
                       atIndex:vertex_bool_loop_constants_buffer_index];
    }
    if (fragment_bool_loop_constants_buffer_index != UINT32_MAX) {
      [encoder setFragmentBuffer:bool_loop_buffer
                          offset:0
                         atIndex:fragment_bool_loop_constants_buffer_index];
    }
  }
  if (float_buffer) {
    if (vertex_float_constants_buffer_index != UINT32_MAX) {
      [encoder setVertexBuffer:float_buffer offset:0 atIndex:vertex_float_constants_buffer_index];
    }
  }
  id<MTLBuffer> fragment_constants_to_bind =
      fragment_float_buffer ? fragment_float_buffer : float_buffer;
  if (fragment_constants_to_bind && fragment_float_constants_buffer_index != UINT32_MAX) {
    [encoder setFragmentBuffer:fragment_constants_to_bind
                        offset:0
                       atIndex:fragment_float_constants_buffer_index];
  }
  if (vertex_shared_memory_buffer_index != UINT32_MAX) {
    [encoder setVertexBuffer:shared_memory_buffer
                      offset:0
                     atIndex:vertex_shared_memory_buffer_index];
  }
  if (vertex_data_buffer && vertex_data_buffer_index != UINT32_MAX) {
    [encoder setVertexBuffer:vertex_data_buffer offset:0 atIndex:vertex_data_buffer_index];
  }
  BindProbeTextures(encoder, vertex_texture_objects, true);
  BindProbeTextures(encoder, fragment_texture_objects, false);
  BindProbeSamplers(encoder, vertex_sampler_objects, true);
  BindProbeSamplers(encoder, fragment_sampler_objects, false);
  if (index_buffer_object) {
    [encoder drawIndexedPrimitives:ToMetalPrimitiveType(primitive_type)
                        indexCount:vertex_count
                         indexType:index_buffer->index_size == 2 ? MTLIndexTypeUInt16
                                                                 : MTLIndexTypeUInt32
                       indexBuffer:index_buffer_object
                 indexBufferOffset:index_buffer->offset];
  } else {
    [encoder drawPrimitives:ToMetalPrimitiveType(primitive_type)
                vertexStart:0
                vertexCount:vertex_count];
  }
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];

  bool succeeded = [command_buffer status] != MTLCommandBufferStatusError;
  if (succeeded) {
    bgra_out.resize(size_t(width) * height * 4);
    MTLRegion region = MTLRegionMake2D(0, 0, width, height);
    [render_texture getBytes:bgra_out.data()
                 bytesPerRow:size_t(width) * 4
                  fromRegion:region
                 mipmapLevel:0];
  } else if (error_out) {
    NSError* error = [command_buffer error];
    *error_out = error ? [[error localizedDescription] UTF8String] : "command buffer failed";
  }

  [system_buffer release];
  if (float_buffer) {
    [float_buffer release];
  }
  if (fragment_float_buffer) {
    [fragment_float_buffer release];
  }
  if (fetch_buffer) {
    [fetch_buffer release];
  }
  if (bool_loop_buffer) {
    [bool_loop_buffer release];
  }
  if (vertex_data_buffer) {
    [vertex_data_buffer release];
  }
  if (index_buffer_object) {
    [index_buffer_object release];
  }
  [shared_memory_buffer release];
  ReleaseOwnedProbeTextures(vertex_texture_objects, dummy_texture);
  ReleaseOwnedProbeTextures(fragment_texture_objects, dummy_texture);
  ReleaseOwnedProbeSamplers(vertex_sampler_objects, dummy_sampler);
  ReleaseOwnedProbeSamplers(fragment_sampler_objects, dummy_sampler);
  if (dummy_texture) {
    [dummy_texture release];
  }
  if (dummy_sampler) {
    [dummy_sampler release];
  }
  [render_texture release];
  [command_queue release];
  return succeeded;
}

}  // namespace rex::graphics::metal
