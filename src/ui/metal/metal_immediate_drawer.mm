#include <rex/ui/metal/immediate_drawer.h>

#import <Metal/Metal.h>

#include <rex/assert.h>
#include <rex/logging.h>
#include <rex/ui/metal/presenter.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace rex::ui::metal {
namespace {

constexpr const char* kImmediateMetalSource = R"(
#include <metal_stdlib>
using namespace metal;

struct ImmediateVertexIn {
  float2 position [[attribute(0)]];
  float2 uv [[attribute(1)]];
  uchar4 color [[attribute(2)]];
};

struct ImmediateVertexOut {
  float4 position [[position]];
  float2 uv;
  float4 color;
};

vertex ImmediateVertexOut immediate_vs(
    ImmediateVertexIn in [[stage_in]],
    constant float2& coordinate_space_size_inv [[buffer(1)]]) {
  ImmediateVertexOut out;
  out.position = float4(in.position.x * coordinate_space_size_inv.x * 2.0 - 1.0,
                        1.0 - in.position.y * coordinate_space_size_inv.y * 2.0,
                        0.0, 1.0);
  out.uv = in.uv;
  out.color = float4(in.color) / 255.0;
  return out;
}

fragment float4 immediate_ps(
    ImmediateVertexOut in [[stage_in]],
    texture2d<float, access::sample> immediate_texture [[texture(0)]],
    sampler immediate_sampler [[sampler(0)]]) {
  return in.color * immediate_texture.sample(immediate_sampler, in.uv);
}
)";

id<MTLTexture> CreateMetalTexture(id<MTLDevice> device, uint32_t width, uint32_t height,
                                  const uint8_t* data) {
  if (!device || !width || !height || !data ||
      size_t(width) > std::numeric_limits<size_t>::max() / 4) {
    return nil;
  }
  const size_t bytes_per_row = size_t(width) * 4;
  if (size_t(height) > std::numeric_limits<size_t>::max() / bytes_per_row) {
    return nil;
  }

  MTLTextureDescriptor* descriptor = [[MTLTextureDescriptor alloc] init];
  descriptor.textureType = MTLTextureType2D;
  descriptor.pixelFormat = MTLPixelFormatRGBA8Unorm;
  descriptor.width = width;
  descriptor.height = height;
  descriptor.depth = 1;
  descriptor.mipmapLevelCount = 1;
  descriptor.arrayLength = 1;
  descriptor.sampleCount = 1;
  descriptor.storageMode = MTLStorageModeShared;
  descriptor.usage = MTLTextureUsageShaderRead;
  id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
  [descriptor release];
  if (!texture) {
    return nil;
  }

  [texture replaceRegion:MTLRegionMake2D(0, 0, width, height)
             mipmapLevel:0
               withBytes:data
             bytesPerRow:bytes_per_row];
  return texture;
}

}  // namespace

std::unique_ptr<MetalImmediateDrawer> MetalImmediateDrawer::Create(void* metal_device) {
  if (!metal_device) {
    return nullptr;
  }
  auto immediate_drawer =
      std::unique_ptr<MetalImmediateDrawer>(new MetalImmediateDrawer(metal_device));
  if (!immediate_drawer->Initialize()) {
    return nullptr;
  }
  return immediate_drawer;
}

MetalImmediateDrawer::MetalImmediateTexture::MetalImmediateTexture(
    uint32_t width, uint32_t height, void* metal_texture, SamplerIndex sampler_index,
    MetalImmediateDrawer* immediate_drawer, size_t immediate_drawer_index)
    : ImmediateTexture(width, height),
      metal_texture_(metal_texture),
      sampler_index_(sampler_index),
      immediate_drawer_(immediate_drawer),
      immediate_drawer_index_(immediate_drawer_index) {}

MetalImmediateDrawer::MetalImmediateTexture::~MetalImmediateTexture() {
  if (immediate_drawer_) {
    immediate_drawer_->OnImmediateTextureDestroyed(*this);
  }
  if (metal_texture_) {
    [(id)metal_texture_ release];
    metal_texture_ = nullptr;
  }
}

