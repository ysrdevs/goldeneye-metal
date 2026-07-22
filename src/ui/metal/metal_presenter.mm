#include <rex/ui/metal/presenter.h>

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#import <QuartzCore/CAMetalLayer.h>

#include <rex/cvar.h>
#include <rex/chrono/clock.h>
#include <rex/graphics/flags.h>
#include <rex/logging.h>
#include <rex/perf/metal_performance.h>
#include <rex/ui/surface.h>
#include <rex/ui/surface_macos.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

#include "metal_presenter_shaders.h"

REXCVAR_DEFINE_BOOL(metal_presenter_clear_test, false, "UI/Metal",
                    "Submit a simple Metal clear/present command for presentation smoke testing")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(metal_show_fps, true, "UI/Metal",
                    "Show the guest frame rate in the Metal output");
REXCVAR_DEFINE_BOOL(metal_show_detailed_performance, false, "UI/Metal",
                    "Show frame time, present GPU time, 1% low and compile hitches");
REXCVAR_DEFINE_STRING(metal_output_scaler, "bilinear", "UI/Metal",
                      "Metal output scaling: bilinear, sharp, or MetalFX")
    .allowed({"bilinear", "sharp", "metalfx"});

namespace rex::ui::metal {
namespace {

namespace profiling = rex::graphics::metal::profiling;

constexpr std::array<uint8_t, 7> kFpsGlyphSpace = {};
constexpr std::array<uint8_t, 7> kFpsGlyphDot = {0b00000, 0b00000, 0b00000, 0b00000,
                                                 0b00000, 0b00110, 0b00110};
constexpr std::array<uint8_t, 7> kFpsGlyphF = {0b11111, 0b10000, 0b10000, 0b11110,
                                               0b10000, 0b10000, 0b10000};
constexpr std::array<uint8_t, 7> kFpsGlyphP = {0b11110, 0b10001, 0b10001, 0b11110,
                                               0b10000, 0b10000, 0b10000};
constexpr std::array<uint8_t, 7> kFpsGlyphR = {0b11110, 0b10001, 0b10001, 0b11110,
                                               0b10100, 0b10010, 0b10001};
constexpr std::array<uint8_t, 7> kFpsGlyphS = {0b01111, 0b10000, 0b10000, 0b01110,
                                               0b00001, 0b00001, 0b11110};
constexpr std::array<uint8_t, 7> kFpsGlyphA = {0b01110, 0b10001, 0b10001, 0b11111,
                                               0b10001, 0b10001, 0b10001};
constexpr std::array<uint8_t, 7> kFpsGlyphC = {0b01111, 0b10000, 0b10000, 0b10000,
                                               0b10000, 0b10000, 0b01111};
constexpr std::array<uint8_t, 7> kFpsGlyphE = {0b11111, 0b10000, 0b10000, 0b11110,
                                               0b10000, 0b10000, 0b11111};
constexpr std::array<uint8_t, 7> kFpsGlyphG = {0b01111, 0b10000, 0b10000, 0b10111,
                                               0b10001, 0b10001, 0b01111};
constexpr std::array<uint8_t, 7> kFpsGlyphH = {0b10001, 0b10001, 0b10001, 0b11111,
                                               0b10001, 0b10001, 0b10001};
constexpr std::array<uint8_t, 7> kFpsGlyphI = {0b11111, 0b00100, 0b00100, 0b00100,
                                               0b00100, 0b00100, 0b11111};
constexpr std::array<uint8_t, 7> kFpsGlyphL = {0b10000, 0b10000, 0b10000, 0b10000,
                                               0b10000, 0b10000, 0b11111};
constexpr std::array<uint8_t, 7> kFpsGlyphM = {0b10001, 0b11011, 0b10101, 0b10101,
                                               0b10001, 0b10001, 0b10001};
constexpr std::array<uint8_t, 7> kFpsGlyphO = {0b01110, 0b10001, 0b10001, 0b10001,
                                               0b10001, 0b10001, 0b01110};
constexpr std::array<uint8_t, 7> kFpsGlyphT = {0b11111, 0b00100, 0b00100, 0b00100,
                                               0b00100, 0b00100, 0b00100};
constexpr std::array<uint8_t, 7> kFpsGlyphU = {0b10001, 0b10001, 0b10001, 0b10001,
                                               0b10001, 0b10001, 0b01110};
constexpr std::array<uint8_t, 7> kFpsGlyphW = {0b10001, 0b10001, 0b10001, 0b10101,
                                               0b10101, 0b10101, 0b01010};
constexpr std::array<uint8_t, 7> kFpsGlyphPercent = {0b11001, 0b11010, 0b00100, 0b01000,
                                                     0b10110, 0b10011, 0b00000};
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
    case 'A':
      return kFpsGlyphA;
    case 'C':
      return kFpsGlyphC;
    case 'E':
      return kFpsGlyphE;
    case 'G':
      return kFpsGlyphG;
    case 'H':
      return kFpsGlyphH;
    case 'I':
      return kFpsGlyphI;
    case 'L':
      return kFpsGlyphL;
    case 'M':
      return kFpsGlyphM;
    case 'O':
      return kFpsGlyphO;
    case 'P':
      return kFpsGlyphP;
    case 'R':
      return kFpsGlyphR;
    case 'S':
      return kFpsGlyphS;
    case 'T':
      return kFpsGlyphT;
    case 'U':
      return kFpsGlyphU;
    case 'W':
      return kFpsGlyphW;
    case '%':
      return kFpsGlyphPercent;
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

enum class MetalFxaaQuality : uint32_t {
  kOff = 0,
  kFxaa = 1,
  kFxaaExtreme = 2,
};

enum class MetalOutputScaler : uint32_t {
  kBilinear = 0,
  kSharp = 1,
  kMetalFx = 2,
};

// Must remain scalar-only and match shaders::PresentParameters in
// metal_presenter_shaders.h. setFragmentBytes copies this structure directly
// into the fragment-stage constant address space.
struct MetalPresentParameters {
  uint32_t fxaa_quality = uint32_t(MetalFxaaQuality::kOff);
  uint32_t postfx_enabled = 0;
  uint32_t output_filter = uint32_t(MetalOutputScaler::kBilinear);
  float source_inv_width = 1.0f;
  float source_inv_height = 1.0f;
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
static_assert(sizeof(MetalPresentParameters) == 15 * sizeof(uint32_t));

float GetFiniteCvarFloat(const char* name, float default_value, float minimum,
                         float maximum) {
  std::string text = rex::cvar::GetFlagByName(name);
  if (text.empty()) {
    return default_value;
  }
  char* parsed_end = nullptr;
  float value = std::strtof(text.c_str(), &parsed_end);
  if (parsed_end == text.c_str() || *parsed_end != '\0' || !std::isfinite(value)) {
    return default_value;
  }
  return std::clamp(value, minimum, maximum);
}

bool GetCvarBoolByName(const char* name) {
  std::string value = rex::cvar::GetFlagByName(name);
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return char(std::tolower(c));
  });
  return value == "true" || value == "1" || value == "yes" || value == "on";
}

MetalOutputScaler GetRequestedMetalOutputScaler() {
  std::string scaler = REXCVAR_GET(metal_output_scaler);
  std::transform(scaler.begin(), scaler.end(), scaler.begin(), [](unsigned char c) {
    c = static_cast<unsigned char>(std::tolower(c));
    return c == '-' ? '_' : char(c);
  });
  if (scaler == "sharp") {
    return MetalOutputScaler::kSharp;
  }
  if (scaler == "metalfx" || scaler == "metal_fx") {
    return MetalOutputScaler::kMetalFx;
  }
  return MetalOutputScaler::kBilinear;
}

const char* MetalOutputScalerName(MetalOutputScaler scaler) {
  switch (scaler) {
    case MetalOutputScaler::kSharp:
      return "Sharp";
    case MetalOutputScaler::kMetalFx:
      return "MetalFX";
    default:
      return "Bilinear";
  }
}

MetalFxaaQuality GetMetalFxaaQuality() {
  std::string effect = rex::cvar::GetFlagByName("swap_post_effect");
  std::transform(effect.begin(), effect.end(), effect.begin(), [](unsigned char c) {
    c = static_cast<unsigned char>(std::tolower(c));
    return c == '-' ? '_' : char(c);
  });
  if (effect == "fxaa_extreme" || effect == "extreme") {
    return MetalFxaaQuality::kFxaaExtreme;
  }
  if (effect == "fxaa") {
    return MetalFxaaQuality::kFxaa;
  }
  return MetalFxaaQuality::kOff;
}

const char* MetalFxaaQualityName(MetalFxaaQuality quality) {
  switch (quality) {
    case MetalFxaaQuality::kFxaa:
      return "FXAA";
    case MetalFxaaQuality::kFxaaExtreme:
      return "FXAA Extreme";
    default:
      return "off";
  }
}

MetalPresentParameters BuildMetalPresentParameters(uint32_t source_width,
                                                   uint32_t source_height) {
  MetalPresentParameters parameters;
  parameters.source_inv_width = 1.0f / float(std::max(uint32_t(1), source_width));
  parameters.source_inv_height = 1.0f / float(std::max(uint32_t(1), source_height));
  // Metal has no command-processor swap-effect stage: both the direct mailbox
  // and CPU-upload fallback converge in this presenter. The hot-reload cvar is
  // therefore the authoritative state here, and reading it avoids coupling
  // UI presentation to a command-processor thread that doesn't consume it.
  parameters.fxaa_quality = uint32_t(GetMetalFxaaQuality());
  parameters.postfx_enabled = GetCvarBoolByName("postfx_enabled") ? 1u : 0u;
  if (!parameters.postfx_enabled) {
    return parameters;
  }

  parameters.brightness = GetFiniteCvarFloat("postfx_brightness", 0.0f, -1.0f, 1.0f);
  parameters.contrast = GetFiniteCvarFloat("postfx_contrast", 1.0f, 0.0f, 2.0f);
  parameters.saturation = GetFiniteCvarFloat("postfx_saturation", 1.0f, 0.0f, 2.0f);
  parameters.vibrance = GetFiniteCvarFloat("postfx_vibrance", 0.0f, -1.0f, 1.0f);
  parameters.temperature = GetFiniteCvarFloat("postfx_temperature", 0.0f, -1.0f, 1.0f);
  parameters.gamma = GetFiniteCvarFloat("postfx_gamma", 1.0f, 0.3f, 3.0f);
  parameters.tint_r = GetFiniteCvarFloat("postfx_tint_r", 1.0f, 0.0f, 1.0f);
  parameters.tint_g = GetFiniteCvarFloat("postfx_tint_g", 1.0f, 0.0f, 1.0f);
  parameters.tint_b = GetFiniteCvarFloat("postfx_tint_b", 1.0f, 0.0f, 1.0f);
  parameters.tint_strength = GetFiniteCvarFloat("postfx_tint", 0.0f, 0.0f, 1.0f);
  return parameters;
}

void DrawFpsOverlay(id<MTLRenderCommandEncoder> encoder,
                    id<MTLRenderPipelineState> pipeline_state, id<MTLTexture> texture,
                    uint32_t texture_width, uint32_t texture_height,
                    const MTLViewport& guest_viewport, double guest_scale, double guest_width,
                    double guest_height, uint32_t drawable_width, uint32_t drawable_height) {
  double overlay_margin_guest =
      guest_width >= 960.0 && guest_height >= 540.0
          ? 6.0
          : (guest_width >= 480.0 && guest_height >= 270.0 ? 4.0 : 2.0);
  MTLViewport fps_viewport = {
      guest_viewport.originX + guest_viewport.width -
          (double(texture_width) + overlay_margin_guest) * guest_scale,
      guest_viewport.originY + overlay_margin_guest * guest_scale,
      double(texture_width) * guest_scale,
      double(texture_height) * guest_scale,
      0.0,
      1.0,
  };
  uint32_t identity_swizzle = 0x688;
  // The FPS image is a host diagnostic, not guest content. Keep it above
  // passive spatial effects as well as outside FXAA and colour grading.
  MetalPresentParameters overlay_parameters;
  overlay_parameters.source_inv_width =
      1.0f / float(std::max(uint32_t(1), texture_width));
  overlay_parameters.source_inv_height =
      1.0f / float(std::max(uint32_t(1), texture_height));
  [encoder setScissorRect:MTLScissorRect{0, 0, drawable_width, drawable_height}];
  [encoder setViewport:fps_viewport];
  [encoder setRenderPipelineState:pipeline_state];
  [encoder setFragmentTexture:texture atIndex:0];
  [encoder setFragmentBytes:&identity_swizzle length:sizeof(identity_swizzle) atIndex:0];
  [encoder setFragmentBytes:&overlay_parameters length:sizeof(overlay_parameters) atIndex:1];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
}

}  // namespace

