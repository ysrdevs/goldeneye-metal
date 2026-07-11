#include <rex/graphics/metal/command_processor.h>

#include <xxhash.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <array>
#include <memory>
#include <unordered_set>

#include <sys/stat.h>

#include <rex/graphics/flags.h>
#include <rex/graphics/metal/msl_compiler.h>
#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/ui/metal/provider.h>
#include <rex/ui/metal/presenter.h>
#include <rex/ui/presenter.h>

namespace rex::graphics::metal {
namespace {

constexpr uint32_t kWatchedFramebufferBase = 0x1ec30000u;
constexpr uint32_t kWatchedFramebufferWidth = 1280u;
constexpr uint32_t kWatchedFramebufferHeight = 720u;
constexpr uint32_t kWatchedFramebufferLength =
    kWatchedFramebufferWidth * kWatchedFramebufferHeight * 4u;
constexpr uint32_t kWatchedResolveBase = 0x1eeb0000u;
constexpr uint32_t kWatchedResolveLength = 1280u * 720u * 4u;
constexpr uint32_t kWatchedSwapBase = 0x1efc8000u;
constexpr uint32_t kWatchedSwapLength = 1280u * 720u * 4u;

class MetalPrimitiveProcessor final : public PrimitiveProcessor {
 public:
  MetalPrimitiveProcessor(const RegisterFile& register_file, memory::Memory& memory,
                          TraceWriter& trace_writer, SharedMemory& shared_memory)
      : PrimitiveProcessor(register_file, memory, trace_writer, shared_memory) {}

  ~MetalPrimitiveProcessor() override { Shutdown(); }

  bool Initialize() {
    constexpr bool full_32bit_vertex_indices_supported = true;
    constexpr bool triangle_fans_supported = false;
    constexpr bool line_loops_supported = false;
    constexpr bool quad_lists_supported = false;
    constexpr bool point_sprites_supported_without_vs_expansion = false;
    constexpr bool rectangle_lists_supported_without_vs_expansion = false;
    return InitializeCommon(full_32bit_vertex_indices_supported, triangle_fans_supported,
                            line_loops_supported, quad_lists_supported,
                            point_sprites_supported_without_vs_expansion,
                            rectangle_lists_supported_without_vs_expansion);
  }

  bool GetProcessedIndexBufferData(const ProcessingResult& result, const void*& data_out,
                                   size_t& size_out) const {
    size_t index_size = result.host_index_format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t)
                                                                               : sizeof(uint32_t);
    size_t required_size = size_t(result.host_draw_vertex_count) * index_size;
    switch (result.index_buffer_type) {
      case ProcessedIndexBufferType::kHostBuiltinForAuto:
      case ProcessedIndexBufferType::kHostBuiltinForDMA: {
        size_t offset = result.host_index_buffer_handle;
        if (offset > builtin_index_buffer_.size() ||
            required_size > builtin_index_buffer_.size() - offset) {
          return false;
        }
        data_out = builtin_index_buffer_.data() + offset;
        size_out = required_size;
        return true;
      }
      case ProcessedIndexBufferType::kHostConverted: {
        size_t handle = result.host_index_buffer_handle;
        if (handle >= converted_index_buffers_.size() ||
            handle >= converted_index_buffer_offsets_.size()) {
          return false;
        }
        const std::vector<uint8_t>& storage = *converted_index_buffers_[handle];
        size_t offset = converted_index_buffer_offsets_[handle];
        if (offset > storage.size() || required_size > storage.size() - offset) {
          return false;
        }
        data_out = storage.data() + offset;
        size_out = required_size;
        return true;
      }
      default:
        return false;
    }
  }

  void EndFrame() {
    ClearPerFrameCache();
    converted_index_buffers_.clear();
    converted_index_buffer_offsets_.clear();
  }

  void Shutdown() {
    if (shutdown_) {
      return;
    }
    shutdown_ = true;
    converted_index_buffers_.clear();
    converted_index_buffer_offsets_.clear();
    builtin_index_buffer_.clear();
    ShutdownCommon();
  }

 protected:
  bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                    std::function<void(void*)> fill_callback) override {
    builtin_index_buffer_.resize(size_bytes);
    fill_callback(builtin_index_buffer_.data());
    return true;
  }

  void* RequestHostConvertedIndexBufferForCurrentFrame(xenos::IndexFormat format,
                                                       uint32_t index_count, bool coalign_for_simd,
                                                       uint32_t coalignment_original_address,
                                                       size_t& backend_handle_out) override {
    size_t index_size = format == xenos::IndexFormat::kInt16 ? sizeof(uint16_t) : sizeof(uint32_t);
    size_t allocation_size =
        index_size * index_count + (coalign_for_simd ? XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE : 0);
    auto storage = std::make_unique<std::vector<uint8_t>>(allocation_size);
    uint8_t* mapping = storage->data();
    if (coalign_for_simd) {
      mapping += GetSimdCoalignmentOffset(mapping, coalignment_original_address);
    }
    backend_handle_out = converted_index_buffers_.size();
    converted_index_buffer_offsets_.push_back(size_t(mapping - storage->data()));
    converted_index_buffers_.push_back(std::move(storage));
    return mapping;
  }

 private:
  bool shutdown_ = false;
  std::vector<uint8_t> builtin_index_buffer_;
  std::vector<std::unique_ptr<std::vector<uint8_t>>> converted_index_buffers_;
  std::vector<size_t> converted_index_buffer_offsets_;
};

std::unique_ptr<PrimitiveProcessor> CreateMetalPrimitiveProcessor(const RegisterFile& register_file,
                                                                  memory::Memory& memory,
                                                                  TraceWriter& trace_writer,
                                                                  SharedMemory& shared_memory) {
  auto processor =
      std::make_unique<MetalPrimitiveProcessor>(register_file, memory, trace_writer, shared_memory);
  if (!processor->Initialize()) {
    return nullptr;
  }
  return processor;
}

bool RangesOverlap(uint32_t a_start, uint32_t a_length, uint32_t b_start, uint32_t b_length) {
  uint64_t a_end = uint64_t(a_start) + a_length;
  uint64_t b_end = uint64_t(b_start) + b_length;
  return uint64_t(a_start) < b_end && uint64_t(b_start) < a_end;
}

uint32_t SwizzleComponent(uint32_t swizzle, uint32_t component_index) {
  return (swizzle >> (3 * component_index)) & 0b111;
}

uint8_t ResolveSwizzledComponent(const uint8_t* rgba, uint32_t component) {
  switch (component) {
    case xenos::XE_GPU_TEXTURE_SWIZZLE_R:
    case xenos::XE_GPU_TEXTURE_SWIZZLE_G:
    case xenos::XE_GPU_TEXTURE_SWIZZLE_B:
    case xenos::XE_GPU_TEXTURE_SWIZZLE_A:
      return rgba[component];
    case xenos::XE_GPU_TEXTURE_SWIZZLE_1:
      return 0xFF;
    case xenos::XE_GPU_TEXTURE_SWIZZLE_0:
    default:
      return 0;
  }
}

void RgbaToDrawableBgra(const uint8_t* rgba, uint32_t guest_swizzle, uint8_t* bgra) {
  uint8_t sampled[4];
  sampled[0] = ResolveSwizzledComponent(rgba, SwizzleComponent(guest_swizzle, 0));
  sampled[1] = ResolveSwizzledComponent(rgba, SwizzleComponent(guest_swizzle, 1));
  sampled[2] = ResolveSwizzledComponent(rgba, SwizzleComponent(guest_swizzle, 2));
  sampled[3] = ResolveSwizzledComponent(rgba, SwizzleComponent(guest_swizzle, 3));
  bgra[0] = sampled[2];
  bgra[1] = sampled[1];
  bgra[2] = sampled[0];
  bgra[3] = sampled[3];
}

void BgraToRgba(const std::vector<uint8_t>& bgra, uint32_t width, uint32_t height,
                std::vector<uint8_t>& rgba_out) {
  rgba_out.resize(size_t(width) * height * 4);
  for (size_t pixel = 0, pixel_count = size_t(width) * height; pixel < pixel_count; ++pixel) {
    const uint8_t* source = bgra.data() + pixel * 4;
    uint8_t* target = rgba_out.data() + pixel * 4;
    target[0] = source[2];
    target[1] = source[1];
    target[2] = source[0];
    target[3] = source[3];
  }
}

void PackBgraForGuestRgba(const uint8_t* bgra, xenos::Endian128 endian, uint8_t out[4]) {
  uint8_t rgba[4] = {bgra[2], bgra[1], bgra[0], bgra[3]};
  switch (endian) {
    case xenos::Endian128::kNone:
      out[0] = rgba[0];
      out[1] = rgba[1];
      out[2] = rgba[2];
      out[3] = rgba[3];
      break;
    case xenos::Endian128::k8in16:
      out[0] = rgba[1];
      out[1] = rgba[0];
      out[2] = rgba[3];
      out[3] = rgba[2];
      break;
    case xenos::Endian128::k8in32:
      out[0] = rgba[3];
      out[1] = rgba[2];
      out[2] = rgba[1];
      out[3] = rgba[0];
      break;
    case xenos::Endian128::k16in32:
      out[0] = rgba[2];
      out[1] = rgba[3];
      out[2] = rgba[0];
      out[3] = rgba[1];
      break;
    default:
      out[0] = rgba[0];
      out[1] = rgba[1];
      out[2] = rgba[2];
      out[3] = rgba[3];
      break;
  }
}

void UnpackGuestRgba(const uint8_t* packed, xenos::Endian128 endian, uint8_t rgba[4]) {
  switch (endian) {
    case xenos::Endian128::kNone:
      rgba[0] = packed[0];
      rgba[1] = packed[1];
      rgba[2] = packed[2];
      rgba[3] = packed[3];
      break;
    case xenos::Endian128::k8in16:
      rgba[0] = packed[1];
      rgba[1] = packed[0];
      rgba[2] = packed[3];
      rgba[3] = packed[2];
      break;
    case xenos::Endian128::k8in32:
      rgba[0] = packed[3];
      rgba[1] = packed[2];
      rgba[2] = packed[1];
      rgba[3] = packed[0];
      break;
    case xenos::Endian128::k16in32:
      rgba[0] = packed[2];
      rgba[1] = packed[3];
      rgba[2] = packed[0];
      rgba[3] = packed[1];
      break;
    default:
      rgba[0] = packed[0];
      rgba[1] = packed[1];
      rgba[2] = packed[2];
      rgba[3] = packed[3];
      break;
  }
}

bool EnvEnabled(const char* name) {
  const char* value = std::getenv(name);
  return value && value[0] && std::strcmp(value, "0") != 0;
}

bool DebugFrameDumpEnabled() {
  return EnvEnabled("GOLDENEYE_METAL_DUMP_FRAMES");
}

bool MetalShaderDumpEnabled() {
  return EnvEnabled("GOLDENEYE_METAL_DUMP_SHADERS");
}

void DumpBgraFrameAsPpm(const char* label, uint32_t index, const std::vector<uint8_t>& bgra,
                        uint32_t width, uint32_t height) {
  if (!DebugFrameDumpEnabled() || !label || !width || !height ||
      bgra.size() < size_t(width) * height * 4) {
    return;
  }

  constexpr const char* kDumpDir = "/tmp/goldeneye_metal_frames";
  ::mkdir(kDumpDir, 0755);

  char path[256];
  std::snprintf(path, sizeof(path), "%s/%s_%03u_%ux%u.ppm", kDumpDir, label, index, width, height);
  FILE* file = std::fopen(path, "wb");
  if (!file) {
    return;
  }

  std::fprintf(file, "P6\n%u %u\n255\n", width, height);
  std::vector<uint8_t> rgb(size_t(width) * 3);
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* source_row = bgra.data() + size_t(y) * width * 4;
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* source = source_row + size_t(x) * 4;
      uint8_t* target = rgb.data() + size_t(x) * 3;
      target[0] = source[2];
      target[1] = source[1];
      target[2] = source[0];
    }
    std::fwrite(rgb.data(), 1, rgb.size(), file);
  }
  std::fclose(file);

  static std::atomic<uint32_t> frame_dump_logs{0};
  uint32_t frame_dump_index = frame_dump_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (frame_dump_index <= 24) {
    std::fprintf(stderr, "[metal] dumped %s frame#%u to %s\n", label, index, path);
    std::fflush(stderr);
  }
}

uint8_t ToProbeSamplerAddressMode(xenos::ClampMode clamp_mode) {
  switch (clamp_mode) {
    case xenos::ClampMode::kRepeat:
      return 0;
    case xenos::ClampMode::kMirroredRepeat:
      return 1;
    case xenos::ClampMode::kClampToBorder:
    case xenos::ClampMode::kMirrorClampToBorder:
      return 3;
    case xenos::ClampMode::kClampToEdge:
    case xenos::ClampMode::kMirrorClampToEdge:
    case xenos::ClampMode::kClampToHalfway:
    case xenos::ClampMode::kMirrorClampToHalfway:
    default:
      return 2;
  }
}

uint8_t ToProbeSamplerAnisotropy(xenos::AnisoFilter aniso_filter) {
  switch (aniso_filter) {
    case xenos::AnisoFilter::kMax_2_1:
      return 2;
    case xenos::AnisoFilter::kMax_4_1:
      return 4;
    case xenos::AnisoFilter::kMax_8_1:
      return 8;
    case xenos::AnisoFilter::kMax_16_1:
      return 16;
    case xenos::AnisoFilter::kMax_1_1:
    case xenos::AnisoFilter::kDisabled:
    case xenos::AnisoFilter::kUseFetchConst:
    default:
      return 1;
  }
}

ProbeSamplerSlot MakeProbeSamplerSlot(const RegisterFile& register_file,
                                      const SpirvShader::SamplerBinding& binding) {
  xenos::xe_gpu_texture_fetch_t fetch = register_file.GetTextureFetch(binding.fetch_constant);
  ProbeSamplerSlot slot;

  xenos::ClampMode clamp_x;
  xenos::ClampMode clamp_y;
  xenos::ClampMode clamp_z;
  texture_util::GetClampModesForDimension(fetch, clamp_x, clamp_y, clamp_z);
  slot.address_mode_s = ToProbeSamplerAddressMode(clamp_x);
  slot.address_mode_t = ToProbeSamplerAddressMode(clamp_y);
  slot.address_mode_r = ToProbeSamplerAddressMode(clamp_z);

  xenos::TextureFilter mag_filter = binding.mag_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mag_filter
                                        : binding.mag_filter;
  xenos::TextureFilter min_filter = binding.min_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.min_filter
                                        : binding.min_filter;
  xenos::TextureFilter mip_filter = binding.mip_filter == xenos::TextureFilter::kUseFetchConst
                                        ? fetch.mip_filter
                                        : binding.mip_filter;
  slot.mag_linear = mag_filter == xenos::TextureFilter::kLinear;
  slot.min_linear = min_filter == xenos::TextureFilter::kLinear;
  slot.mip_linear = mip_filter == xenos::TextureFilter::kLinear;

  xenos::AnisoFilter aniso_filter = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst
                                        ? fetch.aniso_filter
                                        : binding.aniso_filter;
  slot.max_anisotropy = ToProbeSamplerAnisotropy(aniso_filter);
  if (slot.max_anisotropy > 1) {
    slot.mag_linear = 1;
    slot.min_linear = 1;
    slot.mip_linear = 1;
  }
  return slot;
}

std::vector<ProbeSamplerSlot> MakeProbeSamplerSlots(const RegisterFile& register_file,
                                                    const MetalShader& shader) {
  std::vector<ProbeSamplerSlot> slots;
  const auto& bindings = shader.GetSamplerBindingsAfterTranslation();
  slots.reserve(bindings.size());
  for (const auto& binding : bindings) {
    slots.push_back(MakeProbeSamplerSlot(register_file, binding));
  }
  return slots;
}

constexpr uint8_t kVisibleRgbThreshold = 16;

bool BgraHasNonZeroRgb(const std::vector<uint8_t>& bgra) {
  for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
    if (std::max({bgra[i], bgra[i + 1], bgra[i + 2]}) > kVisibleRgbThreshold) {
      return true;
    }
  }
  return false;
}

uint32_t CountVisibleRgbPixels(const std::vector<uint8_t>& bgra) {
  uint32_t visible_pixels = 0;
  for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
    visible_pixels +=
        std::max({bgra[i], bgra[i + 1], bgra[i + 2]}) > kVisibleRgbThreshold ? 1u : 0u;
  }
  return visible_pixels;
}

uint32_t CountNewVisibleRgbPixels(const std::vector<uint8_t>& before,
                                  const std::vector<uint8_t>& after) {
  size_t byte_count = std::min(before.size(), after.size());
  byte_count -= byte_count % 4;
  uint32_t new_visible_pixels = 0;
  for (size_t i = 0; i + 3 < byte_count; i += 4) {
    bool before_visible =
        std::max({before[i], before[i + 1], before[i + 2]}) > kVisibleRgbThreshold;
    bool after_visible = std::max({after[i], after[i + 1], after[i + 2]}) > kVisibleRgbThreshold;
    new_visible_pixels += !before_visible && after_visible ? 1u : 0u;
  }
  return new_visible_pixels;
}

struct BgraFrameStats {
  uint32_t visible_pixels = 0;
  uint8_t min_rgb = 0xFF;
  uint8_t max_rgb = 0;
};

struct BgraBandStats {
  uint32_t top_208_visible = 0;
  uint32_t mid_208_512_visible = 0;
  uint32_t low_512_visible = 0;
};

struct WatchedGuestRgbaStats {
  uint32_t visible_pixels = 0;
  size_t rgb_nonzero_components = 0;
  uint8_t min_rgb = 0xFF;
  uint8_t max_rgb = 0;
  uint8_t samples[4][4] = {};
};

BgraFrameStats GetBgraFrameStats(const std::vector<uint8_t>& bgra) {
  BgraFrameStats stats;
  if (bgra.empty()) {
    stats.min_rgb = 0;
    return stats;
  }
  for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
    uint8_t pixel_max = std::max({bgra[i], bgra[i + 1], bgra[i + 2]});
    uint8_t pixel_min = std::min({bgra[i], bgra[i + 1], bgra[i + 2]});
    stats.visible_pixels += pixel_max > kVisibleRgbThreshold ? 1u : 0u;
    stats.min_rgb = std::min(stats.min_rgb, pixel_min);
    stats.max_rgb = std::max(stats.max_rgb, pixel_max);
  }
  return stats;
}

BgraBandStats GetBgraBandStats(const std::vector<uint8_t>& bgra, uint32_t width, uint32_t height) {
  BgraBandStats stats;
  if (!width || !height || bgra.size() < size_t(width) * height * 4) {
    return stats;
  }
  for (uint32_t y = 0; y < height; ++y) {
    uint32_t* band_counter = y < 208
                                 ? &stats.top_208_visible
                                 : (y < 512 ? &stats.mid_208_512_visible : &stats.low_512_visible);
    const uint8_t* row = bgra.data() + size_t(y) * width * 4;
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* pixel = row + size_t(x) * 4;
      if (std::max({pixel[0], pixel[1], pixel[2]}) > kVisibleRgbThreshold) {
        ++*band_counter;
      }
    }
  }
  return stats;
}

uint32_t BgraRgbRange(const BgraFrameStats& stats) {
  return uint32_t(stats.max_rgb) - uint32_t(stats.min_rgb);
}

WatchedGuestRgbaStats GetWatchedGuestRgbaStats(const uint8_t* tiled_base, xenos::Endian128 endian) {
  WatchedGuestRgbaStats stats;
  if (!tiled_base) {
    stats.min_rgb = 0;
    return stats;
  }
  const uint32_t sample_y[4] = {
      0,
      kWatchedFramebufferHeight / 4,
      kWatchedFramebufferHeight / 2,
      (kWatchedFramebufferHeight * 3) / 4,
  };
  for (uint32_t y = 0; y < kWatchedFramebufferHeight; ++y) {
    for (uint32_t x = 0; x < kWatchedFramebufferWidth; ++x) {
      uint32_t tiled_offset = uint32_t(
          texture_util::GetTiledOffset2D(int32_t(x), int32_t(y), kWatchedFramebufferWidth, 2));
      uint8_t rgba[4] = {};
      UnpackGuestRgba(tiled_base + tiled_offset, endian, rgba);
      uint8_t pixel_min = std::min({rgba[0], rgba[1], rgba[2]});
      uint8_t pixel_max = std::max({rgba[0], rgba[1], rgba[2]});
      stats.min_rgb = std::min(stats.min_rgb, pixel_min);
      stats.max_rgb = std::max(stats.max_rgb, pixel_max);
      stats.visible_pixels += pixel_max > kVisibleRgbThreshold ? 1u : 0u;
      stats.rgb_nonzero_components += rgba[0] != 0;
      stats.rgb_nonzero_components += rgba[1] != 0;
      stats.rgb_nonzero_components += rgba[2] != 0;
      for (uint32_t sample_index = 0; sample_index < rex::countof(sample_y); ++sample_index) {
        if (!x && y == sample_y[sample_index]) {
          std::memcpy(stats.samples[sample_index], rgba, sizeof(rgba));
        }
      }
    }
  }
  return stats;
}

uint32_t BgraSpatialSampleRange(const std::vector<uint8_t>& bgra, uint32_t width, uint32_t height) {
  if (!width || !height || bgra.size() < size_t(width) * height * 4) {
    return 0;
  }
  uint8_t min_rgb = 0xFF;
  uint8_t max_rgb = 0;
  const uint32_t sample_points[4][2] = {
      {0, 0},
      {width / 4, height / 4},
      {width / 2, height / 2},
      {width * 3 / 4, height * 3 / 4},
  };
  for (const auto& sample_point : sample_points) {
    uint32_t x = std::min(sample_point[0], width - 1);
    uint32_t y = std::min(sample_point[1], height - 1);
    const uint8_t* pixel = bgra.data() + (size_t(y) * width + x) * 4;
    for (uint32_t c = 0; c < 3; ++c) {
      min_rgb = std::min(min_rgb, pixel[c]);
      max_rgb = std::max(max_rgb, pixel[c]);
    }
  }
  return uint32_t(max_rgb) - uint32_t(min_rgb);
}

uint32_t BgraSpatialSampleColorDistance(const std::vector<uint8_t>& bgra, uint32_t width,
                                        uint32_t height) {
  if (!width || !height || bgra.size() < size_t(width) * height * 4) {
    return 0;
  }
  uint8_t min_component[3] = {0xFF, 0xFF, 0xFF};
  uint8_t max_component[3] = {0, 0, 0};
  const uint32_t sample_points[4][2] = {
      {0, 0},
      {width / 4, height / 4},
      {width / 2, height / 2},
      {width * 3 / 4, height * 3 / 4},
  };
  for (const auto& sample_point : sample_points) {
    uint32_t x = std::min(sample_point[0], width - 1);
    uint32_t y = std::min(sample_point[1], height - 1);
    const uint8_t* pixel = bgra.data() + (size_t(y) * width + x) * 4;
    for (uint32_t c = 0; c < 3; ++c) {
      min_component[c] = std::min(min_component[c], pixel[c]);
      max_component[c] = std::max(max_component[c], pixel[c]);
    }
  }
  uint32_t distance = 0;
  for (uint32_t c = 0; c < 3; ++c) {
    distance = std::max(distance, uint32_t(max_component[c]) - uint32_t(min_component[c]));
  }
  return distance;
}

uint32_t BgraVisibleSpatialSampleColorDistance(const std::vector<uint8_t>& bgra, uint32_t width,
                                               uint32_t height) {
  if (!width || !height || bgra.size() < size_t(width) * height * 4) {
    return 0;
  }
  uint8_t min_component[3] = {0xFF, 0xFF, 0xFF};
  uint8_t max_component[3] = {0, 0, 0};
  uint32_t visible_samples = 0;
  for (uint32_t y_index = 0; y_index < 5; ++y_index) {
    uint32_t y = height == 1 ? 0 : (height - 1) * y_index / 4;
    for (uint32_t x_index = 0; x_index < 5; ++x_index) {
      uint32_t x = width == 1 ? 0 : (width - 1) * x_index / 4;
      const uint8_t* pixel = bgra.data() + (size_t(y) * width + x) * 4;
      if (std::max({pixel[0], pixel[1], pixel[2]}) <= kVisibleRgbThreshold) {
        continue;
      }
      ++visible_samples;
      for (uint32_t c = 0; c < 3; ++c) {
        min_component[c] = std::min(min_component[c], pixel[c]);
        max_component[c] = std::max(max_component[c], pixel[c]);
      }
    }
  }
  if (visible_samples < 2) {
    return 0;
  }
  uint32_t distance = 0;
  for (uint32_t c = 0; c < 3; ++c) {
    distance = std::max(distance, uint32_t(max_component[c]) - uint32_t(min_component[c]));
  }
  return distance;
}

uint32_t BgraVisibleGridCellCount(const std::vector<uint8_t>& bgra, uint32_t width, uint32_t height,
                                  uint32_t grid_width = 8, uint32_t grid_height = 8) {
  if (!width || !height || !grid_width || !grid_height ||
      bgra.size() < size_t(width) * height * 4) {
    return 0;
  }
  uint32_t visible_cells = 0;
  for (uint32_t gy = 0; gy < grid_height; ++gy) {
    uint32_t y0 = gy * height / grid_height;
    uint32_t y1 = std::max<uint32_t>((gy + 1) * height / grid_height, y0 + 1);
    y1 = std::min(y1, height);
    for (uint32_t gx = 0; gx < grid_width; ++gx) {
      uint32_t x0 = gx * width / grid_width;
      uint32_t x1 = std::max<uint32_t>((gx + 1) * width / grid_width, x0 + 1);
      x1 = std::min(x1, width);
      bool cell_visible = false;
      for (uint32_t y = y0; y < y1 && !cell_visible; ++y) {
        const uint8_t* row = bgra.data() + size_t(y) * width * 4;
        for (uint32_t x = x0; x < x1; ++x) {
          const uint8_t* pixel = row + size_t(x) * 4;
          if (std::max({pixel[0], pixel[1], pixel[2]}) > kVisibleRgbThreshold) {
            cell_visible = true;
            break;
          }
        }
      }
      visible_cells += cell_visible ? 1u : 0u;
    }
  }
  return visible_cells;
}

bool IsDominantFlatVisibleFrame(const std::vector<uint8_t>& bgra, uint32_t width, uint32_t height,
                                uint32_t visible_pixels) {
  if (!width || !height) {
    return false;
  }
  uint32_t pixel_count = width * height;
  return visible_pixels * 4 >= pixel_count * 3 &&
         BgraVisibleSpatialSampleColorDistance(bgra, width, height) <= 8;
}

bool IsUsefulSparseVisibleFrame(const std::vector<uint8_t>& bgra, uint32_t width, uint32_t height,
                                const BgraFrameStats& stats) {
  if (!width || !height || !stats.visible_pixels) {
    return false;
  }
  uint32_t pixel_count = width * height;
  return stats.visible_pixels * 4 >= pixel_count &&
         BgraVisibleGridCellCount(bgra, width, height) >= 12 && BgraRgbRange(stats) >= 64;
}

bool ShouldPreferCandidateFrame(uint32_t current_visible, uint32_t current_width,
                                uint32_t current_height, const std::vector<uint8_t>& candidate_bgra,
                                uint32_t candidate_width, uint32_t candidate_height) {
  if (!candidate_width || !candidate_height || candidate_bgra.empty()) {
    return false;
  }
  uint32_t candidate_visible = CountVisibleRgbPixels(candidate_bgra);
  if (!candidate_visible) {
    return false;
  }
  if (IsDominantFlatVisibleFrame(candidate_bgra, candidate_width, candidate_height,
                                 candidate_visible)) {
    return false;
  }
  uint32_t current_pixels = current_width && current_height ? current_width * current_height : 0;
  uint32_t candidate_pixels = candidate_width * candidate_height;
  if (!current_visible) {
    return true;
  }
  // The native path currently sees partial guest resolves during early boot.
  // Prefer a host-rendered frame when it covers materially more of the screen.
  return candidate_visible * 4 >= candidate_pixels * 3 &&
         (current_visible * 3 < current_pixels ||
          candidate_visible > current_visible + current_visible / 2);
}

uint64_t CandidateFrameScore(const BgraFrameStats& stats, uint32_t spatial_distance = 0) {
  return uint64_t(stats.visible_pixels) +
         uint64_t(std::min<uint32_t>(BgraRgbRange(stats), 64)) * UINT64_C(4096) +
         uint64_t(std::min<uint32_t>(spatial_distance, 255)) * UINT64_C(2048);
}

void DumpFailedMslSource(const MetalShader::MetalTranslation& translation) {
  if (!MetalShaderDumpEnabled()) {
    return;
  }
  const Shader& shader = translation.shader();
  char path[256];
  std::snprintf(path, sizeof(path), "/tmp/goldeneye_metal_failed_%016llx_%016llx.%s.metal",
                static_cast<unsigned long long>(shader.ucode_data_hash()),
                static_cast<unsigned long long>(translation.modification()),
                shader.type() == xenos::ShaderType::kVertex ? "vert" : "frag");
  FILE* file = std::fopen(path, "wb");
  if (!file) {
    return;
  }
  const std::string& source = translation.msl_source();
  if (!source.empty()) {
    std::fwrite(source.data(), 1, source.size(), file);
  }
  std::fclose(file);
  std::fprintf(stderr, "[metal] wrote failed MSL source: %s\n", path);
  std::fflush(stderr);
}

void DumpTargetedMslSource(const MetalShader::MetalTranslation& translation) {
  if (!MetalShaderDumpEnabled()) {
    return;
  }
  const Shader& shader = translation.shader();
  const uint64_t shader_hash = shader.ucode_data_hash();
  if (shader_hash != UINT64_C(0x0a6d1dd7767fdf27) && shader_hash != UINT64_C(0x2e372ea28cc404b7) &&
      shader_hash != UINT64_C(0xb60fecd01106e24e) && shader_hash != UINT64_C(0x21243b8826e3f416) &&
      shader_hash != UINT64_C(0x0b3d5d2e102ac7a0) && shader_hash != UINT64_C(0x53a2a430cb2f1045) &&
      shader_hash != UINT64_C(0x0c2722aa3370cd20) && shader_hash != UINT64_C(0x354e278093e0ab93) &&
      shader_hash != UINT64_C(0xe56d102869a881a2) && shader_hash != UINT64_C(0x46d9c92fae2244df) &&
      shader_hash != UINT64_C(0x3a1fe1560cf25ff6) && shader_hash != UINT64_C(0x0f7f3153e66fa452) &&
      shader_hash != UINT64_C(0x1f207d90237c9c25) && shader_hash != UINT64_C(0xbdc93d3c5da8241f) &&
      shader_hash != UINT64_C(0xde0cec0ab06dd09c) && shader_hash != UINT64_C(0x72cbcaa6a7984111) &&
      shader_hash != UINT64_C(0x8163ce3defa0f50c) && shader_hash != UINT64_C(0xcfa7d5aab3979187)) {
    return;
  }

  const char* stage = shader.type() == xenos::ShaderType::kVertex ? "vert" : "frag";
  char path[256];
  std::snprintf(path, sizeof(path), "/tmp/goldeneye_metal_target_%s_%016llx_%016llx.metal", stage,
                static_cast<unsigned long long>(shader_hash),
                static_cast<unsigned long long>(translation.modification()));
  FILE* file = std::fopen(path, "wb");
  if (!file) {
    return;
  }
  const std::string& source = translation.msl_source();
  if (!source.empty()) {
    std::fwrite(source.data(), 1, source.size(), file);
  }
  std::fclose(file);
  auto ucode_paths = shader.DumpUcode("/tmp/goldeneye_metal_target_ucode");
  std::fprintf(stderr, "[metal] dumped targeted MSL: %s ucode_bin=%s ucode_disasm=%s\n", path,
               ucode_paths.first.string().c_str(), ucode_paths.second.string().c_str());
  std::fflush(stderr);
}

constexpr bool kEnableCpuInterpretedVertices = true;
constexpr bool kEnableHostVertexShaderVariants = true;
constexpr bool kEnableSyntheticFullscreenTexturedQuad = false;
constexpr bool kEnableHostTextureFallback = true;
constexpr uint32_t kCpuVertexShaderInstructionBudget = 4096;
constexpr uint32_t kMaxHostPixelDrawsPerSwap = 24;
constexpr uint32_t kMaxHostPixelDrawsPerShaderPerSwap = 24;
constexpr uint32_t kMaxHostFallbackPixelDrawsPerSwap = 256;

bool MetalPipelineProbesEnabled() {
  return EnvEnabled("GOLDENEYE_METAL_PIPELINE_PROBE");
}

bool MetalHostRenderTargetDebugEnabled() {
  return EnvEnabled("GOLDENEYE_METAL_HOST_RT_DEBUG") ||
         EnvEnabled("GOLDENEYE_METAL_HOST_RT_DEBUG_PRESENT");
}

bool MetalHostRenderTargetDebugPresentEnabled() {
  return EnvEnabled("GOLDENEYE_METAL_HOST_RT_DEBUG_PRESENT");
}

bool MetalHostRenderTargetSolidTestEnabled() {
  return EnvEnabled("GOLDENEYE_METAL_HOST_RT_SOLID_TEST");
}

bool MetalHostRenderTargetTextureAliasEnabled() {
  return EnvEnabled("GOLDENEYE_METAL_HOST_RT_TEXTURE_ALIAS");
}

bool MetalFullscreenProbeEnabled() {
  return EnvEnabled("GOLDENEYE_METAL_FULLSCREEN_PROBE");
}

bool MetalHeuristicPresentationEnabled() {
  return EnvEnabled("GOLDENEYE_METAL_HEURISTIC_PRESENT");
}

bool MetalFallbackResolveEnabled() {
  return EnvEnabled("GOLDENEYE_METAL_FALLBACK_RESOLVE");
}

bool MetalHostPixelDiagnosticsEnabled() {
  return MetalHeuristicPresentationEnabled() || MetalFallbackResolveEnabled() ||
         EnvEnabled("GOLDENEYE_METAL_HOST_PIXEL_DIAGNOSTICS");
}

bool MetalMagentaResolveEnabled() {
  return EnvEnabled("GOLDENEYE_METAL_MAGENTA_RESOLVE");
}

bool IsKnownUnsafeHostPixelShader(uint64_t pixel_shader_hash) {
  return pixel_shader_hash == UINT64_C(0xb60fecd01106e24e);
}

bool IsVoidFragmentMsl(const MetalShader::MetalTranslation* translation) {
  return translation && translation->shader().type() == xenos::ShaderType::kPixel &&
         translation->msl_source().find("fragment void main0") != std::string::npos;
}

bool ShouldUseHostFallbackPixelShader(uint64_t pixel_shader_hash,
                                      const MetalShader::MetalTranslation* translation) {
  if (!MetalHostPixelDiagnosticsEnabled()) {
    return false;
  }
  return IsKnownUnsafeHostPixelShader(pixel_shader_hash) || IsVoidFragmentMsl(translation);
}

float HashColorComponent(uint64_t hash, uint32_t shift) {
  return float((hash >> shift) & 0xFF) / 255.0f;
}

std::vector<uint32_t> PackFloatConstantsForShader(const RegisterFile& regs, const Shader& shader) {
  std::vector<uint32_t> packed;
  const Shader::ConstantRegisterMap& constant_map = shader.constant_register_map();
  packed.reserve(size_t(constant_map.float_count) * 4);
  uint32_t shader_base = shader.type() == xenos::ShaderType::kVertex
                             ? XE_GPU_REG_SHADER_CONSTANT_000_X
                             : XE_GPU_REG_SHADER_CONSTANT_256_X;
  for (uint32_t block = 0; block < rex::countof(constant_map.float_bitmap); ++block) {
    uint64_t bits = constant_map.float_bitmap[block];
    uint32_t bit_index = 0;
    while (rex::bit_scan_forward(bits, &bit_index)) {
      bits &= ~(UINT64_C(1) << bit_index);
      uint32_t storage_index = block * 64 + bit_index;
      const uint32_t* source = &regs[shader_base + storage_index * 4];
      packed.insert(packed.end(), source, source + 4);
    }
  }
  return packed;
}

class VertexExportSink final : public ShaderInterpreter::ExportSink {
 public:
  static constexpr uint32_t kInterpolatorCount = 16;

  void Export(ucode::ExportRegister export_register, const float* value,
              uint32_t value_mask) override {
    if (export_register == ucode::ExportRegister::kVSPosition) {
      for (uint32_t i = 0; i < 4; ++i) {
        if (value_mask & (UINT32_C(1) << i)) {
          position[i] = value[i];
        }
      }
      has_position = true;
    } else if (export_register >= ucode::ExportRegister::kVSInterpolator0 &&
               export_register <= ucode::ExportRegister::kVSInterpolator15) {
      uint32_t interpolator_index =
          uint32_t(export_register) - uint32_t(ucode::ExportRegister::kVSInterpolator0);
      for (uint32_t i = 0; i < 4; ++i) {
        if (value_mask & (UINT32_C(1) << i)) {
          interpolators[interpolator_index][i] = value[i];
        }
      }
      has_interpolator[interpolator_index] = true;
      if (interpolator_index == 0) {
        std::memcpy(interpolator0, interpolators[0].data(), sizeof(interpolator0));
        has_interpolator0 = true;
      }
    }
  }

  float position[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  std::array<std::array<float, 4>, kInterpolatorCount> interpolators = {};
  std::array<bool, kInterpolatorCount> has_interpolator = {};
  float interpolator0[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  bool has_position = false;
  bool has_interpolator0 = false;
};

bool HasAnyInterpolator(const VertexExportSink& sink) {
  for (bool has_interpolator : sink.has_interpolator) {
    if (has_interpolator) {
      return true;
    }
  }
  return false;
}

class PixelExportSink final : public ShaderInterpreter::ExportSink {
 public:
  void Export(ucode::ExportRegister export_register, const float* value,
              uint32_t value_mask) override {
    if (export_register != ucode::ExportRegister::kPSColor0) {
      return;
    }
    for (uint32_t i = 0; i < 4; ++i) {
      if (value_mask & (UINT32_C(1) << i)) {
        color0[i] = value[i];
      }
    }
    has_color0 = true;
  }

  float color0[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  bool has_color0 = false;
};

class MemExportTraceSink final : public ShaderInterpreter::ExportSink {
 public:
  void AllocExport(ucode::AllocType type, uint32_t size) override {
    alloc_type = type;
    alloc_size = size;
    saw_alloc = true;
  }

  void Export(ucode::ExportRegister export_register, const float* value,
              uint32_t value_mask) override {
    if (export_register == ucode::ExportRegister::kExportAddress) {
      for (uint32_t i = 0; i < 4; ++i) {
        if (value_mask & (UINT32_C(1) << i)) {
          address[i] = value[i];
        }
      }
      address_mask |= value_mask;
      saw_address = true;
    } else if (export_register >= ucode::ExportRegister::kExportData0 &&
               export_register <= ucode::ExportRegister::kExportData4) {
      uint32_t data_index =
          uint32_t(export_register) - uint32_t(ucode::ExportRegister::kExportData0);
      for (uint32_t i = 0; i < 4; ++i) {
        if (value_mask & (UINT32_C(1) << i)) {
          data[data_index][i] = value[i];
        }
      }
      data_mask[data_index] |= value_mask;
      saw_data[data_index] = true;
    }
  }

  ucode::AllocType alloc_type = ucode::AllocType::kNone;
  uint32_t alloc_size = 0;
  float address[4] = {};
  std::array<std::array<float, 4>, 5> data = {};
  std::array<uint32_t, 5> data_mask = {};
  std::array<bool, 5> saw_data = {};
  uint32_t address_mask = 0;
  bool saw_alloc = false;
  bool saw_address = false;
};

void TraceCpuMemExportShader(const RegisterFile& register_file, const memory::Memory& memory,
                             MetalShader& vertex_shader, xenos::PrimitiveType prim_type,
                             uint32_t index_count, uint32_t draw_index) {
  if (vertex_shader.ucode_data_hash() != UINT64_C(0x0b3d5d2e102ac7a0) ||
      !ShaderInterpreter::CanInterpretShader(vertex_shader)) {
    return;
  }
  static std::atomic<uint32_t> cpu_memexport_trace_logs{0};
  uint32_t trace_index = cpu_memexport_trace_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (trace_index > 8 && (trace_index & 0xFF) != 0) {
    return;
  }

  uint32_t invocation_count = std::min<uint32_t>(index_count, 6);
  for (uint32_t invocation = 0; invocation < invocation_count; ++invocation) {
    ShaderInterpreter interpreter(register_file, memory);
    interpreter.SetShader(vertex_shader);
    std::fill(interpreter.temp_registers(),
              interpreter.temp_registers() + xenos::kMaxShaderTempRegisters * 4, 0.0f);
    interpreter.temp_registers()[0] = float(invocation);
    MemExportTraceSink sink;
    interpreter.SetExportSink(&sink);
    bool ok = interpreter.ExecuteWithInstructionBudget(kCpuVertexShaderInstructionBudget);
    std::fprintf(stderr,
                 "[metal] cpu memexport trace#%u draw=%u prim=%u indices=%u inv=%u ok=%u "
                 "alloc=%u/%u eA=%u mask=0x%x addr=(%.4g %.4g %.4g %.4g) "
                 "eM2=%u mask=0x%x data=(%.4g %.4g %.4g %.4g)\n",
                 trace_index, draw_index, uint32_t(prim_type), index_count, invocation,
                 ok ? 1u : 0u, sink.saw_alloc ? uint32_t(sink.alloc_type) : 0u, sink.alloc_size,
                 sink.saw_address ? 1u : 0u, sink.address_mask, sink.address[0], sink.address[1],
                 sink.address[2], sink.address[3], sink.saw_data[2] ? 1u : 0u, sink.data_mask[2],
                 sink.data[2][0], sink.data[2][1], sink.data[2][2], sink.data[2][3]);
  }
  std::fflush(stderr);
}

struct InterpretedVertex {
  float guest_position[4];
  float position[4];
  float color[4];
  std::array<std::array<float, 4>, VertexExportSink::kInterpolatorCount> interpolators = {};
  std::array<bool, VertexExportSink::kInterpolatorCount> has_interpolator = {};
  float texcoord[2] = {};
  bool has_color = false;
  bool has_texcoord = false;
};

struct HostPixelProbeVertex {
  float position[4];
  std::array<std::array<float, 4>, VertexExportSink::kInterpolatorCount> interpolators = {};
};

std::array<uint32_t, VertexExportSink::kInterpolatorCount> GetMslPixelInterpolatorByLocation(
    const std::string& source) {
  std::array<uint32_t, VertexExportSink::kInterpolatorCount> interpolator_by_location = {};
  for (uint32_t i = 0; i < VertexExportSink::kInterpolatorCount; ++i) {
    interpolator_by_location[i] = i;
  }
  constexpr const char* kInterpolatorPrefix = "xe_in_interpolator_";
  constexpr const char* kUserLocationPrefix = "[[user(locn";
  size_t search_pos = 0;
  while (true) {
    size_t name_pos = source.find(kInterpolatorPrefix, search_pos);
    if (name_pos == std::string::npos) {
      break;
    }
    size_t interpolator_pos = name_pos + std::strlen(kInterpolatorPrefix);
    uint32_t interpolator_index = 0;
    bool saw_interpolator_digit = false;
    while (interpolator_pos < source.size() && source[interpolator_pos] >= '0' &&
           source[interpolator_pos] <= '9') {
      saw_interpolator_digit = true;
      interpolator_index = interpolator_index * 10 + uint32_t(source[interpolator_pos] - '0');
      ++interpolator_pos;
    }
    if (!saw_interpolator_digit || interpolator_index >= VertexExportSink::kInterpolatorCount) {
      search_pos = interpolator_pos;
      continue;
    }
    size_t attribute_pos = source.find(kUserLocationPrefix, interpolator_pos);
    size_t next_interpolator_pos = source.find(kInterpolatorPrefix, interpolator_pos);
    if (attribute_pos == std::string::npos ||
        (next_interpolator_pos != std::string::npos && attribute_pos > next_interpolator_pos)) {
      search_pos = interpolator_pos;
      continue;
    }
    attribute_pos += std::strlen(kUserLocationPrefix);
    uint32_t location = 0;
    bool saw_location_digit = false;
    while (attribute_pos < source.size() && source[attribute_pos] >= '0' &&
           source[attribute_pos] <= '9') {
      saw_location_digit = true;
      location = location * 10 + uint32_t(source[attribute_pos] - '0');
      ++attribute_pos;
    }
    if (saw_location_digit && location < VertexExportSink::kInterpolatorCount) {
      interpolator_by_location[location] = interpolator_index;
    }
    search_pos = interpolator_pos;
  }
  return interpolator_by_location;
}

bool LooksLikeNormalizedColor(const float* color) {
  for (uint32_t i = 0; i < 4; ++i) {
    if (!std::isfinite(color[i]) || color[i] < -0.001f || color[i] > 1.001f) {
      return false;
    }
  }
  return true;
}

bool HasVisibleRgb(const float* color) {
  return std::max({color[0], color[1], color[2]}) >= 0.08f;
}

std::string MakeFullscreenProbeVertexMsl() {
  std::string source = R"(
#include <metal_stdlib>
using namespace metal;

struct main0_out {
)";
  for (uint32_t i = 0; i < VertexExportSink::kInterpolatorCount; ++i) {
    source += "  float4 xe_out_interpolator_" + std::to_string(i) + " [[user(locn" +
              std::to_string(i) + ")]];\n";
  }
  source += R"(  float4 gl_Position [[position]];
};

vertex main0_out main0(uint vertex_id [[vertex_id]]) {
  constexpr float2 positions[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0),
  };
  float2 position = positions[vertex_id];
  float2 uv = float2(position.x * 0.5 + 0.5, 0.5 - position.y * 0.5);
  main0_out out;
  out.gl_Position = float4(position, 0.0, 1.0);
  float4 interpolator = float4(uv.x, uv.y, uv.x, uv.y);
)";
  for (uint32_t i = 0; i < VertexExportSink::kInterpolatorCount; ++i) {
    source += "  out.xe_out_interpolator_" + std::to_string(i) + " = interpolator;\n";
  }
  source += R"(  return out;
}
)";
  return source;
}

std::string MakeHostPixelProbeVertexMsl() {
  std::string source = R"(
#include <metal_stdlib>
using namespace metal;

struct HostPixelProbeVertex {
  float4 position;
  float4 interpolators[16];
};

struct main0_out {
)";
  for (uint32_t i = 0; i < VertexExportSink::kInterpolatorCount; ++i) {
    source += "  float4 xe_out_interpolator_" + std::to_string(i) + " [[user(locn" +
              std::to_string(i) + ")]];\n";
  }
  source += R"(  float4 gl_Position [[position]];
};

vertex main0_out main0(uint vertex_id [[vertex_id]],
                       constant HostPixelProbeVertex* vertices [[buffer(3)]]) {
  HostPixelProbeVertex host_vertex = vertices[vertex_id];
  main0_out out;
  out.gl_Position = host_vertex.position;
)";
  for (uint32_t i = 0; i < VertexExportSink::kInterpolatorCount; ++i) {
    source += "  out.xe_out_interpolator_" + std::to_string(i) + " = host_vertex.interpolators[" +
              std::to_string(i) + "];\n";
  }
  source += R"(  return out;
}
)";
  return source;
}

std::string MakeDummyFragmentMsl() {
  return R"(
#include <metal_stdlib>
using namespace metal;

fragment float4 main0() {
  return float4(0.0, 0.0, 0.0, 0.0);
}
)";
}

std::string MakeHostFallbackPixelFragmentMsl() {
  return R"(
#include <metal_stdlib>
using namespace metal;

struct main0_in {
  float4 fallback_color [[user(locn15)]];
};

fragment float4 main0(main0_in in [[stage_in]]) {
  float3 rgb = saturate(in.fallback_color.rgb);
  if (max(max(rgb.r, rgb.g), rgb.b) < 0.02) {
    rgb = float3(0.12, 0.16, 0.22);
  }
  float alpha = max(saturate(in.fallback_color.a), 1.0 / 255.0);
  return float4(rgb, alpha);
}
)";
}

std::string MakeHostVertexColorPixelFragmentMsl() {
  return R"(
#include <metal_stdlib>
using namespace metal;

struct main0_in {
  float4 color [[user(locn0)]];
};

fragment float4 main0(main0_in in [[stage_in]]) {
  return saturate(in.color);
}
)";
}

std::string MakeSolidFragmentMsl() {
  return R"(
#include <metal_stdlib>
using namespace metal;

fragment float4 main0() {
  return float4(1.0, 0.0, 1.0, 1.0);
}
)";
}

bool MslWritesSharedMemory(const MetalShader::MetalTranslation* translation) {
  if (!translation) {
    return false;
  }
  const std::string& msl_source = translation->msl_source();
  if (msl_source.find("atomic_fetch_") != std::string::npos) {
    return true;
  }
  constexpr const char* kSharedMemoryAccess = "xe_shared_memory[";
  constexpr const char* kWrappedSharedMemoryAccess = "xe_shared_memory.shared_memory[";
  size_t search_offset = 0;
  while (true) {
    size_t access_offset = msl_source.find(kSharedMemoryAccess, search_offset);
    if (access_offset == std::string::npos) {
      access_offset = msl_source.find(kWrappedSharedMemoryAccess, search_offset);
    }
    if (access_offset == std::string::npos) {
      return false;
    }
    size_t bracket_offset = msl_source.find(']', access_offset);
    if (bracket_offset == std::string::npos) {
      return false;
    }
    size_t operator_offset = msl_source.find_first_not_of(" \t\r\n", bracket_offset + 1);
    if (operator_offset != std::string::npos && msl_source[operator_offset] == '=') {
      return true;
    }
    search_offset = bracket_offset + 1;
  }
}

bool HasMemExportSideEffects(const MetalShader& shader,
                             const MetalShader::MetalTranslation* translation) {
  return shader.memexport_eM_written() || MslWritesSharedMemory(translation);
}

uint32_t FindMslBufferIndex(const std::string& source, const char* parameter_name) {
  size_t parameter_pos = source.find(parameter_name);
  if (parameter_pos == std::string::npos) {
    return UINT32_MAX;
  }
  size_t buffer_pos = source.find("[[buffer(", parameter_pos);
  if (buffer_pos == std::string::npos) {
    return UINT32_MAX;
  }
  buffer_pos += std::strlen("[[buffer(");
  uint32_t index = 0;
  bool saw_digit = false;
  while (buffer_pos < source.size() && source[buffer_pos] >= '0' && source[buffer_pos] <= '9') {
    saw_digit = true;
    index = index * 10 + uint32_t(source[buffer_pos] - '0');
    ++buffer_pos;
  }
  return saw_digit ? index : UINT32_MAX;
}

std::vector<uint32_t> GetMslTextureFetchConstantsByBindingIndex(const std::string& source) {
  std::vector<uint32_t> fetch_constants_by_binding_index;
  constexpr const char* kTexturePrefix = "xe_texture";
  constexpr const char* kTextureAttribute = "[[texture(";
  size_t search_pos = 0;
  while (true) {
    size_t texture_name_pos = source.find(kTexturePrefix, search_pos);
    if (texture_name_pos == std::string::npos) {
      break;
    }
    size_t fetch_pos = texture_name_pos + std::strlen(kTexturePrefix);
    uint32_t fetch_constant = 0;
    bool saw_fetch_digit = false;
    while (fetch_pos < source.size() && source[fetch_pos] >= '0' && source[fetch_pos] <= '9') {
      saw_fetch_digit = true;
      fetch_constant = fetch_constant * 10 + uint32_t(source[fetch_pos] - '0');
      ++fetch_pos;
    }
    if (!saw_fetch_digit) {
      search_pos = fetch_pos;
      continue;
    }
    size_t attribute_pos = source.find(kTextureAttribute, fetch_pos);
    size_t next_parameter_pos = source.find("xe_texture", fetch_pos);
    if (attribute_pos == std::string::npos ||
        (next_parameter_pos != std::string::npos && attribute_pos > next_parameter_pos)) {
      search_pos = fetch_pos;
      continue;
    }
    attribute_pos += std::strlen(kTextureAttribute);
    uint32_t binding_index = 0;
    bool saw_binding_digit = false;
    while (attribute_pos < source.size() && source[attribute_pos] >= '0' &&
           source[attribute_pos] <= '9') {
      saw_binding_digit = true;
      binding_index = binding_index * 10 + uint32_t(source[attribute_pos] - '0');
      ++attribute_pos;
    }
    if (saw_binding_digit) {
      if (binding_index >= fetch_constants_by_binding_index.size()) {
        fetch_constants_by_binding_index.resize(size_t(binding_index) + 1, UINT32_MAX);
      }
      fetch_constants_by_binding_index[binding_index] = fetch_constant;
    }
    search_pos = fetch_pos;
  }
  while (!fetch_constants_by_binding_index.empty() &&
         fetch_constants_by_binding_index.back() == UINT32_MAX) {
    fetch_constants_by_binding_index.pop_back();
  }
  return fetch_constants_by_binding_index;
}

struct VertexMslBufferBindings {
  uint32_t shared_memory = UINT32_MAX;
  uint32_t float_constants = UINT32_MAX;
  uint32_t bool_loop_constants = UINT32_MAX;
  uint32_t fetch_constants = UINT32_MAX;
};

VertexMslBufferBindings GetVertexMslBufferBindings(
    const MetalShader::MetalTranslation* translation) {
  VertexMslBufferBindings bindings;
  if (!translation) {
    return bindings;
  }
  const std::string& source = translation->msl_source();
  bindings.shared_memory = FindMslBufferIndex(source, "xe_shared_memory");
  bindings.float_constants = FindMslBufferIndex(source, "xe_uniform_float_constants");
  bindings.bool_loop_constants = FindMslBufferIndex(source, "xe_uniform_bool_loop_constants");
  bindings.fetch_constants = FindMslBufferIndex(source, "xe_uniform_fetch_constants");
  return bindings;
}

float RectangleEdgeLengthSquared(const InterpretedVertex& a, const InterpretedVertex& b) {
  float dx = b.position[0] - a.position[0];
  float dy = b.position[1] - a.position[1];
  return dx * dx + dy * dy;
}

std::array<InterpretedVertex, 4> OrderRectangleVertices(const InterpretedVertex& v0,
                                                        const InterpretedVertex& v1,
                                                        const InterpretedVertex& v2) {
  const InterpretedVertex* source[3] = {&v0, &v1, &v2};
  float edge_lengths[3] = {
      RectangleEdgeLengthSquared(v1, v2),
      RectangleEdgeLengthSquared(v2, v0),
      RectangleEdgeLengthSquared(v0, v1),
  };
  uint32_t first_index = (edge_lengths[0] > edge_lengths[1] && edge_lengths[0] > edge_lengths[2])
                             ? 0
                             : (edge_lengths[1] > edge_lengths[2] ? 1 : 2);

  std::array<InterpretedVertex, 4> rectangle_vertices = {
      *source[first_index], *source[(first_index + 1) % 3], *source[(first_index + 2) % 3],
      InterpretedVertex{}};
  InterpretedVertex& synthetic_vertex = rectangle_vertices[3];
  for (uint32_t c = 0; c < 4; ++c) {
    synthetic_vertex.guest_position[c] = rectangle_vertices[1].guest_position[c] -
                                         rectangle_vertices[0].guest_position[c] +
                                         rectangle_vertices[2].guest_position[c];
    synthetic_vertex.position[c] = rectangle_vertices[1].position[c] -
                                   rectangle_vertices[0].position[c] +
                                   rectangle_vertices[2].position[c];
  }
  synthetic_vertex.has_color = rectangle_vertices[0].has_color && rectangle_vertices[1].has_color &&
                               rectangle_vertices[2].has_color;
  if (synthetic_vertex.has_color) {
    for (uint32_t c = 0; c < 4; ++c) {
      synthetic_vertex.color[c] = rectangle_vertices[1].color[c] - rectangle_vertices[0].color[c] +
                                  rectangle_vertices[2].color[c];
    }
  }
  return rectangle_vertices;
}

bool TransformGuestPositionForHost(const float* guest_position, reg::PA_CL_VTE_CNTL pa_cl_vte_cntl,
                                   const draw_util::ViewportInfo& viewport_info,
                                   float* host_position_out) {
  float position_w = guest_position[3];
  if (!std::isfinite(position_w) || std::abs(position_w) < 1.0e-6f) {
    return false;
  }
  if (!pa_cl_vte_cntl.vtx_w0_fmt) {
    position_w = 1.0f / position_w;
  }

  float position_x = guest_position[0];
  float position_y = guest_position[1];
  float position_z = guest_position[2];
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    position_x *= position_w;
    position_y *= position_w;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    position_z *= position_w;
  }

  host_position_out[0] =
      position_x * viewport_info.ndc_scale[0] + viewport_info.ndc_offset[0] * position_w;
  host_position_out[1] =
      position_y * viewport_info.ndc_scale[1] + viewport_info.ndc_offset[1] * position_w;
  host_position_out[2] =
      position_z * viewport_info.ndc_scale[2] + viewport_info.ndc_offset[2] * position_w;
  host_position_out[3] = position_w;
  return std::isfinite(host_position_out[0]) && std::isfinite(host_position_out[1]) &&
         std::isfinite(host_position_out[2]) && std::isfinite(host_position_out[3]) &&
         std::abs(host_position_out[3]) >= 1.0e-6f;
}

}  // namespace

MetalCommandProcessor::MetalCommandProcessor(MetalGraphicsSystem* graphics_system,
                                             system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

MetalCommandProcessor::~MetalCommandProcessor() = default;

void MetalCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  // Phase 0.5: close the frame at swap. The base TextureCache has no
  // EndFrame()/EndSubmission() (Vulkan-only); the next frame's first draw
  // re-opens via BeginSubmission()/BeginFrame().
  frame_open_ = false;
  if (frontbuffer_width && frontbuffer_height) {
    fallback_output_width_ = frontbuffer_width;
    fallback_output_height_ = frontbuffer_height;
  }
  last_swap_frontbuffer_ptr_ = frontbuffer_ptr;
  last_swap_frontbuffer_width_ = frontbuffer_width;
  last_swap_frontbuffer_height_ = frontbuffer_height;
  if (register_file_) {
    last_swap_fetch_ = register_file_->GetTextureFetch(0);
    last_swap_fetch_valid_ = last_swap_fetch_.type == xenos::FetchConstantType::kTexture;
  }
  static std::atomic<uint32_t> metal_swap_logs{0};
  uint32_t metal_swap_index = metal_swap_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (metal_swap_index <= 16 || (metal_swap_index & 0x3F) == 0) {
    std::fprintf(stderr, "[metal] IssueSwap#%u fb_pa=0x%08x %ux%u\n", metal_swap_index,
                 frontbuffer_ptr, frontbuffer_width, frontbuffer_height);
    std::fflush(stderr);
  }
  if (metal_swap_index <= 32 || (metal_swap_index & 0x3F) == 0) {
    std::fprintf(stderr,
                 "[metal] draw route summary#%u total=%u color_depth=%u "
                 "shader_color_candidates=%u register_color_candidates=%u "
                 "register_color_unrouted=%u owned_rt_draws=%u owned_rt_targets=%u\n",
                 metal_swap_index, draw_calls_this_swap_, color_depth_draws_this_swap_,
                 color_target_candidate_draws_this_swap_, register_color_candidate_draws_this_swap_,
                 register_color_unrouted_draws_this_swap_, owned_rt_routed_draws_this_swap_,
                 owned_rt_routed_targets_this_swap_);
    std::fflush(stderr);
  }
  draw_calls_this_swap_ = 0;
  color_depth_draws_this_swap_ = 0;
  color_target_candidate_draws_this_swap_ = 0;
  register_color_candidate_draws_this_swap_ = 0;
  register_color_unrouted_draws_this_swap_ = 0;
  owned_rt_routed_draws_this_swap_ = 0;
  owned_rt_routed_targets_this_swap_ = 0;
  if (MetalHeuristicPresentationEnabled() || MetalPipelineProbesEnabled()) {
    RefreshPipelineProbeBacking(fallback_output_width_, fallback_output_height_);
  }
  // Phase 1: read back the host render target on every swap (default path) so the
  // host-RT readback smoke signal proves real geometry reached the texture without
  // any debug env var. RefreshHostRenderTargetBacking self-guards on null/size.
  RefreshHostRenderTargetBacking(fallback_output_width_, fallback_output_height_);
  if (graphics_system_) {
    if (auto* presenter = graphics_system_->presenter()) {
      if (memory_ && frontbuffer_width && frontbuffer_height) {
        if (auto* metal_presenter = dynamic_cast<ui::metal::MetalPresenter*>(presenter)) {
          std::vector<uint8_t> decoded_bgra;
          uint32_t decoded_width = 0;
          uint32_t decoded_height = 0;
          if (DecodeSwapTextureToBgra(frontbuffer_ptr, frontbuffer_width, frontbuffer_height,
                                      decoded_bgra, decoded_width, decoded_height)) {
            BgraFrameStats decoded_stats = GetBgraFrameStats(decoded_bgra);
            uint32_t decoded_visible = decoded_stats.visible_pixels;
            if (MetalHostRenderTargetDebugPresentEnabled() &&
                BgraHasNonZeroRgb(latest_host_render_target_bgra_)) {
              decoded_bgra = latest_host_render_target_bgra_;
              decoded_width = latest_host_render_target_width_;
              decoded_height = latest_host_render_target_height_;
              decoded_stats = GetBgraFrameStats(decoded_bgra);
              decoded_visible = decoded_stats.visible_pixels;
              static std::atomic<uint32_t> host_rt_present_logs{0};
              uint32_t host_rt_present_index =
                  host_rt_present_logs.fetch_add(1, std::memory_order_relaxed) + 1;
              if (host_rt_present_index <= 16 || (host_rt_present_index & 0x3F) == 0) {
                std::fprintf(stderr,
                             "[metal] presenting host render target debug#%u visible=%u "
                             "size=%ux%u\n",
                             host_rt_present_index, decoded_visible, decoded_width, decoded_height);
                std::fflush(stderr);
              }
            }
            if (MetalHeuristicPresentationEnabled()) {
              auto prefer_candidate = [&](const std::vector<uint8_t>& candidate_bgra,
                                          uint32_t candidate_width, uint32_t candidate_height,
                                          const char* label) {
                if (candidate_width && candidate_height &&
                    candidate_bgra.size() >= size_t(candidate_width) * candidate_height * 4) {
                  BgraFrameStats candidate_stats = GetBgraFrameStats(candidate_bgra);
                  uint32_t candidate_pixels = candidate_width * candidate_height;
                  if (std::strcmp(label, "fullscreen postprocess") == 0 &&
                      (candidate_stats.visible_pixels * 3 < candidate_pixels ||
                       (candidate_stats.visible_pixels == candidate_pixels &&
                        BgraSpatialSampleColorDistance(candidate_bgra, candidate_width,
                                                       candidate_height) <= 8))) {
                    static std::atomic<uint32_t> flat_fullscreen_skip_logs{0};
                    uint32_t flat_fullscreen_skip_index =
                        flat_fullscreen_skip_logs.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (flat_fullscreen_skip_index <= 16 ||
                        (flat_fullscreen_skip_index & 0x3F) == 0) {
                      std::fprintf(stderr,
                                   "[metal] skipped sparse/flat fullscreen candidate#%u "
                                   "visible=%u range=%u\n",
                                   flat_fullscreen_skip_index, candidate_stats.visible_pixels,
                                   BgraRgbRange(candidate_stats));
                      std::fflush(stderr);
                    }
                    return false;
                  }
                  if (candidate_stats.visible_pixels * 4 >= candidate_pixels * 3 &&
                      BgraRgbRange(candidate_stats) <= 8 && BgraRgbRange(decoded_stats) >= 32 &&
                      decoded_stats.visible_pixels) {
                    static std::atomic<uint32_t> flat_candidate_skip_logs{0};
                    uint32_t flat_candidate_skip_index =
                        flat_candidate_skip_logs.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (flat_candidate_skip_index <= 16 ||
                        (flat_candidate_skip_index & 0x3F) == 0) {
                      std::fprintf(stderr,
                                   "[metal] skipped flat %s candidate#%u visible=%u "
                                   "range=%u current_visible=%u current_range=%u\n",
                                   label, flat_candidate_skip_index, candidate_stats.visible_pixels,
                                   BgraRgbRange(candidate_stats), decoded_stats.visible_pixels,
                                   BgraRgbRange(decoded_stats));
                      std::fflush(stderr);
                    }
                    return false;
                  }
                  uint32_t current_pixel_count = decoded_width * decoded_height;
                  bool current_swap_sparse =
                      current_pixel_count && decoded_stats.visible_pixels < current_pixel_count / 4;
                  if (std::strcmp(label, "retained texture") == 0 && decoded_stats.visible_pixels &&
                      current_swap_sparse) {
                    uint32_t current_spatial = BgraVisibleSpatialSampleColorDistance(
                        decoded_bgra, decoded_width, decoded_height);
                    uint32_t candidate_spatial = BgraVisibleSpatialSampleColorDistance(
                        candidate_bgra, candidate_width, candidate_height);
                    uint64_t current_score = CandidateFrameScore(decoded_stats, current_spatial);
                    uint64_t candidate_score =
                        CandidateFrameScore(candidate_stats, candidate_spatial);
                    if (candidate_score > current_score + 32768) {
                      decoded_bgra = candidate_bgra;
                      decoded_width = candidate_width;
                      decoded_height = candidate_height;
                      decoded_stats = candidate_stats;
                      decoded_visible = decoded_stats.visible_pixels;
                      static std::atomic<uint32_t> preferred_texture_logs{0};
                      uint32_t preferred_texture_index =
                          preferred_texture_logs.fetch_add(1, std::memory_order_relaxed) + 1;
                      if (preferred_texture_index <= 16 || (preferred_texture_index & 0x3F) == 0) {
                        std::fprintf(stderr,
                                     "[metal] preferred retained texture frame#%u "
                                     "visible=%u range=%u score=%llu over current_score=%llu\n",
                                     preferred_texture_index, decoded_visible,
                                     BgraRgbRange(decoded_stats),
                                     static_cast<unsigned long long>(candidate_score),
                                     static_cast<unsigned long long>(current_score));
                        std::fflush(stderr);
                      }
                      return true;
                    }
                  }
                  bool retained_renderer_candidate =
                      std::strcmp(label, "fullscreen postprocess") == 0;
                  if (retained_renderer_candidate && decoded_stats.visible_pixels) {
                    uint32_t current_spatial = BgraVisibleSpatialSampleColorDistance(
                        decoded_bgra, decoded_width, decoded_height);
                    uint32_t candidate_spatial = BgraVisibleSpatialSampleColorDistance(
                        candidate_bgra, candidate_width, candidate_height);
                    uint64_t current_score = CandidateFrameScore(decoded_stats, current_spatial);
                    uint64_t candidate_score =
                        CandidateFrameScore(candidate_stats, candidate_spatial);
                    bool candidate_not_worse =
                        candidate_score >= current_score &&
                        candidate_stats.visible_pixels + candidate_stats.visible_pixels / 20 >=
                            decoded_stats.visible_pixels &&
                        !IsDominantFlatVisibleFrame(candidate_bgra, candidate_width,
                                                    candidate_height,
                                                    candidate_stats.visible_pixels);
                    if (candidate_not_worse) {
                      decoded_bgra = candidate_bgra;
                      decoded_width = candidate_width;
                      decoded_height = candidate_height;
                      decoded_stats = candidate_stats;
                      decoded_visible = decoded_stats.visible_pixels;
                      static std::atomic<uint32_t> preferred_retained_renderer_logs{0};
                      uint32_t preferred_retained_renderer_index =
                          preferred_retained_renderer_logs.fetch_add(1, std::memory_order_relaxed) +
                          1;
                      if (preferred_retained_renderer_index <= 16 ||
                          (preferred_retained_renderer_index & 0x3F) == 0) {
                        std::fprintf(stderr,
                                     "[metal] preferred %s retained renderer frame#%u "
                                     "visible=%u range=%u score=%llu over current_score=%llu\n",
                                     label, preferred_retained_renderer_index, decoded_visible,
                                     BgraRgbRange(decoded_stats),
                                     static_cast<unsigned long long>(candidate_score),
                                     static_cast<unsigned long long>(current_score));
                        std::fflush(stderr);
                      }
                      return true;
                    }
                  }
                  bool spatial_texture_candidate = std::strcmp(label, "retained texture") == 0 ||
                                                   std::strcmp(label, "resolved color") == 0 ||
                                                   std::strcmp(label, "pipeline probe") == 0;
                  if (spatial_texture_candidate && decoded_stats.visible_pixels &&
                      (candidate_stats.visible_pixels * 4 >= candidate_pixels ||
                       IsUsefulSparseVisibleFrame(candidate_bgra, candidate_width, candidate_height,
                                                  candidate_stats)) &&
                      !IsDominantFlatVisibleFrame(candidate_bgra, candidate_width, candidate_height,
                                                  candidate_stats.visible_pixels)) {
                    uint32_t current_spatial = BgraVisibleSpatialSampleColorDistance(
                        decoded_bgra, decoded_width, decoded_height);
                    uint32_t candidate_spatial = BgraVisibleSpatialSampleColorDistance(
                        candidate_bgra, candidate_width, candidate_height);
                    uint64_t current_score = CandidateFrameScore(decoded_stats, current_spatial);
                    uint64_t candidate_score =
                        CandidateFrameScore(candidate_stats, candidate_spatial);
                    if (candidate_score > current_score + 32768) {
                      decoded_bgra = candidate_bgra;
                      decoded_width = candidate_width;
                      decoded_height = candidate_height;
                      decoded_stats = candidate_stats;
                      decoded_visible = decoded_stats.visible_pixels;
                      static std::atomic<uint32_t> preferred_spatial_logs{0};
                      uint32_t preferred_spatial_index =
                          preferred_spatial_logs.fetch_add(1, std::memory_order_relaxed) + 1;
                      if (preferred_spatial_index <= 16 || (preferred_spatial_index & 0x3F) == 0) {
                        std::fprintf(
                            stderr,
                            "[metal] preferred spatial %s frame#%u visible=%u "
                            "spatial=%u score=%llu over current_spatial=%u "
                            "current_score=%llu\n",
                            label, preferred_spatial_index, decoded_visible, candidate_spatial,
                            static_cast<unsigned long long>(candidate_score), current_spatial,
                            static_cast<unsigned long long>(current_score));
                        std::fflush(stderr);
                      }
                      return true;
                    }
                  }
                }
                if (!ShouldPreferCandidateFrame(decoded_visible, decoded_width, decoded_height,
                                                candidate_bgra, candidate_width,
                                                candidate_height)) {
                  return false;
                }
                decoded_bgra = candidate_bgra;
                decoded_width = candidate_width;
                decoded_height = candidate_height;
                decoded_stats = GetBgraFrameStats(decoded_bgra);
                decoded_visible = decoded_stats.visible_pixels;
                static std::atomic<uint32_t> preferred_frame_logs{0};
                uint32_t preferred_frame_index =
                    preferred_frame_logs.fetch_add(1, std::memory_order_relaxed) + 1;
                if (preferred_frame_index <= 16 || (preferred_frame_index & 0x3F) == 0) {
                  std::fprintf(stderr,
                               "[metal] preferred %s frame#%u over partial swap "
                               "visible=%u size=%ux%u\n",
                               label, preferred_frame_index, decoded_visible, decoded_width,
                               decoded_height);
                  std::fflush(stderr);
                }
                return true;
              };
              if (decoded_visible) {
                prefer_candidate(latest_draw_event_frame_bgra_, latest_draw_event_frame_width_,
                                 latest_draw_event_frame_height_, "retained draw");
                prefer_candidate(latest_fullscreen_postprocess_bgra_,
                                 latest_fullscreen_postprocess_width_,
                                 latest_fullscreen_postprocess_height_, "fullscreen postprocess");
                prefer_candidate(latest_texture_candidate_bgra_, latest_texture_candidate_width_,
                                 latest_texture_candidate_height_, "retained texture");
                prefer_candidate(resolved_color_bgra_, resolved_color_width_,
                                 resolved_color_height_, "resolved color");
                prefer_candidate(latest_pipeline_probe_bgra_, latest_pipeline_probe_width_,
                                 latest_pipeline_probe_height_, "pipeline probe");
              }
              if (MetalHeuristicPresentationEnabled() && !BgraHasNonZeroRgb(decoded_bgra) &&
                  draw_count_) {
                if (draw_renderer_ &&
                    (!pending_draw_events_.empty() || !pending_host_vertices_.empty())) {
                  std::vector<uint8_t> diagnostic_bgra;
                  MetalHostTexture host_texture = {};
                  MetalHostTexture* host_texture_ptr = nullptr;
                  if (kEnableHostTextureFallback && MetalHostPixelDiagnosticsEnabled() &&
                      !pending_host_texture_rgba_.empty() && pending_host_texture_width_ &&
                      pending_host_texture_height_) {
                    host_texture.rgba = pending_host_texture_rgba_.data();
                    host_texture.width = pending_host_texture_width_;
                    host_texture.height = pending_host_texture_height_;
                    host_texture.bytes_per_row = size_t(pending_host_texture_width_) * 4;
                    host_texture_ptr = &host_texture;
                  }
                  if (draw_renderer_->RenderDrawEventFrame(
                          decoded_width, decoded_height, draw_count_, metal_swap_index,
                          pending_draw_events_, pending_host_vertices_, diagnostic_bgra,
                          host_texture_ptr)) {
                    decoded_bgra = std::move(diagnostic_bgra);
                    latest_draw_event_frame_bgra_ = decoded_bgra;
                    latest_draw_event_frame_width_ = decoded_width;
                    latest_draw_event_frame_height_ = decoded_height;
                    ++diagnostic_frame_count_;
                    if (diagnostic_frame_count_ <= 8 || (diagnostic_frame_count_ & 0x3F) == 0) {
                      std::fprintf(stderr,
                                   "[metal] draw event frame#%u presented for black swap "
                                   "(draw_count=%u events=%zu host_vertices=%zu swap=%u)\n",
                                   diagnostic_frame_count_, draw_count_,
                                   pending_draw_events_.size(), pending_host_vertices_.size(),
                                   metal_swap_index);
                      std::fflush(stderr);
                    }
                  } else if (metal_swap_index <= 8 || (metal_swap_index & 0x3F) == 0) {
                    std::fprintf(stderr,
                                 "[metal] draw event frame render failed "
                                 "(draw_count=%u events=%zu host_vertices=%zu swap=%u)\n",
                                 draw_count_, pending_draw_events_.size(),
                                 pending_host_vertices_.size(), metal_swap_index);
                    std::fflush(stderr);
                  }
                } else if (BgraHasNonZeroRgb(latest_draw_event_frame_bgra_)) {
                  decoded_bgra = latest_draw_event_frame_bgra_;
                  decoded_width = latest_draw_event_frame_width_;
                  decoded_height = latest_draw_event_frame_height_;
                  static std::atomic<uint32_t> retained_draw_present_logs{0};
                  uint32_t retained_draw_present_index =
                      retained_draw_present_logs.fetch_add(1, std::memory_order_relaxed) + 1;
                  if (retained_draw_present_index <= 8 ||
                      (retained_draw_present_index & 0x3F) == 0) {
                    std::fprintf(stderr,
                                 "[metal] presenting retained draw event frame#%u for black swap "
                                 "size=%ux%u\n",
                                 retained_draw_present_index, decoded_width, decoded_height);
                    std::fflush(stderr);
                  }
                } else if (BgraHasNonZeroRgb(latest_fullscreen_postprocess_bgra_)) {
                  decoded_bgra = latest_fullscreen_postprocess_bgra_;
                  decoded_width = latest_fullscreen_postprocess_width_;
                  decoded_height = latest_fullscreen_postprocess_height_;
                  static std::atomic<uint32_t> fullscreen_present_logs{0};
                  uint32_t fullscreen_present_index =
                      fullscreen_present_logs.fetch_add(1, std::memory_order_relaxed) + 1;
                  if (fullscreen_present_index <= 8 || (fullscreen_present_index & 0x3F) == 0) {
                    std::fprintf(stderr,
                                 "[metal] presenting fullscreen postprocess#%u for black swap "
                                 "size=%ux%u\n",
                                 fullscreen_present_index, decoded_width, decoded_height);
                    std::fflush(stderr);
                  }
                } else if (BgraHasNonZeroRgb(pending_texture_resolve_bgra_)) {
                  decoded_bgra = pending_texture_resolve_bgra_;
                  decoded_width = pending_texture_resolve_width_;
                  decoded_height = pending_texture_resolve_height_;
                  static std::atomic<uint32_t> pending_present_logs{0};
                  uint32_t pending_present_index =
                      pending_present_logs.fetch_add(1, std::memory_order_relaxed) + 1;
                  if (pending_present_index <= 8 || (pending_present_index & 0x3F) == 0) {
                    std::fprintf(stderr,
                                 "[metal] presenting pending texture resolve#%u for black swap "
                                 "size=%ux%u\n",
                                 pending_present_index, decoded_width, decoded_height);
                    std::fflush(stderr);
                  }
                } else if (BgraHasNonZeroRgb(latest_texture_candidate_bgra_)) {
                  decoded_bgra = latest_texture_candidate_bgra_;
                  decoded_width = latest_texture_candidate_width_;
                  decoded_height = latest_texture_candidate_height_;
                  static std::atomic<uint32_t> texture_candidate_present_logs{0};
                  uint32_t texture_candidate_present_index =
                      texture_candidate_present_logs.fetch_add(1, std::memory_order_relaxed) + 1;
                  if (texture_candidate_present_index <= 8 ||
                      (texture_candidate_present_index & 0x3F) == 0) {
                    std::fprintf(stderr,
                                 "[metal] presenting retained texture candidate#%u for black swap "
                                 "size=%ux%u\n",
                                 texture_candidate_present_index, decoded_width, decoded_height);
                    std::fflush(stderr);
                  }
                } else if (BgraHasNonZeroRgb(resolved_color_bgra_)) {
                  decoded_bgra = resolved_color_bgra_;
                  decoded_width = resolved_color_width_;
                  decoded_height = resolved_color_height_;
                  static std::atomic<uint32_t> resolved_present_logs{0};
                  uint32_t resolved_present_index =
                      resolved_present_logs.fetch_add(1, std::memory_order_relaxed) + 1;
                  if (resolved_present_index <= 8 || (resolved_present_index & 0x3F) == 0) {
                    std::fprintf(stderr,
                                 "[metal] presenting resolved color backing#%u for black swap "
                                 "size=%ux%u\n",
                                 resolved_present_index, decoded_width, decoded_height);
                    std::fflush(stderr);
                  }
                } else if (!latest_pipeline_probe_bgra_.empty()) {
                  decoded_bgra = latest_pipeline_probe_bgra_;
                  decoded_width = latest_pipeline_probe_width_;
                  decoded_height = latest_pipeline_probe_height_;
                  static std::atomic<uint32_t> probe_present_logs{0};
                  uint32_t probe_present_index =
                      probe_present_logs.fetch_add(1, std::memory_order_relaxed) + 1;
                  if (probe_present_index <= 8 || (probe_present_index & 0x3F) == 0) {
                    std::fprintf(stderr, "[metal] presenting latest pipeline probe fallback#%u\n",
                                 probe_present_index);
                    std::fflush(stderr);
                  }
                } else if (!latest_pipeline_probe_bgra_.empty()) {
                  decoded_bgra = latest_pipeline_probe_bgra_;
                  decoded_width = latest_pipeline_probe_width_;
                  decoded_height = latest_pipeline_probe_height_;
                }
              }
            }
            static std::atomic<uint32_t> presenter_source_logs{0};
            uint32_t presenter_source_index =
                presenter_source_logs.fetch_add(1, std::memory_order_relaxed) + 1;
            if (presenter_source_index <= 16 || (presenter_source_index & 0x3F) == 0) {
              auto sample_bgra = [&](uint32_t x, uint32_t y) -> const uint8_t* {
                static const uint8_t zero[4] = {};
                if (!decoded_width || !decoded_height || decoded_bgra.empty()) {
                  return zero;
                }
                x = std::min(x, decoded_width - 1);
                y = std::min(y, decoded_height - 1);
                size_t offset = (size_t(y) * decoded_width + x) * 4;
                return offset + 3 < decoded_bgra.size() ? decoded_bgra.data() + offset : zero;
              };
              BgraFrameStats presenter_stats = GetBgraFrameStats(decoded_bgra);
              const uint8_t* p00 = sample_bgra(0, 0);
              const uint8_t* p10 = sample_bgra(decoded_width / 4, decoded_height / 4);
              const uint8_t* p50 = sample_bgra(decoded_width / 2, decoded_height / 2);
              const uint8_t* p90 = sample_bgra(decoded_width * 3 / 4, decoded_height * 3 / 4);
              std::fprintf(stderr,
                           "[metal] presenter source#%u swap=%u size=%ux%u visible=%u "
                           "range=%u p00=%02x %02x %02x %02x "
                           "p25=%02x %02x %02x %02x p50=%02x %02x %02x %02x "
                           "p75=%02x %02x %02x %02x\n",
                           presenter_source_index, metal_swap_index, decoded_width, decoded_height,
                           presenter_stats.visible_pixels, BgraRgbRange(presenter_stats), p00[0],
                           p00[1], p00[2], p00[3], p10[0], p10[1], p10[2], p10[3], p50[0], p50[1],
                           p50[2], p50[3], p90[0], p90[1], p90[2], p90[3]);
              std::fflush(stderr);
            }
            if (presenter_source_index <= 8 || (presenter_source_index & 0x3F) == 0) {
              DumpBgraFrameAsPpm("presenter", presenter_source_index, decoded_bgra, decoded_width,
                                 decoded_height);
            }
            metal_presenter->UpdateGuestFrontbuffer(decoded_width, decoded_height,
                                                    decoded_bgra.data(), size_t(decoded_width) * 4);
          } else {
            const uint8_t* frontbuffer =
                memory_->TranslatePhysical<const uint8_t*>(frontbuffer_ptr);
            metal_presenter->UpdateGuestFrontbuffer(frontbuffer_width, frontbuffer_height,
                                                    frontbuffer, size_t(frontbuffer_width) * 4);
          }
        }
      }
      bool refreshed = presenter->RefreshGuestOutput(
          frontbuffer_width, frontbuffer_height, frontbuffer_width, frontbuffer_height,
          [](ui::Presenter::GuestOutputRefreshContext& context) {
            context.SetIs8bpc(true);
            return true;
          });
      if (!refreshed && metal_swap_index <= 16) {
        std::fprintf(stderr, "[metal] IssueSwap refresh rejected for %ux%u\n", frontbuffer_width,
                     frontbuffer_height);
        std::fflush(stderr);
      }
    }
  }
  pending_draw_events_.clear();
  pending_host_vertices_.clear();
  pending_texture_resolve_bgra_.clear();
  pending_texture_resolve_width_ = 0;
  pending_texture_resolve_height_ = 0;
  pending_host_texture_rgba_.clear();
  pending_host_texture_width_ = 0;
  pending_host_texture_height_ = 0;
  latest_host_pixel_frame_bgra_.clear();
  latest_host_pixel_frame_width_ = 0;
  latest_host_pixel_frame_height_ = 0;
  latest_host_pixel_frame_draw_count_ = 0;
  latest_host_pixel_frame_from_fallback_ = false;
  ResetPipelineProbeContext(host_pixel_probe_context_);
  pipeline_probe_draws_this_swap_ = 0;
  pipeline_probe_skipped_this_swap_ = 0;
  host_pixel_draws_this_swap_ = 0;
  host_fallback_pixel_draws_this_swap_ = 0;
  host_pixel_skipped_vertices_this_swap_ = 0;
  host_pixel_shader_draws_this_swap_.clear();
  if (primitive_processor_) {
    static_cast<MetalPrimitiveProcessor*>(primitive_processor_.get())->EndFrame();
  }
  LogIncompleteOnce("swap");
}

void MetalCommandProcessor::TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) {
  const bool watched_framebuffer =
      RangesOverlap(base_ptr, length, kWatchedFramebufferBase, kWatchedFramebufferLength);
  const bool watched_resolve =
      RangesOverlap(base_ptr, length, kWatchedResolveBase, kWatchedResolveLength);
  const bool watched_swap = RangesOverlap(base_ptr, length, kWatchedSwapBase, kWatchedSwapLength);
  if (watched_framebuffer || watched_resolve || watched_swap) {
    static std::atomic<uint32_t> watched_trace_write_logs{0};
    uint32_t trace_write_index =
        watched_trace_write_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (trace_write_index <= 32 || (trace_write_index & 0xFF) == 0) {
      std::fprintf(stderr,
                   "[metal] watched trace write#%u range=0x%08x+0x%x "
                   "framebuffer=%u resolve=%u swap=%u\n",
                   trace_write_index, base_ptr, length, watched_framebuffer ? 1u : 0u,
                   watched_resolve ? 1u : 0u, watched_swap ? 1u : 0u);
      std::fflush(stderr);
    }
  }
  if (shared_memory_) {
    shared_memory_->MemoryInvalidationCallback(base_ptr, length, true);
  }
  InvalidateRetainedResolvedFrames(base_ptr, length);
}

void MetalCommandProcessor::RestoreEdramSnapshot(const void* snapshot) {
  (void)snapshot;
}

bool MetalCommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    return false;
  }
  fallback_output_width_ =
      uint32_t(std::clamp(REXCVAR_GET(video_mode_width), int32_t(1), int32_t(8192)));
  fallback_output_height_ =
      uint32_t(std::clamp(REXCVAR_GET(video_mode_height), int32_t(1), int32_t(8192)));
  SpirvShaderTranslator::Features metal_shader_features(true);
  metal_shader_features.image_view_format_swizzle = false;
  shader_translator_ =
      std::make_unique<SpirvShaderTranslator>(metal_shader_features, false, false, false);
  if (auto* provider = dynamic_cast<ui::metal::MetalProvider*>(graphics_system_->provider())) {
    metal_device_ = provider->metal_device();
    if (memory_) {
      shared_memory_ = std::make_unique<MetalSharedMemory>(*memory_, trace_writer_);
      if (!shared_memory_->Initialize(metal_device_)) {
        REXLOG_WARN("Metal shared memory initialization failed; texture cache disabled");
        shared_memory_.reset();
      }
      if (shared_memory_) {
        texture_cache_ =
            MetalTextureCache::Create(*register_file_, *shared_memory_, metal_device_, 1, 1);
        if (!texture_cache_) {
          REXLOG_WARN("Metal texture cache initialization failed");
        }
        primitive_processor_ = CreateMetalPrimitiveProcessor(*register_file_, *memory_,
                                                             trace_writer_, *shared_memory_);
        if (!primitive_processor_) {
          REXLOG_WARN("Metal primitive processor initialization failed");
        }
      }
    }
    draw_renderer_ = MetalDrawRenderer::Create(metal_device_);
    std::string probe_context_error;
    pipeline_probe_context_ = CreatePipelineProbeContext(metal_device_, &probe_context_error);
    if (!pipeline_probe_context_) {
      REXLOG_WARN("Metal persistent pipeline probe unavailable: {}", probe_context_error);
    }
    std::string host_pixel_context_error;
    host_pixel_probe_context_ =
        CreatePipelineProbeContext(metal_device_, &host_pixel_context_error);
    if (!host_pixel_probe_context_) {
      REXLOG_WARN("Metal host-pixel probe context unavailable: {}", host_pixel_context_error);
    }
    std::string host_rt_context_error;
    host_render_target_context_ =
        CreateHostRenderTargetContext(metal_device_, &host_rt_context_error);
    if (!host_render_target_context_) {
      REXLOG_WARN("Metal host render-target debug context unavailable: {}", host_rt_context_error);
    }
  }
  if (!draw_renderer_) {
    REXLOG_WARN("Metal diagnostic draw renderer unavailable; swaps will only present guest memory");
  }
  REXLOG_WARN("Native Metal command processor active; real title rendering is experimental");
  return true;
}

void MetalCommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  // Phase 0.5: mirror VulkanCommandProcessor::ClearCaches for the shared-memory /
  // texture-cache lifecycle, scoped to base-class methods present in Metal.
  if (texture_cache_) {
    texture_cache_->ClearCache();
  }
  if (shared_memory_) {
    shared_memory_->ClearCache();
  }
}

void MetalCommandProcessor::ShutdownContext() {
  REXLOG_INFO("Metal shader cache shutdown: {} shaders cached, {} translated, {} failed",
              shaders_.size(), translated_shader_count_, failed_shader_translation_count_);
  for (auto& pipeline_entry : render_pipeline_states_) {
    ReleaseRenderPipelineState(pipeline_entry.second);
  }
  render_pipeline_states_.clear();
  for (auto& pipeline_entry : fullscreen_pixel_pipeline_states_) {
    ReleaseRenderPipelineState(pipeline_entry.second);
  }
  fullscreen_pixel_pipeline_states_.clear();
  for (auto& pipeline_entry : host_pixel_pipeline_states_) {
    ReleaseRenderPipelineState(pipeline_entry.second);
  }
  host_pixel_pipeline_states_.clear();
  for (auto& pipeline_entry : solid_color_pipeline_states_) {
    ReleaseRenderPipelineState(pipeline_entry.second);
  }
  solid_color_pipeline_states_.clear();
  for (auto& pipeline_entry : memexport_pipeline_states_) {
    ReleaseRenderPipelineState(pipeline_entry.second);
  }
  memexport_pipeline_states_.clear();
  ReleaseMslLibrary(fullscreen_vertex_library_);
  fullscreen_vertex_library_ = nullptr;
  ReleaseMslLibrary(host_pixel_vertex_library_);
  host_pixel_vertex_library_ = nullptr;
  ReleaseRenderPipelineState(host_fallback_pixel_pipeline_state_);
  host_fallback_pixel_pipeline_state_ = nullptr;
  ReleaseMslLibrary(host_fallback_pixel_fragment_library_);
  host_fallback_pixel_fragment_library_ = nullptr;
  ReleaseMslLibrary(dummy_fragment_library_);
  dummy_fragment_library_ = nullptr;
  ReleaseMslLibrary(solid_fragment_library_);
  solid_fragment_library_ = nullptr;
  ReleasePipelineProbeContext(pipeline_probe_context_);
  pipeline_probe_context_ = nullptr;
  ReleasePipelineProbeContext(host_pixel_probe_context_);
  host_pixel_probe_context_ = nullptr;
  for (auto& rt_entry : host_render_targets_) {
    ReleasePipelineProbeContext(rt_entry.second.context);
    rt_entry.second.context = nullptr;
  }
  host_render_targets_.clear();
  ReleasePipelineProbeContext(host_render_target_context_);
  host_render_target_context_ = nullptr;
  probed_pipeline_keys_.clear();
  shaders_.clear();
  texture_cache_.reset();
  primitive_processor_.reset();
  if (shared_memory_) {
    shared_memory_->Shutdown();
  }
  shared_memory_.reset();
  draw_renderer_.reset();
  shader_translator_.reset();
}

void MetalCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);
  if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5 && texture_cache_) {
    texture_cache_->TextureFetchConstantWritten((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) /
                                                6);
  }
}

void MetalCommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                                  uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  uint32_t end_index = start_index + num_registers - 1;
  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    if (texture_cache_) {
      uint32_t first_fetch_dword = start_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
      uint32_t last_fetch_dword = end_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
      texture_cache_->TextureFetchConstantsWritten(first_fetch_dword / 6, last_fetch_dword / 6);
    }
    return;
  }
  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
}

Shader* MetalCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                          const uint32_t* host_address, uint32_t dword_count) {
  (void)guest_address;
  if (!host_address || !dword_count) {
    return nullptr;
  }
  uint64_t data_hash = XXH3_64bits(host_address, dword_count * sizeof(uint32_t));
  return LoadShaderFromCache(shader_type, host_address, dword_count, data_hash);
}

bool MetalCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
  (void)major_mode_explicit;

  static std::atomic<uint32_t> metal_draw_logs{0};
  uint32_t metal_draw_index = metal_draw_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  ++draw_calls_this_swap_;
  if (metal_draw_index <= 16 || (metal_draw_index & 0xFF) == 0) {
    std::fprintf(stderr, "[metal] IssueDraw#%u prim=%u indices=%u vs=%p ps=%p\n", metal_draw_index,
                 uint32_t(prim_type), index_count, active_vertex_shader_, active_pixel_shader_);
    std::fflush(stderr);
  }

  auto* vertex_shader = static_cast<MetalShader*>(active_vertex_shader_);
  auto* pixel_shader = static_cast<MetalShader*>(active_pixel_shader_);
  // [TEMP upstream-diagnostic] Per-0a6d/2e372 fetch dump: prove whether these draws
  // are a MIX of type=3 (kVertex, the real menu) and type=2 (kTexture, junk that
  // Vulkan/D3D12 also drop). Logged before any routing so every draw is seen.
  if (vertex_shader && pixel_shader &&
      vertex_shader->ucode_data_hash() == UINT64_C(0x0a6d1dd7767fdf27) &&
      pixel_shader->ucode_data_hash() == UINT64_C(0x2e372ea28cc404b7)) {
    const Shader::ConstantRegisterMap& cmap = vertex_shader->constant_register_map();
    uint32_t used_slot = UINT32_MAX;
    for (uint32_t i = 0; i < rex::countof(cmap.vertex_fetch_bitmap); ++i) {
      uint32_t bits = cmap.vertex_fetch_bitmap[i];
      uint32_t b;
      if (rex::bit_scan_forward(bits, &b)) {
        used_slot = i * 32 + b;
        break;
      }
    }
    uint32_t binding_slot = vertex_shader->vertex_bindings().empty()
                                ? UINT32_MAX
                                : vertex_shader->vertex_bindings()[0].fetch_constant;
    xenos::xe_gpu_vertex_fetch_t f0 = register_file_->GetVertexFetch(0);
    if (used_slot != UINT32_MAX) {
      xenos::xe_gpu_vertex_fetch_t fs = register_file_->GetVertexFetch(used_slot);
      // The fetch is valid (type=3) live. Now answer: do these draws carry real
      // color, or are they black background fills? Vertex stride is 7 dwords;
      // position is dwords 0..2, the vfetch_mini color is at dword offset 3
      // (dwords 3..6). Read vertex 0's color across draws and count any non-black.
      const uint32_t* vtx =
          memory_ ? memory_->TranslatePhysical<const uint32_t*>(fs.address << 2) : nullptr;
      uint32_t cr = vtx ? vtx[3] : 0u, cg = vtx ? vtx[4] : 0u, cb = vtx ? vtx[5] : 0u,
               ca = vtx ? vtx[6] : 0u;
      static std::atomic<uint32_t> nonblack_0a6d{0};
      static std::atomic<uint32_t> total_0a6d{0};
      total_0a6d.fetch_add(1, std::memory_order_relaxed);
      if (cr || cg || cb) {
        uint32_t nb = nonblack_0a6d.fetch_add(1, std::memory_order_relaxed) + 1;
        if (nb <= 24) {
          std::fprintf(stderr,
                       "[metal][0A6D-COLOR] NONBLACK#%u draw=%u color=(%08x %08x %08x %08x) "
                       "addr=0x%08x indices=%u prim=%u\n",
                       nb, metal_draw_index, cr, cg, cb, ca, fs.address << 2, index_count,
                       uint32_t(prim_type));
          std::fflush(stderr);
        }
      }
    } else {
      std::fprintf(stderr,
                   "[metal][0A6D] draw=%u used_slot=NONE binding_slot=%u "
                   "slot0=(%08x %08x type=%u addr=0x%08x size=%u) indices=%u prim=%u\n",
                   metal_draw_index, binding_slot, f0.dword_0, f0.dword_1, uint32_t(f0.type),
                   f0.address << 2, f0.size, index_count, uint32_t(prim_type));
    }
    std::fflush(stderr);
  }
  if (register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) {
    return IssueCopy();
  }
  ++color_depth_draws_this_swap_;
  current_host_vertex_shader_type_ = Shader::HostVertexShaderType::kVertex;
  if (kEnableHostVertexShaderVariants) {
    current_host_vertex_shader_type_ =
        prim_type == xenos::PrimitiveType::kRectangleList
            ? Shader::HostVertexShaderType::kRectangleListAsTriangleStrip
            : (prim_type == xenos::PrimitiveType::kPointList
                   ? Shader::HostVertexShaderType::kPointListAsTriangleStrip
                   : Shader::HostVertexShaderType::kVertex);
  }
  PrimitiveProcessor::ProcessingResult primitive_processing_result = {};
  bool primitive_processing_ok =
      primitive_processor_ && primitive_processor_->Process(primitive_processing_result);
  if (primitive_processing_ok) {
    current_host_vertex_shader_type_ = primitive_processing_result.host_vertex_shader_type;
  }
  xenos::PrimitiveType host_prim_type =
      primitive_processing_ok ? primitive_processing_result.host_primitive_type : prim_type;
  uint32_t host_draw_vertex_count =
      primitive_processing_ok ? primitive_processing_result.host_draw_vertex_count : index_count;
  if (primitive_processing_ok || primitive_processor_) {
    uint64_t vs_hash_for_log = vertex_shader ? vertex_shader->ucode_data_hash() : uint64_t(0);
    uint64_t ps_hash_for_log = pixel_shader ? pixel_shader->ucode_data_hash() : uint64_t(0);
    bool interesting_primitive_log = metal_draw_index <= 32 || (metal_draw_index & 0xFF) == 0 ||
                                     ps_hash_for_log == UINT64_C(0xbdc93d3c5da8241f) ||
                                     ps_hash_for_log == UINT64_C(0x21243bcf873f69df) ||
                                     vs_hash_for_log == UINT64_C(0x1f207d90237c9c25);
    if (interesting_primitive_log) {
      std::fprintf(
          stderr,
          "[metal] primitive route#%u ok=%u draw=%u guest_prim=%u host_prim=%u "
          "guest_vertices=%u host_vertices=%u host_vs_type=%u index_type=%u "
          "host_index_format=%u reset=%u vs=%016llx ps=%016llx\n",
          metal_draw_index, primitive_processing_ok ? 1u : 0u, metal_draw_index,
          primitive_processing_ok ? uint32_t(primitive_processing_result.guest_primitive_type)
                                  : uint32_t(prim_type),
          primitive_processing_ok ? uint32_t(primitive_processing_result.host_primitive_type)
                                  : uint32_t(prim_type),
          primitive_processing_ok ? primitive_processing_result.guest_draw_vertex_count
                                  : index_count,
          primitive_processing_ok ? primitive_processing_result.host_draw_vertex_count
                                  : index_count,
          uint32_t(current_host_vertex_shader_type_),
          primitive_processing_ok ? uint32_t(primitive_processing_result.index_buffer_type) : 0u,
          primitive_processing_ok ? uint32_t(primitive_processing_result.host_index_format) : 0u,
          primitive_processing_ok && primitive_processing_result.host_primitive_reset_enabled ? 1u
                                                                                              : 0u,
          static_cast<unsigned long long>(vs_hash_for_log),
          static_cast<unsigned long long>(ps_hash_for_log));
      std::fflush(stderr);
    }
  }
  // Phase 0.5: open the frame on the first draw so the texture cache services
  // BeginSubmission/BeginFrame (needed for the resolve -> watch -> RequestRange
  // refetch loop across frames). Only base-class TextureCache methods are used.
  if (!frame_open_) {
    frame_open_ = true;
    if (texture_cache_) {
      texture_cache_->BeginSubmission(counter());
      texture_cache_->BeginFrame();
    }
    // Phase 1: start each frame's host render targets fresh. The per-RT
    // persistent contexts keep their MTLTexture alive across frames; reset
    // only the initialized flag so the first draw re-clears (MTLLoadActionClear)
    // and later draws accumulate (MTLLoadActionLoad) within the frame.
    for (auto& rt_entry : host_render_targets_) {
      if (rt_entry.second.context) {
        ResetPipelineProbeContext(rt_entry.second.context);
      }
    }
  }
  ++draw_count_;
  bool vertex_translation_ok = true;
  bool pixel_translation_ok = true;
  MetalShader::MetalTranslation* vertex_translation = nullptr;
  MetalShader::MetalTranslation* pixel_translation = nullptr;
  uint64_t vertex_modification = 0;
  uint64_t pixel_modification = 0;
  if (vertex_shader) {
    if (!vertex_shader->is_ucode_analyzed()) {
      vertex_shader->AnalyzeUcode(ucode_disasm_buffer_);
    }
    if (vertex_shader->writes_position()) {
      last_position_vertex_shader_ = vertex_shader;
      std::memcpy(last_position_registers_.data(), register_file_->values,
                  sizeof(register_file_->values));
      last_position_registers_valid_ = true;
    }
    if (!vertex_shader->writes_position()) {
      static std::atomic<uint32_t> positionless_vs_logs{0};
      uint32_t positionless_vs_index =
          positionless_vs_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (positionless_vs_index <= 16 || (positionless_vs_index & 0xFF) == 0) {
        std::fprintf(stderr,
                     "[metal] positionless VS#%u draw=%u vs=%016llx translated=%u "
                     "valid=%u eM=0x%02x msl_writes=%u\n",
                     positionless_vs_index, metal_draw_index,
                     static_cast<unsigned long long>(vertex_shader->ucode_data_hash()),
                     vertex_translation_ok ? 1u : 0u,
                     vertex_translation && vertex_translation->is_valid() ? 1u : 0u,
                     uint32_t(vertex_shader->memexport_eM_written()),
                     MslWritesSharedMemory(vertex_translation) ? 1u : 0u);
        std::fflush(stderr);
      }
    }
  }
  if (pixel_shader) {
    if (!pixel_shader->is_ucode_analyzed()) {
      pixel_shader->AnalyzeUcode(ucode_disasm_buffer_);
    }
  }
  uint32_t interpolator_mask = 0;
  uint32_t ps_param_gen_pos = UINT32_MAX;
  GetCurrentShaderModifications(vertex_shader, pixel_shader, vertex_modification,
                                pixel_modification, &interpolator_mask, &ps_param_gen_pos);
  if (vertex_shader) {
    vertex_translation_ok = EnsureShaderTranslated(*vertex_shader, vertex_modification);
    if (vertex_translation_ok) {
      vertex_translation = static_cast<MetalShader::MetalTranslation*>(
          vertex_shader->GetTranslation(vertex_modification));
      // [TEMP upstream-diag] one-shot dump of 0a6d's translated MSL to inspect the
      // offset-3 packed-color unpack (the visible menu draws carry packed RGBA8 there).
      {
        static std::atomic<bool> dumped_0a6d{false};
        if (MetalShaderDumpEnabled() && vertex_translation &&
            vertex_shader->ucode_data_hash() == UINT64_C(0x0a6d1dd7767fdf27) &&
            !dumped_0a6d.exchange(true)) {
          const std::string& src = vertex_translation->msl_source();
          if (FILE* f = std::fopen("/tmp/0a6d_vs.msl", "wb")) {
            std::fwrite(src.data(), 1, src.size(), f);
            std::fclose(f);
          }
          std::fprintf(stderr, "[metal][0A6D-MSL] dumped %zu bytes to /tmp/0a6d_vs.msl\n",
                       src.size());
          std::fflush(stderr);
        }
      }
    }
  }
  if (pixel_shader) {
    pixel_translation_ok = EnsureShaderTranslated(*pixel_shader, pixel_modification);
    if (pixel_translation_ok) {
      pixel_translation = static_cast<MetalShader::MetalTranslation*>(
          pixel_shader->GetTranslation(pixel_modification));
    }
  }
  if (pixel_shader && ps_param_gen_pos != UINT32_MAX) {
    static std::atomic<uint32_t> param_gen_logs{0};
    uint32_t param_gen_index = param_gen_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (param_gen_index <= 16 || (param_gen_index & 0xFF) == 0) {
      std::fprintf(stderr,
                   "[metal] param-gen draw#%u draw=%u ps=%016llx pos=%u "
                   "interp_mask=0x%04x vs_mod=%016llx ps_mod=%016llx\n",
                   param_gen_index, metal_draw_index,
                   static_cast<unsigned long long>(pixel_shader->ucode_data_hash()),
                   ps_param_gen_pos, interpolator_mask,
                   static_cast<unsigned long long>(vertex_modification),
                   static_cast<unsigned long long>(pixel_modification));
      std::fflush(stderr);
    }
  }
  if (pixel_shader && pixel_shader->ucode_data_hash() == UINT64_C(0xbdc93d3c5da8241f)) {
    static std::atomic<uint32_t> bdc_state_logs{0};
    uint32_t bdc_state_index = bdc_state_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (bdc_state_index <= 24 || (bdc_state_index & 0x3F) == 0) {
      std::vector<uint32_t> bdc_float_constants =
          PackFloatConstantsForShader(*register_file_, *pixel_shader);
      auto packed_float = [&](uint32_t constant_index, uint32_t component) -> float {
        uint32_t word_index = constant_index * 4 + component;
        return word_index < bdc_float_constants.size()
                   ? rex::memory::Reinterpret<float>(bdc_float_constants[word_index])
                   : 0.0f;
      };
      uint32_t bdc_float_buffer_index =
          pixel_translation
              ? FindMslBufferIndex(pixel_translation->msl_source(), "xe_uniform_float_constants")
              : UINT32_MAX;
      uint32_t normalized_color_mask =
          draw_util::GetNormalizedColorMask(*register_file_, pixel_shader->writes_color_targets());
      std::fprintf(
          stderr,
          "[metal] bdc draw state#%u draw=%u prim=%u indices=%u "
          "color_mask=0x%04x rb_mask=0x%08x color_info0=0x%08x "
          "blend0=0x%08x colorcontrol=0x%08x float_cb=%u "
          "float_count=%zu c0=(%.4g %.4g %.4g %.4g) "
          "flags=0x%08x alpha_ref=%.4g vs=%016llx vs_pos=%u\n",
          bdc_state_index, metal_draw_index, uint32_t(prim_type), index_count,
          normalized_color_mask, register_file_->values[XE_GPU_REG_RB_COLOR_MASK],
          register_file_->values[XE_GPU_REG_RB_COLOR_INFO],
          register_file_->values[XE_GPU_REG_RB_BLENDCONTROL0],
          register_file_->values[XE_GPU_REG_RB_COLORCONTROL], bdc_float_buffer_index,
          bdc_float_constants.size() / 4, packed_float(0, 0), packed_float(0, 1),
          packed_float(0, 2), packed_float(0, 3), system_constants_.flags,
          register_file_->Get<float>(XE_GPU_REG_RB_ALPHA_REF),
          static_cast<unsigned long long>(vertex_shader ? vertex_shader->ucode_data_hash() : 0),
          vertex_shader && vertex_shader->writes_position() ? 1u : 0u);
      std::fflush(stderr);
    }
  }
  if ((metal_draw_index >= 41 && metal_draw_index <= 90) ||
      (metal_draw_index >= 288 && metal_draw_index <= 330)) {
    std::fprintf(
        stderr,
        "[metal] draw-window#%u prim=%u indices=%u vs=%016llx ps=%016llx "
        "vs_pos=%u vs_mem=0x%02x vs_ok=%u ps_color=0x%02x ps_tex=0x%08x "
        "ps_bind=%zu ps_ok=%u ps_void=%u ps_fallback=%u edram=%u\n",
        metal_draw_index, uint32_t(prim_type), index_count,
        static_cast<unsigned long long>(vertex_shader ? vertex_shader->ucode_data_hash() : 0),
        static_cast<unsigned long long>(pixel_shader ? pixel_shader->ucode_data_hash() : 0),
        vertex_shader && vertex_shader->writes_position() ? 1u : 0u,
        vertex_shader ? uint32_t(vertex_shader->memexport_eM_written()) : 0u,
        vertex_translation_ok ? 1u : 0u, pixel_shader ? pixel_shader->writes_color_targets() : 0u,
        pixel_shader ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0u,
        pixel_shader ? pixel_shader->GetTextureBindingsAfterTranslation().size() : 0,
        pixel_translation_ok ? 1u : 0u, IsVoidFragmentMsl(pixel_translation) ? 1u : 0u,
        pixel_shader &&
                ShouldUseHostFallbackPixelShader(pixel_shader->ucode_data_hash(), pixel_translation)
            ? 1u
            : 0u,
        uint32_t(register_file_->Get<reg::RB_MODECONTROL>().edram_mode));
    std::fflush(stderr);
  }
  {
    uint64_t draw_signature_parts[6] = {
        vertex_shader ? vertex_shader->ucode_data_hash() : 0,
        pixel_shader ? pixel_shader->ucode_data_hash() : 0,
        uint64_t(uint32_t(prim_type)) | (uint64_t(index_count) << 32),
        uint64_t(vertex_shader && vertex_shader->writes_position() ? 1u : 0u) |
            (uint64_t(vertex_shader ? uint32_t(vertex_shader->memexport_eM_written()) : 0u) << 8) |
            (uint64_t(pixel_shader ? pixel_shader->writes_color_targets() : 0u) << 16) |
            (uint64_t(pixel_shader && pixel_shader->writes_depth() ? 1u : 0u) << 24) |
            (uint64_t(pixel_shader && pixel_shader->kills_pixels() ? 1u : 0u) << 25),
        pixel_shader ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0,
        uint64_t(uint32_t(register_file_->Get<reg::RB_MODECONTROL>().edram_mode)) |
            (uint64_t(IsVoidFragmentMsl(pixel_translation) ? 1u : 0u) << 8) |
            (uint64_t(pixel_shader && ShouldUseHostFallbackPixelShader(
                                          pixel_shader->ucode_data_hash(), pixel_translation)
                          ? 1u
                          : 0u)
             << 9),
    };
    uint64_t draw_signature_key = XXH3_64bits(draw_signature_parts, sizeof(draw_signature_parts));
    static std::unordered_set<uint64_t> logged_draw_signatures;
    if (logged_draw_signatures.size() < 128 &&
        logged_draw_signatures.insert(draw_signature_key).second) {
      std::fprintf(
          stderr,
          "[metal] draw signature#%zu first_draw=%u prim=%u indices=%u "
          "vs=%016llx ps=%016llx vs_pos=%u vs_mem=0x%02x "
          "ps_color=0x%02x ps_depth=%u ps_kill=%u ps_tex=0x%08x "
          "ps_bind=%zu ps_void=%u ps_fallback=%u edram=%u\n",
          logged_draw_signatures.size(), metal_draw_index, uint32_t(prim_type), index_count,
          static_cast<unsigned long long>(vertex_shader ? vertex_shader->ucode_data_hash() : 0),
          static_cast<unsigned long long>(pixel_shader ? pixel_shader->ucode_data_hash() : 0),
          vertex_shader && vertex_shader->writes_position() ? 1u : 0u,
          vertex_shader ? uint32_t(vertex_shader->memexport_eM_written()) : 0u,
          pixel_shader ? pixel_shader->writes_color_targets() : 0u,
          pixel_shader && pixel_shader->writes_depth() ? 1u : 0u,
          pixel_shader && pixel_shader->kills_pixels() ? 1u : 0u,
          pixel_shader ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0u,
          pixel_shader ? pixel_shader->GetTextureBindingsAfterTranslation().size() : 0,
          IsVoidFragmentMsl(pixel_translation) ? 1u : 0u,
          pixel_shader && ShouldUseHostFallbackPixelShader(pixel_shader->ucode_data_hash(),
                                                           pixel_translation)
              ? 1u
              : 0u,
          uint32_t(register_file_->Get<reg::RB_MODECONTROL>().edram_mode));
      std::fflush(stderr);
    }
  }
  uint32_t register_color_mask_all = 0;
  bool register_color_candidate_draw = false;
  if (register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kColorDepth) {
    register_color_mask_all = draw_util::GetNormalizedColorMask(*register_file_, 0xF);
    if (register_color_mask_all) {
      register_color_candidate_draw = true;
      ++register_color_candidate_draws_this_swap_;
    }
  }
  auto active_color_write_mask = [&]() -> uint32_t {
    if (!register_color_candidate_draw) {
      return 0;
    }
    uint32_t shader_color_mask = pixel_shader ? uint32_t(pixel_shader->writes_color_targets()) : 0;
    if (shader_color_mask) {
      uint32_t normalized_shader_mask =
          draw_util::GetNormalizedColorMask(*register_file_, shader_color_mask);
      if (normalized_shader_mask) {
        return normalized_shader_mask;
      }
    }
    return register_color_mask_all;
  };
  bool owned_rt_routed_this_draw = false;
  if (!register_color_candidate_draw && vertex_shader && pixel_shader &&
      !vertex_shader->writes_position() && !vertex_shader->memexport_eM_written() &&
      pixel_translation_ok && !pixel_shader->writes_color_targets() &&
      !pixel_shader->writes_depth() && !pixel_shader->kills_pixels() &&
      !pixel_shader->memexport_eM_written() &&
      pixel_shader->GetTextureBindingsAfterTranslation().empty() &&
      IsVoidFragmentMsl(pixel_translation)) {
    if (register_color_candidate_draw) {
      ++register_color_unrouted_draws_this_swap_;
      static std::atomic<uint32_t> register_color_unrouted_logs{0};
      uint32_t register_color_unrouted_index =
          register_color_unrouted_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (register_color_unrouted_index <= 64 || (register_color_unrouted_index & 0xFF) == 0) {
        std::fprintf(
            stderr,
            "[metal] register-color unrouted#%u draw=%u reason=no-effect-skip "
            "prim=%u indices=%u reg_mask=0x%04x rb_mask=0x%08x "
            "color0=0x%08x blend0=0x%08x vs=%016llx ps=%016llx "
            "vs_pos=%u ps_color=0x%02x ps_void=%u ps_tex=0x%08x\n",
            register_color_unrouted_index, metal_draw_index, uint32_t(prim_type), index_count,
            register_color_mask_all, register_file_->values[XE_GPU_REG_RB_COLOR_MASK],
            register_file_->values[XE_GPU_REG_RB_COLOR_INFO],
            register_file_->values[XE_GPU_REG_RB_BLENDCONTROL0],
            static_cast<unsigned long long>(vertex_shader->ucode_data_hash()),
            static_cast<unsigned long long>(pixel_shader->ucode_data_hash()),
            vertex_shader->writes_position() ? 1u : 0u, pixel_shader->writes_color_targets(),
            IsVoidFragmentMsl(pixel_translation) ? 1u : 0u,
            pixel_shader->GetUsedTextureMaskAfterTranslation());
        std::fflush(stderr);
      }
    }
    static std::atomic<uint32_t> no_effect_draw_logs{0};
    uint32_t no_effect_draw_index = no_effect_draw_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (no_effect_draw_index <= 16 || (no_effect_draw_index & 0xFF) == 0) {
      std::fprintf(stderr,
                   "[metal] skipped no-effect draw#%u draw=%u prim=%u indices=%u "
                   "vs=%016llx ps=%016llx\n",
                   no_effect_draw_index, metal_draw_index, uint32_t(prim_type), index_count,
                   static_cast<unsigned long long>(vertex_shader->ucode_data_hash()),
                   static_cast<unsigned long long>(pixel_shader->ucode_data_hash()));
      std::fflush(stderr);
    }
    return true;
  }
  bool draw_constants_updated = false;
  auto update_draw_constants = [&]() {
    if (!draw_constants_updated) {
      UpdateMinimalSystemConstants(prim_type, index_buffer_info);
      if (primitive_processing_ok) {
        system_constants_.vertex_index_endian =
            primitive_processing_result.host_shader_index_endian;
        system_constants_.line_loop_closing_index =
            primitive_processing_result.line_loop_closing_index;
      }
      UpdateGuestConstantBuffers();
      draw_constants_updated = true;
    }
  };
  if (pixel_shader && pixel_shader->writes_color_targets() &&
      register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kColorDepth) {
    ++color_target_candidate_draws_this_swap_;
  }
  if (metal_device_ && vertex_shader && vertex_translation_ok &&
      HasMemExportSideEffects(*vertex_shader, vertex_translation)) {
    update_draw_constants();
    static std::atomic<uint32_t> memexport_detect_logs{0};
    uint32_t memexport_detect_index =
        memexport_detect_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (memexport_detect_index <= 16 || (memexport_detect_index & 0xFF) == 0) {
      std::fprintf(stderr,
                   "[metal] memexport side-effect shader#%u draw=%u vs=%016llx "
                   "eM=0x%02x msl_writes=%u\n",
                   memexport_detect_index, metal_draw_index,
                   static_cast<unsigned long long>(vertex_shader->ucode_data_hash()),
                   uint32_t(vertex_shader->memexport_eM_written()),
                   MslWritesSharedMemory(vertex_translation) ? 1u : 0u);
      std::fflush(stderr);
    }
    TraceCpuMemExportShader(*register_file_, *memory_, *vertex_shader, prim_type, index_count,
                            metal_draw_index);
    ExecuteMemExportVertexShader(*vertex_shader, prim_type, index_count);
  }
  MetalShader* route_vertex_shader = nullptr;
  bool route_uses_reused_position_shader = false;
  if (vertex_shader && vertex_shader->writes_position()) {
    route_vertex_shader = vertex_shader;
  } else if (vertex_shader && !vertex_shader->writes_position() && last_position_vertex_shader_ &&
             last_position_registers_valid_ && !vertex_shader->memexport_eM_written()) {
    route_vertex_shader = last_position_vertex_shader_;
    route_uses_reused_position_shader = true;
    if (!route_vertex_shader->is_ucode_analyzed()) {
      route_vertex_shader->AnalyzeUcode(ucode_disasm_buffer_);
    }
  }
  if (metal_device_ && route_vertex_shader && route_vertex_shader->writes_position() &&
      pixel_shader && pixel_translation_ok &&
      register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kColorDepth &&
      !IsVoidFragmentMsl(pixel_translation)) {
    void* pipeline_state = EnsureRenderPipeline(*route_vertex_shader, *pixel_shader);
    uint32_t normalized_color_mask = active_color_write_mask();
    if (pipeline_state && normalized_color_mask) {
      update_draw_constants();
      bool routed_this_draw = false;
      for (uint32_t rt_index = 0; rt_index < xenos::kMaxColorRenderTargets; ++rt_index) {
        if (!((normalized_color_mask >> (rt_index * 4)) & 0xF)) {
          continue;
        }
        HostRenderTarget* active_host_rt = EnsureHostRenderTarget(rt_index);
        if (!active_host_rt || !active_host_rt->context) {
          continue;
        }
        routed_this_draw = true;
        ++owned_rt_routed_targets_this_swap_;
        void* saved_host_render_target_context = host_render_target_context_;
        host_render_target_context_ = active_host_rt->context;
        TryRenderPipelineProbe(*route_vertex_shader, *pixel_shader, pipeline_state, host_prim_type,
                               host_draw_vertex_count, true,
                               primitive_processing_ok ? &primitive_processing_result : nullptr);
        if (latest_host_render_target_width_ && latest_host_render_target_height_ &&
            !latest_host_render_target_bgra_.empty()) {
          bool new_has_visible_rgb = BgraHasNonZeroRgb(latest_host_render_target_bgra_);
          bool cached_has_visible_rgb = BgraHasNonZeroRgb(active_host_rt->bgra);
          if (new_has_visible_rgb || !cached_has_visible_rgb) {
            active_host_rt->bgra = latest_host_render_target_bgra_;
            active_host_rt->width = latest_host_render_target_width_;
            active_host_rt->height = latest_host_render_target_height_;
          } else {
            static std::atomic<uint32_t> preserved_rt_logs{0};
            uint32_t preserved_rt_index =
                preserved_rt_logs.fetch_add(1, std::memory_order_relaxed) + 1;
            if (preserved_rt_index <= 16 || (preserved_rt_index & 0xFF) == 0) {
              std::fprintf(stderr,
                           "[metal] preserved owned RT#%u draw=%u rt=%u "
                           "new_visible=0 cached_visible=%u\n",
                           preserved_rt_index, metal_draw_index, rt_index,
                           CountVisibleRgbPixels(active_host_rt->bgra));
              std::fflush(stderr);
            }
          }
        }
        if (vertex_shader->ucode_data_hash() == UINT64_C(0x0a6d1dd7767fdf27) &&
            pixel_shader->ucode_data_hash() == UINT64_C(0x2e372ea28cc404b7) && rt_index == 0 &&
            memory_) {
          static std::atomic<uint32_t> producer_vertex_logs{0};
          uint32_t producer_vertex_index =
              producer_vertex_logs.fetch_add(1, std::memory_order_relaxed) + 1;
          if (producer_vertex_index <= 16 || (producer_vertex_index & 0x3F) == 0) {
            xenos::xe_gpu_vertex_fetch_t fetch = register_file_->GetVertexFetch(0);
            uint32_t fetch_byte_address = uint32_t(fetch.address * sizeof(uint32_t));
            const uint32_t* vertex_words =
                memory_->TranslatePhysical<const uint32_t*>(fetch_byte_address);
            std::fprintf(stderr,
                         "[metal] producer vertex input#%u draw=%u prim=%u indices=%u "
                         "fetch0(type=%u addr=0x%08x stride=7 size=%u endian=%u) "
                         "flags=0x%08x ndc_scale=(%.4g %.4g %.4g) "
                         "ndc_offset=(%.4g %.4g %.4g)\n",
                         producer_vertex_index, metal_draw_index, uint32_t(prim_type), index_count,
                         uint32_t(fetch.type), fetch_byte_address, uint32_t(fetch.size),
                         uint32_t(fetch.endian), system_constants_.flags,
                         system_constants_.ndc_scale[0], system_constants_.ndc_scale[1],
                         system_constants_.ndc_scale[2], system_constants_.ndc_offset[0],
                         system_constants_.ndc_offset[1], system_constants_.ndc_offset[2]);
            if (vertex_words && fetch.type == xenos::FetchConstantType::kVertex) {
              uint32_t sample_count = std::min<uint32_t>(3, std::max<uint32_t>(index_count, 1));
              for (uint32_t sample = 0; sample < sample_count; ++sample) {
                uint32_t guest_vertex_index = sample;
                const uint32_t* words = vertex_words + guest_vertex_index * 7;
                float position[4] = {
                    xenos::GpuSwap(rex::memory::Reinterpret<float>(words[0]), fetch.endian),
                    xenos::GpuSwap(rex::memory::Reinterpret<float>(words[1]), fetch.endian),
                    xenos::GpuSwap(rex::memory::Reinterpret<float>(words[2]), fetch.endian),
                    1.0f,
                };
                float color[4] = {
                    xenos::GpuSwap(rex::memory::Reinterpret<float>(words[3]), fetch.endian),
                    xenos::GpuSwap(rex::memory::Reinterpret<float>(words[4]), fetch.endian),
                    xenos::GpuSwap(rex::memory::Reinterpret<float>(words[5]), fetch.endian),
                    xenos::GpuSwap(rex::memory::Reinterpret<float>(words[6]), fetch.endian),
                };
                float reciprocal_w =
                    (system_constants_.flags & SpirvShaderTranslator::kSysFlag_WNotReciprocal)
                        ? position[3]
                        : (position[3] ? 1.0f / position[3] : 0.0f);
                float ndc_x = (position[0] * reciprocal_w) * system_constants_.ndc_scale[0] +
                              system_constants_.ndc_offset[0] * reciprocal_w;
                float ndc_y = (position[1] * reciprocal_w) * system_constants_.ndc_scale[1] +
                              system_constants_.ndc_offset[1] * reciprocal_w;
                float ndc_z = position[2] * system_constants_.ndc_scale[2] +
                              system_constants_.ndc_offset[2] * reciprocal_w;
                std::fprintf(stderr,
                             "[metal]   producer v%u pos=(%.4g %.4g %.4g %.4g) "
                             "ndc=(%.4g %.4g %.4g %.4g) "
                             "color=(%.4g %.4g %.4g %.4g) words=%08x %08x %08x "
                             "%08x %08x %08x %08x\n",
                             sample, position[0], position[1], position[2], position[3], ndc_x,
                             ndc_y, ndc_z, reciprocal_w, color[0], color[1], color[2], color[3],
                             words[0], words[1], words[2], words[3], words[4], words[5], words[6]);
              }
            } else {
              std::fprintf(stderr, "[metal]   producer vertex input unavailable\n");
            }
            std::fflush(stderr);
          }
        }
        host_render_target_context_ = saved_host_render_target_context;
        static std::atomic<uint32_t> host_rt_draw_logs{0};
        uint32_t host_rt_draw_index = host_rt_draw_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (host_rt_draw_index <= 32 || (host_rt_draw_index & 0xFF) == 0) {
          std::fprintf(
              stderr,
              "[metal] host RT draw#%u draw=%u rt=%u color_mask=0x%04x "
              "visible=%u size=%ux%u color=0x%08x surface=0x%08x "
              "route_vs=%016llx active_vs=%016llx reused_pos=%u\n",
              host_rt_draw_index, metal_draw_index, rt_index, normalized_color_mask,
              CountVisibleRgbPixels(active_host_rt->bgra), active_host_rt->width,
              active_host_rt->height, active_host_rt->color_info, active_host_rt->surface_info,
              static_cast<unsigned long long>(route_vertex_shader->ucode_data_hash()),
              static_cast<unsigned long long>(vertex_shader ? vertex_shader->ucode_data_hash() : 0),
              route_uses_reused_position_shader ? 1u : 0u);
          std::fflush(stderr);
        }
      }
      if (routed_this_draw) {
        owned_rt_routed_this_draw = true;
        ++owned_rt_routed_draws_this_swap_;
      }
    }
  }
  if (MetalHostPixelDiagnosticsEnabled() && metal_device_ && vertex_shader &&
      !vertex_shader->writes_position() && pixel_shader && pixel_translation_ok &&
      !IsVoidFragmentMsl(pixel_translation) &&
      register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kColorDepth) {
    uint32_t normalized_color_mask = active_color_write_mask();
    if (normalized_color_mask) {
      update_draw_constants();
      bool routed_this_draw = false;
      for (uint32_t rt_index = 0; rt_index < xenos::kMaxColorRenderTargets; ++rt_index) {
        if (!((normalized_color_mask >> (rt_index * 4)) & 0xF)) {
          continue;
        }
        HostRenderTarget* active_host_rt = EnsureHostRenderTarget(rt_index);
        if (!active_host_rt || !active_host_rt->context) {
          continue;
        }
        routed_this_draw = true;
        ++owned_rt_routed_targets_this_swap_;
        std::vector<uint8_t> fullscreen_bgra;
        bool rendered_fullscreen = RenderFullscreenPixelShader(
            *pixel_shader, fallback_output_width_, fallback_output_height_, fullscreen_bgra, false);
        bool fullscreen_has_visible_rgb = BgraHasNonZeroRgb(fullscreen_bgra);
        bool host_rt_has_visible_rgb = BgraHasNonZeroRgb(active_host_rt->bgra);
        bool accept_fullscreen_rt =
            rendered_fullscreen && fullscreen_has_visible_rgb &&
            fullscreen_bgra.size() >= size_t(fallback_output_width_) * fallback_output_height_ * 4;
        if (accept_fullscreen_rt) {
          active_host_rt->bgra = std::move(fullscreen_bgra);
          active_host_rt->width = fallback_output_width_;
          active_host_rt->height = fallback_output_height_;
        }
        static std::atomic<uint32_t> positionless_host_rt_draw_logs{0};
        uint32_t positionless_host_rt_draw_index =
            positionless_host_rt_draw_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (positionless_host_rt_draw_index <= 32 ||
            (positionless_host_rt_draw_index & 0xFF) == 0) {
          std::fprintf(stderr,
                       "[metal] positionless RT draw#%u draw=%u rt=%u "
                       "color_mask=0x%04x rendered=%u accepted=%u visible=%u size=%ux%u "
                       "ps=%016llx textures=%u\n",
                       positionless_host_rt_draw_index, metal_draw_index, rt_index,
                       normalized_color_mask, rendered_fullscreen ? 1u : 0u,
                       accept_fullscreen_rt ? 1u : 0u, CountVisibleRgbPixels(active_host_rt->bgra),
                       active_host_rt->width, active_host_rt->height,
                       static_cast<unsigned long long>(pixel_shader->ucode_data_hash()),
                       pixel_shader->GetUsedTextureMaskAfterTranslation());
          std::fflush(stderr);
        }
      }
      if (routed_this_draw) {
        owned_rt_routed_this_draw = true;
        ++owned_rt_routed_draws_this_swap_;
      }
    }
  }
  if (register_color_candidate_draw && !owned_rt_routed_this_draw) {
    ++register_color_unrouted_draws_this_swap_;
    static std::atomic<uint32_t> register_color_unrouted_logs{0};
    uint32_t register_color_unrouted_index =
        register_color_unrouted_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (register_color_unrouted_index <= 64 || (register_color_unrouted_index & 0xFF) == 0) {
      std::fprintf(
          stderr,
          "[metal] register-color unrouted#%u draw=%u reason=no-route "
          "prim=%u indices=%u reg_mask=0x%04x rb_mask=0x%08x "
          "color0=0x%08x blend0=0x%08x vs=%016llx ps=%016llx "
          "vs_pos=%u vs_ok=%u ps_color=0x%02x ps_ok=%u ps_void=%u "
          "ps_tex=0x%08x ps_bind=%zu\n",
          register_color_unrouted_index, metal_draw_index, uint32_t(prim_type), index_count,
          register_color_mask_all, register_file_->values[XE_GPU_REG_RB_COLOR_MASK],
          register_file_->values[XE_GPU_REG_RB_COLOR_INFO],
          register_file_->values[XE_GPU_REG_RB_BLENDCONTROL0],
          static_cast<unsigned long long>(vertex_shader ? vertex_shader->ucode_data_hash() : 0),
          static_cast<unsigned long long>(pixel_shader ? pixel_shader->ucode_data_hash() : 0),
          vertex_shader && vertex_shader->writes_position() ? 1u : 0u,
          vertex_translation_ok ? 1u : 0u, pixel_shader ? pixel_shader->writes_color_targets() : 0u,
          pixel_translation_ok ? 1u : 0u, IsVoidFragmentMsl(pixel_translation) ? 1u : 0u,
          pixel_shader ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0u,
          pixel_shader ? pixel_shader->GetTextureBindingsAfterTranslation().size() : 0);
      std::fflush(stderr);
    }
  }
  bool host_render_target_debug = MetalHostRenderTargetDebugEnabled();
  if (MetalPipelineProbesEnabled() && metal_device_ && vertex_shader &&
      vertex_shader->writes_position() && pixel_shader && vertex_translation_ok &&
      pixel_translation_ok) {
    if (void* pipeline_state = EnsureRenderPipeline(*vertex_shader, *pixel_shader)) {
      update_draw_constants();
      TryRenderPipelineProbe(*vertex_shader, *pixel_shader, pipeline_state, prim_type, index_count,
                             false);
    }
  }
  if (host_render_target_debug && metal_device_ && vertex_shader &&
      !vertex_shader->writes_position() && pixel_shader && pixel_translation_ok &&
      !pixel_shader->GetTextureBindingsAfterTranslation().empty()) {
    update_draw_constants();
    std::vector<uint8_t> fullscreen_bgra;
    bool fullscreen_ok = RenderFullscreenPixelShader(
        *pixel_shader, fallback_output_width_, fallback_output_height_, fullscreen_bgra, true);
    bool fullscreen_nonzero = fullscreen_ok && BgraHasNonZeroRgb(fullscreen_bgra);
    static std::atomic<uint32_t> host_rt_fullscreen_logs{0};
    uint32_t host_rt_fullscreen_index =
        host_rt_fullscreen_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (host_rt_fullscreen_index <= 16 || (host_rt_fullscreen_index & 0x3F) == 0) {
      std::fprintf(stderr,
                   "[metal] host render target fullscreen#%u draw=%u ok=%u nonzero=%u "
                   "ps=%016llx textures=%zu\n",
                   host_rt_fullscreen_index, metal_draw_index, fullscreen_ok ? 1u : 0u,
                   fullscreen_nonzero ? 1u : 0u,
                   static_cast<unsigned long long>(pixel_shader->ucode_data_hash()),
                   pixel_shader->GetTextureBindingsAfterTranslation().size());
      std::fflush(stderr);
    }
  }
  if (MetalFullscreenProbeEnabled() && metal_device_ && vertex_shader &&
      !vertex_shader->writes_position() && pixel_shader && pixel_translation_ok &&
      !pixel_shader->GetTextureBindingsAfterTranslation().empty()) {
    update_draw_constants();
    std::vector<uint8_t> fullscreen_bgra;
    if (RenderFullscreenPixelShader(*pixel_shader, fallback_output_width_, fallback_output_height_,
                                    fullscreen_bgra) &&
        BgraHasNonZeroRgb(fullscreen_bgra)) {
      BgraFrameStats fullscreen_stats = GetBgraFrameStats(fullscreen_bgra);
      bool useful_sparse_fullscreen = IsUsefulSparseVisibleFrame(
          fullscreen_bgra, fallback_output_width_, fallback_output_height_, fullscreen_stats);
      if ((fullscreen_stats.visible_pixels * 3 < fallback_output_width_ * fallback_output_height_ &&
           !useful_sparse_fullscreen) ||
          IsDominantFlatVisibleFrame(fullscreen_bgra, fallback_output_width_,
                                     fallback_output_height_, fullscreen_stats.visible_pixels)) {
        static std::atomic<uint32_t> skipped_flat_fullscreen_logs{0};
        uint32_t skipped_flat_fullscreen_index =
            skipped_flat_fullscreen_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (skipped_flat_fullscreen_index <= 16 || (skipped_flat_fullscreen_index & 0x3F) == 0) {
          std::fprintf(
              stderr,
              "[metal] skipped sparse/flat fullscreen postprocess#%u draw=%u "
              "visible=%u grid=%u range=%u size=%ux%u\n",
              skipped_flat_fullscreen_index, metal_draw_index, fullscreen_stats.visible_pixels,
              BgraVisibleGridCellCount(fullscreen_bgra, fallback_output_width_,
                                       fallback_output_height_),
              BgraRgbRange(fullscreen_stats), fallback_output_width_, fallback_output_height_);
          std::fflush(stderr);
        }
      } else {
        auto fullscreen_sample = [&](uint32_t x, uint32_t y) -> const uint8_t* {
          static const uint8_t zero[4] = {};
          if (!fallback_output_width_ || !fallback_output_height_ || fullscreen_bgra.empty()) {
            return zero;
          }
          x = std::min(x, fallback_output_width_ - 1);
          y = std::min(y, fallback_output_height_ - 1);
          size_t offset = (size_t(y) * fallback_output_width_ + x) * 4;
          return offset + 3 < fullscreen_bgra.size() ? fullscreen_bgra.data() + offset : zero;
        };
        static std::atomic<uint32_t> fullscreen_sample_logs{0};
        uint32_t fullscreen_sample_index =
            fullscreen_sample_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fullscreen_sample_index <= 16 || (fullscreen_sample_index & 0x3F) == 0) {
          const uint8_t* p00 = fullscreen_sample(0, 0);
          const uint8_t* p25 =
              fullscreen_sample(fallback_output_width_ / 4, fallback_output_height_ / 4);
          const uint8_t* p50 =
              fullscreen_sample(fallback_output_width_ / 2, fallback_output_height_ / 2);
          const uint8_t* p75 =
              fullscreen_sample(fallback_output_width_ * 3 / 4, fallback_output_height_ * 3 / 4);
          std::fprintf(stderr,
                       "[metal] fullscreen sample#%u draw=%u ps=%016llx visible=%u range=%u "
                       "p00=%02x %02x %02x %02x p25=%02x %02x %02x %02x "
                       "p50=%02x %02x %02x %02x p75=%02x %02x %02x %02x\n",
                       fullscreen_sample_index, metal_draw_index,
                       static_cast<unsigned long long>(pixel_shader->ucode_data_hash()),
                       fullscreen_stats.visible_pixels, BgraRgbRange(fullscreen_stats), p00[0],
                       p00[1], p00[2], p00[3], p25[0], p25[1], p25[2], p25[3], p50[0], p50[1],
                       p50[2], p50[3], p75[0], p75[1], p75[2], p75[3]);
          std::fflush(stderr);
        }
        if (fullscreen_sample_index <= 8) {
          DumpBgraFrameAsPpm("fullscreen", fullscreen_sample_index, fullscreen_bgra,
                             fallback_output_width_, fallback_output_height_);
        }
        if (last_copy_dest_base_) {
          if (BlitAndWriteResolvedColor(last_copy_dest_base_, fallback_output_width_,
                                        fallback_output_height_, fullscreen_bgra,
                                        fallback_output_width_, fallback_output_height_, 0, 0,
                                        fallback_output_width_, fallback_output_height_)) {
            static std::atomic<uint32_t> fullscreen_resolve_write_logs{0};
            uint32_t fullscreen_resolve_write_index =
                fullscreen_resolve_write_logs.fetch_add(1, std::memory_order_relaxed) + 1;
            if (fullscreen_resolve_write_index <= 16 ||
                (fullscreen_resolve_write_index & 0xFF) == 0) {
              std::fprintf(stderr,
                           "[metal] wrote fullscreen postprocess resolve#%u draw=%u "
                           "base=0x%08x size=%ux%u\n",
                           fullscreen_resolve_write_index, metal_draw_index, last_copy_dest_base_,
                           fallback_output_width_, fallback_output_height_);
              std::fflush(stderr);
            }
          }
        }
        latest_fullscreen_postprocess_bgra_ = std::move(fullscreen_bgra);
        latest_fullscreen_postprocess_width_ = fallback_output_width_;
        latest_fullscreen_postprocess_height_ = fallback_output_height_;
        latest_fullscreen_postprocess_draw_count_ = draw_count_;
        static std::atomic<uint32_t> fullscreen_postprocess_logs{0};
        uint32_t fullscreen_postprocess_index =
            fullscreen_postprocess_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fullscreen_postprocess_index <= 16 || (fullscreen_postprocess_index & 0xFF) == 0) {
          std::vector<uint32_t> packed_pixel_constants =
              PackFloatConstantsForShader(*register_file_, *pixel_shader);
          auto pixel_constant = [&](uint32_t packed_index, uint32_t component) -> float {
            uint32_t word_index = packed_index * 4 + component;
            return word_index < packed_pixel_constants.size()
                       ? rex::memory::Reinterpret<float>(packed_pixel_constants[word_index])
                       : 0.0f;
          };
          xenos::xe_gpu_texture_fetch_t fetch0 = register_file_->GetTextureFetch(0);
          xenos::xe_gpu_texture_fetch_t fetch1 = register_file_->GetTextureFetch(1);
          std::fprintf(
              stderr,
              "[metal] fullscreen postprocess frame#%u draw=%u prim=%u indices=%u "
              "vs=%016llx ps=%016llx size=%ux%u pc_count=%zu "
              "c0=(%.4g %.4g %.4g %.4g) c1=(%.4g %.4g %.4g %.4g) "
              "tf0=(%08x %08x %08x %08x %08x %08x) "
              "tf1=(%08x %08x %08x %08x %08x %08x)\n",
              fullscreen_postprocess_index, metal_draw_index, uint32_t(prim_type), index_count,
              static_cast<unsigned long long>(vertex_shader ? vertex_shader->ucode_data_hash() : 0),
              static_cast<unsigned long long>(pixel_shader->ucode_data_hash()),
              latest_fullscreen_postprocess_width_, latest_fullscreen_postprocess_height_,
              packed_pixel_constants.size() / 4, pixel_constant(0, 0), pixel_constant(0, 1),
              pixel_constant(0, 2), pixel_constant(0, 3), pixel_constant(1, 0),
              pixel_constant(1, 1), pixel_constant(1, 2), pixel_constant(1, 3), fetch0.dword_0,
              fetch0.dword_1, fetch0.dword_2, fetch0.dword_3, fetch0.dword_4, fetch0.dword_5,
              fetch1.dword_0, fetch1.dword_1, fetch1.dword_2, fetch1.dword_3, fetch1.dword_4,
              fetch1.dword_5);
          std::fflush(stderr);
        }
      }
    }
  }

  if (MetalHeuristicPresentationEnabled() &&
      pending_draw_events_.size() < MetalDrawRenderer::kMaxDrawEventsPerFrame) {
    MetalDrawEvent& event = pending_draw_events_.emplace_back();
    event.primitive_type = uint32_t(prim_type);
    event.index_count = index_count;
    event.vertex_shader_hash = vertex_shader ? vertex_shader->ucode_data_hash() : 0;
    event.pixel_shader_hash = pixel_shader ? pixel_shader->ucode_data_hash() : 0;
    event.vertex_binding_count =
        vertex_shader ? uint32_t(vertex_shader->vertex_bindings().size()) : 0;
    event.texture_binding_count =
        (vertex_shader ? uint32_t(vertex_shader->texture_bindings().size()) : 0) +
        (pixel_shader ? uint32_t(pixel_shader->texture_bindings().size()) : 0);
  }

  bool current_draw_has_host_texture = false;
  bool current_draw_has_shader_texture = false;
  uint32_t current_draw_texture_width = 0;
  uint32_t current_draw_texture_height = 0;
  uint32_t current_draw_texture_base = 0;
  uint32_t current_draw_texture_pitch = 0;
  uint32_t current_draw_texture_resolve_x_offset = 0;
  uint32_t current_draw_texture_resolve_y_offset = 0;
  if (pixel_shader && register_file_) {
    uint64_t selected_texture_area = 0;
    for (const auto& binding : pixel_shader->GetTextureBindingsAfterTranslation()) {
      xenos::xe_gpu_texture_fetch_t texture_fetch =
          register_file_->GetTextureFetch(binding.fetch_constant);
      if (texture_fetch.type != xenos::FetchConstantType::kTexture ||
          texture_fetch.dimension != xenos::DataDimension::k2DOrStacked) {
        continue;
      }
      uint32_t width_minus_1 = 0;
      uint32_t height_minus_1 = 0;
      uint32_t depth_or_array_size_minus_1 = 0;
      texture_util::GetSubresourcesFromFetchConstant(texture_fetch, &width_minus_1, &height_minus_1,
                                                     &depth_or_array_size_minus_1, nullptr, nullptr,
                                                     nullptr, nullptr);
      if (depth_or_array_size_minus_1 != 0) {
        continue;
      }
      uint32_t texture_width = width_minus_1 + 1;
      uint32_t texture_height = height_minus_1 + 1;
      uint64_t texture_area = uint64_t(texture_width) * texture_height;
      if (texture_area > selected_texture_area) {
        selected_texture_area = texture_area;
        current_draw_texture_width = texture_width;
        current_draw_texture_height = texture_height;
        current_draw_texture_base = texture_fetch.base_address << 12;
        current_draw_texture_pitch = std::max<uint32_t>(texture_fetch.pitch << 5, texture_width);
        current_draw_has_shader_texture = true;
      }
    }
    if (current_draw_texture_base && current_draw_texture_pitch &&
        last_copy_dest_base_ > current_draw_texture_base) {
      uint32_t resolve_delta = last_copy_dest_base_ - current_draw_texture_base;
      uint32_t row_bytes = current_draw_texture_pitch * 4;
      if (row_bytes) {
        current_draw_texture_resolve_y_offset = resolve_delta / row_bytes;
        current_draw_texture_resolve_x_offset = (resolve_delta % row_bytes) / 4;
        if (current_draw_texture_resolve_y_offset >= current_draw_texture_height) {
          current_draw_texture_resolve_y_offset = 0;
          current_draw_texture_resolve_x_offset = 0;
        }
      }
    }
  }
  if ((MetalHeuristicPresentationEnabled() || MetalFallbackResolveEnabled()) && pixel_shader &&
      memory_ && register_file_) {
    for (const auto& binding : pixel_shader->GetTextureBindingsAfterTranslation()) {
      xenos::xe_gpu_texture_fetch_t texture_fetch =
          register_file_->GetTextureFetch(binding.fetch_constant);
      std::vector<uint8_t> texture_rgba;
      uint32_t texture_width = 0;
      uint32_t texture_height = 0;
      if (!DecodeTextureFetchToRgba(texture_fetch, 0, 0, texture_rgba, texture_width,
                                    texture_height) ||
          !texture_width || !texture_height) {
        continue;
      }
      std::vector<uint8_t> texture_bgra(size_t(texture_width) * texture_height * 4);
      std::vector<uint8_t> texture_sample_rgba(size_t(texture_width) * texture_height * 4);
      for (size_t pixel = 0, pixel_count = size_t(texture_width) * texture_height;
           pixel < pixel_count; ++pixel) {
        const uint8_t* source_pixel = texture_rgba.data() + pixel * 4;
        uint8_t swizzled_rgba[4] = {
            ResolveSwizzledComponent(source_pixel, SwizzleComponent(texture_fetch.swizzle, 0)),
            ResolveSwizzledComponent(source_pixel, SwizzleComponent(texture_fetch.swizzle, 1)),
            ResolveSwizzledComponent(source_pixel, SwizzleComponent(texture_fetch.swizzle, 2)),
            ResolveSwizzledComponent(source_pixel, SwizzleComponent(texture_fetch.swizzle, 3)),
        };
        uint8_t* sample_pixel = texture_sample_rgba.data() + pixel * 4;
        std::memcpy(sample_pixel, swizzled_rgba, 4);
        texture_bgra[pixel * 4 + 0] = swizzled_rgba[2];
        texture_bgra[pixel * 4 + 1] = swizzled_rgba[1];
        texture_bgra[pixel * 4 + 2] = swizzled_rgba[0];
        texture_bgra[pixel * 4 + 3] = swizzled_rgba[3];
      }
      if (!BgraHasNonZeroRgb(texture_bgra)) {
        continue;
      }
      pending_texture_resolve_bgra_ = std::move(texture_bgra);
      pending_texture_resolve_width_ = texture_width;
      pending_texture_resolve_height_ = texture_height;
      pending_host_texture_rgba_ = std::move(texture_sample_rgba);
      pending_host_texture_width_ = texture_width;
      pending_host_texture_height_ = texture_height;
      current_draw_texture_width = texture_width;
      current_draw_texture_height = texture_height;
      RetainTextureCandidateIfUseful(pending_texture_resolve_bgra_, texture_width, texture_height,
                                     "draw texture");
      current_draw_has_host_texture = true;
      static std::atomic<uint32_t> pending_texture_resolve_logs{0};
      uint32_t pending_texture_resolve_index =
          pending_texture_resolve_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (pending_texture_resolve_index <= 16 || (pending_texture_resolve_index & 0xFF) == 0) {
        std::fprintf(stderr,
                     "[metal] pending texture resolve#%u draw=%u ps=%016llx fetch=%u "
                     "size=%ux%u swiz=0x%03x\n",
                     pending_texture_resolve_index, metal_draw_index,
                     static_cast<unsigned long long>(pixel_shader->ucode_data_hash()),
                     binding.fetch_constant, texture_width, texture_height,
                     uint32_t(texture_fetch.swizzle));
        std::fflush(stderr);
      }
      break;
    }
  }

  const bool allow_host_cpu_fallbacks = MetalHostPixelDiagnosticsEnabled();
  bool void_register_color_draw =
      register_color_candidate_draw && pixel_shader && IsVoidFragmentMsl(pixel_translation);
  size_t synthetic_host_vertex_start = pending_host_vertices_.size();
  uint32_t synthetic_host_vertex_count = 0;
  bool emitted_positionless_textured_quad = false;
  if (allow_host_cpu_fallbacks && vertex_shader && !vertex_shader->writes_position() &&
      (current_draw_has_host_texture || current_draw_has_shader_texture) &&
      pending_host_vertices_.size() + 6 <= MetalDrawRenderer::kMaxHostVerticesPerFrame) {
    auto append_fullscreen_textured_vertex = [&](float x, float y, float u, float v) {
      MetalHostVertex& host_vertex = pending_host_vertices_.emplace_back();
      host_vertex.x = x;
      host_vertex.y = y;
      host_vertex.z = 0.0f;
      host_vertex.w = 1.0f;
      host_vertex.r = 1.0f;
      host_vertex.g = 1.0f;
      host_vertex.b = 1.0f;
      host_vertex.a = 1.0f;
      host_vertex.u = u;
      host_vertex.v = v;
      host_vertex.texture_weight = 1.0f;
      host_vertex.viewport_x = 0.0f;
      host_vertex.viewport_y = 0.0f;
      host_vertex.viewport_width = float(std::max<uint32_t>(fallback_output_width_, 1));
      host_vertex.viewport_height = float(std::max<uint32_t>(fallback_output_height_, 1));
      host_vertex.interpolator_mask = UINT32_C(1);
      host_vertex.interpolators[0] = {u, v, u, v};
    };
    append_fullscreen_textured_vertex(-1.0f, 1.0f, 0.0f, 0.0f);
    append_fullscreen_textured_vertex(1.0f, 1.0f, 1.0f, 0.0f);
    append_fullscreen_textured_vertex(-1.0f, -1.0f, 0.0f, 1.0f);
    append_fullscreen_textured_vertex(-1.0f, -1.0f, 0.0f, 1.0f);
    append_fullscreen_textured_vertex(1.0f, 1.0f, 1.0f, 0.0f);
    append_fullscreen_textured_vertex(1.0f, -1.0f, 1.0f, 1.0f);
    synthetic_host_vertex_count = 6;
    emitted_positionless_textured_quad = true;
    static std::atomic<uint32_t> synthetic_textured_quad_logs{0};
    uint32_t synthetic_textured_quad_index =
        synthetic_textured_quad_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (synthetic_textured_quad_index <= 16 || (synthetic_textured_quad_index & 0xFF) == 0) {
      std::fprintf(
          stderr,
          "[metal] synthetic fullscreen textured quad#%u draw=%u vs=%016llx "
          "ps=%016llx texture=%ux%u host_vertices=%zu\n",
          synthetic_textured_quad_index, metal_draw_index,
          static_cast<unsigned long long>(vertex_shader->ucode_data_hash()),
          static_cast<unsigned long long>(pixel_shader ? pixel_shader->ucode_data_hash() : 0),
          pending_host_texture_width_, pending_host_texture_height_, pending_host_vertices_.size());
      std::fflush(stderr);
    }
  }

  uint32_t host_rt_cpu_color_mask = 0;
  bool positionless_producer_needs_interpolator_merge =
      vertex_shader && !vertex_shader->writes_position() &&
      !vertex_shader->memexport_eM_written() && last_position_vertex_shader_ &&
      last_position_registers_valid_;
  bool host_rt_cpu_producer_enabled =
      (MetalHostPixelDiagnosticsEnabled() || positionless_producer_needs_interpolator_merge) &&
      metal_device_ && register_file_ && pixel_shader && pixel_translation_ok &&
      register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kColorDepth &&
      (!IsVoidFragmentMsl(pixel_translation) || void_register_color_draw);
  if (host_rt_cpu_producer_enabled) {
    host_rt_cpu_color_mask = active_color_write_mask();
    host_rt_cpu_producer_enabled = host_rt_cpu_color_mask != 0;
  }
  if (emitted_positionless_textured_quad && host_rt_cpu_producer_enabled && pixel_shader &&
      pixel_translation_ok && IsHostPixelProbeAllowed(*pixel_shader)) {
    for (uint32_t rt_index = 0; rt_index < xenos::kMaxColorRenderTargets; ++rt_index) {
      if (!((host_rt_cpu_color_mask >> (rt_index * 4)) & 0xF)) {
        continue;
      }
      HostRenderTarget* active_host_rt = EnsureHostRenderTarget(rt_index);
      if (!active_host_rt || !active_host_rt->context) {
        continue;
      }
      std::vector<uint8_t> rt_bgra;
      if (RenderHostPixelShader(*pixel_shader, pending_host_vertices_, synthetic_host_vertex_start,
                                synthetic_host_vertex_count, fallback_output_width_,
                                fallback_output_height_, rt_bgra, active_host_rt->context)) {
        active_host_rt->bgra = std::move(rt_bgra);
        active_host_rt->width = fallback_output_width_;
        active_host_rt->height = fallback_output_height_;
        static std::atomic<uint32_t> positionless_quad_rt_logs{0};
        uint32_t positionless_quad_rt_index =
            positionless_quad_rt_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (positionless_quad_rt_index <= 16 || (positionless_quad_rt_index & 0xFF) == 0) {
          BgraFrameStats rt_stats = GetBgraFrameStats(active_host_rt->bgra);
          BgraBandStats rt_band_stats =
              GetBgraBandStats(active_host_rt->bgra, active_host_rt->width, active_host_rt->height);
          std::fprintf(stderr,
                       "[metal] positionless textured RT draw#%u draw=%u rt=%u "
                       "vertices=%u visible=%u range=%u bands(top208=%u mid=%u low=%u) "
                       "size=%ux%u ps=%016llx\n",
                       positionless_quad_rt_index, metal_draw_index, rt_index,
                       synthetic_host_vertex_count, rt_stats.visible_pixels, BgraRgbRange(rt_stats),
                       rt_band_stats.top_208_visible, rt_band_stats.mid_208_512_visible,
                       rt_band_stats.low_512_visible, active_host_rt->width, active_host_rt->height,
                       static_cast<unsigned long long>(pixel_shader->ucode_data_hash()));
          std::fflush(stderr);
        }
      }
    }
  }
  bool need_cpu_interpreted_vertices =
      (MetalHostPixelDiagnosticsEnabled() || host_rt_cpu_producer_enabled) &&
      !emitted_positionless_textured_quad;
  MetalShader* cpu_vertex_shader = nullptr;
  if (need_cpu_interpreted_vertices) {
    if (vertex_shader && vertex_shader->writes_position()) {
      cpu_vertex_shader = vertex_shader;
    } else if (last_position_vertex_shader_) {
      cpu_vertex_shader = last_position_vertex_shader_;
      static std::atomic<uint32_t> substituted_position_vs_logs{0};
      uint32_t substituted_position_vs_index =
          substituted_position_vs_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (substituted_position_vs_index <= 16 || (substituted_position_vs_index & 0xFF) == 0) {
        std::fprintf(
            stderr,
            "[metal] CPU fallback substituting position VS draw=%u active=%016llx "
            "fallback=%016llx\n",
            metal_draw_index,
            static_cast<unsigned long long>(vertex_shader ? vertex_shader->ucode_data_hash() : 0),
            static_cast<unsigned long long>(cpu_vertex_shader->ucode_data_hash()));
        std::fflush(stderr);
      }
    }
  }

  if (need_cpu_interpreted_vertices && kEnableCpuInterpretedVertices && draw_renderer_ && memory_ &&
      cpu_vertex_shader &&
      pending_host_vertices_.size() < MetalDrawRenderer::kMaxHostVerticesPerFrame &&
      ShaderInterpreter::CanInterpretShader(*cpu_vertex_shader)) {
    if (cpu_vertex_shader->ucode_data_hash() == UINT64_C(0x0a6d1dd7767fdf27)) {
      static std::atomic<uint32_t> first_panel_shader_fetch_logs{0};
      uint32_t first_panel_fetch_index =
          first_panel_shader_fetch_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (first_panel_fetch_index <= 4) {
        if (first_panel_fetch_index == 1 && MetalShaderDumpEnabled()) {
          auto dumped_paths = cpu_vertex_shader->DumpUcode("/tmp/goldeneye_metal_ucode");
          std::fprintf(stderr, "[metal] first-panel-vs dumped ucode bin=%s disasm=%s\n",
                       dumped_paths.first.string().c_str(), dumped_paths.second.string().c_str());
        }
        const Shader::ConstantRegisterMap& constant_map =
            cpu_vertex_shader->constant_register_map();
        for (uint32_t bitmap_index = 0;
             bitmap_index < rex::countof(constant_map.vertex_fetch_bitmap); ++bitmap_index) {
          uint32_t bits = constant_map.vertex_fetch_bitmap[bitmap_index];
          uint32_t bit_index = 0;
          while (rex::bit_scan_forward(bits, &bit_index)) {
            bits &= ~(UINT32_C(1) << bit_index);
            uint32_t fetch_index = bitmap_index * 32 + bit_index;
            xenos::xe_gpu_vertex_fetch_t fetch = register_file_->GetVertexFetch(fetch_index);
            const uint32_t* words =
                memory_->TranslatePhysical<const uint32_t*>(fetch.address * sizeof(uint32_t));
            uint32_t sample[42] = {};
            if (words) {
              for (uint32_t sample_index = 0; sample_index < rex::countof(sample); ++sample_index) {
                sample[sample_index] = xenos::GpuSwap(words[sample_index], fetch.endian);
              }
            }
            std::fprintf(stderr,
                         "[metal] first-panel-vs fetch dump#%u draw=%u fetch=%u type=%u "
                         "addr=0x%08x dwords=0x%08x size=%u endian=%u sample="
                         "%08x %08x %08x %08x %08x %08x %08x "
                         "%08x %08x %08x %08x %08x %08x %08x "
                         "%08x %08x %08x %08x %08x %08x %08x "
                         "%08x %08x %08x %08x %08x %08x %08x "
                         "%08x %08x %08x %08x %08x %08x %08x "
                         "%08x %08x %08x %08x %08x %08x %08x\n",
                         first_panel_fetch_index, metal_draw_index, fetch_index,
                         uint32_t(fetch.type), uint32_t(fetch.address * sizeof(uint32_t)),
                         uint32_t(fetch.address), uint32_t(fetch.size), uint32_t(fetch.endian),
                         sample[0], sample[1], sample[2], sample[3], sample[4], sample[5],
                         sample[6], sample[7], sample[8], sample[9], sample[10], sample[11],
                         sample[12], sample[13], sample[14], sample[15], sample[16], sample[17],
                         sample[18], sample[19], sample[20], sample[21], sample[22], sample[23],
                         sample[24], sample[25], sample[26], sample[27], sample[28], sample[29],
                         sample[30], sample[31], sample[32], sample[33], sample[34], sample[35],
                         sample[36], sample[37], sample[38], sample[39], sample[40], sample[41]);
          }
        }
        std::fflush(stderr);
      }
    }
    if (cpu_vertex_shader->ucode_data_hash() == UINT64_C(0x3a1fe1560cf25ff6)) {
      static std::atomic<uint32_t> bad_shader_fetch_logs{0};
      uint32_t bad_shader_fetch_index =
          bad_shader_fetch_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (bad_shader_fetch_index <= 4) {
        if (bad_shader_fetch_index == 1 && MetalShaderDumpEnabled()) {
          auto dumped_paths = cpu_vertex_shader->DumpUcode("/tmp/goldeneye_metal_ucode");
          std::fprintf(stderr, "[metal] bad-vs dumped ucode bin=%s disasm=%s\n",
                       dumped_paths.first.string().c_str(), dumped_paths.second.string().c_str());
        }
        const Shader::ConstantRegisterMap& constant_map =
            cpu_vertex_shader->constant_register_map();
        for (uint32_t bitmap_index = 0;
             bitmap_index < rex::countof(constant_map.vertex_fetch_bitmap); ++bitmap_index) {
          uint32_t bits = constant_map.vertex_fetch_bitmap[bitmap_index];
          uint32_t bit_index = 0;
          while (rex::bit_scan_forward(bits, &bit_index)) {
            bits &= ~(UINT32_C(1) << bit_index);
            uint32_t fetch_index = bitmap_index * 32 + bit_index;
            xenos::xe_gpu_vertex_fetch_t fetch = register_file_->GetVertexFetch(fetch_index);
            uint32_t byte_address = fetch.address * sizeof(uint32_t);
            const uint32_t* words = memory_->TranslatePhysical<const uint32_t*>(byte_address);
            uint32_t sample[30] = {};
            if (words) {
              for (uint32_t sample_index = 0; sample_index < rex::countof(sample); ++sample_index) {
                sample[sample_index] = xenos::GpuSwap(words[sample_index], fetch.endian);
              }
            }
            std::fprintf(stderr,
                         "[metal] bad-vs fetch dump#%u draw=%u fetch=%u type=%u addr=0x%08x "
                         "dwords=0x%08x size=%u endian=%u stride_hint=%u sample=%08x %08x "
                         "%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x "
                         "%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x "
                         "%08x %08x %08x %08x %08x %08x %08x %08x\n",
                         bad_shader_fetch_index, metal_draw_index, fetch_index,
                         uint32_t(fetch.type), byte_address, fetch.address, uint32_t(fetch.size),
                         uint32_t(fetch.endian), 5u, sample[0], sample[1], sample[2], sample[3],
                         sample[4], sample[5], sample[6], sample[7], sample[8], sample[9],
                         sample[10], sample[11], sample[12], sample[13], sample[14], sample[15],
                         sample[16], sample[17], sample[18], sample[19], sample[20], sample[21],
                         sample[22], sample[23], sample[24], sample[25], sample[26], sample[27],
                         sample[28], sample[29]);
          }
        }
        std::vector<uint32_t> packed_float_constants =
            PackFloatConstantsForShader(*register_file_, *cpu_vertex_shader);
        auto packed_float = [&](uint32_t packed_index, uint32_t component) -> float {
          uint32_t word_index = packed_index * 4 + component;
          if (word_index >= packed_float_constants.size()) {
            return 0.0f;
          }
          return rex::memory::Reinterpret<float>(packed_float_constants[word_index]);
        };
        std::fprintf(stderr,
                     "[metal] bad-vs packed floats#%u count=%zu c0=(%.4g %.4g %.4g %.4g) "
                     "c1=(%.4g %.4g %.4g %.4g) c2=(%.4g %.4g %.4g %.4g) "
                     "c3=(%.4g %.4g %.4g %.4g) c79=(%.4g %.4g %.4g %.4g)\n",
                     bad_shader_fetch_index, packed_float_constants.size() / 4, packed_float(0, 0),
                     packed_float(0, 1), packed_float(0, 2), packed_float(0, 3), packed_float(1, 0),
                     packed_float(1, 1), packed_float(1, 2), packed_float(1, 3), packed_float(2, 0),
                     packed_float(2, 1), packed_float(2, 2), packed_float(2, 3), packed_float(3, 0),
                     packed_float(3, 1), packed_float(3, 2), packed_float(3, 3),
                     packed_float(79, 0), packed_float(79, 1), packed_float(79, 2),
                     packed_float(79, 3));
        for (uint32_t bitmap_index = 0;
             bitmap_index < rex::countof(constant_map.vertex_fetch_bitmap); ++bitmap_index) {
          uint32_t bits = constant_map.vertex_fetch_bitmap[bitmap_index];
          uint32_t bit_index = 0;
          while (rex::bit_scan_forward(bits, &bit_index)) {
            bits &= ~(UINT32_C(1) << bit_index);
            uint32_t fetch_index = bitmap_index * 32 + bit_index;
            xenos::xe_gpu_vertex_fetch_t fetch = register_file_->GetVertexFetch(fetch_index);
            const uint32_t* words =
                memory_->TranslatePhysical<const uint32_t*>(fetch.address * sizeof(uint32_t));
            if (!words) {
              continue;
            }
            for (uint32_t vertex = 0; vertex < 6; ++vertex) {
              uint32_t base = vertex * 5;
              uint32_t x_word = xenos::GpuSwap(words[base + 0], fetch.endian);
              uint32_t y_word = xenos::GpuSwap(words[base + 1], fetch.endian);
              uint32_t z_word = xenos::GpuSwap(words[base + 2], fetch.endian);
              uint32_t u_word = xenos::GpuSwap(words[base + 3], fetch.endian);
              uint32_t v_word = xenos::GpuSwap(words[base + 4], fetch.endian);
              float x = rex::memory::Reinterpret<float>(x_word);
              float y = rex::memory::Reinterpret<float>(y_word);
              float z = rex::memory::Reinterpret<float>(z_word);
              float u = rex::memory::Reinterpret<float>(u_word);
              float v = rex::memory::Reinterpret<float>(v_word);
              float wzxy[4] = {1.0f, z, x, y};
              float manual_position[4] = {};
              for (uint32_t component = 0; component < 4; ++component) {
                float constant_wzxy[4] = {packed_float(component, 3), packed_float(component, 2),
                                          packed_float(component, 0), packed_float(component, 1)};
                for (uint32_t dot_component = 0; dot_component < 4; ++dot_component) {
                  manual_position[component] += wzxy[dot_component] * constant_wzxy[dot_component];
                }
              }
              std::fprintf(stderr,
                           "[metal] bad-vs manual#%u fetch=%u v=%u src=(%.4g %.4g %.4g) "
                           "uv=(%.4g %.4g) pos=(%.4g %.4g %.4g %.4g)\n",
                           bad_shader_fetch_index, fetch_index, vertex, x, y, z, u, v,
                           manual_position[0], manual_position[1], manual_position[2],
                           manual_position[3]);
            }
          }
        }
        std::fflush(stderr);
      }
    }
    bool using_position_register_snapshot = last_position_registers_valid_ &&
                                            cpu_vertex_shader == last_position_vertex_shader_ &&
                                            vertex_shader && vertex_shader != cpu_vertex_shader;
    RegisterFile position_register_file = *register_file_;
    if (using_position_register_snapshot) {
      std::memcpy(position_register_file.values, last_position_registers_.data(),
                  sizeof(position_register_file.values));
    }
    reg::RB_DEPTHCONTROL normalized_depth_control =
        draw_util::GetNormalizedDepthControl(position_register_file);
    draw_util::ViewportInfo viewport_info = {};
    draw_util::GetHostViewportInfo(position_register_file, 1, 1, false, fallback_output_width_,
                                   fallback_output_height_, true, normalized_depth_control, false,
                                   false, pixel_shader && pixel_shader->writes_depth(),
                                   viewport_info);
    auto pa_cl_vte_cntl = position_register_file.Get<reg::PA_CL_VTE_CNTL>();
    uint32_t available_host_vertices =
        MetalDrawRenderer::kMaxHostVerticesPerFrame - uint32_t(pending_host_vertices_.size());
    uint32_t source_vertex_limit = 0;
    uint32_t max_output_vertices = 0;
    if (prim_type == xenos::PrimitiveType::kTriangleList) {
      source_vertex_limit = std::min(index_count - (index_count % 3), available_host_vertices);
      max_output_vertices = source_vertex_limit;
    } else if (prim_type == xenos::PrimitiveType::kTriangleStrip ||
               prim_type == xenos::PrimitiveType::kTriangleFan) {
      uint32_t triangle_count = index_count >= 3 ? index_count - 2 : 0;
      triangle_count = std::min(triangle_count, available_host_vertices / 3);
      source_vertex_limit = triangle_count ? triangle_count + 2 : 0;
      max_output_vertices = triangle_count * 3;
    } else if (prim_type == xenos::PrimitiveType::kRectangleList) {
      uint32_t rectangle_count = index_count / 3;
      rectangle_count = std::min(rectangle_count, available_host_vertices / 6);
      source_vertex_limit = rectangle_count * 3;
      max_output_vertices = rectangle_count * 6;
    }

    auto vgt_draw_initiator = register_file_->Get<reg::VGT_DRAW_INITIATOR>();
    uint32_t index_offset = register_file_->Get<reg::VGT_INDX_OFFSET>().indx_offset;
    uint32_t min_index = register_file_->Get<reg::VGT_MIN_VTX_INDX>().min_indx;
    uint32_t max_index = register_file_->Get<reg::VGT_MAX_VTX_INDX>().max_indx;
    auto read_vertex_index = [&](uint32_t host_vertex_index) -> uint32_t {
      uint32_t vertex_index = 0;
      if (!index_buffer_info) {
        vertex_index = host_vertex_index & xenos::kVertexIndexMask;
      } else {
        size_t index_size = index_buffer_info->format == xenos::IndexFormat::kInt16
                                ? sizeof(uint16_t)
                                : sizeof(uint32_t);
        size_t offset = size_t(host_vertex_index) * index_size;
        if (offset + index_size <= index_buffer_info->length) {
          const uint8_t* index_data = memory_->TranslatePhysical<const uint8_t*>(
              index_buffer_info->guest_base + uint32_t(offset));
          if (index_buffer_info->format == xenos::IndexFormat::kInt16) {
            uint16_t raw_index;
            std::memcpy(&raw_index, index_data, sizeof(raw_index));
            vertex_index =
                xenos::GpuSwap(raw_index, index_buffer_info->endianness) & xenos::kVertexIndexMask;
          } else {
            uint32_t raw_index;
            std::memcpy(&raw_index, index_data, sizeof(raw_index));
            vertex_index =
                xenos::GpuSwap(raw_index, index_buffer_info->endianness) & xenos::kVertexIndexMask;
          }
        }
      }
      if (vgt_draw_initiator.source_select == xenos::SourceSelect::kAutoIndex ||
          vgt_draw_initiator.source_select == xenos::SourceSelect::kDMA) {
        vertex_index =
            std::min(max_index, std::max(min_index, (vertex_index + index_offset) & 0xFFFFFF));
      }
      return vertex_index;
    };

    if (source_vertex_limit && max_output_vertices) {
      ShaderInterpreter interpreter(position_register_file, *memory_);
      interpreter.SetShader(*cpu_vertex_shader);
      interpreter.SetTextureFetchCallback(MetalCommandProcessor::InterpreterTextureFetchThunk,
                                          this);
      std::unique_ptr<ShaderInterpreter> interpolator_interpreter;
      MetalShader* interpolator_vertex_shader = nullptr;
      if (vertex_shader && vertex_shader != cpu_vertex_shader &&
          ShaderInterpreter::CanInterpretShader(*vertex_shader)) {
        interpolator_vertex_shader = vertex_shader;
        interpolator_interpreter = std::make_unique<ShaderInterpreter>(*register_file_, *memory_);
        interpolator_interpreter->SetShader(*interpolator_vertex_shader);
        interpolator_interpreter->SetTextureFetchCallback(
            MetalCommandProcessor::InterpreterTextureFetchThunk, this);
        static std::atomic<uint32_t> dual_vertex_interpreter_logs{0};
        uint32_t dual_vertex_log_index =
            dual_vertex_interpreter_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (dual_vertex_log_index <= 16 || (dual_vertex_log_index & 0xFF) == 0) {
          std::fprintf(
              stderr,
              "[metal] CPU fallback dual VS draw=%u position=%016llx "
              "interpolator=%016llx\n",
              metal_draw_index,
              static_cast<unsigned long long>(cpu_vertex_shader->ucode_data_hash()),
              static_cast<unsigned long long>(interpolator_vertex_shader->ucode_data_hash()));
          std::fflush(stderr);
        }
      }
      float color_r =
          void_register_color_draw
              ? 0.0f
              : 0.25f + 0.70f * HashColorComponent(cpu_vertex_shader->ucode_data_hash(), 0);
      float color_g =
          void_register_color_draw
              ? 0.0f
              : 0.25f + 0.70f * HashColorComponent(
                                    pixel_shader ? pixel_shader->ucode_data_hash() : 0, 16);
      float color_b =
          void_register_color_draw
              ? 0.0f
              : 0.35f + 0.55f * HashColorComponent(cpu_vertex_shader->ucode_data_hash(), 32);
      float pixel_shader_color[4] = {color_r, color_g, color_b,
                                     void_register_color_draw ? 0.0f : 0.72f};
      bool has_pixel_shader_color = false;
      if (pixel_shader && ShaderInterpreter::CanInterpretShader(*pixel_shader)) {
        ShaderInterpreter pixel_interpreter(*register_file_, *memory_);
        pixel_interpreter.SetShader(*pixel_shader);
        pixel_interpreter.SetTextureFetchCallback(
            MetalCommandProcessor::InterpreterTextureFetchThunk, this);
        std::fill(pixel_interpreter.temp_registers(),
                  pixel_interpreter.temp_registers() + xenos::kMaxShaderTempRegisters * 4, 0.0f);
        PixelExportSink pixel_export_sink;
        pixel_interpreter.SetExportSink(&pixel_export_sink);
        if (pixel_interpreter.ExecuteWithInstructionBudget(kCpuVertexShaderInstructionBudget) &&
            pixel_export_sink.has_color0) {
          bool finite_color = true;
          for (uint32_t c = 0; c < 4; ++c) {
            finite_color &= std::isfinite(pixel_export_sink.color0[c]);
          }
          float candidate_pixel_shader_color[4];
          for (uint32_t c = 0; c < 4; ++c) {
            candidate_pixel_shader_color[c] = std::clamp(pixel_export_sink.color0[c], 0.0f, 1.0f);
          }
          if (finite_color && HasVisibleRgb(candidate_pixel_shader_color)) {
            for (uint32_t c = 0; c < 4; ++c) {
              pixel_shader_color[c] = candidate_pixel_shader_color[c];
            }
            has_pixel_shader_color = true;
            static std::atomic<uint32_t> pixel_color_logs{0};
            uint32_t pixel_color_index =
                pixel_color_logs.fetch_add(1, std::memory_order_relaxed) + 1;
            if (pixel_color_index <= 16 || (pixel_color_index & 0xFF) == 0) {
              std::fprintf(stderr,
                           "[metal] CPU fallback PS color#%u draw=%u ps=%016llx "
                           "rgba=(%.4g,%.4g,%.4g,%.4g)\n",
                           pixel_color_index, metal_draw_index,
                           static_cast<unsigned long long>(pixel_shader->ucode_data_hash()),
                           pixel_shader_color[0], pixel_shader_color[1], pixel_shader_color[2],
                           pixel_shader_color[3]);
              std::fflush(stderr);
            }
          }
        }
      }
      std::vector<InterpretedVertex> interpreted_vertices;
      interpreted_vertices.reserve(source_vertex_limit);
      float raw_min[4] = {INFINITY, INFINITY, INFINITY, INFINITY};
      float raw_max[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
      float host_min[4] = {INFINITY, INFINITY, INFINITY, INFINITY};
      float host_max[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
      float interp0_min[4] = {INFINITY, INFINITY, INFINITY, INFINITY};
      float interp0_max[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
      uint32_t interp0_vertex_count = 0;
      uint32_t budget_failed_vertices = 0;
      uint32_t no_position_vertices = 0;
      uint32_t invalid_position_vertices = 0;
      uint32_t transform_failed_vertices = 0;
      bool sanitize_first_panel_screen_rect =
          cpu_vertex_shader->ucode_data_hash() == UINT64_C(0x0a6d1dd7767fdf27) &&
          prim_type == xenos::PrimitiveType::kRectangleList;
      for (uint32_t i = 0; i < source_vertex_limit; ++i) {
        std::fill(interpreter.temp_registers(),
                  interpreter.temp_registers() + xenos::kMaxShaderTempRegisters * 4, 0.0f);
        uint32_t guest_vertex_index = read_vertex_index(i);
        interpreter.temp_registers()[0] = float(guest_vertex_index);
        VertexExportSink export_sink;
        interpreter.SetExportSink(&export_sink);
        if (!interpreter.ExecuteWithInstructionBudget(kCpuVertexShaderInstructionBudget)) {
          ++budget_failed_vertices;
          static std::atomic<uint32_t> interpreter_budget_logs{0};
          uint32_t budget_log_index =
              interpreter_budget_logs.fetch_add(1, std::memory_order_relaxed) + 1;
          if (budget_log_index <= 8 || (budget_log_index & 0xFF) == 0) {
            std::fprintf(stderr,
                         "[metal] interpreted vertex shader budget exhausted "
                         "(draw=%u vertex=%u budget=%u shader=%016llx)\n",
                         metal_draw_index, i, kCpuVertexShaderInstructionBudget,
                         static_cast<unsigned long long>(cpu_vertex_shader->ucode_data_hash()));
            std::fflush(stderr);
          }
          break;
        }
        if (cpu_vertex_shader->ucode_data_hash() == UINT64_C(0x3a1fe1560cf25ff6) && i < 12) {
          static std::atomic<uint32_t> bad_shader_interpreter_logs{0};
          uint32_t bad_shader_interpreter_index =
              bad_shader_interpreter_logs.fetch_add(1, std::memory_order_relaxed) + 1;
          if (bad_shader_interpreter_index <= 48) {
            const float* r0 = interpreter.temp_registers();
            const float* r1 = interpreter.temp_registers() + 4;
            std::fprintf(stderr,
                         "[metal] bad-vs interp#%u draw=%u i=%u guest=%u has_pos=%u "
                         "pos=(%.4g %.4g %.4g %.4g) r0=(%.4g %.4g %.4g %.4g) "
                         "r1=(%.4g %.4g %.4g %.4g)\n",
                         bad_shader_interpreter_index, metal_draw_index, i, guest_vertex_index,
                         export_sink.has_position ? 1u : 0u, export_sink.position[0],
                         export_sink.position[1], export_sink.position[2], export_sink.position[3],
                         r0[0], r0[1], r0[2], r0[3], r1[0], r1[1], r1[2], r1[3]);
            std::fflush(stderr);
          }
        }
        if (cpu_vertex_shader->ucode_data_hash() == UINT64_C(0x0a6d1dd7767fdf27) && i < 12) {
          static std::atomic<uint32_t> first_panel_interpreter_logs{0};
          uint32_t first_panel_interpreter_index =
              first_panel_interpreter_logs.fetch_add(1, std::memory_order_relaxed) + 1;
          if (first_panel_interpreter_index <= 48) {
            const float* r0 = interpreter.temp_registers();
            const float* r1 = interpreter.temp_registers() + 4;
            std::fprintf(stderr,
                         "[metal] first-panel-vs interp#%u draw=%u i=%u guest=%u has_pos=%u "
                         "has_i0=%u pos=(%.4g %.4g %.4g %.4g) "
                         "i0=(%.4g %.4g %.4g %.4g) r0=(%.4g %.4g %.4g %.4g) "
                         "r1=(%.4g %.4g %.4g %.4g)\n",
                         first_panel_interpreter_index, metal_draw_index, i, guest_vertex_index,
                         export_sink.has_position ? 1u : 0u,
                         export_sink.has_interpolator0 ? 1u : 0u, export_sink.position[0],
                         export_sink.position[1], export_sink.position[2], export_sink.position[3],
                         export_sink.interpolator0[0], export_sink.interpolator0[1],
                         export_sink.interpolator0[2], export_sink.interpolator0[3], r0[0], r0[1],
                         r0[2], r0[3], r1[0], r1[1], r1[2], r1[3]);
            std::fflush(stderr);
          }
        }
        if (!export_sink.has_position) {
          ++no_position_vertices;
          continue;
        }
        if (sanitize_first_panel_screen_rect) {
          for (uint32_t c = 0; c < 3; ++c) {
            if (!std::isfinite(export_sink.position[c])) {
              export_sink.position[c] = 0.0f;
            }
          }
          if (!std::isfinite(export_sink.position[3]) ||
              std::abs(export_sink.position[3]) < 1.0e-6f) {
            export_sink.position[3] = 1.0f;
          }
        }
        if (!std::isfinite(export_sink.position[0]) || !std::isfinite(export_sink.position[1]) ||
            !std::isfinite(export_sink.position[2]) || !std::isfinite(export_sink.position[3]) ||
            std::abs(export_sink.position[3]) < 1.0e-6f) {
          ++invalid_position_vertices;
          continue;
        }
        InterpretedVertex interpreted_vertex;
        std::memcpy(interpreted_vertex.guest_position, export_sink.position,
                    sizeof(interpreted_vertex.guest_position));
        VertexExportSink interpolator_export_sink;
        const VertexExportSink* interpolator_sink = &export_sink;
        if (interpolator_interpreter) {
          std::fill(interpolator_interpreter->temp_registers(),
                    interpolator_interpreter->temp_registers() + xenos::kMaxShaderTempRegisters * 4,
                    0.0f);
          interpolator_interpreter->temp_registers()[0] = float(guest_vertex_index);
          interpolator_interpreter->SetExportSink(&interpolator_export_sink);
          if (interpolator_interpreter->ExecuteWithInstructionBudget(
                  kCpuVertexShaderInstructionBudget) &&
              HasAnyInterpolator(interpolator_export_sink)) {
            interpolator_sink = &interpolator_export_sink;
          }
        }
        interpreted_vertex.has_interpolator = interpolator_sink->has_interpolator;
        interpreted_vertex.interpolators = interpolator_sink->interpolators;
        if (interpolator_sink->has_interpolator0) {
          ++interp0_vertex_count;
          for (uint32_t c = 0; c < 4; ++c) {
            interp0_min[c] = std::min(interp0_min[c], interpolator_sink->interpolator0[c]);
            interp0_max[c] = std::max(interp0_max[c], interpolator_sink->interpolator0[c]);
          }
        }
        for (uint32_t interpolator_index = 0;
             interpolator_index < VertexExportSink::kInterpolatorCount; ++interpolator_index) {
          if (!interpolator_sink->has_interpolator[interpolator_index]) {
            continue;
          }
          const auto& interpolator = interpolator_sink->interpolators[interpolator_index];
          if (LooksLikeNormalizedColor(interpolator.data()) && HasVisibleRgb(interpolator.data())) {
            std::memcpy(interpreted_vertex.color, interpolator.data(),
                        sizeof(interpreted_vertex.color));
            interpreted_vertex.has_color = true;
            break;
          }
        }
        if (!TransformGuestPositionForHost(export_sink.position, pa_cl_vte_cntl, viewport_info,
                                           interpreted_vertex.position)) {
          ++transform_failed_vertices;
          continue;
        }
        for (uint32_t c = 0; c < 4; ++c) {
          raw_min[c] = std::min(raw_min[c], interpreted_vertex.guest_position[c]);
          raw_max[c] = std::max(raw_max[c], interpreted_vertex.guest_position[c]);
          host_min[c] = std::min(host_min[c], interpreted_vertex.position[c]);
          host_max[c] = std::max(host_max[c], interpreted_vertex.position[c]);
        }
        interpreted_vertices.push_back(interpreted_vertex);
      }

      bool current_draw_needs_texture_coordinates =
          (current_draw_has_host_texture || current_draw_has_shader_texture) &&
          current_draw_texture_width && current_draw_texture_height;
      if (current_draw_needs_texture_coordinates && interpreted_vertices.size() > 1) {
        uint32_t best_texcoord_interpolator = VertexExportSink::kInterpolatorCount;
        float best_texcoord_score = 0.0f;
        float best_texcoord_min[2] = {0.0f, 0.0f};
        float best_texcoord_max[2] = {0.0f, 0.0f};
        uint32_t degenerate_texcoord_interpolator = VertexExportSink::kInterpolatorCount;
        float degenerate_texcoord_min[2] = {0.0f, 0.0f};
        float degenerate_texcoord_max[2] = {0.0f, 0.0f};

        for (uint32_t interpolator_index = 0;
             interpolator_index < VertexExportSink::kInterpolatorCount; ++interpolator_index) {
          float texcoord_min[2] = {INFINITY, INFINITY};
          float texcoord_max[2] = {-INFINITY, -INFINITY};
          bool all_vertices_have_texcoord = true;
          bool all_texcoords_in_unit_range = true;
          for (const InterpretedVertex& interpreted_vertex : interpreted_vertices) {
            if (!interpreted_vertex.has_interpolator[interpolator_index]) {
              all_vertices_have_texcoord = false;
              break;
            }
            const auto& interpolator = interpreted_vertex.interpolators[interpolator_index];
            if (!std::isfinite(interpolator[0]) || !std::isfinite(interpolator[1])) {
              all_vertices_have_texcoord = false;
              break;
            }
            all_texcoords_in_unit_range &= interpolator[0] >= -0.001f &&
                                           interpolator[0] <= 1.001f &&
                                           interpolator[1] >= -0.001f && interpolator[1] <= 1.001f;
            for (uint32_t c = 0; c < 2; ++c) {
              texcoord_min[c] = std::min(texcoord_min[c], interpolator[c]);
              texcoord_max[c] = std::max(texcoord_max[c], interpolator[c]);
            }
          }
          if (!all_vertices_have_texcoord || !all_texcoords_in_unit_range) {
            continue;
          }
          float texcoord_span_x = texcoord_max[0] - texcoord_min[0];
          float texcoord_span_y = texcoord_max[1] - texcoord_min[1];
          float texcoord_score = texcoord_span_x + texcoord_span_y;
          if (texcoord_score < 1.0e-5f) {
            if (degenerate_texcoord_interpolator == VertexExportSink::kInterpolatorCount) {
              degenerate_texcoord_interpolator = interpolator_index;
              degenerate_texcoord_min[0] = texcoord_min[0];
              degenerate_texcoord_min[1] = texcoord_min[1];
              degenerate_texcoord_max[0] = texcoord_max[0];
              degenerate_texcoord_max[1] = texcoord_max[1];
            }
            continue;
          }
          if (texcoord_score > best_texcoord_score) {
            best_texcoord_interpolator = interpolator_index;
            best_texcoord_score = texcoord_score;
            best_texcoord_min[0] = texcoord_min[0];
            best_texcoord_min[1] = texcoord_min[1];
            best_texcoord_max[0] = texcoord_max[0];
            best_texcoord_max[1] = texcoord_max[1];
          }
        }

        if (best_texcoord_interpolator != VertexExportSink::kInterpolatorCount) {
          for (InterpretedVertex& interpreted_vertex : interpreted_vertices) {
            const auto& interpolator = interpreted_vertex.interpolators[best_texcoord_interpolator];
            interpreted_vertex.texcoord[0] = interpolator[0];
            interpreted_vertex.texcoord[1] = interpolator[1];
            interpreted_vertex.has_texcoord = true;
          }
          static std::atomic<uint32_t> texcoord_slot_logs{0};
          uint32_t texcoord_slot_log_index =
              texcoord_slot_logs.fetch_add(1, std::memory_order_relaxed) + 1;
          if (texcoord_slot_log_index <= 16 || (texcoord_slot_log_index & 0xFF) == 0) {
            std::fprintf(stderr,
                         "[metal] selected interpreted texcoord slot#%u draw=%u slot=%u "
                         "uv=(%.4g..%.4g,%.4g..%.4g) score=%.4g\n",
                         texcoord_slot_log_index, metal_draw_index, best_texcoord_interpolator,
                         best_texcoord_min[0], best_texcoord_max[0], best_texcoord_min[1],
                         best_texcoord_max[1], best_texcoord_score);
            std::fflush(stderr);
          }
        } else if (degenerate_texcoord_interpolator != VertexExportSink::kInterpolatorCount &&
                   (raw_max[0] - raw_min[0] > 2.0f || raw_max[1] - raw_min[1] > 2.0f)) {
          static std::atomic<uint32_t> degenerate_texcoord_logs{0};
          uint32_t degenerate_texcoord_index =
              degenerate_texcoord_logs.fetch_add(1, std::memory_order_relaxed) + 1;
          if (degenerate_texcoord_index <= 16 || (degenerate_texcoord_index & 0xFF) == 0) {
            std::fprintf(
                stderr,
                "[metal] ignored degenerate interpreted texcoords#%u draw=%u "
                "slot=%u uv=(%.4g..%.4g,%.4g..%.4g) "
                "raw_xy=(%.4g..%.4g,%.4g..%.4g)\n",
                degenerate_texcoord_index, metal_draw_index, degenerate_texcoord_interpolator,
                degenerate_texcoord_min[0], degenerate_texcoord_max[0], degenerate_texcoord_min[1],
                degenerate_texcoord_max[1], raw_min[0], raw_max[0], raw_min[1], raw_max[1]);
            std::fflush(stderr);
          }
        }
      }

      auto append_host_vertex = [&](const InterpretedVertex& interpreted_vertex) {
        if (pending_host_vertices_.size() >= MetalDrawRenderer::kMaxHostVerticesPerFrame) {
          return false;
        }
        MetalHostVertex& host_vertex = pending_host_vertices_.emplace_back();
        host_vertex.x = interpreted_vertex.position[0];
        host_vertex.y = -interpreted_vertex.position[1];
        host_vertex.z = interpreted_vertex.position[2];
        host_vertex.w = interpreted_vertex.position[3];
        host_vertex.r = interpreted_vertex.has_color ? interpreted_vertex.color[0] : color_r;
        host_vertex.g =
            interpreted_vertex.has_color ? interpreted_vertex.color[1] : pixel_shader_color[1];
        host_vertex.b =
            interpreted_vertex.has_color ? interpreted_vertex.color[2] : pixel_shader_color[2];
        host_vertex.a =
            interpreted_vertex.has_color ? interpreted_vertex.color[3] : pixel_shader_color[3];
        if (!interpreted_vertex.has_color && has_pixel_shader_color) {
          host_vertex.r = pixel_shader_color[0];
        }
        host_vertex.viewport_x = float(viewport_info.xy_offset[0]);
        host_vertex.viewport_y = float(viewport_info.xy_offset[1]);
        host_vertex.viewport_width = float(std::max<uint32_t>(viewport_info.xy_extent[0], 1));
        host_vertex.viewport_height = float(std::max<uint32_t>(viewport_info.xy_extent[1], 1));
        if (current_draw_needs_texture_coordinates) {
          if (interpreted_vertex.has_texcoord && interpreted_vertex.texcoord[0] >= -0.001f &&
              interpreted_vertex.texcoord[0] <= 1.001f &&
              interpreted_vertex.texcoord[1] >= -0.001f &&
              interpreted_vertex.texcoord[1] <= 1.001f) {
            host_vertex.u = std::clamp(interpreted_vertex.texcoord[0], 0.0f, 1.0f);
            host_vertex.v = std::clamp(interpreted_vertex.texcoord[1], 0.0f, 1.0f);
          } else {
            host_vertex.u = std::clamp((interpreted_vertex.guest_position[0] + 0.5f +
                                        float(current_draw_texture_resolve_x_offset)) /
                                           float(std::max(current_draw_texture_width, UINT32_C(1))),
                                       0.0f, 1.0f);
            host_vertex.v =
                std::clamp((interpreted_vertex.guest_position[1] + 0.5f +
                            float(current_draw_texture_resolve_y_offset)) /
                               float(std::max(current_draw_texture_height, UINT32_C(1))),
                           0.0f, 1.0f);
          }
          host_vertex.texture_weight = 1.0f;
        }
        host_vertex.interpolator_mask = 0;
        bool suppress_texture_interpolators =
            current_draw_needs_texture_coordinates && !interpreted_vertex.has_texcoord;
        for (uint32_t interpolator_index = 0;
             interpolator_index < MetalHostVertex::kInterpolatorCount; ++interpolator_index) {
          if (!suppress_texture_interpolators &&
              interpolator_index < interpreted_vertex.has_interpolator.size() &&
              interpreted_vertex.has_interpolator[interpolator_index]) {
            host_vertex.interpolator_mask |= UINT32_C(1) << interpolator_index;
            host_vertex.interpolators[interpolator_index] =
                interpreted_vertex.interpolators[interpolator_index];
          }
        }
        if (vertex_shader && vertex_shader->ucode_data_hash() == UINT64_C(0x0a6d1dd7767fdf27) &&
            pixel_shader && pixel_shader->ucode_data_hash() == UINT64_C(0x2e372ea28cc404b7)) {
          static std::atomic<uint32_t> interpreted_0a6d_logs{0};
          uint32_t interpreted_0a6d_index =
              interpreted_0a6d_logs.fetch_add(1, std::memory_order_relaxed) + 1;
          if (interpreted_0a6d_index <= 24 || (interpreted_0a6d_index & 0x3F) == 0) {
            const auto& interp0 = interpreted_vertex.interpolators[0];
            std::fprintf(stderr,
                         "[metal] interpreted 0a6d vertex#%u draw=%u pos=(%.4g %.4g %.4g %.4g) "
                         "guest=(%.4g %.4g %.4g %.4g) color=%u rgba=(%.4g %.4g %.4g %.4g) "
                         "tex=%u uv=(%.4g %.4g) i0=%u host_mask=0x%04x "
                         "i0=(%.4g %.4g %.4g %.4g)\n",
                         interpreted_0a6d_index, metal_draw_index, host_vertex.x, host_vertex.y,
                         host_vertex.z, host_vertex.w, interpreted_vertex.guest_position[0],
                         interpreted_vertex.guest_position[1], interpreted_vertex.guest_position[2],
                         interpreted_vertex.guest_position[3],
                         interpreted_vertex.has_color ? 1u : 0u, host_vertex.r, host_vertex.g,
                         host_vertex.b, host_vertex.a, interpreted_vertex.has_texcoord ? 1u : 0u,
                         interpreted_vertex.texcoord[0], interpreted_vertex.texcoord[1],
                         (interpreted_vertex.has_interpolator.size() > 0 &&
                          interpreted_vertex.has_interpolator[0])
                             ? 1u
                             : 0u,
                         host_vertex.interpolator_mask, interp0[0], interp0[1], interp0[2],
                         interp0[3]);
            std::fflush(stderr);
          }
        }
        return true;
      };

      size_t host_pixel_vertex_start = pending_host_vertices_.size();
      uint32_t appended_vertices = 0;
      bool triangle_list_is_three_vertex_rectangle = false;
      if (prim_type == xenos::PrimitiveType::kTriangleList && interpreted_vertices.size() >= 3 &&
          current_draw_needs_texture_coordinates) {
        const InterpretedVertex& v0 = interpreted_vertices[0];
        const InterpretedVertex& v1 = interpreted_vertices[1];
        const InterpretedVertex& v2 = interpreted_vertices[2];
        float first_three_min_x =
            std::min({v0.guest_position[0], v1.guest_position[0], v2.guest_position[0]});
        float first_three_max_x =
            std::max({v0.guest_position[0], v1.guest_position[0], v2.guest_position[0]});
        float first_three_min_y =
            std::min({v0.guest_position[1], v1.guest_position[1], v2.guest_position[1]});
        float first_three_max_y =
            std::max({v0.guest_position[1], v1.guest_position[1], v2.guest_position[1]});
        bool has_screen_rect_span = first_three_max_x - first_three_min_x > 2.0f &&
                                    first_three_max_y - first_three_min_y > 2.0f;
        bool trailing_vertices_are_degenerate = true;
        for (size_t i = 3; i < interpreted_vertices.size(); ++i) {
          const InterpretedVertex& trailing_vertex = interpreted_vertices[i];
          bool near_origin = std::abs(trailing_vertex.guest_position[0]) < 1.0e-5f &&
                             std::abs(trailing_vertex.guest_position[1]) < 1.0e-5f &&
                             std::abs(trailing_vertex.guest_position[2]) < 1.0e-5f;
          trailing_vertices_are_degenerate &= near_origin;
        }
        triangle_list_is_three_vertex_rectangle =
            has_screen_rect_span && trailing_vertices_are_degenerate;
      }
      if (triangle_list_is_three_vertex_rectangle) {
        std::array<InterpretedVertex, 4> ordered_rectangle_vertices = OrderRectangleVertices(
            interpreted_vertices[0], interpreted_vertices[1], interpreted_vertices[2]);
        const InterpretedVertex* rectangle_triangles[6] = {
            &ordered_rectangle_vertices[0], &ordered_rectangle_vertices[1],
            &ordered_rectangle_vertices[2], &ordered_rectangle_vertices[2],
            &ordered_rectangle_vertices[1], &ordered_rectangle_vertices[3]};
        for (const InterpretedVertex* rectangle_vertex : rectangle_triangles) {
          if (append_host_vertex(*rectangle_vertex)) {
            ++appended_vertices;
          }
        }
        static std::atomic<uint32_t> triangle_rect_logs{0};
        uint32_t triangle_rect_index =
            triangle_rect_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (triangle_rect_index <= 16 || (triangle_rect_index & 0xFF) == 0) {
          std::fprintf(stderr,
                       "[metal] expanded triangle-list rectangle#%u draw=%u vertices=%zu "
                       "span=(%.4g..%.4g,%.4g..%.4g)\n",
                       triangle_rect_index, metal_draw_index, interpreted_vertices.size(),
                       raw_min[0], raw_max[0], raw_min[1], raw_max[1]);
          std::fflush(stderr);
        }
      } else if (prim_type == xenos::PrimitiveType::kTriangleList) {
        uint32_t triangle_vertex_count = uint32_t(interpreted_vertices.size());
        triangle_vertex_count -= triangle_vertex_count % 3;
        for (uint32_t i = 0; i < triangle_vertex_count; ++i) {
          if (append_host_vertex(interpreted_vertices[i])) {
            ++appended_vertices;
          }
        }
      } else if (prim_type == xenos::PrimitiveType::kTriangleStrip) {
        for (uint32_t i = 0; i + 2 < interpreted_vertices.size(); ++i) {
          uint32_t triangle_indices[3] = {
              (i & 1) ? i + 1 : i,
              (i & 1) ? i : i + 1,
              i + 2,
          };
          for (uint32_t triangle_index : triangle_indices) {
            if (append_host_vertex(interpreted_vertices[triangle_index])) {
              ++appended_vertices;
            }
          }
        }
      } else if (prim_type == xenos::PrimitiveType::kTriangleFan) {
        for (uint32_t i = 1; i + 1 < interpreted_vertices.size(); ++i) {
          uint32_t triangle_indices[3] = {0, i, i + 1};
          for (uint32_t triangle_index : triangle_indices) {
            if (append_host_vertex(interpreted_vertices[triangle_index])) {
              ++appended_vertices;
            }
          }
        }
      } else if (prim_type == xenos::PrimitiveType::kRectangleList) {
        uint32_t rectangle_vertex_count = uint32_t(interpreted_vertices.size());
        rectangle_vertex_count -= rectangle_vertex_count % 3;
        for (uint32_t i = 0; i + 2 < rectangle_vertex_count; i += 3) {
          std::array<InterpretedVertex, 4> ordered_rectangle_vertices = OrderRectangleVertices(
              interpreted_vertices[i], interpreted_vertices[i + 1], interpreted_vertices[i + 2]);
          const InterpretedVertex* rectangle_triangles[6] = {
              &ordered_rectangle_vertices[0], &ordered_rectangle_vertices[1],
              &ordered_rectangle_vertices[2], &ordered_rectangle_vertices[2],
              &ordered_rectangle_vertices[1], &ordered_rectangle_vertices[3]};
          for (const InterpretedVertex* rectangle_vertex : rectangle_triangles) {
            if (append_host_vertex(*rectangle_vertex)) {
              ++appended_vertices;
            }
          }
        }
      }
      static std::atomic<uint32_t> interpreted_draw_logs{0};
      uint32_t interpreted_draw_index =
          interpreted_draw_logs.fetch_add(appended_vertices ? 1 : 0, std::memory_order_relaxed) + 1;
      bool force_interpreted_draw_log =
          cpu_vertex_shader->ucode_data_hash() == UINT64_C(0x3a1fe1560cf25ff6);
      if (appended_vertices && (interpreted_draw_index <= 32 || force_interpreted_draw_log)) {
        std::fprintf(
            stderr,
            "[metal] interpreted draw#%u prim=%u vertices=%u total_host_vertices=%zu "
            "viewport=%u,%u %ux%u ndc_scale=%.4g,%.4g,%.4g "
            "ndc_offset=%.4g,%.4g,%.4g raw_xy=(%.4g..%.4g,%.4g..%.4g) "
            "host_xy=(%.4g..%.4g,%.4g..%.4g) "
            "i0_count=%u i0=(%.4g..%.4g,%.4g..%.4g,%.4g..%.4g,%.4g..%.4g)\n",
            interpreted_draw_index, uint32_t(prim_type), appended_vertices,
            pending_host_vertices_.size(), viewport_info.xy_offset[0], viewport_info.xy_offset[1],
            viewport_info.xy_extent[0], viewport_info.xy_extent[1], viewport_info.ndc_scale[0],
            viewport_info.ndc_scale[1], viewport_info.ndc_scale[2], viewport_info.ndc_offset[0],
            viewport_info.ndc_offset[1], viewport_info.ndc_offset[2], raw_min[0], raw_max[0],
            raw_min[1], raw_max[1], host_min[0], host_max[0], host_min[1], host_max[1],
            interp0_vertex_count, interp0_min[0], interp0_max[0], interp0_min[1], interp0_max[1],
            interp0_min[2], interp0_max[2], interp0_min[3], interp0_max[3]);
        std::fflush(stderr);
      }
      bool host_rt_cpu_probe_allowed = pixel_shader && IsHostPixelProbeAllowed(*pixel_shader);
      bool trace_host_rt_cpu_gate =
          pixel_shader && (pixel_shader->ucode_data_hash() == UINT64_C(0xbdc93d3c5da8241f) ||
                           pixel_shader->ucode_data_hash() == UINT64_C(0x0f7f3153e66fa452) ||
                           pixel_shader->ucode_data_hash() == UINT64_C(0x21243b8826e3f416));
      if (trace_host_rt_cpu_gate) {
        static std::atomic<uint32_t> host_rt_cpu_gate_logs{0};
        uint32_t host_rt_cpu_gate_index =
            host_rt_cpu_gate_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (host_rt_cpu_gate_index <= 64 || (host_rt_cpu_gate_index & 0xFF) == 0) {
          std::fprintf(
              stderr,
              "[metal] host RT CPU gate#%u draw=%u ps=%016llx "
              "appended=%u enabled=%u color_mask=0x%04x ps_ok=%u allowed=%u "
              "disabled=%u pending_start=%zu pending_total=%zu\n",
              host_rt_cpu_gate_index, metal_draw_index,
              static_cast<unsigned long long>(pixel_shader->ucode_data_hash()), appended_vertices,
              host_rt_cpu_producer_enabled ? 1u : 0u, host_rt_cpu_color_mask,
              pixel_translation_ok ? 1u : 0u, host_rt_cpu_probe_allowed ? 1u : 0u,
              disabled_host_pixel_shader_hashes_.count(pixel_shader->ucode_data_hash()) ? 1u : 0u,
              host_pixel_vertex_start, pending_host_vertices_.size());
          std::fflush(stderr);
        }
      }
      if (appended_vertices && host_rt_cpu_producer_enabled && pixel_shader &&
          pixel_translation_ok && host_rt_cpu_probe_allowed) {
        reg::RB_SURFACE_INFO producer_surface_info = register_file_->Get<reg::RB_SURFACE_INFO>();
        draw_util::Scissor producer_scissor = {};
        draw_util::GetScissor(*register_file_, producer_scissor, false);
        for (uint32_t rt_index = 0; rt_index < xenos::kMaxColorRenderTargets; ++rt_index) {
          if (!((host_rt_cpu_color_mask >> (rt_index * 4)) & 0xF)) {
            continue;
          }
          reg::RB_COLOR_INFO producer_color_info = register_file_->Get<reg::RB_COLOR_INFO>(
              reg::RB_COLOR_INFO::rt_register_indices[rt_index]);
          HostRenderTarget* active_host_rt = EnsureHostRenderTarget(rt_index);
          if (!active_host_rt || !active_host_rt->context) {
            continue;
          }
          std::vector<uint8_t> rt_bgra;
          if (RenderHostPixelShader(*pixel_shader, pending_host_vertices_, host_pixel_vertex_start,
                                    appended_vertices, fallback_output_width_,
                                    fallback_output_height_, rt_bgra, active_host_rt->context,
                                    void_register_color_draw)) {
            bool rt_bgra_has_visible_rgb = BgraHasNonZeroRgb(rt_bgra);
            bool active_host_rt_has_visible_rgb = BgraHasNonZeroRgb(active_host_rt->bgra);
            bool accept_host_rt_cpu = rt_bgra_has_visible_rgb || !active_host_rt_has_visible_rgb;
            if (accept_host_rt_cpu) {
              active_host_rt->bgra = std::move(rt_bgra);
              active_host_rt->width = fallback_output_width_;
              active_host_rt->height = fallback_output_height_;
            }
            static std::atomic<uint32_t> host_rt_cpu_draw_logs{0};
            uint32_t host_rt_cpu_draw_index =
                host_rt_cpu_draw_logs.fetch_add(1, std::memory_order_relaxed) + 1;
            bool force_host_rt_cpu_draw_log =
                pixel_shader->ucode_data_hash() == UINT64_C(0xbdc93d3c5da8241f) ||
                pixel_shader->ucode_data_hash() == UINT64_C(0x0f7f3153e66fa452) ||
                pixel_shader->ucode_data_hash() == UINT64_C(0x21243b8826e3f416);
            if (host_rt_cpu_draw_index <= 32 || (host_rt_cpu_draw_index & 0xFF) == 0 ||
                force_host_rt_cpu_draw_log) {
              BgraFrameStats rt_stats = GetBgraFrameStats(active_host_rt->bgra);
              BgraBandStats rt_band_stats = GetBgraBandStats(
                  active_host_rt->bgra, active_host_rt->width, active_host_rt->height);
              std::fprintf(stderr,
                           "[metal] host RT CPU draw#%u draw=%u rt=%u "
                           "color_mask=0x%04x vertices=%u accepted=%u visible=%u range=%u "
                           "bands(top208=%u mid=%u low=%u) size=%ux%u scissor=%u,%u %ux%u "
                           "ps=%016llx\n",
                           host_rt_cpu_draw_index, metal_draw_index, rt_index,
                           host_rt_cpu_color_mask, appended_vertices, accept_host_rt_cpu ? 1u : 0u,
                           rt_stats.visible_pixels, BgraRgbRange(rt_stats),
                           rt_band_stats.top_208_visible, rt_band_stats.mid_208_512_visible,
                           rt_band_stats.low_512_visible, active_host_rt->width,
                           active_host_rt->height, producer_scissor.offset[0],
                           producer_scissor.offset[1], producer_scissor.extent[0],
                           producer_scissor.extent[1],
                           static_cast<unsigned long long>(pixel_shader->ucode_data_hash()));
              uint32_t pitch_tiles = xenos::GetSurfacePitchTiles(
                  producer_surface_info.surface_pitch, producer_surface_info.msaa_samples,
                  xenos::IsColorRenderTargetFormat64bpp(producer_color_info.color_format));
              uint32_t base_tile_row =
                  pitch_tiles ? producer_color_info.color_base / pitch_tiles : 0;
              uint32_t base_tile_column =
                  pitch_tiles ? producer_color_info.color_base % pitch_tiles : 0;
              uint32_t base_pixel_y =
                  base_tile_row *
                  (xenos::kEdramTileHeightSamples >>
                   uint32_t(producer_surface_info.msaa_samples >= xenos::MsaaSamples::k2X));
              std::fprintf(stderr,
                           "[metal] host RT producer-map#%u draw=%u rt=%u "
                           "color_base=%u color_fmt=%u surface_pitch=%u msaa=%u "
                           "pitch_tiles=%u base_tile=%u,%u base_pixel_y=%u "
                           "raw_y=%.4g..%.4g viewport=%u,%u %ux%u scissor=%u,%u %ux%u\n",
                           host_rt_cpu_draw_index, metal_draw_index, rt_index,
                           uint32_t(producer_color_info.color_base),
                           uint32_t(producer_color_info.color_format),
                           uint32_t(producer_surface_info.surface_pitch),
                           uint32_t(producer_surface_info.msaa_samples), pitch_tiles,
                           base_tile_column, base_tile_row, base_pixel_y, raw_min[1], raw_max[1],
                           viewport_info.xy_offset[0], viewport_info.xy_offset[1],
                           viewport_info.xy_extent[0], viewport_info.xy_extent[1],
                           producer_scissor.offset[0], producer_scissor.offset[1],
                           producer_scissor.extent[0], producer_scissor.extent[1]);
              std::fflush(stderr);
            }
          }
        }
      }
      if (appended_vertices && pixel_shader && pixel_translation_ok &&
          (latest_host_pixel_frame_width_ != fallback_output_width_ ||
           latest_host_pixel_frame_height_ != fallback_output_height_)) {
        latest_host_pixel_frame_bgra_.clear();
        latest_host_pixel_frame_width_ = 0;
        latest_host_pixel_frame_height_ = 0;
        latest_host_pixel_frame_draw_count_ = 0;
        latest_host_pixel_frame_from_fallback_ = false;
      }
      bool host_pixel_probe_allowed = MetalHostPixelDiagnosticsEnabled() && pixel_shader &&
                                      IsHostPixelProbeAllowed(*pixel_shader);
      bool use_host_fallback_pixel_draw =
          host_pixel_probe_allowed && pixel_shader &&
          ShouldUseHostFallbackPixelShader(pixel_shader->ucode_data_hash(), pixel_translation);
      bool force_host_pixel_draw = host_pixel_probe_allowed && pixel_shader &&
                                   pixel_shader->ucode_data_hash() == UINT64_C(0x21243b8826e3f416);
      uint32_t host_pixel_shader_draws =
          pixel_shader ? host_pixel_shader_draws_this_swap_[pixel_shader->ucode_data_hash()] : 0;
      bool normal_host_pixel_budget_ok =
          !use_host_fallback_pixel_draw &&
          host_pixel_draws_this_swap_ < kMaxHostPixelDrawsPerSwap &&
          host_pixel_shader_draws < kMaxHostPixelDrawsPerShaderPerSwap;
      bool fallback_host_pixel_budget_ok =
          use_host_fallback_pixel_draw && !latest_host_pixel_frame_width_ &&
          host_fallback_pixel_draws_this_swap_ < kMaxHostFallbackPixelDrawsPerSwap;
      bool attempted_host_pixel_draw = false;
      if (appended_vertices && pixel_shader && pixel_translation_ok && host_pixel_probe_allowed &&
          (normal_host_pixel_budget_ok || fallback_host_pixel_budget_ok || force_host_pixel_draw) &&
          (latest_host_pixel_frame_bgra_.empty() ||
           (latest_host_pixel_frame_width_ == fallback_output_width_ &&
            latest_host_pixel_frame_height_ == fallback_output_height_))) {
        if (use_host_fallback_pixel_draw) {
          ++host_fallback_pixel_draws_this_swap_;
        } else {
          if (!force_host_pixel_draw || host_pixel_draws_this_swap_ < kMaxHostPixelDrawsPerSwap) {
            ++host_pixel_draws_this_swap_;
          }
          if (!force_host_pixel_draw ||
              host_pixel_shader_draws < kMaxHostPixelDrawsPerShaderPerSwap) {
            ++host_pixel_shader_draws_this_swap_[pixel_shader->ucode_data_hash()];
          }
        }
        attempted_host_pixel_draw = true;
        std::vector<uint8_t> host_pixel_candidate_bgra;
        if (!host_pixel_probe_context_) {
          host_pixel_candidate_bgra = latest_host_pixel_frame_bgra_;
        }
        if (RenderHostPixelShader(*pixel_shader, pending_host_vertices_, host_pixel_vertex_start,
                                  appended_vertices, fallback_output_width_,
                                  fallback_output_height_, host_pixel_candidate_bgra) &&
            BgraHasNonZeroRgb(host_pixel_candidate_bgra)) {
          bool keep_candidate =
              !latest_host_pixel_frame_width_ ||
              (!use_host_fallback_pixel_draw && !latest_host_pixel_frame_from_fallback_);
          if (keep_candidate) {
            latest_host_pixel_frame_bgra_ = std::move(host_pixel_candidate_bgra);
            latest_host_pixel_frame_width_ = fallback_output_width_;
            latest_host_pixel_frame_height_ = fallback_output_height_;
            latest_host_pixel_frame_draw_count_ = draw_count_;
            latest_host_pixel_frame_from_fallback_ = use_host_fallback_pixel_draw;
          }
          static std::atomic<uint32_t> host_pixel_frame_logs{0};
          uint32_t host_pixel_frame_index =
              host_pixel_frame_logs.fetch_add(1, std::memory_order_relaxed) + 1;
          if (host_pixel_frame_index <= 16 || (host_pixel_frame_index & 0x3F) == 0) {
            BgraFrameStats host_pixel_stats = GetBgraFrameStats(
                keep_candidate ? latest_host_pixel_frame_bgra_ : host_pixel_candidate_bgra);
            std::fprintf(stderr,
                         "[metal] host pixel frame#%u draw=%u ps=%016llx "
                         "vertices=%u visible=%u kept=%u fallback=%u range=%u size=%ux%u\n",
                         host_pixel_frame_index, metal_draw_index,
                         static_cast<unsigned long long>(pixel_shader->ucode_data_hash()),
                         appended_vertices, host_pixel_stats.visible_pixels,
                         keep_candidate ? 1u : 0u, use_host_fallback_pixel_draw ? 1u : 0u,
                         BgraRgbRange(host_pixel_stats), latest_host_pixel_frame_width_,
                         latest_host_pixel_frame_height_);
            std::fflush(stderr);
          }
        }
      }
      if (MetalHostPixelDiagnosticsEnabled() && appended_vertices && pixel_shader &&
          pixel_translation_ok && !attempted_host_pixel_draw) {
        host_pixel_skipped_vertices_this_swap_ += appended_vertices;
        static std::atomic<uint32_t> host_pixel_skip_logs{0};
        uint32_t host_pixel_skip_index =
            host_pixel_skip_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (host_pixel_skip_index <= 8 || (host_pixel_skip_index & 0xFF) == 0) {
          std::fprintf(
              stderr,
              "[metal] host pixel skipped#%u draw=%u ps=%016llx "
              "vertices=%u allowed=%u global_budget=%u/%u shader_budget=%u/%u "
              "fallback_budget=%u/%u force=%u disabled=%u unsafe=%u\n",
              host_pixel_skip_index, metal_draw_index,
              static_cast<unsigned long long>(pixel_shader->ucode_data_hash()), appended_vertices,
              host_pixel_probe_allowed ? 1u : 0u, host_pixel_draws_this_swap_,
              kMaxHostPixelDrawsPerSwap, host_pixel_shader_draws,
              kMaxHostPixelDrawsPerShaderPerSwap, host_fallback_pixel_draws_this_swap_,
              kMaxHostFallbackPixelDrawsPerSwap, force_host_pixel_draw ? 1u : 0u,
              disabled_host_pixel_shader_hashes_.count(pixel_shader->ucode_data_hash()) ? 1u : 0u,
              IsKnownUnsafeHostPixelShader(pixel_shader->ucode_data_hash()) ? 1u : 0u);
          std::fflush(stderr);
        }
      }
      if (!appended_vertices) {
        static std::atomic<uint32_t> interpreted_skip_logs{0};
        uint32_t interpreted_skip_index =
            interpreted_skip_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (interpreted_skip_index <= 16 || (interpreted_skip_index & 0xFF) == 0) {
          std::fprintf(stderr,
                       "[metal] interpreted draw skipped#%u draw=%u prim=%u indices=%u "
                       "source_limit=%u interpreted=%zu budget_fail=%u no_pos=%u "
                       "bad_pos=%u transform_fail=%u shader=%016llx\n",
                       interpreted_skip_index, metal_draw_index, uint32_t(prim_type), index_count,
                       source_vertex_limit, interpreted_vertices.size(), budget_failed_vertices,
                       no_position_vertices, invalid_position_vertices, transform_failed_vertices,
                       static_cast<unsigned long long>(cpu_vertex_shader->ucode_data_hash()));
          std::fflush(stderr);
        }
      }
    }
  }

  if (vertex_shader || pixel_shader) {
    REXLOG_DEBUG(
        "Metal draw reached translated shader path: prim={} indices={} vs={} ps={} cached={} "
        "translation_ok={}/{}",
        uint32_t(prim_type), index_count,
        vertex_shader ? fmt::format("{:016X}", vertex_shader->ucode_data_hash()) : "none",
        pixel_shader ? fmt::format("{:016X}", pixel_shader->ucode_data_hash()) : "none",
        shaders_.size(), vertex_translation_ok, pixel_translation_ok);
  }
  LogIncompleteOnce("draw");
  return true;
}

bool MetalCommandProcessor::IssueCopy() {
  static std::atomic<uint32_t> metal_copy_logs{0};
  uint32_t metal_copy_index = metal_copy_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (metal_copy_index <= 16 || (metal_copy_index & 0x3F) == 0) {
    std::fprintf(stderr, "[metal] IssueCopy#%u\n", metal_copy_index);
    std::fflush(stderr);
  }
  if (register_file_) {
    last_copy_dest_base_ = register_file_->values[XE_GPU_REG_RB_COPY_DEST_BASE];
  }
  bool wrote_source_resolve = false;
  reg::RB_COPY_CONTROL active_copy_control = {};
  reg::RB_COPY_DEST_INFO active_copy_dest_info = {};
  reg::RB_COPY_DEST_PITCH active_copy_dest_pitch = {};
  bool active_copy_valid = false;
  if (register_file_) {
    active_copy_control = register_file_->Get<reg::RB_COPY_CONTROL>();
    active_copy_dest_info = register_file_->Get<reg::RB_COPY_DEST_INFO>();
    active_copy_dest_pitch = register_file_->Get<reg::RB_COPY_DEST_PITCH>();
    active_copy_valid = active_copy_control.value && active_copy_dest_info.value &&
                        active_copy_dest_pitch.copy_dest_pitch &&
                        active_copy_dest_pitch.copy_dest_height;
  }
  draw_util::ResolveInfo resolve_info = {};
  bool resolve_info_valid = false;
  if (memory_ && register_file_) {
    uint32_t resolve_scale_x = texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
    uint32_t resolve_scale_y = texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
    resolve_info_valid =
        draw_util::GetResolveInfo(*register_file_, *memory_, trace_writer_, resolve_scale_x,
                                  resolve_scale_y, false, false, resolve_info);
    draw_util::ResolveCopyShaderConstants copy_shader_constants = {};
    uint32_t copy_group_count_x = 0;
    uint32_t copy_group_count_y = 0;
    draw_util::ResolveCopyShaderIndex copy_shader = draw_util::ResolveCopyShaderIndex::kUnknown;
    const char* copy_shader_name = "none";
    if (resolve_info_valid) {
      copy_shader =
          resolve_info.GetCopyShader(resolve_scale_x, resolve_scale_y, copy_shader_constants,
                                     copy_group_count_x, copy_group_count_y);
      if (copy_shader != draw_util::ResolveCopyShaderIndex::kUnknown) {
        copy_shader_name = draw_util::resolve_copy_shader_info[size_t(copy_shader)].debug_name;
      } else {
        copy_shader_name = "unknown";
      }
    }
    bool watched_framebuffer =
        RangesOverlap(resolve_info.copy_dest_base, resolve_info.copy_dest_extent_length,
                      kWatchedFramebufferBase, kWatchedFramebufferLength);
    bool watched_resolve =
        RangesOverlap(resolve_info.copy_dest_base, resolve_info.copy_dest_extent_length,
                      kWatchedResolveBase, kWatchedResolveLength);
    bool watched_swap =
        RangesOverlap(resolve_info.copy_dest_base, resolve_info.copy_dest_extent_length,
                      kWatchedSwapBase, kWatchedSwapLength);
    std::fprintf(stderr,
                 "[metal] copy-map#%u rb_dest=0x%08x active=%u ctrl=0x%08x src=%u cmd=%u "
                 "clear(c=%u d=%u) pitch=%u height=%u resolve_valid=%u resolve_dest=0x%08x "
                 "extent=0x%08x+0x%x watched(framebuffer=%u resolve=%u swap=%u) shader=%s "
                 "groups=%ux%u\n",
                 metal_copy_index, last_copy_dest_base_, active_copy_valid ? 1u : 0u,
                 active_copy_control.value, uint32_t(active_copy_control.copy_src_select),
                 uint32_t(active_copy_control.copy_command),
                 uint32_t(active_copy_control.color_clear_enable),
                 uint32_t(active_copy_control.depth_clear_enable),
                 uint32_t(active_copy_dest_pitch.copy_dest_pitch),
                 uint32_t(active_copy_dest_pitch.copy_dest_height), resolve_info_valid ? 1u : 0u,
                 resolve_info.copy_dest_base, resolve_info.copy_dest_extent_start,
                 resolve_info.copy_dest_extent_length, watched_framebuffer ? 1u : 0u,
                 watched_resolve ? 1u : 0u, watched_swap ? 1u : 0u, copy_shader_name,
                 copy_group_count_x, copy_group_count_y);
    std::fflush(stderr);
    if (resolve_info_valid && (metal_copy_index <= 16 || (metal_copy_index & 0x3F) == 0)) {
      uint32_t edram_base = 0;
      uint32_t edram_row_length = 0;
      uint32_t edram_rows = 0;
      uint32_t edram_pitch = 0;
      resolve_info.GetCopyEdramTileSpan(edram_base, edram_row_length, edram_rows, edram_pitch);
      std::fprintf(stderr,
                   "[metal] resolve-info#%u valid=1 rect=%ux%u dest_base=0x%08x "
                   "dest_extent=0x%08x+0x%x dest_pitch=%u dest_height=%u "
                   "dest_offset=%u,%u edram(base=%u row=%u rows=%u pitch=%u) color(base=%u fmt=%u) "
                   "depth(base=%u fmt=%u) shader=%s groups=%ux%u\n",
                   metal_copy_index, resolve_info.coordinate_info.width_div_8 * 8,
                   resolve_info.height_div_8 * 8, resolve_info.copy_dest_base,
                   resolve_info.copy_dest_extent_start, resolve_info.copy_dest_extent_length,
                   uint32_t(resolve_info.copy_dest_coordinate_info.pitch_aligned_div_32) * 32,
                   uint32_t(resolve_info.copy_dest_coordinate_info.height_aligned_div_32) * 32,
                   uint32_t(resolve_info.copy_dest_coordinate_info.offset_x_div_8) * 8,
                   uint32_t(resolve_info.copy_dest_coordinate_info.offset_y_div_8) * 8, edram_base,
                   edram_row_length, edram_rows, edram_pitch, resolve_info.color_original_base,
                   uint32_t(resolve_info.color_edram_info.format), resolve_info.depth_original_base,
                   uint32_t(resolve_info.depth_edram_info.format), copy_shader_name,
                   copy_group_count_x, copy_group_count_y);
      std::fflush(stderr);
    }
  }
  auto compute_copy_rect = [&](uint32_t resolve_width, uint32_t resolve_height, uint32_t& rect_x,
                               uint32_t& rect_y, uint32_t& rect_width, uint32_t& rect_height) {
    rect_x = 0;
    rect_y = 0;
    rect_width = resolve_width;
    rect_height = resolve_height;
    if (!memory_ || !register_file_) {
      return;
    }
    xenos::xe_gpu_vertex_fetch_t copy_vertex_fetch = register_file_->GetVertexFetch(0);
    if (copy_vertex_fetch.type != xenos::FetchConstantType::kVertex || copy_vertex_fetch.size < 6) {
      return;
    }
    const float* vertices_guest = reinterpret_cast<const float*>(
        memory_->TranslatePhysical(copy_vertex_fetch.address * sizeof(uint32_t)));
    if (!vertices_guest) {
      return;
    }
    float copy_vertices[6];
    for (uint32_t i = 0; i < 6; ++i) {
      copy_vertices[i] = xenos::GpuSwap(vertices_guest[i], copy_vertex_fetch.endian);
    }
    float min_x = std::min({copy_vertices[0], copy_vertices[2], copy_vertices[4]}) + 0.5f;
    float max_x = std::max({copy_vertices[0], copy_vertices[2], copy_vertices[4]}) + 0.5f;
    float min_y = std::min({copy_vertices[1], copy_vertices[3], copy_vertices[5]}) + 0.5f;
    float max_y = std::max({copy_vertices[1], copy_vertices[3], copy_vertices[5]}) + 0.5f;
    min_x = std::clamp(min_x, 0.0f, float(resolve_width));
    max_x = std::clamp(max_x, 0.0f, float(resolve_width));
    min_y = std::clamp(min_y, 0.0f, float(resolve_height));
    max_y = std::clamp(max_y, 0.0f, float(resolve_height));
    if (max_x <= min_x || max_y <= min_y) {
      return;
    }
    rect_x = uint32_t(std::floor(min_x));
    rect_y = uint32_t(std::floor(min_y));
    uint32_t rect_x1 = uint32_t(std::ceil(max_x));
    uint32_t rect_y1 = uint32_t(std::ceil(max_y));
    rect_width = rect_x1 > rect_x ? rect_x1 - rect_x : 0;
    rect_height = rect_y1 > rect_y ? rect_y1 - rect_y : 0;
  };
  if (MetalMagentaResolveEnabled() && memory_ && register_file_ && last_copy_dest_base_ &&
      active_copy_valid &&
      active_copy_dest_info.copy_dest_format == xenos::ColorFormat::k_8_8_8_8 &&
      !active_copy_dest_info.copy_dest_array) {
    uint32_t dest_pitch = std::max<uint32_t>(active_copy_dest_pitch.copy_dest_pitch, 1);
    uint32_t magenta_dest_base = last_copy_dest_base_;
    uint32_t magenta_dest_y = 0;
    if (last_copy_dest_base_ == kWatchedResolveBase && dest_pitch) {
      uint32_t row_bytes = dest_pitch * 4;
      uint32_t framebuffer_to_resolve = kWatchedResolveBase - kWatchedFramebufferBase;
      if (!(framebuffer_to_resolve % row_bytes)) {
        magenta_dest_base = kWatchedFramebufferBase;
        magenta_dest_y = framebuffer_to_resolve / row_bytes;
      }
    }
    uint32_t surface_width = dest_pitch;
    uint32_t surface_height = std::max(
        fallback_output_height_, std::max<uint32_t>(active_copy_dest_pitch.copy_dest_height, 1));
    if (magenta_dest_y) {
      surface_height =
          std::max(surface_height, magenta_dest_y + std::max(fallback_output_height_, 1u));
    }
    if (last_swap_frontbuffer_ptr_ > magenta_dest_base && dest_pitch) {
      uint32_t row_bytes = dest_pitch * 4;
      uint32_t swap_offset = last_swap_frontbuffer_ptr_ - magenta_dest_base;
      if (row_bytes && !(swap_offset % row_bytes)) {
        uint32_t swap_offset_rows = swap_offset / row_bytes;
        surface_height =
            std::max(surface_height, swap_offset_rows + std::max(fallback_output_height_, 1u));
      }
    }
    uint32_t copy_rect_x = 0;
    uint32_t copy_rect_y = 0;
    uint32_t copy_rect_width = surface_width;
    uint32_t copy_rect_height = surface_height;
    if (copy_rect_width && copy_rect_height) {
      std::vector<uint8_t> magenta_bgra(size_t(surface_width) * surface_height * 4);
      for (size_t pixel = 0, pixel_count = size_t(surface_width) * surface_height;
           pixel < pixel_count; ++pixel) {
        uint8_t* bgra = magenta_bgra.data() + pixel * 4;
        bgra[0] = 0xFF;
        bgra[1] = 0x00;
        bgra[2] = 0xFF;
        bgra[3] = 0xFF;
      }
      if (WriteBgraToTiledResolveRegion(magenta_dest_base, dest_pitch, surface_height, magenta_bgra,
                                        surface_width, surface_height, copy_rect_x, copy_rect_y,
                                        copy_rect_x, copy_rect_y, copy_rect_width,
                                        copy_rect_height)) {
        wrote_source_resolve = true;
        static std::atomic<uint32_t> magenta_resolve_logs{0};
        uint32_t magenta_resolve_index =
            magenta_resolve_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (magenta_resolve_index <= 16 || (magenta_resolve_index & 0x3F) == 0) {
          std::fprintf(stderr,
                       "[metal] milestone-a magenta resolve#%u copy=%u base=0x%08x "
                       "canonical=0x%08x dest_y=%u surface=%ux%u rect=%u,%u %ux%u "
                       "pitch=%u resolve_info=%u\n",
                       magenta_resolve_index, metal_copy_index, last_copy_dest_base_,
                       magenta_dest_base, magenta_dest_y, surface_width, surface_height,
                       copy_rect_x, copy_rect_y, copy_rect_width, copy_rect_height, dest_pitch,
                       resolve_info_valid ? 1u : 0u);
          std::fflush(stderr);
        }
      }
    }
  }
  if (memory_ && register_file_ && last_copy_dest_base_ && active_copy_valid &&
      (active_copy_control.color_clear_enable || active_copy_control.depth_clear_enable)) {
    static std::atomic<uint32_t> copy_clear_side_effect_logs{0};
    uint32_t copy_clear_side_effect_index =
        copy_clear_side_effect_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (copy_clear_side_effect_index <= 16 || (copy_clear_side_effect_index & 0xFF) == 0) {
      std::fprintf(stderr,
                   "[metal] copy clear side effect#%u dest=0x%08x src=%u cmd=%u "
                   "clear(c=%u d=%u) color_clear=0x%08x\n",
                   copy_clear_side_effect_index, last_copy_dest_base_,
                   uint32_t(active_copy_control.copy_src_select),
                   uint32_t(active_copy_control.copy_command),
                   uint32_t(active_copy_control.color_clear_enable),
                   uint32_t(active_copy_control.depth_clear_enable),
                   register_file_->values[XE_GPU_REG_RB_COLOR_CLEAR]);
      std::fflush(stderr);
    }
  }
  HostRenderTarget* resolve_host_rt =
      active_copy_control.copy_src_select < xenos::kMaxColorRenderTargets
          ? FindHostRenderTarget(uint32_t(active_copy_control.copy_src_select))
          : nullptr;
  if (!resolve_host_rt && resolve_info_valid &&
      active_copy_control.copy_src_select < xenos::kMaxColorRenderTargets &&
      !resolve_info.IsCopyingDepth()) {
    uint32_t resolve_source_color_base =
        (resolve_info.color_original_base & (xenos::kEdramTileCount - 1));
    uint32_t resolve_min_surface_pitch = uint32_t(resolve_info.coordinate_info.width_div_8) * 8;
    resolve_host_rt = FindHostRenderTargetForResolve(
        uint32_t(active_copy_control.copy_src_select), resolve_source_color_base,
        resolve_info.color_edram_info.format, resolve_min_surface_pitch,
        resolve_info.color_edram_info.msaa_samples);
  }
  void* resolve_host_rt_context =
      resolve_host_rt && resolve_host_rt->context ? resolve_host_rt->context : nullptr;
  auto refresh_resolve_host_rt = [&](uint32_t resolve_width, uint32_t resolve_height) -> bool {
    if (resolve_host_rt && resolve_host_rt->width == resolve_width &&
        resolve_host_rt->height >= resolve_height &&
        resolve_host_rt->bgra.size() >= size_t(resolve_width) * resolve_height * 4) {
      latest_host_render_target_bgra_ = resolve_host_rt->bgra;
      latest_host_render_target_width_ = resolve_host_rt->width;
      latest_host_render_target_height_ = resolve_host_rt->height;
      static std::atomic<uint32_t> host_rt_cache_source_logs{0};
      uint32_t cache_source_index =
          host_rt_cache_source_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (cache_source_index <= 16 || (cache_source_index & 0xFF) == 0) {
        std::fprintf(stderr,
                     "[metal] host RT resolve using owned cache#%u rt=%u visible=%u "
                     "size=%ux%u\n",
                     cache_source_index, uint32_t(active_copy_control.copy_src_select),
                     CountVisibleRgbPixels(latest_host_render_target_bgra_),
                     latest_host_render_target_width_, latest_host_render_target_height_);
        std::fflush(stderr);
      }
      return true;
    }
    if (resolve_host_rt_context) {
      void* saved_host_render_target_context = host_render_target_context_;
      host_render_target_context_ = resolve_host_rt_context;
      bool refreshed = RefreshHostRenderTargetBacking(resolve_width, resolve_height);
      host_render_target_context_ = saved_host_render_target_context;
      if (refreshed && resolve_host_rt) {
        bool refreshed_has_visible_rgb = BgraHasNonZeroRgb(latest_host_render_target_bgra_);
        bool cached_has_visible_rgb = BgraHasNonZeroRgb(resolve_host_rt->bgra);
        if (!refreshed_has_visible_rgb && cached_has_visible_rgb &&
            resolve_host_rt->width == resolve_width && resolve_host_rt->height == resolve_height) {
          static std::atomic<uint32_t> host_rt_cache_resolve_logs{0};
          uint32_t cache_resolve_index =
              host_rt_cache_resolve_logs.fetch_add(1, std::memory_order_relaxed) + 1;
          if (cache_resolve_index <= 16 || (cache_resolve_index & 0xFF) == 0) {
            std::fprintf(stderr,
                         "[metal] host RT resolve using cached producer#%u rt=%u "
                         "context_visible=0 cached_visible=%u size=%ux%u\n",
                         cache_resolve_index, uint32_t(active_copy_control.copy_src_select),
                         CountVisibleRgbPixels(resolve_host_rt->bgra), resolve_host_rt->width,
                         resolve_host_rt->height);
            std::fflush(stderr);
          }
          latest_host_render_target_bgra_ = resolve_host_rt->bgra;
          latest_host_render_target_width_ = resolve_host_rt->width;
          latest_host_render_target_height_ = resolve_host_rt->height;
        } else {
          resolve_host_rt->bgra = latest_host_render_target_bgra_;
          resolve_host_rt->width = latest_host_render_target_width_;
          resolve_host_rt->height = latest_host_render_target_height_;
        }
        return true;
      }
    }
    if (resolve_host_rt && resolve_host_rt->width && resolve_host_rt->height &&
        resolve_host_rt->bgra.size() >=
            size_t(resolve_host_rt->width) * resolve_host_rt->height * 4) {
      latest_host_render_target_bgra_ = resolve_host_rt->bgra;
      latest_host_render_target_width_ = resolve_host_rt->width;
      latest_host_render_target_height_ = resolve_host_rt->height;
      return true;
    }
    return false;
  };
  auto compute_host_source_y = [&](uint32_t raw_source_y) -> uint32_t {
    if (!register_file_ || active_copy_control.copy_src_select >= xenos::kMaxColorRenderTargets) {
      return raw_source_y;
    }
    reg::RB_SURFACE_INFO surface_info = register_file_->Get<reg::RB_SURFACE_INFO>();
    reg::RB_COLOR_INFO color_info = register_file_->Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[uint32_t(active_copy_control.copy_src_select)]);
    bool is_64bpp = xenos::IsColorRenderTargetFormat64bpp(color_info.color_format);
    uint32_t pitch_tiles = xenos::GetSurfacePitchTiles(surface_info.surface_pitch,
                                                       surface_info.msaa_samples, is_64bpp);
    if (!pitch_tiles) {
      return raw_source_y;
    }
    uint32_t sample_count_log2_y = uint32_t(surface_info.msaa_samples >= xenos::MsaaSamples::k2X);
    uint32_t tile_height_pixels = xenos::kEdramTileHeightSamples >> sample_count_log2_y;
    uint32_t raw_source_y_samples = raw_source_y << sample_count_log2_y;
    uint32_t base_offset_y_tiles = raw_source_y_samples / xenos::kEdramTileHeightSamples;
    uint32_t base_tiles =
        color_info.color_base + (base_offset_y_tiles * pitch_tiles << uint32_t(is_64bpp));
    uint32_t source_base_delta = (base_tiles + xenos::kEdramTileCount -
                                  (color_info.color_base & (xenos::kEdramTileCount - 1))) &
                                 (xenos::kEdramTileCount - 1);
    return (source_base_delta / pitch_tiles) * tile_height_pixels +
           ((raw_source_y_samples % xenos::kEdramTileHeightSamples) >> sample_count_log2_y);
  };
  if (!wrote_source_resolve && !resolve_info_valid && active_copy_valid && memory_ &&
      register_file_ && resolve_host_rt_context &&
      active_copy_dest_info.copy_dest_format == xenos::ColorFormat::k_8_8_8_8 &&
      !active_copy_dest_info.copy_dest_array &&
      active_copy_control.copy_src_select < xenos::kMaxColorRenderTargets) {
    uint32_t resolve_width = fallback_output_width_;
    uint32_t resolve_height = fallback_output_height_;
    uint32_t source_rect_x = 0;
    uint32_t source_rect_y = 0;
    uint32_t source_rect_width = resolve_width;
    uint32_t source_rect_height = resolve_height;
    compute_copy_rect(resolve_width, resolve_height, source_rect_x, source_rect_y,
                      source_rect_width, source_rect_height);
    if (source_rect_width && source_rect_height &&
        refresh_resolve_host_rt(resolve_width, resolve_height)) {
      PendingReadbackResolveSlice slice;
      slice.copy_dest_base = last_copy_dest_base_;
      slice.pitch = std::max<uint32_t>(active_copy_dest_pitch.copy_dest_pitch, 1);
      slice.source_x = source_rect_x;
      slice.source_y = compute_host_source_y(source_rect_y);
      slice.dest_y = source_rect_y;
      slice.width = source_rect_width;
      slice.height = source_rect_height;
      slice.endian = active_copy_dest_info.copy_dest_endian;
      slice.bgra = latest_host_render_target_bgra_;
      slice.bgra_width = latest_host_render_target_width_;
      slice.bgra_height = latest_host_render_target_height_;
      if (pending_readback_resolve_slices_.size() >= 8) {
        pending_readback_resolve_slices_.erase(pending_readback_resolve_slices_.begin());
      }
      pending_readback_resolve_slices_.push_back(std::move(slice));
      static std::atomic<uint32_t> pending_readback_slice_logs{0};
      uint32_t pending_readback_slice_index =
          pending_readback_slice_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (pending_readback_slice_index <= 16 || (pending_readback_slice_index & 0x3F) == 0) {
        const PendingReadbackResolveSlice& logged_slice = pending_readback_resolve_slices_.back();
        std::fprintf(stderr,
                     "[metal] queued readback resolve slice#%u copy=%u dest=0x%08x "
                     "pitch=%u dest_y=%u source=%u,%u size=%ux%u bgra=%ux%u\n",
                     pending_readback_slice_index, metal_copy_index, logged_slice.copy_dest_base,
                     logged_slice.pitch, logged_slice.dest_y, logged_slice.source_x,
                     logged_slice.source_y, logged_slice.width, logged_slice.height,
                     logged_slice.bgra_width, logged_slice.bgra_height);
        std::fflush(stderr);
      }
    }
  }
  if (!wrote_source_resolve && resolve_info_valid && resolve_info.copy_dest_extent_length &&
      memory_ && register_file_) {
    uint32_t resolve_width = fallback_output_width_;
    uint32_t resolve_height = fallback_output_height_;
    bool refreshed_host_rt = refresh_resolve_host_rt(resolve_width, resolve_height);
    bool log_resolve_gate =
        RangesOverlap(resolve_info.copy_dest_base, resolve_info.copy_dest_extent_length,
                      kWatchedFramebufferBase, kWatchedFramebufferLength) ||
        RangesOverlap(resolve_info.copy_dest_base, resolve_info.copy_dest_extent_length,
                      kWatchedResolveBase, kWatchedResolveLength) ||
        RangesOverlap(resolve_info.copy_dest_base, resolve_info.copy_dest_extent_length,
                      kWatchedSwapBase, kWatchedSwapLength);
    if (log_resolve_gate) {
      static std::atomic<uint32_t> readback_resolve_gate_logs{0};
      uint32_t gate_index = readback_resolve_gate_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (gate_index <= 32 || (gate_index & 0xFF) == 0) {
        std::fprintf(stderr,
                     "[metal] readback resolve gate#%u copy=%u dest=0x%08x "
                     "extent=0x%08x+0x%x refreshed=%u rt=%u owner=%u context=%u "
                     "latest=%ux%u bytes=%zu visible=%u\n",
                     gate_index, metal_copy_index, resolve_info.copy_dest_base,
                     resolve_info.copy_dest_extent_start, resolve_info.copy_dest_extent_length,
                     refreshed_host_rt ? 1u : 0u, uint32_t(active_copy_control.copy_src_select),
                     resolve_host_rt ? 1u : 0u, resolve_host_rt_context ? 1u : 0u,
                     latest_host_render_target_width_, latest_host_render_target_height_,
                     latest_host_render_target_bgra_.size(),
                     CountVisibleRgbPixels(latest_host_render_target_bgra_));
        std::fflush(stderr);
      }
    }
    if (refreshed_host_rt) {
      uint32_t dest_pitch = std::max<uint32_t>(
          uint32_t(resolve_info.copy_dest_coordinate_info.pitch_aligned_div_32) * 32, 1);
      uint32_t dest_height = std::max<uint32_t>(
          uint32_t(resolve_info.copy_dest_coordinate_info.height_aligned_div_32) * 32, 1);
      if (last_swap_frontbuffer_ptr_ > resolve_info.copy_dest_base && dest_pitch) {
        uint32_t row_bytes = dest_pitch * 4;
        uint32_t swap_offset = last_swap_frontbuffer_ptr_ - resolve_info.copy_dest_base;
        if (row_bytes && !(swap_offset % row_bytes)) {
          uint32_t swap_offset_rows = swap_offset / row_bytes;
          dest_height = std::max(
              dest_height, swap_offset_rows + std::max<uint32_t>(last_swap_frontbuffer_height_, 1));
        }
      }
      uint32_t rect_x = uint32_t(resolve_info.copy_dest_coordinate_info.offset_x_div_8) * 8;
      uint32_t rect_y = uint32_t(resolve_info.copy_dest_coordinate_info.offset_y_div_8) * 8;
      uint32_t rect_width = resolve_info.coordinate_info.width_div_8 * 8;
      uint32_t rect_height = resolve_info.height_div_8 * 8;
      if (!rect_width || !rect_height || rect_width > resolve_width ||
          rect_height > resolve_height) {
        rect_x = 0;
        rect_y = 0;
        rect_width = resolve_width;
        rect_height = resolve_height;
      }
      uint32_t source_rect_x = 0;
      uint32_t source_rect_y = 0;
      uint32_t source_rect_width = resolve_width;
      uint32_t source_rect_height = resolve_height;
      compute_copy_rect(resolve_width, resolve_height, source_rect_x, source_rect_y,
                        source_rect_width, source_rect_height);
      draw_util::ResolveEdramInfo source_edram_info = resolve_info.IsCopyingDepth()
                                                          ? resolve_info.depth_edram_info
                                                          : resolve_info.color_edram_info;
      uint32_t host_source_rect_x =
          source_rect_x >> uint32_t(source_edram_info.msaa_samples >= xenos::MsaaSamples::k4X);
      uint32_t host_source_rect_y =
          source_rect_y >> uint32_t(source_edram_info.msaa_samples >= xenos::MsaaSamples::k2X);
      uint32_t source_original_base = resolve_info.IsCopyingDepth()
                                          ? resolve_info.depth_original_base
                                          : resolve_info.color_original_base;
      if (source_edram_info.pitch_tiles) {
        uint32_t source_base_delta = (source_edram_info.base_tiles + xenos::kEdramTileCount -
                                      (source_original_base & (xenos::kEdramTileCount - 1))) &
                                     (xenos::kEdramTileCount - 1);
        uint32_t tile_width_pixels =
            xenos::kEdramTileWidthSamples >>
            (uint32_t(source_edram_info.msaa_samples >= xenos::MsaaSamples::k4X) +
             uint32_t(source_edram_info.format_is_64bpp));
        uint32_t tile_height_pixels =
            xenos::kEdramTileHeightSamples >>
            uint32_t(source_edram_info.msaa_samples >= xenos::MsaaSamples::k2X);
        host_source_rect_x =
            (source_base_delta % source_edram_info.pitch_tiles) * tile_width_pixels +
            (uint32_t(resolve_info.coordinate_info.edram_offset_x_div_8) * 8);
        host_source_rect_y =
            (source_base_delta / source_edram_info.pitch_tiles) * tile_height_pixels +
            (uint32_t(resolve_info.coordinate_info.edram_offset_y_div_8) * 8);
      }
      uint32_t copy_width = std::min(rect_width, source_rect_width);
      uint32_t copy_height = std::min(rect_height, source_rect_height);
      std::vector<uint8_t> edram_resolved_bgra;
      bool used_edram_resolve = false;
      if (resolve_host_rt && DumpHostRenderTargetToEdram(*resolve_host_rt)) {
        used_edram_resolve =
            ResolveEdramToBgra(resolve_info, copy_width, copy_height, edram_resolved_bgra);
      }
      if (log_resolve_gate) {
        static std::atomic<uint32_t> readback_resolve_rect_logs{0};
        uint32_t rect_index =
            readback_resolve_rect_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (rect_index <= 32 || (rect_index & 0xFF) == 0) {
          std::fprintf(stderr,
                       "[metal] readback resolve rect#%u copy=%u dest=0x%08x "
                       "pitch=%u height=%u rect=%u,%u %ux%u source_rect=%u,%u %ux%u "
                       "host_source=%u,%u copy=%ux%u msaa=%u\n",
                       rect_index, metal_copy_index, resolve_info.copy_dest_base, dest_pitch,
                       dest_height, rect_x, rect_y, rect_width, rect_height, source_rect_x,
                       source_rect_y, source_rect_width, source_rect_height, host_source_rect_x,
                       host_source_rect_y, copy_width, copy_height,
                       uint32_t(source_edram_info.msaa_samples));
          std::fflush(stderr);
        }
      }
      uint32_t write_dest_base = resolve_info.copy_dest_base;
      uint32_t write_dest_y = rect_y;
      if (source_rect_y && dest_pitch) {
        uint64_t anchor_offset = uint64_t(source_rect_y) * dest_pitch * 4;
        if (resolve_info.copy_dest_base >= anchor_offset) {
          write_dest_base = uint32_t(resolve_info.copy_dest_base - anchor_offset);
          write_dest_y = source_rect_y;
          dest_height = std::max(dest_height, write_dest_y + copy_height);
        }
      }
      if (write_dest_base != resolve_info.copy_dest_base) {
        for (auto it = pending_readback_resolve_slices_.begin();
             it != pending_readback_resolve_slices_.end();) {
          if (it->copy_dest_base != resolve_info.copy_dest_base || it->pitch != dest_pitch ||
              it->bgra.empty() || !it->bgra_width || !it->bgra_height) {
            ++it;
            continue;
          }
          uint32_t slice_dest_height = std::max(dest_height, it->dest_y + it->height);
          bool wrote_slice = WriteBgraToTiledResolveRegion(
              write_dest_base, dest_pitch, slice_dest_height, it->bgra, it->bgra_width,
              it->bgra_height, it->source_x, it->source_y, it->source_x, it->dest_y, it->width,
              it->height, it->endian);
          static std::atomic<uint32_t> flushed_readback_slice_logs{0};
          uint32_t flushed_readback_slice_index =
              flushed_readback_slice_logs.fetch_add(1, std::memory_order_relaxed) + 1;
          if (flushed_readback_slice_index <= 16 || (flushed_readback_slice_index & 0x3F) == 0) {
            std::fprintf(stderr,
                         "[metal] flushed readback resolve slice#%u anchor=0x%08x "
                         "canonical=0x%08x dest_y=%u source=%u,%u size=%ux%u ok=%u\n",
                         flushed_readback_slice_index, resolve_info.copy_dest_base, write_dest_base,
                         it->dest_y, it->source_x, it->source_y, it->width, it->height,
                         wrote_slice ? 1u : 0u);
            std::fflush(stderr);
          }
          it = pending_readback_resolve_slices_.erase(it);
        }
      }
      const std::vector<uint8_t>& resolve_source_bgra =
          used_edram_resolve ? edram_resolved_bgra : latest_host_render_target_bgra_;
      uint32_t resolve_source_width =
          used_edram_resolve ? copy_width : latest_host_render_target_width_;
      uint32_t resolve_source_height =
          used_edram_resolve ? copy_height : latest_host_render_target_height_;
      uint32_t resolve_source_x = used_edram_resolve ? 0 : host_source_rect_x;
      uint32_t resolve_source_y = used_edram_resolve ? 0 : host_source_rect_y;
      bool wrote_host_rt_resolve = WriteBgraToTiledResolveRegion(
          write_dest_base, dest_pitch, dest_height, resolve_source_bgra, resolve_source_width,
          resolve_source_height, resolve_source_x, resolve_source_y, rect_x, write_dest_y,
          copy_width, copy_height, resolve_info.copy_dest_info.copy_dest_endian);
      if (wrote_host_rt_resolve) {
        wrote_source_resolve = true;
        static std::atomic<uint32_t> host_rt_resolve_logs{0};
        uint32_t host_rt_resolve_index =
            host_rt_resolve_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (host_rt_resolve_index <= 16 || (host_rt_resolve_index & 0x3F) == 0) {
          BgraFrameStats host_rt_stats = GetBgraFrameStats(latest_host_render_target_bgra_);
          BgraBandStats host_rt_band_stats =
              GetBgraBandStats(latest_host_render_target_bgra_, latest_host_render_target_width_,
                               latest_host_render_target_height_);
          std::fprintf(
              stderr,
              "[metal] readback resolve#%u copy=%u src_rt=%u dest=0x%08x "
              "extent=0x%08x+0x%x pitch=%u height=%u source=%u,%u "
              "host_source=%u,%u msaa=%u "
              "dest_rect=%u,%u %ux%u "
              "source_visible=%u source_range=%u source_bands(top208=%u mid=%u "
              "low=%u) edram_path=%u edram_visible=%u edram_range=%u\n",
              host_rt_resolve_index, metal_copy_index,
              uint32_t(active_copy_control.copy_src_select), resolve_info.copy_dest_base,
              resolve_info.copy_dest_extent_start, resolve_info.copy_dest_extent_length, dest_pitch,
              dest_height, source_rect_x, source_rect_y, host_source_rect_x, host_source_rect_y,
              uint32_t(source_edram_info.msaa_samples), rect_x, rect_y, copy_width, copy_height,
              host_rt_stats.visible_pixels, BgraRgbRange(host_rt_stats),
              host_rt_band_stats.top_208_visible, host_rt_band_stats.mid_208_512_visible,
              host_rt_band_stats.low_512_visible, used_edram_resolve ? 1u : 0u,
              used_edram_resolve ? CountVisibleRgbPixels(edram_resolved_bgra) : 0u,
              used_edram_resolve ? BgraRgbRange(GetBgraFrameStats(edram_resolved_bgra)) : 0u);
          std::fflush(stderr);
        }
        if (resolve_host_rt && resolve_host_rt->context && active_copy_control.color_clear_enable &&
            latest_host_render_target_width_ && latest_host_render_target_height_) {
          uint32_t clear_rgba = resolve_info.rb_color_clear;
          uint8_t clear_r = uint8_t(clear_rgba >> 16);
          uint8_t clear_g = uint8_t(clear_rgba >> 8);
          uint8_t clear_b = uint8_t(clear_rgba);
          uint8_t clear_a = uint8_t(clear_rgba >> 24);
          uint32_t clear_x = std::min(resolve_source_x, latest_host_render_target_width_);
          uint32_t clear_y = std::min(resolve_source_y, latest_host_render_target_height_);
          uint32_t clear_width = std::min(copy_width, latest_host_render_target_width_ - clear_x);
          uint32_t clear_height =
              std::min(copy_height, latest_host_render_target_height_ - clear_y);
          std::string clear_error;
          if (ClearPipelineProbeContextRect(
                  resolve_host_rt->context, latest_host_render_target_width_,
                  latest_host_render_target_height_, clear_x, clear_y, clear_width, clear_height,
                  double(clear_r) / 255.0, double(clear_g) / 255.0, double(clear_b) / 255.0,
                  double(clear_a) / 255.0, &clear_error)) {
            if (resolve_host_rt->bgra.size() <
                size_t(latest_host_render_target_width_) * latest_host_render_target_height_ * 4) {
              resolve_host_rt->bgra.assign(
                  size_t(latest_host_render_target_width_) * latest_host_render_target_height_ * 4,
                  0);
            }
            for (uint32_t row = 0; row < clear_height; ++row) {
              uint8_t* bgra =
                  resolve_host_rt->bgra.data() +
                  (size_t(clear_y + row) * latest_host_render_target_width_ + clear_x) * 4;
              for (uint32_t pixel = 0; pixel < clear_width; ++pixel, bgra += 4) {
                bgra[0] = clear_b;
                bgra[1] = clear_g;
                bgra[2] = clear_r;
                bgra[3] = clear_a;
              }
            }
            resolve_host_rt->width = latest_host_render_target_width_;
            resolve_host_rt->height = latest_host_render_target_height_;
            latest_host_render_target_bgra_ = resolve_host_rt->bgra;
            static std::atomic<uint32_t> host_rt_clear_logs{0};
            uint32_t host_rt_clear_index =
                host_rt_clear_logs.fetch_add(1, std::memory_order_relaxed) + 1;
            if (host_rt_clear_index <= 16 || (host_rt_clear_index & 0xFF) == 0) {
              std::fprintf(stderr,
                           "[metal] host RT resolve-clear#%u copy=%u rt=%u rect=%u,%u %ux%u "
                           "size=%ux%u rgba=%02x %02x %02x %02x\n",
                           host_rt_clear_index, metal_copy_index,
                           uint32_t(active_copy_control.copy_src_select), clear_x, clear_y,
                           clear_width, clear_height, latest_host_render_target_width_,
                           latest_host_render_target_height_, clear_r, clear_g, clear_b, clear_a);
              std::fflush(stderr);
            }
          } else {
            static std::atomic<uint32_t> host_rt_clear_fail_logs{0};
            uint32_t host_rt_clear_fail_index =
                host_rt_clear_fail_logs.fetch_add(1, std::memory_order_relaxed) + 1;
            if (host_rt_clear_fail_index <= 8 || (host_rt_clear_fail_index & 0xFF) == 0) {
              std::fprintf(stderr, "[metal] host RT resolve-clear failed#%u copy=%u rt=%u: %s\n",
                           host_rt_clear_fail_index, metal_copy_index,
                           uint32_t(active_copy_control.copy_src_select), clear_error.c_str());
              std::fflush(stderr);
            }
          }
        }
      }
    }
  }
  if (MetalFallbackResolveEnabled()) {
    if (!wrote_source_resolve && memory_ && register_file_ && last_copy_dest_base_ &&
        active_copy_valid && pipeline_probe_context_) {
      reg::RB_COPY_DEST_PITCH copy_dest_pitch = active_copy_dest_pitch;
      uint32_t resolve_width = std::min<uint32_t>(
          fallback_output_width_, std::max<uint32_t>(copy_dest_pitch.copy_dest_pitch, 1));
      uint32_t resolve_height = fallback_output_height_;
      uint32_t copy_rect_x = 0;
      uint32_t copy_rect_y = 0;
      uint32_t copy_rect_width = resolve_width;
      uint32_t copy_rect_height = resolve_height;
      compute_copy_rect(resolve_width, resolve_height, copy_rect_x, copy_rect_y, copy_rect_width,
                        copy_rect_height);
      if (RefreshPipelineProbeBacking(resolve_width, resolve_height) &&
          WriteBgraToTiledResolveRect(last_copy_dest_base_, copy_dest_pitch.copy_dest_pitch,
                                      copy_dest_pitch.copy_dest_height, resolved_color_bgra_,
                                      resolved_color_width_, resolved_color_height_, copy_rect_x,
                                      copy_rect_y, copy_rect_width, copy_rect_height)) {
        wrote_source_resolve = true;
        static std::atomic<uint32_t> probe_target_resolve_logs{0};
        uint32_t probe_target_resolve_index =
            probe_target_resolve_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (probe_target_resolve_index <= 16 || (probe_target_resolve_index & 0x3F) == 0) {
          std::fprintf(stderr,
                       "[metal] wrote probe render target resolve#%u base=0x%08x "
                       "size=%ux%u rect=%u,%u %ux%u pitch=%u height=%u\n",
                       probe_target_resolve_index, last_copy_dest_base_, resolved_color_width_,
                       resolved_color_height_, copy_rect_x, copy_rect_y, copy_rect_width,
                       copy_rect_height, copy_dest_pitch.copy_dest_pitch,
                       copy_dest_pitch.copy_dest_height);
          std::fflush(stderr);
        }
      }
    }
    if (!wrote_source_resolve && memory_ && register_file_ && last_copy_dest_base_ &&
        active_copy_valid) {
      reg::RB_COPY_DEST_PITCH copy_dest_pitch = active_copy_dest_pitch;
      if (copy_dest_pitch.copy_dest_pitch && copy_dest_pitch.copy_dest_height &&
          copy_dest_pitch.copy_dest_height >= fallback_output_height_) {
        xenos::xe_gpu_texture_fetch_t source_fetch = register_file_->GetTextureFetch(0);
        std::vector<uint8_t> source_rgba;
        uint32_t source_width = 0;
        uint32_t source_height = 0;
        if (DecodeTextureFetchToRgba(source_fetch, 0, 0, source_rgba, source_width,
                                     source_height) &&
            source_width && source_height) {
          std::vector<uint8_t> source_texture_rgba(source_rgba.size());
          for (size_t pixel = 0, pixel_count = size_t(source_width) * source_height;
               pixel < pixel_count; ++pixel) {
            const uint8_t* source_pixel = source_rgba.data() + pixel * 4;
            uint8_t* target_pixel = source_texture_rgba.data() + pixel * 4;
            target_pixel[0] =
                ResolveSwizzledComponent(source_pixel, SwizzleComponent(source_fetch.swizzle, 0));
            target_pixel[1] =
                ResolveSwizzledComponent(source_pixel, SwizzleComponent(source_fetch.swizzle, 1));
            target_pixel[2] =
                ResolveSwizzledComponent(source_pixel, SwizzleComponent(source_fetch.swizzle, 2));
            target_pixel[3] =
                ResolveSwizzledComponent(source_pixel, SwizzleComponent(source_fetch.swizzle, 3));
          }

          uint32_t resolve_width = std::min<uint32_t>(
              fallback_output_width_, std::max<uint32_t>(copy_dest_pitch.copy_dest_pitch, 1));
          uint32_t resolve_height = std::min<uint32_t>(
              fallback_output_height_, std::max<uint32_t>(copy_dest_pitch.copy_dest_height, 1));
          std::vector<MetalHostVertex> copy_vertices;
          xenos::xe_gpu_vertex_fetch_t vertex_fetch = register_file_->GetVertexFetch(0);
          float source_copy_vertices[6] = {
              -0.5f,
              -0.5f,
              float(source_width) - 0.5f,
              -0.5f,
              float(source_width) - 0.5f,
              float(source_height) - 0.5f,
          };
          if (vertex_fetch.type == xenos::FetchConstantType::kVertex && vertex_fetch.size >= 6) {
            const float* vertices_guest = reinterpret_cast<const float*>(
                memory_->TranslatePhysical(vertex_fetch.address * sizeof(uint32_t)));
            if (vertices_guest) {
              for (uint32_t i = 0; i < 6; ++i) {
                source_copy_vertices[i] = xenos::GpuSwap(vertices_guest[i], vertex_fetch.endian);
              }
            }
          }
          float copy_rectangle[8] = {
              source_copy_vertices[0],
              source_copy_vertices[1],
              source_copy_vertices[2],
              source_copy_vertices[3],
              source_copy_vertices[4],
              source_copy_vertices[5],
              source_copy_vertices[0] + source_copy_vertices[4] - source_copy_vertices[2],
              source_copy_vertices[1] + source_copy_vertices[5] - source_copy_vertices[3],
          };
          auto append_textured_copy_vertex = [&](uint32_t rectangle_index) {
            if (copy_vertices.size() >= MetalDrawRenderer::kMaxHostVerticesPerFrame) {
              return;
            }
            float screen_x = copy_rectangle[rectangle_index * 2] + 0.5f;
            float screen_y = copy_rectangle[rectangle_index * 2 + 1] + 0.5f;
            MetalHostVertex& host_vertex = copy_vertices.emplace_back();
            host_vertex.x = (screen_x / float(std::max(resolve_width, UINT32_C(1)))) * 2.0f - 1.0f;
            host_vertex.y = 1.0f - (screen_y / float(std::max(resolve_height, UINT32_C(1)))) * 2.0f;
            host_vertex.z = 0.0f;
            host_vertex.w = 1.0f;
            host_vertex.r = 1.0f;
            host_vertex.g = 1.0f;
            host_vertex.b = 1.0f;
            host_vertex.a = 1.0f;
            host_vertex.u =
                std::clamp(screen_x / float(std::max(source_width, UINT32_C(1))), 0.0f, 1.0f);
            host_vertex.v =
                std::clamp(screen_y / float(std::max(source_height, UINT32_C(1))), 0.0f, 1.0f);
            host_vertex.texture_weight = 1.0f;
          };
          uint32_t copy_triangle_indices[6] = {0, 1, 2, 0, 2, 3};
          for (uint32_t copy_triangle_index : copy_triangle_indices) {
            append_textured_copy_vertex(copy_triangle_index);
          }

          MetalHostTexture host_texture = {};
          host_texture.rgba = source_texture_rgba.data();
          host_texture.width = source_width;
          host_texture.height = source_height;
          host_texture.bytes_per_row = size_t(source_width) * 4;
          std::vector<uint8_t> metal_resolve_bgra;
          bool metal_resolve_rendered =
              draw_renderer_ &&
              draw_renderer_->RenderDrawEventFrame(
                  resolve_width, resolve_height, draw_count_, metal_copy_index,
                  pending_draw_events_, copy_vertices, metal_resolve_bgra, &host_texture) &&
              BlitAndWriteResolvedColor(last_copy_dest_base_, copy_dest_pitch.copy_dest_pitch,
                                        copy_dest_pitch.copy_dest_height, metal_resolve_bgra,
                                        resolve_width, resolve_height, 0, 0, resolve_width,
                                        resolve_height);
          if (metal_resolve_rendered) {
            wrote_source_resolve = true;
            static std::atomic<uint32_t> source_resolve_write_logs{0};
            uint32_t source_resolve_write_index =
                source_resolve_write_logs.fetch_add(1, std::memory_order_relaxed) + 1;
            if (source_resolve_write_index <= 8 || (source_resolve_write_index & 0x3F) == 0) {
              std::fprintf(stderr,
                           "[metal] wrote textured source resolve#%u src=%ux%u dest=0x%08x "
                           "pitch=%u height=%u image=%ux%u verts=(%.2f,%.2f %.2f,%.2f %.2f,%.2f)\n",
                           source_resolve_write_index, source_width, source_height,
                           last_copy_dest_base_, copy_dest_pitch.copy_dest_pitch,
                           copy_dest_pitch.copy_dest_height, resolve_width, resolve_height,
                           source_copy_vertices[0], source_copy_vertices[1],
                           source_copy_vertices[2], source_copy_vertices[3],
                           source_copy_vertices[4], source_copy_vertices[5]);
              std::fflush(stderr);
            }
          } else {
            std::vector<uint8_t> source_bgra(size_t(source_width) * source_height * 4);
            for (size_t pixel = 0, pixel_count = size_t(source_width) * source_height;
                 pixel < pixel_count; ++pixel) {
              RgbaToDrawableBgra(source_rgba.data() + pixel * 4, source_fetch.swizzle,
                                 source_bgra.data() + pixel * 4);
            }
            if (BlitAndWriteResolvedColor(last_copy_dest_base_, copy_dest_pitch.copy_dest_pitch,
                                          copy_dest_pitch.copy_dest_height, source_bgra,
                                          source_width, source_height, 0, 0, source_width,
                                          source_height)) {
              wrote_source_resolve = true;
            }
          }
        }
      }
    }
    if (!wrote_source_resolve && memory_ && register_file_ && last_copy_dest_base_ &&
        active_copy_valid && !pending_texture_resolve_bgra_.empty() &&
        pending_texture_resolve_width_ && pending_texture_resolve_height_) {
      reg::RB_COPY_DEST_PITCH copy_dest_pitch = active_copy_dest_pitch;
      uint32_t resolve_width = std::min<uint32_t>(
          pending_texture_resolve_width_, std::max<uint32_t>(copy_dest_pitch.copy_dest_pitch, 1));
      uint32_t resolve_height = pending_texture_resolve_height_;
      uint32_t copy_rect_x = 0;
      uint32_t copy_rect_y = 0;
      uint32_t copy_rect_width = resolve_width;
      uint32_t copy_rect_height = resolve_height;
      xenos::xe_gpu_vertex_fetch_t copy_vertex_fetch = register_file_->GetVertexFetch(0);
      if (copy_vertex_fetch.type == xenos::FetchConstantType::kVertex &&
          copy_vertex_fetch.size >= 6) {
        const float* vertices_guest = reinterpret_cast<const float*>(
            memory_->TranslatePhysical(copy_vertex_fetch.address * sizeof(uint32_t)));
        if (vertices_guest) {
          float copy_vertices[6];
          for (uint32_t i = 0; i < 6; ++i) {
            copy_vertices[i] = xenos::GpuSwap(vertices_guest[i], copy_vertex_fetch.endian);
          }
          float min_x = std::min({copy_vertices[0], copy_vertices[2], copy_vertices[4]}) + 0.5f;
          float max_x = std::max({copy_vertices[0], copy_vertices[2], copy_vertices[4]}) + 0.5f;
          float min_y = std::min({copy_vertices[1], copy_vertices[3], copy_vertices[5]}) + 0.5f;
          float max_y = std::max({copy_vertices[1], copy_vertices[3], copy_vertices[5]}) + 0.5f;
          min_x = std::clamp(min_x, 0.0f, float(resolve_width));
          max_x = std::clamp(max_x, 0.0f, float(resolve_width));
          min_y = std::clamp(min_y, 0.0f, float(resolve_height));
          max_y = std::clamp(max_y, 0.0f, float(resolve_height));
          if (max_x > min_x && max_y > min_y) {
            copy_rect_x = uint32_t(std::floor(min_x));
            copy_rect_y = uint32_t(std::floor(min_y));
            uint32_t rect_x1 = uint32_t(std::ceil(max_x));
            uint32_t rect_y1 = uint32_t(std::ceil(max_y));
            copy_rect_width = rect_x1 > copy_rect_x ? rect_x1 - copy_rect_x : 0;
            copy_rect_height = rect_y1 > copy_rect_y ? rect_y1 - copy_rect_y : 0;
          }
        }
      }
      if (BlitAndWriteResolvedColor(last_copy_dest_base_, copy_dest_pitch.copy_dest_pitch,
                                    fallback_output_height_, pending_texture_resolve_bgra_,
                                    pending_texture_resolve_width_, pending_texture_resolve_height_,
                                    copy_rect_x, copy_rect_y, copy_rect_width, copy_rect_height)) {
        wrote_source_resolve = true;
        static std::atomic<uint32_t> texture_resolve_write_logs{0};
        uint32_t texture_resolve_write_index =
            texture_resolve_write_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (texture_resolve_write_index <= 16 || (texture_resolve_write_index & 0xFF) == 0) {
          std::fprintf(stderr,
                       "[metal] wrote pending texture resolve#%u base=0x%08x image=%ux%u "
                       "rect=%u,%u %ux%u pitch=%u height=%u\n",
                       texture_resolve_write_index, last_copy_dest_base_,
                       pending_texture_resolve_width_, pending_texture_resolve_height_, copy_rect_x,
                       copy_rect_y, copy_rect_width, copy_rect_height,
                       copy_dest_pitch.copy_dest_pitch, copy_dest_pitch.copy_dest_height);
          std::fflush(stderr);
        }
        if (copy_rect_y + copy_rect_height >= fallback_output_height_) {
          pending_draw_events_.clear();
          pending_host_vertices_.clear();
          pending_texture_resolve_bgra_.clear();
          pending_texture_resolve_width_ = 0;
          pending_texture_resolve_height_ = 0;
        }
      }
    }
    if (!wrote_source_resolve && memory_ && register_file_ && draw_renderer_ &&
        last_copy_dest_base_ && draw_count_ && !pending_host_vertices_.empty()) {
      reg::RB_COPY_DEST_PITCH copy_dest_pitch = active_copy_dest_pitch;
      if (!copy_dest_pitch.copy_dest_pitch) {
        copy_dest_pitch.copy_dest_pitch = fallback_output_width_;
      }
      if (!copy_dest_pitch.copy_dest_height) {
        copy_dest_pitch.copy_dest_height = fallback_output_height_;
      }
      uint32_t resolve_width = std::min<uint32_t>(
          fallback_output_width_, std::max<uint32_t>(copy_dest_pitch.copy_dest_pitch, 1));
      uint32_t resolve_height = fallback_output_height_;
      uint32_t copy_rect_x = 0;
      uint32_t copy_rect_y = 0;
      uint32_t copy_rect_width = resolve_width;
      uint32_t copy_rect_height = resolve_height;
      xenos::xe_gpu_vertex_fetch_t copy_vertex_fetch = register_file_->GetVertexFetch(0);
      if (copy_vertex_fetch.type == xenos::FetchConstantType::kVertex &&
          copy_vertex_fetch.size >= 6) {
        const float* vertices_guest = reinterpret_cast<const float*>(
            memory_->TranslatePhysical(copy_vertex_fetch.address * sizeof(uint32_t)));
        if (vertices_guest) {
          float copy_vertices[6];
          for (uint32_t i = 0; i < 6; ++i) {
            copy_vertices[i] = xenos::GpuSwap(vertices_guest[i], copy_vertex_fetch.endian);
          }
          float min_x = std::min({copy_vertices[0], copy_vertices[2], copy_vertices[4]}) + 0.5f;
          float max_x = std::max({copy_vertices[0], copy_vertices[2], copy_vertices[4]}) + 0.5f;
          float min_y = std::min({copy_vertices[1], copy_vertices[3], copy_vertices[5]}) + 0.5f;
          float max_y = std::max({copy_vertices[1], copy_vertices[3], copy_vertices[5]}) + 0.5f;
          min_x = std::clamp(min_x, 0.0f, float(resolve_width));
          max_x = std::clamp(max_x, 0.0f, float(resolve_width));
          min_y = std::clamp(min_y, 0.0f, float(resolve_height));
          max_y = std::clamp(max_y, 0.0f, float(resolve_height));
          if (max_x > min_x && max_y > min_y) {
            copy_rect_x = uint32_t(std::floor(min_x));
            copy_rect_y = uint32_t(std::floor(min_y));
            uint32_t rect_x1 = uint32_t(std::ceil(max_x));
            uint32_t rect_y1 = uint32_t(std::ceil(max_y));
            copy_rect_width = rect_x1 > copy_rect_x ? rect_x1 - copy_rect_x : 0;
            copy_rect_height = rect_y1 > copy_rect_y ? rect_y1 - copy_rect_y : 0;
          }
        }
      }
      MetalHostTexture host_texture = {};
      MetalHostTexture* host_texture_ptr = nullptr;
      if (kEnableHostTextureFallback && !pending_host_texture_rgba_.empty() &&
          pending_host_texture_width_ && pending_host_texture_height_) {
        host_texture.rgba = pending_host_texture_rgba_.data();
        host_texture.width = pending_host_texture_width_;
        host_texture.height = pending_host_texture_height_;
        host_texture.bytes_per_row = size_t(pending_host_texture_width_) * 4;
        host_texture_ptr = &host_texture;
      }
      bool recent_fullscreen_resolve =
          latest_fullscreen_postprocess_draw_count_ &&
          draw_count_ >= latest_fullscreen_postprocess_draw_count_ &&
          draw_count_ - latest_fullscreen_postprocess_draw_count_ <= 2 &&
          latest_fullscreen_postprocess_width_ == fallback_output_width_ &&
          latest_fullscreen_postprocess_height_ == fallback_output_height_ &&
          BgraHasNonZeroRgb(latest_fullscreen_postprocess_bgra_);
      if (!host_texture_ptr && recent_fullscreen_resolve) {
        static std::atomic<uint32_t> fallback_stomp_skip_logs{0};
        uint32_t fallback_stomp_skip_index =
            fallback_stomp_skip_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fallback_stomp_skip_index <= 16 || (fallback_stomp_skip_index & 0x3F) == 0) {
          std::fprintf(stderr,
                       "[metal] skipped untextured fallback resolve#%u after fullscreen "
                       "draw_count=%u fullscreen_draw_count=%u host_vertices=%zu\n",
                       fallback_stomp_skip_index, draw_count_,
                       latest_fullscreen_postprocess_draw_count_, pending_host_vertices_.size());
          std::fflush(stderr);
        }
        return true;
      }
      bool recent_host_pixel_frame =
          latest_host_pixel_frame_draw_count_ &&
          draw_count_ >= latest_host_pixel_frame_draw_count_ &&
          draw_count_ - latest_host_pixel_frame_draw_count_ <= 64 &&
          latest_host_pixel_frame_width_ == resolve_width &&
          latest_host_pixel_frame_height_ == resolve_height &&
          latest_host_pixel_frame_bgra_.size() >= size_t(resolve_width) * resolve_height * 4;
      if (recent_host_pixel_frame) {
        BgraFrameStats host_pixel_stats = GetBgraFrameStats(latest_host_pixel_frame_bgra_);
        const std::vector<uint8_t>* selected_resolve_bgra = &latest_host_pixel_frame_bgra_;
        BgraFrameStats selected_resolve_stats = host_pixel_stats;
        std::vector<uint8_t> fallback_candidate_bgra;
        if (host_pixel_skipped_vertices_this_swap_ && latest_host_pixel_frame_from_fallback_ &&
            draw_renderer_ &&
            draw_renderer_->RenderDrawEventFrame(
                resolve_width, resolve_height, draw_count_, metal_copy_index, pending_draw_events_,
                pending_host_vertices_, fallback_candidate_bgra, host_texture_ptr)) {
          BgraFrameStats fallback_candidate_stats = GetBgraFrameStats(fallback_candidate_bgra);
          if (fallback_candidate_stats.visible_pixels >
                  host_pixel_stats.visible_pixels + host_pixel_stats.visible_pixels / 5 &&
              !IsDominantFlatVisibleFrame(fallback_candidate_bgra, resolve_width, resolve_height,
                                          fallback_candidate_stats.visible_pixels)) {
            selected_resolve_bgra = &fallback_candidate_bgra;
            selected_resolve_stats = fallback_candidate_stats;
            static std::atomic<uint32_t> fallback_candidate_prefer_logs{0};
            uint32_t fallback_candidate_prefer_index =
                fallback_candidate_prefer_logs.fetch_add(1, std::memory_order_relaxed) + 1;
            if (fallback_candidate_prefer_index <= 16 ||
                (fallback_candidate_prefer_index & 0x3F) == 0) {
              std::fprintf(stderr,
                           "[metal] preferred fallback candidate#%u over host pixel "
                           "skipped_vertices=%u host_visible=%u fallback_visible=%u "
                           "host_vertices=%zu\n",
                           fallback_candidate_prefer_index, host_pixel_skipped_vertices_this_swap_,
                           host_pixel_stats.visible_pixels, fallback_candidate_stats.visible_pixels,
                           pending_host_vertices_.size());
              std::fflush(stderr);
            }
          }
        }
        if (selected_resolve_stats.visible_pixels &&
            !IsDominantFlatVisibleFrame(*selected_resolve_bgra, resolve_width, resolve_height,
                                        selected_resolve_stats.visible_pixels) &&
            CompositeAndWriteResolvedColor(last_copy_dest_base_, copy_dest_pitch.copy_dest_pitch,
                                           fallback_output_height_, *selected_resolve_bgra,
                                           resolve_width, resolve_height, copy_rect_x, copy_rect_y,
                                           copy_rect_width, copy_rect_height)) {
          wrote_source_resolve = true;
          latest_draw_event_frame_bgra_ = resolved_color_bgra_;
          latest_draw_event_frame_width_ = resolved_color_width_;
          latest_draw_event_frame_height_ = resolved_color_height_;
          BgraFrameStats resolved_write_stats = GetBgraFrameStats(resolved_color_bgra_);
          static std::atomic<uint32_t> host_pixel_resolve_write_logs{0};
          uint32_t host_pixel_resolve_write_index =
              host_pixel_resolve_write_logs.fetch_add(1, std::memory_order_relaxed) + 1;
          if (host_pixel_resolve_write_index <= 16 ||
              (host_pixel_resolve_write_index & 0x3F) == 0) {
            std::fprintf(stderr,
                         "[metal] wrote host pixel resolve#%u base=0x%08x pitch=%u height=%u "
                         "image=%ux%u rect=%u,%u %ux%u host_vertices=%zu "
                         "source_visible=%u resolved_visible=%u range=%u\n",
                         host_pixel_resolve_write_index, last_copy_dest_base_,
                         copy_dest_pitch.copy_dest_pitch, copy_dest_pitch.copy_dest_height,
                         resolve_width, resolve_height, copy_rect_x, copy_rect_y, copy_rect_width,
                         copy_rect_height, pending_host_vertices_.size(),
                         selected_resolve_stats.visible_pixels, resolved_write_stats.visible_pixels,
                         BgraRgbRange(resolved_write_stats));
            std::fflush(stderr);
          }
          if (host_pixel_resolve_write_index <= 8) {
            DumpBgraFrameAsPpm("host_resolve", host_pixel_resolve_write_index, resolved_color_bgra_,
                               resolved_color_width_, resolved_color_height_);
          }
          if (copy_rect_y + copy_rect_height >= fallback_output_height_) {
            pending_draw_events_.clear();
            pending_host_vertices_.clear();
            pending_texture_resolve_bgra_.clear();
            pending_texture_resolve_width_ = 0;
            pending_texture_resolve_height_ = 0;
          }
        }
      }
      if (!wrote_source_resolve) {
        std::vector<uint8_t> resolve_bgra;
        if (draw_renderer_->RenderDrawEventFrame(
                resolve_width, resolve_height, draw_count_, metal_copy_index, pending_draw_events_,
                pending_host_vertices_, resolve_bgra, host_texture_ptr) &&
            BlitAndWriteResolvedColor(last_copy_dest_base_, copy_dest_pitch.copy_dest_pitch,
                                      fallback_output_height_, resolve_bgra, resolve_width,
                                      resolve_height, copy_rect_x, copy_rect_y, copy_rect_width,
                                      copy_rect_height)) {
          latest_draw_event_frame_bgra_ = resolve_bgra;
          latest_draw_event_frame_width_ = resolve_width;
          latest_draw_event_frame_height_ = resolve_height;
          static std::atomic<uint32_t> resolve_write_logs{0};
          uint32_t resolve_write_index =
              resolve_write_logs.fetch_add(1, std::memory_order_relaxed) + 1;
          if (resolve_write_index <= 8 || (resolve_write_index & 0x3F) == 0) {
            std::fprintf(stderr,
                         "[metal] wrote fallback resolve#%u base=0x%08x pitch=%u height=%u "
                         "image=%ux%u rect=%u,%u %ux%u host_vertices=%zu texture=%u\n",
                         resolve_write_index, last_copy_dest_base_, copy_dest_pitch.copy_dest_pitch,
                         copy_dest_pitch.copy_dest_height, resolve_width, resolve_height,
                         copy_rect_x, copy_rect_y, copy_rect_width, copy_rect_height,
                         pending_host_vertices_.size(), host_texture_ptr ? 1u : 0u);
            std::fflush(stderr);
          }
          if (copy_rect_y + copy_rect_height >= fallback_output_height_) {
            pending_draw_events_.clear();
            pending_host_vertices_.clear();
            pending_texture_resolve_bgra_.clear();
            pending_texture_resolve_width_ = 0;
            pending_texture_resolve_height_ = 0;
          }
        }
      }
    }
  }
  if (memory_ && register_file_ && (metal_copy_index <= 16 || (metal_copy_index & 0x3F) == 0)) {
    reg::RB_COPY_CONTROL copy_control = register_file_->Get<reg::RB_COPY_CONTROL>();
    reg::RB_COPY_DEST_INFO copy_dest_info = register_file_->Get<reg::RB_COPY_DEST_INFO>();
    reg::RB_COPY_DEST_PITCH copy_dest_pitch = register_file_->Get<reg::RB_COPY_DEST_PITCH>();
    reg::RB_SURFACE_INFO surface_info = register_file_->Get<reg::RB_SURFACE_INFO>();
    reg::RB_COLOR_INFO color_info = register_file_->Get<reg::RB_COLOR_INFO>(
        reg::RB_COLOR_INFO::rt_register_indices[std::min<uint32_t>(
            uint32_t(copy_control.copy_src_select), xenos::kMaxColorRenderTargets - 1)]);
    reg::RB_DEPTH_INFO depth_info = register_file_->Get<reg::RB_DEPTH_INFO>();
    xenos::xe_gpu_vertex_fetch_t fetch = register_file_->GetVertexFetch(0);
    float vertices[6] = {};
    if (fetch.type == xenos::FetchConstantType::kVertex && fetch.size >= 6) {
      const float* vertices_guest = reinterpret_cast<const float*>(
          memory_->TranslatePhysical(fetch.address * sizeof(uint32_t)));
      if (vertices_guest) {
        for (uint32_t i = 0; i < 6; ++i) {
          vertices[i] = xenos::GpuSwap(vertices_guest[i], fetch.endian);
        }
      }
    }
    std::fprintf(
        stderr,
        "[metal] resolve#%u raw ctrl=0x%08x src=%u cmd=%u sample=%u clear(c=%u d=%u) "
        "dest_base=0x%08x dest_info=0x%08x fmt=%u endian=%u pitch=%u height=%u "
        "surface_pitch=%u msaa=%u color(base=%u fmt=%u) depth(base=%u fmt=%u) "
        "vf0(type=%u addr=0x%08x size=%u endian=%u verts=%.2f,%.2f %.2f,%.2f %.2f,%.2f)\n",
        metal_copy_index, copy_control.value, uint32_t(copy_control.copy_src_select),
        uint32_t(copy_control.copy_command), uint32_t(copy_control.copy_sample_select),
        uint32_t(copy_control.color_clear_enable), uint32_t(copy_control.depth_clear_enable),
        register_file_->values[XE_GPU_REG_RB_COPY_DEST_BASE], copy_dest_info.value,
        uint32_t(copy_dest_info.copy_dest_format), uint32_t(copy_dest_info.copy_dest_endian),
        uint32_t(copy_dest_pitch.copy_dest_pitch), uint32_t(copy_dest_pitch.copy_dest_height),
        uint32_t(surface_info.surface_pitch), uint32_t(surface_info.msaa_samples),
        uint32_t(color_info.color_base), uint32_t(color_info.color_format),
        uint32_t(depth_info.depth_base), uint32_t(depth_info.depth_format), uint32_t(fetch.type),
        uint32_t(fetch.address * sizeof(uint32_t)), uint32_t(fetch.size), uint32_t(fetch.endian),
        vertices[0], vertices[1], vertices[2], vertices[3], vertices[4], vertices[5]);
    std::fflush(stderr);
  }
  LogIncompleteOnce("copy");
  return true;
}

bool MetalCommandProcessor::WriteBgraToTiledResolve(uint32_t dest_base, uint32_t pitch,
                                                    uint32_t height,
                                                    const std::vector<uint8_t>& bgra,
                                                    uint32_t width, uint32_t source_height) {
  return WriteBgraToTiledResolveRect(dest_base, pitch, height, bgra, width, source_height, 0, 0,
                                     width, source_height);
}

bool MetalCommandProcessor::WriteBgraToTiledResolveRect(
    uint32_t dest_base, uint32_t pitch, uint32_t height, const std::vector<uint8_t>& bgra,
    uint32_t width, uint32_t source_height, uint32_t rect_x, uint32_t rect_y, uint32_t rect_width,
    uint32_t rect_height, xenos::Endian128 dest_endian) {
  return WriteBgraToTiledResolveRegion(dest_base, pitch, height, bgra, width, source_height, rect_x,
                                       rect_y, rect_x, rect_y, rect_width, rect_height,
                                       dest_endian);
}

bool MetalCommandProcessor::WriteBgraToTiledResolveRegion(
    uint32_t dest_base, uint32_t pitch, uint32_t height, const std::vector<uint8_t>& bgra,
    uint32_t width, uint32_t source_height, uint32_t source_x, uint32_t source_y, uint32_t dest_x,
    uint32_t dest_y, uint32_t copy_width, uint32_t copy_height, xenos::Endian128 dest_endian) {
  auto log_write_failure = [&](const char* reason) {
    uint64_t surface_bytes = uint64_t(pitch) * height * 4;
    const bool watched_framebuffer =
        RangesOverlap(dest_base, uint32_t(std::min<uint64_t>(surface_bytes, UINT32_MAX)),
                      kWatchedFramebufferBase, kWatchedFramebufferLength);
    const bool watched_resolve =
        RangesOverlap(dest_base, uint32_t(std::min<uint64_t>(surface_bytes, UINT32_MAX)),
                      kWatchedResolveBase, kWatchedResolveLength);
    const bool watched_swap =
        RangesOverlap(dest_base, uint32_t(std::min<uint64_t>(surface_bytes, UINT32_MAX)),
                      kWatchedSwapBase, kWatchedSwapLength);
    if (!watched_framebuffer && !watched_resolve && !watched_swap) {
      return;
    }
    static std::atomic<uint32_t> resolve_write_fail_logs{0};
    uint32_t fail_index = resolve_write_fail_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (fail_index <= 32 || (fail_index & 0xFF) == 0) {
      std::fprintf(stderr,
                   "[metal] readback resolve write failed#%u reason=%s dest=0x%08x "
                   "surface=%ux%u source=%ux%u source_origin=%u,%u dest_origin=%u,%u "
                   "copy=%ux%u bgra_bytes=%zu watched(framebuffer=%u resolve=%u swap=%u)\n",
                   fail_index, reason, dest_base, pitch, height, width, source_height, source_x,
                   source_y, dest_x, dest_y, copy_width, copy_height, bgra.size(),
                   watched_framebuffer ? 1u : 0u, watched_resolve ? 1u : 0u,
                   watched_swap ? 1u : 0u);
      std::fflush(stderr);
    }
  };
  if (!memory_ || !dest_base || !pitch || !height || !width || !source_height ||
      bgra.size() < size_t(width) * source_height * 4) {
    log_write_failure("invalid-input");
    return false;
  }
  if (source_x >= width || source_y >= source_height || dest_x >= pitch || dest_y >= height) {
    log_write_failure("origin-out-of-range");
    return false;
  }
  uint32_t write_width = std::min({copy_width, width - source_x, pitch - dest_x});
  uint32_t write_height = std::min({copy_height, source_height - source_y, height - dest_y});
  if (!write_width || !write_height) {
    log_write_failure("empty-clipped-rect");
    return false;
  }
  uint8_t* dest = memory_->TranslatePhysical<uint8_t*>(dest_base);
  if (!dest) {
    log_write_failure("unmapped-dest");
    return false;
  }
  for (uint32_t y = 0; y < write_height; ++y) {
    uint32_t resolved_source_y = source_y + y;
    uint32_t resolved_dest_y = dest_y + y;
    const uint8_t* source_row = bgra.data() + size_t(resolved_source_y) * width * 4;
    for (uint32_t x = 0; x < write_width; ++x) {
      uint32_t resolved_source_x = source_x + x;
      uint32_t resolved_dest_x = dest_x + x;
      uint32_t tiled_offset = uint32_t(texture_util::GetTiledOffset2D(
          int32_t(resolved_dest_x), int32_t(resolved_dest_y), pitch, 2));
      const uint8_t* source = source_row + size_t(resolved_source_x) * 4;
      uint8_t* target = dest + tiled_offset;
      uint8_t packed[4];
      PackBgraForGuestRgba(source, dest_endian, packed);
      target[0] = packed[0];
      target[1] = packed[1];
      target[2] = packed[2];
      target[3] = packed[3];
    }
  }
  if (shared_memory_) {
    uint64_t surface_bytes = uint64_t(pitch) * height * 4;
    uint32_t clamped_start = std::min(dest_base, SharedMemory::kBufferSize);
    uint32_t clamped_length =
        uint32_t(std::min<uint64_t>(surface_bytes, SharedMemory::kBufferSize - clamped_start));
    if (clamped_length &&
        !shared_memory_->CommitGuestCpuWriteAsGpu(clamped_start, clamped_length)) {
      log_write_failure("shared-memory-commit");
      return false;
    }
  }
  uint64_t surface_bytes = uint64_t(pitch) * height * 4;
  uint32_t clamped_start = std::min(dest_base, SharedMemory::kBufferSize);
  uint32_t clamped_length =
      uint32_t(std::min<uint64_t>(surface_bytes, SharedMemory::kBufferSize - clamped_start));
  const bool watched_framebuffer = RangesOverlap(
      clamped_start, clamped_length, kWatchedFramebufferBase, kWatchedFramebufferLength);
  const bool watched_resolve =
      RangesOverlap(clamped_start, clamped_length, kWatchedResolveBase, kWatchedResolveLength);
  const bool watched_swap =
      RangesOverlap(clamped_start, clamped_length, kWatchedSwapBase, kWatchedSwapLength);
  if (watched_framebuffer || watched_resolve || watched_swap) {
    static std::atomic<uint32_t> watched_resolve_write_logs{0};
    uint32_t resolve_write_index =
        watched_resolve_write_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (resolve_write_index <= 32 || (resolve_write_index & 0xFF) == 0) {
      const uint8_t* watched_framebuffer_bytes =
          memory_->TranslatePhysical<const uint8_t*>(kWatchedFramebufferBase);
      WatchedGuestRgbaStats watched_decoded_stats = {};
      if (watched_framebuffer) {
        watched_decoded_stats = GetWatchedGuestRgbaStats(watched_framebuffer_bytes, dest_endian);
      }
      size_t watched_framebuffer_nonzero = 0;
      size_t watched_framebuffer_rgb_nonzero = 0;
      uint8_t watched_p00[4] = {};
      uint8_t watched_p25[4] = {};
      uint8_t watched_p50[4] = {};
      uint8_t watched_p75[4] = {};
      uint32_t copied_sample_matches = 0;
      uint32_t copied_sample_visible = 0;
      uint8_t copied_src_sample[4][4] = {};
      uint8_t copied_dst_sample[4][4] = {};
      if (watched_framebuffer_bytes) {
        for (uint32_t i = 0; i < kWatchedFramebufferLength; ++i) {
          uint8_t value = watched_framebuffer_bytes[i];
          watched_framebuffer_nonzero += value != 0;
          watched_framebuffer_rgb_nonzero += (i & 3u) != 3u && value != 0;
        }
        auto copy_watched_sample = [&](uint32_t byte_offset, uint8_t sample[4]) {
          byte_offset = std::min(byte_offset, kWatchedFramebufferLength - 4);
          std::memcpy(sample, watched_framebuffer_bytes + byte_offset, 4);
        };
        copy_watched_sample(0, watched_p00);
        copy_watched_sample(kWatchedFramebufferLength / 4, watched_p25);
        copy_watched_sample(kWatchedFramebufferLength / 2, watched_p50);
        copy_watched_sample((kWatchedFramebufferLength * 3) / 4, watched_p75);
        for (uint32_t sample_index = 0; sample_index < 4; ++sample_index) {
          uint32_t sample_dx = write_width > 1 ? ((write_width - 1) * sample_index) / 3 : 0;
          uint32_t sample_dy = write_height > 1 ? ((write_height - 1) * sample_index) / 3 : 0;
          uint32_t sample_source_x = source_x + sample_dx;
          uint32_t sample_source_y = source_y + sample_dy;
          uint32_t sample_dest_x = dest_x + sample_dx;
          uint32_t sample_dest_y = dest_y + sample_dy;
          if (sample_source_x >= width || sample_source_y >= source_height ||
              sample_dest_x >= pitch || sample_dest_y >= height) {
            continue;
          }
          const uint8_t* source_sample =
              bgra.data() + (size_t(sample_source_y) * width + sample_source_x) * 4;
          copied_src_sample[sample_index][0] = source_sample[2];
          copied_src_sample[sample_index][1] = source_sample[1];
          copied_src_sample[sample_index][2] = source_sample[0];
          copied_src_sample[sample_index][3] = source_sample[3];
          uint32_t tiled_offset = uint32_t(texture_util::GetTiledOffset2D(
              int32_t(sample_dest_x), int32_t(sample_dest_y), pitch, 2));
          UnpackGuestRgba(dest + tiled_offset, dest_endian, copied_dst_sample[sample_index]);
          copied_sample_visible +=
              std::max({copied_src_sample[sample_index][0], copied_src_sample[sample_index][1],
                        copied_src_sample[sample_index][2]}) > kVisibleRgbThreshold
                  ? 1u
                  : 0u;
          copied_sample_matches +=
              std::memcmp(copied_src_sample[sample_index], copied_dst_sample[sample_index],
                          sizeof(copied_src_sample[sample_index])) == 0
                  ? 1u
                  : 0u;
        }
      }
      std::fprintf(
          stderr,
          "[metal] watched GPU resolve write#%u dest=0x%08x "
          "range=0x%08x+0x%x framebuffer=%u resolve=%u swap=%u "
          "pitch=%u height=%u rect=%u,%u %ux%u source_origin=%u,%u "
          "source=%ux%u visible=%u watched_1ec30000(raw_nonzero=%zu raw_rgb=%zu "
          "p00=%02x %02x %02x %02x p25=%02x %02x %02x %02x "
          "p50=%02x %02x %02x %02x p75=%02x %02x %02x %02x "
          "decoded_visible=%u decoded_rgb=%zu decoded_range=%u "
          "rgba00=%02x %02x %02x %02x rgba25=%02x %02x %02x %02x "
          "rgba50=%02x %02x %02x %02x rgba75=%02x %02x %02x %02x "
          "copy_match=%u/4 copy_visible_samples=%u "
          "src00=%02x %02x %02x %02x dst00=%02x %02x %02x %02x "
          "src75=%02x %02x %02x %02x dst75=%02x %02x %02x %02x)\n",
          resolve_write_index, dest_base, clamped_start, clamped_length,
          watched_framebuffer ? 1u : 0u, watched_resolve ? 1u : 0u, watched_swap ? 1u : 0u, pitch,
          height, dest_x, dest_y, write_width, write_height, source_x, source_y, width,
          source_height, CountVisibleRgbPixels(bgra), watched_framebuffer_nonzero,
          watched_framebuffer_rgb_nonzero, watched_p00[0], watched_p00[1], watched_p00[2],
          watched_p00[3], watched_p25[0], watched_p25[1], watched_p25[2], watched_p25[3],
          watched_p50[0], watched_p50[1], watched_p50[2], watched_p50[3], watched_p75[0],
          watched_p75[1], watched_p75[2], watched_p75[3], watched_decoded_stats.visible_pixels,
          watched_decoded_stats.rgb_nonzero_components,
          watched_framebuffer
              ? uint32_t(watched_decoded_stats.max_rgb) - uint32_t(watched_decoded_stats.min_rgb)
              : 0u,
          watched_decoded_stats.samples[0][0], watched_decoded_stats.samples[0][1],
          watched_decoded_stats.samples[0][2], watched_decoded_stats.samples[0][3],
          watched_decoded_stats.samples[1][0], watched_decoded_stats.samples[1][1],
          watched_decoded_stats.samples[1][2], watched_decoded_stats.samples[1][3],
          watched_decoded_stats.samples[2][0], watched_decoded_stats.samples[2][1],
          watched_decoded_stats.samples[2][2], watched_decoded_stats.samples[2][3],
          watched_decoded_stats.samples[3][0], watched_decoded_stats.samples[3][1],
          watched_decoded_stats.samples[3][2], watched_decoded_stats.samples[3][3],
          copied_sample_matches, copied_sample_visible, copied_src_sample[0][0],
          copied_src_sample[0][1], copied_src_sample[0][2], copied_src_sample[0][3],
          copied_dst_sample[0][0], copied_dst_sample[0][1], copied_dst_sample[0][2],
          copied_dst_sample[0][3], copied_src_sample[3][0], copied_src_sample[3][1],
          copied_src_sample[3][2], copied_src_sample[3][3], copied_dst_sample[3][0],
          copied_dst_sample[3][1], copied_dst_sample[3][2], copied_dst_sample[3][3]);
      std::fflush(stderr);
    }
  }
  RetainResolvedFrameForBase(dest_base, bgra, width, source_height, "resolved write");
  return true;
}

bool MetalCommandProcessor::EnsureResolvedColorBacking(uint32_t width, uint32_t height) {
  if (!width || !height) {
    return false;
  }
  if (resolved_color_width_ == width && resolved_color_height_ == height &&
      resolved_color_bgra_.size() == size_t(width) * height * 4) {
    return true;
  }
  resolved_color_width_ = width;
  resolved_color_height_ = height;
  resolved_color_bgra_.assign(size_t(width) * height * 4, 0);
  for (size_t i = 3; i < resolved_color_bgra_.size(); i += 4) {
    resolved_color_bgra_[i] = 0xFF;
  }
  static std::atomic<uint32_t> resolved_backing_logs{0};
  uint32_t resolved_backing_index =
      resolved_backing_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (resolved_backing_index <= 8 || (resolved_backing_index & 0x3F) == 0) {
    std::fprintf(stderr, "[metal] resolved color backing#%u size=%ux%u bytes=%zu\n",
                 resolved_backing_index, width, height, resolved_color_bgra_.size());
    std::fflush(stderr);
  }
  return true;
}

bool MetalCommandProcessor::BlitToResolvedColorBacking(const std::vector<uint8_t>& bgra,
                                                       uint32_t width, uint32_t height,
                                                       uint32_t rect_x, uint32_t rect_y,
                                                       uint32_t rect_width, uint32_t rect_height) {
  if (!EnsureResolvedColorBacking(std::max(width, fallback_output_width_),
                                  std::max(height, fallback_output_height_)) ||
      bgra.size() < size_t(width) * height * 4 || rect_x >= width || rect_y >= height ||
      rect_x >= resolved_color_width_ || rect_y >= resolved_color_height_) {
    return false;
  }
  uint32_t copy_width = std::min({rect_width, width - rect_x, resolved_color_width_ - rect_x});
  uint32_t copy_height = std::min({rect_height, height - rect_y, resolved_color_height_ - rect_y});
  if (!copy_width || !copy_height) {
    return false;
  }
  for (uint32_t y = 0; y < copy_height; ++y) {
    const uint8_t* source = bgra.data() + (size_t(rect_y + y) * width + rect_x) * 4;
    uint8_t* target =
        resolved_color_bgra_.data() + (size_t(rect_y + y) * resolved_color_width_ + rect_x) * 4;
    std::memcpy(target, source, size_t(copy_width) * 4);
  }
  return true;
}

bool MetalCommandProcessor::CompositeVisibleToResolvedColorBacking(const std::vector<uint8_t>& bgra,
                                                                   uint32_t width,
                                                                   uint32_t height) {
  if (!EnsureResolvedColorBacking(std::max(width, fallback_output_width_),
                                  std::max(height, fallback_output_height_)) ||
      bgra.size() < size_t(width) * height * 4) {
    return false;
  }
  BgraFrameStats source_stats = GetBgraFrameStats(bgra);
  BgraFrameStats target_stats = GetBgraFrameStats(resolved_color_bgra_);
  bool fill_only =
      target_stats.visible_pixels > source_stats.visible_pixels + source_stats.visible_pixels / 8;
  uint32_t copy_width = std::min(width, resolved_color_width_);
  uint32_t copy_height = std::min(height, resolved_color_height_);
  uint32_t copied_pixels = 0;
  uint32_t skipped_existing_pixels = 0;
  for (uint32_t y = 0; y < copy_height; ++y) {
    const uint8_t* source_row = bgra.data() + size_t(y) * width * 4;
    uint8_t* target_row = resolved_color_bgra_.data() + size_t(y) * resolved_color_width_ * 4;
    for (uint32_t x = 0; x < copy_width; ++x) {
      const uint8_t* source = source_row + size_t(x) * 4;
      if (!source[0] && !source[1] && !source[2]) {
        continue;
      }
      uint8_t* target = target_row + size_t(x) * 4;
      if (fill_only && (target[0] || target[1] || target[2])) {
        ++skipped_existing_pixels;
        continue;
      }
      target[0] = source[0];
      target[1] = source[1];
      target[2] = source[2];
      target[3] = source[3];
      ++copied_pixels;
    }
  }
  static std::atomic<uint32_t> composite_logs{0};
  uint32_t composite_index = composite_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (copied_pixels && (composite_index <= 16 || (composite_index & 0x3F) == 0)) {
    std::fprintf(stderr,
                 "[metal] composited pipeline probe#%u into resolved backing size=%ux%u "
                 "visible=%u fill_only=%u skipped_existing=%u source_visible=%u "
                 "target_visible=%u\n",
                 composite_index, width, height, copied_pixels, fill_only ? 1u : 0u,
                 skipped_existing_pixels, source_stats.visible_pixels, target_stats.visible_pixels);
    std::fflush(stderr);
  }
  return copied_pixels != 0;
}

bool MetalCommandProcessor::RefreshPipelineProbeBacking(uint32_t width, uint32_t height) {
  if (!pipeline_probe_context_ || !width || !height) {
    return false;
  }
  std::vector<uint8_t> probe_bgra;
  std::string read_error;
  if (!ReadPipelineProbeContext(pipeline_probe_context_, width, height, probe_bgra, &read_error)) {
    static std::atomic<uint32_t> probe_read_fail_logs{0};
    uint32_t probe_read_fail_index =
        probe_read_fail_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (probe_read_fail_index <= 4 || (probe_read_fail_index & 0xFF) == 0) {
      std::fprintf(stderr, "[metal] persistent probe read skipped#%u: %s\n", probe_read_fail_index,
                   read_error.c_str());
      std::fflush(stderr);
    }
    return false;
  }
  uint32_t visible = CountVisibleRgbPixels(probe_bgra);
  if (!visible) {
    return false;
  }
  latest_pipeline_probe_bgra_ = std::move(probe_bgra);
  latest_pipeline_probe_width_ = width;
  latest_pipeline_probe_height_ = height;
  resolved_color_bgra_ = latest_pipeline_probe_bgra_;
  resolved_color_width_ = width;
  resolved_color_height_ = height;
  static std::atomic<uint32_t> probe_read_logs{0};
  uint32_t probe_read_index = probe_read_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (probe_read_index <= 16 || (probe_read_index & 0x3F) == 0) {
    std::fprintf(stderr, "[metal] read persistent pipeline probe#%u size=%ux%u visible=%u\n",
                 probe_read_index, width, height, visible);
    std::fflush(stderr);
  }
  return true;
}

bool MetalCommandProcessor::RefreshHostRenderTargetBacking(uint32_t width, uint32_t height) {
  if (!host_render_target_context_ || !width || !height) {
    return false;
  }
  std::vector<uint8_t> host_rt_bgra;
  std::string read_error;
  if (!ReadPipelineProbeContext(host_render_target_context_, width, height, host_rt_bgra,
                                &read_error)) {
    static std::atomic<uint32_t> host_rt_read_fail_logs{0};
    uint32_t host_rt_read_fail_index =
        host_rt_read_fail_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (host_rt_read_fail_index <= 4 || (host_rt_read_fail_index & 0xFF) == 0) {
      std::fprintf(stderr, "[metal] host render target read skipped#%u: %s\n",
                   host_rt_read_fail_index, read_error.c_str());
      std::fflush(stderr);
    }
    return false;
  }
  uint32_t visible = CountVisibleRgbPixels(host_rt_bgra);
  latest_host_render_target_bgra_ = std::move(host_rt_bgra);
  latest_host_render_target_width_ = width;
  latest_host_render_target_height_ = height;
  static std::atomic<uint32_t> host_rt_read_logs{0};
  uint32_t host_rt_read_index = host_rt_read_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (host_rt_read_index <= 16 || (host_rt_read_index & 0x3F) == 0) {
    std::fprintf(stderr, "[metal] read host render target debug#%u size=%ux%u visible=%u\n",
                 host_rt_read_index, width, height, visible);
    std::fflush(stderr);
  }
  return true;
}

bool MetalCommandProcessor::EnsureEdramBgraBacking() {
  if (edram_bgra_.size() == xenos::kEdramSizeBytes) {
    return true;
  }
  edram_bgra_.assign(xenos::kEdramSizeBytes, 0);
  static std::atomic<uint32_t> edram_backing_logs{0};
  uint32_t edram_backing_index = edram_backing_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (edram_backing_index <= 4 || (edram_backing_index & 0x3F) == 0) {
    std::fprintf(stderr, "[metal] edram backing#%u bytes=%zu\n", edram_backing_index,
                 edram_bgra_.size());
    std::fflush(stderr);
  }
  return true;
}

bool MetalCommandProcessor::DumpHostRenderTargetToEdram(const HostRenderTarget& target) {
  if (!EnsureEdramBgraBacking() || target.bgra.empty() || !target.width || !target.height ||
      target.bgra.size() < size_t(target.width) * target.height * 4) {
    return false;
  }
  reg::RB_COLOR_INFO color_info = {};
  color_info.value = target.color_info;
  xenos::ColorRenderTargetFormat color_format =
      xenos::GetStorageColorFormat(color_info.color_format);
  if (color_format != xenos::ColorRenderTargetFormat::k_8_8_8_8 &&
      color_format != xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
    static std::atomic<uint32_t> unsupported_dump_logs{0};
    uint32_t unsupported_dump_index =
        unsupported_dump_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (unsupported_dump_index <= 8 || (unsupported_dump_index & 0xFF) == 0) {
      std::fprintf(stderr,
                   "[metal] edram dump skipped#%u rt=%u unsupported color_format=%u "
                   "color=0x%08x surface=0x%08x\n",
                   unsupported_dump_index, target.rt_index, uint32_t(color_info.color_format),
                   target.color_info, target.surface_info);
      std::fflush(stderr);
    }
    return false;
  }
  reg::RB_SURFACE_INFO surface_info = {};
  surface_info.value = target.surface_info;
  uint32_t pitch_tiles =
      xenos::GetSurfacePitchTiles(surface_info.surface_pitch, surface_info.msaa_samples, false);
  if (!pitch_tiles) {
    return false;
  }
  uint32_t base_tiles =
      (color_info.color_base | (color_info.color_base_bit_11 << 11)) & (xenos::kEdramTileCount - 1);
  uint32_t sample_x_log2 = uint32_t(surface_info.msaa_samples >= xenos::MsaaSamples::k4X);
  uint32_t sample_y_log2 = uint32_t(surface_info.msaa_samples >= xenos::MsaaSamples::k2X);
  uint32_t sample_count = 1u << uint32_t(surface_info.msaa_samples);
  uint32_t dump_width = std::min(target.width, surface_info.surface_pitch);
  uint32_t visible = CountVisibleRgbPixels(target.bgra);
  for (uint32_t y = 0; y < target.height; ++y) {
    const uint8_t* source_row = target.bgra.data() + size_t(y) * target.width * 4;
    for (uint32_t x = 0; x < dump_width; ++x) {
      const uint8_t* source = source_row + size_t(x) * 4;
      for (uint32_t sample = 0; sample < sample_count; ++sample) {
        uint32_t sample_x =
            (x << sample_x_log2) + (sample_x_log2 ? (sample & ((1u << sample_x_log2) - 1u)) : 0u);
        uint32_t sample_y = (y << sample_y_log2) + (sample >> sample_x_log2);
        uint32_t tile_x = sample_x / xenos::kEdramTileWidthSamples;
        uint32_t tile_y = sample_y / xenos::kEdramTileHeightSamples;
        uint32_t tile = (base_tiles + tile_y * pitch_tiles + tile_x) & (xenos::kEdramTileCount - 1);
        uint32_t tile_sample_x = sample_x % xenos::kEdramTileWidthSamples;
        uint32_t tile_sample_y = sample_y % xenos::kEdramTileHeightSamples;
        size_t offset =
            (size_t(tile) * xenos::kEdramTileWidthSamples * xenos::kEdramTileHeightSamples +
             size_t(tile_sample_y) * xenos::kEdramTileWidthSamples + tile_sample_x) *
            4;
        std::memcpy(edram_bgra_.data() + offset, source, 4);
      }
    }
  }
  static std::atomic<uint32_t> edram_dump_logs{0};
  uint32_t edram_dump_index = edram_dump_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (edram_dump_index <= 32 || (edram_dump_index & 0xFF) == 0) {
    std::fprintf(stderr,
                 "[metal] edram dump#%u rt=%u base=%u pitch_tiles=%u msaa=%u "
                 "size=%ux%u dump_width=%u visible=%u color=0x%08x surface=0x%08x\n",
                 edram_dump_index, target.rt_index, base_tiles, pitch_tiles,
                 uint32_t(surface_info.msaa_samples), target.width, target.height, dump_width,
                 visible, target.color_info, target.surface_info);
    std::fflush(stderr);
  }
  return true;
}

bool MetalCommandProcessor::ResolveEdramToBgra(const draw_util::ResolveInfo& resolve_info,
                                               uint32_t width, uint32_t height,
                                               std::vector<uint8_t>& bgra_out) {
  if (!EnsureEdramBgraBacking() || !width || !height || resolve_info.IsCopyingDepth()) {
    return false;
  }
  draw_util::ResolveCopyShaderConstants copy_shader_constants = {};
  uint32_t copy_group_count_x = 0;
  uint32_t copy_group_count_y = 0;
  draw_util::ResolveCopyShaderIndex copy_shader =
      resolve_info.GetCopyShader(texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1,
                                 texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1,
                                 copy_shader_constants, copy_group_count_x, copy_group_count_y);
  (void)copy_shader_constants;
  if (copy_shader == draw_util::ResolveCopyShaderIndex::kUnknown) {
    return false;
  }
  draw_util::ResolveEdramInfo edram_info = resolve_info.color_edram_info;
  xenos::ColorRenderTargetFormat color_format =
      xenos::GetStorageColorFormat(xenos::ColorRenderTargetFormat(edram_info.format));
  if (color_format != xenos::ColorRenderTargetFormat::k_8_8_8_8 &&
      color_format != xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
    return false;
  }
  uint32_t sample_x_log2 = uint32_t(edram_info.msaa_samples >= xenos::MsaaSamples::k4X);
  uint32_t sample_y_log2 = uint32_t(edram_info.msaa_samples >= xenos::MsaaSamples::k2X);
  uint32_t selected_sample = uint32_t(resolve_info.copy_dest_coordinate_info.copy_sample_select);
  if (selected_sample > uint32_t(xenos::CopySampleSelect::k3)) {
    selected_sample =
        resolve_info.copy_dest_coordinate_info.copy_sample_select == xenos::CopySampleSelect::k23
            ? 2u
            : 0u;
  }
  uint32_t max_sample = (1u << uint32_t(edram_info.msaa_samples)) - 1u;
  selected_sample = std::min(selected_sample, max_sample);
  bgra_out.assign(size_t(width) * height * 4, 0);
  uint32_t base_pixel_x = uint32_t(resolve_info.coordinate_info.edram_offset_x_div_8) * 8;
  uint32_t base_pixel_y = uint32_t(resolve_info.coordinate_info.edram_offset_y_div_8) * 8;
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t* dest_row = bgra_out.data() + size_t(y) * width * 4;
    for (uint32_t x = 0; x < width; ++x) {
      uint32_t pixel_x = base_pixel_x + x;
      uint32_t pixel_y = base_pixel_y + y;
      uint32_t sample_x = (pixel_x << sample_x_log2) +
                          (sample_x_log2 ? (selected_sample & ((1u << sample_x_log2) - 1u)) : 0u);
      uint32_t sample_y = (pixel_y << sample_y_log2) + (selected_sample >> sample_x_log2);
      uint32_t tile_x = sample_x / xenos::kEdramTileWidthSamples;
      uint32_t tile_y = sample_y / xenos::kEdramTileHeightSamples;
      uint32_t tile = (edram_info.base_tiles + tile_y * edram_info.pitch_tiles + tile_x) &
                      (xenos::kEdramTileCount - 1);
      uint32_t tile_sample_x = sample_x % xenos::kEdramTileWidthSamples;
      uint32_t tile_sample_y = sample_y % xenos::kEdramTileHeightSamples;
      size_t source_offset =
          (size_t(tile) * xenos::kEdramTileWidthSamples * xenos::kEdramTileHeightSamples +
           size_t(tile_sample_y) * xenos::kEdramTileWidthSamples + tile_sample_x) *
          4;
      std::memcpy(dest_row + size_t(x) * 4, edram_bgra_.data() + source_offset, 4);
    }
  }
  static std::atomic<uint32_t> edram_resolve_logs{0};
  uint32_t edram_resolve_index = edram_resolve_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (edram_resolve_index <= 32 || (edram_resolve_index & 0xFF) == 0) {
    BgraFrameStats stats = GetBgraFrameStats(bgra_out);
    std::fprintf(stderr,
                 "[metal] edram readback resolve#%u shader=%s groups=%ux%u "
                 "base=%u pitch_tiles=%u msaa=%u sample=%u size=%ux%u visible=%u range=%u\n",
                 edram_resolve_index,
                 draw_util::resolve_copy_shader_info[size_t(copy_shader)].debug_name,
                 copy_group_count_x, copy_group_count_y, edram_info.base_tiles,
                 edram_info.pitch_tiles, uint32_t(edram_info.msaa_samples), selected_sample, width,
                 height, stats.visible_pixels, BgraRgbRange(stats));
    std::fflush(stderr);
  }
  return true;
}

uint64_t MetalCommandProcessor::GetHostRenderTargetKey(uint32_t rt_index) const {
  if (!register_file_ || rt_index >= xenos::kMaxColorRenderTargets) {
    return 0;
  }
  reg::RB_SURFACE_INFO surface_info = register_file_->Get<reg::RB_SURFACE_INFO>();
  reg::RB_COLOR_INFO color_info =
      register_file_->Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[rt_index]);
  if (!surface_info.surface_pitch) {
    return 0;
  }
  uint32_t key_parts[3] = {rt_index, color_info.value, surface_info.value};
  return XXH3_64bits(key_parts, sizeof(key_parts));
}

MetalCommandProcessor::HostRenderTarget* MetalCommandProcessor::FindHostRenderTarget(
    uint32_t rt_index) {
  uint64_t key = GetHostRenderTargetKey(rt_index);
  if (!key) {
    return nullptr;
  }
  auto it = host_render_targets_.find(key);
  return it == host_render_targets_.end() ? nullptr : &it->second;
}

MetalCommandProcessor::HostRenderTarget* MetalCommandProcessor::FindHostRenderTargetForResolve(
    uint32_t rt_index, uint32_t color_base, uint32_t color_format, uint32_t min_surface_pitch,
    xenos::MsaaSamples msaa_samples) {
  if (HostRenderTarget* exact_target = FindHostRenderTarget(rt_index)) {
    return exact_target;
  }
  HostRenderTarget* best_target = nullptr;
  uint32_t best_pitch = 0;
  for (auto& [key, target] : host_render_targets_) {
    (void)key;
    if (!target.context || target.rt_index != rt_index || !target.width || !target.height) {
      continue;
    }
    reg::RB_COLOR_INFO stored_color = {};
    stored_color.value = target.color_info;
    uint32_t stored_color_base = stored_color.color_base | (stored_color.color_base_bit_11 << 11);
    if ((stored_color_base & (xenos::kEdramTileCount - 1)) !=
            (color_base & (xenos::kEdramTileCount - 1)) ||
        uint32_t(stored_color.color_format) != color_format) {
      continue;
    }
    reg::RB_SURFACE_INFO stored_surface = {};
    stored_surface.value = target.surface_info;
    if (stored_surface.surface_pitch < min_surface_pitch) {
      continue;
    }
    if (!best_target || stored_surface.surface_pitch < best_pitch) {
      best_target = &target;
      best_pitch = stored_surface.surface_pitch;
    }
  }
  if (best_target) {
    static std::atomic<uint32_t> resolve_rt_owner_logs{0};
    uint32_t owner_index = resolve_rt_owner_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (owner_index <= 16 || (owner_index & 0xFF) == 0) {
      reg::RB_SURFACE_INFO owner_surface = {};
      owner_surface.value = best_target->surface_info;
      std::fprintf(stderr,
                   "[metal] resolve RT owner#%u rt=%u color_base=%u fmt=%u msaa=%u "
                   "min_pitch=%u owner=%ux%u color=0x%08x surface=0x%08x owner_msaa=%u\n",
                   owner_index, rt_index, color_base, color_format, uint32_t(msaa_samples),
                   min_surface_pitch, best_target->width, best_target->height,
                   best_target->color_info, best_target->surface_info,
                   uint32_t(owner_surface.msaa_samples));
      std::fflush(stderr);
    }
  }
  return best_target;
}

MetalCommandProcessor::HostRenderTarget* MetalCommandProcessor::EnsureHostRenderTarget(
    uint32_t rt_index) {
  uint64_t key = GetHostRenderTargetKey(rt_index);
  if (!key || !metal_device_ || !register_file_) {
    return nullptr;
  }
  auto [it, inserted] = host_render_targets_.try_emplace(key);
  (void)inserted;
  HostRenderTarget& target = it->second;
  if (target.context) {
    return &target;
  }
  std::string context_error;
  target.context = CreateHostRenderTargetContext(metal_device_, &context_error);
  target.rt_index = rt_index;
  target.color_info =
      register_file_->Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[rt_index])
          .value;
  target.surface_info = register_file_->Get<reg::RB_SURFACE_INFO>().value;
  if (!target.context) {
    host_render_targets_.erase(it);
    static std::atomic<uint32_t> rt_context_fail_logs{0};
    uint32_t rt_context_fail_index =
        rt_context_fail_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (rt_context_fail_index <= 8 || (rt_context_fail_index & 0x3F) == 0) {
      std::fprintf(stderr, "[metal] host RT context failed#%u rt=%u: %s\n", rt_context_fail_index,
                   rt_index, context_error.c_str());
      std::fflush(stderr);
    }
    return nullptr;
  }
  static std::atomic<uint32_t> rt_context_logs{0};
  uint32_t rt_context_index = rt_context_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (rt_context_index <= 16 || (rt_context_index & 0x3F) == 0) {
    std::fprintf(stderr,
                 "[metal] host RT context#%u rt=%u key=%016llx color=0x%08x surface=0x%08x\n",
                 rt_context_index, rt_index, static_cast<unsigned long long>(key),
                 target.color_info, target.surface_info);
    std::fflush(stderr);
  }
  return &target;
}

bool MetalCommandProcessor::BlitAndWriteResolvedColor(uint32_t dest_base, uint32_t pitch,
                                                      uint32_t height,
                                                      const std::vector<uint8_t>& bgra,
                                                      uint32_t width, uint32_t source_height,
                                                      uint32_t rect_x, uint32_t rect_y,
                                                      uint32_t rect_width, uint32_t rect_height) {
  if (!BlitToResolvedColorBacking(bgra, width, source_height, rect_x, rect_y, rect_width,
                                  rect_height)) {
    return false;
  }
  bool wrote_dest = WriteBgraToTiledResolveRect(dest_base, pitch, height, resolved_color_bgra_,
                                                resolved_color_width_, resolved_color_height_,
                                                rect_x, rect_y, rect_width, rect_height);
  if (wrote_dest && last_swap_frontbuffer_ptr_ && last_swap_frontbuffer_ptr_ != dest_base &&
      fallback_output_width_ && fallback_output_height_) {
    WriteBgraToTiledResolveRect(last_swap_frontbuffer_ptr_, fallback_output_width_,
                                fallback_output_height_, resolved_color_bgra_,
                                resolved_color_width_, resolved_color_height_, rect_x, rect_y,
                                rect_width, rect_height);
  }
  return wrote_dest;
}

bool MetalCommandProcessor::CompositeAndWriteResolvedColor(
    uint32_t dest_base, uint32_t pitch, uint32_t height, const std::vector<uint8_t>& bgra,
    uint32_t width, uint32_t source_height, uint32_t rect_x, uint32_t rect_y, uint32_t rect_width,
    uint32_t rect_height) {
  if (latest_draw_event_frame_width_ == fallback_output_width_ &&
      latest_draw_event_frame_height_ == fallback_output_height_ &&
      latest_draw_event_frame_bgra_.size() ==
          size_t(fallback_output_width_) * fallback_output_height_ * 4 &&
      resolved_color_width_ == fallback_output_width_ &&
      resolved_color_height_ == fallback_output_height_ &&
      resolved_color_bgra_.size() == latest_draw_event_frame_bgra_.size()) {
    BgraFrameStats retained_stats = GetBgraFrameStats(latest_draw_event_frame_bgra_);
    BgraFrameStats current_stats = GetBgraFrameStats(resolved_color_bgra_);
    if (retained_stats.visible_pixels > current_stats.visible_pixels) {
      resolved_color_bgra_ = latest_draw_event_frame_bgra_;
    }
  }
  if (!EnsureResolvedColorBacking(std::max(width, fallback_output_width_),
                                  std::max(source_height, fallback_output_height_)) ||
      bgra.size() < size_t(width) * source_height * 4 || rect_x >= width ||
      rect_y >= source_height || rect_x >= resolved_color_width_ ||
      rect_y >= resolved_color_height_) {
    return false;
  }
  BgraFrameStats source_stats = GetBgraFrameStats(bgra);
  BgraFrameStats target_stats = GetBgraFrameStats(resolved_color_bgra_);
  bool fill_only =
      target_stats.visible_pixels > source_stats.visible_pixels + source_stats.visible_pixels / 8;
  uint32_t copy_width = std::min({rect_width, width - rect_x, resolved_color_width_ - rect_x});
  uint32_t copy_height =
      std::min({rect_height, source_height - rect_y, resolved_color_height_ - rect_y});
  if (!copy_width || !copy_height) {
    return false;
  }
  uint32_t copied_pixels = 0;
  uint32_t skipped_existing_pixels = 0;
  for (uint32_t y = 0; y < copy_height; ++y) {
    const uint8_t* source_row = bgra.data() + size_t(rect_y + y) * width * 4;
    uint8_t* target_row =
        resolved_color_bgra_.data() + size_t(rect_y + y) * resolved_color_width_ * 4;
    for (uint32_t x = 0; x < copy_width; ++x) {
      const uint8_t* source = source_row + size_t(rect_x + x) * 4;
      if (!source[0] && !source[1] && !source[2]) {
        continue;
      }
      uint8_t* target = target_row + size_t(rect_x + x) * 4;
      if (fill_only && (target[0] || target[1] || target[2])) {
        ++skipped_existing_pixels;
        continue;
      }
      target[0] = source[0];
      target[1] = source[1];
      target[2] = source[2];
      target[3] = source[3];
      ++copied_pixels;
    }
  }
  if (!copied_pixels) {
    return false;
  }
  static std::atomic<uint32_t> composite_rect_logs{0};
  uint32_t composite_rect_index = composite_rect_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (composite_rect_index <= 16 || (composite_rect_index & 0x3F) == 0) {
    std::fprintf(stderr,
                 "[metal] composited resolve rect#%u rect=%u,%u %ux%u copied=%u "
                 "fill_only=%u skipped_existing=%u source_visible=%u target_visible=%u\n",
                 composite_rect_index, rect_x, rect_y, copy_width, copy_height, copied_pixels,
                 fill_only ? 1u : 0u, skipped_existing_pixels, source_stats.visible_pixels,
                 target_stats.visible_pixels);
    std::fflush(stderr);
  }
  bool wrote_dest = WriteBgraToTiledResolveRect(dest_base, pitch, height, resolved_color_bgra_,
                                                resolved_color_width_, resolved_color_height_,
                                                rect_x, rect_y, copy_width, copy_height);
  if (wrote_dest && last_swap_frontbuffer_ptr_ && last_swap_frontbuffer_ptr_ != dest_base &&
      fallback_output_width_ && fallback_output_height_) {
    WriteBgraToTiledResolveRect(last_swap_frontbuffer_ptr_, fallback_output_width_,
                                fallback_output_height_, resolved_color_bgra_,
                                resolved_color_width_, resolved_color_height_, rect_x, rect_y,
                                copy_width, copy_height);
  }
  return wrote_dest;
}

bool MetalCommandProcessor::DecodeResolvedColorBackingToRgba(uint32_t base_physical, uint32_t width,
                                                             uint32_t height,
                                                             std::vector<uint8_t>& rgba_out) {
  if (!base_physical || !width || !height) {
    return false;
  }
  if (width != fallback_output_width_ || height != fallback_output_height_) {
    return false;
  }
  RefreshPipelineProbeBacking(width, height);
  if (base_physical != last_copy_dest_base_ && base_physical != last_swap_frontbuffer_ptr_) {
    return false;
  }

  const std::vector<uint8_t>* best_bgra = nullptr;
  const char* best_label = nullptr;
  BgraFrameStats best_stats = {};
  uint64_t best_score = 0;
  auto consider_candidate = [&](const std::vector<uint8_t>& candidate, uint32_t candidate_width,
                                uint32_t candidate_height, const char* label) {
    if (candidate_width != width || candidate_height != height ||
        candidate.size() < size_t(width) * height * 4) {
      return;
    }
    BgraFrameStats stats = GetBgraFrameStats(candidate);
    if (std::strcmp(label, "fullscreen") == 0 && stats.visible_pixels == width * height &&
        BgraSpatialSampleColorDistance(candidate, candidate_width, candidate_height) <= 8) {
      static std::atomic<uint32_t> resolved_flat_fullscreen_skip_logs{0};
      uint32_t resolved_flat_fullscreen_skip_index =
          resolved_flat_fullscreen_skip_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (resolved_flat_fullscreen_skip_index <= 16 ||
          (resolved_flat_fullscreen_skip_index & 0x3F) == 0) {
        std::fprintf(stderr,
                     "[metal] skipped resolved spatially-flat fullscreen candidate#%u "
                     "visible=%u range=%u\n",
                     resolved_flat_fullscreen_skip_index, stats.visible_pixels,
                     BgraRgbRange(stats));
        std::fflush(stderr);
      }
      return;
    }
    uint32_t spatial_distance =
        BgraVisibleSpatialSampleColorDistance(candidate, candidate_width, candidate_height);
    uint64_t score = CandidateFrameScore(stats, spatial_distance);
    if (score > best_score) {
      best_bgra = &candidate;
      best_label = label;
      best_stats = stats;
      best_score = score;
    }
  };
  consider_candidate(resolved_color_bgra_, resolved_color_width_, resolved_color_height_,
                     "resolved");
  consider_candidate(latest_draw_event_frame_bgra_, latest_draw_event_frame_width_,
                     latest_draw_event_frame_height_, "draw");
  consider_candidate(latest_fullscreen_postprocess_bgra_, latest_fullscreen_postprocess_width_,
                     latest_fullscreen_postprocess_height_, "fullscreen");
  consider_candidate(latest_pipeline_probe_bgra_, latest_pipeline_probe_width_,
                     latest_pipeline_probe_height_, "probe");

  if (!best_bgra || best_stats.visible_pixels < (width * height) / 4) {
    return false;
  }

  BgraToRgba(*best_bgra, width, height, rgba_out);
  static std::atomic<uint32_t> resolved_texture_logs{0};
  uint32_t resolved_texture_index =
      resolved_texture_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (resolved_texture_index <= 16 || (resolved_texture_index & 0x3F) == 0) {
    std::fprintf(stderr,
                 "[metal] decoded texture from resolved backing#%u base=0x%08x size=%ux%u "
                 "visible=%u range=%u score=%llu source=%s\n",
                 resolved_texture_index, base_physical, width, height, best_stats.visible_pixels,
                 BgraRgbRange(best_stats), static_cast<unsigned long long>(best_score),
                 best_label ? best_label : "unknown");
    std::fflush(stderr);
  }
  return true;
}

void MetalCommandProcessor::RetainResolvedFrameForBase(uint32_t base_physical,
                                                       const std::vector<uint8_t>& bgra,
                                                       uint32_t width, uint32_t height,
                                                       const char* label) {
  if (!base_physical || !width || !height || bgra.size() < size_t(width) * height * 4) {
    return;
  }
  BgraFrameStats stats = GetBgraFrameStats(bgra);
  if (!stats.visible_pixels || stats.visible_pixels < (width * height) / 4 ||
      IsDominantFlatVisibleFrame(bgra, width, height, stats.visible_pixels)) {
    return;
  }
  uint32_t spatial_distance = BgraVisibleSpatialSampleColorDistance(bgra, width, height);
  uint64_t score = CandidateFrameScore(stats, spatial_distance);
  auto existing_it = retained_resolve_frames_by_base_.find(base_physical);
  if (existing_it != retained_resolve_frames_by_base_.end()) {
    const RetainedResolvedFrame& existing = existing_it->second;
    if (existing.score >= score && existing.visible_pixels >= stats.visible_pixels) {
      return;
    }
    if (existing.score > score + score / 8) {
      return;
    }
  }

  RetainedResolvedFrame& retained = retained_resolve_frames_by_base_[base_physical];
  retained.bgra = bgra;
  retained.label = label ? label : "unknown";
  retained.width = width;
  retained.height = height;
  retained.visible_pixels = stats.visible_pixels;
  retained.score = score;
  retained.draw_count = draw_count_;

  while (retained_resolve_frames_by_base_.size() > 8) {
    auto erase_it = retained_resolve_frames_by_base_.begin();
    for (auto it = retained_resolve_frames_by_base_.begin();
         it != retained_resolve_frames_by_base_.end(); ++it) {
      if (it->second.draw_count < erase_it->second.draw_count ||
          (it->second.draw_count == erase_it->second.draw_count &&
           it->second.score < erase_it->second.score)) {
        erase_it = it;
      }
    }
    retained_resolve_frames_by_base_.erase(erase_it);
  }

  static std::atomic<uint32_t> retained_resolve_base_logs{0};
  uint32_t retained_resolve_base_index =
      retained_resolve_base_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (retained_resolve_base_index <= 16 || (retained_resolve_base_index & 0x3F) == 0) {
    std::fprintf(stderr,
                 "[metal] retained resolved frame by base#%u base=0x%08x source=%s "
                 "size=%ux%u visible=%u score=%llu entries=%zu\n",
                 retained_resolve_base_index, base_physical, retained.label.c_str(), width, height,
                 retained.visible_pixels, static_cast<unsigned long long>(retained.score),
                 retained_resolve_frames_by_base_.size());
    std::fflush(stderr);
  }
}

void MetalCommandProcessor::InvalidateRetainedResolvedFrames(uint32_t base_physical,
                                                             uint32_t length) {
  if (!base_physical || !length || retained_resolve_frames_by_base_.empty()) {
    return;
  }
  uint64_t range_start = base_physical;
  uint64_t range_end = range_start + length;
  uint32_t invalidated = 0;
  for (auto it = retained_resolve_frames_by_base_.begin();
       it != retained_resolve_frames_by_base_.end();) {
    uint64_t frame_start = it->first;
    uint64_t frame_end = frame_start + uint64_t(it->second.width) * it->second.height * 4;
    if (range_start < frame_end && range_end > frame_start) {
      it = retained_resolve_frames_by_base_.erase(it);
      ++invalidated;
    } else {
      ++it;
    }
  }
  if (invalidated) {
    static std::atomic<uint32_t> invalidated_resolve_base_logs{0};
    uint32_t invalidated_resolve_base_index =
        invalidated_resolve_base_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (invalidated_resolve_base_index <= 16 || (invalidated_resolve_base_index & 0x3F) == 0) {
      std::fprintf(stderr,
                   "[metal] invalidated retained resolved frame base entries#%u "
                   "base=0x%08x length=0x%08x count=%u remaining=%zu\n",
                   invalidated_resolve_base_index, base_physical, length, invalidated,
                   retained_resolve_frames_by_base_.size());
      std::fflush(stderr);
    }
  }
}

MetalShader* MetalCommandProcessor::LoadShaderFromCache(xenos::ShaderType shader_type,
                                                        const uint32_t* host_address,
                                                        uint32_t dword_count, uint64_t data_hash) {
  auto it = shaders_.find(data_hash);
  if (it != shaders_.end()) {
    return it->second.get();
  }

  auto shader = std::make_unique<MetalShader>(shader_type, data_hash, host_address, dword_count);
  MetalShader* shader_ptr = shader.get();
  shaders_.emplace(data_hash, std::move(shader));
  REXLOG_INFO("Metal cached {} shader {:016X} ({} dwords)",
              shader_type == xenos::ShaderType::kVertex ? "vertex" : "pixel", data_hash,
              dword_count);
  return shader_ptr;
}

bool MetalCommandProcessor::EnsureShaderTranslated(MetalShader& shader, uint64_t modification) {
  if (!shader_translator_) {
    return false;
  }

  if (!shader.is_ucode_analyzed()) {
    shader.AnalyzeUcode(ucode_disasm_buffer_);
  }

  bool is_new_translation = false;
  auto* translation = static_cast<MetalShader::MetalTranslation*>(
      shader.GetOrCreateTranslation(modification, &is_new_translation));
  if (!is_new_translation) {
    return translation->is_valid();
  }

  if (!shader_translator_->TranslateAnalyzedShader(*translation)) {
    ++failed_shader_translation_count_;
    std::fprintf(stderr,
                 "[metal] SPIR-V translation failed shader=%016llx modification=%016llx "
                 "host_vs_type=%u errors=%zu\n",
                 static_cast<unsigned long long>(shader.ucode_data_hash()),
                 static_cast<unsigned long long>(modification),
                 shader.type() == xenos::ShaderType::kVertex
                     ? uint32_t(current_host_vertex_shader_type_)
                     : 0u,
                 translation->errors().size());
    for (const Shader::Error& error : translation->errors()) {
      std::fprintf(stderr, "[metal]   shader error fatal=%u: %s\n", error.is_fatal ? 1u : 0u,
                   error.message.c_str());
    }
    std::fflush(stderr);
    REXLOG_ERROR("Metal SPIR-V translation failed for shader {:016X} modification {:016X}",
                 shader.ucode_data_hash(), modification);
    return false;
  }

  ++translated_shader_count_;
  REXLOG_INFO("Metal translated {} shader {:016X} modification {:016X} to {} SPIR-V bytes",
              shader.type() == xenos::ShaderType::kVertex ? "vertex" : "pixel",
              shader.ucode_data_hash(), modification, translation->translated_binary().size());

  if (!translation->TranslateMslFromSpirv()) {
    ++failed_shader_translation_count_;
    std::fprintf(stderr,
                 "[metal] MSL translation failed shader=%016llx modification=%016llx "
                 "host_vs_type=%u\n",
                 static_cast<unsigned long long>(shader.ucode_data_hash()),
                 static_cast<unsigned long long>(modification),
                 shader.type() == xenos::ShaderType::kVertex
                     ? uint32_t(current_host_vertex_shader_type_)
                     : 0u);
    std::fflush(stderr);
    REXLOG_ERROR("Metal MSL translation failed for shader {:016X} modification {:016X}",
                 shader.ucode_data_hash(), modification);
    return false;
  }
  translation->set_host_disassembly(translation->msl_source());
  DumpTargetedMslSource(*translation);
  if (metal_device_) {
    std::string metal_compile_error;
    if (!translation->CompileMslLibrary(metal_device_, &metal_compile_error)) {
      ++failed_shader_translation_count_;
      DumpFailedMslSource(*translation);
      std::fprintf(stderr,
                   "[metal] MSL compile failed shader=%016llx modification=%016llx "
                   "host_vs_type=%u: %s\n",
                   static_cast<unsigned long long>(shader.ucode_data_hash()),
                   static_cast<unsigned long long>(modification),
                   shader.type() == xenos::ShaderType::kVertex
                       ? uint32_t(current_host_vertex_shader_type_)
                       : 0u,
                   metal_compile_error.c_str());
      std::fflush(stderr);
      REXLOG_ERROR("Metal MSL compile failed for shader {:016X} modification {:016X}: {}",
                   shader.ucode_data_hash(), modification, metal_compile_error);
      return false;
    }
  }
  REXLOG_INFO("Metal translated shader {:016X} modification {:016X} to {} MSL bytes",
              shader.ucode_data_hash(), modification, translation->msl_source().size());

  if (!REXCVAR_GET(dump_shaders).empty()) {
    translation->Dump(REXCVAR_GET(dump_shaders), "metal_spirv");
  }
  return translation->is_valid();
}

bool MetalCommandProcessor::EnsureShaderTranslated(MetalShader& shader) {
  return EnsureShaderTranslated(shader, GetDefaultShaderModification(shader));
}

uint64_t MetalCommandProcessor::GetDefaultShaderModification(MetalShader& shader) const {
  auto sq_program_cntl = register_file_->Get<reg::SQ_PROGRAM_CNTL>();
  uint32_t dynamic_addressable_register_count = shader.GetDynamicAddressableRegisterCount(
      shader.type() == xenos::ShaderType::kVertex ? sq_program_cntl.vs_num_reg
                                                  : sq_program_cntl.ps_num_reg);
  return shader.type() == xenos::ShaderType::kVertex
             ? shader_translator_->GetDefaultVertexShaderModification(
                   dynamic_addressable_register_count, current_host_vertex_shader_type_)
             : shader_translator_->GetDefaultPixelShaderModification(
                   dynamic_addressable_register_count);
}

uint64_t MetalCommandProcessor::GetCurrentVertexShaderModification(MetalShader& shader,
                                                                   uint32_t interpolator_mask,
                                                                   bool ps_param_gen_used) const {
  SpirvShaderTranslator::Modification modification(GetDefaultShaderModification(shader));
  modification.vertex.interpolator_mask = interpolator_mask;
  if (current_host_vertex_shader_type_ == Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    modification.vertex.output_point_parameters = uint32_t(ps_param_gen_used);
  } else {
    modification.vertex.output_point_parameters =
        uint32_t((shader.writes_point_size_edge_flag_kill_vertex() & 0b001) &&
                 register_file_->Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                     xenos::PrimitiveType::kPointList);
  }
  modification.vertex.point_ps_ucp_mode = register_file_->Get<reg::PA_CL_CLIP_CNTL>().ps_ucp_mode;
  return modification.value;
}

uint64_t MetalCommandProcessor::GetCurrentPixelShaderModification(MetalShader& shader,
                                                                  uint32_t interpolator_mask,
                                                                  uint32_t param_gen_pos) const {
  SpirvShaderTranslator::Modification modification(GetDefaultShaderModification(shader));
  modification.pixel.interpolator_mask = interpolator_mask;
  modification.pixel.interpolators_centroid =
      interpolator_mask & ~xenos::GetInterpolatorSamplingPattern(
                              register_file_->Get<reg::RB_SURFACE_INFO>().msaa_samples,
                              register_file_->Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl,
                              register_file_->Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
  if (param_gen_pos < xenos::kMaxInterpolators) {
    modification.pixel.param_gen_enable = 1;
    modification.pixel.param_gen_interpolator = param_gen_pos;
    modification.pixel.param_gen_point =
        uint32_t(register_file_->Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                 xenos::PrimitiveType::kPointList);
  } else {
    modification.pixel.param_gen_enable = 0;
    modification.pixel.param_gen_interpolator = 0;
    modification.pixel.param_gen_point = 0;
  }
  return modification.value;
}

void MetalCommandProcessor::GetCurrentShaderModifications(MetalShader* vertex_shader,
                                                          MetalShader* pixel_shader,
                                                          uint64_t& vertex_modification,
                                                          uint64_t& pixel_modification,
                                                          uint32_t* interpolator_mask_out,
                                                          uint32_t* ps_param_gen_pos_out) const {
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask = 0;
  if (pixel_shader) {
    uint32_t pixel_input_mask = pixel_shader->GetInterpolatorInputMask(
        register_file_->Get<reg::SQ_PROGRAM_CNTL>(), register_file_->Get<reg::SQ_CONTEXT_MISC>(),
        ps_param_gen_pos);
    interpolator_mask = vertex_shader ? (vertex_shader->writes_interpolators() & pixel_input_mask)
                                      : pixel_input_mask;
  }
  vertex_modification = vertex_shader
                            ? GetCurrentVertexShaderModification(*vertex_shader, interpolator_mask,
                                                                 ps_param_gen_pos != UINT32_MAX)
                            : 0;
  pixel_modification = pixel_shader ? GetCurrentPixelShaderModification(
                                          *pixel_shader, interpolator_mask, ps_param_gen_pos)
                                    : 0;
  if (interpolator_mask_out) {
    *interpolator_mask_out = interpolator_mask;
  }
  if (ps_param_gen_pos_out) {
    *ps_param_gen_pos_out = ps_param_gen_pos;
  }
}

MetalShader::MetalTranslation* MetalCommandProcessor::GetTranslatedShader(MetalShader& shader,
                                                                          uint64_t modification) {
  if (!EnsureShaderTranslated(shader, modification)) {
    return nullptr;
  }
  return static_cast<MetalShader::MetalTranslation*>(shader.GetTranslation(modification));
}

MetalShader::MetalTranslation* MetalCommandProcessor::GetTranslatedShader(MetalShader& shader) {
  return GetTranslatedShader(shader, GetDefaultShaderModification(shader));
}

void* MetalCommandProcessor::EnsureRenderPipeline(MetalShader& vertex_shader,
                                                  MetalShader& pixel_shader) {
  uint64_t vertex_modification = 0;
  uint64_t pixel_modification = 0;
  GetCurrentShaderModifications(&vertex_shader, &pixel_shader, vertex_modification,
                                pixel_modification);
  uint64_t pipeline_key_parts[4] = {vertex_shader.ucode_data_hash(), pixel_shader.ucode_data_hash(),
                                    vertex_modification, pixel_modification};
  uint64_t pipeline_key = XXH3_64bits(pipeline_key_parts, sizeof(pipeline_key_parts));
  auto existing_pipeline = render_pipeline_states_.find(pipeline_key);
  if (existing_pipeline != render_pipeline_states_.end()) {
    return existing_pipeline->second;
  }

  auto* vertex_translation = GetTranslatedShader(vertex_shader, vertex_modification);
  auto* pixel_translation = GetTranslatedShader(pixel_shader, pixel_modification);
  if (!vertex_translation || !pixel_translation || !vertex_translation->metal_library() ||
      !pixel_translation->metal_library()) {
    render_pipeline_states_.emplace(pipeline_key, nullptr);
    return nullptr;
  }

  std::string pipeline_error;
  void* pipeline_state =
      CreateRenderPipelineState(metal_device_, vertex_translation->metal_library(),
                                pixel_translation->metal_library(), &pipeline_error);
  render_pipeline_states_.emplace(pipeline_key, pipeline_state);

  static std::atomic<uint32_t> pipeline_logs{0};
  uint32_t pipeline_log_index = pipeline_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (pipeline_state) {
    if (pipeline_log_index <= 16 || (pipeline_log_index & 0x3F) == 0) {
      std::fprintf(stderr, "[metal] render pipeline ready#%u vs=%016llx ps=%016llx key=%016llx\n",
                   pipeline_log_index,
                   static_cast<unsigned long long>(vertex_shader.ucode_data_hash()),
                   static_cast<unsigned long long>(pixel_shader.ucode_data_hash()),
                   static_cast<unsigned long long>(pipeline_key));
      std::fflush(stderr);
    }
  } else if (pipeline_log_index <= 16 || (pipeline_log_index & 0x3F) == 0) {
    std::fprintf(
        stderr, "[metal] render pipeline failed#%u vs=%016llx ps=%016llx: %s\n", pipeline_log_index,
        static_cast<unsigned long long>(vertex_shader.ucode_data_hash()),
        static_cast<unsigned long long>(pixel_shader.ucode_data_hash()), pipeline_error.c_str());
    std::fflush(stderr);
  }
  return pipeline_state;
}

void* MetalCommandProcessor::EnsureFullscreenPixelPipeline(MetalShader& pixel_shader) {
  if (!metal_device_) {
    return nullptr;
  }

  uint64_t vertex_modification = 0;
  uint64_t pixel_modification = 0;
  GetCurrentShaderModifications(nullptr, &pixel_shader, vertex_modification, pixel_modification);
  auto* pixel_translation = GetTranslatedShader(pixel_shader, pixel_modification);
  if (!pixel_translation || !pixel_translation->metal_library()) {
    return nullptr;
  }

  if (!fullscreen_vertex_library_) {
    std::string vertex_error;
    fullscreen_vertex_library_ =
        CreateMslLibrary(metal_device_, MakeFullscreenProbeVertexMsl(), &vertex_error);
    if (!fullscreen_vertex_library_) {
      static std::atomic<uint32_t> fullscreen_vertex_logs{0};
      uint32_t fullscreen_vertex_index =
          fullscreen_vertex_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (fullscreen_vertex_index <= 8 || (fullscreen_vertex_index & 0x3F) == 0) {
        std::fprintf(stderr, "[metal] fullscreen postprocess VS compile failed#%u: %s\n",
                     fullscreen_vertex_index, vertex_error.c_str());
        std::fflush(stderr);
      }
      return nullptr;
    }
  }

  uint64_t pipeline_key_parts[2] = {pixel_shader.ucode_data_hash(), pixel_modification};
  uint64_t pipeline_key = XXH3_64bits(pipeline_key_parts, sizeof(pipeline_key_parts));
  auto existing_pipeline = fullscreen_pixel_pipeline_states_.find(pipeline_key);
  if (existing_pipeline != fullscreen_pixel_pipeline_states_.end()) {
    return existing_pipeline->second;
  }

  std::string pipeline_error;
  void* pipeline_state =
      CreateRenderPipelineState(metal_device_, fullscreen_vertex_library_,
                                pixel_translation->metal_library(), &pipeline_error);
  fullscreen_pixel_pipeline_states_.emplace(pipeline_key, pipeline_state);
  static std::atomic<uint32_t> fullscreen_pipeline_logs{0};
  uint32_t fullscreen_pipeline_index =
      fullscreen_pipeline_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (pipeline_state) {
    if (fullscreen_pipeline_index <= 16 || (fullscreen_pipeline_index & 0x3F) == 0) {
      std::fprintf(stderr,
                   "[metal] fullscreen postprocess pipeline ready#%u ps=%016llx key=%016llx\n",
                   fullscreen_pipeline_index,
                   static_cast<unsigned long long>(pixel_shader.ucode_data_hash()),
                   static_cast<unsigned long long>(pipeline_key));
      std::fflush(stderr);
    }
  } else if (fullscreen_pipeline_index <= 16 || (fullscreen_pipeline_index & 0x3F) == 0) {
    std::fprintf(stderr, "[metal] fullscreen postprocess pipeline failed#%u ps=%016llx: %s\n",
                 fullscreen_pipeline_index,
                 static_cast<unsigned long long>(pixel_shader.ucode_data_hash()),
                 pipeline_error.c_str());
    std::fflush(stderr);
  }
  return pipeline_state;
}

void* MetalCommandProcessor::EnsureHostPixelPipeline(MetalShader& pixel_shader) {
  if (!metal_device_) {
    return nullptr;
  }

  uint64_t vertex_modification = 0;
  uint64_t pixel_modification = 0;
  GetCurrentShaderModifications(nullptr, &pixel_shader, vertex_modification, pixel_modification);
  auto* pixel_translation = GetTranslatedShader(pixel_shader, pixel_modification);
  if (!pixel_translation || !pixel_translation->metal_library()) {
    return nullptr;
  }

  if (!host_pixel_vertex_library_) {
    std::string vertex_error;
    host_pixel_vertex_library_ =
        CreateMslLibrary(metal_device_, MakeHostPixelProbeVertexMsl(), &vertex_error);
    if (!host_pixel_vertex_library_) {
      static std::atomic<uint32_t> host_pixel_vertex_logs{0};
      uint32_t host_pixel_vertex_index =
          host_pixel_vertex_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (host_pixel_vertex_index <= 8 || (host_pixel_vertex_index & 0x3F) == 0) {
        std::fprintf(stderr, "[metal] host pixel VS compile failed#%u: %s\n",
                     host_pixel_vertex_index, vertex_error.c_str());
        std::fflush(stderr);
      }
      return nullptr;
    }
  }

  uint64_t pipeline_key_parts[2] = {pixel_shader.ucode_data_hash(), pixel_modification};
  uint64_t pipeline_key = XXH3_64bits(pipeline_key_parts, sizeof(pipeline_key_parts));
  auto existing_pipeline = host_pixel_pipeline_states_.find(pipeline_key);
  if (existing_pipeline != host_pixel_pipeline_states_.end()) {
    return existing_pipeline->second;
  }

  std::string pipeline_error;
  void* pipeline_state =
      CreateRenderPipelineState(metal_device_, host_pixel_vertex_library_,
                                pixel_translation->metal_library(), &pipeline_error);
  host_pixel_pipeline_states_.emplace(pipeline_key, pipeline_state);
  static std::atomic<uint32_t> host_pixel_pipeline_logs{0};
  uint32_t host_pixel_pipeline_index =
      host_pixel_pipeline_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (pipeline_state) {
    if (host_pixel_pipeline_index <= 16 || (host_pixel_pipeline_index & 0x3F) == 0) {
      std::fprintf(stderr, "[metal] host pixel pipeline ready#%u ps=%016llx key=%016llx\n",
                   host_pixel_pipeline_index,
                   static_cast<unsigned long long>(pixel_shader.ucode_data_hash()),
                   static_cast<unsigned long long>(pipeline_key));
      std::fflush(stderr);
    }
  } else if (host_pixel_pipeline_index <= 16 || (host_pixel_pipeline_index & 0x3F) == 0) {
    std::fprintf(
        stderr, "[metal] host pixel pipeline failed#%u ps=%016llx: %s\n", host_pixel_pipeline_index,
        static_cast<unsigned long long>(pixel_shader.ucode_data_hash()), pipeline_error.c_str());
    std::fflush(stderr);
  }
  return pipeline_state;
}

void* MetalCommandProcessor::EnsureHostFallbackPixelPipeline() {
  if (!metal_device_) {
    return nullptr;
  }

  if (!host_pixel_vertex_library_) {
    std::string vertex_error;
    host_pixel_vertex_library_ =
        CreateMslLibrary(metal_device_, MakeHostPixelProbeVertexMsl(), &vertex_error);
    if (!host_pixel_vertex_library_) {
      static std::atomic<uint32_t> host_fallback_vertex_logs{0};
      uint32_t host_fallback_vertex_index =
          host_fallback_vertex_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (host_fallback_vertex_index <= 8 || (host_fallback_vertex_index & 0x3F) == 0) {
        std::fprintf(stderr, "[metal] host fallback VS compile failed#%u: %s\n",
                     host_fallback_vertex_index, vertex_error.c_str());
        std::fflush(stderr);
      }
      return nullptr;
    }
  }

  if (!host_fallback_pixel_fragment_library_) {
    std::string fragment_error;
    host_fallback_pixel_fragment_library_ =
        CreateMslLibrary(metal_device_, MakeHostFallbackPixelFragmentMsl(), &fragment_error);
    if (!host_fallback_pixel_fragment_library_) {
      static std::atomic<uint32_t> host_fallback_fragment_logs{0};
      uint32_t host_fallback_fragment_index =
          host_fallback_fragment_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (host_fallback_fragment_index <= 8 || (host_fallback_fragment_index & 0x3F) == 0) {
        std::fprintf(stderr, "[metal] host fallback fragment compile failed#%u: %s\n",
                     host_fallback_fragment_index, fragment_error.c_str());
        std::fflush(stderr);
      }
      return nullptr;
    }
  }

  if (host_fallback_pixel_pipeline_state_) {
    return host_fallback_pixel_pipeline_state_;
  }

  std::string pipeline_error;
  host_fallback_pixel_pipeline_state_ =
      CreateRenderPipelineState(metal_device_, host_pixel_vertex_library_,
                                host_fallback_pixel_fragment_library_, &pipeline_error);
  static std::atomic<uint32_t> host_fallback_pipeline_logs{0};
  uint32_t host_fallback_pipeline_index =
      host_fallback_pipeline_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (host_fallback_pixel_pipeline_state_) {
    if (host_fallback_pipeline_index <= 16 || (host_fallback_pipeline_index & 0x3F) == 0) {
      std::fprintf(stderr, "[metal] host fallback pixel pipeline ready#%u\n",
                   host_fallback_pipeline_index);
      std::fflush(stderr);
    }
  } else if (host_fallback_pipeline_index <= 16 || (host_fallback_pipeline_index & 0x3F) == 0) {
    std::fprintf(stderr, "[metal] host fallback pixel pipeline failed#%u: %s\n",
                 host_fallback_pipeline_index, pipeline_error.c_str());
    std::fflush(stderr);
  }
  return host_fallback_pixel_pipeline_state_;
}

void* MetalCommandProcessor::EnsureHostVertexColorPixelPipeline() {
  if (!metal_device_) {
    return nullptr;
  }

  if (!host_pixel_vertex_library_) {
    std::string vertex_error;
    host_pixel_vertex_library_ =
        CreateMslLibrary(metal_device_, MakeHostPixelProbeVertexMsl(), &vertex_error);
    if (!host_pixel_vertex_library_) {
      static std::atomic<uint32_t> host_vertex_color_vertex_logs{0};
      uint32_t host_vertex_color_vertex_index =
          host_vertex_color_vertex_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (host_vertex_color_vertex_index <= 8 || (host_vertex_color_vertex_index & 0x3F) == 0) {
        std::fprintf(stderr, "[metal] host vertex-color VS compile failed#%u: %s\n",
                     host_vertex_color_vertex_index, vertex_error.c_str());
        std::fflush(stderr);
      }
      return nullptr;
    }
  }

  if (!host_vertex_color_pixel_fragment_library_) {
    std::string fragment_error;
    host_vertex_color_pixel_fragment_library_ =
        CreateMslLibrary(metal_device_, MakeHostVertexColorPixelFragmentMsl(), &fragment_error);
    if (!host_vertex_color_pixel_fragment_library_) {
      static std::atomic<uint32_t> host_vertex_color_fragment_logs{0};
      uint32_t host_vertex_color_fragment_index =
          host_vertex_color_fragment_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (host_vertex_color_fragment_index <= 8 || (host_vertex_color_fragment_index & 0x3F) == 0) {
        std::fprintf(stderr, "[metal] host vertex-color fragment compile failed#%u: %s\n",
                     host_vertex_color_fragment_index, fragment_error.c_str());
        std::fflush(stderr);
      }
      return nullptr;
    }
  }

  if (host_vertex_color_pixel_pipeline_state_) {
    return host_vertex_color_pixel_pipeline_state_;
  }

  std::string pipeline_error;
  host_vertex_color_pixel_pipeline_state_ =
      CreateRenderPipelineState(metal_device_, host_pixel_vertex_library_,
                                host_vertex_color_pixel_fragment_library_, &pipeline_error);
  static std::atomic<uint32_t> host_vertex_color_pipeline_logs{0};
  uint32_t host_vertex_color_pipeline_index =
      host_vertex_color_pipeline_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (host_vertex_color_pixel_pipeline_state_) {
    if (host_vertex_color_pipeline_index <= 16 || (host_vertex_color_pipeline_index & 0x3F) == 0) {
      std::fprintf(stderr, "[metal] host vertex-color pixel pipeline ready#%u\n",
                   host_vertex_color_pipeline_index);
      std::fflush(stderr);
    }
  } else if (host_vertex_color_pipeline_index <= 16 ||
             (host_vertex_color_pipeline_index & 0x3F) == 0) {
    std::fprintf(stderr, "[metal] host vertex-color pixel pipeline failed#%u: %s\n",
                 host_vertex_color_pipeline_index, pipeline_error.c_str());
    std::fflush(stderr);
  }
  return host_vertex_color_pixel_pipeline_state_;
}

bool MetalCommandProcessor::IsHostPixelProbeAllowed(MetalShader& pixel_shader) const {
  uint64_t pixel_shader_hash = pixel_shader.ucode_data_hash();
  return disabled_host_pixel_shader_hashes_.find(pixel_shader_hash) ==
         disabled_host_pixel_shader_hashes_.end();
}

bool MetalCommandProcessor::RenderFullscreenPixelShader(MetalShader& pixel_shader, uint32_t width,
                                                        uint32_t height,
                                                        std::vector<uint8_t>& bgra_out,
                                                        bool host_render_target_context) {
  if (!width || !height || !memory_ || !register_file_) {
    return false;
  }
  void* pipeline_state = EnsureFullscreenPixelPipeline(pixel_shader);
  if (!pipeline_state) {
    return false;
  }

  uint64_t vertex_modification = 0;
  uint64_t pixel_modification = 0;
  GetCurrentShaderModifications(nullptr, &pixel_shader, vertex_modification, pixel_modification);
  auto* pixel_translation =
      static_cast<MetalShader::MetalTranslation*>(pixel_shader.GetTranslation(pixel_modification));

  std::vector<std::vector<uint8_t>> texture_storage;
  std::vector<ProbeTextureSlot> texture_slots;
  std::vector<uint32_t> texture_fetches_by_binding_index =
      pixel_translation ? GetMslTextureFetchConstantsByBindingIndex(pixel_translation->msl_source())
                        : std::vector<uint32_t>();
  if (texture_fetches_by_binding_index.empty()) {
    const auto& bindings = pixel_shader.GetTextureBindingsAfterTranslation();
    texture_fetches_by_binding_index.resize(bindings.size(), UINT32_MAX);
    for (size_t i = 0; i < bindings.size(); ++i) {
      texture_fetches_by_binding_index[i] = bindings[i].fetch_constant;
    }
  }

  SpirvShaderTranslator::SystemConstants fullscreen_system_constants = system_constants_;
  if (texture_cache_) {
    uint32_t used_texture_mask = pixel_shader.GetUsedTextureMaskAfterTranslation();
    if (used_texture_mask) {
      texture_cache_->RequestTextures(used_texture_mask);
      uint32_t textures_resolution_scaled = 0;
      uint32_t textures_remaining = used_texture_mask;
      uint32_t texture_index = 0;
      while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
        textures_remaining &= ~(UINT32_C(1) << texture_index);
        uint32_t& texture_signs_uint =
            fullscreen_system_constants.texture_swizzled_signs[texture_index >> 2];
        uint32_t texture_signs_shift = 8 * (texture_index & 3);
        uint32_t texture_signs_shifted =
            uint32_t(texture_cache_->GetActiveTextureSwizzledSigns(texture_index))
            << texture_signs_shift;
        uint32_t texture_signs_mask = UINT32_C(0xFF) << texture_signs_shift;
        texture_signs_uint = (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;

        uint32_t& texture_swizzles_uint =
            fullscreen_system_constants.texture_swizzles[texture_index >> 1];
        uint32_t texture_swizzle_shift = 12 * (texture_index & 1);
        uint32_t texture_swizzle_shifted =
            texture_cache_->GetActiveTextureHostSwizzle(texture_index) << texture_swizzle_shift;
        uint32_t texture_swizzle_mask = ((UINT32_C(1) << 12) - 1) << texture_swizzle_shift;
        texture_swizzles_uint =
            (texture_swizzles_uint & ~texture_swizzle_mask) | texture_swizzle_shifted;

        textures_resolution_scaled |=
            uint32_t(texture_cache_->IsActiveTextureResolutionScaled(texture_index))
            << texture_index;
      }
      fullscreen_system_constants.textures_resolution_scaled = textures_resolution_scaled;
    }
  }

  texture_storage.resize(texture_fetches_by_binding_index.size());
  texture_slots.resize(texture_fetches_by_binding_index.size());
  for (size_t i = 0; i < texture_fetches_by_binding_index.size(); ++i) {
    uint32_t fetch_constant = texture_fetches_by_binding_index[i];
    if (fetch_constant == UINT32_MAX) {
      continue;
    }
    xenos::xe_gpu_texture_fetch_t fetch = register_file_->GetTextureFetch(fetch_constant);
    uint32_t width_minus_1 = 0;
    uint32_t height_minus_1 = 0;
    uint32_t depth_or_array_size_minus_1 = 0;
    uint32_t base_page = 0;
    uint32_t mip_page = 0;
    uint32_t mip_max_level = 0;
    texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                   &depth_or_array_size_minus_1, &base_page,
                                                   &mip_page, nullptr, &mip_max_level);
    (void)width_minus_1;
    (void)height_minus_1;
    (void)depth_or_array_size_minus_1;
    (void)mip_page;
    (void)mip_max_level;
    uint32_t base_physical = base_page ? (base_page << 12) : 0;
    // Fullscreen postprocess commonly reads recently rendered guest textures. The persistent
    // texture cache can be stale for those until render-target writes are imported into it.
    if (base_physical) {
      static std::atomic<uint32_t> fullscreen_resolved_texture_bypass_logs{0};
      uint32_t bypass_index =
          fullscreen_resolved_texture_bypass_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (bypass_index <= 16 || (bypass_index & 0x3F) == 0) {
        std::fprintf(stderr,
                     "[metal] fullscreen using decoded texture#%u ps=%016llx "
                     "binding=%zu fetch=%u base=0x%08x last_copy=0x%08x last_swap=0x%08x\n",
                     bypass_index, static_cast<unsigned long long>(pixel_shader.ucode_data_hash()),
                     i, fetch_constant, base_physical, last_copy_dest_base_,
                     last_swap_frontbuffer_ptr_);
        std::fflush(stderr);
      }
    }
    uint32_t texture_width = 0;
    uint32_t texture_height = 0;
    bool decoded_texture =
        DecodeTextureFetchToRgba(fetch, 0, 0, texture_storage[i], texture_width, texture_height);
    if (pixel_shader.ucode_data_hash() == UINT64_C(0x21243b8826e3f416)) {
      static std::atomic<uint32_t> fullscreen_texture_slot_logs{0};
      uint32_t slot_log_index =
          fullscreen_texture_slot_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (slot_log_index <= 24 || (slot_log_index & 0x3F) == 0) {
        std::fprintf(stderr,
                     "[metal] fullscreen texture slot#%u binding=%zu fetch=%u "
                     "base=0x%08x fetch_size=%ux%u decoded=%u decoded_size=%ux%u "
                     "visible=%u bytes=%zu\n",
                     slot_log_index, i, fetch_constant, base_physical, width_minus_1 + 1,
                     height_minus_1 + 1, decoded_texture ? 1u : 0u, texture_width, texture_height,
                     decoded_texture ? CountVisibleRgbPixels(texture_storage[i]) : 0u,
                     texture_storage[i].size());
        std::fflush(stderr);
      }
    }
    if (!decoded_texture) {
      continue;
    }
    texture_slots[i].rgba = texture_storage[i].data();
    texture_slots[i].width = texture_width;
    texture_slots[i].height = texture_height;
    texture_slots[i].array_length = 1;
    texture_slots[i].bytes_per_row = size_t(texture_width) * 4;
    texture_slots[i].bytes_per_image = texture_storage[i].size();
  }

  uint32_t alpha_shift = SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  fullscreen_system_constants.flags &= ~(UINT32_C(7) << alpha_shift);
  fullscreen_system_constants.flags |= uint32_t(xenos::CompareFunction::kAlways) << alpha_shift;
  fullscreen_system_constants.alpha_to_mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    fullscreen_system_constants.color_exp_bias[i] = 1.0f;
  }

  uint32_t fragment_float_constants_buffer_index =
      pixel_translation
          ? FindMslBufferIndex(pixel_translation->msl_source(), "xe_uniform_float_constants")
          : UINT32_MAX;
  uint32_t fragment_fetch_constants_buffer_index =
      pixel_translation
          ? FindMslBufferIndex(pixel_translation->msl_source(), "xe_uniform_fetch_constants")
          : UINT32_MAX;
  uint32_t fragment_bool_loop_constants_buffer_index =
      pixel_translation
          ? FindMslBufferIndex(pixel_translation->msl_source(), "xe_uniform_bool_loop_constants")
          : UINT32_MAX;
  std::vector<uint32_t> fragment_float_constants =
      PackFloatConstantsForShader(*register_file_, pixel_shader);
  const void* fragment_float_constants_data =
      fragment_float_constants.empty() ? nullptr : fragment_float_constants.data();
  size_t fragment_float_constants_size = fragment_float_constants.size() * sizeof(uint32_t);
  std::vector<ProbeSamplerSlot> fragment_sampler_slots =
      MakeProbeSamplerSlots(*register_file_, pixel_shader);

  std::string render_error;
  bool rendered = false;
  if (host_render_target_context) {
    rendered = RenderPipelineProbeToContext(
        host_render_target_context_, pipeline_state, &fullscreen_system_constants,
        sizeof(fullscreen_system_constants), nullptr, 0, fetch_constants_.data(),
        fetch_constants_.size() * sizeof(uint32_t), memory_->physical_membase(), size_t(0x20000000),
        nullptr, nullptr, 0, 0, texture_slots.empty() ? nullptr : texture_slots.data(),
        texture_slots.size(),
        pixel_shader.GetSamplerBindingsAfterTranslation().size(),
        uint32_t(xenos::PrimitiveType::kTriangleList), 3, width, height, &render_error, UINT32_MAX,
        UINT32_MAX, UINT32_MAX, fragment_float_constants_data, fragment_float_constants_size,
        fragment_float_constants_buffer_index, fragment_fetch_constants_buffer_index, nullptr,
        fragment_sampler_slots.empty() ? nullptr : fragment_sampler_slots.data(), nullptr, 0,
        UINT32_MAX, bool_loop_constants_.data(), bool_loop_constants_.size() * sizeof(uint32_t),
        UINT32_MAX, fragment_bool_loop_constants_buffer_index);
    if (rendered && RefreshHostRenderTargetBacking(width, height)) {
      bgra_out = latest_host_render_target_bgra_;
    }
  } else {
    rendered = RenderPipelineProbe(
        metal_device_, pipeline_state, &fullscreen_system_constants,
        sizeof(fullscreen_system_constants), nullptr, 0, fetch_constants_.data(),
        fetch_constants_.size() * sizeof(uint32_t), memory_->physical_membase(), size_t(0x20000000),
        nullptr, nullptr, 0, 0, texture_slots.empty() ? nullptr : texture_slots.data(),
        texture_slots.size(),
        pixel_shader.GetSamplerBindingsAfterTranslation().size(),
        uint32_t(xenos::PrimitiveType::kTriangleList), 3, width, height, bgra_out, &render_error,
        UINT32_MAX, UINT32_MAX, UINT32_MAX, nullptr, 0, fragment_float_constants_data,
        fragment_float_constants_size, fragment_float_constants_buffer_index,
        fragment_fetch_constants_buffer_index, nullptr,
        fragment_sampler_slots.empty() ? nullptr : fragment_sampler_slots.data(), nullptr, 0,
        UINT32_MAX, bool_loop_constants_.data(), bool_loop_constants_.size() * sizeof(uint32_t),
        UINT32_MAX, fragment_bool_loop_constants_buffer_index);
  }
  if (!rendered) {
    static std::atomic<uint32_t> fullscreen_render_logs{0};
    uint32_t fullscreen_render_index =
        fullscreen_render_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (fullscreen_render_index <= 16 || (fullscreen_render_index & 0xFF) == 0) {
      std::fprintf(stderr, "[metal] fullscreen postprocess render failed#%u ps=%016llx: %s\n",
                   fullscreen_render_index,
                   static_cast<unsigned long long>(pixel_shader.ucode_data_hash()),
                   render_error.c_str());
      std::fflush(stderr);
    }
  } else if (pixel_shader.ucode_data_hash() == UINT64_C(0x21243b8826e3f416) &&
             !BgraHasNonZeroRgb(bgra_out)) {
    static std::atomic<uint32_t> fullscreen_black_logs{0};
    uint32_t fullscreen_black_index =
        fullscreen_black_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (fullscreen_black_index <= 16 || (fullscreen_black_index & 0x3F) == 0) {
      auto sample = [&](uint32_t x, uint32_t y) -> const uint8_t* {
        static const uint8_t zero[4] = {};
        if (!width || !height || bgra_out.empty()) {
          return zero;
        }
        x = std::min(x, width - 1);
        y = std::min(y, height - 1);
        size_t offset = (size_t(y) * width + x) * 4;
        return offset + 3 < bgra_out.size() ? bgra_out.data() + offset : zero;
      };
      auto packed_float = [&](uint32_t packed_index, uint32_t component) -> float {
        uint32_t word_index = packed_index * 4 + component;
        return word_index < fragment_float_constants.size()
                   ? rex::memory::Reinterpret<float>(fragment_float_constants[word_index])
                   : 0.0f;
      };
      const uint8_t* p00 = sample(0, 0);
      const uint8_t* p50 = sample(width / 2, height / 2);
      xenos::xe_gpu_texture_fetch_t fetch0 = register_file_->GetTextureFetch(0);
      xenos::xe_gpu_texture_fetch_t fetch1 = register_file_->GetTextureFetch(1);
      std::fprintf(stderr,
                   "[metal] fullscreen rendered black#%u ps=%016llx size=%ux%u "
                   "textures=%zu samplers=%zu pc_count=%zu "
                   "c0=(%.4g %.4g %.4g %.4g) c1=(%.4g %.4g %.4g %.4g) "
                   "p00=%02x %02x %02x %02x p50=%02x %02x %02x %02x "
                   "flags=0x%08x signs0=(%08x %08x %08x %08x) "
                   "swiz0=(%08x %08x %08x %08x) "
                   "tf0=(%08x %08x %08x %08x %08x %08x) "
                   "tf1=(%08x %08x %08x %08x %08x %08x)\n",
                   fullscreen_black_index,
                   static_cast<unsigned long long>(pixel_shader.ucode_data_hash()), width, height,
                   texture_slots.size(), pixel_shader.GetSamplerBindingsAfterTranslation().size(),
                   fragment_float_constants.size() / 4, packed_float(0, 0), packed_float(0, 1),
                   packed_float(0, 2), packed_float(0, 3), packed_float(1, 0), packed_float(1, 1),
                   packed_float(1, 2), packed_float(1, 3), p00[0], p00[1], p00[2], p00[3], p50[0],
                   p50[1], p50[2], p50[3], fullscreen_system_constants.flags,
                   fullscreen_system_constants.texture_swizzled_signs[0],
                   fullscreen_system_constants.texture_swizzled_signs[1],
                   fullscreen_system_constants.texture_swizzled_signs[2],
                   fullscreen_system_constants.texture_swizzled_signs[3],
                   fullscreen_system_constants.texture_swizzles[0],
                   fullscreen_system_constants.texture_swizzles[1],
                   fullscreen_system_constants.texture_swizzles[2],
                   fullscreen_system_constants.texture_swizzles[3], fetch0.dword_0, fetch0.dword_1,
                   fetch0.dword_2, fetch0.dword_3, fetch0.dword_4, fetch0.dword_5, fetch1.dword_0,
                   fetch1.dword_1, fetch1.dword_2, fetch1.dword_3, fetch1.dword_4, fetch1.dword_5);
      std::fflush(stderr);
    }
  }
  return rendered;
}

bool MetalCommandProcessor::RenderHostPixelShader(MetalShader& pixel_shader,
                                                  const std::vector<MetalHostVertex>& host_vertices,
                                                  size_t host_vertex_start,
                                                  size_t host_vertex_count, uint32_t width,
                                                  uint32_t height, std::vector<uint8_t>& bgra_out,
                                                  void* persistent_context_override,
                                                  bool use_host_vertex_color_fragment) {
  if (!width || !height || !memory_ || !register_file_ ||
      host_vertex_start >= host_vertices.size()) {
    return false;
  }
  host_vertex_count = std::min(host_vertex_count, host_vertices.size() - host_vertex_start);
  host_vertex_count -= host_vertex_count % 3;
  if (!host_vertex_count) {
    return false;
  }
  uint64_t vertex_modification = 0;
  uint64_t pixel_modification = 0;
  GetCurrentShaderModifications(nullptr, &pixel_shader, vertex_modification, pixel_modification);
  auto* pixel_translation =
      static_cast<MetalShader::MetalTranslation*>(pixel_shader.GetTranslation(pixel_modification));
  bool use_fallback_fragment =
      !use_host_vertex_color_fragment &&
      ShouldUseHostFallbackPixelShader(pixel_shader.ucode_data_hash(), pixel_translation);
  void* pipeline_state = use_host_vertex_color_fragment
                             ? EnsureHostVertexColorPixelPipeline()
                             : (use_fallback_fragment ? EnsureHostFallbackPixelPipeline()
                                                      : EnsureHostPixelPipeline(pixel_shader));
  if (!pipeline_state) {
    return false;
  }

  std::vector<HostPixelProbeVertex> probe_vertices;
  probe_vertices.reserve(host_vertex_count);
  bool has_texture_binding = !pixel_shader.GetTextureBindingsAfterTranslation().empty();
  std::array<uint32_t, VertexExportSink::kInterpolatorCount> interpolator_by_location =
      pixel_translation ? GetMslPixelInterpolatorByLocation(pixel_translation->msl_source())
                        : GetMslPixelInterpolatorByLocation(std::string());
  for (size_t i = 0; i < host_vertex_count; ++i) {
    const MetalHostVertex& host_vertex = host_vertices[host_vertex_start + i];
    HostPixelProbeVertex& probe_vertex = probe_vertices.emplace_back();
    probe_vertex.position[0] = host_vertex.x;
    probe_vertex.position[1] = host_vertex.y;
    probe_vertex.position[2] = host_vertex.z;
    probe_vertex.position[3] = host_vertex.w;
    float fallback_interpolator[4];
    if (has_texture_binding && host_vertex.texture_weight > 0.0f) {
      fallback_interpolator[0] = host_vertex.u;
      fallback_interpolator[1] = host_vertex.v;
      fallback_interpolator[2] = host_vertex.u;
      fallback_interpolator[3] = host_vertex.v;
    } else {
      fallback_interpolator[0] = host_vertex.r;
      fallback_interpolator[1] = host_vertex.g;
      fallback_interpolator[2] = host_vertex.b;
      fallback_interpolator[3] = host_vertex.a;
    }
    for (uint32_t location = 0; location < VertexExportSink::kInterpolatorCount; ++location) {
      uint32_t interpolator_index = interpolator_by_location[location];
      if (interpolator_index < MetalHostVertex::kInterpolatorCount &&
          (host_vertex.interpolator_mask & (UINT32_C(1) << interpolator_index))) {
        probe_vertex.interpolators[location] = host_vertex.interpolators[interpolator_index];
      } else {
        for (uint32_t component = 0; component < 4; ++component) {
          probe_vertex.interpolators[location][component] = fallback_interpolator[component];
        }
      }
    }
    if (use_fallback_fragment) {
      for (uint32_t component = 0; component < 4; ++component) {
        probe_vertex.interpolators[15][component] = fallback_interpolator[component];
      }
    }
  }

  std::vector<std::vector<uint8_t>> texture_storage;
  std::vector<ProbeTextureSlot> texture_slots;
  std::vector<uint32_t> texture_fetches_by_binding_index =
      !use_host_vertex_color_fragment && !use_fallback_fragment && pixel_translation
          ? GetMslTextureFetchConstantsByBindingIndex(pixel_translation->msl_source())
          : std::vector<uint32_t>();
  if (texture_fetches_by_binding_index.empty()) {
    const auto& bindings = pixel_shader.GetTextureBindingsAfterTranslation();
    texture_fetches_by_binding_index.resize(bindings.size(), UINT32_MAX);
    for (size_t i = 0; i < bindings.size(); ++i) {
      texture_fetches_by_binding_index[i] = bindings[i].fetch_constant;
    }
  }

  SpirvShaderTranslator::SystemConstants host_system_constants = system_constants_;
  if (!use_host_vertex_color_fragment && !use_fallback_fragment && texture_cache_) {
    uint32_t used_texture_mask = pixel_shader.GetUsedTextureMaskAfterTranslation();
    if (used_texture_mask) {
      texture_cache_->RequestTextures(used_texture_mask);
      uint32_t textures_resolution_scaled = 0;
      uint32_t textures_remaining = used_texture_mask;
      uint32_t texture_index = 0;
      while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
        textures_remaining &= ~(UINT32_C(1) << texture_index);
        uint32_t& texture_signs_uint =
            host_system_constants.texture_swizzled_signs[texture_index >> 2];
        uint32_t texture_signs_shift = 8 * (texture_index & 3);
        uint32_t texture_signs_shifted =
            uint32_t(texture_cache_->GetActiveTextureSwizzledSigns(texture_index))
            << texture_signs_shift;
        uint32_t texture_signs_mask = UINT32_C(0xFF) << texture_signs_shift;
        texture_signs_uint = (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;

        uint32_t& texture_swizzles_uint =
            host_system_constants.texture_swizzles[texture_index >> 1];
        uint32_t texture_swizzle_shift = 12 * (texture_index & 1);
        uint32_t texture_swizzle_shifted =
            texture_cache_->GetActiveTextureHostSwizzle(texture_index) << texture_swizzle_shift;
        uint32_t texture_swizzle_mask = ((UINT32_C(1) << 12) - 1) << texture_swizzle_shift;
        texture_swizzles_uint =
            (texture_swizzles_uint & ~texture_swizzle_mask) | texture_swizzle_shifted;

        textures_resolution_scaled |=
            uint32_t(texture_cache_->IsActiveTextureResolutionScaled(texture_index))
            << texture_index;
      }
      host_system_constants.textures_resolution_scaled = textures_resolution_scaled;
    }
  }

  texture_storage.resize(texture_fetches_by_binding_index.size());
  texture_slots.resize(texture_fetches_by_binding_index.size());
  std::vector<uint32_t> texture_slot_bases(texture_fetches_by_binding_index.size(), 0);
  std::vector<uint32_t> texture_slot_pitches(texture_fetches_by_binding_index.size(), 0);
  std::vector<uint32_t> texture_slot_tiled(texture_fetches_by_binding_index.size(), 0);
  for (size_t i = 0; i < texture_fetches_by_binding_index.size(); ++i) {
    uint32_t fetch_constant = texture_fetches_by_binding_index[i];
    if (fetch_constant == UINT32_MAX) {
      continue;
    }
    xenos::xe_gpu_texture_fetch_t fetch = register_file_->GetTextureFetch(fetch_constant);
    // [ge-sticky] Historical binding-order diagnostic
    // (GOLDENEYE_METAL_STICKY_TEX=1). Remember the last valid content texture
    // and substitute it for an invalid current fetch to isolate stale texture
    // state from the rest of the producer pipeline. This changes execution and
    // is never part of a strict-path result.
    {
      static const bool ge_sticky_tex = std::getenv("GOLDENEYE_METAL_STICKY_TEX") != nullptr;
      // Track the last-valid *content* texture (a real 2D texture, not the
      // framebuffer/swap/EDRAM aliases and not the 0x10000000 placeholder). The
      // menu shaders read fetch constant 1, which is never bound; the menu texture
      // (0x15c5c000) is written to fetch constant 0 transiently. When a draw's
      // fetch is invalid, substitute the last-valid content texture to prove the
      // pipeline renders it end-to-end.
      static xenos::xe_gpu_texture_fetch_t ge_last_content_fetch{};
      static bool ge_last_content_ok = false;
      const uint32_t base_addr = fetch.base_address << 12;
      const bool is_content_tex = fetch.type == xenos::FetchConstantType::kTexture &&
                                  base_addr != 0u && base_addr != 0x1ec30000u &&
                                  base_addr != 0x1efc8000u && base_addr != 0x10000000u;
      if (is_content_tex) {
        ge_last_content_fetch = fetch;
        ge_last_content_ok = true;
      } else if (ge_sticky_tex && ge_last_content_ok &&
                 fetch.type != xenos::FetchConstantType::kTexture) {
        fetch = ge_last_content_fetch;
      }
    }
    texture_slot_bases[i] = fetch.base_address << 12;
    texture_slot_pitches[i] = fetch.pitch << 5;
    texture_slot_tiled[i] = fetch.tiled ? 1u : 0u;
    uint32_t texture_width = 0;
    uint32_t texture_height = 0;
    if (!DecodeTextureFetchToRgba(fetch, 0, 0, texture_storage[i], texture_width, texture_height)) {
      continue;
    }
    texture_slots[i].rgba = texture_storage[i].data();
    texture_slots[i].width = texture_width;
    texture_slots[i].height = texture_height;
    texture_slots[i].array_length = 1;
    texture_slots[i].bytes_per_row = size_t(texture_width) * 4;
    texture_slots[i].bytes_per_image = texture_storage[i].size();
  }

  uint32_t alpha_shift = SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  host_system_constants.flags &= ~(UINT32_C(7) << alpha_shift);
  host_system_constants.flags |= uint32_t(xenos::CompareFunction::kAlways) << alpha_shift;
  host_system_constants.alpha_to_mask = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    host_system_constants.color_exp_bias[i] = 1.0f;
  }

  uint32_t fragment_float_constants_buffer_index =
      pixel_translation
          ? FindMslBufferIndex(pixel_translation->msl_source(), "xe_uniform_float_constants")
          : UINT32_MAX;
  uint32_t fragment_fetch_constants_buffer_index =
      pixel_translation
          ? FindMslBufferIndex(pixel_translation->msl_source(), "xe_uniform_fetch_constants")
          : UINT32_MAX;
  uint32_t fragment_bool_loop_constants_buffer_index =
      pixel_translation
          ? FindMslBufferIndex(pixel_translation->msl_source(), "xe_uniform_bool_loop_constants")
          : UINT32_MAX;
  std::vector<uint32_t> fragment_float_constants =
      (use_host_vertex_color_fragment || use_fallback_fragment)
          ? std::vector<uint32_t>()
          : PackFloatConstantsForShader(*register_file_, pixel_shader);
  const void* fragment_float_constants_data =
      fragment_float_constants.empty() ? nullptr : fragment_float_constants.data();
  size_t fragment_float_constants_size = fragment_float_constants.size() * sizeof(uint32_t);
  std::vector<ProbeSamplerSlot> fragment_sampler_slots =
      (use_host_vertex_color_fragment || use_fallback_fragment)
          ? std::vector<ProbeSamplerSlot>()
          : MakeProbeSamplerSlots(*register_file_, pixel_shader);
  size_t fragment_sampler_count = (use_host_vertex_color_fragment || use_fallback_fragment)
                                      ? 0
                                      : pixel_shader.GetSamplerBindingsAfterTranslation().size();
  if (use_host_vertex_color_fragment || use_fallback_fragment) {
    fragment_float_constants_buffer_index = UINT32_MAX;
    fragment_fetch_constants_buffer_index = UINT32_MAX;
    fragment_bool_loop_constants_buffer_index = UINT32_MAX;
  }

  std::string render_error;
  bool rendered = false;
  std::vector<uint8_t> before_context_bgra;
  void* persistent_host_context =
      persistent_context_override ? persistent_context_override : host_pixel_probe_context_;
  bool use_persistent_host_context = persistent_host_context && !use_fallback_fragment;
  if (use_persistent_host_context) {
    ReadPipelineProbeContext(persistent_host_context, width, height, before_context_bgra, nullptr);
    rendered =
        RenderPipelineProbeToContext(
            persistent_host_context, pipeline_state, &host_system_constants,
            sizeof(host_system_constants), nullptr, 0, fetch_constants_.data(),
            fetch_constants_.size() * sizeof(uint32_t), memory_->physical_membase(),
            size_t(0x20000000), nullptr, nullptr, 0, 0,
            texture_slots.empty() ? nullptr : texture_slots.data(), texture_slots.size(),
            fragment_sampler_count, uint32_t(xenos::PrimitiveType::kTriangleList),
            uint32_t(probe_vertices.size()), width, height, &render_error, UINT32_MAX, UINT32_MAX,
            UINT32_MAX, fragment_float_constants_data, fragment_float_constants_size,
            fragment_float_constants_buffer_index, fragment_fetch_constants_buffer_index, nullptr,
            fragment_sampler_slots.empty() ? nullptr : fragment_sampler_slots.data(),
            probe_vertices.data(), probe_vertices.size() * sizeof(HostPixelProbeVertex), 3,
            bool_loop_constants_.data(), bool_loop_constants_.size() * sizeof(uint32_t), UINT32_MAX,
            fragment_bool_loop_constants_buffer_index) &&
        ReadPipelineProbeContext(persistent_host_context, width, height, bgra_out, &render_error);
  } else {
    const uint8_t* initial_bgra = nullptr;
    size_t initial_bgra_row_pitch = 0;
    if (bgra_out.size() >= size_t(width) * height * 4) {
      initial_bgra = bgra_out.data();
      initial_bgra_row_pitch = size_t(width) * 4;
    }
    rendered = RenderPipelineProbe(
        metal_device_, pipeline_state, &host_system_constants, sizeof(host_system_constants),
        nullptr, 0, fetch_constants_.data(), fetch_constants_.size() * sizeof(uint32_t),
        memory_->physical_membase(), size_t(0x20000000), nullptr, nullptr, 0, 0,
        texture_slots.empty() ? nullptr : texture_slots.data(), texture_slots.size(),
        fragment_sampler_count, uint32_t(xenos::PrimitiveType::kTriangleList),
        uint32_t(probe_vertices.size()), width, height, bgra_out, &render_error, UINT32_MAX,
        UINT32_MAX, UINT32_MAX, initial_bgra, initial_bgra_row_pitch, fragment_float_constants_data,
        fragment_float_constants_size, fragment_float_constants_buffer_index,
        fragment_fetch_constants_buffer_index, nullptr,
        fragment_sampler_slots.empty() ? nullptr : fragment_sampler_slots.data(),
        probe_vertices.data(), probe_vertices.size() * sizeof(HostPixelProbeVertex), 3,
        bool_loop_constants_.data(), bool_loop_constants_.size() * sizeof(uint32_t), UINT32_MAX,
        fragment_bool_loop_constants_buffer_index);
  }
  if (!rendered) {
    disabled_host_pixel_shader_hashes_.insert(pixel_shader.ucode_data_hash());
    static std::atomic<uint32_t> host_pixel_render_logs{0};
    uint32_t host_pixel_render_index =
        host_pixel_render_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (host_pixel_render_index <= 16 || (host_pixel_render_index & 0xFF) == 0) {
      std::fprintf(stderr,
                   "[metal] host pixel render failed#%u ps=%016llx vertices=%zu "
                   "disabled=1: %s\n",
                   host_pixel_render_index,
                   static_cast<unsigned long long>(pixel_shader.ucode_data_hash()),
                   host_vertex_count, render_error.c_str());
      std::fflush(stderr);
    }
  }
  if (pixel_shader.ucode_data_hash() == UINT64_C(0x2e372ea28cc404b7)) {
    static std::atomic<uint32_t> producer_probe_logs{0};
    uint32_t producer_probe_index = producer_probe_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    BgraFrameStats producer_probe_stats = GetBgraFrameStats(bgra_out);
    bool force_producer_probe_log = rendered && producer_probe_stats.visible_pixels == 0 &&
                                    ((producer_probe_index & 0x3F) == 0);
    if (producer_probe_index <= 32 || force_producer_probe_log) {
      float host_color_min[4] = {INFINITY, INFINITY, INFINITY, INFINITY};
      float host_color_max[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
      float probe_i0_min[4] = {INFINITY, INFINITY, INFINITY, INFINITY};
      float probe_i0_max[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
      uint32_t host_i0_mask_count = 0;
      for (size_t i = 0; i < host_vertex_count; ++i) {
        const MetalHostVertex& host_vertex = host_vertices[host_vertex_start + i];
        const HostPixelProbeVertex& probe_vertex = probe_vertices[i];
        const float host_color[4] = {host_vertex.r, host_vertex.g, host_vertex.b, host_vertex.a};
        for (uint32_t component = 0; component < 4; ++component) {
          host_color_min[component] = std::min(host_color_min[component], host_color[component]);
          host_color_max[component] = std::max(host_color_max[component], host_color[component]);
          probe_i0_min[component] =
              std::min(probe_i0_min[component], probe_vertex.interpolators[0][component]);
          probe_i0_max[component] =
              std::max(probe_i0_max[component], probe_vertex.interpolators[0][component]);
        }
        if (host_vertex.interpolator_mask & UINT32_C(1)) {
          ++host_i0_mask_count;
        }
      }
      const MetalHostVertex* first_host =
          host_vertex_count ? &host_vertices[host_vertex_start] : nullptr;
      const MetalHostVertex* last_host =
          host_vertex_count ? &host_vertices[host_vertex_start + host_vertex_count - 1] : nullptr;
      const HostPixelProbeVertex* first_probe =
          probe_vertices.empty() ? nullptr : &probe_vertices.front();
      const HostPixelProbeVertex* last_probe =
          probe_vertices.empty() ? nullptr : &probe_vertices.back();
      BgraFrameStats before_stats = GetBgraFrameStats(before_context_bgra);
      std::fprintf(
          stderr,
          "[metal] producer probe#%u rendered=%u visible_before=%u visible_after=%u "
          "verts=%zu map_loc0_i%u i0_mask=%u/%zu fallback=%u tex=%u "
          "float_cb=%u floats=%zu host_rgba_range=(%.4g..%.4g %.4g..%.4g "
          "%.4g..%.4g %.4g..%.4g) probe_i0_range=(%.4g..%.4g %.4g..%.4g "
          "%.4g..%.4g %.4g..%.4g) first_host_pos=(%.4g %.4g %.4g %.4g) "
          "first_host_rgba=(%.4g %.4g %.4g %.4g) first_host_mask=0x%04x "
          "first_probe_i0=(%.4g %.4g %.4g %.4g) last_host_pos=(%.4g %.4g %.4g %.4g) "
          "last_host_rgba=(%.4g %.4g %.4g %.4g) last_host_mask=0x%04x "
          "last_probe_i0=(%.4g %.4g %.4g %.4g)\n",
          producer_probe_index, rendered ? 1u : 0u, before_stats.visible_pixels,
          producer_probe_stats.visible_pixels, probe_vertices.size(), interpolator_by_location[0],
          host_i0_mask_count, host_vertex_count, use_fallback_fragment ? 1u : 0u,
          has_texture_binding ? 1u : 0u, fragment_float_constants_buffer_index,
          fragment_float_constants.size(), host_color_min[0], host_color_max[0], host_color_min[1],
          host_color_max[1], host_color_min[2], host_color_max[2], host_color_min[3],
          host_color_max[3], probe_i0_min[0], probe_i0_max[0], probe_i0_min[1], probe_i0_max[1],
          probe_i0_min[2], probe_i0_max[2], probe_i0_min[3], probe_i0_max[3],
          first_host ? first_host->x : 0.0f, first_host ? first_host->y : 0.0f,
          first_host ? first_host->z : 0.0f, first_host ? first_host->w : 0.0f,
          first_host ? first_host->r : 0.0f, first_host ? first_host->g : 0.0f,
          first_host ? first_host->b : 0.0f, first_host ? first_host->a : 0.0f,
          first_host ? first_host->interpolator_mask : 0u,
          first_probe ? first_probe->interpolators[0][0] : 0.0f,
          first_probe ? first_probe->interpolators[0][1] : 0.0f,
          first_probe ? first_probe->interpolators[0][2] : 0.0f,
          first_probe ? first_probe->interpolators[0][3] : 0.0f, last_host ? last_host->x : 0.0f,
          last_host ? last_host->y : 0.0f, last_host ? last_host->z : 0.0f,
          last_host ? last_host->w : 0.0f, last_host ? last_host->r : 0.0f,
          last_host ? last_host->g : 0.0f, last_host ? last_host->b : 0.0f,
          last_host ? last_host->a : 0.0f, last_host ? last_host->interpolator_mask : 0u,
          last_probe ? last_probe->interpolators[0][0] : 0.0f,
          last_probe ? last_probe->interpolators[0][1] : 0.0f,
          last_probe ? last_probe->interpolators[0][2] : 0.0f,
          last_probe ? last_probe->interpolators[0][3] : 0.0f);
      std::fflush(stderr);
    }
  }
  if (pixel_shader.ucode_data_hash() == UINT64_C(0x21243b8826e3f416)) {
    static std::atomic<uint32_t> composite_probe_logs{0};
    uint32_t composite_probe_index =
        composite_probe_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (composite_probe_index <= 16 || (composite_probe_index & 0x3F) == 0) {
      auto sample_bgra = [&](uint32_t x, uint32_t y) -> const uint8_t* {
        static const uint8_t zero[4] = {};
        if (!width || !height || bgra_out.empty()) {
          return zero;
        }
        x = std::min(x, width - 1);
        y = std::min(y, height - 1);
        size_t offset = (size_t(y) * width + x) * 4;
        return offset + 3 < bgra_out.size() ? bgra_out.data() + offset : zero;
      };
      auto packed_float = [&](uint32_t packed_index, uint32_t component) -> float {
        uint32_t word_index = packed_index * 4 + component;
        return word_index < fragment_float_constants.size()
                   ? rex::memory::Reinterpret<float>(fragment_float_constants[word_index])
                   : 0.0f;
      };
      const HostPixelProbeVertex* first_probe =
          probe_vertices.empty() ? nullptr : &probe_vertices.front();
      const HostPixelProbeVertex* last_probe =
          probe_vertices.empty() ? nullptr : &probe_vertices.back();
      float i0_min[4] = {INFINITY, INFINITY, INFINITY, INFINITY};
      float i0_max[4] = {-INFINITY, -INFINITY, -INFINITY, -INFINITY};
      for (const HostPixelProbeVertex& probe_vertex : probe_vertices) {
        for (uint32_t component = 0; component < 4; ++component) {
          i0_min[component] = std::min(i0_min[component], probe_vertex.interpolators[0][component]);
          i0_max[component] = std::max(i0_max[component], probe_vertex.interpolators[0][component]);
        }
      }
      const uint8_t* p00 = sample_bgra(0, 0);
      const uint8_t* p50 = sample_bgra(width / 2, height / 2);
      BgraFrameStats composite_stats = GetBgraFrameStats(bgra_out);
      BgraBandStats composite_band_stats = GetBgraBandStats(bgra_out, width, height);
      std::fprintf(
          stderr,
          "[metal] composite probe#%u rendered=%u visible=%u range=%u "
          "bands(top208=%u mid=%u low=%u) verts=%zu size=%ux%u "
          "i0_first=(%.4g %.4g %.4g %.4g) "
          "i0_last=(%.4g %.4g %.4g %.4g) "
          "i0_range=(%.4g..%.4g %.4g..%.4g %.4g..%.4g %.4g..%.4g) "
          "c0=(%.4g %.4g %.4g %.4g) "
          "c1=(%.4g %.4g %.4g %.4g) tex_count=%zu "
          "tex0=fc%u base=0x%08x %ux%u pitch=%u tiled=%u "
          "tex1=fc%u base=0x%08x %ux%u pitch=%u tiled=%u "
          "tex2=fc%u base=0x%08x %ux%u pitch=%u tiled=%u "
          "tex3=fc%u base=0x%08x %ux%u pitch=%u tiled=%u "
          "swiz0=(%08x %08x) signs0=(%08x %08x) "
          "p00=%02x %02x %02x %02x p50=%02x %02x %02x %02x\n",
          composite_probe_index, rendered ? 1u : 0u, composite_stats.visible_pixels,
          BgraRgbRange(composite_stats), composite_band_stats.top_208_visible,
          composite_band_stats.mid_208_512_visible, composite_band_stats.low_512_visible,
          probe_vertices.size(), width, height,
          first_probe ? first_probe->interpolators[0][0] : 0.0f,
          first_probe ? first_probe->interpolators[0][1] : 0.0f,
          first_probe ? first_probe->interpolators[0][2] : 0.0f,
          first_probe ? first_probe->interpolators[0][3] : 0.0f,
          last_probe ? last_probe->interpolators[0][0] : 0.0f,
          last_probe ? last_probe->interpolators[0][1] : 0.0f,
          last_probe ? last_probe->interpolators[0][2] : 0.0f,
          last_probe ? last_probe->interpolators[0][3] : 0.0f, i0_min[0], i0_max[0], i0_min[1],
          i0_max[1], i0_min[2], i0_max[2], i0_min[3], i0_max[3], packed_float(0, 0),
          packed_float(0, 1), packed_float(0, 2), packed_float(0, 3), packed_float(1, 0),
          packed_float(1, 1), packed_float(1, 2), packed_float(1, 3), texture_slots.size(),
          texture_fetches_by_binding_index.size() > 0 ? texture_fetches_by_binding_index[0]
                                                      : UINT32_MAX,
          texture_slot_bases.size() > 0 ? texture_slot_bases[0] : 0,
          texture_slots.size() > 0 ? texture_slots[0].width : 0,
          texture_slots.size() > 0 ? texture_slots[0].height : 0,
          texture_slot_pitches.size() > 0 ? texture_slot_pitches[0] : 0,
          texture_slot_tiled.size() > 0 ? texture_slot_tiled[0] : 0,
          texture_fetches_by_binding_index.size() > 1 ? texture_fetches_by_binding_index[1]
                                                      : UINT32_MAX,
          texture_slot_bases.size() > 1 ? texture_slot_bases[1] : 0,
          texture_slots.size() > 1 ? texture_slots[1].width : 0,
          texture_slots.size() > 1 ? texture_slots[1].height : 0,
          texture_slot_pitches.size() > 1 ? texture_slot_pitches[1] : 0,
          texture_slot_tiled.size() > 1 ? texture_slot_tiled[1] : 0,
          texture_fetches_by_binding_index.size() > 2 ? texture_fetches_by_binding_index[2]
                                                      : UINT32_MAX,
          texture_slot_bases.size() > 2 ? texture_slot_bases[2] : 0,
          texture_slots.size() > 2 ? texture_slots[2].width : 0,
          texture_slots.size() > 2 ? texture_slots[2].height : 0,
          texture_slot_pitches.size() > 2 ? texture_slot_pitches[2] : 0,
          texture_slot_tiled.size() > 2 ? texture_slot_tiled[2] : 0,
          texture_fetches_by_binding_index.size() > 3 ? texture_fetches_by_binding_index[3]
                                                      : UINT32_MAX,
          texture_slot_bases.size() > 3 ? texture_slot_bases[3] : 0,
          texture_slots.size() > 3 ? texture_slots[3].width : 0,
          texture_slots.size() > 3 ? texture_slots[3].height : 0,
          texture_slot_pitches.size() > 3 ? texture_slot_pitches[3] : 0,
          texture_slot_tiled.size() > 3 ? texture_slot_tiled[3] : 0,
          host_system_constants.texture_swizzles[0], host_system_constants.texture_swizzles[1],
          host_system_constants.texture_swizzled_signs[0],
          host_system_constants.texture_swizzled_signs[1], p00[0], p00[1], p00[2], p00[3], p50[0],
          p50[1], p50[2], p50[3]);
      std::fflush(stderr);
    }
  }
  if (rendered && !before_context_bgra.empty() && before_context_bgra.size() == bgra_out.size()) {
    BgraFrameStats before_stats = GetBgraFrameStats(before_context_bgra);
    BgraFrameStats after_stats = GetBgraFrameStats(bgra_out);
    uint32_t new_visible_pixels = CountNewVisibleRgbPixels(before_context_bgra, bgra_out);
    if (new_visible_pixels == 0 && after_stats.visible_pixels <= before_stats.visible_pixels) {
      static std::atomic<uint32_t> stale_host_pixel_logs{0};
      uint32_t stale_host_pixel_index =
          stale_host_pixel_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (stale_host_pixel_index <= 16 || (stale_host_pixel_index & 0x3F) == 0) {
        std::fprintf(stderr,
                     "[metal] skipped stale host pixel frame#%u ps=%016llx "
                     "visible_before=%u visible_after=%u\n",
                     stale_host_pixel_index,
                     static_cast<unsigned long long>(pixel_shader.ucode_data_hash()),
                     before_stats.visible_pixels, after_stats.visible_pixels);
        std::fflush(stderr);
      }
      return false;
    }
  }
  return rendered;
}

void* MetalCommandProcessor::EnsureSolidColorPipeline(MetalShader& vertex_shader) {
  if (!metal_device_) {
    return nullptr;
  }

  auto* vertex_translation = GetTranslatedShader(vertex_shader);
  if (!vertex_translation || !vertex_translation->metal_library()) {
    return nullptr;
  }

  if (!solid_fragment_library_) {
    std::string fragment_error;
    solid_fragment_library_ =
        CreateMslLibrary(metal_device_, MakeSolidFragmentMsl(), &fragment_error);
    if (!solid_fragment_library_) {
      static std::atomic<uint32_t> solid_fragment_logs{0};
      uint32_t solid_fragment_index =
          solid_fragment_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (solid_fragment_index <= 8 || (solid_fragment_index & 0x3F) == 0) {
        std::fprintf(stderr, "[metal] solid fragment compile failed#%u: %s\n", solid_fragment_index,
                     fragment_error.c_str());
        std::fflush(stderr);
      }
      return nullptr;
    }
  }

  uint64_t vertex_modification = GetDefaultShaderModification(vertex_shader);
  uint64_t pipeline_key_parts[2] = {vertex_shader.ucode_data_hash(), vertex_modification};
  uint64_t pipeline_key = XXH3_64bits(pipeline_key_parts, sizeof(pipeline_key_parts));
  auto existing_pipeline = solid_color_pipeline_states_.find(pipeline_key);
  if (existing_pipeline != solid_color_pipeline_states_.end()) {
    return existing_pipeline->second;
  }

  std::string pipeline_error;
  void* pipeline_state = CreateRenderPipelineState(
      metal_device_, vertex_translation->metal_library(), solid_fragment_library_, &pipeline_error);
  solid_color_pipeline_states_.emplace(pipeline_key, pipeline_state);
  static std::atomic<uint32_t> solid_pipeline_logs{0};
  uint32_t solid_pipeline_index = solid_pipeline_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (pipeline_state) {
    if (solid_pipeline_index <= 16 || (solid_pipeline_index & 0x3F) == 0) {
      std::fprintf(stderr, "[metal] solid vertex probe pipeline ready#%u vs=%016llx key=%016llx\n",
                   solid_pipeline_index,
                   static_cast<unsigned long long>(vertex_shader.ucode_data_hash()),
                   static_cast<unsigned long long>(pipeline_key));
      std::fflush(stderr);
    }
  } else if (solid_pipeline_index <= 16 || (solid_pipeline_index & 0x3F) == 0) {
    std::fprintf(stderr, "[metal] solid vertex probe pipeline failed#%u vs=%016llx: %s\n",
                 solid_pipeline_index,
                 static_cast<unsigned long long>(vertex_shader.ucode_data_hash()),
                 pipeline_error.c_str());
    std::fflush(stderr);
  }
  return pipeline_state;
}

void* MetalCommandProcessor::EnsureMemExportPipeline(MetalShader& vertex_shader) {
  if (!metal_device_) {
    return nullptr;
  }

  auto* vertex_translation = GetTranslatedShader(vertex_shader);
  if (!vertex_translation || !vertex_translation->metal_library()) {
    return nullptr;
  }

  if (!dummy_fragment_library_) {
    std::string fragment_error;
    dummy_fragment_library_ =
        CreateMslLibrary(metal_device_, MakeDummyFragmentMsl(), &fragment_error);
    if (!dummy_fragment_library_) {
      static std::atomic<uint32_t> dummy_fragment_logs{0};
      uint32_t dummy_fragment_index =
          dummy_fragment_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (dummy_fragment_index <= 8 || (dummy_fragment_index & 0x3F) == 0) {
        std::fprintf(stderr, "[metal] memexport dummy fragment compile failed#%u: %s\n",
                     dummy_fragment_index, fragment_error.c_str());
        std::fflush(stderr);
      }
      return nullptr;
    }
  }

  uint64_t vertex_modification = GetDefaultShaderModification(vertex_shader);
  uint64_t pipeline_key_parts[2] = {vertex_shader.ucode_data_hash(), vertex_modification};
  uint64_t pipeline_key = XXH3_64bits(pipeline_key_parts, sizeof(pipeline_key_parts));
  auto existing_pipeline = memexport_pipeline_states_.find(pipeline_key);
  if (existing_pipeline != memexport_pipeline_states_.end()) {
    return existing_pipeline->second;
  }

  std::string pipeline_error;
  void* pipeline_state = CreateRenderPipelineState(
      metal_device_, vertex_translation->metal_library(), dummy_fragment_library_, &pipeline_error);
  memexport_pipeline_states_.emplace(pipeline_key, pipeline_state);
  static std::atomic<uint32_t> memexport_pipeline_logs{0};
  uint32_t memexport_pipeline_index =
      memexport_pipeline_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (pipeline_state) {
    if (memexport_pipeline_index <= 16 || (memexport_pipeline_index & 0x3F) == 0) {
      std::fprintf(stderr, "[metal] memexport pipeline ready#%u vs=%016llx key=%016llx\n",
                   memexport_pipeline_index,
                   static_cast<unsigned long long>(vertex_shader.ucode_data_hash()),
                   static_cast<unsigned long long>(pipeline_key));
      std::fflush(stderr);
    }
  } else if (memexport_pipeline_index <= 16 || (memexport_pipeline_index & 0x3F) == 0) {
    std::fprintf(
        stderr, "[metal] memexport pipeline failed#%u vs=%016llx: %s\n", memexport_pipeline_index,
        static_cast<unsigned long long>(vertex_shader.ucode_data_hash()), pipeline_error.c_str());
    std::fflush(stderr);
  }
  return pipeline_state;
}

bool MetalCommandProcessor::ExecuteMemExportVertexShader(MetalShader& vertex_shader,
                                                         xenos::PrimitiveType prim_type,
                                                         uint32_t index_count) {
  if (!memory_ || !register_file_ || !index_count) {
    return false;
  }
  auto* vertex_translation = static_cast<MetalShader::MetalTranslation*>(
      vertex_shader.GetTranslation(GetDefaultShaderModification(vertex_shader)));
  if (!HasMemExportSideEffects(vertex_shader, vertex_translation)) {
    return false;
  }
  void* pipeline_state = EnsureMemExportPipeline(vertex_shader);
  if (!pipeline_state) {
    return false;
  }

  VertexMslBufferBindings vertex_buffer_bindings = GetVertexMslBufferBindings(vertex_translation);
  std::vector<uint32_t> vertex_float_constants =
      PackFloatConstantsForShader(*register_file_, vertex_shader);
  const void* vertex_float_constants_data =
      vertex_float_constants.empty() ? nullptr : vertex_float_constants.data();
  size_t vertex_float_constants_size = vertex_float_constants.size() * sizeof(uint32_t);

  std::vector<uint8_t> ignored_bgra;
  std::string render_error;
  bool rendered = RenderPipelineProbe(
      metal_device_, pipeline_state, &system_constants_, sizeof(system_constants_),
      vertex_float_constants_data, vertex_float_constants_size, fetch_constants_.data(),
      fetch_constants_.size() * sizeof(uint32_t), memory_->physical_membase(), size_t(0x20000000),
      nullptr, nullptr, 0, 0, nullptr, 0, 0, uint32_t(prim_type), index_count, 1, 1, ignored_bgra,
      &render_error, vertex_buffer_bindings.shared_memory, vertex_buffer_bindings.float_constants,
      vertex_buffer_bindings.fetch_constants, nullptr, 0, nullptr, 0, UINT32_MAX, UINT32_MAX,
      nullptr, nullptr, nullptr, 0, UINT32_MAX, bool_loop_constants_.data(),
      bool_loop_constants_.size() * sizeof(uint32_t), vertex_buffer_bindings.bool_loop_constants,
      UINT32_MAX);
  static std::atomic<uint32_t> memexport_exec_logs{0};
  uint32_t memexport_exec_index = memexport_exec_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (rendered) {
    if (memexport_exec_index <= 16 || (memexport_exec_index & 0xFF) == 0) {
      std::fprintf(stderr,
                   "[metal] executed memexport shader#%u vs=%016llx prim=%u vertices=%u "
                   "shared_buffer=%u float_buffer=%u fetch_buffer=%u\n",
                   memexport_exec_index,
                   static_cast<unsigned long long>(vertex_shader.ucode_data_hash()),
                   uint32_t(prim_type), index_count, vertex_buffer_bindings.shared_memory,
                   vertex_buffer_bindings.float_constants, vertex_buffer_bindings.fetch_constants);
      std::fflush(stderr);
    }
  } else if (memexport_exec_index <= 16 || (memexport_exec_index & 0xFF) == 0) {
    std::fprintf(
        stderr, "[metal] memexport shader failed#%u vs=%016llx: %s\n", memexport_exec_index,
        static_cast<unsigned long long>(vertex_shader.ucode_data_hash()), render_error.c_str());
    std::fflush(stderr);
  }
  return rendered;
}

void MetalCommandProcessor::UpdateMinimalSystemConstants(xenos::PrimitiveType prim_type,
                                                         const IndexBufferInfo* index_buffer_info) {
  if (!register_file_) {
    return;
  }
  const RegisterFile& regs = *register_file_;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();

  uint32_t flags = 0;
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  }
  if (draw_util::IsPrimitivePolygonal(regs)) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  }
  if (index_buffer_info) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (index_buffer_info->format == xenos::IndexFormat::kInt32) {
      flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    }
  }
  flags |= uint32_t(rb_surface_info.msaa_samples)
           << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
  }
  xenos::CompareFunction alpha_test_function = rb_colorcontrol.alpha_test_enable
                                                   ? rb_colorcontrol.alpha_func
                                                   : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function) << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  system_constants_.flags = flags;

  xenos::Endian vertex_index_endian = xenos::Endian::kNone;
  if (index_buffer_info) {
    vertex_index_endian = index_buffer_info->endianness;
    if (index_buffer_info->format == xenos::IndexFormat::kInt16 &&
        vertex_index_endian != xenos::Endian::kNone &&
        vertex_index_endian != xenos::Endian::k8in16) {
      vertex_index_endian = vertex_index_endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16
                                                                         : xenos::Endian::kNone;
    }
  }
  system_constants_.vertex_index_load_address =
      index_buffer_info ? index_buffer_info->guest_base : 0;
  system_constants_.vertex_index_endian = vertex_index_endian;
  system_constants_.line_loop_closing_index = UINT32_MAX;
  system_constants_.vertex_base_index = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
  system_constants_.vertex_index_reset = regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx;
  system_constants_.vertex_index_min = regs.Get<uint32_t>(XE_GPU_REG_VGT_MIN_VTX_INDX);
  system_constants_.vertex_index_max = regs.Get<uint32_t>(XE_GPU_REG_VGT_MAX_VTX_INDX);

  reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);
  draw_util::ViewportInfo viewport_info = {};
  draw_util::GetHostViewportInfo(
      regs, 1, 1, true, fallback_output_width_, fallback_output_height_, true,
      normalized_depth_control, false, false,
      active_pixel_shader_ && static_cast<MetalShader*>(active_pixel_shader_)->writes_depth(),
      viewport_info);
  for (uint32_t i = 0; i < 3; ++i) {
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  if (!pa_cl_clip_cntl.clip_disable) {
    float* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t user_clip_plane_index;
    while (rex::bit_scan_forward(user_clip_planes_remaining, &user_clip_plane_index)) {
      user_clip_planes_remaining &= ~(UINT32_C(1) << user_clip_plane_index);
      const void* user_clip_plane_regs =
          &regs[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4];
      std::memcpy(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float));
      user_clip_plane_write_ptr += 4;
    }
  }

  if (prim_type == xenos::PrimitiveType::kPointList ||
      vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    system_constants_.point_vertex_diameter_min = float(pa_su_point_minmax.min_size) * 0.125f;
    system_constants_.point_vertex_diameter_max = float(pa_su_point_minmax.max_size) * 0.125f;
    system_constants_.point_constant_diameter[0] = float(pa_su_point_size.width) * 0.125f;
    system_constants_.point_constant_diameter[1] = float(pa_su_point_size.height) * 0.125f;
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        1.0f / float(std::max(viewport_info.xy_extent[0], UINT32_C(1)));
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        1.0f / float(std::max(viewport_info.xy_extent[1], UINT32_C(1)));
  }

  system_constants_.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  system_constants_.alpha_to_mask =
      rb_colorcontrol.alpha_to_mask_enable ? (rb_colorcontrol.value >> 24) | (UINT32_C(1) << 8) : 0;
  for (uint32_t i = 0; i < 4; ++i) {
    system_constants_.color_exp_bias[i] = 1.0f;
  }
  system_constants_.edram_blend_constant[0] = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
  system_constants_.edram_blend_constant[1] = regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
  system_constants_.edram_blend_constant[2] = regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
  system_constants_.edram_blend_constant[3] = regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
}

void MetalCommandProcessor::UpdateGuestConstantBuffers() {
  if (!register_file_) {
    return;
  }
  std::memcpy(float_constants_.data(), &register_file_->values[XE_GPU_REG_SHADER_CONSTANT_000_X],
              float_constants_.size() * sizeof(uint32_t));
  std::memcpy(fetch_constants_.data(),
              &register_file_->values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
              fetch_constants_.size() * sizeof(uint32_t));
  std::memcpy(bool_loop_constants_.data(),
              &register_file_->values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
              bool_loop_constants_.size() * sizeof(uint32_t));
  system_constants_.textures_resolution_scaled = 0;
  std::fill(std::begin(system_constants_.texture_swizzled_signs),
            std::end(system_constants_.texture_swizzled_signs), 0);
  std::fill(std::begin(system_constants_.texture_swizzles),
            std::end(system_constants_.texture_swizzles), 0);
  for (uint32_t texture_index = 0; texture_index < 32; ++texture_index) {
    xenos::xe_gpu_texture_fetch_t fetch = register_file_->GetTextureFetch(texture_index);
    if (fetch.type != xenos::FetchConstantType::kTexture) {
      continue;
    }
    uint32_t& texture_signs_uint = system_constants_.texture_swizzled_signs[texture_index >> 2];
    uint32_t texture_signs_shift = 8 * (texture_index & 3);
    uint32_t texture_signs_shifted = uint32_t(texture_util::SwizzleSigns(fetch))
                                     << texture_signs_shift;
    uint32_t texture_signs_mask = UINT32_C(0xFF) << texture_signs_shift;
    texture_signs_uint = (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
  }
}

bool MetalCommandProcessor::EnsureVertexFetchRangesResident(const MetalShader& vertex_shader) {
  if (!shared_memory_ || !shared_memory_->buffer() || !register_file_) {
    return false;
  }

  const Shader::ConstantRegisterMap& constant_map = vertex_shader.constant_register_map();
  for (uint32_t bitmap_index = 0;
       bitmap_index < rex::countof(constant_map.vertex_fetch_bitmap); ++bitmap_index) {
    uint32_t fetch_bits = constant_map.vertex_fetch_bitmap[bitmap_index];
    uint32_t bit_index = 0;
    while (rex::bit_scan_forward(fetch_bits, &bit_index)) {
      fetch_bits &= ~(UINT32_C(1) << bit_index);
      uint32_t fetch_index = bitmap_index * 32 + bit_index;
      xenos::xe_gpu_vertex_fetch_t fetch = register_file_->GetVertexFetch(fetch_index);
      switch (fetch.type) {
        case xenos::FetchConstantType::kVertex:
          break;
        case xenos::FetchConstantType::kInvalidVertex:
          if (REXCVAR_GET(gpu_allow_invalid_fetch_constants)) {
            break;
          }
          REXGPU_WARN(
              "Metal vertex fetch constant {} ({:08X} {:08X}) has invalid type",
              fetch_index, fetch.dword_0, fetch.dword_1);
          return false;
        default:
          REXGPU_WARN(
              "Metal vertex fetch constant {} ({:08X} {:08X}) is not a vertex fetch",
              fetch_index, fetch.dword_0, fetch.dword_1);
          return false;
      }

      uint32_t fetch_start = fetch.address << 2;
      uint32_t fetch_length = fetch.size << 2;
      if (!fetch_length) {
        continue;
      }
      if (fetch_start >= SharedMemory::kBufferSize ||
          fetch_length > SharedMemory::kBufferSize - fetch_start ||
          !shared_memory_->RequestRange(fetch_start, fetch_length)) {
        REXGPU_ERROR(
            "Failed to make Metal vertex fetch {} resident at 0x{:08X} (size {})",
            fetch_index, fetch_start, fetch_length);
        return false;
      }
    }
  }
  return true;
}

void MetalCommandProcessor::TryRenderPipelineProbe(
    MetalShader& vertex_shader, MetalShader& pixel_shader, void* pipeline_state,
    xenos::PrimitiveType prim_type, uint32_t index_count, bool host_render_target_debug,
    const PrimitiveProcessor::ProcessingResult* primitive_processing_result) {
  if (!pipeline_state || !metal_device_ || !memory_) {
    return;
  }
  if (!register_file_) {
    return;
  }
  void* persistent_context =
      host_render_target_debug ? host_render_target_context_ : pipeline_probe_context_;
  const char* persistent_label =
      host_render_target_debug ? "host render target debug" : "persistent pipeline probe";
  if (!persistent_context) {
    return;
  }
  constexpr uint32_t kMaxPersistentProbeDrawsPerSwap = 256;
  if (!host_render_target_debug &&
      pipeline_probe_draws_this_swap_ >= kMaxPersistentProbeDrawsPerSwap) {
    ++pipeline_probe_skipped_this_swap_;
    if (pipeline_probe_skipped_this_swap_ == 1 || (pipeline_probe_skipped_this_swap_ & 0xFF) == 0) {
      std::fprintf(stderr,
                   "[metal] persistent probe draw budget exhausted skipped=%u "
                   "budget=%u\n",
                   pipeline_probe_skipped_this_swap_, kMaxPersistentProbeDrawsPerSwap);
      std::fflush(stderr);
    }
    return;
  }
  uint64_t vertex_modification = 0;
  uint64_t pixel_modification = 0;
  GetCurrentShaderModifications(&vertex_shader, &pixel_shader, vertex_modification,
                                pixel_modification);
  uint64_t pipeline_key_parts[4] = {vertex_shader.ucode_data_hash(), pixel_shader.ucode_data_hash(),
                                    vertex_modification, pixel_modification};
  uint64_t pipeline_key = XXH3_64bits(pipeline_key_parts, sizeof(pipeline_key_parts));
  bool first_pipeline_probe = probed_pipeline_keys_.emplace(pipeline_key).second;

  static std::atomic<uint32_t> texture_binding_logs{0};
  uint32_t texture_binding_log_index = 0;
  if (first_pipeline_probe) {
    texture_binding_log_index = texture_binding_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  }
  if (first_pipeline_probe && texture_binding_log_index <= 8 && register_file_) {
    auto log_texture_bindings = [&](const char* stage, const MetalShader& shader) {
      size_t binding_index = 0;
      for (const auto& binding : shader.GetTextureBindingsAfterTranslation()) {
        xenos::xe_gpu_texture_fetch_t fetch =
            register_file_->GetTextureFetch(binding.fetch_constant);
        uint32_t width_minus_1 = 0;
        uint32_t height_minus_1 = 0;
        uint32_t depth_or_array_size_minus_1 = 0;
        uint32_t base_page = 0;
        texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                       &depth_or_array_size_minus_1, &base_page,
                                                       nullptr, nullptr, nullptr);
        std::fprintf(stderr,
                     "[metal] texture binding probe#%u %s shader=%016llx binding=%zu "
                     "fetch=%u signed=%u dim=%u live(type=%u fmt=%u base=0x%08x "
                     "size=%ux%ux%u pitch=%u tiled=%u endian=%u swiz=0x%03x)\n",
                     texture_binding_log_index, stage,
                     static_cast<unsigned long long>(shader.ucode_data_hash()), binding_index,
                     binding.fetch_constant, uint32_t(binding.is_signed),
                     uint32_t(binding.dimension), uint32_t(fetch.type), uint32_t(fetch.format),
                     base_page << 12, width_minus_1 + 1, height_minus_1 + 1,
                     depth_or_array_size_minus_1 + 1, uint32_t(fetch.pitch << 5),
                     uint32_t(fetch.tiled), uint32_t(fetch.endianness), uint32_t(fetch.swizzle));
        ++binding_index;
      }
    };
    log_texture_bindings("vs", vertex_shader);
    log_texture_bindings("ps", pixel_shader);
    std::fflush(stderr);
  }

  uint32_t probe_primitive_type = uint32_t(prim_type);
  uint32_t probe_vertex_count = index_count;
  if (!probe_vertex_count) {
    return;
  }
  if (!primitive_processing_result &&
      current_host_vertex_shader_type_ ==
          Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) {
    probe_primitive_type = uint32_t(xenos::PrimitiveType::kTriangleStrip);
    probe_vertex_count = std::max((index_count / 3) * 4, UINT32_C(4));
  } else if (!primitive_processing_result &&
             current_host_vertex_shader_type_ ==
                 Shader::HostVertexShaderType::kPointListAsTriangleStrip) {
    probe_primitive_type = uint32_t(xenos::PrimitiveType::kTriangleStrip);
    probe_vertex_count = std::max(index_count * 4, UINT32_C(4));
  }

  std::vector<std::vector<uint8_t>> vertex_texture_storage;
  std::vector<std::vector<uint8_t>> fragment_texture_storage;
  std::vector<ProbeTextureSlot> vertex_texture_slots;
  std::vector<ProbeTextureSlot> fragment_texture_slots;
  if (texture_cache_) {
    uint32_t used_texture_mask = vertex_shader.GetUsedTextureMaskAfterTranslation() |
                                 pixel_shader.GetUsedTextureMaskAfterTranslation();
    if (used_texture_mask) {
      texture_cache_->RequestTextures(used_texture_mask);
      uint32_t textures_resolution_scaled = 0;
      uint32_t textures_remaining = used_texture_mask;
      uint32_t texture_index = 0;
      while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
        textures_remaining &= ~(UINT32_C(1) << texture_index);
        uint32_t& texture_signs_uint = system_constants_.texture_swizzled_signs[texture_index >> 2];
        uint32_t texture_signs_shift = 8 * (texture_index & 3);
        uint32_t texture_signs_shifted =
            uint32_t(texture_cache_->GetActiveTextureSwizzledSigns(texture_index))
            << texture_signs_shift;
        uint32_t texture_signs_mask = UINT32_C(0xFF) << texture_signs_shift;
        texture_signs_uint = (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;

        uint32_t& texture_swizzles_uint = system_constants_.texture_swizzles[texture_index >> 1];
        uint32_t texture_swizzle_shift = 12 * (texture_index & 1);
        uint32_t texture_swizzle_shifted =
            texture_cache_->GetActiveTextureHostSwizzle(texture_index) << texture_swizzle_shift;
        uint32_t texture_swizzle_mask = ((UINT32_C(1) << 12) - 1) << texture_swizzle_shift;
        texture_swizzles_uint =
            (texture_swizzles_uint & ~texture_swizzle_mask) | texture_swizzle_shifted;

        textures_resolution_scaled |=
            uint32_t(texture_cache_->IsActiveTextureResolutionScaled(texture_index))
            << texture_index;
      }
      system_constants_.textures_resolution_scaled = textures_resolution_scaled;
    }
  }
  auto prepare_probe_textures = [&](const char* stage, MetalShader& shader,
                                    std::vector<std::vector<uint8_t>>& storage,
                                    std::vector<ProbeTextureSlot>& slots) {
    uint64_t shader_modification =
        shader.type() == xenos::ShaderType::kVertex ? vertex_modification : pixel_modification;
    auto* translation =
        static_cast<MetalShader::MetalTranslation*>(shader.GetTranslation(shader_modification));
    std::vector<uint32_t> texture_fetches_by_binding_index =
        translation ? GetMslTextureFetchConstantsByBindingIndex(translation->msl_source())
                    : std::vector<uint32_t>();
    if (texture_fetches_by_binding_index.empty()) {
      const auto& bindings = shader.GetTextureBindingsAfterTranslation();
      texture_fetches_by_binding_index.resize(bindings.size(), UINT32_MAX);
      for (size_t i = 0; i < bindings.size(); ++i) {
        texture_fetches_by_binding_index[i] = bindings[i].fetch_constant;
      }
    }
    storage.clear();
    slots.clear();
    storage.resize(texture_fetches_by_binding_index.size());
    slots.resize(texture_fetches_by_binding_index.size());
    for (size_t i = 0; i < texture_fetches_by_binding_index.size(); ++i) {
      uint32_t fetch_constant = texture_fetches_by_binding_index[i];
      if (fetch_constant == UINT32_MAX) {
        continue;
      }
      const bool binding_is_signed =
          i < shader.GetTextureBindingsAfterTranslation().size()
              ? bool(shader.GetTextureBindingsAfterTranslation()[i].is_signed)
              : false;
      if (texture_cache_) {
        if (void* cached_texture =
                texture_cache_->GetActiveTexture(fetch_constant, binding_is_signed)) {
          slots[i].metal_texture = cached_texture;
          slots[i].width = texture_cache_->GetActiveTextureWidth(fetch_constant);
          slots[i].height = texture_cache_->GetActiveTextureHeight(fetch_constant);
          slots[i].array_length = 1;
          static std::atomic<uint32_t> cached_texture_logs{0};
          uint32_t cached_texture_index =
              cached_texture_logs.fetch_add(1, std::memory_order_relaxed) + 1;
          if (cached_texture_index <= 12 || (cached_texture_index & 0x3F) == 0) {
            std::fprintf(stderr,
                         "[metal] probe cached texture#%u %s shader=%016llx binding=%zu fetch=%u "
                         "size=%ux%u\n",
                         cached_texture_index, stage,
                         static_cast<unsigned long long>(shader.ucode_data_hash()), i,
                         fetch_constant, slots[i].width, slots[i].height);
            std::fflush(stderr);
          }
          continue;
        }
      }
      xenos::xe_gpu_texture_fetch_t fetch = register_file_->GetTextureFetch(fetch_constant);
      uint32_t texture_width = 0;
      uint32_t texture_height = 0;
      if (DecodeTextureFetchToRgba(fetch, 0, 0, storage[i], texture_width, texture_height)) {
        slots[i].rgba = storage[i].data();
        slots[i].width = texture_width;
        slots[i].height = texture_height;
        slots[i].array_length = 1;
        slots[i].bytes_per_row = size_t(texture_width) * 4;
        slots[i].bytes_per_image = storage[i].size();
        static std::atomic<uint32_t> uploaded_texture_logs{0};
        uint32_t uploaded_texture_index =
            uploaded_texture_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (uploaded_texture_index <= 12 || (uploaded_texture_index & 0x3F) == 0) {
          std::fprintf(stderr,
                       "[metal] probe texture slot#%u %s shader=%016llx binding=%zu fetch=%u "
                       "size=%ux%u bytes=%zu\n",
                       uploaded_texture_index, stage,
                       static_cast<unsigned long long>(shader.ucode_data_hash()), i, fetch_constant,
                       texture_width, texture_height, storage[i].size());
          std::fflush(stderr);
        }
      } else {
        static std::atomic<uint32_t> skipped_texture_logs{0};
        uint32_t skipped_texture_index =
            skipped_texture_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (skipped_texture_index <= 12 || (skipped_texture_index & 0x3F) == 0) {
          std::fprintf(stderr,
                       "[metal] probe texture dummy#%u %s shader=%016llx binding=%zu fetch=%u "
                       "type=%u fmt=%u dim=%u\n",
                       skipped_texture_index, stage,
                       static_cast<unsigned long long>(shader.ucode_data_hash()), i, fetch_constant,
                       uint32_t(fetch.type), uint32_t(fetch.format), uint32_t(fetch.dimension));
          std::fflush(stderr);
        }
      }
    }
  };
  prepare_probe_textures("vs", vertex_shader, vertex_texture_storage, vertex_texture_slots);
  prepare_probe_textures("ps", pixel_shader, fragment_texture_storage, fragment_texture_slots);
  std::vector<ProbeSamplerSlot> vertex_sampler_slots =
      MakeProbeSamplerSlots(*register_file_, vertex_shader);
  std::vector<ProbeSamplerSlot> fragment_sampler_slots =
      MakeProbeSamplerSlots(*register_file_, pixel_shader);

  uint32_t probe_width = std::max<uint32_t>(fallback_output_width_, 1);
  uint32_t probe_height = std::max<uint32_t>(fallback_output_height_, 1);
  const uint8_t* probe_initial_bgra = nullptr;
  size_t probe_initial_row_pitch = 0;
  if (resolved_color_width_ == probe_width && resolved_color_height_ == probe_height &&
      resolved_color_bgra_.size() >= size_t(probe_width) * probe_height * 4) {
    probe_initial_bgra = resolved_color_bgra_.data();
    probe_initial_row_pitch = size_t(probe_width) * 4;
  }
  auto* vertex_translation = static_cast<MetalShader::MetalTranslation*>(
      vertex_shader.GetTranslation(vertex_modification));
  auto* pixel_shader_translation =
      static_cast<MetalShader::MetalTranslation*>(pixel_shader.GetTranslation(pixel_modification));
  VertexMslBufferBindings vertex_buffer_bindings = GetVertexMslBufferBindings(vertex_translation);
  void* resident_shared_memory_buffer = nullptr;
  if (vertex_buffer_bindings.shared_memory != UINT32_MAX) {
    if (!EnsureVertexFetchRangesResident(vertex_shader)) {
      static std::atomic<uint32_t> vertex_residency_failure_logs{0};
      uint32_t failure_index =
          vertex_residency_failure_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (failure_index <= 16 || (failure_index & 0xFF) == 0) {
        std::fprintf(stderr,
                     "[metal] guest pipeline vertex residency failed#%u vs=%016llx\n",
                     failure_index,
                     static_cast<unsigned long long>(vertex_shader.ucode_data_hash()));
        std::fflush(stderr);
      }
      return;
    }
    resident_shared_memory_buffer = shared_memory_->buffer();
  }
  uint32_t fragment_float_constants_buffer_index =
      pixel_shader_translation
          ? FindMslBufferIndex(pixel_shader_translation->msl_source(), "xe_uniform_float_constants")
          : UINT32_MAX;
  uint32_t fragment_fetch_constants_buffer_index =
      pixel_shader_translation
          ? FindMslBufferIndex(pixel_shader_translation->msl_source(), "xe_uniform_fetch_constants")
          : UINT32_MAX;
  uint32_t fragment_bool_loop_constants_buffer_index =
      pixel_shader_translation ? FindMslBufferIndex(pixel_shader_translation->msl_source(),
                                                    "xe_uniform_bool_loop_constants")
                               : UINT32_MAX;
  std::vector<uint32_t> vertex_float_constants =
      PackFloatConstantsForShader(*register_file_, vertex_shader);
  std::vector<uint32_t> fragment_float_constants =
      PackFloatConstantsForShader(*register_file_, pixel_shader);
  const void* vertex_float_constants_data =
      vertex_float_constants.empty() ? nullptr : vertex_float_constants.data();
  size_t vertex_float_constants_size = vertex_float_constants.size() * sizeof(uint32_t);
  const void* fragment_float_constants_data =
      fragment_float_constants.empty() ? nullptr : fragment_float_constants.data();
  size_t fragment_float_constants_size = fragment_float_constants.size() * sizeof(uint32_t);
  if (first_pipeline_probe) {
    static std::atomic<uint32_t> vertex_buffer_binding_logs{0};
    uint32_t vertex_buffer_binding_index =
        vertex_buffer_binding_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (vertex_buffer_binding_index <= 16 || (vertex_buffer_binding_index & 0x3F) == 0) {
      const uint32_t* fetch_47 = fetch_constants_.data() + 47 * 4;
      std::fprintf(
          stderr,
          "[metal] shader MSL buffers#%u vs=%016llx ps=%016llx "
          "v_shared=%u v_float=%u v_bool=%u v_fetch=%u "
          "f_float=%u f_bool=%u f_fetch=%u "
          "fetch47=%08x %08x %08x %08x index(base=%d min=%u max=%u endian=%u "
          "load=0x%08x flags=0x%08x)\n",
          vertex_buffer_binding_index,
          static_cast<unsigned long long>(vertex_shader.ucode_data_hash()),
          static_cast<unsigned long long>(pixel_shader.ucode_data_hash()),
          vertex_buffer_bindings.shared_memory, vertex_buffer_bindings.float_constants,
          vertex_buffer_bindings.bool_loop_constants, vertex_buffer_bindings.fetch_constants,
          fragment_float_constants_buffer_index, fragment_bool_loop_constants_buffer_index,
          fragment_fetch_constants_buffer_index, fetch_47[0], fetch_47[1], fetch_47[2], fetch_47[3],
          system_constants_.vertex_base_index, system_constants_.vertex_index_min,
          system_constants_.vertex_index_max, uint32_t(system_constants_.vertex_index_endian),
          system_constants_.vertex_index_load_address, system_constants_.flags);
      uint32_t logged_fetches = 0;
      const Shader::ConstantRegisterMap& constant_map = vertex_shader.constant_register_map();
      for (uint32_t bitmap_index = 0; bitmap_index < rex::countof(constant_map.vertex_fetch_bitmap);
           ++bitmap_index) {
        uint32_t bits = constant_map.vertex_fetch_bitmap[bitmap_index];
        uint32_t bit_index = 0;
        while (rex::bit_scan_forward(bits, &bit_index)) {
          bits &= ~(UINT32_C(1) << bit_index);
          uint32_t fetch_index = bitmap_index * 32 + bit_index;
          const uint32_t* fetch_words = fetch_constants_.data() + fetch_index * 2;
          std::fprintf(stderr, "[metal]   used vfetch%u=%08x %08x\n", fetch_index,
                       fetch_words[0], fetch_words[1]);
          if (++logged_fetches >= 8) {
            break;
          }
        }
        if (logged_fetches >= 8) {
          break;
        }
      }
      std::fflush(stderr);
    }
  }
  ProbeIndexBuffer probe_index_buffer;
  const ProbeIndexBuffer* probe_index_buffer_ptr = nullptr;
  if (primitive_processing_result && primitive_processing_result->index_buffer_type !=
                                         PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
    const void* index_data = nullptr;
    void* index_metal_buffer = nullptr;
    size_t index_data_size = 0;
    size_t index_buffer_offset = 0;
    if (primitive_processing_result->index_buffer_type ==
        PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA) {
      index_metal_buffer = shared_memory_ ? shared_memory_->buffer() : nullptr;
      index_data_size = SharedMemory::kBufferSize;
      index_buffer_offset = primitive_processing_result->guest_index_base;
    } else if (primitive_processor_) {
      auto* metal_primitive_processor =
          static_cast<MetalPrimitiveProcessor*>(primitive_processor_.get());
      metal_primitive_processor->GetProcessedIndexBufferData(*primitive_processing_result,
                                                             index_data, index_data_size);
    }
    if ((!index_data && !index_metal_buffer) || !index_data_size) {
      static std::atomic<uint32_t> index_buffer_failure_logs{0};
      uint32_t failure_index =
          index_buffer_failure_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (failure_index <= 16 || (failure_index & 0x3F) == 0) {
        std::fprintf(stderr,
                     "[metal] guest pipeline index buffer unavailable#%u type=%u "
                     "count=%u handle=%zu base=0x%08x\n",
                     failure_index, uint32_t(primitive_processing_result->index_buffer_type),
                     primitive_processing_result->host_draw_vertex_count,
                     primitive_processing_result->host_index_buffer_handle,
                     primitive_processing_result->guest_index_base);
        std::fflush(stderr);
      }
      return;
    }
    probe_index_buffer.data = index_data;
    probe_index_buffer.metal_buffer = index_metal_buffer;
    probe_index_buffer.size = index_data_size;
    probe_index_buffer.offset = index_buffer_offset;
    probe_index_buffer.index_size =
        primitive_processing_result->host_index_format == xenos::IndexFormat::kInt16 ? 2 : 4;
    probe_index_buffer_ptr = &probe_index_buffer;
  }
  SpirvShaderTranslator::SystemConstants probe_system_constants = system_constants_;
  if (persistent_context) {
    std::string persistent_probe_error;
    bool persistent_probe_ok = RenderPipelineProbeToContext(
        persistent_context, pipeline_state, &probe_system_constants, sizeof(probe_system_constants),
        vertex_float_constants_data, vertex_float_constants_size, fetch_constants_.data(),
        fetch_constants_.size() * sizeof(uint32_t), memory_->physical_membase(), size_t(0x20000000),
        resident_shared_memory_buffer,
        vertex_texture_slots.empty() ? nullptr : vertex_texture_slots.data(),
        vertex_texture_slots.size(), vertex_shader.GetSamplerBindingsAfterTranslation().size(),
        fragment_texture_slots.empty() ? nullptr : fragment_texture_slots.data(),
        fragment_texture_slots.size(), pixel_shader.GetSamplerBindingsAfterTranslation().size(),
        probe_primitive_type, probe_vertex_count, probe_width, probe_height,
        &persistent_probe_error, vertex_buffer_bindings.shared_memory,
        vertex_buffer_bindings.float_constants, vertex_buffer_bindings.fetch_constants,
        fragment_float_constants_data, fragment_float_constants_size,
        fragment_float_constants_buffer_index, fragment_fetch_constants_buffer_index,
        vertex_sampler_slots.empty() ? nullptr : vertex_sampler_slots.data(),
        fragment_sampler_slots.empty() ? nullptr : fragment_sampler_slots.data(), nullptr, 0,
        UINT32_MAX, bool_loop_constants_.data(), bool_loop_constants_.size() * sizeof(uint32_t),
        vertex_buffer_bindings.bool_loop_constants, fragment_bool_loop_constants_buffer_index,
        probe_index_buffer_ptr);
    ++pipeline_probe_draws_this_swap_;
    if (!persistent_probe_ok) {
      ++pipeline_probe_failure_count_;
      if (pipeline_probe_failure_count_ <= 8 || (pipeline_probe_failure_count_ & 0x3F) == 0) {
        std::fprintf(stderr, "[metal] %s failed#%u vs=%016llx ps=%016llx: %s\n", persistent_label,
                     pipeline_probe_failure_count_,
                     static_cast<unsigned long long>(vertex_shader.ucode_data_hash()),
                     static_cast<unsigned long long>(pixel_shader.ucode_data_hash()),
                     persistent_probe_error.c_str());
        std::fflush(stderr);
      }
      return;
    }
    ++pipeline_probe_success_count_;
    bool should_read = first_pipeline_probe || pipeline_probe_success_count_ <= 8 ||
                       (pipeline_probe_success_count_ & 0x3F) == 0;
    bool nonzero = false;
    if (should_read) {
      if (host_render_target_debug) {
        RefreshHostRenderTargetBacking(probe_width, probe_height);
        nonzero = BgraHasNonZeroRgb(latest_host_render_target_bgra_);
        if (!nonzero && MetalHostRenderTargetSolidTestEnabled()) {
          if (void* solid_pipeline_state = EnsureSolidColorPipeline(vertex_shader)) {
            std::string solid_context_error;
            bool solid_context_ok = RenderPipelineProbeToContext(
                persistent_context, solid_pipeline_state, &probe_system_constants,
                sizeof(probe_system_constants), vertex_float_constants_data,
                vertex_float_constants_size, fetch_constants_.data(),
                fetch_constants_.size() * sizeof(uint32_t), memory_->physical_membase(),
                size_t(0x20000000), resident_shared_memory_buffer,
                vertex_texture_slots.empty() ? nullptr : vertex_texture_slots.data(),
                vertex_texture_slots.size(),
                vertex_shader.GetSamplerBindingsAfterTranslation().size(), nullptr, 0, 0,
                probe_primitive_type, probe_vertex_count, probe_width, probe_height,
                &solid_context_error, vertex_buffer_bindings.shared_memory,
                vertex_buffer_bindings.float_constants, vertex_buffer_bindings.fetch_constants,
                nullptr, 0, UINT32_MAX, UINT32_MAX,
                vertex_sampler_slots.empty() ? nullptr : vertex_sampler_slots.data(), nullptr);
            bool solid_read_ok =
                solid_context_ok && RefreshHostRenderTargetBacking(probe_width, probe_height);
            nonzero = solid_read_ok && BgraHasNonZeroRgb(latest_host_render_target_bgra_);
            static std::atomic<uint32_t> host_rt_solid_logs{0};
            uint32_t host_rt_solid_index =
                host_rt_solid_logs.fetch_add(1, std::memory_order_relaxed) + 1;
            if (host_rt_solid_index <= 16 || (host_rt_solid_index & 0x3F) == 0) {
              std::fprintf(stderr,
                           "[metal] host render target solid test#%u ok=%u read=%u "
                           "nonzero=%u vs=%016llx %s\n",
                           host_rt_solid_index, solid_context_ok ? 1u : 0u, solid_read_ok ? 1u : 0u,
                           nonzero ? 1u : 0u,
                           static_cast<unsigned long long>(vertex_shader.ucode_data_hash()),
                           solid_context_ok ? "" : solid_context_error.c_str());
              std::fflush(stderr);
            }
          }
        }
      } else {
        RefreshPipelineProbeBacking(probe_width, probe_height);
        nonzero = BgraHasNonZeroRgb(latest_pipeline_probe_bgra_);
      }
      if (!host_render_target_debug && !nonzero) {
        if (void* solid_pipeline_state = EnsureSolidColorPipeline(vertex_shader)) {
          std::vector<uint8_t> solid_bgra;
          std::string solid_error;
          bool solid_ok = RenderPipelineProbe(
              metal_device_, solid_pipeline_state, &probe_system_constants,
              sizeof(probe_system_constants), vertex_float_constants_data,
              vertex_float_constants_size, fetch_constants_.data(),
              fetch_constants_.size() * sizeof(uint32_t), memory_->physical_membase(),
              size_t(0x20000000), resident_shared_memory_buffer,
              vertex_texture_slots.empty() ? nullptr : vertex_texture_slots.data(),
              vertex_texture_slots.size(),
              vertex_shader.GetSamplerBindingsAfterTranslation().size(), nullptr, 0, 0,
              probe_primitive_type, probe_vertex_count, 320, 180, solid_bgra, &solid_error,
              vertex_buffer_bindings.shared_memory, vertex_buffer_bindings.float_constants,
              vertex_buffer_bindings.fetch_constants, nullptr, 0, nullptr, 0, UINT32_MAX,
              UINT32_MAX, vertex_sampler_slots.empty() ? nullptr : vertex_sampler_slots.data(),
              nullptr);
          uint32_t solid_visible = solid_ok ? CountVisibleRgbPixels(solid_bgra) : 0;
          static std::atomic<uint32_t> solid_probe_logs{0};
          uint32_t solid_probe_index = solid_probe_logs.fetch_add(1, std::memory_order_relaxed) + 1;
          if (first_pipeline_probe || solid_probe_index <= 16 || (solid_probe_index & 0x3F) == 0) {
            std::fprintf(stderr, "[metal] solid vertex probe#%u ok=%u visible=%u vs=%016llx %s\n",
                         solid_probe_index, solid_ok ? 1u : 0u, solid_visible,
                         static_cast<unsigned long long>(vertex_shader.ucode_data_hash()),
                         solid_ok ? "" : solid_error.c_str());
            std::fflush(stderr);
          }
        }
      }
      if (!host_render_target_debug && !nonzero && pixel_shader_translation &&
          pixel_shader_translation->metal_library() && !fragment_texture_slots.empty()) {
        std::string fullscreen_vertex_error;
        void* fullscreen_vertex_library = CreateMslLibrary(
            metal_device_, MakeFullscreenProbeVertexMsl(), &fullscreen_vertex_error);
        if (fullscreen_vertex_library) {
          std::string fullscreen_pipeline_error;
          void* fullscreen_pipeline_state = CreateRenderPipelineState(
              metal_device_, fullscreen_vertex_library, pixel_shader_translation->metal_library(),
              &fullscreen_pipeline_error);
          if (fullscreen_pipeline_state) {
            std::vector<uint8_t> fullscreen_probe_bgra;
            std::string fullscreen_probe_error;
            bool fullscreen_probe_ok = RenderPipelineProbe(
                metal_device_, fullscreen_pipeline_state, &probe_system_constants,
                sizeof(probe_system_constants), nullptr, 0, fetch_constants_.data(),
                fetch_constants_.size() * sizeof(uint32_t), memory_->physical_membase(),
                size_t(0x20000000), nullptr, nullptr, 0, 0,
                fragment_texture_slots.empty() ? nullptr : fragment_texture_slots.data(),
                fragment_texture_slots.size(),
                pixel_shader.GetSamplerBindingsAfterTranslation().size(),
                uint32_t(xenos::PrimitiveType::kTriangleList), 3, probe_width, probe_height,
                fullscreen_probe_bgra, &fullscreen_probe_error, UINT32_MAX, UINT32_MAX, UINT32_MAX,
                nullptr, 0, fragment_float_constants_data, fragment_float_constants_size,
                fragment_float_constants_buffer_index, fragment_fetch_constants_buffer_index,
                nullptr, fragment_sampler_slots.empty() ? nullptr : fragment_sampler_slots.data());
            uint32_t fullscreen_visible =
                fullscreen_probe_ok ? CountVisibleRgbPixels(fullscreen_probe_bgra) : 0;
            bool fullscreen_flat = fullscreen_probe_ok &&
                                   IsDominantFlatVisibleFrame(fullscreen_probe_bgra, probe_width,
                                                              probe_height, fullscreen_visible);
            bool fullscreen_nonzero = fullscreen_visible != 0 && !fullscreen_flat;
            static std::atomic<uint32_t> fullscreen_probe_logs{0};
            uint32_t fullscreen_probe_index =
                fullscreen_probe_logs.fetch_add(1, std::memory_order_relaxed) + 1;
            if (fullscreen_probe_index <= 12 || (fullscreen_probe_index & 0x3F) == 0) {
              std::fprintf(stderr,
                           "[metal] persistent fullscreen PS probe#%u ok=%u visible=%u "
                           "accepted=%u ps=%016llx %s\n",
                           fullscreen_probe_index, fullscreen_probe_ok ? 1u : 0u,
                           fullscreen_visible, fullscreen_nonzero ? 1u : 0u,
                           static_cast<unsigned long long>(pixel_shader.ucode_data_hash()),
                           fullscreen_probe_ok ? "" : fullscreen_probe_error.c_str());
              std::fflush(stderr);
            }
            if (fullscreen_nonzero) {
              CompositeVisibleToResolvedColorBacking(fullscreen_probe_bgra, probe_width,
                                                     probe_height);
              latest_pipeline_probe_bgra_ = std::move(fullscreen_probe_bgra);
              latest_pipeline_probe_width_ = probe_width;
              latest_pipeline_probe_height_ = probe_height;
              nonzero = true;
            }
            ReleaseRenderPipelineState(fullscreen_pipeline_state);
          } else {
            static std::atomic<uint32_t> fullscreen_pipeline_logs{0};
            uint32_t fullscreen_pipeline_index =
                fullscreen_pipeline_logs.fetch_add(1, std::memory_order_relaxed) + 1;
            if (fullscreen_pipeline_index <= 8 || (fullscreen_pipeline_index & 0x3F) == 0) {
              std::fprintf(stderr,
                           "[metal] persistent fullscreen PS pipeline failed#%u ps=%016llx: "
                           "%s\n",
                           fullscreen_pipeline_index,
                           static_cast<unsigned long long>(pixel_shader.ucode_data_hash()),
                           fullscreen_pipeline_error.c_str());
              std::fflush(stderr);
            }
          }
          ReleaseMslLibrary(fullscreen_vertex_library);
        } else {
          static std::atomic<uint32_t> fullscreen_vertex_logs{0};
          uint32_t fullscreen_vertex_index =
              fullscreen_vertex_logs.fetch_add(1, std::memory_order_relaxed) + 1;
          if (fullscreen_vertex_index <= 8 || (fullscreen_vertex_index & 0x3F) == 0) {
            std::fprintf(stderr, "[metal] persistent fullscreen probe VS compile failed#%u: %s\n",
                         fullscreen_vertex_index, fullscreen_vertex_error.c_str());
            std::fflush(stderr);
          }
        }
      }
    }
    if (first_pipeline_probe || pipeline_probe_success_count_ <= 8 ||
        (pipeline_probe_success_count_ & 0x3F) == 0) {
      std::fprintf(stderr,
                   "[metal] %s ok#%u draw_in_swap=%u nonzero=%u "
                   "vs=%016llx ps=%016llx\n",
                   persistent_label, pipeline_probe_success_count_, pipeline_probe_draws_this_swap_,
                   nonzero ? 1u : 0u,
                   static_cast<unsigned long long>(vertex_shader.ucode_data_hash()),
                   static_cast<unsigned long long>(pixel_shader.ucode_data_hash()));
      std::fflush(stderr);
    }
    return;
  }
  std::vector<uint8_t> probe_bgra;
  std::string probe_error;
  bool probe_ok = RenderPipelineProbe(
      metal_device_, pipeline_state, &probe_system_constants, sizeof(probe_system_constants),
      vertex_float_constants_data, vertex_float_constants_size, fetch_constants_.data(),
      fetch_constants_.size() * sizeof(uint32_t), memory_->physical_membase(), size_t(0x20000000),
      resident_shared_memory_buffer,
      vertex_texture_slots.empty() ? nullptr : vertex_texture_slots.data(),
      vertex_texture_slots.size(), vertex_shader.GetSamplerBindingsAfterTranslation().size(),
      fragment_texture_slots.empty() ? nullptr : fragment_texture_slots.data(),
      fragment_texture_slots.size(), pixel_shader.GetSamplerBindingsAfterTranslation().size(),
      probe_primitive_type, probe_vertex_count, probe_width, probe_height, probe_bgra, &probe_error,
      vertex_buffer_bindings.shared_memory, vertex_buffer_bindings.float_constants,
      vertex_buffer_bindings.fetch_constants, probe_initial_bgra, probe_initial_row_pitch,
      fragment_float_constants_data, fragment_float_constants_size,
      fragment_float_constants_buffer_index, fragment_fetch_constants_buffer_index,
      vertex_sampler_slots.empty() ? nullptr : vertex_sampler_slots.data(),
      fragment_sampler_slots.empty() ? nullptr : fragment_sampler_slots.data(), nullptr, 0,
      UINT32_MAX, bool_loop_constants_.data(), bool_loop_constants_.size() * sizeof(uint32_t),
      vertex_buffer_bindings.bool_loop_constants, fragment_bool_loop_constants_buffer_index,
      probe_index_buffer_ptr);
  if (!probe_ok) {
    ++pipeline_probe_failure_count_;
    if (pipeline_probe_failure_count_ <= 8 || (pipeline_probe_failure_count_ & 0x3F) == 0) {
      std::fprintf(stderr, "[metal] guest pipeline probe failed#%u vs=%016llx ps=%016llx: %s\n",
                   pipeline_probe_failure_count_,
                   static_cast<unsigned long long>(vertex_shader.ucode_data_hash()),
                   static_cast<unsigned long long>(pixel_shader.ucode_data_hash()),
                   probe_error.c_str());
      std::fflush(stderr);
    }
    return;
  }

  bool nonzero = BgraHasNonZeroRgb(probe_bgra);
  if (!nonzero && pixel_shader_translation && pixel_shader_translation->metal_library() &&
      !fragment_texture_slots.empty()) {
    std::string fullscreen_vertex_error;
    void* fullscreen_vertex_library =
        CreateMslLibrary(metal_device_, MakeFullscreenProbeVertexMsl(), &fullscreen_vertex_error);
    if (fullscreen_vertex_library) {
      std::string fullscreen_pipeline_error;
      void* fullscreen_pipeline_state = CreateRenderPipelineState(
          metal_device_, fullscreen_vertex_library, pixel_shader_translation->metal_library(),
          &fullscreen_pipeline_error);
      if (fullscreen_pipeline_state) {
        SpirvShaderTranslator::SystemConstants fullscreen_system_constants = system_constants_;
        uint32_t alpha_shift = SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
        fullscreen_system_constants.flags &= ~(UINT32_C(7) << alpha_shift);
        fullscreen_system_constants.flags |= uint32_t(xenos::CompareFunction::kAlways)
                                             << alpha_shift;
        fullscreen_system_constants.alpha_to_mask = 0;
        for (uint32_t i = 0; i < 4; ++i) {
          fullscreen_system_constants.color_exp_bias[i] = 1.0f;
        }
        std::vector<uint8_t> fullscreen_probe_bgra;
        std::string fullscreen_probe_error;
        bool fullscreen_probe_ok = RenderPipelineProbe(
            metal_device_, fullscreen_pipeline_state, &fullscreen_system_constants,
            sizeof(fullscreen_system_constants), nullptr, 0, fetch_constants_.data(),
            fetch_constants_.size() * sizeof(uint32_t), memory_->physical_membase(),
            size_t(0x20000000), nullptr, nullptr, 0, 0,
            fragment_texture_slots.empty() ? nullptr : fragment_texture_slots.data(),
            fragment_texture_slots.size(), pixel_shader.GetSamplerBindingsAfterTranslation().size(),
            uint32_t(xenos::PrimitiveType::kTriangleList), 3, probe_width, probe_height,
            fullscreen_probe_bgra, &fullscreen_probe_error, UINT32_MAX, UINT32_MAX, UINT32_MAX,
            nullptr, 0, fragment_float_constants_data, fragment_float_constants_size,
            fragment_float_constants_buffer_index, fragment_fetch_constants_buffer_index, nullptr,
            fragment_sampler_slots.empty() ? nullptr : fragment_sampler_slots.data());
        bool fullscreen_nonzero = fullscreen_probe_ok && BgraHasNonZeroRgb(fullscreen_probe_bgra);
        static std::atomic<uint32_t> fullscreen_probe_logs{0};
        uint32_t fullscreen_probe_index =
            fullscreen_probe_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fullscreen_probe_index <= 8 || (fullscreen_probe_index & 0x3F) == 0) {
          std::fprintf(stderr, "[metal] fullscreen PS probe#%u ok=%u nonzero=%u ps=%016llx %s\n",
                       fullscreen_probe_index, fullscreen_probe_ok ? 1u : 0u,
                       fullscreen_nonzero ? 1u : 0u,
                       static_cast<unsigned long long>(pixel_shader.ucode_data_hash()),
                       fullscreen_probe_ok ? "" : fullscreen_probe_error.c_str());
          std::fflush(stderr);
        }
        if (fullscreen_nonzero) {
          probe_bgra = std::move(fullscreen_probe_bgra);
          nonzero = true;
        }
        ReleaseRenderPipelineState(fullscreen_pipeline_state);
      } else {
        static std::atomic<uint32_t> fullscreen_pipeline_logs{0};
        uint32_t fullscreen_pipeline_index =
            fullscreen_pipeline_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fullscreen_pipeline_index <= 8 || (fullscreen_pipeline_index & 0x3F) == 0) {
          std::fprintf(stderr, "[metal] fullscreen PS pipeline failed#%u ps=%016llx: %s\n",
                       fullscreen_pipeline_index,
                       static_cast<unsigned long long>(pixel_shader.ucode_data_hash()),
                       fullscreen_pipeline_error.c_str());
          std::fflush(stderr);
        }
      }
      ReleaseMslLibrary(fullscreen_vertex_library);
    } else {
      static std::atomic<uint32_t> fullscreen_vertex_logs{0};
      uint32_t fullscreen_vertex_index =
          fullscreen_vertex_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (fullscreen_vertex_index <= 8 || (fullscreen_vertex_index & 0x3F) == 0) {
        std::fprintf(stderr, "[metal] fullscreen probe VS compile failed#%u: %s\n",
                     fullscreen_vertex_index, fullscreen_vertex_error.c_str());
        std::fflush(stderr);
      }
    }
  }
  if (nonzero) {
    CompositeVisibleToResolvedColorBacking(probe_bgra, probe_width, probe_height);
    latest_pipeline_probe_bgra_ = std::move(probe_bgra);
    latest_pipeline_probe_width_ = probe_width;
    latest_pipeline_probe_height_ = probe_height;
  }
  ++pipeline_probe_success_count_;
  if (pipeline_probe_success_count_ <= 8 || (pipeline_probe_success_count_ & 0x3F) == 0) {
    std::fprintf(stderr, "[metal] guest pipeline probe ok#%u nonzero=%u vs=%016llx ps=%016llx\n",
                 pipeline_probe_success_count_, nonzero ? 1u : 0u,
                 static_cast<unsigned long long>(vertex_shader.ucode_data_hash()),
                 static_cast<unsigned long long>(pixel_shader.ucode_data_hash()));
    std::fflush(stderr);
  }
}

bool MetalCommandProcessor::RetainTextureCandidateIfUseful(const std::vector<uint8_t>& bgra,
                                                           uint32_t width, uint32_t height,
                                                           const char* label) {
  if (!MetalHeuristicPresentationEnabled()) {
    return false;
  }
  auto log_reject = [&](const char* reason, const BgraFrameStats* stats, uint32_t spatial_distance,
                        uint64_t score) {
    if (width < 640 || height < 360) {
      return;
    }
    static std::atomic<uint32_t> rejected_texture_candidate_logs{0};
    uint32_t rejected_texture_candidate_index =
        rejected_texture_candidate_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (rejected_texture_candidate_index <= 16 || (rejected_texture_candidate_index & 0x3F) == 0) {
      std::fprintf(stderr,
                   "[metal] rejected texture candidate#%u source=%s reason=%s size=%ux%u "
                   "visible=%u range=%u spatial=%u score=%llu best=%llu\n",
                   rejected_texture_candidate_index, label ? label : "unknown", reason, width,
                   height, stats ? stats->visible_pixels : 0, stats ? BgraRgbRange(*stats) : 0,
                   spatial_distance, static_cast<unsigned long long>(score),
                   static_cast<unsigned long long>(latest_texture_candidate_score_));
      std::fflush(stderr);
    }
  };
  if (!width || !height || bgra.size() < size_t(width) * height * 4) {
    log_reject("invalid", nullptr, 0, 0);
    return false;
  }
  BgraFrameStats stats = GetBgraFrameStats(bgra);
  uint32_t pixel_count = width * height;
  bool useful_sparse = IsUsefulSparseVisibleFrame(bgra, width, height, stats);
  if (!stats.visible_pixels || (stats.visible_pixels * 3 < pixel_count && !useful_sparse)) {
    log_reject("sparse", &stats, BgraVisibleGridCellCount(bgra, width, height),
               CandidateFrameScore(stats));
    return false;
  }
  if (IsDominantFlatVisibleFrame(bgra, width, height, stats.visible_pixels)) {
    log_reject("dominant-flat", &stats, BgraVisibleSpatialSampleColorDistance(bgra, width, height),
               CandidateFrameScore(stats));
    return false;
  }
  uint32_t spatial_distance = BgraVisibleSpatialSampleColorDistance(bgra, width, height);
  if (spatial_distance <= 8 && BgraRgbRange(stats) <= 32) {
    log_reject("low-variation", &stats, spatial_distance, CandidateFrameScore(stats));
    return false;
  }
  uint64_t score = CandidateFrameScore(stats, spatial_distance);
  if (score <= latest_texture_candidate_score_) {
    log_reject("lower-score", &stats, spatial_distance, score);
    return false;
  }
  latest_texture_candidate_bgra_ = bgra;
  latest_texture_candidate_width_ = width;
  latest_texture_candidate_height_ = height;
  latest_texture_candidate_score_ = score;
  static std::atomic<uint32_t> retained_texture_candidate_logs{0};
  uint32_t retained_texture_candidate_index =
      retained_texture_candidate_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (retained_texture_candidate_index <= 16 || (retained_texture_candidate_index & 0x3F) == 0) {
    std::fprintf(stderr,
                 "[metal] retained texture candidate#%u source=%s size=%ux%u visible=%u "
                 "range=%u spatial=%u score=%llu\n",
                 retained_texture_candidate_index, label ? label : "unknown", width, height,
                 stats.visible_pixels, BgraRgbRange(stats), spatial_distance,
                 static_cast<unsigned long long>(score));
    std::fflush(stderr);
  }
  return true;
}

bool MetalCommandProcessor::DecodeTextureFetchToRgba(const xenos::xe_gpu_texture_fetch_t& fetch,
                                                     uint32_t fallback_base_physical,
                                                     uint32_t decode_base_override_physical,
                                                     std::vector<uint8_t>& rgba_out,
                                                     uint32_t& width_out, uint32_t& height_out) {
  if (!memory_ || !register_file_) {
    return false;
  }

  if (fetch.type != xenos::FetchConstantType::kTexture) {
    return false;
  }

  uint32_t width_minus_1 = 0;
  uint32_t height_minus_1 = 0;
  uint32_t depth_or_array_size_minus_1 = 0;
  uint32_t base_page = 0;
  uint32_t mip_page = 0;
  uint32_t mip_max_level = 0;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                 &depth_or_array_size_minus_1, &base_page,
                                                 &mip_page, nullptr, &mip_max_level);
  (void)mip_page;
  (void)mip_max_level;

  xenos::TextureFormat format = GetBaseFormat(fetch.format);
  uint32_t width = width_minus_1 + 1;
  uint32_t height = height_minus_1 + 1;
  uint32_t base_physical = base_page ? (base_page << 12) : fallback_base_physical;
  uint32_t decode_base_physical = base_physical;
  if (decode_base_override_physical) {
    decode_base_physical = decode_base_override_physical;
  }

  if (format != xenos::TextureFormat::k_8_8_8_8 ||
      fetch.dimension != xenos::DataDimension::k2DOrStacked || depth_or_array_size_minus_1 != 0 ||
      !width || !height) {
    return false;
  }

  const FormatInfo* format_info = FormatInfo::Get(format);
  if (!format_info || format_info->bytes_per_block() != 4) {
    return false;
  }

  if (!decode_base_override_physical &&
      DecodeResolvedColorBackingToRgba(base_physical, width, height, rgba_out)) {
    width_out = width;
    height_out = height;
    return true;
  }

  const uint8_t* guest_base = memory_->TranslatePhysical<const uint8_t*>(decode_base_physical);
  if (!guest_base) {
    return false;
  }

  uint32_t pitch_texels = fetch.pitch << 5;
  if (pitch_texels < width) {
    pitch_texels = width;
  }

  bool watched_framebuffer_read =
      RangesOverlap(decode_base_physical, uint32_t(uint64_t(pitch_texels) * height * 4),
                    kWatchedFramebufferBase, kWatchedFramebufferLength);
  size_t watched_raw_nonzero_bytes = 0;
  size_t watched_raw_nonzero_rgb_bytes = 0;
  if (watched_framebuffer_read) {
    size_t raw_size = size_t(pitch_texels) * height * 4;
    for (size_t i = 0; i < raw_size; ++i) {
      uint8_t value = guest_base[i];
      watched_raw_nonzero_bytes += value != 0;
      watched_raw_nonzero_rgb_bytes += (i & 3u) != 3u && value != 0;
    }
  }

  rgba_out.resize(size_t(width) * height * 4);
  if (fetch.tiled) {
    texture_conversion::UntileInfo untile_info;
    untile_info.offset_x = 0;
    untile_info.offset_y = 0;
    untile_info.width = width;
    untile_info.height = height;
    untile_info.input_pitch = pitch_texels;
    untile_info.output_pitch = width;
    untile_info.input_format_info = format_info;
    untile_info.output_format_info = format_info;
    untile_info.copy_callback = [endian = fetch.endianness](void* output, const void* input,
                                                            size_t length) {
      texture_conversion::CopySwapBlock(endian, output, input, length);
    };
    texture_conversion::Untile(rgba_out.data(), guest_base, &untile_info);
  } else {
    size_t guest_row_pitch = size_t(pitch_texels) * 4;
    size_t output_row_pitch = size_t(width) * 4;
    for (uint32_t y = 0; y < height; ++y) {
      texture_conversion::CopySwapBlock(fetch.endianness,
                                        rgba_out.data() + size_t(y) * output_row_pitch,
                                        guest_base + size_t(y) * guest_row_pitch, output_row_pitch);
    }
  }

  size_t nonzero_rgb_count = 0;
  size_t top_sampled_band_rgb_count = 0;
  size_t middle_band_rgb_count = 0;
  size_t lower_band_rgb_count = 0;
  uint8_t min_rgb = 0xFF;
  uint8_t max_rgb = 0;
  for (size_t pixel = 0, pixel_count = size_t(width) * height; pixel < pixel_count; ++pixel) {
    const uint8_t* rgba = rgba_out.data() + pixel * 4;
    for (uint32_t c = 0; c < 3; ++c) {
      min_rgb = std::min(min_rgb, rgba[c]);
      max_rgb = std::max(max_rgb, rgba[c]);
      bool nonzero_component = rgba[c] != 0;
      nonzero_rgb_count += nonzero_component;
      if (nonzero_component && width) {
        size_t y = pixel / width;
        top_sampled_band_rgb_count += y < 208;
        middle_band_rgb_count += y >= 208 && y < 512;
        lower_band_rgb_count += y >= 512;
      }
    }
  }

  if (watched_framebuffer_read) {
    static std::atomic<uint32_t> watched_texture_read_logs{0};
    uint32_t watched_read_index =
        watched_texture_read_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (watched_read_index <= 32 || (watched_read_index & 0xFF) == 0) {
      std::fprintf(stderr,
                   "[metal] watched framebuffer texture read#%u base=0x%08x size=%ux%u "
                   "pitch=%u tiled=%u raw_nonzero=%zu raw_rgb_nonzero=%zu "
                   "decoded_rgb_nonzero=%zu last_copy=0x%08x last_swap=0x%08x "
                   "override=0x%08x\n",
                   watched_read_index, decode_base_physical, width, height, pitch_texels,
                   uint32_t(fetch.tiled), watched_raw_nonzero_bytes, watched_raw_nonzero_rgb_bytes,
                   nonzero_rgb_count, last_copy_dest_base_, last_swap_frontbuffer_ptr_,
                   decode_base_override_physical);
      std::fflush(stderr);
    }
  }

  static std::atomic<uint32_t> decoded_texture_logs{0};
  uint32_t decoded_texture_index = decoded_texture_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (decoded_texture_index <= 12 || (decoded_texture_index & 0x3F) == 0) {
    const uint8_t* first = rgba_out.data();
    std::fprintf(stderr,
                 "[metal] decoded texture#%u base=0x%08x size=%ux%u pitch=%u tiled=%u fmt=%u "
                 "rgb_min=%u rgb_max=%u rgb_nonzero=%zu bands(top208=%zu mid=%zu low=%zu) "
                 "first_rgba=%02x %02x %02x %02x\n",
                 decoded_texture_index, decode_base_physical, width, height,
                 uint32_t(fetch.pitch << 5), uint32_t(fetch.tiled), uint32_t(fetch.format),
                 uint32_t(min_rgb), uint32_t(max_rgb), nonzero_rgb_count,
                 top_sampled_band_rgb_count, middle_band_rgb_count, lower_band_rgb_count, first[0],
                 first[1], first[2], first[3]);
    std::fflush(stderr);
  }

  if (!decode_base_override_physical && !nonzero_rgb_count &&
      MetalHostRenderTargetTextureAliasEnabled() && base_physical == UINT32_C(0x1ec30000) &&
      latest_host_render_target_width_ == width && latest_host_render_target_height_ == height &&
      latest_host_render_target_bgra_.size() >= size_t(width) * height * 4 &&
      BgraHasNonZeroRgb(latest_host_render_target_bgra_)) {
    BgraToRgba(latest_host_render_target_bgra_, width, height, rgba_out);
    width_out = width;
    height_out = height;
    static std::atomic<uint32_t> host_rt_alias_logs{0};
    uint32_t host_rt_alias_index = host_rt_alias_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (host_rt_alias_index <= 16 || (host_rt_alias_index & 0x3F) == 0) {
      std::fprintf(stderr,
                   "[metal] using host RT texture alias#%u base=0x%08x size=%ux%u "
                   "visible=%u\n",
                   host_rt_alias_index, base_physical, width, height,
                   CountVisibleRgbPixels(latest_host_render_target_bgra_));
      std::fflush(stderr);
    }
    return true;
  }

  std::vector<uint8_t> candidate_bgra(size_t(width) * height * 4);
  std::vector<uint8_t> raw_candidate_bgra(size_t(width) * height * 4);
  for (size_t pixel = 0, pixel_count = size_t(width) * height; pixel < pixel_count; ++pixel) {
    const uint8_t* rgba = rgba_out.data() + pixel * 4;
    RgbaToDrawableBgra(rgba, fetch.swizzle, candidate_bgra.data() + pixel * 4);
    uint8_t* raw_bgra = raw_candidate_bgra.data() + pixel * 4;
    raw_bgra[0] = rgba[2];
    raw_bgra[1] = rgba[1];
    raw_bgra[2] = rgba[0];
    raw_bgra[3] = rgba[3];
  }
  if (DebugFrameDumpEnabled() && base_physical == UINT32_C(0x1ec30000) &&
      width == fallback_output_width_ && height == fallback_output_height_) {
    static std::atomic<uint32_t> raw_texture_dump_logs{0};
    uint32_t raw_texture_dump_index =
        raw_texture_dump_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (raw_texture_dump_index <= 4) {
      DumpBgraFrameAsPpm("texture_raw_1ec30000", raw_texture_dump_index, raw_candidate_bgra, width,
                         height);
      DumpBgraFrameAsPpm("texture_swizzled_1ec30000", raw_texture_dump_index, candidate_bgra, width,
                         height);
    }
  }
  RetainTextureCandidateIfUseful(candidate_bgra, width, height, "decoded texture");
  RetainTextureCandidateIfUseful(raw_candidate_bgra, width, height, "decoded texture raw");

  BgraFrameStats raw_stats = GetBgraFrameStats(candidate_bgra);
  BgraFrameStats decode_baseline_stats = raw_stats;
  uint32_t raw_spatial = BgraVisibleSpatialSampleColorDistance(candidate_bgra, width, height);
  uint64_t decode_baseline_score = CandidateFrameScore(raw_stats, raw_spatial);
  if (!decode_base_override_physical) {
    // Do not replace a freshly decoded visible texture with a historical
    // retained frame for the same base. That policy is useful only as a black
    // frame fallback; during normal swaps it can pin presentation to an older
    // high-score frame while the game continues advancing.
  }

  if (MetalHeuristicPresentationEnabled() && !decode_base_override_physical &&
      width == fallback_output_width_ && height == fallback_output_height_) {
    const std::vector<uint8_t>* resolved_candidate = nullptr;
    const char* resolved_candidate_label = nullptr;
    BgraFrameStats resolved_stats = {};
    uint32_t resolved_spatial = 0;
    uint64_t best_resolved_score = 0;
    auto consider_resolved_candidate = [&](const std::vector<uint8_t>& candidate,
                                           uint32_t candidate_width, uint32_t candidate_height,
                                           const char* label) {
      if (candidate_width != width || candidate_height != height ||
          candidate.size() < size_t(width) * height * 4) {
        return;
      }
      BgraFrameStats stats = GetBgraFrameStats(candidate);
      uint32_t candidate_spatial =
          BgraVisibleSpatialSampleColorDistance(candidate, candidate_width, candidate_height);
      uint64_t candidate_score = CandidateFrameScore(stats, candidate_spatial);
      if (candidate_score <= best_resolved_score) {
        return;
      }
      resolved_candidate = &candidate;
      resolved_candidate_label = label;
      resolved_stats = stats;
      resolved_spatial = candidate_spatial;
      best_resolved_score = candidate_score;
    };
    consider_resolved_candidate(resolved_color_bgra_, resolved_color_width_, resolved_color_height_,
                                "resolved");
    consider_resolved_candidate(latest_draw_event_frame_bgra_, latest_draw_event_frame_width_,
                                latest_draw_event_frame_height_, "draw");
    consider_resolved_candidate(latest_host_pixel_frame_bgra_, latest_host_pixel_frame_width_,
                                latest_host_pixel_frame_height_, "host-pixel");
    uint64_t resolved_score = CandidateFrameScore(resolved_stats, resolved_spatial);
    bool resolved_is_better = resolved_candidate &&
                              resolved_stats.visible_pixels * 4 >= width * height &&
                              resolved_score > decode_baseline_score &&
                              !IsDominantFlatVisibleFrame(*resolved_candidate, width, height,
                                                          resolved_stats.visible_pixels);
    if (resolved_is_better) {
      BgraToRgba(*resolved_candidate, width, height, rgba_out);
      RetainResolvedFrameForBase(
          base_physical, *resolved_candidate, width, height,
          resolved_candidate_label ? resolved_candidate_label : "texture override alias");
      RetainTextureCandidateIfUseful(*resolved_candidate, width, height,
                                     "resolved texture override");
      static std::atomic<uint32_t> resolved_texture_override_logs{0};
      uint32_t resolved_texture_override_index =
          resolved_texture_override_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (resolved_texture_override_index <= 16 || (resolved_texture_override_index & 0x3F) == 0) {
        std::fprintf(stderr,
                     "[metal] using resolved texture override#%u base=0x%08x size=%ux%u "
                     "raw_visible=%u raw_score=%llu baseline_visible=%u "
                     "baseline_score=%llu resolved_visible=%u "
                     "resolved_score=%llu source=%s\n",
                     resolved_texture_override_index, base_physical, width, height,
                     raw_stats.visible_pixels,
                     static_cast<unsigned long long>(CandidateFrameScore(raw_stats, raw_spatial)),
                     decode_baseline_stats.visible_pixels,
                     static_cast<unsigned long long>(decode_baseline_score),
                     resolved_stats.visible_pixels, static_cast<unsigned long long>(resolved_score),
                     resolved_candidate_label ? resolved_candidate_label : "unknown");
        std::fflush(stderr);
      }
    } else {
      static std::atomic<uint32_t> resolved_texture_override_skip_logs{0};
      uint32_t resolved_texture_override_skip_index =
          resolved_texture_override_skip_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (resolved_texture_override_skip_index <= 16 ||
          (resolved_texture_override_skip_index & 0x3F) == 0) {
        std::fprintf(
            stderr,
            "[metal] skipped resolved texture override#%u base=0x%08x size=%ux%u "
            "raw_visible=%u raw_score=%llu baseline_visible=%u "
            "baseline_score=%llu resolved_visible=%u "
            "resolved_score=%llu flat=%u source=%s\n",
            resolved_texture_override_skip_index, base_physical, width, height,
            raw_stats.visible_pixels,
            static_cast<unsigned long long>(CandidateFrameScore(raw_stats, raw_spatial)),
            decode_baseline_stats.visible_pixels,
            static_cast<unsigned long long>(decode_baseline_score), resolved_stats.visible_pixels,
            static_cast<unsigned long long>(resolved_score),
            resolved_candidate && IsDominantFlatVisibleFrame(*resolved_candidate, width, height,
                                                             resolved_stats.visible_pixels)
                ? 1u
                : 0u,
            resolved_candidate_label ? resolved_candidate_label : "none");
        std::fflush(stderr);
      }
    }
  }

  width_out = width;
  height_out = height;
  return true;
}

bool MetalCommandProcessor::InterpreterTextureFetchThunk(
    void* user_data, const ucode::TextureFetchInstruction& instr, const float* coordinates,
    uint32_t coordinate_count, float* rgba_out) {
  return static_cast<MetalCommandProcessor*>(user_data)->SampleInterpreterTextureFetch(
      instr, coordinates, coordinate_count, rgba_out);
}

bool MetalCommandProcessor::SampleInterpreterTextureFetch(
    const ucode::TextureFetchInstruction& instr, const float* coordinates,
    uint32_t coordinate_count, float* rgba_out) {
  if (!register_file_ || !rgba_out || instr.opcode() != ucode::FetchOpcode::kTextureFetch ||
      instr.dimension() != xenos::FetchOpDimension::k2D || coordinate_count < 2) {
    return false;
  }

  xenos::xe_gpu_texture_fetch_t fetch =
      register_file_->GetTextureFetch(instr.fetch_constant_index());
  std::vector<uint8_t> rgba;
  uint32_t width = 0;
  uint32_t height = 0;
  if (!DecodeTextureFetchToRgba(fetch, 0, 0, rgba, width, height) || !width || !height) {
    return false;
  }

  float u = coordinates[0] + instr.offset_x();
  float v = coordinates[1] + instr.offset_y();
  if (!instr.unnormalized_coordinates()) {
    u *= float(width);
    v *= float(height);
  }
  int32_t x = int32_t(std::floor(u));
  int32_t y = int32_t(std::floor(v));
  x = std::clamp<int32_t>(x, 0, int32_t(width) - 1);
  y = std::clamp<int32_t>(y, 0, int32_t(height) - 1);

  const uint8_t* texel = rgba.data() + (size_t(y) * width + uint32_t(x)) * 4;
  uint8_t swizzled[4] = {
      ResolveSwizzledComponent(texel, SwizzleComponent(fetch.swizzle, 0)),
      ResolveSwizzledComponent(texel, SwizzleComponent(fetch.swizzle, 1)),
      ResolveSwizzledComponent(texel, SwizzleComponent(fetch.swizzle, 2)),
      ResolveSwizzledComponent(texel, SwizzleComponent(fetch.swizzle, 3)),
  };
  for (uint32_t i = 0; i < 4; ++i) {
    rgba_out[i] = float(swizzled[i]) * (1.0f / 255.0f);
  }

  static std::atomic<uint32_t> interpreter_tfetch_logs{0};
  uint32_t log_index = interpreter_tfetch_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (log_index <= 24 || (log_index & 0xFF) == 0) {
    uint32_t base_page = 0;
    texture_util::GetSubresourcesFromFetchConstant(fetch, nullptr, nullptr, nullptr, &base_page,
                                                   nullptr, nullptr, nullptr);
    std::fprintf(stderr,
                 "[metal] interpreter texture fetch#%u const=%u base=0x%08x size=%ux%u "
                 "coord=(%.4g,%.4g) unnorm=%u sample=%d,%d rgba=(%.4g,%.4g,%.4g,%.4g)\n",
                 log_index, instr.fetch_constant_index(), base_page << 12, width, height,
                 coordinates[0], coordinates[1], instr.unnormalized_coordinates() ? 1u : 0u, x, y,
                 rgba_out[0], rgba_out[1], rgba_out[2], rgba_out[3]);
    std::fflush(stderr);
  }
  return true;
}

bool MetalCommandProcessor::DecodeSwapTextureToBgra(uint32_t fallback_frontbuffer_ptr,
                                                    uint32_t fallback_width,
                                                    uint32_t fallback_height,
                                                    std::vector<uint8_t>& bgra_out,
                                                    uint32_t& width_out, uint32_t& height_out) {
  if (!memory_ || !register_file_) {
    return false;
  }

  xenos::xe_gpu_texture_fetch_t fetch = register_file_->GetTextureFetch(0);
  if (fetch.type != xenos::FetchConstantType::kTexture) {
    static std::atomic<uint32_t> invalid_fetch_logs{0};
    uint32_t invalid_fetch_index = invalid_fetch_logs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (invalid_fetch_index <= 8 || (invalid_fetch_index & 0x3F) == 0) {
      std::fprintf(stderr,
                   "[metal] swap fetch invalid#%u type=%u dwords=%08x %08x %08x %08x %08x %08x\n",
                   invalid_fetch_index, uint32_t(fetch.type), fetch.dword_0, fetch.dword_1,
                   fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5);
      std::fflush(stderr);
    }
    return false;
  }

  uint32_t width_minus_1 = 0;
  uint32_t height_minus_1 = 0;
  uint32_t depth_or_array_size_minus_1 = 0;
  uint32_t base_page = 0;
  uint32_t mip_page = 0;
  uint32_t mip_max_level = 0;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                 &depth_or_array_size_minus_1, &base_page,
                                                 &mip_page, nullptr, &mip_max_level);
  (void)mip_page;
  (void)mip_max_level;

  xenos::TextureFormat format = GetBaseFormat(fetch.format);
  uint32_t width = width_minus_1 + 1;
  uint32_t height = height_minus_1 + 1;
  uint32_t base_physical = base_page ? (base_page << 12) : fallback_frontbuffer_ptr;
  uint32_t decode_base_physical = base_physical;

  static std::atomic<uint32_t> fetch_log_count{0};
  uint32_t fetch_log_index = fetch_log_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (fetch_log_index <= 8 || (fetch_log_index & 0x3F) == 0) {
    std::fprintf(stderr,
                 "[metal] swap fetch#%u base=0x%08x fallback=0x%08x size=%ux%u vd=%ux%u "
                 "pitch=%u tiled=%u fmt=%u base_fmt=%u endian=%u swiz=0x%03x decode_base=0x%08x\n",
                 fetch_log_index, base_physical, fallback_frontbuffer_ptr, width, height,
                 fallback_width, fallback_height, uint32_t(fetch.pitch << 5), uint32_t(fetch.tiled),
                 uint32_t(fetch.format), uint32_t(format), uint32_t(fetch.endianness),
                 uint32_t(fetch.swizzle), decode_base_physical);
    std::fflush(stderr);
  }

  if (format != xenos::TextureFormat::k_8_8_8_8 ||
      fetch.dimension != xenos::DataDimension::k2DOrStacked || depth_or_array_size_minus_1 != 0 ||
      !width || !height) {
    return false;
  }

  if (fallback_width && fallback_height &&
      (width > fallback_width * 2 || height > fallback_height * 2)) {
    return false;
  }

  std::vector<uint8_t> rgba;
  if (!DecodeTextureFetchToRgba(fetch, fallback_frontbuffer_ptr, 0, rgba, width_out, height_out)) {
    return false;
  }

  bgra_out.resize(size_t(width) * height * 4);
  uint8_t min_rgb = 0xFF;
  uint8_t max_rgb = 0;
  size_t nonzero_rgb_count = 0;
  for (size_t pixel = 0, pixel_count = size_t(width) * height; pixel < pixel_count; ++pixel) {
    RgbaToDrawableBgra(rgba.data() + pixel * 4, fetch.swizzle, bgra_out.data() + pixel * 4);
    const uint8_t* bgra = bgra_out.data() + pixel * 4;
    for (uint32_t c = 0; c < 3; ++c) {
      min_rgb = std::min(min_rgb, bgra[c]);
      max_rgb = std::max(max_rgb, bgra[c]);
      nonzero_rgb_count += bgra[c] != 0;
    }
  }

  static std::atomic<uint32_t> decoded_stats_logs{0};
  uint32_t decoded_stats_index = decoded_stats_logs.fetch_add(1, std::memory_order_relaxed) + 1;
  if (decoded_stats_index <= 8 || (decoded_stats_index & 0x3F) == 0) {
    const uint8_t* first = bgra_out.data();
    std::fprintf(stderr,
                 "[metal] decoded swap#%u rgb_min=%u rgb_max=%u rgb_nonzero=%zu first_bgra=%02x "
                 "%02x %02x %02x\n",
                 decoded_stats_index, uint32_t(min_rgb), uint32_t(max_rgb), nonzero_rgb_count,
                 first[0], first[1], first[2], first[3]);
    std::fflush(stderr);
  }

  width_out = width;
  height_out = height;
  return true;
}

void MetalCommandProcessor::LogIncompleteOnce(const char* path) {
  if (logged_incomplete_) {
    return;
  }
  logged_incomplete_ = true;
  REXLOG_WARN("Metal backend reached {}; native renderer implementation is incomplete", path);
}

}  // namespace rex::graphics::metal
