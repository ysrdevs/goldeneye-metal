#include <rex/graphics/metal/draw_renderer.h>

#import <Metal/Metal.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <rex/logging.h>

namespace rex::graphics::metal {
namespace {

struct DiagnosticUniforms {
  uint32_t draw_count;
  uint32_t swap_count;
  float width;
  float height;
};

struct DrawEventUniforms {
  uint32_t draw_count;
  uint32_t swap_count;
  uint32_t event_count;
  uint32_t _pad_0;
  float width;
  float height;
  float _pad_1;
  float _pad_2;
};

struct HostUniforms {
  float width;
  float height;
  float _pad_0;
  float _pad_1;
};

constexpr const char* kDiagnosticMetalSource = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
  float4 position [[position]];
  float2 uv;
};

struct DiagnosticUniforms {
  uint draw_count;
  uint swap_count;
  float width;
  float height;
};

struct DrawEventUniforms {
  uint draw_count;
  uint swap_count;
  uint event_count;
  uint _pad_0;
  float width;
  float height;
  float _pad_1;
  float _pad_2;
};

struct HostUniforms {
  float width;
  float height;
  float _pad_0;
  float _pad_1;
};

struct DrawEvent {
  uint primitive_type;
  uint index_count;
  uint vertex_binding_count;
  uint texture_binding_count;
  ulong vertex_shader_hash;
  ulong pixel_shader_hash;
};

struct HostVertex {
  float4 position;
  float4 color;
  float4 viewport;
  float4 texcoord;
  float4 interpolators[16];
  uint interpolator_mask;
  uint3 _pad_interpolators;
};

vertex VertexOut diagnostic_vs(uint vertex_id [[vertex_id]]) {
  constexpr float2 positions[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0),
  };
  VertexOut out;
  out.position = float4(positions[vertex_id], 0.0, 1.0);
  out.uv = positions[vertex_id] * 0.5 + 0.5;
  return out;
}

fragment float4 diagnostic_ps(VertexOut in [[stage_in]],
                              constant DiagnosticUniforms& uniforms [[buffer(0)]]) {
  float2 uv = clamp(in.uv, float2(0.0), float2(1.0));
  float draw_phase = fmod(float(uniforms.draw_count), 257.0) / 256.0;
  float swap_phase = fmod(float(uniforms.swap_count), 131.0) / 130.0;
  float grid = step(0.985, fract(uv.x * 24.0)) + step(0.985, fract(uv.y * 14.0));
  float3 color = float3(uv.x, uv.y, 0.18 + 0.65 * draw_phase);
  color = mix(color, float3(1.0, 0.92, 0.25), min(grid, 1.0) * 0.35);
  color.r = mix(color.r, 0.25 + 0.75 * swap_phase, 0.28);
  return float4(color, 1.0);
}

struct EventVertexOut {
  float4 position [[position]];
  float3 color;
  float intensity;
};

static float hash_component(ulong hash_value, uint shift) {
  return float((hash_value >> shift) & 255ul) / 255.0;
}

vertex EventVertexOut draw_event_vs(uint vertex_id [[vertex_id]],
                                    uint instance_id [[instance_id]],
                                    constant DrawEventUniforms& uniforms [[buffer(0)]],
                                    constant DrawEvent* events [[buffer(1)]]) {
  DrawEvent event = events[instance_id];
  float event_count = max(float(uniforms.event_count), 1.0);
  float lane_width = 2.0 / event_count;
  float x0 = -1.0 + lane_width * float(instance_id);
  float x1 = x0 + lane_width * 0.86;
  float height_phase = fmod(float(event.index_count), 97.0) / 97.0;
  float y0 = -1.0;
  float y1 = mix(-0.58, 0.96, height_phase);

  constexpr float2 corners[4] = {
    float2(0.0, 0.0),
    float2(1.0, 0.0),
    float2(0.0, 1.0),
    float2(1.0, 1.0),
  };
  float2 corner = corners[vertex_id];

  EventVertexOut out;
  out.position = float4(mix(x0, x1, corner.x), mix(y0, y1, corner.y), 0.0, 1.0);
  float primitive_phase = fmod(float(event.primitive_type), 11.0) / 10.0;
  out.color = float3(hash_component(event.vertex_shader_hash, 0),
                     hash_component(event.pixel_shader_hash, 16),
                     0.25 + 0.7 * primitive_phase);
  out.color = mix(out.color, float3(0.96, 0.84, 0.24),
                  min(float(event.vertex_binding_count + event.texture_binding_count) / 12.0,
                      0.45));
  out.intensity = 0.42 + 0.58 * corner.y;
  return out;
}

fragment float4 draw_event_ps(EventVertexOut in [[stage_in]]) {
  return float4(saturate(in.color * in.intensity), 1.0);
}