std::unique_ptr<MetalPresenter> MetalPresenter::Create(void* metal_device,
                                                       HostGpuLossCallback host_gpu_loss_callback) {
  if (!metal_device) {
    return nullptr;
  }
  std::unique_ptr<MetalPresenter> presenter(
      new MetalPresenter(metal_device, std::move(host_gpu_loss_callback)));
  presenter->command_queue_ = [(id<MTLDevice>)metal_device newCommandQueue];
  if (!presenter->command_queue_) {
    REXLOG_ERROR("MetalPresenter: failed to create the presentation command queue");
    return nullptr;
  }
  [(id<MTLCommandQueue>)presenter->command_queue_ setLabel:@"ReXGlue guest output"];
  if (!presenter->InitializeCommonSurfaceIndependent()) {
    return nullptr;
  }
  return presenter;
}

MetalPresenter::MetalPresenter(void* metal_device, HostGpuLossCallback host_gpu_loss_callback)
    : Presenter(std::move(host_gpu_loss_callback)), metal_device_(metal_device) {
  [(id)metal_device_ retain];
  if (@available(macOS 13.0, *)) {
    metalfx_supported_ =
        [MTLFXSpatialScalerDescriptor supportsDevice:(id<MTLDevice>)metal_device_] == YES;
  }
  REXLOG_INFO("MetalPresenter: MetalFX spatial scaling {} on {}",
              metalfx_supported_ ? "is supported" : "is unavailable",
              [[(id<MTLDevice>)metal_device_ name] UTF8String]);
}

