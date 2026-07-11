#include <rex/graphics/metal/msl_compiler.h>

#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <utility>

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

constexpr uint32_t kMaxProbeDrawsPerCommandBuffer = 64;
constexpr uint32_t kMaxCommittedProbeCommandBuffers = 4;
constexpr size_t kProbeUploadAlignment = 256;
constexpr size_t kProbeUploadChunkSize = 1 << 20;
constexpr uint32_t kInvalidProbeUploadArena = UINT32_MAX;

uint32_t GetTiledRgba8Offset(uint32_t x, uint32_t y, uint32_t pitch) {
  pitch = (pitch + 31u) & ~31u;
  uint32_t macro = ((x >> 5u) + (y >> 5u) * (pitch >> 5u)) << 9u;
  uint32_t micro = ((x & 7u) + ((y & 14u) << 2u)) << 2u;
  uint32_t offset = macro + ((micro & ~15u) << 1u) + (micro & 15u) + ((y & 1u) << 4u);
  return ((offset & ~511u) << 3u) + ((y & 16u) << 7u) + ((offset & 448u) << 2u) +
         (((((y & 8u) >> 2u) + (x >> 3u)) & 3u) << 6u) + (offset & 63u);
}

uint32_t GetTiledRgba8UpperBound(uint32_t right, uint32_t bottom, uint32_t pitch) {
  if (!right || !bottom) {
    return 0;
  }
  uint32_t tile_x = (right - 1u) & ~31u;
  uint32_t tile_y = (bottom - 1u) & ~31u;
  return GetTiledRgba8Offset(tile_x, tile_y, pitch) + 4096u;
}

struct CommittedProbeCommandBuffer {
  // Explicit +1 ownership transferred from PipelineProbeContext's open buffer.
  id<MTLCommandBuffer> command_buffer = nil;
  uint32_t draw_submission_count = 0;
  uint32_t upload_arena_index = kInvalidProbeUploadArena;
};

struct ProbeUploadChunk {
  id<MTLBuffer> buffer = nil;
  size_t capacity = 0;
  size_t offset = 0;
};

struct ProbeUploadArena {
  std::vector<ProbeUploadChunk> chunks;
  bool in_use = false;
};

struct ProbeUploadAllocation {
  id<MTLBuffer> buffer = nil;
  NSUInteger offset = 0;
};

struct PipelineProbeContext;
void ResetOpenProbeBindingTracking(PipelineProbeContext* context);

struct PipelineProbeContext {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> command_queue = nil;
  id<MTLTexture> render_texture = nil;
  id<MTLBuffer> private_readback_buffer = nil;
  size_t private_readback_capacity = 0;
  id<MTLTexture> dummy_texture = nil;
  id<MTLSamplerState> dummy_sampler = nil;
  std::unordered_map<uint64_t, id<MTLSamplerState>> sampler_cache;
  id<MTLRenderPipelineState> clear_pipeline_state = nil;
  id<MTLComputePipelineState> tiled_resolve_pipeline_state = nil;
  MTLStorageMode storage_mode = MTLStorageModeShared;
  uint32_t width = 0;
  uint32_t height = 0;
  bool initialized = false;
  // commandBuffer and renderCommandEncoderWithDescriptor return autoreleased
  // objects. The open objects each have an explicit +1 retain so they remain
  // valid across RenderPipelineProbeToContext's per-call autorelease pools.
  id<MTLCommandBuffer> open_command_buffer = nil;
  id<MTLRenderCommandEncoder> open_render_encoder = nil;
  uint32_t open_draw_submission_count = 0;
  uint32_t open_upload_arena_index = kInvalidProbeUploadArena;
  // Bindings persist within an encoder. Track everything optional so each draw
  // can clear the previous draw's state before installing its own resources.
  uint32_t tracked_vertex_buffer_mask = 0;
  uint32_t tracked_fragment_buffer_mask = 0;
  NSUInteger tracked_vertex_texture_count = 0;
  NSUInteger tracked_fragment_texture_count = 0;
  NSUInteger tracked_vertex_sampler_count = 0;
  NSUInteger tracked_fragment_sampler_count = 0;
  // All buffers use this context's single queue, so waiting for the newest also
  // completes older buffers while retaining their individual error status.
  std::vector<CommittedProbeCommandBuffer> committed_command_buffers;
  // Each arena belongs to exactly one open or committed command buffer. It is
  // reset only after that buffer completes, so draw data may be copied once
  // and bound by offset without allocating an MTLBuffer for every argument.
  ProbeUploadArena upload_arenas[kMaxCommittedProbeCommandBuffers];
  PipelineProbeUploadStats upload_stats;
};

struct TiledResolveConstants {
  uint32_t source_row_pitch;
  uint32_t destination_buffer_offset;
  uint32_t destination_pitch;
  uint32_t destination_x;
  uint32_t destination_y;
  uint32_t copy_width;
  uint32_t copy_height;
  uint32_t destination_endian;
};

void ResetProbeUploadArena(ProbeUploadArena& arena) {
  for (ProbeUploadChunk& chunk : arena.chunks) {
    chunk.offset = 0;
  }
}

uint32_t AcquireProbeUploadArena(PipelineProbeContext* context) {
  for (uint32_t i = 0; i < kMaxCommittedProbeCommandBuffers; ++i) {
    ProbeUploadArena& arena = context->upload_arenas[i];
    if (!arena.in_use) {
      ResetProbeUploadArena(arena);
      arena.in_use = true;
      return i;
    }
  }
  return kInvalidProbeUploadArena;
}

void ReleaseProbeUploadArena(PipelineProbeContext* context, uint32_t arena_index) {
  if (arena_index >= kMaxCommittedProbeCommandBuffers) {
    return;
  }
  ProbeUploadArena& arena = context->upload_arenas[arena_index];
  ResetProbeUploadArena(arena);
  arena.in_use = false;
}

ProbeUploadAllocation UploadProbeDrawData(PipelineProbeContext* context, const void* source,
                                          size_t length, std::string* error_out) {
  ProbeUploadAllocation allocation;
  if (!source || !length) {
    return allocation;
  }
  if (context->open_upload_arena_index >= kMaxCommittedProbeCommandBuffers ||
      length > SIZE_MAX - (kProbeUploadAlignment - 1)) {
    if (error_out) {
      *error_out = "persistent probe upload arena is unavailable or the upload is too large";
    }
    return allocation;
  }

  ProbeUploadArena& arena = context->upload_arenas[context->open_upload_arena_index];
  for (ProbeUploadChunk& chunk : arena.chunks) {
    if (chunk.offset > SIZE_MAX - (kProbeUploadAlignment - 1)) {
      continue;
    }
    size_t aligned_offset =
        (chunk.offset + (kProbeUploadAlignment - 1)) & ~(kProbeUploadAlignment - 1);
    if (aligned_offset <= chunk.capacity && length <= chunk.capacity - aligned_offset) {
      uint8_t* destination = static_cast<uint8_t*>([chunk.buffer contents]);
      if (!destination) {
        continue;
      }
      std::memcpy(destination + aligned_offset, source, length);
      chunk.offset = aligned_offset + length;
      allocation.buffer = chunk.buffer;
      allocation.offset = NSUInteger(aligned_offset);
      ++context->upload_stats.suballocation_count;
      context->upload_stats.suballocation_bytes += length;
      return allocation;
    }
  }

  size_t aligned_length = (length + (kProbeUploadAlignment - 1)) & ~(kProbeUploadAlignment - 1);
  size_t chunk_capacity = std::max(kProbeUploadChunkSize, aligned_length);
  id<MTLBuffer> buffer = [context->device newBufferWithLength:chunk_capacity
                                                      options:MTLResourceStorageModeShared];
  uint8_t* destination = buffer ? static_cast<uint8_t*>([buffer contents]) : nullptr;
  if (!buffer || !destination) {
    if (buffer) {
      [buffer release];
    }
    if (error_out) {
      *error_out = "failed to grow the persistent probe upload arena";
    }
    return allocation;
  }

  std::memcpy(destination, source, length);
  ProbeUploadChunk chunk;
  chunk.buffer = buffer;
  chunk.capacity = chunk_capacity;
  chunk.offset = length;
  arena.chunks.push_back(chunk);
  allocation.buffer = buffer;
  ++context->upload_stats.buffer_allocation_count;
  context->upload_stats.buffer_allocation_bytes += chunk_capacity;
  ++context->upload_stats.suballocation_count;
  context->upload_stats.suballocation_bytes += length;
  return allocation;
}

