#include <rex/ui/metal/presenter.h>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/surface.h>
#include <rex/ui/surface_macos.h>

#include <algorithm>
#include <cstring>
#include <utility>

REXCVAR_DEFINE_BOOL(metal_presenter_clear_test, false, "UI/Metal",
                    "Submit a simple Metal clear/present command for presentation smoke testing")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::ui::metal {
namespace {

constexpr const char* kGuestPresentMetalSource = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
  float4 position [[position]];
  float2 uv [[user(locn0)]];
};

vertex VertexOut guest_present_vs(uint vertex_id [[vertex_id]]) {
  constexpr float2 positions[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0),
  };
  VertexOut out;
  float2 position = positions[vertex_id];
  out.position = float4(position, 0.0, 1.0);
  out.uv = float2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);
  return out;
}

fragment float4 guest_present_ps(VertexOut in [[stage_in]],
                                 texture2d<float> guest_texture [[texture(0)]]) {
  constexpr sampler guest_sampler(coord::normalized, address::clamp_to_edge, filter::linear);
  return guest_texture.sample(guest_sampler, in.uv);
}
)";

class MetalGuestOutputRefreshContext final : public Presenter::GuestOutputRefreshContext {
 public:
  explicit MetalGuestOutputRefreshContext(bool& is_8bpc_out_ref)
      : GuestOutputRefreshContext(is_8bpc_out_ref) {}
};

}  // namespace

std::unique_ptr<MetalPresenter> MetalPresenter::Create(void* metal_device,
                                                       HostGpuLossCallback host_gpu_loss_callback) {
  if (!metal_device) {
    return nullptr;
  }
  std::unique_ptr<MetalPresenter> presenter(
      new MetalPresenter(metal_device, std::move(host_gpu_loss_callback)));
  if (!presenter->InitializeCommonSurfaceIndependent()) {
    return nullptr;
  }
  return presenter;
}

MetalPresenter::MetalPresenter(void* metal_device, HostGpuLossCallback host_gpu_loss_callback)
    : Presenter(std::move(host_gpu_loss_callback)), metal_device_(metal_device) {
  [(id)metal_device_ retain];
}

MetalPresenter::~MetalPresenter() {
  DisconnectPaintingFromSurfaceFromUIThreadImpl();
  if (command_queue_) {
    [(id)command_queue_ release];
    command_queue_ = nullptr;
  }
  if (guest_texture_) {
    [(id)guest_texture_ release];
    guest_texture_ = nullptr;
  }
  if (guest_pipeline_state_) {
    [(id)guest_pipeline_state_ release];
    guest_pipeline_state_ = nullptr;
  }
  if (metal_device_) {
    [(id)metal_device_ release];
    metal_device_ = nullptr;
  }
}

Surface::TypeFlags MetalPresenter::GetSupportedSurfaceTypes() const {
  return Surface::kTypeFlag_CAMetalLayer;
}

bool MetalPresenter::CaptureGuestOutput(RawImage& image_out) {
  (void)image_out;
  return false;
}

void MetalPresenter::UpdateGuestFrontbuffer(uint32_t width, uint32_t height, const void* pixels,
                                            size_t row_pitch) {
  if (!width || !height || !pixels || row_pitch < size_t(width) * 4) {
    return;
  }

  std::lock_guard<std::mutex> lock(guest_frame_mutex_);
  guest_frame_width_ = width;
  guest_frame_height_ = height;
  size_t packed_pitch = size_t(width) * 4;
  guest_frame_bgra_.resize(packed_pitch * height);
  auto* dst = guest_frame_bgra_.data();
  auto* src = static_cast<const uint8_t*>(pixels);
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(dst + size_t(y) * packed_pitch, src + size_t(y) * row_pitch, packed_pitch);
  }
}

Presenter::SurfacePaintConnectResult
MetalPresenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(Surface& new_surface,
                                                                uint32_t new_surface_width,
                                                                uint32_t new_surface_height,
                                                                bool was_paintable,
                                                                bool& is_vsync_implicit_out) {
  (void)was_paintable;
  if (new_surface.GetType() != Surface::kTypeIndex_CAMetalLayer) {
    return SurfacePaintConnectResult::kFailureSurfaceUnusable;
  }

  auto& metal_surface = static_cast<MacOSMetalLayerSurface&>(new_surface);
  CAMetalLayer* layer = (CAMetalLayer*)metal_surface.metal_layer();
  if (!layer) {
    return SurfacePaintConnectResult::kFailureSurfaceUnusable;
  }

  [layer setDevice:(id<MTLDevice>)metal_device_];
  [layer setPixelFormat:MTLPixelFormatBGRA8Unorm];
  [layer setFramebufferOnly:NO];
  [layer setDrawableSize:CGSizeMake(new_surface_width, new_surface_height)];

  if (metal_layer_ != layer) {
    if (metal_layer_) {
      [(id)metal_layer_ release];
    }
    metal_layer_ = layer;
    [(id)metal_layer_ retain];
  }

  // CAMetalLayer presentation is synchronized by Core Animation.
  is_vsync_implicit_out = true;
  return SurfacePaintConnectResult::kSuccess;
}

