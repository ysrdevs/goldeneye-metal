#include <rex/ui/metal/presenter.h>

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <rex/cvar.h>
#include <rex/chrono/clock.h>
#include <rex/graphics/flags.h>
#include <rex/logging.h>
#include <rex/ui/surface.h>
#include <rex/ui/surface_macos.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

REXCVAR_DEFINE_BOOL(metal_presenter_clear_test, false, "UI/Metal",
                    "Submit a simple Metal clear/present command for presentation smoke testing")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(metal_show_fps, true, "UI/Metal",
                    "Show the guest frame rate in the Metal output");

namespace rex::ui::metal {
namespace {

namespace profiling = rex::graphics::metal::profiling;

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

constexpr std::array<uint8_t, 7> kFpsGlyphSpace = {};
constexpr std::array<uint8_t, 7> kFpsGlyphDot = {0b00000, 0b00000, 0b00000, 0b00000,
                                                 0b00000, 0b00110, 0b00110};
constexpr std::array<uint8_t, 7> kFpsGlyphF = {0b11111, 0b10000, 0b10000, 0b11110,
                                               0b10000, 0b10000, 0b10000};
constexpr std::array<uint8_t, 7> kFpsGlyphP = {0b11110, 0b10001, 0b10001, 0b11110,
                                               0b10000, 0b10000, 0b10000};
constexpr std::array<uint8_t, 7> kFpsGlyphS = {0b01111, 0b10000, 0b10000, 0b01110,
                                               0b00001, 0b00001, 0b11110};
constexpr std::array<std::array<uint8_t, 7>, 10> kFpsDigitGlyphs = {{
    {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110},
    {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110},
    {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111},
    {0b11110, 0b00001, 0b00001, 0b01110, 0b00001, 0b00001, 0b11110},
    {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010},
    {0b11111, 0b10000, 0b10000, 0b11110, 0b00001, 0b00001, 0b11110},
    {0b01110, 0b10000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110},
    {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000},
    {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110},
    {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00001, 0b01110},
}};

const std::array<uint8_t, 7>& GetFpsGlyph(char character) {
  if (character >= '0' && character <= '9') {
    return kFpsDigitGlyphs[size_t(character - '0')];
  }
  switch (character) {
    case 'F':
      return kFpsGlyphF;
    case 'P':
      return kFpsGlyphP;
    case 'S':
      return kFpsGlyphS;
    case '.':
      return kFpsGlyphDot;
    default:
      return kFpsGlyphSpace;
  }
}

bool GetPackedBgraLayout(uint32_t width, uint32_t height, size_t& row_pitch_out, size_t& size_out) {
  if (!width || !height) {
    return false;
  }
  row_pitch_out = size_t(width) * 4;
  if (size_t(height) > std::numeric_limits<size_t>::max() / row_pitch_out) {
    return false;
  }
  size_out = row_pitch_out * height;
  return true;
}

uint64_t HashGuestFrame(const uint8_t* data, size_t size, uint32_t width, uint32_t height) {
  uint64_t hash = UINT64_C(0x9E3779B185EBCA87) ^ uint64_t(size) ^ (uint64_t(width) << 32) ^ height;
  while (size >= sizeof(uint64_t)) {
    uint64_t word;
    std::memcpy(&word, data, sizeof(word));
    word ^= word >> 33;
    word *= UINT64_C(0xC2B2AE3D27D4EB4F);
    word ^= word >> 29;
    hash ^= word;
    hash =
        ((hash << 27) | (hash >> 37)) * UINT64_C(0x9E3779B185EBCA87) + UINT64_C(0x165667B19E3779F9);
    data += sizeof(uint64_t);
    size -= sizeof(uint64_t);
  }
  uint64_t tail = 0;
  if (size) {
    std::memcpy(&tail, data, size);
    hash ^= tail * UINT64_C(0xC2B2AE3D27D4EB4F);
  }
  hash ^= hash >> 33;
  hash *= UINT64_C(0xFF51AFD7ED558CCD);
  hash ^= hash >> 33;
  return hash;
}

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
  size_t packed_pitch = 0;
  size_t packed_size = 0;
  if (!pixels || !GetPackedBgraLayout(width, height, packed_pitch, packed_size) ||
      row_pitch < packed_pitch) {
    return;
  }