struct HostVertexOut {
  float4 position [[position]];
  float4 color;
  float2 uv;
  float texture_weight;
};

vertex HostVertexOut host_triangle_vs(uint vertex_id [[vertex_id]],
                                      constant HostVertex* vertices [[buffer(0)]],
                                      constant HostUniforms& uniforms [[buffer(1)]]) {
  (void)uniforms;
  HostVertex host_vertex = vertices[vertex_id];
  HostVertexOut out;
  out.position = host_vertex.position;
  out.color = host_vertex.color;
  out.uv = host_vertex.texcoord.xy;
  out.texture_weight = host_vertex.texcoord.z;
  return out;
}

fragment float4 host_triangle_ps(HostVertexOut in [[stage_in]]) {
  return saturate(in.color);
}

fragment float4 host_textured_triangle_ps(HostVertexOut in [[stage_in]],
                                          texture2d<float> source_texture [[texture(0)]],
                                          sampler source_sampler [[sampler(0)]]) {
  float4 color = saturate(in.color);
  float4 sampled = source_texture.sample(source_sampler, clamp(in.uv, float2(0.0), float2(1.0)));
  return mix(color, sampled, saturate(in.texture_weight));
}
)";

}  // namespace

std::unique_ptr<MetalDrawRenderer> MetalDrawRenderer::Create(void* metal_device) {
  if (!metal_device) {
    return nullptr;
  }
  std::unique_ptr<MetalDrawRenderer> renderer(new MetalDrawRenderer(metal_device));
  if (!renderer->Initialize()) {
    return nullptr;
  }
  return renderer;
}

MetalDrawRenderer::MetalDrawRenderer(void* metal_device) : metal_device_(metal_device) {
  [(id)metal_device_ retain];
}

MetalDrawRenderer::~MetalDrawRenderer() {
  if (render_texture_) {
    [(id)render_texture_ release];
    render_texture_ = nullptr;
  }
  if (pipeline_state_) {
    [(id)pipeline_state_ release];
    pipeline_state_ = nullptr;
  }
  if (event_pipeline_state_) {
    [(id)event_pipeline_state_ release];
    event_pipeline_state_ = nullptr;
  }
  if (host_triangle_pipeline_state_) {
    [(id)host_triangle_pipeline_state_ release];
    host_triangle_pipeline_state_ = nullptr;
  }
  if (host_textured_triangle_pipeline_state_) {
    [(id)host_textured_triangle_pipeline_state_ release];
    host_textured_triangle_pipeline_state_ = nullptr;
  }
  if (command_queue_) {
    [(id)command_queue_ release];
    command_queue_ = nullptr;
  }
  if (metal_device_) {
    [(id)metal_device_ release];
    metal_device_ = nullptr;
  }
}