void DiscardEmptyOpenPipelineProbeCommandBuffer(PipelineProbeContext* context) {
  if (!context || context->open_draw_submission_count) {
    return;
  }
  if (context->open_render_encoder) {
    [context->open_render_encoder endEncoding];
    [context->open_render_encoder release];
    context->open_render_encoder = nil;
  }
  if (context->open_command_buffer) {
    [context->open_command_buffer release];
    context->open_command_buffer = nil;
  }
  ReleaseProbeUploadArena(context, context->open_upload_arena_index);
  context->open_upload_arena_index = kInvalidProbeUploadArena;
  ResetOpenProbeBindingTracking(context);
}

bool EnsureTiledResolvePipelineState(PipelineProbeContext* context, std::string* error_out) {
  if (!context || !context->device) {
    if (error_out) {
      *error_out = "missing probe context or Metal device";
    }
    return false;
  }
  if (context->tiled_resolve_pipeline_state) {
    return true;
  }
  static constexpr char kTiledResolveMsl[] = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct TiledResolveConstants {
  uint source_row_pitch;
  uint destination_buffer_offset;
  uint destination_pitch;
  uint destination_x;
  uint destination_y;
  uint copy_width;
  uint copy_height;
  uint destination_endian;
};

uint tiled_rgba8_offset(uint x, uint y, uint pitch) {
  constexpr uint bytes_per_pixel_log2 = 2;
  uint aligned_pitch = (pitch + 31u) & ~31u;
  uint row_macro = ((y / 32u) * (aligned_pitch / 32u)) << (bytes_per_pixel_log2 + 7u);
  uint row_micro = ((y & 6u) << 2u) << bytes_per_pixel_log2;
  uint row_base = row_macro + ((row_micro & ~15u) << 1u) + (row_micro & 15u) +
                  ((y & 8u) << (3u + bytes_per_pixel_log2)) + ((y & 1u) << 4u);
  uint column_macro = (x / 32u) << (bytes_per_pixel_log2 + 7u);
  uint column_micro = (x & 7u) << bytes_per_pixel_log2;
  uint offset = row_base + column_macro + ((column_micro & ~15u) << 1u) +
                (column_micro & 15u);
  return ((offset & ~511u) << 3u) + ((offset & 448u) << 2u) + (offset & 63u) +
         ((y & 16u) << 7u) + (((((y & 8u) >> 2u) + (x >> 3u)) & 3u) << 6u);
}

kernel void resolve_bgra8_to_xenos_tiled(
    device const uchar* source [[buffer(0)]], device uint* destination [[buffer(1)]],
    constant TiledResolveConstants& constants [[buffer(2)]],
    uint2 position [[thread_position_in_grid]]) {
  if (position.x >= constants.copy_width || position.y >= constants.copy_height) {
    return;
  }
  uint source_offset = position.y * constants.source_row_pitch + position.x * 4u;
  uchar4 bgra = uchar4(source[source_offset], source[source_offset + 1u],
                       source[source_offset + 2u], source[source_offset + 3u]);
  uchar4 packed;
  switch (constants.destination_endian) {
    case 1u:  // Endian128::k8in16.
      packed = bgra.yzwx;
      break;
    case 2u:  // Endian128::k8in32.
      packed = bgra.wxyz;
      break;
    case 3u:  // Endian128::k16in32.
      packed = bgra.xwzy;
      break;
    default:  // Endian128::kNone: raw BGRA becomes guest RGBA.
      packed = bgra.zyxw;
      break;
  }
  uint tiled_offset = tiled_rgba8_offset(constants.destination_x + position.x,
                                         constants.destination_y + position.y,
                                         constants.destination_pitch);
  uint byte_offset = constants.destination_buffer_offset + tiled_offset;
  destination[byte_offset >> 2u] = uint(packed.x) | (uint(packed.y) << 8u) |
                                   (uint(packed.z) << 16u) | (uint(packed.w) << 24u);
}
)MSL";

  NSError* error = nil;
  id<MTLLibrary> library =
      [context->device newLibraryWithSource:[NSString stringWithUTF8String:kTiledResolveMsl]
                                    options:nil
                                      error:&error];
  if (!library) {
    if (error_out) {
      *error_out = error ? [[error localizedDescription] UTF8String]
                         : "tiled resolve compute library failed";
    }
    return false;
  }
  id<MTLFunction> function = [library newFunctionWithName:@"resolve_bgra8_to_xenos_tiled"];
  if (function) {
    context->tiled_resolve_pipeline_state =
        [context->device newComputePipelineStateWithFunction:function error:&error];
    [function release];
  }
  [library release];
  if (!context->tiled_resolve_pipeline_state) {
    if (error_out) {
      *error_out = error ? [[error localizedDescription] UTF8String]
                         : "tiled resolve compute pipeline failed";
    }
    return false;
  }
  return true;
}

id<MTLBuffer> EnsureProbeReadbackBuffer(PipelineProbeContext* context, uint32_t full_width,
                                        uint32_t full_height, size_t row_pitch,
                                        uint32_t read_height, std::string* error_out) {
  size_t readback_size = row_pitch * read_height;
  if (context->private_readback_capacity < readback_size) {
    size_t full_row_pitch = (size_t(full_width) * 4 + 255) & ~size_t(255);
    size_t allocation_size = std::max(readback_size, full_row_pitch * full_height);
    id<MTLBuffer> larger_readback_buffer =
        [context->device newBufferWithLength:allocation_size options:MTLResourceStorageModeShared];
    if (larger_readback_buffer) {
      if (context->private_readback_buffer) {
        [context->private_readback_buffer release];
      }
      context->private_readback_buffer = larger_readback_buffer;
      context->private_readback_capacity = allocation_size;
    }
  }
  id<MTLBuffer> readback_buffer =
      context->private_readback_capacity >= readback_size ? context->private_readback_buffer : nil;
  if (!readback_buffer && error_out) {
    *error_out = "failed to create persistent render target staging buffer";
  }
  return readback_buffer;
}