  std::lock_guard<std::mutex> lock(guest_frame_mutex_);
  guest_frame_width_ = width;
  guest_frame_height_ = height;
  guest_frame_bgra_.resize(packed_size);
  auto* dst = guest_frame_bgra_.data();
  auto* src = static_cast<const uint8_t*>(pixels);
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(dst + size_t(y) * packed_pitch, src + size_t(y) * row_pitch, packed_pitch);
  }
  FinalizeGuestFrameLocked();
}

void MetalPresenter::UpdateGuestFrontbuffer(uint32_t width, uint32_t height,
                                            std::vector<uint8_t>&& packed_bgra) {
  size_t packed_pitch = 0;
  size_t packed_size = 0;
  if (!GetPackedBgraLayout(width, height, packed_pitch, packed_size) ||
      packed_bgra.size() < packed_size) {
    return;
  }

  std::lock_guard<std::mutex> lock(guest_frame_mutex_);
  guest_frame_bgra_ = std::move(packed_bgra);
  guest_frame_bgra_.resize(packed_size);
  guest_frame_width_ = width;
  guest_frame_height_ = height;
  FinalizeGuestFrameLocked();
}

void MetalPresenter::FinalizeGuestFrameLocked() {
  uint64_t now = rex::chrono::Clock::QueryHostTickCount();
  uint64_t tick_frequency = rex::chrono::Clock::QueryHostTickFrequency();
  if (!guest_fps_sample_start_tick_ || now < guest_fps_sample_start_tick_) {
    guest_fps_sample_start_tick_ = now;
    guest_fps_sample_frame_count_ = 0;
  } else {
    ++guest_fps_sample_frame_count_;
    uint64_t elapsed = now - guest_fps_sample_start_tick_;
    if (tick_frequency && elapsed >= tick_frequency / 2) {
      guest_fps_ = double(guest_fps_sample_frame_count_) * double(tick_frequency) / double(elapsed);
      guest_fps_sample_start_tick_ = now;
      guest_fps_sample_frame_count_ = 0;
    }
  }

  if (profile_enabled_) {
    uint64_t source_hash = HashGuestFrame(guest_frame_bgra_.data(), guest_frame_bgra_.size(),
                                          guest_frame_width_, guest_frame_height_);
    bool unchanged = profile_last_source_valid_ && source_hash == profile_last_source_hash_ &&
                     guest_frame_width_ == profile_last_source_width_ &&
                     guest_frame_height_ == profile_last_source_height_;
    profile_last_source_hash_ = source_hash;
    profile_last_source_width_ = guest_frame_width_;
    profile_last_source_height_ = guest_frame_height_;
    profile_last_source_valid_ = true;
    std::lock_guard<std::mutex> profile_lock(profile_mutex_);
    profile_window_.RecordSource(unchanged);
  }

  if (REXCVAR_GET(metal_show_fps)) {
    DrawGuestFpsOverlayLocked();
  }
  ++guest_frame_generation_;
}

void MetalPresenter::EndProfiledPresentAttempt(bool drawable_nil, uint64_t present_commit_ns) {
  if (!profile_enabled_) {
    return;
  }
  std::lock_guard<std::mutex> lock(profile_mutex_);
  if (drawable_nil) {
    profile_window_.RecordDrawableNil();
  } else {
    profile_window_.Record(profiling::PresenterEvent::kPresentCommit, present_commit_ns);
    profile_window_.RecordCommit();
  }
  ++profiled_present_attempt_count_;
  if (profile_window_.EndAttempt()) {
    ReportProfileWindowLocked();
    profile_window_.Reset();
  }
}