MetalPresenter::~MetalPresenter() {
  DisconnectPaintingFromSurfaceFromUIThreadImpl();
  ReleaseMetalFxResources();
  for (GuestOutputMailboxTexture& mailbox_texture : guest_output_mailbox_textures_) {
    if (mailbox_texture.texture) {
      [(id)mailbox_texture.texture release];
      mailbox_texture.texture = nullptr;
    }
  }
  if (command_queue_) {
    [(id)command_queue_ release];
    command_queue_ = nullptr;
  }
  if (guest_texture_) {
    [(id)guest_texture_ release];
    guest_texture_ = nullptr;
  }
  for (FpsOverlayTexture& overlay_texture : fps_overlay_textures_) {
    if (overlay_texture.texture) {
      [(id)overlay_texture.texture release];
      overlay_texture.texture = nullptr;
    }
  }
  if (guest_pipeline_state_) {
    [(id)guest_pipeline_state_ release];
    guest_pipeline_state_ = nullptr;
  }
  if (fps_pipeline_state_) {
    [(id)fps_pipeline_state_ release];
    fps_pipeline_state_ = nullptr;
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
  guest_frame_id_ = RecordGuestFrameArrivalLocked();
  BuildGuestFpsOverlayLocked(guest_frame_width_, guest_frame_height_);
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
  guest_frame_id_ = RecordGuestFrameArrivalLocked();
  BuildGuestFpsOverlayLocked(guest_frame_width_, guest_frame_height_);
}

void MetalPresenter::FinalizeGuestFrameLocked() {
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

  ++guest_frame_generation_;
}

uint64_t MetalPresenter::RecordGuestFrameArrivalLocked() {
  const uint64_t guest_frame_id = rex::perf::RecordMetalGuestFrame();
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
  return guest_frame_id;
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

void MetalPresenter::BuildGuestFpsOverlayLocked(uint32_t width, uint32_t height) {
  auto clear_overlay = [&]() {
    if (fps_overlay_bgra_.empty() && !fps_overlay_width_ && !fps_overlay_height_ &&
        !fps_overlay_scale_ && !fps_overlay_label_[0]) {
      return;
    }
    fps_overlay_bgra_.clear();
    fps_overlay_width_ = 0;
    fps_overlay_height_ = 0;
    fps_overlay_scale_ = 0;
    fps_overlay_detailed_ = false;
    fps_overlay_label_.fill(0);
    ++fps_overlay_generation_;
  };
  if (!REXCVAR_GET(metal_show_fps) || !width || !height) {
    clear_overlay();
    return;
  }

  const bool detailed = REXCVAR_GET(metal_show_detailed_performance);
  const uint32_t scale =
      width >= 960 && height >= 540 ? 3 : (width >= 480 && height >= 270 ? 2 : 1);
  const bool mode_changed = fps_overlay_detailed_ != detailed;
  const bool shape_stale = fps_overlay_bgra_.empty() || fps_overlay_scale_ != scale ||
                           fps_overlay_width_ > width || fps_overlay_height_ > height;
  const uint64_t now = rex::chrono::Clock::QueryHostTickCount();
  const uint64_t tick_frequency = rex::chrono::Clock::QueryHostTickFrequency();
  // Snapshot construction calculates percentiles over bounded sample windows.
  // Five updates per second keeps the detailed overlay responsive without
  // copying and sorting those windows on every guest frame.
  const uint64_t snapshot_interval = tick_frequency ? std::max<uint64_t>(1, tick_frequency / 5) : 0;
  const bool snapshot_due = !fps_performance_snapshot_valid_ || mode_changed ||
                            !fps_performance_snapshot_tick_ || now < fps_performance_snapshot_tick_ ||
                            !snapshot_interval ||
                            now - fps_performance_snapshot_tick_ >= snapshot_interval;
  if (detailed && !snapshot_due && !shape_stale) {
    return;
  }
  if (snapshot_due) {
    const rex::perf::MetalPerformanceSnapshot performance =
        rex::perf::GetMetalPerformanceSnapshot();
    fps_cached_performance_fps_ = performance.fps;
    fps_cached_frame_time_ms_ = performance.frame_time_ms;
    fps_cached_gpu_time_ms_ = performance.gpu_time_ms;
    fps_cached_one_percent_low_fps_ = performance.one_percent_low_fps;
    fps_cached_compile_hitches_ =
        performance.shader_compile_hitches + performance.pipeline_compile_hitches;
    fps_performance_snapshot_tick_ = now;
    fps_performance_snapshot_valid_ = true;
  }
  fps_overlay_detailed_ = detailed;

  char label[128];
  const double displayed_fps =
      std::min(fps_cached_performance_fps_ > 0.0 ? fps_cached_performance_fps_ : guest_fps_,
               999.9);
  int label_length = 0;
  if (detailed) {
    label_length = std::snprintf(
        label, sizeof(label),
        "FPS %.1f\nFRAME %.1f MS\nGPU %.1f MS\n1%% LOW %.1f\nHITCH %llu",
        displayed_fps, std::min(fps_cached_frame_time_ms_, 999.9),
        std::min(fps_cached_gpu_time_ms_, 999.9),
        std::min(fps_cached_one_percent_low_fps_, 999.9),
        static_cast<unsigned long long>(fps_cached_compile_hitches_));
  } else {
    label_length = std::snprintf(label, sizeof(label), "FPS %.1f", displayed_fps);
  }
  if (label_length <= 0) {
    clear_overlay();
    return;
  }
  const size_t character_count =
      std::min<size_t>(size_t(label_length), sizeof(label) - 1);
  constexpr uint32_t kGlyphWidth = 5;
  constexpr uint32_t kGlyphHeight = 7;
  uint32_t padding = scale * 2;
  uint32_t character_advance = (kGlyphWidth + 1) * scale;
  size_t line_character_count = 0;
  size_t maximum_line_character_count = 0;
  uint32_t line_count = 1;
  for (size_t character_index = 0; character_index < character_count; ++character_index) {
    if (label[character_index] == '\n') {
      maximum_line_character_count =
          std::max(maximum_line_character_count, line_character_count);
      line_character_count = 0;
      ++line_count;
    } else {
      ++line_character_count;
    }
  }
  maximum_line_character_count =
      std::max(maximum_line_character_count, line_character_count);
  uint32_t label_width = maximum_line_character_count
                             ? uint32_t(maximum_line_character_count) * character_advance - scale
                             : 0;
  const uint32_t line_advance = (kGlyphHeight + 2) * scale;
  uint32_t box_width = label_width + padding * 2;
  uint32_t box_height = kGlyphHeight * scale + (line_count - 1) * line_advance + padding * 2;
  if (!box_width || !box_height || box_width > width || box_height > height) {
    clear_overlay();
    return;
  }
  if (!fps_overlay_bgra_.empty() && fps_overlay_width_ == box_width &&
      fps_overlay_height_ == box_height && fps_overlay_scale_ == scale &&
      std::strncmp(fps_overlay_label_.data(), label, fps_overlay_label_.size()) == 0) {
    return;
  }

  fps_overlay_width_ = box_width;
  fps_overlay_height_ = box_height;
  fps_overlay_scale_ = scale;
  fps_overlay_label_.fill(0);
  std::memcpy(fps_overlay_label_.data(), label,
              std::min(fps_overlay_label_.size() - 1, size_t(label_length)));
  fps_overlay_bgra_.assign(size_t(box_width) * box_height * 4, 0);
  for (size_t pixel = 0; pixel < size_t(box_width) * box_height; ++pixel) {
    fps_overlay_bgra_[pixel * 4 + 3] = 178;
  }
  uint32_t text_x = 0;
  uint32_t text_y = 0;
  for (size_t character_index = 0; character_index < character_count; ++character_index) {
    if (label[character_index] == '\n') {
      text_x = 0;
      text_y += line_advance;
      continue;
    }
    const auto& glyph = GetFpsGlyph(label[character_index]);
    uint32_t glyph_x = padding + text_x;
    for (uint32_t glyph_y = 0; glyph_y < kGlyphHeight; ++glyph_y) {
      for (uint32_t glyph_x_offset = 0; glyph_x_offset < kGlyphWidth; ++glyph_x_offset) {
        if (!(glyph[glyph_y] & (1u << (kGlyphWidth - 1 - glyph_x_offset)))) {
          continue;
        }
        for (uint32_t scale_y = 0; scale_y < scale; ++scale_y) {
          for (uint32_t scale_x = 0; scale_x < scale; ++scale_x) {
            size_t offset = (size_t(padding + text_y + glyph_y * scale + scale_y) * box_width + glyph_x +
                             glyph_x_offset * scale + scale_x) *
                            4;
            fps_overlay_bgra_[offset + 0] = 0xFF;
            fps_overlay_bgra_[offset + 1] = 0xFF;
            fps_overlay_bgra_[offset + 2] = 0xFF;
            fps_overlay_bgra_[offset + 3] = 0xFF;
          }
        }
      }
    }
    text_x += character_advance;
  }
  ++fps_overlay_generation_;
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
  const bool vsync_enabled = REXCVAR_GET(vsync);
  [layer setDisplaySyncEnabled:vsync_enabled ? YES : NO];
  REXLOG_INFO("MetalPresenter: display synchronization {}",
              vsync_enabled ? "enabled" : "disabled");

  if (metal_layer_ != layer) {
    if (metal_layer_) {
      [(id)metal_layer_ release];
    }
    metal_layer_ = layer;
    [(id)metal_layer_ retain];
  }

  is_vsync_implicit_out = vsync_enabled;
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
  assert_true(mailbox_index < guest_output_mailbox_textures_.size());
  assert_not_zero(frontbuffer_width);
  assert_not_zero(frontbuffer_height);

  GuestOutputMailboxTexture& mailbox_texture = guest_output_mailbox_textures_[mailbox_index];
  if (mailbox_texture.texture && (mailbox_texture.width != frontbuffer_width ||
                                  mailbox_texture.height != frontbuffer_height)) {
    // Metal command buffers retain their referenced resources. Also, direct
    // producers are required to use command_queue_, so replacing a free
    // mailbox texture can't race with queued work using its previous version.
    [(id)mailbox_texture.texture release];
    mailbox_texture.texture = nullptr;
    mailbox_texture.width = 0;
    mailbox_texture.height = 0;
  }
  if (!mailbox_texture.texture) {
    MTLTextureDescriptor* texture_desc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:frontbuffer_width
                                                          height:frontbuffer_height
                                                       mipmapped:NO];
    texture_desc.usage =
        MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget;
    texture_desc.storageMode = MTLStorageModePrivate;
    mailbox_texture.texture = [(id<MTLDevice>)metal_device_ newTextureWithDescriptor:texture_desc];
    if (mailbox_texture.texture) {
      mailbox_texture.width = frontbuffer_width;
      mailbox_texture.height = frontbuffer_height;
      [(id<MTLTexture>)mailbox_texture.texture
          setLabel:[NSString stringWithFormat:@"ReXGlue guest output mailbox %u", mailbox_index]];
    } else {
      // A missing direct texture isn't fatal to presentation. A refresher that
      // doesn't require it may still succeed and use the CPU-upload fallback.
      REXLOG_WARN("MetalPresenter: failed to create direct guest output texture {}x{}",
                  frontbuffer_width, frontbuffer_height);
    }
  }

  MetalGuestOutputRefreshContext context(is_8bpc_out_ref, mailbox_texture.texture, command_queue_);
  bool refresher_succeeded = refresher(context);
  if (refresher_succeeded && context.direct_valid()) {
    // The presentation snapshot is produced on the renderer queue, then read
    // by a copy submitted to command_queue_. Metal's automatic hazard tracking
    // doesn't order conflicting accesses across queues. Complete that copy
    // before returning to the renderer so it may safely reuse its snapshot
    // texture for the next frame.
    id<MTLCommandBuffer> direct_refresh_completion =
        [(id<MTLCommandQueue>)command_queue_ commandBuffer];
    if (direct_refresh_completion) {
      direct_refresh_completion.label = @"ReXGlue direct guest output refresh completion";
      [direct_refresh_completion commit];
      [direct_refresh_completion waitUntilCompleted];
    }
    if (!direct_refresh_completion ||
        [direct_refresh_completion status] != MTLCommandBufferStatusCompleted) {
      NSError* error = direct_refresh_completion ? [direct_refresh_completion error] : nil;
      const char* error_text = error ? [[error localizedDescription] UTF8String] : "unknown error";
      REXLOG_WARN("MetalPresenter: direct guest output copy did not complete: {}", error_text);
      refresher_succeeded = false;
    }
  }
  mailbox_texture.direct_valid = refresher_succeeded && context.direct_valid();
  mailbox_texture.guest_swizzle = mailbox_texture.direct_valid ? context.guest_swizzle() : 0x688;
  if (refresher_succeeded) {
    {
      std::lock_guard<std::mutex> lock(guest_frame_mutex_);
      uint64_t guest_frame_id = 0;
      if (!mailbox_texture.direct_valid && guest_frame_id_ &&
          guest_frame_refresh_generation_ != guest_frame_generation_) {
        // UpdateGuestFrontbuffer already assigned the ID while publishing the
        // exact CPU pixels. Consume it here rather than counting the fallback
        // refresh a second time.
        guest_frame_id = guest_frame_id_;
        guest_frame_refresh_generation_ = guest_frame_generation_;
      } else {
        guest_frame_id = RecordGuestFrameArrivalLocked();
        if (!mailbox_texture.direct_valid && !guest_frame_bgra_.empty()) {
          // A repeated CPU image is still a new guest refresh. Keep its pixels
          // and ID paired under the same ownership lock.
          guest_frame_id_ = guest_frame_id;
          guest_frame_refresh_generation_ = guest_frame_generation_;
        }
      }
      mailbox_texture.guest_frame_id = guest_frame_id;
      BuildGuestFpsOverlayLocked(frontbuffer_width, frontbuffer_height);
    }
    if (profile_enabled_ && mailbox_texture.direct_valid) {
      std::lock_guard<std::mutex> profile_lock(profile_mutex_);
      profile_window_.RecordSource(false);
    }
  }
  return refresher_succeeded;
}

bool MetalPresenter::EnsureGuestPipeline() {
  if (guest_pipeline_state_) {
    return true;
  }
  if (fps_pipeline_state_) {
    [(id)fps_pipeline_state_ release];
    fps_pipeline_state_ = nullptr;
  }

  id<MTLDevice> device = (id<MTLDevice>)metal_device_;
  NSError* error = nil;
  NSString* source = [NSString stringWithUTF8String:shaders::kGuestPresentMetalSource];
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
  NSError* guest_pipeline_error = nil;
  guest_pipeline_state_ = [device newRenderPipelineStateWithDescriptor:descriptor
                                                                 error:&guest_pipeline_error];

  descriptor.colorAttachments[0].blendingEnabled = YES;
  descriptor.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
  descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
  descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  descriptor.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
  descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
  descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  NSError* fps_pipeline_error = nil;
  fps_pipeline_state_ = [device newRenderPipelineStateWithDescriptor:descriptor
                                                               error:&fps_pipeline_error];
  [descriptor release];
  [vertex_function release];
  [fragment_function release];
  [library release];

  if (!guest_pipeline_state_) {
    const char* error_text = guest_pipeline_error
                                 ? [[guest_pipeline_error localizedDescription] UTF8String]
                                 : "unknown error";
    REXLOG_ERROR("MetalPresenter: failed to create guest present pipeline: {}", error_text);
    if (fps_pipeline_state_) {
      [(id)fps_pipeline_state_ release];
      fps_pipeline_state_ = nullptr;
    }
    return false;
  }
  if (!fps_pipeline_state_) {
    const char* error_text = fps_pipeline_error
                                 ? [[fps_pipeline_error localizedDescription] UTF8String]
                                 : "unknown error";
    REXLOG_WARN("MetalPresenter: failed to create FPS overlay pipeline; continuing "
                "without the overlay: {}",
                error_text);
  }
  return true;
}

void MetalPresenter::ReleaseMetalFxResources() {
  if (metalfx_spatial_scaler_) {
    [(id)metalfx_spatial_scaler_ release];
    metalfx_spatial_scaler_ = nullptr;
  }
  if (metalfx_input_texture_) {
    [(id)metalfx_input_texture_ release];
    metalfx_input_texture_ = nullptr;
  }
  if (metalfx_output_texture_) {
    [(id)metalfx_output_texture_ release];
    metalfx_output_texture_ = nullptr;
  }
  metalfx_input_width_ = 0;
  metalfx_input_height_ = 0;
  metalfx_output_width_ = 0;
  metalfx_output_height_ = 0;
}

bool MetalPresenter::EnsureMetalFxResources(uint32_t input_width, uint32_t input_height,
                                            uint32_t output_width, uint32_t output_height) {
  if (!metalfx_supported_ || !input_width || !input_height || !output_width ||
      !output_height) {
    return false;
  }
  if (metalfx_spatial_scaler_ && metalfx_input_texture_ && metalfx_output_texture_ &&
      metalfx_input_width_ == input_width && metalfx_input_height_ == input_height &&
      metalfx_output_width_ == output_width && metalfx_output_height_ == output_height) {
    return true;
  }
  if (metalfx_failed_input_width_ == input_width &&
      metalfx_failed_input_height_ == input_height &&
      metalfx_failed_output_width_ == output_width &&
      metalfx_failed_output_height_ == output_height) {
    return false;
  }

  ReleaseMetalFxResources();
  if (@available(macOS 13.0, *)) {
    MTLFXSpatialScalerDescriptor* scaler_descriptor =
        [[MTLFXSpatialScalerDescriptor alloc] init];
    scaler_descriptor.colorTextureFormat = MTLPixelFormatBGRA8Unorm;
    scaler_descriptor.outputTextureFormat = MTLPixelFormatBGRA8Unorm;
    scaler_descriptor.inputWidth = input_width;
    scaler_descriptor.inputHeight = input_height;
    scaler_descriptor.outputWidth = output_width;
    scaler_descriptor.outputHeight = output_height;
    scaler_descriptor.colorProcessingMode =
        MTLFXSpatialScalerColorProcessingModePerceptual;
    id<MTLFXSpatialScaler> scaler =
        [scaler_descriptor newSpatialScalerWithDevice:(id<MTLDevice>)metal_device_];
    [scaler_descriptor release];
    if (scaler) {
      MTLTextureDescriptor* input_descriptor =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                             width:input_width
                                                            height:input_height
                                                         mipmapped:NO];
      input_descriptor.storageMode = MTLStorageModePrivate;
      input_descriptor.usage =
          MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | scaler.colorTextureUsage;
      id<MTLTexture> input_texture =
          [(id<MTLDevice>)metal_device_ newTextureWithDescriptor:input_descriptor];

      MTLTextureDescriptor* output_descriptor =
          [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                             width:output_width
                                                            height:output_height
                                                         mipmapped:NO];
      output_descriptor.storageMode = MTLStorageModePrivate;
      output_descriptor.usage = MTLTextureUsageShaderRead | scaler.outputTextureUsage;
      id<MTLTexture> output_texture =
          [(id<MTLDevice>)metal_device_ newTextureWithDescriptor:output_descriptor];
      if (input_texture && output_texture) {
        input_texture.label = @"ReXGlue MetalFX processed input";
        output_texture.label = @"ReXGlue MetalFX upscaled output";
        metalfx_spatial_scaler_ = scaler;
        metalfx_input_texture_ = input_texture;
        metalfx_output_texture_ = output_texture;
        metalfx_input_width_ = input_width;
        metalfx_input_height_ = input_height;
        metalfx_output_width_ = output_width;
        metalfx_output_height_ = output_height;
        metalfx_failed_input_width_ = 0;
        metalfx_failed_input_height_ = 0;
        metalfx_failed_output_width_ = 0;
        metalfx_failed_output_height_ = 0;
        REXLOG_INFO("MetalPresenter: created MetalFX spatial scaler {}x{} -> {}x{}",
                    input_width, input_height, output_width, output_height);
        return true;
      }
      if (input_texture) {
        [input_texture release];
      }
      if (output_texture) {
        [output_texture release];
      }
      [scaler release];
    }
  }

  metalfx_failed_input_width_ = input_width;
  metalfx_failed_input_height_ = input_height;
  metalfx_failed_output_width_ = output_width;
  metalfx_failed_output_height_ = output_height;
  REXLOG_WARN("MetalPresenter: failed to initialize MetalFX {}x{} -> {}x{}; using Sharp",
              input_width, input_height, output_width, output_height);
  return false;
}