bool EnsureDummyProbeResources(PipelineProbeContext* context, std::string* error_out) {
  if (context->dummy_texture && context->dummy_sampler) {
    return true;
  }
  if (!context->dummy_texture) {
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:1
                                                          height:1
                                                       mipmapped:NO];
    descriptor.textureType = MTLTextureType2DArray;
    descriptor.arrayLength = 1;
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    context->dummy_texture = [context->device newTextureWithDescriptor:descriptor];
    uint32_t zero = 0;
    if (context->dummy_texture) {
      MTLRegion region = MTLRegionMake3D(0, 0, 0, 1, 1, 1);
      [context->dummy_texture replaceRegion:region
                                mipmapLevel:0
                                      slice:0
                                  withBytes:&zero
                                bytesPerRow:4
                              bytesPerImage:4];
    }
  }
  if (!context->dummy_sampler) {
    MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
    descriptor.minFilter = MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
    context->dummy_sampler = [context->device newSamplerStateWithDescriptor:descriptor];
    [descriptor release];
  }
  if (context->dummy_texture && context->dummy_sampler) {
    return true;
  }
  if (error_out) {
    *error_out = "failed to create persistent probe fallback texture or sampler";
  }
  return false;
}

uint64_t GetProbeSamplerKey(const ProbeSamplerSlot& slot) {
  return uint64_t(slot.min_linear) | (uint64_t(slot.mag_linear) << 8) |
         (uint64_t(slot.mip_linear) << 16) | (uint64_t(slot.address_mode_s) << 24) |
         (uint64_t(slot.address_mode_t) << 32) | (uint64_t(slot.address_mode_r) << 40) |
         (uint64_t(slot.max_anisotropy) << 48);
}

void CreateCachedProbeSamplers(PipelineProbeContext* context, const ProbeSamplerSlot* slots,
                               size_t slot_count, id<MTLSamplerState> fallback_sampler,
                               std::vector<id<MTLSamplerState>>& samplers_out) {
  samplers_out.clear();
  samplers_out.reserve(slot_count);
  for (size_t i = 0; i < slot_count; ++i) {
    id<MTLSamplerState> sampler = nil;
    if (slots) {
      uint64_t key = GetProbeSamplerKey(slots[i]);
      auto existing = context->sampler_cache.find(key);
      if (existing != context->sampler_cache.end()) {
        sampler = existing->second;
      } else {
        sampler = CreateProbeSampler(context->device, slots[i]);
        if (sampler) {
          context->sampler_cache.emplace(key, sampler);
        }
      }
    }
    samplers_out.push_back(sampler ? sampler : fallback_sampler);
  }
}

void ResetOpenProbeBindingTracking(PipelineProbeContext* context) {
  context->tracked_vertex_buffer_mask = 0;
  context->tracked_fragment_buffer_mask = 0;
  context->tracked_vertex_texture_count = 0;
  context->tracked_fragment_texture_count = 0;
  context->tracked_vertex_sampler_count = 0;
  context->tracked_fragment_sampler_count = 0;
}

uint32_t GetPendingProbeDrawSubmissionCount(const PipelineProbeContext* context) {
  if (!context) {
    return 0;
  }
  uint64_t submission_count = context->open_draw_submission_count;
  for (const CommittedProbeCommandBuffer& committed : context->committed_command_buffers) {
    submission_count += committed.draw_submission_count;
  }
  return uint32_t(std::min<uint64_t>(submission_count, UINT32_MAX));
}

bool FinalizeOpenPipelineProbeCommandBuffer(PipelineProbeContext* context, std::string* error_out) {
  if (!context->open_command_buffer && !context->open_render_encoder) {
    return true;
  }
  if (!context->open_command_buffer || !context->open_render_encoder ||
      !context->open_draw_submission_count ||
      context->open_upload_arena_index >= kMaxCommittedProbeCommandBuffers) {
    if (context->open_render_encoder) {
      [context->open_render_encoder endEncoding];
      [context->open_render_encoder release];
      context->open_render_encoder = nil;
    }
    if (context->open_command_buffer) {
      [context->open_command_buffer release];
      context->open_command_buffer = nil;
    }
    ReleaseProbeUploadArena(context, context->open_upload_arena_index);
    context->open_upload_arena_index = kInvalidProbeUploadArena;
    context->open_draw_submission_count = 0;
    ResetOpenProbeBindingTracking(context);
    context->initialized = false;
    if (error_out) {
      *error_out = "inconsistent open probe command buffer state";
    }
    return false;
  }

  id<MTLRenderCommandEncoder> encoder = context->open_render_encoder;
  [encoder endEncoding];
  context->open_render_encoder = nil;
  CommittedProbeCommandBuffer committed;
  committed.command_buffer = context->open_command_buffer;
  committed.draw_submission_count = context->open_draw_submission_count;
  committed.upload_arena_index = context->open_upload_arena_index;
  context->open_command_buffer = nil;
  context->open_draw_submission_count = 0;
  context->open_upload_arena_index = kInvalidProbeUploadArena;
  ResetOpenProbeBindingTracking(context);
  context->committed_command_buffers.push_back(committed);
  [committed.command_buffer commit];
  [encoder release];
  return true;
}

bool ConsumeCompletedPipelineProbeCommands(PipelineProbeContext* context, std::string* error_out) {
  std::string first_error;
  for (const CommittedProbeCommandBuffer& committed : context->committed_command_buffers) {
    id<MTLCommandBuffer> command_buffer = committed.command_buffer;
    if ([command_buffer status] != MTLCommandBufferStatusCompleted && first_error.empty()) {
      NSError* command_error = [command_buffer error];
      const char* description =
          command_error ? [[command_error localizedDescription] UTF8String] : nullptr;
      first_error = description ? description : "asynchronous probe command buffer failed";
    }
    [command_buffer release];
    ReleaseProbeUploadArena(context, committed.upload_arena_index);
  }
  context->committed_command_buffers.clear();
  if (first_error.empty()) {
    return true;
  }
  context->initialized = false;
  if (error_out) {
    *error_out = std::move(first_error);
  }
  return false;
}

bool WaitPendingPipelineProbeCommands(PipelineProbeContext* context, std::string* error_out,
                                      uint32_t* waited_submission_count_out) {
  if (waited_submission_count_out) {
    *waited_submission_count_out = 0;
  }
  if (!context) {
    if (error_out) {
      *error_out = "missing probe context";
    }
    return false;
  }
  uint32_t pending_submission_count = GetPendingProbeDrawSubmissionCount(context);
  if (!pending_submission_count) {
    DiscardEmptyOpenPipelineProbeCommandBuffer(context);
    return true;
  }
  if (waited_submission_count_out) {
    *waited_submission_count_out = pending_submission_count;
  }
  if (!FinalizeOpenPipelineProbeCommandBuffer(context, error_out)) {
    return false;
  }
  [context->committed_command_buffers.back().command_buffer waitUntilCompleted];
  return ConsumeCompletedPipelineProbeCommands(context, error_out);
}