void MetalPresenter::ReportProfileWindowLocked() {
  uint64_t first_attempt = profiled_present_attempt_count_ - profile_window_.attempt_count() + 1;
  std::fprintf(stderr,
               "[metal-profile] presenter attempts=%llu-%llu sources=%llu unchanged_sources=%llu "
               "drawable_nil=%llu uploads=%llu upload_bytes=%llu commits=%llu\n",
               static_cast<unsigned long long>(first_attempt),
               static_cast<unsigned long long>(profiled_present_attempt_count_),
               static_cast<unsigned long long>(profile_window_.source_count()),
               static_cast<unsigned long long>(profile_window_.unchanged_source_count()),
               static_cast<unsigned long long>(profile_window_.drawable_nil_count()),
               static_cast<unsigned long long>(profile_window_.upload_count()),
               static_cast<unsigned long long>(profile_window_.upload_byte_count()),
               static_cast<unsigned long long>(profile_window_.commit_count()));
  for (size_t event_index = 0; event_index < size_t(profiling::PresenterEvent::kCount);
       ++event_index) {
    profiling::PresenterEvent event = profiling::PresenterEvent(event_index);
    const profiling::DurationWindow& metric = profile_window_.event(event);
    std::fprintf(
        stderr,
        "[metal-profile] presenter attempts=%llu-%llu event=%s calls=%llu total_ns=%llu "
        "avg_ns_per_attempt=%llu max_call_ns=%llu max_attempt_ns=%llu "
        "max_calls_per_attempt=%llu\n",
        static_cast<unsigned long long>(first_attempt),
        static_cast<unsigned long long>(profiled_present_attempt_count_),
        profiling::PresenterEventName(event),
        static_cast<unsigned long long>(metric.total.call_count),
        static_cast<unsigned long long>(metric.total.total_ns),
        static_cast<unsigned long long>(metric.total.total_ns / profile_window_.attempt_count()),
        static_cast<unsigned long long>(metric.total.max_call_ns),
        static_cast<unsigned long long>(metric.max_swap_ns),
        static_cast<unsigned long long>(metric.max_calls_per_swap));
  }
  std::fflush(stderr);
}