bool MetalDrawRenderer::Initialize() {
  id<MTLDevice> device = (id<MTLDevice>)metal_device_;
  command_queue_ = [device newCommandQueue];
  if (!command_queue_) {
    REXLOG_ERROR("MetalDrawRenderer: failed to create command queue");
    return false;
  }

  NSError* error = nil;
  NSString* source = [NSString stringWithUTF8String:kDiagnosticMetalSource];
  id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
  if (!library) {
    const char* error_text = error ? [[error localizedDescription] UTF8String] : "unknown error";
    std::fprintf(stderr, "[metal] MetalDrawRenderer library compile failed: %s\n", error_text);
    std::fflush(stderr);
    REXLOG_ERROR("MetalDrawRenderer: failed to compile diagnostic shaders: {}", error_text);
    return false;
  }

  id<MTLFunction> vertex_function = [library newFunctionWithName:@"diagnostic_vs"];
  id<MTLFunction> fragment_function = [library newFunctionWithName:@"diagnostic_ps"];
  id<MTLFunction> event_vertex_function = [library newFunctionWithName:@"draw_event_vs"];
  id<MTLFunction> event_fragment_function = [library newFunctionWithName:@"draw_event_ps"];
  id<MTLFunction> host_vertex_function = [library newFunctionWithName:@"host_triangle_vs"];
  id<MTLFunction> host_fragment_function = [library newFunctionWithName:@"host_triangle_ps"];
  id<MTLFunction> host_textured_fragment_function =
      [library newFunctionWithName:@"host_textured_triangle_ps"];
  MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = vertex_function;
  descriptor.fragmentFunction = fragment_function;
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  pipeline_state_ = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  descriptor.vertexFunction = event_vertex_function;
  descriptor.fragmentFunction = event_fragment_function;
  event_pipeline_state_ = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  descriptor.vertexFunction = host_vertex_function;
  descriptor.fragmentFunction = host_fragment_function;
  host_triangle_pipeline_state_ = [device newRenderPipelineStateWithDescriptor:descriptor
                                                                         error:&error];
  descriptor.fragmentFunction = host_textured_fragment_function;
  host_textured_triangle_pipeline_state_ = [device newRenderPipelineStateWithDescriptor:descriptor
                                                                                  error:&error];
  [descriptor release];
  [vertex_function release];
  [fragment_function release];
  [event_vertex_function release];
  [event_fragment_function release];
  [host_vertex_function release];
  [host_fragment_function release];
  [host_textured_fragment_function release];
  [library release];

  if (!pipeline_state_) {
    const char* error_text = error ? [[error localizedDescription] UTF8String] : "unknown error";
    std::fprintf(stderr, "[metal] MetalDrawRenderer diagnostic pipeline failed: %s\n", error_text);
    std::fflush(stderr);
    REXLOG_ERROR("MetalDrawRenderer: failed to create diagnostic pipeline: {}", error_text);
    return false;
  }
  if (!event_pipeline_state_) {
    const char* error_text = error ? [[error localizedDescription] UTF8String] : "unknown error";
    std::fprintf(stderr, "[metal] MetalDrawRenderer draw event pipeline failed: %s\n", error_text);
    std::fflush(stderr);
    REXLOG_ERROR("MetalDrawRenderer: failed to create draw event pipeline: {}", error_text);
    return false;
  }
  if (!host_triangle_pipeline_state_) {
    const char* error_text = error ? [[error localizedDescription] UTF8String] : "unknown error";
    std::fprintf(stderr, "[metal] MetalDrawRenderer host triangle pipeline unavailable: %s\n",
                 error_text);
    std::fflush(stderr);
    REXLOG_WARN("MetalDrawRenderer: failed to create host triangle pipeline: {}", error_text);
  }
  if (!host_textured_triangle_pipeline_state_) {
    const char* error_text = error ? [[error localizedDescription] UTF8String] : "unknown error";
    std::fprintf(stderr, "[metal] MetalDrawRenderer host textured pipeline unavailable: %s\n",
                 error_text);
    std::fflush(stderr);
    REXLOG_WARN("MetalDrawRenderer: failed to create host textured pipeline: {}", error_text);
  }
  std::fprintf(stderr, "[metal] MetalDrawRenderer initialized (host_triangles=%u textured=%u)\n",
               host_triangle_pipeline_state_ ? 1u : 0u,
               host_textured_triangle_pipeline_state_ ? 1u : 0u);
  std::fflush(stderr);
  return true;
}

bool MetalDrawRenderer::RenderDiagnosticFrame(uint32_t width, uint32_t height, uint32_t draw_count,
                                              uint32_t swap_count, std::vector<uint8_t>& bgra_out) {
  static const std::vector<MetalDrawEvent> no_events;
  static const std::vector<MetalHostVertex> no_host_vertices;
  return RenderDrawEventFrame(width, height, draw_count, swap_count, no_events, no_host_vertices,
                              bgra_out);
}