bool EnsureOpenPipelineProbeEncoder(PipelineProbeContext* context, std::string* error_out) {
  if (context->open_command_buffer && context->open_render_encoder) {
    return true;
  }
  if (context->open_command_buffer || context->open_render_encoder) {
    FinalizeOpenPipelineProbeCommandBuffer(context, error_out);
    return false;
  }

  uint32_t upload_arena_index = AcquireProbeUploadArena(context);
  if (upload_arena_index == kInvalidProbeUploadArena) {
    if (!WaitPendingPipelineProbeCommands(context, error_out, nullptr)) {
      return false;
    }
    upload_arena_index = AcquireProbeUploadArena(context);
  }
  if (upload_arena_index == kInvalidProbeUploadArena) {
    if (error_out) {
      *error_out = "no reusable persistent probe upload arena is available";
    }
    return false;
  }

  id<MTLCommandBuffer> command_buffer = [context->command_queue commandBuffer];
  if (!command_buffer) {
    ReleaseProbeUploadArena(context, upload_arena_index);
    if (error_out) {
      *error_out = "failed to create persistent probe command buffer";
    }
    return false;
  }
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = context->render_texture;
  pass.colorAttachments[0].loadAction =
      context->initialized ? MTLLoadActionLoad : MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
  id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
  if (!encoder) {
    ReleaseProbeUploadArena(context, upload_arena_index);
    if (error_out) {
      *error_out = "failed to create persistent probe command encoder";
    }
    return false;
  }

  context->open_command_buffer = [command_buffer retain];
  context->open_render_encoder = [encoder retain];
  context->open_draw_submission_count = 0;
  context->open_upload_arena_index = upload_arena_index;
  ResetOpenProbeBindingTracking(context);
  return true;
}

void ClearTrackedOpenProbeBindings(PipelineProbeContext* context) {
  id<MTLRenderCommandEncoder> encoder = context->open_render_encoder;
  for (uint32_t index = 0; index < 32; ++index) {
    uint32_t bit = uint32_t(1) << index;
    if (context->tracked_vertex_buffer_mask & bit) {
      [encoder setVertexBuffer:nil offset:0 atIndex:index];
    }
    if (context->tracked_fragment_buffer_mask & bit) {
      [encoder setFragmentBuffer:nil offset:0 atIndex:index];
    }
  }
  for (NSUInteger index = 0; index < context->tracked_vertex_texture_count; ++index) {
    [encoder setVertexTexture:nil atIndex:index];
  }
  for (NSUInteger index = 0; index < context->tracked_fragment_texture_count; ++index) {
    [encoder setFragmentTexture:nil atIndex:index];
  }
  for (NSUInteger index = 0; index < context->tracked_vertex_sampler_count; ++index) {
    [encoder setVertexSamplerState:nil atIndex:index];
  }
  for (NSUInteger index = 0; index < context->tracked_fragment_sampler_count; ++index) {
    [encoder setFragmentSamplerState:nil atIndex:index];
  }
  ResetOpenProbeBindingTracking(context);
}

void TrackOpenProbeBufferBinding(PipelineProbeContext* context, bool vertex_stage, uint32_t index) {
  if (index >= 32) {
    return;
  }
  uint32_t& mask =
      vertex_stage ? context->tracked_vertex_buffer_mask : context->tracked_fragment_buffer_mask;
  mask |= uint32_t(1) << index;
}

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
  if (!WaitPendingPipelineProbeCommands(context, error_out, nullptr)) {
    return false;
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
  context->committed_command_buffers.reserve(kMaxCommittedProbeCommandBuffers);
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

bool WaitPipelineProbeContext(void* opaque_context, std::string* error_out,
                              uint32_t* waited_submission_count_out) {
  return WaitPendingPipelineProbeCommands(static_cast<PipelineProbeContext*>(opaque_context),
                                          error_out, waited_submission_count_out);
}

uint32_t GetPipelineProbeContextPendingSubmissionCount(void* opaque_context) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  return GetPendingProbeDrawSubmissionCount(context);
}

bool GetPipelineProbeContextUploadStats(void* opaque_context, PipelineProbeUploadStats* stats_out) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  if (!context || !stats_out) {
    return false;
  }
  *stats_out = context->upload_stats;
  return true;
}

void ResetPipelineProbeContext(void* opaque_context) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  if (context) {
    std::string finalize_error;
    if (!FinalizeOpenPipelineProbeCommandBuffer(context, &finalize_error)) {
      std::fprintf(stderr, "[metal] probe context reset failed to finalize: %s\n",
                   finalize_error.c_str());
    } else if (context->committed_command_buffers.size() >= kMaxCommittedProbeCommandBuffers) {
      uint32_t waited_submission_count = 0;
      if (!WaitPendingPipelineProbeCommands(context, &finalize_error, &waited_submission_count)) {
        std::fprintf(stderr, "[metal] probe context reset drained %u submissions with error: %s\n",
                     waited_submission_count, finalize_error.c_str());
      }
    }
    context->initialized = false;
  }
}

void ReleasePipelineProbeContext(void* opaque_context) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  if (!context) {
    return;
  }
  std::string wait_error;
  uint32_t waited_submission_count = 0;
  if (!WaitPendingPipelineProbeCommands(context, &wait_error, &waited_submission_count)) {
    std::fprintf(stderr, "[metal] probe context release drained %u failed submission(s): %s\n",
                 waited_submission_count, wait_error.c_str());
  }
  if (context->clear_pipeline_state) {
    [context->clear_pipeline_state release];
  }
  if (context->tiled_resolve_pipeline_state) {
    [context->tiled_resolve_pipeline_state release];
  }
  if (context->render_texture) {
    [context->render_texture release];
  }
  if (context->private_readback_buffer) {
    [context->private_readback_buffer release];
  }
  if (context->dummy_texture) {
    [context->dummy_texture release];
  }
  if (context->dummy_sampler) {
    [context->dummy_sampler release];
  }
  for (auto& sampler : context->sampler_cache) {
    [sampler.second release];
  }
  context->sampler_cache.clear();
  for (ProbeUploadArena& arena : context->upload_arenas) {
    for (ProbeUploadChunk& chunk : arena.chunks) {
      if (chunk.buffer) {
        [chunk.buffer release];
      }
    }
    arena.chunks.clear();
    arena.in_use = false;
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
  @autoreleasepool {
    auto* context = static_cast<PipelineProbeContext*>(opaque_context);
    if (!WaitPendingPipelineProbeCommands(context, error_out, nullptr) ||
        !EnsureProbeContextTexture(context, width, height, error_out)) {
      return false;
    }

    id<MTLCommandBuffer> command_buffer = [context->command_queue commandBuffer];
    if (!command_buffer) {
      if (error_out) {
        *error_out = "failed to create probe clear command buffer";
      }
      return false;
    }
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = context->render_texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(red, green, blue, alpha);

    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    if (!encoder) {
      if (error_out) {
        *error_out = "failed to create probe clear command encoder";
      }
      return false;
    }
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];

    bool succeeded = [command_buffer status] == MTLCommandBufferStatusCompleted;
    if (succeeded) {
      context->initialized = true;
    } else if (error_out) {
      NSError* error = [command_buffer error];
      *error_out = error ? [[error localizedDescription] UTF8String] : "command buffer failed";
    }
    return succeeded;
  }
}