void MetalPresenter::DrawGuestFpsOverlayLocked() {
  if (!guest_frame_width_ || !guest_frame_height_ ||
      guest_frame_bgra_.size() < size_t(guest_frame_width_) * guest_frame_height_ * 4) {
    return;
  }

  char label[16];
  double displayed_fps = std::min(guest_fps_, 999.9);
  int label_length = std::snprintf(label, sizeof(label), "FPS %.1f", displayed_fps);
  if (label_length <= 0) {
    return;
  }
  size_t character_count = std::min<size_t>(size_t(label_length), sizeof(label) - 1);
  uint32_t scale = guest_frame_width_ >= 960 && guest_frame_height_ >= 540
                       ? 3
                       : (guest_frame_width_ >= 480 && guest_frame_height_ >= 270 ? 2 : 1);
  constexpr uint32_t kGlyphWidth = 5;
  constexpr uint32_t kGlyphHeight = 7;
  uint32_t padding = scale * 2;
  uint32_t character_advance = (kGlyphWidth + 1) * scale;
  uint32_t label_width =
      character_count ? uint32_t(character_count) * character_advance - scale : 0;
  uint32_t box_width = label_width + padding * 2;
  uint32_t box_height = kGlyphHeight * scale + padding * 2;
  if (box_width > guest_frame_width_ || box_height > guest_frame_height_) {
    return;
  }
  uint32_t box_x =
      guest_frame_width_ - box_width - std::min(padding, guest_frame_width_ - box_width);
  uint32_t box_y = std::min(padding, guest_frame_height_ - box_height);

  for (uint32_t y = box_y; y < box_y + box_height; ++y) {
    uint8_t* row = guest_frame_bgra_.data() + size_t(y) * guest_frame_width_ * 4;
    for (uint32_t x = box_x; x < box_x + box_width; ++x) {
      uint8_t* pixel = row + size_t(x) * 4;
      pixel[0] = uint8_t(uint32_t(pixel[0]) * 3 / 10);
      pixel[1] = uint8_t(uint32_t(pixel[1]) * 3 / 10);
      pixel[2] = uint8_t(uint32_t(pixel[2]) * 3 / 10);
      pixel[3] = 0xFF;
    }
  }

  uint32_t text_x = box_x + padding;
  uint32_t text_y = box_y + padding;
  for (size_t character_index = 0; character_index < character_count; ++character_index) {
    const auto& glyph = GetFpsGlyph(label[character_index]);
    uint32_t glyph_x = text_x + uint32_t(character_index) * character_advance;
    for (uint32_t glyph_y = 0; glyph_y < kGlyphHeight; ++glyph_y) {
      for (uint32_t glyph_x_offset = 0; glyph_x_offset < kGlyphWidth; ++glyph_x_offset) {
        if (!(glyph[glyph_y] & (1u << (kGlyphWidth - 1 - glyph_x_offset)))) {
          continue;
        }
        for (uint32_t scale_y = 0; scale_y < scale; ++scale_y) {
          uint8_t* row = guest_frame_bgra_.data() +
                         size_t(text_y + glyph_y * scale + scale_y) * guest_frame_width_ * 4;
          for (uint32_t scale_x = 0; scale_x < scale; ++scale_x) {
            uint8_t* pixel = row + size_t(glyph_x + glyph_x_offset * scale + scale_x) * 4;
            pixel[0] = 0xFF;
            pixel[1] = 0xFF;
            pixel[2] = 0xFF;
            pixel[3] = 0xFF;
          }
        }
      }
    }
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
  // Match the cross-backend GPU vsync setting. CAMetalLayer defaults to
  // display-synchronized presentation, which silently quantized a 35-55 FPS
  // native Metal workload down to 30 FPS even when GPU vsync was disabled.
  [layer setDisplaySyncEnabled:REXCVAR_GET(vsync) ? YES : NO];

  if (metal_layer_ != layer) {
    if (metal_layer_) {
      [(id)metal_layer_ release];
    }
    metal_layer_ = layer;
    [(id)metal_layer_ retain];
  }

  is_vsync_implicit_out = REXCVAR_GET(vsync);
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
    uint64_t next_drawable_start_ns = profile_enabled_ ? profiling::NowNs() : 0;
    id<CAMetalDrawable> drawable = [(CAMetalLayer*)metal_layer_ nextDrawable];
    if (profile_enabled_) {
      std::lock_guard<std::mutex> lock(profile_mutex_);
      profile_window_.Record(profiling::PresenterEvent::kNextDrawable,
                             profiling::ElapsedNs(next_drawable_start_ns));
    }
    if (!drawable) {
      EndProfiledPresentAttempt(true);
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
          guest_texture_generation_ = 0;
        }
        if (guest_texture_ && guest_texture_generation_ != guest_frame_generation_) {
          MTLRegion region = MTLRegionMake2D(0, 0, guest_frame_width_, guest_frame_height_);
          uint64_t upload_start_ns = profile_enabled_ ? profiling::NowNs() : 0;
          [(id<MTLTexture>)guest_texture_ replaceRegion:region
                                            mipmapLevel:0
                                              withBytes:guest_frame_bgra_.data()
                                            bytesPerRow:size_t(guest_frame_width_) * 4];
          if (profile_enabled_) {
            std::lock_guard<std::mutex> profile_lock(profile_mutex_);
            profile_window_.Record(profiling::PresenterEvent::kUpload,
                                   profiling::ElapsedNs(upload_start_ns));
            profile_window_.RecordUpload(size_t(guest_frame_width_) * guest_frame_height_ * 4);
          }
          guest_texture_generation_ = guest_frame_generation_;
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

    uint64_t present_start_ns = profile_enabled_ ? profiling::NowNs() : 0;
    [command_buffer presentDrawable:drawable];
    [command_buffer commit];
    EndProfiledPresentAttempt(false, profile_enabled_ ? profiling::ElapsedNs(present_start_ns) : 0);
  }

  return PaintResult::kPresented;
}

}  // namespace rex::ui::metal