bool MetalDrawRenderer::RenderDrawEventFrame(uint32_t width, uint32_t height, uint32_t draw_count,
                                             uint32_t swap_count,
                                             const std::vector<MetalDrawEvent>& events,
                                             const std::vector<MetalHostVertex>& host_vertices,
                                             std::vector<uint8_t>& bgra_out,
                                             const MetalHostTexture* host_texture) {
  if (!width || !height || !command_queue_ || !pipeline_state_) {
    return false;
  }

  id<MTLDevice> device = (id<MTLDevice>)metal_device_;
  if (!render_texture_ || render_texture_width_ != width || render_texture_height_ != height) {
    if (render_texture_) {
      [(id)render_texture_ release];
      render_texture_ = nullptr;
    }
    MTLTextureDescriptor* texture_descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    texture_descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    texture_descriptor.storageMode = MTLStorageModeShared;
    render_texture_ = [device newTextureWithDescriptor:texture_descriptor];
    render_texture_width_ = width;
    render_texture_height_ = height;
    if (!render_texture_) {
      return false;
    }
  }

  uint32_t host_vertex_count = std::min<uint32_t>(uint32_t(host_vertices.size()),
                                                  MetalDrawRenderer::kMaxHostVerticesPerFrame);
  host_vertex_count -= host_vertex_count % 3;

  id<MTLBuffer> host_vertex_buffer = nil;
  bool host_texture_valid = host_texture && host_texture->rgba && host_texture->width &&
                            host_texture->height &&
                            host_texture->bytes_per_row >= size_t(host_texture->width) * 4 &&
                            host_textured_triangle_pipeline_state_;
  id<MTLTexture> host_source_texture = nil;
  if (host_vertex_count) {
    size_t host_vertex_bytes = size_t(host_vertex_count) * sizeof(MetalHostVertex);
    host_vertex_buffer = [device newBufferWithLength:host_vertex_bytes
                                             options:MTLResourceStorageModeShared];
    if (host_vertex_buffer) {
      std::memcpy([host_vertex_buffer contents], host_vertices.data(), host_vertex_bytes);
    } else {
      host_vertex_count = 0;
    }
  }
  if (host_vertex_count && host_texture_valid) {
    MTLTextureDescriptor* texture_descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:host_texture->width
                                                          height:host_texture->height
                                                       mipmapped:NO];
    texture_descriptor.usage = MTLTextureUsageShaderRead;
    texture_descriptor.storageMode = MTLStorageModeShared;
    host_source_texture = [device newTextureWithDescriptor:texture_descriptor];
    if (host_source_texture) {
      MTLRegion source_region = MTLRegionMake2D(0, 0, host_texture->width, host_texture->height);
      [host_source_texture replaceRegion:source_region
                             mipmapLevel:0
                               withBytes:host_texture->rgba
                             bytesPerRow:host_texture->bytes_per_row];
    } else {
      host_texture_valid = false;
    }
  }

  uint32_t event_count =
      std::min<uint32_t>(uint32_t(events.size()), MetalDrawRenderer::kMaxDrawEventsPerFrame);
  id<MTLBuffer> event_buffer = nil;
  if (!host_vertex_count && event_count) {
    size_t event_bytes = size_t(event_count) * sizeof(MetalDrawEvent);
    event_buffer = [device newBufferWithLength:event_bytes options:MTLResourceStorageModeShared];
    if (event_buffer) {
      std::memcpy([event_buffer contents], events.data(), event_bytes);
    } else {
      event_count = 0;
    }
  }

  id<MTLCommandBuffer> command_buffer = [(id<MTLCommandQueue>)command_queue_ commandBuffer];
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = (id<MTLTexture>)render_texture_;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.015, 0.02, 0.028, 1.0);

  id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
  if (!host_vertex_count) {
    [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline_state_];
    DiagnosticUniforms uniforms = {draw_count, swap_count, float(width), float(height)};
    [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
  }
  if (host_vertex_count && (host_triangle_pipeline_state_ || host_texture_valid)) {
    [encoder
        setRenderPipelineState:
            (id<MTLRenderPipelineState>)(host_texture_valid ? host_textured_triangle_pipeline_state_
                                                            : host_triangle_pipeline_state_)];
    HostUniforms host_uniforms = {float(width), float(height), 0.0f, 0.0f};
    [encoder setVertexBuffer:host_vertex_buffer offset:0 atIndex:0];
    [encoder setVertexBytes:&host_uniforms length:sizeof(host_uniforms) atIndex:1];
    if (host_texture_valid) {
      MTLSamplerDescriptor* sampler_descriptor = [[MTLSamplerDescriptor alloc] init];
      sampler_descriptor.minFilter = MTLSamplerMinMagFilterLinear;
      sampler_descriptor.magFilter = MTLSamplerMinMagFilterLinear;
      sampler_descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
      sampler_descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
      sampler_descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
      id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:sampler_descriptor];
      [sampler_descriptor release];
      [encoder setFragmentTexture:host_source_texture atIndex:0];
      [encoder setFragmentSamplerState:sampler atIndex:0];
      [sampler release];
    }
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:host_vertex_count];
  }
  if (!host_vertex_count && event_count && event_pipeline_state_) {
    DrawEventUniforms event_uniforms = {draw_count,   swap_count,    event_count, 0,
                                        float(width), float(height), 0.0f,        0.0f};
    [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)event_pipeline_state_];
    [encoder setVertexBytes:&event_uniforms length:sizeof(event_uniforms) atIndex:0];
    [encoder setVertexBuffer:event_buffer offset:0 atIndex:1];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip
                vertexStart:0
                vertexCount:4
              instanceCount:event_count];
  }
  [encoder endEncoding];
  [command_buffer commit];
  [command_buffer waitUntilCompleted];

  if (host_vertex_buffer) {
    [host_vertex_buffer release];
  }
  if (event_buffer) {
    [event_buffer release];
  }
  if (host_source_texture) {
    [host_source_texture release];
  }

  if ([command_buffer status] == MTLCommandBufferStatusError) {
    NSError* error = [command_buffer error];
    REXLOG_ERROR("MetalDrawRenderer: diagnostic command buffer failed: {}",
                 error ? [[error localizedDescription] UTF8String] : "unknown error");
    return false;
  }

  bgra_out.resize(size_t(width) * height * 4);
  MTLRegion region = MTLRegionMake2D(0, 0, width, height);
  [(id<MTLTexture>)render_texture_ getBytes:bgra_out.data()
                                bytesPerRow:size_t(width) * 4
                                 fromRegion:region
                                mipmapLevel:0];
  return true;
}

}  // namespace rex::graphics::metal