bool ClearPipelineProbeContextRect(void* opaque_context, uint32_t width, uint32_t height,
                                   uint32_t x, uint32_t y, uint32_t clear_width,
                                   uint32_t clear_height, double red, double green, double blue,
                                   double alpha, std::string* error_out) {
  @autoreleasepool {
    auto* context = static_cast<PipelineProbeContext*>(opaque_context);
    if (!WaitPendingPipelineProbeCommands(context, error_out, nullptr)) {
      return false;
    }
    if (!clear_width || !clear_height || x >= width || y >= height) {
      return true;
    }
    clear_width = std::min(clear_width, width - x);
    clear_height = std::min(clear_height, height - y);
    if (!x && !y && clear_width == width && clear_height == height) {
      return ClearPipelineProbeContext(opaque_context, width, height, red, green, blue, alpha,
                                       error_out);
    }
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
    if (!command_buffer) {
      [constants_buffer release];
      if (error_out) {
        *error_out = "failed to create rectangular probe clear command buffer";
      }
      return false;
    }
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = context->render_texture;
    pass.colorAttachments[0].loadAction =
        context->initialized ? MTLLoadActionLoad : MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);

    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    if (!encoder) {
      [constants_buffer release];
      if (error_out) {
        *error_out = "failed to create rectangular probe clear command encoder";
      }
      return false;
    }
    MTLScissorRect scissor = {x, y, clear_width, clear_height};
    [encoder setScissorRect:scissor];
    [encoder setRenderPipelineState:context->clear_pipeline_state];
    [encoder setFragmentBuffer:constants_buffer offset:0 atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];

    bool succeeded = [command_buffer status] == MTLCommandBufferStatusCompleted;
    if (succeeded) {
      context->initialized = true;
    } else if (error_out) {
      NSError* error = [command_buffer error];
      *error_out = error ? [[error localizedDescription] UTF8String] : "command buffer failed";
    }
    [constants_buffer release];
    return succeeded;
  }
}