void MetalPresenter::DisconnectPaintingFromSurfaceFromUIThreadImpl() {
  if (metal_layer_) {
    [(id)metal_layer_ release];
    metal_layer_ = nullptr;
  }
}

bool MetalPresenter::RefreshGuestOutputImpl(
    uint32_t mailbox_index, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
    std::function<bool(GuestOutputRefreshContext& context)> refresher, bool& is_8bpc_out_ref) {
  (void)mailbox_index;
  (void)frontbuffer_width;
  (void)frontbuffer_height;
  MetalGuestOutputRefreshContext context(is_8bpc_out_ref);
  return refresher(context);
}

bool MetalPresenter::EnsureGuestPipeline() {
  if (guest_pipeline_state_) {
    return true;
  }

  id<MTLDevice> device = (id<MTLDevice>)metal_device_;
  NSError* error = nil;
  NSString* source = [NSString stringWithUTF8String:kGuestPresentMetalSource];
  id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
  if (!library) {
    const char* error_text = error ? [[error localizedDescription] UTF8String] : "unknown error";
    REXLOG_ERROR("MetalPresenter: failed to compile guest present shaders: {}", error_text);
    return false;
  }

  id<MTLFunction> vertex_function = [library newFunctionWithName:@"guest_present_vs"];
  id<MTLFunction> fragment_function = [library newFunctionWithName:@"guest_present_ps"];
  MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = vertex_function;
  descriptor.fragmentFunction = fragment_function;
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  guest_pipeline_state_ = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  [descriptor release];
  [vertex_function release];
  [fragment_function release];
  [library release];

  if (!guest_pipeline_state_) {
    const char* error_text = error ? [[error localizedDescription] UTF8String] : "unknown error";
    REXLOG_ERROR("MetalPresenter: failed to create guest present pipeline: {}", error_text);
    return false;
  }
  return true;
}

Presenter::PaintResult MetalPresenter::PaintAndPresentImpl(bool execute_ui_drawers) {
  (void)execute_ui_drawers;
  if (!metal_layer_) {
    return PaintResult::kPresented;
  }
  if (!command_queue_) {
    command_queue_ = [(id<MTLDevice>)metal_device_ newCommandQueue];
    if (!command_queue_) {
      return PaintResult::kNotPresented;
    }
  }

  @autoreleasepool {
    id<CAMetalDrawable> drawable = [(CAMetalLayer*)metal_layer_ nextDrawable];
    if (!drawable) {
      return PaintResult::kNotPresented;
    }

    id<MTLCommandBuffer> command_buffer = [(id<MTLCommandQueue>)command_queue_ commandBuffer];

    {
      std::lock_guard<std::mutex> lock(guest_frame_mutex_);
      if (!guest_frame_bgra_.empty() && guest_frame_width_ && guest_frame_height_) {
        if (!guest_texture_ || guest_texture_width_ != guest_frame_width_ ||
            guest_texture_height_ != guest_frame_height_) {
          if (guest_texture_) {
            [(id)guest_texture_ release];
          }
          MTLTextureDescriptor* texture_desc =
              [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                 width:guest_frame_width_
                                                                height:guest_frame_height_
                                                             mipmapped:NO];
          texture_desc.usage = MTLTextureUsageShaderRead;
          texture_desc.storageMode = MTLStorageModeShared;
          guest_texture_ = [(id<MTLDevice>)metal_device_ newTextureWithDescriptor:texture_desc];
          guest_texture_width_ = guest_frame_width_;
          guest_texture_height_ = guest_frame_height_;
        }
        if (guest_texture_) {
          MTLRegion region = MTLRegionMake2D(0, 0, guest_frame_width_, guest_frame_height_);
          [(id<MTLTexture>)guest_texture_ replaceRegion:region
                                            mipmapLevel:0
                                              withBytes:guest_frame_bgra_.data()
                                            bytesPerRow:size_t(guest_frame_width_) * 4];
        }
      }
    }

    id<MTLTexture> drawable_texture = [drawable texture];
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable_texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.015, 0.015, 0.018, 1.0);
    id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
    if (guest_texture_) {
      if (EnsureGuestPipeline()) {
        double drawable_width = double([drawable_texture width]);
        double drawable_height = double([drawable_texture height]);
        double guest_width = double([(id<MTLTexture>)guest_texture_ width]);
        double guest_height = double([(id<MTLTexture>)guest_texture_ height]);
        double scale = std::min(drawable_width / guest_width, drawable_height / guest_height);
        double viewport_width = guest_width * scale;
        double viewport_height = guest_height * scale;
        MTLViewport viewport = {(drawable_width - viewport_width) * 0.5,
                                (drawable_height - viewport_height) * 0.5,
                                viewport_width,
                                viewport_height,
                                0.0,
                                1.0};
        [encoder setViewport:viewport];
        [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)guest_pipeline_state_];
        [encoder setFragmentTexture:(id<MTLTexture>)guest_texture_ atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      }
    }
    [encoder endEncoding];

    [command_buffer presentDrawable:drawable];
    [command_buffer commit];
  }

  return PaintResult::kPresented;
}

}  // namespace rex::ui::metal