void MetalImmediateDrawer::MetalImmediateTexture::OnImmediateDrawerDestroyed() {
  immediate_drawer_ = nullptr;
  immediate_drawer_index_ = 0;
}

MetalImmediateDrawer::MetalImmediateDrawer(void* metal_device) : metal_device_(metal_device) {
  [(id)metal_device_ retain];
}

MetalImmediateDrawer::~MetalImmediateDrawer() {
  ResetBatch();
  render_command_encoder_ = nullptr;

  for (MetalImmediateTexture* texture : textures_) {
    texture->OnImmediateDrawerDestroyed();
  }
  textures_.clear();

  for (void*& sampler : samplers_) {
    if (sampler) {
      [(id)sampler release];
      sampler = nullptr;
    }
  }
  if (white_texture_) {
    [(id)white_texture_ release];
    white_texture_ = nullptr;
  }
  if (pipeline_state_) {
    [(id)pipeline_state_ release];
    pipeline_state_ = nullptr;
  }
  if (metal_device_) {
    [(id)metal_device_ release];
    metal_device_ = nullptr;
  }
}

bool MetalImmediateDrawer::Initialize() {
  id<MTLDevice> device = (id<MTLDevice>)metal_device_;

  NSError* library_error = nil;
  NSString* shader_source = [[NSString alloc] initWithUTF8String:kImmediateMetalSource];
  id<MTLLibrary> library = [device newLibraryWithSource:shader_source
                                                options:nil
                                                  error:&library_error];
  [shader_source release];
  if (!library) {
    const char* error_text =
        library_error ? [[library_error localizedDescription] UTF8String] : "unknown error";
    REXLOG_ERROR("MetalImmediateDrawer: failed to compile shaders: {}", error_text);
    return false;
  }

  id<MTLFunction> vertex_function = [library newFunctionWithName:@"immediate_vs"];
  id<MTLFunction> fragment_function = [library newFunctionWithName:@"immediate_ps"];
  if (!vertex_function || !fragment_function) {
    REXLOG_ERROR("MetalImmediateDrawer: failed to load shader entry points");
    if (vertex_function) {
      [vertex_function release];
    }
    if (fragment_function) {
      [fragment_function release];
    }
    [library release];
    return false;
  }

  MTLVertexDescriptor* vertex_descriptor = [[MTLVertexDescriptor alloc] init];
  vertex_descriptor.attributes[0].format = MTLVertexFormatFloat2;
  vertex_descriptor.attributes[0].offset = offsetof(ImmediateVertex, x);
  vertex_descriptor.attributes[0].bufferIndex = 0;
  vertex_descriptor.attributes[1].format = MTLVertexFormatFloat2;
  vertex_descriptor.attributes[1].offset = offsetof(ImmediateVertex, u);
  vertex_descriptor.attributes[1].bufferIndex = 0;
  vertex_descriptor.attributes[2].format = MTLVertexFormatUChar4;
  vertex_descriptor.attributes[2].offset = offsetof(ImmediateVertex, color);
  vertex_descriptor.attributes[2].bufferIndex = 0;
  vertex_descriptor.layouts[0].stride = sizeof(ImmediateVertex);
  vertex_descriptor.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
  vertex_descriptor.layouts[0].stepRate = 1;

  MTLRenderPipelineDescriptor* pipeline_descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  pipeline_descriptor.label = @"ReXGlue immediate UI";
  pipeline_descriptor.vertexFunction = vertex_function;
  pipeline_descriptor.fragmentFunction = fragment_function;
  pipeline_descriptor.vertexDescriptor = vertex_descriptor;
  pipeline_descriptor.rasterSampleCount = 1;
  pipeline_descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  pipeline_descriptor.colorAttachments[0].blendingEnabled = YES;
  pipeline_descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
  pipeline_descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
  pipeline_descriptor.colorAttachments[0].destinationRGBBlendFactor =
      MTLBlendFactorOneMinusSourceAlpha;
  pipeline_descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
  pipeline_descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
  pipeline_descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;

  NSError* pipeline_error = nil;
  pipeline_state_ = [device newRenderPipelineStateWithDescriptor:pipeline_descriptor
                                                           error:&pipeline_error];
  [pipeline_descriptor release];
  [vertex_descriptor release];
  [fragment_function release];
  [vertex_function release];
  [library release];
  if (!pipeline_state_) {
    const char* error_text =
        pipeline_error ? [[pipeline_error localizedDescription] UTF8String] : "unknown error";
    REXLOG_ERROR("MetalImmediateDrawer: failed to create the render pipeline: {}", error_text);
    return false;
  }

  for (size_t sampler_index = 0; sampler_index < samplers_.size(); ++sampler_index) {
    const auto index = SamplerIndex(sampler_index);
    const bool linear = index == SamplerIndex::kLinearClamp || index == SamplerIndex::kLinearRepeat;
    const bool repeated =
        index == SamplerIndex::kNearestRepeat || index == SamplerIndex::kLinearRepeat;
    MTLSamplerDescriptor* sampler_descriptor = [[MTLSamplerDescriptor alloc] init];
    sampler_descriptor.minFilter =
        linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sampler_descriptor.magFilter =
        linear ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    sampler_descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
    sampler_descriptor.sAddressMode =
        repeated ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
    sampler_descriptor.tAddressMode =
        repeated ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
    sampler_descriptor.rAddressMode =
        repeated ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge;
    sampler_descriptor.maxAnisotropy = 1;
    samplers_[sampler_index] = [device newSamplerStateWithDescriptor:sampler_descriptor];
    [sampler_descriptor release];
    if (!samplers_[sampler_index]) {
      REXLOG_ERROR("MetalImmediateDrawer: failed to create sampler {}", sampler_index);
      return false;
    }
  }

  constexpr uint8_t kWhitePixel[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  white_texture_ = CreateMetalTexture(device, 1, 1, kWhitePixel);
  if (!white_texture_) {
    REXLOG_ERROR("MetalImmediateDrawer: failed to create the solid-color texture");
    return false;
  }
  [(id<MTLTexture>)white_texture_ setLabel:@"ReXGlue immediate UI white"];
  return true;
}

std::unique_ptr<ImmediateTexture> MetalImmediateDrawer::CreateTexture(uint32_t width,
                                                                      uint32_t height,
                                                                      ImmediateTextureFilter filter,
                                                                      bool is_repeated,
                                                                      const uint8_t* data) {
  id<MTLTexture> metal_texture =
      CreateMetalTexture((id<MTLDevice>)metal_device_, width, height, data);
  if (!metal_texture) {
    REXLOG_ERROR("MetalImmediateDrawer: failed to create texture {}x{}", width, height);
  } else {
    [metal_texture setLabel:@"ReXGlue immediate UI texture"];
  }

  SamplerIndex sampler_index;
  if (filter == ImmediateTextureFilter::kLinear) {
    sampler_index = is_repeated ? SamplerIndex::kLinearRepeat : SamplerIndex::kLinearClamp;
  } else {
    sampler_index = is_repeated ? SamplerIndex::kNearestRepeat : SamplerIndex::kNearestClamp;
  }

  const size_t texture_index = textures_.size();
  auto texture = std::make_unique<MetalImmediateTexture>(
      width, height, metal_texture, sampler_index, metal_texture ? this : nullptr, texture_index);
  if (metal_texture) {
    textures_.push_back(texture.get());
  }
  return texture;
}

void MetalImmediateDrawer::Begin(UIDrawContext& ui_draw_context, float coordinate_space_width,
                                 float coordinate_space_height) {
  ImmediateDrawer::Begin(ui_draw_context, coordinate_space_width, coordinate_space_height);
  assert_false(batch_open_);

  const auto& metal_ui_draw_context = static_cast<const MetalUIDrawContext&>(ui_draw_context);
  render_command_encoder_ = metal_ui_draw_context.render_command_encoder();
  current_texture_ = nullptr;
  current_sampler_index_ = SamplerIndex::kCount;
  current_scissor_left_ = 0;
  current_scissor_top_ = 0;
  current_scissor_width_ = 0;
  current_scissor_height_ = 0;

  id<MTLRenderCommandEncoder> encoder = (id<MTLRenderCommandEncoder>)render_command_encoder_;
  if (!encoder || !pipeline_state_) {
    return;
  }

  MTLViewport viewport = {0.0,
                          0.0,
                          double(ui_draw_context.render_target_width()),
                          double(ui_draw_context.render_target_height()),
                          0.0,
                          1.0};
  [encoder setViewport:viewport];
  [encoder setCullMode:MTLCullModeNone];
  [encoder setTriangleFillMode:MTLTriangleFillModeFill];
  [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline_state_];

  const float coordinate_space_size_inv[2] = {
      1.0f / this->coordinate_space_width(),
      1.0f / this->coordinate_space_height(),
  };
  [encoder setVertexBytes:coordinate_space_size_inv
                   length:sizeof(coordinate_space_size_inv)
                  atIndex:1];
}

void MetalImmediateDrawer::BeginDrawBatch(const ImmediateDrawBatch& batch) {
  assert_false(batch_open_);
  ResetBatch();

  id<MTLRenderCommandEncoder> encoder = (id<MTLRenderCommandEncoder>)render_command_encoder_;
  if (!encoder || batch.vertex_count <= 0 || !batch.vertices || batch.index_count < 0 ||
      (batch.indices && batch.index_count <= 0)) {
    return;
  }

  const size_t vertex_count = size_t(batch.vertex_count);
  if (vertex_count > std::numeric_limits<size_t>::max() / sizeof(ImmediateVertex)) {
    return;
  }
  const size_t vertex_size = vertex_count * sizeof(ImmediateVertex);

  size_t index_size = 0;
  if (batch.indices) {
    const size_t index_count = size_t(batch.index_count);
    if (index_count > std::numeric_limits<size_t>::max() / sizeof(uint16_t)) {
      return;
    }
    index_size = index_count * sizeof(uint16_t);
  }
  if (index_size > std::numeric_limits<size_t>::max() - vertex_size) {
    return;
  }
  const size_t buffer_size = vertex_size + index_size;

  id<MTLBuffer> buffer =
      [(id<MTLDevice>)metal_device_ newBufferWithLength:buffer_size
                                                options:MTLResourceStorageModeShared];
  if (!buffer) {
    REXLOG_ERROR("MetalImmediateDrawer: failed to allocate a {}-byte draw buffer", buffer_size);
    return;
  }
  std::memcpy([buffer contents], batch.vertices, vertex_size);
  if (index_size) {
    std::memcpy(static_cast<uint8_t*>([buffer contents]) + vertex_size, batch.indices, index_size);
  }

  batch_buffer_ = buffer;
  batch_index_offset_ = vertex_size;
  batch_vertex_count_ = batch.vertex_count;
  batch_index_count_ = batch.index_count;
  batch_has_index_buffer_ = batch.indices != nullptr;
  batch_open_ = true;
  [encoder setVertexBuffer:buffer offset:0 atIndex:0];
}

void MetalImmediateDrawer::Draw(const ImmediateDraw& draw) {
  id<MTLRenderCommandEncoder> encoder = (id<MTLRenderCommandEncoder>)render_command_encoder_;
  if (!batch_open_ || !batch_buffer_ || !encoder || draw.count <= 0) {
    return;
  }

  if (batch_has_index_buffer_) {
    if (draw.index_offset < 0 || draw.index_offset > batch_index_count_ ||
        draw.count > batch_index_count_ - draw.index_offset) {
      return;
    }
  } else if (draw.base_vertex < 0 || draw.base_vertex > batch_vertex_count_ ||
             draw.count > batch_vertex_count_ - draw.base_vertex) {
    return;
  }

  uint32_t scissor_left;
  uint32_t scissor_top;
  uint32_t scissor_width;
  uint32_t scissor_height;
  if (!ScissorToRenderTarget(draw, scissor_left, scissor_top, scissor_width, scissor_height)) {
    return;
  }
  if (current_scissor_left_ != scissor_left || current_scissor_top_ != scissor_top ||
      current_scissor_width_ != scissor_width || current_scissor_height_ != scissor_height) {
    current_scissor_left_ = scissor_left;
    current_scissor_top_ = scissor_top;
    current_scissor_width_ = scissor_width;
    current_scissor_height_ = scissor_height;
    MTLScissorRect scissor = {
        NSUInteger(scissor_left),
        NSUInteger(scissor_top),
        NSUInteger(scissor_width),
        NSUInteger(scissor_height),
    };
    [encoder setScissorRect:scissor];
  }

  id<MTLTexture> metal_texture = (id<MTLTexture>)white_texture_;
  SamplerIndex sampler_index = SamplerIndex::kNearestClamp;
  auto* texture = static_cast<MetalImmediateTexture*>(draw.texture);
  if (texture && texture->immediate_drawer_ == this && texture->metal_texture_) {
    metal_texture = (id<MTLTexture>)texture->metal_texture_;
    sampler_index = texture->sampler_index_;
  }
  if (current_texture_ != metal_texture) {
    current_texture_ = metal_texture;
    [encoder setFragmentTexture:metal_texture atIndex:0];
  }
  if (current_sampler_index_ != sampler_index) {
    current_sampler_index_ = sampler_index;
    [encoder setFragmentSamplerState:(id<MTLSamplerState>)samplers_[size_t(sampler_index)]
                             atIndex:0];
  }

  MTLPrimitiveType primitive_type;
  switch (draw.primitive_type) {
    case ImmediatePrimitiveType::kLines:
      primitive_type = MTLPrimitiveTypeLine;
      break;
    case ImmediatePrimitiveType::kTriangles:
      primitive_type = MTLPrimitiveTypeTriangle;
      break;
    default:
      assert_unhandled_case(draw.primitive_type);
      return;
  }

  if (batch_has_index_buffer_) {
    const size_t index_offset = batch_index_offset_ + size_t(draw.index_offset) * sizeof(uint16_t);
    [encoder drawIndexedPrimitives:primitive_type
                        indexCount:NSUInteger(draw.count)
                         indexType:MTLIndexTypeUInt16
                       indexBuffer:(id<MTLBuffer>)batch_buffer_
                 indexBufferOffset:index_offset
                     instanceCount:1
                        baseVertex:NSInteger(draw.base_vertex)
                      baseInstance:0];
  } else {
    [encoder drawPrimitives:primitive_type
                vertexStart:NSUInteger(draw.base_vertex)
                vertexCount:NSUInteger(draw.count)];
  }
}

void MetalImmediateDrawer::EndDrawBatch() {
  ResetBatch();
}

void MetalImmediateDrawer::End() {
  assert_false(batch_open_);
  render_command_encoder_ = nullptr;
  ImmediateDrawer::End();
}

void MetalImmediateDrawer::OnImmediateTextureDestroyed(MetalImmediateTexture& texture) {
  const size_t texture_index = texture.immediate_drawer_index_;
  assert_true(texture_index < textures_.size());
  assert_true(textures_[texture_index] == &texture);
  if (texture_index + 1 != textures_.size()) {
    textures_[texture_index] = textures_.back();
    textures_[texture_index]->immediate_drawer_index_ = texture_index;
  }
  textures_.pop_back();
  texture.immediate_drawer_ = nullptr;
  texture.immediate_drawer_index_ = 0;
}

void MetalImmediateDrawer::ResetBatch() {
  if (batch_buffer_) {
    [(id)batch_buffer_ release];
    batch_buffer_ = nullptr;
  }
  batch_index_offset_ = 0;
  batch_vertex_count_ = 0;
  batch_index_count_ = 0;
  batch_has_index_buffer_ = false;
  batch_open_ = false;
}

}  // namespace rex::ui::metal