bool QueuePipelineProbeContextClearRect(void* opaque_context, uint32_t width, uint32_t height,
                                        uint32_t x, uint32_t y, uint32_t clear_width,
                                        uint32_t clear_height, double red, double green,
                                        double blue, double alpha, std::string* error_out) {
  @autoreleasepool {
    auto* context = static_cast<PipelineProbeContext*>(opaque_context);
    if (!context) {
      if (error_out) {
        *error_out = "missing probe context";
      }
      return false;
    }
    if (!clear_width || !clear_height || x >= width || y >= height) {
      return true;
    }
    clear_width = std::min(clear_width, width - x);
    clear_height = std::min(clear_height, height - y);
    if (!EnsureProbeContextTexture(context, width, height, error_out) ||
        !EnsureClearPipelineState(context, error_out)) {
      return false;
    }

    if (!EnsureOpenPipelineProbeEncoder(context, error_out)) {
      return false;
    }
    float clear_constants[4] = {float(red), float(green), float(blue), float(alpha)};
    ProbeUploadAllocation constants =
        UploadProbeDrawData(context, clear_constants, sizeof(clear_constants), error_out);
    if (!constants.buffer) {
      DiscardEmptyOpenPipelineProbeCommandBuffer(context);
      if (error_out) {
        if (error_out->empty()) {
          *error_out = "failed to upload queued clear constants";
        }
      }
      return false;
    }

    id<MTLRenderCommandEncoder> encoder = context->open_render_encoder;
    ClearTrackedOpenProbeBindings(context);
    MTLViewport viewport = {0.0, 0.0, double(width), double(height), 0.0, 1.0};
    MTLScissorRect scissor = {x, y, clear_width, clear_height};
    [encoder setViewport:viewport];
    [encoder setScissorRect:scissor];
    [encoder setTriangleFillMode:MTLTriangleFillModeFill];
    [encoder setCullMode:MTLCullModeNone];
    [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
    [encoder setDepthClipMode:MTLDepthClipModeClip];
    [encoder setBlendColorRed:0.0 green:0.0 blue:0.0 alpha:0.0];
    [encoder setRenderPipelineState:context->clear_pipeline_state];
    [encoder setFragmentBuffer:constants.buffer offset:constants.offset atIndex:0];
    TrackOpenProbeBufferBinding(context, false, 0);
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    ++context->open_draw_submission_count;
    context->initialized = true;

    if (context->open_draw_submission_count >= kMaxProbeDrawsPerCommandBuffer &&
        !FinalizeOpenPipelineProbeCommandBuffer(context, error_out)) {
      return false;
    }
    if (context->committed_command_buffers.size() >= kMaxCommittedProbeCommandBuffers) {
      return WaitPendingPipelineProbeCommands(context, error_out, nullptr);
    }
    return true;
  }
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
  @autoreleasepool {
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
    if (!EnsureProbeContextTexture(context, width, height, error_out) ||
        !EnsureDummyProbeResources(context, error_out)) {
      return false;
    }

    if (!EnsureOpenPipelineProbeEncoder(context, error_out)) {
      return false;
    }

    id<MTLDevice> device = context->device;
    ProbeUploadAllocation system_buffer =
        UploadProbeDrawData(context, system_constants, system_constants_size, error_out);
    ProbeUploadAllocation float_buffer =
        UploadProbeDrawData(context, float_constants, float_constants_size, error_out);
    ProbeUploadAllocation fragment_float_buffer = UploadProbeDrawData(
        context, fragment_float_constants, fragment_float_constants_size, error_out);
    ProbeUploadAllocation fetch_buffer =
        UploadProbeDrawData(context, fetch_constants, fetch_constants_size, error_out);
    ProbeUploadAllocation bool_loop_buffer =
        UploadProbeDrawData(context, bool_loop_constants, bool_loop_constants_size, error_out);
    ProbeUploadAllocation vertex_data_buffer;
    if (vertex_data_buffer_index != UINT32_MAX) {
      vertex_data_buffer = UploadProbeDrawData(context, vertex_data, vertex_data_size, error_out);
    }
    ProbeUploadAllocation uploaded_index_buffer;
    id<MTLBuffer> external_index_buffer = nil;
    id<MTLBuffer> index_buffer_object = nil;
    NSUInteger index_buffer_offset = 0;
    if (index_buffer) {
      if (index_buffer->metal_buffer) {
        external_index_buffer = [(id<MTLBuffer>)index_buffer->metal_buffer retain];
        index_buffer_object = external_index_buffer;
        index_buffer_offset = NSUInteger(index_buffer->offset);
      } else {
        uploaded_index_buffer =
            UploadProbeDrawData(context, index_buffer->data, index_buffer->size, error_out);
        index_buffer_object = uploaded_index_buffer.buffer;
        index_buffer_offset = uploaded_index_buffer.offset + NSUInteger(index_buffer->offset);
      }
    }
    id<MTLBuffer> shared_memory_buffer = nil;
    if (shared_memory_metal_buffer) {
      shared_memory_buffer = [(id<MTLBuffer>)shared_memory_metal_buffer retain];
    } else if (shared_memory && shared_memory_size) {
      shared_memory_buffer = [device newBufferWithBytesNoCopy:shared_memory
                                                       length:shared_memory_size
                                                      options:MTLResourceStorageModeShared
                                                  deallocator:nil];
    }
    bool missing_argument_buffer =
        !system_buffer.buffer ||
        (vertex_shared_memory_buffer_index != UINT32_MAX && !shared_memory_buffer) ||
        (float_constants && float_constants_size && !float_buffer.buffer) ||
        (fragment_float_constants && fragment_float_constants_size &&
         !fragment_float_buffer.buffer) ||
        (fetch_constants && fetch_constants_size && !fetch_buffer.buffer) ||
        (bool_loop_constants && bool_loop_constants_size && !bool_loop_buffer.buffer) ||
        (vertex_data && vertex_data_size && vertex_data_buffer_index != UINT32_MAX &&
         !vertex_data_buffer.buffer) ||
        (index_buffer && !index_buffer_object);
    if (missing_argument_buffer) {
      if (external_index_buffer) {
        [external_index_buffer release];
      }
      if (shared_memory_buffer) {
        [shared_memory_buffer release];
      }
      if (error_out) {
        if (error_out->empty()) {
          *error_out =
              index_buffer && !index_buffer_object
                  ? "failed to upload persistent probe index data"
                  : (vertex_shared_memory_buffer_index != UINT32_MAX && !shared_memory_buffer
                         ? "required persistent shared-memory buffer is unavailable"
                         : "failed to upload persistent probe argument data");
        }
      }
      DiscardEmptyOpenPipelineProbeCommandBuffer(context);
      return false;
    }

    id<MTLTexture> dummy_texture = context->dummy_texture;
    id<MTLSamplerState> dummy_sampler = context->dummy_sampler;
    std::vector<id<MTLTexture>> vertex_texture_objects;
    std::vector<id<MTLTexture>> fragment_texture_objects;
    std::vector<id<MTLSamplerState>> vertex_sampler_objects;
    std::vector<id<MTLSamplerState>> fragment_sampler_objects;
    CreateProbeTextures(device, vertex_textures, vertex_texture_count, dummy_texture,
                        vertex_texture_objects);
    CreateProbeTextures(device, fragment_textures, fragment_texture_count, dummy_texture,
                        fragment_texture_objects);
    CreateCachedProbeSamplers(context, vertex_samplers, vertex_sampler_count, dummy_sampler,
                              vertex_sampler_objects);
    CreateCachedProbeSamplers(context, fragment_samplers, fragment_sampler_count, dummy_sampler,
                              fragment_sampler_objects);

    auto release_submission_resources = [&]() {
      if (external_index_buffer) {
        [external_index_buffer release];
      }
      if (shared_memory_buffer) {
        [shared_memory_buffer release];
      }
      ReleaseOwnedProbeTextures(vertex_texture_objects, dummy_texture);
      ReleaseOwnedProbeTextures(fragment_texture_objects, dummy_texture);
      vertex_sampler_objects.clear();
      fragment_sampler_objects.clear();
    };

    id<MTLRenderCommandEncoder> encoder = context->open_render_encoder;
    ClearTrackedOpenProbeBindings(context);
    [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline_state];
    MTLViewport viewport;
    MTLScissorRect scissor;
    double blend_red = 0.0;
    double blend_green = 0.0;
    double blend_blue = 0.0;
    double blend_alpha = 0.0;
    if (rasterization_state) {
      viewport = {rasterization_state->viewport_x,     rasterization_state->viewport_y,
                  rasterization_state->viewport_width, rasterization_state->viewport_height,
                  rasterization_state->viewport_z_min, rasterization_state->viewport_z_max};
      scissor = {rasterization_state->scissor_x, rasterization_state->scissor_y,
                 rasterization_state->scissor_width, rasterization_state->scissor_height};
      blend_red = rasterization_state->blend_red;
      blend_green = rasterization_state->blend_green;
      blend_blue = rasterization_state->blend_blue;
      blend_alpha = rasterization_state->blend_alpha;
    } else {
      viewport = {0.0, 0.0, double(width), double(height), 0.0, 1.0};
      scissor = {0, 0, width, height};
    }
    [encoder setViewport:viewport];
    [encoder setScissorRect:scissor];
    [encoder setBlendColorRed:blend_red green:blend_green blue:blend_blue alpha:blend_alpha];
    [encoder setVertexBuffer:system_buffer.buffer offset:system_buffer.offset atIndex:0];
    [encoder setFragmentBuffer:system_buffer.buffer offset:system_buffer.offset atIndex:0];
    if (vertex_fetch_constants_buffer_index != UINT32_MAX) {
      [encoder setVertexBuffer:fetch_buffer.buffer
                        offset:fetch_buffer.offset
                       atIndex:vertex_fetch_constants_buffer_index];
      TrackOpenProbeBufferBinding(context, true, vertex_fetch_constants_buffer_index);
    }
    if (fragment_fetch_constants_buffer_index != UINT32_MAX) {
      [encoder setFragmentBuffer:fetch_buffer.buffer
                          offset:fetch_buffer.offset
                         atIndex:fragment_fetch_constants_buffer_index];
      TrackOpenProbeBufferBinding(context, false, fragment_fetch_constants_buffer_index);
    }
    if (vertex_bool_loop_constants_buffer_index != UINT32_MAX) {
      [encoder setVertexBuffer:bool_loop_buffer.buffer
                        offset:bool_loop_buffer.offset
                       atIndex:vertex_bool_loop_constants_buffer_index];
      TrackOpenProbeBufferBinding(context, true, vertex_bool_loop_constants_buffer_index);
    }
    if (fragment_bool_loop_constants_buffer_index != UINT32_MAX) {
      [encoder setFragmentBuffer:bool_loop_buffer.buffer
                          offset:bool_loop_buffer.offset
                         atIndex:fragment_bool_loop_constants_buffer_index];
      TrackOpenProbeBufferBinding(context, false, fragment_bool_loop_constants_buffer_index);
    }
    if (vertex_float_constants_buffer_index != UINT32_MAX) {
      [encoder setVertexBuffer:float_buffer.buffer
                        offset:float_buffer.offset
                       atIndex:vertex_float_constants_buffer_index];
      TrackOpenProbeBufferBinding(context, true, vertex_float_constants_buffer_index);
    }
    const ProbeUploadAllocation& fragment_constants_to_bind =
        fragment_float_buffer.buffer ? fragment_float_buffer : float_buffer;
    if (fragment_float_constants_buffer_index != UINT32_MAX) {
      [encoder setFragmentBuffer:fragment_constants_to_bind.buffer
                          offset:fragment_constants_to_bind.offset
                         atIndex:fragment_float_constants_buffer_index];
      TrackOpenProbeBufferBinding(context, false, fragment_float_constants_buffer_index);
    }
    if (vertex_shared_memory_buffer_index != UINT32_MAX) {
      [encoder setVertexBuffer:shared_memory_buffer
                        offset:0
                       atIndex:vertex_shared_memory_buffer_index];
      TrackOpenProbeBufferBinding(context, true, vertex_shared_memory_buffer_index);
    }
    if (vertex_data_buffer_index != UINT32_MAX) {
      [encoder setVertexBuffer:vertex_data_buffer.buffer
                        offset:vertex_data_buffer.offset
                       atIndex:vertex_data_buffer_index];
      TrackOpenProbeBufferBinding(context, true, vertex_data_buffer_index);
    }
    BindProbeTextures(encoder, vertex_texture_objects, true);
    BindProbeTextures(encoder, fragment_texture_objects, false);
    BindProbeSamplers(encoder, vertex_sampler_objects, true);
    BindProbeSamplers(encoder, fragment_sampler_objects, false);
    context->tracked_vertex_texture_count = vertex_texture_objects.size();
    context->tracked_fragment_texture_count = fragment_texture_objects.size();
    context->tracked_vertex_sampler_count = vertex_sampler_objects.size();
    context->tracked_fragment_sampler_count = fragment_sampler_objects.size();
    if (index_buffer_object) {
      [encoder drawIndexedPrimitives:ToMetalPrimitiveType(primitive_type)
                          indexCount:vertex_count
                           indexType:index_buffer->index_size == 2 ? MTLIndexTypeUInt16
                                                                   : MTLIndexTypeUInt32
                         indexBuffer:index_buffer_object
                   indexBufferOffset:index_buffer_offset];
    } else {
      [encoder drawPrimitives:ToMetalPrimitiveType(primitive_type)
                  vertexStart:0
                  vertexCount:vertex_count];
    }
    ++context->open_draw_submission_count;
    context->initialized = true;

    // Normal command buffers retain every encoded resource. Balance all local
    // ownership immediately; the retained command buffer owns the in-flight
    // lifetime until the context is drained.
    release_submission_resources();

    // A no-copy buffer aliases caller-owned bytes rather than snapshotting them.
    // Preserve the old synchronous contract whenever that buffer is actually
    // bound to the vertex stage. The resident MTLBuffer path remains asynchronous.
    bool raw_nocopy_buffer_bound = !shared_memory_metal_buffer && shared_memory &&
                                   vertex_shared_memory_buffer_index != UINT32_MAX;
    if (context->open_draw_submission_count >= kMaxProbeDrawsPerCommandBuffer &&
        !FinalizeOpenPipelineProbeCommandBuffer(context, error_out)) {
      return false;
    }
    bool committed_limit_reached =
        context->committed_command_buffers.size() >= kMaxCommittedProbeCommandBuffers;
    if (raw_nocopy_buffer_bound || committed_limit_reached) {
      return WaitPendingPipelineProbeCommands(context, error_out, nullptr);
    }
    return true;
  }
}

bool ReadPipelineProbeContextRect(void* opaque_context, uint32_t width, uint32_t height, uint32_t x,
                                  uint32_t y, uint32_t read_width, uint32_t read_height,
                                  std::vector<uint8_t>& bgra_out, std::string* error_out) {
  @autoreleasepool {
    auto* context = static_cast<PipelineProbeContext*>(opaque_context);
    if (!context) {
      if (error_out) {
        *error_out = "missing probe context";
      }
      return false;
    }
    bool texture_valid = context->render_texture && context->initialized && width && height &&
                         context->width == width && context->height == height;
    if (!texture_valid) {
      // Read is a fence even if the requested texture metadata is invalid.
      if (!WaitPendingPipelineProbeCommands(context, error_out, nullptr)) {
        return false;
      }
      if (error_out) {
        *error_out = "persistent probe texture is unavailable or has a different size";
      }
      return false;
    }
    if (!read_width || !read_height || x >= width || y >= height || read_width > width - x ||
        read_height > height - y || size_t(read_width) > SIZE_MAX / 4 ||
        size_t(read_height) > SIZE_MAX / (size_t(read_width) * 4)) {
      if (!WaitPendingPipelineProbeCommands(context, error_out, nullptr)) {
        return false;
      }
      if (error_out) {
        *error_out = "persistent probe read rectangle is empty or out of bounds";
      }
      return false;
    }
    bgra_out.resize(size_t(read_width) * read_height * 4);
    if (context->storage_mode == MTLStorageModePrivate) {
      // The readback blit is submitted to the same queue as the render work.
      // Commit pending draws without a separate CPU wait; waiting for the blit
      // completes all earlier work in queue order.
      if (!FinalizeOpenPipelineProbeCommandBuffer(context, error_out)) {
        return false;
      }
      size_t row_pitch = (size_t(read_width) * 4 + 255) & ~size_t(255);
      id<MTLBuffer> readback_buffer =
          EnsureProbeReadbackBuffer(context, width, height, row_pitch, read_height, error_out);
      if (!readback_buffer) {
        WaitPendingPipelineProbeCommands(context, nullptr, nullptr);
        return false;
      }
      id<MTLCommandBuffer> command_buffer = [context->command_queue commandBuffer];
      if (!command_buffer) {
        WaitPendingPipelineProbeCommands(context, nullptr, nullptr);
        if (error_out) {
          *error_out = "failed to create private render target readback command buffer";
        }
        return false;
      }
      id<MTLBlitCommandEncoder> blit_encoder = [command_buffer blitCommandEncoder];
      if (!blit_encoder) {
        WaitPendingPipelineProbeCommands(context, nullptr, nullptr);
        if (error_out) {
          *error_out = "failed to create private render target readback encoder";
        }
        return false;
      }
      [blit_encoder copyFromTexture:context->render_texture
                        sourceSlice:0
                        sourceLevel:0
                       sourceOrigin:MTLOriginMake(x, y, 0)
                         sourceSize:MTLSizeMake(read_width, read_height, 1)
                           toBuffer:readback_buffer
                  destinationOffset:0
             destinationBytesPerRow:row_pitch
           destinationBytesPerImage:row_pitch * read_height];
      [blit_encoder endEncoding];
      [command_buffer commit];
      [command_buffer waitUntilCompleted];
      bool succeeded = [command_buffer status] == MTLCommandBufferStatusCompleted;
      bool prior_commands_succeeded = ConsumeCompletedPipelineProbeCommands(context, error_out);
      if (!prior_commands_succeeded) {
        return false;
      }
      if (!succeeded) {
        if (error_out) {
          NSError* error = [command_buffer error];
          *error_out = error ? [[error localizedDescription] UTF8String]
                             : "private render target readback failed";
        }
        return false;
      }
      const uint8_t* source = static_cast<const uint8_t*>([readback_buffer contents]);
      size_t tight_row_pitch = size_t(read_width) * 4;
      if (row_pitch == tight_row_pitch) {
        std::memcpy(bgra_out.data(), source, tight_row_pitch * read_height);
      } else {
        for (uint32_t row = 0; row < read_height; ++row) {
          std::memcpy(bgra_out.data() + size_t(row) * tight_row_pitch,
                      source + size_t(row) * row_pitch, tight_row_pitch);
        }
      }
      return true;
    }
    if (!WaitPendingPipelineProbeCommands(context, error_out, nullptr)) {
      return false;
    }
    MTLRegion region = MTLRegionMake2D(x, y, read_width, read_height);
    [context->render_texture getBytes:bgra_out.data()
                          bytesPerRow:size_t(read_width) * 4
                           fromRegion:region
                          mipmapLevel:0];
    return true;
  }
}

bool ResolvePipelineProbeContextToXenosTiled(void* opaque_context, uint32_t width, uint32_t height,
                                             uint32_t source_x, uint32_t source_y,
                                             uint32_t resolve_width, uint32_t resolve_height,
                                             const ProbeTiledResolveTarget& destination,
                                             std::vector<uint8_t>* bgra_out,
                                             std::string* error_out) {
  @autoreleasepool {
    auto* context = static_cast<PipelineProbeContext*>(opaque_context);
    if (bgra_out) {
      bgra_out->clear();
    }
    if (!context) {
      if (error_out) {
        *error_out = "missing probe context";
      }
      return false;
    }

    auto reject_and_drain = [&](const std::string& reason) {
      std::string drain_error;
      bool drained = WaitPendingPipelineProbeCommands(context, &drain_error, nullptr);
      if (error_out) {
        *error_out = reason;
        if (!drained && !drain_error.empty()) {
          error_out->append("; prior render work failed: ");
          error_out->append(drain_error);
        }
      }
      return false;
    };

    bool texture_valid = context->render_texture && context->initialized && width && height &&
                         context->width == width && context->height == height;
    if (!texture_valid) {
      return reject_and_drain("persistent probe texture is unavailable or has a different size");
    }
    if (!resolve_width || !resolve_height || source_x >= width || source_y >= height ||
        resolve_width > width - source_x || resolve_height > height - source_y ||
        !destination.metal_buffer || !destination.pitch || !destination.height ||
        destination.x >= destination.pitch || destination.y >= destination.height ||
        resolve_width > destination.pitch - destination.x ||
        resolve_height > destination.height - destination.y || destination.endian > 3 ||
        (destination.buffer_offset & 3) || destination.buffer_offset > UINT32_MAX ||
        resolve_width > UINT32_MAX / 4 || size_t(resolve_width) > SIZE_MAX / 4 ||
        size_t(resolve_height) > SIZE_MAX / (size_t(resolve_width) * 4)) {
      return reject_and_drain("invalid tiled resolve rectangle, surface, buffer, or endian");
    }

    id<MTLBuffer> destination_buffer = (id<MTLBuffer>)destination.metal_buffer;
    uint64_t aligned_destination_pitch = (uint64_t(destination.pitch) + 31u) & ~uint64_t(31u);
    uint64_t aligned_destination_height = (uint64_t(destination.height) + 31u) & ~uint64_t(31u);
    if (aligned_destination_pitch > uint64_t(UINT32_MAX / 4) / aligned_destination_height) {
      return reject_and_drain("tiled resolve surface byte extent exceeds 32-bit addressing");
    }
    uint32_t tiled_surface_extent =
        GetTiledRgba8UpperBound(destination.pitch, destination.height, destination.pitch);
    uint64_t destination_end = uint64_t(destination.buffer_offset) + tiled_surface_extent;
    if ([destination_buffer storageMode] != MTLStorageModeShared || !tiled_surface_extent ||
        destination_end > [destination_buffer length] || destination_end > UINT32_MAX) {
      return reject_and_drain(
          "tiled resolve destination is not a sufficiently large shared Metal buffer");
    }

    std::string setup_error;
    if (!EnsureTiledResolvePipelineState(context, &setup_error)) {
      return reject_and_drain(setup_error);
    }
    size_t row_pitch = (size_t(resolve_width) * 4 + 255) & ~size_t(255);
    id<MTLBuffer> staging_buffer =
        EnsureProbeReadbackBuffer(context, width, height, row_pitch, resolve_height, &setup_error);
    if (!staging_buffer) {
      return reject_and_drain(setup_error);
    }

    // Commit render work without waiting. The blit and compute command buffer is
    // on the same queue, so waiting for it completes every earlier submission.
    std::string finalize_error;
    if (!FinalizeOpenPipelineProbeCommandBuffer(context, &finalize_error)) {
      return reject_and_drain(finalize_error.empty()
                                  ? "failed to finalize pending render work for tiled resolve"
                                  : finalize_error);
    }
    id<MTLCommandBuffer> command_buffer = [context->command_queue commandBuffer];
    if (!command_buffer) {
      return reject_and_drain("failed to create tiled resolve command buffer");
    }
    id<MTLBlitCommandEncoder> blit_encoder = [command_buffer blitCommandEncoder];
    if (!blit_encoder) {
      return reject_and_drain("failed to create tiled resolve blit encoder");
    }
    [blit_encoder copyFromTexture:context->render_texture
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(source_x, source_y, 0)
                       sourceSize:MTLSizeMake(resolve_width, resolve_height, 1)
                         toBuffer:staging_buffer
                destinationOffset:0
           destinationBytesPerRow:row_pitch
         destinationBytesPerImage:row_pitch * resolve_height];
    [blit_encoder endEncoding];

    id<MTLComputeCommandEncoder> compute_encoder = [command_buffer computeCommandEncoder];
    if (!compute_encoder) {
      return reject_and_drain("failed to create tiled resolve compute encoder");
    }
    TiledResolveConstants constants = {
        uint32_t(row_pitch), uint32_t(destination.buffer_offset),
        destination.pitch,   destination.x,
        destination.y,       resolve_width,
        resolve_height,      destination.endian,
    };
    id<MTLComputePipelineState> pipeline_state = context->tiled_resolve_pipeline_state;
    [compute_encoder setComputePipelineState:pipeline_state];
    [compute_encoder setBuffer:staging_buffer offset:0 atIndex:0];
    [compute_encoder setBuffer:destination_buffer offset:0 atIndex:1];
    [compute_encoder setBytes:&constants length:sizeof(constants) atIndex:2];
    NSUInteger thread_width = std::max<NSUInteger>(
        1, std::min<NSUInteger>(resolve_width, [pipeline_state threadExecutionWidth]));
    NSUInteger max_threads = [pipeline_state maxTotalThreadsPerThreadgroup];
    NSUInteger thread_height =
        std::max<NSUInteger>(1, std::min<NSUInteger>(resolve_height, max_threads / thread_width));
    [compute_encoder dispatchThreads:MTLSizeMake(resolve_width, resolve_height, 1)
               threadsPerThreadgroup:MTLSizeMake(thread_width, thread_height, 1)];
    [compute_encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];

    std::string prior_error;
    bool prior_commands_succeeded = ConsumeCompletedPipelineProbeCommands(context, &prior_error);
    bool resolve_succeeded = [command_buffer status] == MTLCommandBufferStatusCompleted;
    if (!prior_commands_succeeded || !resolve_succeeded) {
      if (error_out) {
        error_out->clear();
        if (!prior_commands_succeeded) {
          error_out->append("prior render work failed: ");
          error_out->append(prior_error);
        }
        if (!resolve_succeeded) {
          if (!error_out->empty()) {
            error_out->append("; ");
          }
          NSError* command_error = [command_buffer error];
          const char* description =
              command_error ? [[command_error localizedDescription] UTF8String] : nullptr;
          error_out->append(description ? description : "tiled resolve blit or compute failed");
        }
      }
      return false;
    }

    if (bgra_out) {
      size_t tight_row_pitch = size_t(resolve_width) * 4;
      bgra_out->resize(tight_row_pitch * resolve_height);
      const uint8_t* source = static_cast<const uint8_t*>([staging_buffer contents]);
      if (row_pitch == tight_row_pitch) {
        std::memcpy(bgra_out->data(), source, tight_row_pitch * resolve_height);
      } else {
        for (uint32_t row = 0; row < resolve_height; ++row) {
          std::memcpy(bgra_out->data() + size_t(row) * tight_row_pitch,
                      source + size_t(row) * row_pitch, tight_row_pitch);
        }
      }
    }
    return true;
  }
}

bool ReadPipelineProbeContext(void* opaque_context, uint32_t width, uint32_t height,
                              std::vector<uint8_t>& bgra_out, std::string* error_out) {
  return ReadPipelineProbeContextRect(opaque_context, width, height, 0, 0, width, height, bgra_out,
                                      error_out);
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