Presenter::PaintResult MetalPresenter::PaintAndPresentImpl(bool execute_ui_drawers) {
  if (!metal_layer_) {
    return PaintResult::kPresented;
  }
  if (!command_queue_) {
    return PaintResult::kNotPresented;
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

    id<MTLTexture> source_texture = nil;
    uint32_t source_guest_swizzle = 0x688;
    uint64_t source_guest_frame_id = 0;
    // Preserve UpdateGuestFrontbuffer as a self-contained fallback, including
    // before the first successful mailbox publication.
    bool use_cpu_fallback = true;
    uint32_t guest_output_mailbox_index;
    std::unique_lock<std::mutex> guest_output_consumer_lock(
        ConsumeGuestOutput(guest_output_mailbox_index, nullptr, nullptr));
    if (guest_output_mailbox_index != UINT32_MAX) {
      const GuestOutputMailboxTexture& mailbox_texture =
          guest_output_mailbox_textures_[guest_output_mailbox_index];
      if (mailbox_texture.texture && mailbox_texture.direct_valid) {
        source_texture = [(id<MTLTexture>)mailbox_texture.texture retain];
        source_guest_swizzle = mailbox_texture.guest_swizzle;
        source_guest_frame_id = mailbox_texture.guest_frame_id;
        use_cpu_fallback = false;
      }
    }

    if (use_cpu_fallback) {
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
        if (guest_texture_) {
          source_texture = [(id<MTLTexture>)guest_texture_ retain];
          source_guest_frame_id = guest_frame_id_;
        }
      }
    }

    id<MTLTexture> fps_texture = nil;
    uint32_t fps_width = 0;
    uint32_t fps_height = 0;
    std::shared_ptr<std::atomic<uint32_t>> fps_texture_in_flight;
    if (source_texture && REXCVAR_GET(metal_show_fps)) {
      std::lock_guard<std::mutex> lock(guest_frame_mutex_);
      if (!fps_overlay_bgra_.empty() && fps_overlay_width_ && fps_overlay_height_) {
        bool current_is_up_to_date = false;
        if (fps_overlay_current_texture_ < fps_overlay_textures_.size()) {
          current_is_up_to_date =
              fps_overlay_textures_[fps_overlay_current_texture_].generation ==
              fps_overlay_generation_;
        }
        if (!current_is_up_to_date) {
          size_t upload_index = fps_overlay_textures_.size();
          auto idle = [&](size_t index) {
            return fps_overlay_textures_[index].in_flight->load(std::memory_order_acquire) == 0;
          };
          if (fps_overlay_current_texture_ < fps_overlay_textures_.size() &&
              idle(fps_overlay_current_texture_)) {
            upload_index = fps_overlay_current_texture_;
          }
          if (upload_index == fps_overlay_textures_.size()) {
            for (size_t index = 0; index < fps_overlay_textures_.size(); ++index) {
              const FpsOverlayTexture& candidate = fps_overlay_textures_[index];
              if (idle(index) && candidate.texture && candidate.width == fps_overlay_width_ &&
                  candidate.height == fps_overlay_height_) {
                upload_index = index;
                break;
              }
            }
          }
          if (upload_index == fps_overlay_textures_.size()) {
            for (size_t index = 0; index < fps_overlay_textures_.size(); ++index) {
              if (idle(index)) {
                upload_index = index;
                break;
              }
            }
          }
          if (upload_index < fps_overlay_textures_.size()) {
            FpsOverlayTexture& upload = fps_overlay_textures_[upload_index];
            if (!upload.texture || upload.width != fps_overlay_width_ ||
                upload.height != fps_overlay_height_) {
              MTLTextureDescriptor* texture_desc =
                  [MTLTextureDescriptor
                      texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                   width:fps_overlay_width_
                                                  height:fps_overlay_height_
                                               mipmapped:NO];
              texture_desc.usage = MTLTextureUsageShaderRead;
              texture_desc.storageMode = MTLStorageModeShared;
              id<MTLTexture> replacement =
                  [(id<MTLDevice>)metal_device_ newTextureWithDescriptor:texture_desc];
              if (replacement) {
                [replacement setLabel:[NSString
                                          stringWithFormat:@"ReXGlue guest FPS overlay %zu",
                                                           upload_index]];
                if (upload.texture) {
                  [(id)upload.texture release];
                }
                upload.texture = replacement;
                upload.width = fps_overlay_width_;
                upload.height = fps_overlay_height_;
                upload.generation = 0;
              }
            }
            if (upload.texture && upload.width == fps_overlay_width_ &&
                upload.height == fps_overlay_height_) {
              [(id<MTLTexture>)upload.texture
                  replaceRegion:MTLRegionMake2D(0, 0, fps_overlay_width_, fps_overlay_height_)
                    mipmapLevel:0
                      withBytes:fps_overlay_bgra_.data()
                    bytesPerRow:size_t(fps_overlay_width_) * 4];
              upload.generation = fps_overlay_generation_;
              fps_overlay_current_texture_ = upload_index;
            }
          }
        }
        // If all three textures are still being read, retain the previous
        // generation for this paint and retry the upload next time. Rendering
        // a slightly older label is preferable to allocating or racing a GPU
        // read for a diagnostic overlay.
        if (fps_overlay_current_texture_ < fps_overlay_textures_.size()) {
          FpsOverlayTexture& current =
              fps_overlay_textures_[fps_overlay_current_texture_];
          if (current.texture) {
            fps_texture = [(id<MTLTexture>)current.texture retain];
            fps_width = current.width;
            fps_height = current.height;
            fps_texture_in_flight = current.in_flight;
            fps_texture_in_flight->fetch_add(1, std::memory_order_acq_rel);
          }
        }
      }
    }

    id<MTLTexture> drawable_texture = [drawable texture];
    bool guest_presented = false;
    MTLViewport guest_viewport = {};
    double guest_scale = 0.0;
    double guest_width = 0.0;
    double guest_height = 0.0;
    id<MTLTexture> presentation_texture = source_texture;
    uint32_t presentation_swizzle = source_guest_swizzle;
    MetalPresentParameters presentation_parameters;
    MetalOutputScaler requested_scaler = GetRequestedMetalOutputScaler();
    MetalOutputScaler active_scaler = requested_scaler;
    const char* scaler_fallback_reason = nullptr;
    if (source_texture) {
      if (EnsureGuestPipeline()) {
        MetalPresentParameters guest_parameters = BuildMetalPresentParameters(
            uint32_t([source_texture width]), uint32_t([source_texture height]));
        int32_t fxaa_quality = int32_t(guest_parameters.fxaa_quality);
        int32_t previous_fxaa_quality = last_reported_fxaa_quality_.exchange(fxaa_quality);
        if (previous_fxaa_quality != fxaa_quality) {
          REXLOG_INFO("MetalPresenter: anti-aliasing {}",
                      MetalFxaaQualityName(MetalFxaaQuality(guest_parameters.fxaa_quality)));
        }
        int32_t postfx_enabled = guest_parameters.postfx_enabled ? 1 : 0;
        int32_t previous_postfx_enabled =
            last_reported_postfx_enabled_.exchange(postfx_enabled);
        if (previous_postfx_enabled != postfx_enabled) {
          REXLOG_INFO("MetalPresenter: color grading {}",
                      postfx_enabled ? "enabled" : "disabled");
        }

        double drawable_width = double([drawable_texture width]);
        double drawable_height = double([drawable_texture height]);
        guest_width = double([source_texture width]);
        guest_height = double([source_texture height]);
        guest_scale = std::min(drawable_width / guest_width, drawable_height / guest_height);
        double viewport_width = guest_width * guest_scale;
        double viewport_height = guest_height * guest_scale;
        guest_viewport = {(drawable_width - viewport_width) * 0.5,
                          (drawable_height - viewport_height) * 0.5,
                          viewport_width,
                          viewport_height,
                          0.0,
                          1.0};

        uint32_t source_width = uint32_t([source_texture width]);
        uint32_t source_height = uint32_t([source_texture height]);
        uint32_t metalfx_width = std::max(
            uint32_t(1), uint32_t(std::min(drawable_width, std::round(viewport_width))));
        uint32_t metalfx_height = std::max(
            uint32_t(1), uint32_t(std::min(drawable_height, std::round(viewport_height))));
        if (requested_scaler != MetalOutputScaler::kMetalFx) {
          if (metalfx_spatial_scaler_ || metalfx_failed_input_width_ ||
              metalfx_failed_output_width_) {
            ReleaseMetalFxResources();
            metalfx_failed_input_width_ = 0;
            metalfx_failed_input_height_ = 0;
            metalfx_failed_output_width_ = 0;
            metalfx_failed_output_height_ = 0;
          }
        } else if (!metalfx_supported_) {
          active_scaler = MetalOutputScaler::kSharp;
          scaler_fallback_reason = "unsupported by this GPU";
        } else if (metalfx_width < source_width || metalfx_height < source_height ||
                   (metalfx_width == source_width && metalfx_height == source_height)) {
          active_scaler = MetalOutputScaler::kSharp;
          scaler_fallback_reason = "the drawable is not larger than the game image";
        } else if (!EnsureMetalFxResources(source_width, source_height, metalfx_width,
                                           metalfx_height)) {
          active_scaler = MetalOutputScaler::kSharp;
          scaler_fallback_reason = "scaler initialization failed";
        } else {
          MTLRenderPassDescriptor* metalfx_input_pass =
              [MTLRenderPassDescriptor renderPassDescriptor];
          metalfx_input_pass.colorAttachments[0].texture =
              (id<MTLTexture>)metalfx_input_texture_;
          metalfx_input_pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
          metalfx_input_pass.colorAttachments[0].storeAction = MTLStoreActionStore;
          id<MTLRenderCommandEncoder> metalfx_input_encoder =
              [command_buffer renderCommandEncoderWithDescriptor:metalfx_input_pass];
          if (metalfx_input_encoder) {
            // Correct the guest swizzle and apply FXAA / colour grading at the
            // native game resolution before MetalFX reconstructs Retina pixels.
            guest_parameters.output_filter = uint32_t(MetalOutputScaler::kBilinear);
            [metalfx_input_encoder
                setViewport:MTLViewport{0.0, 0.0, double(source_width),
                                        double(source_height), 0.0, 1.0}];
            [metalfx_input_encoder
                setScissorRect:MTLScissorRect{0, 0, source_width, source_height}];
            [metalfx_input_encoder
                setRenderPipelineState:(id<MTLRenderPipelineState>)guest_pipeline_state_];
            // The source, not the render target, is sampled by the pre-pass.
            [metalfx_input_encoder setFragmentTexture:source_texture atIndex:0];
            [metalfx_input_encoder setFragmentBytes:&source_guest_swizzle
                                              length:sizeof(source_guest_swizzle)
                                             atIndex:0];
            [metalfx_input_encoder setFragmentBytes:&guest_parameters
                                              length:sizeof(guest_parameters)
                                             atIndex:1];
            [metalfx_input_encoder drawPrimitives:MTLPrimitiveTypeTriangle
                                      vertexStart:0
                                      vertexCount:3];
            [metalfx_input_encoder endEncoding];

            id<MTLFXSpatialScaler> scaler =
                (id<MTLFXSpatialScaler>)metalfx_spatial_scaler_;
            scaler.inputContentWidth = source_width;
            scaler.inputContentHeight = source_height;
            scaler.colorTexture = (id<MTLTexture>)metalfx_input_texture_;
            scaler.outputTexture = (id<MTLTexture>)metalfx_output_texture_;
            [scaler encodeToCommandBuffer:command_buffer];

            presentation_texture = (id<MTLTexture>)metalfx_output_texture_;
            presentation_swizzle = 0x688;
            presentation_parameters = MetalPresentParameters{};
            presentation_parameters.source_inv_width = 1.0f / float(metalfx_width);
            presentation_parameters.source_inv_height = 1.0f / float(metalfx_height);
            guest_viewport.width = metalfx_width;
            guest_viewport.height = metalfx_height;
            guest_viewport.originX = (drawable_width - double(metalfx_width)) * 0.5;
            guest_viewport.originY = (drawable_height - double(metalfx_height)) * 0.5;
            guest_scale = std::min(double(metalfx_width) / guest_width,
                                   double(metalfx_height) / guest_height);
          } else {
            active_scaler = MetalOutputScaler::kSharp;
            scaler_fallback_reason = "the MetalFX input pass could not be encoded";
          }
        }

        if (active_scaler != MetalOutputScaler::kMetalFx) {
          presentation_texture = source_texture;
          presentation_swizzle = source_guest_swizzle;
          presentation_parameters = guest_parameters;
          presentation_parameters.output_filter =
              active_scaler == MetalOutputScaler::kSharp
                  ? uint32_t(MetalOutputScaler::kSharp)
                  : uint32_t(MetalOutputScaler::kBilinear);
        }

        int32_t previous_requested_scaler =
            last_reported_requested_scaler_.exchange(int32_t(requested_scaler));
        int32_t previous_active_scaler =
            last_reported_active_scaler_.exchange(int32_t(active_scaler));
        if (previous_requested_scaler != int32_t(requested_scaler) ||
            previous_active_scaler != int32_t(active_scaler)) {
          if (requested_scaler != active_scaler) {
            REXLOG_WARN("MetalPresenter: {} requested, using {} ({})",
                        MetalOutputScalerName(requested_scaler),
                        MetalOutputScalerName(active_scaler), scaler_fallback_reason);
          } else {
            REXLOG_INFO("MetalPresenter: output scaling {}",
                        MetalOutputScalerName(active_scaler));
          }
        }
        guest_presented = presentation_texture != nil;
      }
    }

    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable_texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.015, 0.015, 0.018, 1.0);
    id<MTLRenderCommandEncoder> encoder =
        [command_buffer renderCommandEncoderWithDescriptor:pass];
    if (guest_presented) {
      [encoder setViewport:guest_viewport];
      [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)guest_pipeline_state_];
      [encoder setFragmentTexture:presentation_texture atIndex:0];
      [encoder setFragmentBytes:&presentation_swizzle
                         length:sizeof(presentation_swizzle)
                        atIndex:0];
      [encoder setFragmentBytes:&presentation_parameters
                         length:sizeof(presentation_parameters)
                        atIndex:1];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    }
    if (execute_ui_drawers) {
      MetalUIDrawContext ui_draw_context(*this, uint32_t([drawable_texture width]),
                                         uint32_t([drawable_texture height]), encoder);
      ExecuteUIDrawersFromUIThread(ui_draw_context);
    }
    if (guest_presented && fps_pipeline_state_ && fps_texture && fps_width && fps_height) {
      DrawFpsOverlay(encoder, (id<MTLRenderPipelineState>)fps_pipeline_state_, fps_texture,
                     fps_width, fps_height, guest_viewport, guest_scale, guest_width,
                     guest_height, uint32_t([drawable_texture width]),
                     uint32_t([drawable_texture height]));
    }
    [encoder endEncoding];

    uint64_t present_start_ns = profile_enabled_ ? profiling::NowNs() : 0;
    [command_buffer presentDrawable:drawable];
    if (fps_texture_in_flight) {
      const std::shared_ptr<std::atomic<uint32_t>> completion_in_flight =
          fps_texture_in_flight;
      [command_buffer addCompletedHandler:^(id<MTLCommandBuffer>) {
        completion_in_flight->fetch_sub(1, std::memory_order_acq_rel);
      }];
    }
    if (guest_presented && source_guest_frame_id) {
      const uint64_t performance_frame_id = source_guest_frame_id;
      [command_buffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        const CFTimeInterval gpu_start = completed.GPUStartTime;
        const CFTimeInterval gpu_end = completed.GPUEndTime;
        if (gpu_end > gpu_start) {
          rex::perf::RecordMetalGpuTime(
              uint64_t((gpu_end - gpu_start) * 1000000000.0),
              performance_frame_id);
        }
      }];
    }
    [command_buffer commit];
    // Keep the mailbox image acquired until the read has been committed. This
    // prevents another consumer from releasing the slot back to a producer
    // early enough for its next write to be committed first.
    guest_output_consumer_lock.unlock();
    if (source_texture) {
      [source_texture release];
    }
    if (fps_texture) {
      [fps_texture release];
    }
    EndProfiledPresentAttempt(false, profile_enabled_ ? profiling::ElapsedNs(present_start_ns) : 0);
  }

  return PaintResult::kPresented;
}

}  // namespace rex::ui::metal
