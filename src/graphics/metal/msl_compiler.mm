#include <rex/graphics/metal/msl_compiler.h>

#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <system_error>
#include <unordered_map>
#include <unistd.h>
#include <utility>

namespace rex::graphics::metal {
namespace {

static_assert(MTLCompareFunctionNever == 0 && MTLCompareFunctionLess == 1 &&
              MTLCompareFunctionEqual == 2 && MTLCompareFunctionLessEqual == 3 &&
              MTLCompareFunctionGreater == 4 && MTLCompareFunctionNotEqual == 5 &&
              MTLCompareFunctionGreaterEqual == 6 && MTLCompareFunctionAlways == 7);
static_assert(MTLStencilOperationKeep == 0 && MTLStencilOperationZero == 1 &&
              MTLStencilOperationReplace == 2 && MTLStencilOperationIncrementClamp == 3 &&
              MTLStencilOperationDecrementClamp == 4 && MTLStencilOperationInvert == 5 &&
              MTLStencilOperationIncrementWrap == 6 && MTLStencilOperationDecrementWrap == 7);

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

MTLCullMode ToMetalCullMode(ProbeCullMode cull_mode) {
  switch (cull_mode) {
    case ProbeCullMode::kFront:
      return MTLCullModeFront;
    case ProbeCullMode::kBack:
      return MTLCullModeBack;
    default:
      return MTLCullModeNone;
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
      !std::isfinite(state->blend_alpha) ||
      uint32_t(state->cull_mode) > uint32_t(ProbeCullMode::kBack)) {
    return false;
  }
  return true;
}

bool IsProbeDepthStencilStateValid(const ProbeDepthStencilState* state) {
  if (!state) {
    return true;
  }
  auto face_valid = [](const ProbeStencilFaceState& face) {
    return face.compare_function <= 7 && face.stencil_failure_operation <= 7 &&
           face.depth_failure_operation <= 7 && face.depth_stencil_pass_operation <= 7;
  };
  return state->depth_compare_function <= 7 && face_valid(state->front) && face_valid(state->back);
}

uint64_t GetProbeDepthStencilKey(const ProbeDepthStencilState& state) {
  uint64_t key = uint64_t(state.depth_test_enabled);
  key |= uint64_t(state.depth_write_enabled) << 1;
  key |= uint64_t(state.depth_compare_function & 7) << 2;
  key |= uint64_t(state.stencil_test_enabled) << 5;
  uint32_t shift = 6;
  auto append_face = [&](const ProbeStencilFaceState& face) {
    key |= uint64_t(face.compare_function & 7) << shift;
    shift += 3;
    key |= uint64_t(face.stencil_failure_operation & 7) << shift;
    shift += 3;
    key |= uint64_t(face.depth_failure_operation & 7) << shift;
    shift += 3;
    key |= uint64_t(face.depth_stencil_pass_operation & 7) << shift;
    shift += 3;
    key |= uint64_t(face.read_mask) << shift;
    shift += 8;
    key |= uint64_t(face.write_mask) << shift;
    shift += 8;
  };
  append_face(state.front);
  append_face(state.back);
  return key;
}

id<MTLDepthStencilState> CreateProbeDepthStencilState(id<MTLDevice> device,
                                                      const ProbeDepthStencilState& state) {
  MTLDepthStencilDescriptor* descriptor = [[MTLDepthStencilDescriptor alloc] init];
  if (state.depth_test_enabled) {
    descriptor.depthCompareFunction = MTLCompareFunction(state.depth_compare_function);
    descriptor.depthWriteEnabled = state.depth_write_enabled;
  }
  if (state.stencil_test_enabled) {
    auto make_face = [](const ProbeStencilFaceState& face) {
      MTLStencilDescriptor* descriptor = [[MTLStencilDescriptor alloc] init];
      descriptor.stencilCompareFunction = MTLCompareFunction(face.compare_function);
      descriptor.stencilFailureOperation = MTLStencilOperation(face.stencil_failure_operation);
      descriptor.depthFailureOperation = MTLStencilOperation(face.depth_failure_operation);
      descriptor.depthStencilPassOperation = MTLStencilOperation(face.depth_stencil_pass_operation);
      descriptor.readMask = face.read_mask;
      descriptor.writeMask = face.write_mask;
      return descriptor;
    };
    MTLStencilDescriptor* front = make_face(state.front);
    MTLStencilDescriptor* back = make_face(state.back);
    descriptor.frontFaceStencil = front;
    descriptor.backFaceStencil = back;
    [front release];
    [back release];
  }
  id<MTLDepthStencilState> depth_stencil_state =
      [device newDepthStencilStateWithDescriptor:descriptor];
  [descriptor release];
  return depth_stencil_state;
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

namespace {

constexpr uintmax_t kMaximumPipelineArchiveBytes = uintmax_t(128) * 1024 * 1024;

uint64_t PipelineCacheNowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

NSString* PathToNSString(const std::filesystem::path& path) {
  const std::string path_utf8 = path.string();
  return [NSString stringWithUTF8String:path_utf8.c_str()];
}

std::string NSErrorDescription(NSError* error, const char* fallback) {
  if (!error) {
    return fallback;
  }
  NSString* description = [error localizedDescription];
  return description ? std::string([description UTF8String]) : fallback;
}

bool FsyncPath(const std::filesystem::path& path, std::string* error_out) {
  int descriptor = open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    if (error_out) {
      *error_out = "open for fsync failed: " + std::string(std::strerror(errno));
    }
    return false;
  }
  bool succeeded = fsync(descriptor) == 0;
  int saved_errno = errno;
  close(descriptor);
  if (!succeeded && error_out) {
    *error_out = "fsync failed: " + std::string(std::strerror(saved_errno));
  }
  return succeeded;
}

}  // namespace

uint64_t GetMetalDeviceCacheKey(void* metal_device) {
  if (!metal_device) {
    return 0;
  }
  id<MTLDevice> device = (id<MTLDevice>)metal_device;
  uint64_t registry_id = uint64_t([device registryID]);
  if (registry_id) {
    return registry_id;
  }

  // registryID is available on every supported macOS release, but retain a
  // deterministic fallback for unusual virtual/test devices.
  constexpr uint64_t kFnvOffset = UINT64_C(14695981039346656037);
  constexpr uint64_t kFnvPrime = UINT64_C(1099511628211);
  uint64_t hash = kFnvOffset;
  const char* name = [[device name] UTF8String];
  if (name) {
    for (const unsigned char* character = reinterpret_cast<const unsigned char*>(name); *character;
         ++character) {
      hash ^= *character;
      hash *= kFnvPrime;
    }
  }
  return hash;
}

void* CreateMetalPipelineBinaryArchive(void* metal_device,
                                       const std::filesystem::path& archive_path,
                                       bool* loaded_existing_out,
                                       std::string* warning_or_error_out) {
  if (loaded_existing_out) {
    *loaded_existing_out = false;
  }
  if (warning_or_error_out) {
    warning_or_error_out->clear();
  }
  if (!metal_device || archive_path.empty()) {
    if (warning_or_error_out) {
      *warning_or_error_out = "missing Metal device or archive path";
    }
    return nullptr;
  }

  std::error_code filesystem_error;
  const bool archive_exists = std::filesystem::exists(archive_path, filesystem_error);
  bool load_existing = false;
  if (filesystem_error) {
    if (warning_or_error_out) {
      *warning_or_error_out = "could not inspect existing Metal pipeline archive";
    }
  } else if (archive_exists) {
    load_existing = std::filesystem::is_regular_file(archive_path, filesystem_error);
    if (filesystem_error || !load_existing) {
      if (warning_or_error_out) {
        *warning_or_error_out = filesystem_error
                                    ? "could not inspect existing Metal pipeline archive"
                                    : "existing Metal pipeline archive is not a regular file; "
                                      "creating a fresh archive";
      }
      load_existing = false;
    }
  }
  if (load_existing) {
    uintmax_t archive_size = std::filesystem::file_size(archive_path, filesystem_error);
    if (filesystem_error || archive_size == 0 || archive_size > kMaximumPipelineArchiveBytes) {
      load_existing = false;
      if (warning_or_error_out) {
        *warning_or_error_out =
            filesystem_error ? "could not inspect existing Metal pipeline archive"
            : archive_size == 0
                ? "existing Metal pipeline archive is empty; creating a fresh archive"
                : "existing Metal pipeline archive exceeds the 128 MiB safety limit; "
                  "creating a fresh archive";
      }
    }
  }

  MTLBinaryArchiveDescriptor* descriptor = [[MTLBinaryArchiveDescriptor alloc] init];
  id<MTLBinaryArchive> archive = nil;
  if (load_existing) {
    NSString* archive_path_string = PathToNSString(archive_path);
    if (archive_path_string) {
      descriptor.url = [NSURL fileURLWithPath:archive_path_string];
      NSError* load_error = nil;
      archive = [(id<MTLDevice>)metal_device newBinaryArchiveWithDescriptor:descriptor
                                                                      error:&load_error];
      if (!archive && warning_or_error_out) {
        *warning_or_error_out =
            "existing Metal pipeline archive was rejected; creating a fresh archive: " +
            NSErrorDescription(load_error, "unknown Metal archive load error");
      }
    }
  }

  if (!archive) {
    descriptor.url = nil;
    NSError* create_error = nil;
    archive = [(id<MTLDevice>)metal_device newBinaryArchiveWithDescriptor:descriptor
                                                                    error:&create_error];
    if (!archive && warning_or_error_out) {
      *warning_or_error_out =
          "could not create Metal pipeline archive: " +
          NSErrorDescription(create_error, "unknown Metal archive creation error");
    }
  } else if (loaded_existing_out) {
    *loaded_existing_out = true;
  }
  [descriptor release];

  if (archive) {
    [archive setLabel:@"ReXGlue title pipeline cache"];
  }
  return archive;
}

bool SerializeMetalPipelineBinaryArchive(void* binary_archive,
                                         const std::filesystem::path& archive_path,
                                         uint64_t* serialized_size_out, std::string* error_out) {
  if (serialized_size_out) {
    *serialized_size_out = 0;
  }
  if (error_out) {
    error_out->clear();
  }
  if (!binary_archive || archive_path.empty()) {
    if (error_out) {
      *error_out = "missing Metal pipeline archive or destination path";
    }
    return false;
  }

  std::error_code filesystem_error;
  std::filesystem::create_directories(archive_path.parent_path(), filesystem_error);
  if (filesystem_error) {
    if (error_out) {
      *error_out = "could not create Metal pipeline cache directory: " + filesystem_error.message();
    }
    return false;
  }

  static std::atomic<uint64_t> temporary_file_sequence{0};
  std::filesystem::path temporary_path = archive_path;
  temporary_path += ".tmp-" + std::to_string(uint64_t(getpid())) + "-" +
                    std::to_string(temporary_file_sequence.fetch_add(1));
  std::filesystem::remove(temporary_path, filesystem_error);
  filesystem_error.clear();

  // This path also runs on the background cache worker. Give temporary
  // Foundation and Metal objects an explicit lifetime on non-AppKit threads.
  bool serialized = false;
  std::string objective_c_error;
  @autoreleasepool {
    NSString* temporary_path_string = PathToNSString(temporary_path);
    if (!temporary_path_string) {
      objective_c_error = "Metal pipeline cache path is not valid UTF-8";
    } else {
      NSError* serialize_error = nil;
      serialized = [(id<MTLBinaryArchive>)binary_archive
          serializeToURL:[NSURL fileURLWithPath:temporary_path_string]
                   error:&serialize_error];
      if (!serialized) {
        objective_c_error =
            NSErrorDescription(serialize_error, "unknown Metal archive serialization error");
      }
    }
  }
  if (!serialized) {
    if (error_out) {
      *error_out = std::move(objective_c_error);
    }
    std::filesystem::remove(temporary_path, filesystem_error);
    return false;
  }

  uintmax_t temporary_size = std::filesystem::file_size(temporary_path, filesystem_error);
  if (filesystem_error || temporary_size == 0 || temporary_size > kMaximumPipelineArchiveBytes) {
    if (error_out) {
      *error_out = filesystem_error ? "could not inspect serialized Metal pipeline archive"
                   : temporary_size == 0
                       ? "Metal serialized an empty pipeline archive"
                       : "serialized Metal pipeline archive exceeds the 128 MiB safety limit";
    }
    std::filesystem::remove(temporary_path, filesystem_error);
    return false;
  }

  if (!FsyncPath(temporary_path, error_out)) {
    std::filesystem::remove(temporary_path, filesystem_error);
    return false;
  }

  // POSIX rename within one directory atomically replaces the old archive, so
  // interruption can leave either the previous complete file or the new one,
  // never a partially serialized destination.
  if (rename(temporary_path.c_str(), archive_path.c_str()) != 0) {
    if (error_out) {
      *error_out =
          "atomic Metal pipeline archive replacement failed: " + std::string(std::strerror(errno));
    }
    std::filesystem::remove(temporary_path, filesystem_error);
    return false;
  }

  // Best effort: the file itself is durable already. Syncing the directory
  // also persists the rename across a sudden power loss on filesystems that
  // require it.
  int directory_descriptor = open(archive_path.parent_path().c_str(), O_RDONLY);
  if (directory_descriptor >= 0) {
    fsync(directory_descriptor);
    close(directory_descriptor);
  }
  if (serialized_size_out) {
    *serialized_size_out = uint64_t(temporary_size);
  }
  return true;
}

void ReleaseMetalPipelineBinaryArchive(void* binary_archive) {
  if (binary_archive) {
    [(id<MTLBinaryArchive>)binary_archive release];
  }
}

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
                                const ProbeColorTargetState* color_target_state,
                                void* binary_archive,
                                RenderPipelineCacheTelemetry* cache_telemetry_out,
                                uint32_t sample_count) {
  RenderPipelineCacheTelemetry cache_telemetry;
  if (cache_telemetry_out) {
    *cache_telemetry_out = {};
  }
  if (!metal_device || !vertex_library || !fragment_library ||
      (sample_count != 1 && sample_count != 2 && sample_count != 4) ||
      ![(id<MTLDevice>)metal_device supportsTextureSampleCount:sample_count]) {
    if (error_out) {
      *error_out = !metal_device || !vertex_library || !fragment_library
                       ? "missing Metal device or shader library"
                       : "unsupported Metal render-pipeline sample count";
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
  descriptor.rasterSampleCount = sample_count;
  descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
  descriptor.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
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
  id<MTLRenderPipelineState> pipeline_state = nil;
  if (binary_archive) {
    cache_telemetry.archive_enabled = true;
    descriptor.binaryArchives = @[ (id<MTLBinaryArchive>)binary_archive ];

    uint64_t lookup_start_ns = PipelineCacheNowNs();
    pipeline_state = [(id<MTLDevice>)metal_device
        newRenderPipelineStateWithDescriptor:descriptor
                                     options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                  reflection:nil
                                       error:&error];
    cache_telemetry.archive_lookup_ns = PipelineCacheNowNs() - lookup_start_ns;
    if (pipeline_state) {
      cache_telemetry.archive_hit = true;
      cache_telemetry.pipeline_build_ns = cache_telemetry.archive_lookup_ns;
    } else {
      cache_telemetry.archive_miss = true;
      NSError* archive_add_error = nil;
      uint64_t archive_add_start_ns = PipelineCacheNowNs();
      bool archive_add_succeeded = [(id<MTLBinaryArchive>)binary_archive
          addRenderPipelineFunctionsWithDescriptor:descriptor
                                             error:&archive_add_error];
      cache_telemetry.archive_add_ns = PipelineCacheNowNs() - archive_add_start_ns;
      if (!archive_add_succeeded) {
        cache_telemetry.archive_update_failed = true;
        cache_telemetry.archive_error =
            NSErrorDescription(archive_add_error, "unknown Metal archive update error");
      }

      // A miss or an outdated archive must never affect correctness. Compile
      // through Metal's normal path, still attaching the archive so functions
      // successfully added above can be reused immediately.
      error = nil;
      uint64_t build_start_ns = PipelineCacheNowNs();
      pipeline_state =
          [(id<MTLDevice>)metal_device newRenderPipelineStateWithDescriptor:descriptor
                                                                    options:MTLPipelineOptionNone
                                                                 reflection:nil
                                                                      error:&error];
      cache_telemetry.pipeline_build_ns = PipelineCacheNowNs() - build_start_ns;
      cache_telemetry.archive_updated = archive_add_succeeded && pipeline_state;
    }
  } else {
    uint64_t build_start_ns = PipelineCacheNowNs();
    pipeline_state = [(id<MTLDevice>)metal_device newRenderPipelineStateWithDescriptor:descriptor
                                                                                 error:&error];
    cache_telemetry.pipeline_build_ns = PipelineCacheNowNs() - build_start_ns;
  }
  [descriptor release];
  [vertex_function release];
  [fragment_function release];

  if (cache_telemetry_out) {
    *cache_telemetry_out = std::move(cache_telemetry);
  }

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
  uint32_t auxiliary_submission_count = 0;
  uint32_t upload_arena_index = kInvalidProbeUploadArena;
  void (*async_failure_callback)(void* context, uint32_t start, uint32_t length) = nullptr;
  void* async_failure_callback_context = nullptr;
  uint32_t async_failure_start = 0;
  uint32_t async_failure_length = 0;
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

struct ProbeDepthStencilTarget {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> command_queue = nil;
  id<MTLTexture> texture = nil;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t sample_count = 1;
  bool initialized = false;
  // An open render encoder retains exclusive logical ownership. Before a
  // different color context uses this target, the previous owner's command
  // buffer is finalized so commits to the shared queue preserve draw order.
  PipelineProbeContext* open_owner = nullptr;
  std::vector<PipelineProbeContext*> attached_contexts;
};

struct PipelineProbeContext {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> command_queue = nil;
  id<MTLTexture> render_texture = nil;
  // For multisampled contexts, drawing targets this texture. Consumers resolve
  // it into render_texture only when a read, guest copy, or presentation needs
  // single-sample color data.
  id<MTLTexture> multisample_render_texture = nil;
  ProbeDepthStencilTarget* depth_stencil_target = nullptr;
  id<MTLBuffer> private_readback_buffer = nil;
  size_t private_readback_capacity = 0;
  id<MTLTexture> dummy_texture = nil;
  id<MTLSamplerState> dummy_sampler = nil;
  std::unordered_map<uint64_t, id<MTLSamplerState>> sampler_cache;
  std::unordered_map<uint64_t, id<MTLDepthStencilState>> depth_stencil_state_cache;
  id<MTLRenderPipelineState> clear_pipeline_state = nil;
  id<MTLRenderPipelineState> depth_clear_pipeline_state = nil;
  id<MTLComputePipelineState> tiled_resolve_pipeline_state = nil;
  MTLStorageMode storage_mode = MTLStorageModeShared;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t sample_count = 1;
  bool initialized = false;
  bool color_resolve_dirty = false;
  uint64_t multisample_resolve_count = 0;
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

ProbeDepthStencilTarget* CreateProbeDepthStencilTarget(PipelineProbeContext* context) {
  if (!context || !context->device || !context->command_queue) {
    return nullptr;
  }
  auto* target = new ProbeDepthStencilTarget();
  target->device = context->device;
  target->command_queue = context->command_queue;
  target->attached_contexts.push_back(context);
  context->depth_stencil_target = target;
  return target;
}

void DetachProbeDepthStencilTarget(PipelineProbeContext* context) {
  if (!context || !context->depth_stencil_target) {
    return;
  }
  ProbeDepthStencilTarget* target = context->depth_stencil_target;
  if (target->open_owner == context) {
    target->open_owner = nullptr;
  }
  auto attached_it =
      std::find(target->attached_contexts.begin(), target->attached_contexts.end(), context);
  if (attached_it != target->attached_contexts.end()) {
    target->attached_contexts.erase(attached_it);
  }
  context->depth_stencil_target = nullptr;
  if (!target->attached_contexts.empty()) {
    return;
  }
  if (target->texture) {
    [target->texture release];
  }
  delete target;
}

void AttachProbeDepthStencilTarget(PipelineProbeContext* context, ProbeDepthStencilTarget* target) {
  if (!context || !target || context->depth_stencil_target == target) {
    return;
  }
  DetachProbeDepthStencilTarget(context);
  target->attached_contexts.push_back(context);
  context->depth_stencil_target = target;
}

void ReleaseOpenProbeDepthStencilOwnership(PipelineProbeContext* context) {
  if (context && context->depth_stencil_target &&
      context->depth_stencil_target->open_owner == context) {
    context->depth_stencil_target->open_owner = nullptr;
  }
}

void InvalidateProbeContextTargets(PipelineProbeContext* context) {
  if (!context) {
    return;
  }
  context->initialized = false;
  context->color_resolve_dirty = false;
  if (context->depth_stencil_target) {
    context->depth_stencil_target->initialized = false;
  }
}

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

void ConfigureProbeDepthStencilPass(MTLRenderPassDescriptor* pass,
                                    id<MTLTexture> depth_stencil_texture, MTLLoadAction load_action,
                                    double clear_depth = 1.0, uint32_t clear_stencil = 0) {
  pass.depthAttachment.texture = depth_stencil_texture;
  pass.depthAttachment.loadAction = load_action;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.depthAttachment.clearDepth = clear_depth;
  pass.stencilAttachment.texture = depth_stencil_texture;
  pass.stencilAttachment.loadAction = load_action;
  pass.stencilAttachment.storeAction = MTLStoreActionStore;
  pass.stencilAttachment.clearStencil = clear_stencil;
}

void ConfigureProbeColorPass(MTLRenderPassDescriptor* pass, PipelineProbeContext* context,
                             MTLLoadAction load_action, MTLClearColor clear_color) {
  MTLRenderPassColorAttachmentDescriptor* color = pass.colorAttachments[0];
  color.texture =
      context->sample_count > 1 ? context->multisample_render_texture : context->render_texture;
  color.loadAction = load_action;
  color.clearColor = clear_color;
  if (context->sample_count > 1) {
    color.resolveTexture = nil;
    color.storeAction = MTLStoreActionStore;
  } else {
    color.resolveTexture = nil;
    color.storeAction = MTLStoreActionStore;
  }
}

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

void NotifyProbeCommandFailure(const CommittedProbeCommandBuffer& committed) {
  if (committed.async_failure_callback && committed.async_failure_length) {
    committed.async_failure_callback(committed.async_failure_callback_context,
                                     committed.async_failure_start, committed.async_failure_length);
  }
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
  ReleaseOpenProbeDepthStencilOwnership(context);
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

bool GetCachedProbeDepthStencilState(PipelineProbeContext* context,
                                     const ProbeDepthStencilState* state,
                                     id<MTLDepthStencilState>& state_out, std::string* error_out) {
  state_out = nil;
  ProbeDepthStencilState disabled_state;
  const ProbeDepthStencilState& effective_state = state ? *state : disabled_state;
  uint64_t key = GetProbeDepthStencilKey(effective_state);
  auto existing = context->depth_stencil_state_cache.find(key);
  if (existing != context->depth_stencil_state_cache.end()) {
    state_out = existing->second;
    return true;
  }
  id<MTLDepthStencilState> created = CreateProbeDepthStencilState(context->device, effective_state);
  if (!created) {
    if (error_out) {
      *error_out = "failed to create persistent probe depth/stencil state";
    }
    return false;
  }
  context->depth_stencil_state_cache.emplace(key, created);
  state_out = created;
  return true;
}

void ResetOpenProbeBindingTracking(PipelineProbeContext* context) {
  context->tracked_vertex_buffer_mask = 0;
  context->tracked_fragment_buffer_mask = 0;
  context->tracked_vertex_texture_count = 0;
  context->tracked_fragment_texture_count = 0;
  context->tracked_vertex_sampler_count = 0;
  context->tracked_fragment_sampler_count = 0;
}

uint32_t GetPendingProbeSubmissionCount(const PipelineProbeContext* context) {
  if (!context) {
    return 0;
  }
  uint64_t submission_count = context->open_draw_submission_count;
  for (const CommittedProbeCommandBuffer& committed : context->committed_command_buffers) {
    submission_count +=
        uint64_t(committed.draw_submission_count) + committed.auxiliary_submission_count;
  }
  return uint32_t(std::min<uint64_t>(submission_count, UINT32_MAX));
}

uint32_t GetCommittedProbeDrawCommandBufferCount(const PipelineProbeContext* context) {
  if (!context) {
    return 0;
  }
  uint32_t command_buffer_count = 0;
  for (const CommittedProbeCommandBuffer& committed : context->committed_command_buffers) {
    command_buffer_count += committed.draw_submission_count != 0;
  }
  return command_buffer_count;
}

bool FinalizeOpenPipelineProbeCommandBuffer(PipelineProbeContext* context, std::string* error_out) {
  if (!context->open_command_buffer && !context->open_render_encoder) {
    ReleaseOpenProbeDepthStencilOwnership(context);
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
    ReleaseOpenProbeDepthStencilOwnership(context);
    ResetOpenProbeBindingTracking(context);
    InvalidateProbeContextTargets(context);
    if (error_out) {
      *error_out = "inconsistent open probe command buffer state";
    }
    return false;
  }

  id<MTLRenderCommandEncoder> encoder = context->open_render_encoder;
  [encoder endEncoding];
  context->open_render_encoder = nil;
  ReleaseOpenProbeDepthStencilOwnership(context);
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

bool FinalizeProbeColorForConsumer(PipelineProbeContext* context, std::string* error_out) {
  if (!context) {
    if (error_out) {
      *error_out = "missing probe context";
    }
    return false;
  }
  if (!FinalizeOpenPipelineProbeCommandBuffer(context, error_out)) {
    return false;
  }
  if (context->sample_count == 1 || !context->color_resolve_dirty) {
    return true;
  }
  if (!context->initialized || !context->multisample_render_texture || !context->render_texture) {
    InvalidateProbeContextTargets(context);
    if (error_out) {
      *error_out = "multisample probe color target is unavailable for resolve";
    }
    return false;
  }

  id<MTLCommandBuffer> command_buffer = [context->command_queue commandBuffer];
  if (!command_buffer) {
    if (error_out) {
      *error_out = "failed to create multisample color resolve command buffer";
    }
    return false;
  }
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  MTLRenderPassColorAttachmentDescriptor* color = pass.colorAttachments[0];
  color.texture = context->multisample_render_texture;
  color.loadAction = MTLLoadActionLoad;
  color.resolveTexture = context->render_texture;
  // Preserve the multisample attachment because later render passes continue
  // it with Load until another consumer needs an updated single-sample image.
  color.storeAction = MTLStoreActionStoreAndMultisampleResolve;
  id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
  if (!encoder) {
    if (error_out) {
      *error_out = "failed to create multisample color resolve encoder";
    }
    return false;
  }
  [encoder endEncoding];

  CommittedProbeCommandBuffer committed;
  committed.command_buffer = [command_buffer retain];
  committed.auxiliary_submission_count = 1;
  context->committed_command_buffers.push_back(committed);
  [command_buffer commit];
  context->color_resolve_dirty = false;
  ++context->multisample_resolve_count;
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
    if ([command_buffer status] != MTLCommandBufferStatusCompleted) {
      NotifyProbeCommandFailure(committed);
    }
    [command_buffer release];
    ReleaseProbeUploadArena(context, committed.upload_arena_index);
  }
  context->committed_command_buffers.clear();
  if (first_error.empty()) {
    return true;
  }
  InvalidateProbeContextTargets(context);
  if (error_out) {
    *error_out = std::move(first_error);
  }
  return false;
}

bool ConsumeOldestPipelineProbeCommand(PipelineProbeContext* context, std::string* error_out) {
  if (!context || context->committed_command_buffers.empty()) {
    return true;
  }

  CommittedProbeCommandBuffer committed = context->committed_command_buffers.front();
  context->committed_command_buffers.erase(context->committed_command_buffers.begin());
  id<MTLCommandBuffer> command_buffer = committed.command_buffer;
  bool succeeded = [command_buffer status] == MTLCommandBufferStatusCompleted;
  if (!succeeded) {
    NSError* command_error = [command_buffer error];
    const char* description =
        command_error ? [[command_error localizedDescription] UTF8String] : nullptr;
    InvalidateProbeContextTargets(context);
    if (error_out) {
      *error_out = description ? description : "asynchronous probe command buffer failed";
    }
    NotifyProbeCommandFailure(committed);
  }
  [command_buffer release];
  ReleaseProbeUploadArena(context, committed.upload_arena_index);
  return succeeded;
}

bool WaitOldestPipelineProbeCommand(PipelineProbeContext* context, std::string* error_out) {
  if (!context) {
    if (error_out) {
      *error_out = "missing probe context";
    }
    return false;
  }
  if ((context->open_command_buffer || context->open_render_encoder) &&
      !FinalizeOpenPipelineProbeCommandBuffer(context, error_out)) {
    return false;
  }
  if (context->committed_command_buffers.empty()) {
    return true;
  }
  [context->committed_command_buffers.front().command_buffer waitUntilCompleted];
  return ConsumeOldestPipelineProbeCommand(context, error_out);
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
  uint32_t pending_submission_count = GetPendingProbeSubmissionCount(context);
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

bool PrepareProbeDepthStencilSubmission(PipelineProbeContext* context, bool acquire_open_ownership,
                                        std::string* error_out) {
  if (!context || !context->depth_stencil_target) {
    if (error_out) {
      *error_out = "missing persistent probe depth/stencil target";
    }
    return false;
  }
  ProbeDepthStencilTarget* target = context->depth_stencil_target;
  PipelineProbeContext* previous_owner = target->open_owner;
  if (previous_owner && previous_owner != context) {
    std::string finalize_error;
    if (!FinalizeOpenPipelineProbeCommandBuffer(previous_owner, &finalize_error)) {
      if (error_out) {
        *error_out =
            finalize_error.empty()
                ? "failed to finalize the previous shared depth/stencil owner"
                : "failed to finalize the previous shared depth/stencil owner: " + finalize_error;
      }
      return false;
    }
  }
  if (target->open_owner && target->open_owner != context) {
    if (error_out) {
      *error_out = "shared depth/stencil target retained an inconsistent open owner";
    }
    return false;
  }
  target->open_owner = acquire_open_ownership ? context : nullptr;
  return true;
}

bool EnsureProbeDepthStencilTexture(PipelineProbeContext* context, uint32_t width, uint32_t height,
                                    std::string* error_out) {
  if (!context || !context->depth_stencil_target || !width || !height) {
    if (error_out) {
      *error_out = "missing persistent probe depth/stencil target or size";
    }
    return false;
  }
  ProbeDepthStencilTarget* target = context->depth_stencil_target;
  if (target->device != context->device || target->command_queue != context->command_queue) {
    if (error_out) {
      *error_out = "persistent probe depth/stencil target belongs to a different device or queue";
    }
    return false;
  }
  if (target->texture && target->width == width && target->height == height) {
    return true;
  }

  // A resize replaces the texture shared by every attached color context.
  // Drain all of them first so no encoder can retain the old texture while
  // another context starts using its replacement.
  for (PipelineProbeContext* attached_context : target->attached_contexts) {
    if (!WaitPendingPipelineProbeCommands(attached_context, error_out, nullptr)) {
      return false;
    }
  }
  target->open_owner = nullptr;
  if (target->texture) {
    [target->texture release];
    target->texture = nil;
  }
  MTLTextureDescriptor* descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  if (target->sample_count > 1) {
    descriptor.textureType = MTLTextureType2DMultisample;
    descriptor.sampleCount = target->sample_count;
  }
  descriptor.usage = MTLTextureUsageRenderTarget;
  descriptor.storageMode = MTLStorageModePrivate;
  target->texture = [target->device newTextureWithDescriptor:descriptor];
  if (!target->texture) {
    target->width = 0;
    target->height = 0;
    target->initialized = false;
    if (error_out) {
      *error_out = "failed to create persistent probe depth/stencil texture";
    }
    return false;
  }
  target->width = width;
  target->height = height;
  target->initialized = false;
  return true;
}

bool EnsureOpenPipelineProbeEncoder(PipelineProbeContext* context, std::string* error_out) {
  if (context->open_command_buffer && context->open_render_encoder) {
    if (context->depth_stencil_target && context->depth_stencil_target->open_owner == context) {
      return true;
    }
    if (error_out) {
      *error_out = "persistent probe encoder lost shared depth/stencil ownership";
    }
    return false;
  }
  if (context->open_command_buffer || context->open_render_encoder) {
    FinalizeOpenPipelineProbeCommandBuffer(context, error_out);
    return false;
  }

  uint32_t upload_arena_index = AcquireProbeUploadArena(context);
  while (upload_arena_index == kInvalidProbeUploadArena &&
         !context->committed_command_buffers.empty()) {
    // Keep three command buffers in flight while recycling only the oldest
    // draw arena. Auxiliary resolve buffers don't own arenas, so consume
    // completed queue entries until an actual draw arena becomes reusable.
    if (!WaitOldestPipelineProbeCommand(context, error_out)) {
      return false;
    }
    upload_arena_index = AcquireProbeUploadArena(context);
  }
  if (upload_arena_index == kInvalidProbeUploadArena) {
    ReleaseOpenProbeDepthStencilOwnership(context);
    if (error_out) {
      *error_out = "no reusable persistent probe upload arena is available";
    }
    return false;
  }

  if (!PrepareProbeDepthStencilSubmission(context, true, error_out)) {
    ReleaseProbeUploadArena(context, upload_arena_index);
    return false;
  }

  id<MTLCommandBuffer> command_buffer = [context->command_queue commandBuffer];
  if (!command_buffer) {
    ReleaseProbeUploadArena(context, upload_arena_index);
    ReleaseOpenProbeDepthStencilOwnership(context);
    if (error_out) {
      *error_out = "failed to create persistent probe command buffer";
    }
    return false;
  }
  MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
  ConfigureProbeColorPass(pass, context,
                          context->initialized ? MTLLoadActionLoad : MTLLoadActionClear,
                          MTLClearColorMake(0.0, 0.0, 0.0, 1.0));
  ProbeDepthStencilTarget* depth_stencil_target = context->depth_stencil_target;
  ConfigureProbeDepthStencilPass(
      pass, depth_stencil_target->texture,
      depth_stencil_target->initialized ? MTLLoadActionLoad : MTLLoadActionClear);
  id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
  if (!encoder) {
    ReleaseProbeUploadArena(context, upload_arena_index);
    ReleaseOpenProbeDepthStencilOwnership(context);
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
  if (!EnsureProbeDepthStencilTexture(context, width, height, error_out)) {
    return false;
  }
  if (context->render_texture &&
      (context->sample_count == 1 || context->multisample_render_texture) &&
      context->width == width && context->height == height) {
    return true;
  }
  if (!WaitPendingPipelineProbeCommands(context, error_out, nullptr)) {
    return false;
  }
  if (context->render_texture) {
    [context->render_texture release];
    context->render_texture = nil;
  }
  if (context->multisample_render_texture) {
    [context->multisample_render_texture release];
    context->multisample_render_texture = nil;
  }
  MTLTextureDescriptor* texture_descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  texture_descriptor.usage =
      MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  texture_descriptor.storageMode = context->storage_mode;
  context->render_texture = [context->device newTextureWithDescriptor:texture_descriptor];
  if (context->render_texture && context->sample_count > 1) {
    MTLTextureDescriptor* multisample_descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    multisample_descriptor.textureType = MTLTextureType2DMultisample;
    multisample_descriptor.sampleCount = context->sample_count;
    multisample_descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    multisample_descriptor.storageMode = MTLStorageModePrivate;
    context->multisample_render_texture =
        [context->device newTextureWithDescriptor:multisample_descriptor];
  }
  if (!context->render_texture ||
      (context->sample_count > 1 && !context->multisample_render_texture)) {
    if (context->render_texture) {
      [context->render_texture release];
      context->render_texture = nil;
    }
    if (context->multisample_render_texture) {
      [context->multisample_render_texture release];
      context->multisample_render_texture = nil;
    }
    context->width = 0;
    context->height = 0;
    context->initialized = false;
    if (error_out) {
      *error_out = "failed to create persistent probe color texture";
    }
    return false;
  }
  context->width = width;
  context->height = height;
  context->initialized = false;
  context->color_resolve_dirty = false;
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
  descriptor.rasterSampleCount = context->sample_count;
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
  descriptor.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
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

bool EnsureDepthClearPipelineState(PipelineProbeContext* context, std::string* error_out) {
  if (!context || !context->device) {
    if (error_out) {
      *error_out = "missing probe context or Metal device";
    }
    return false;
  }
  if (context->depth_clear_pipeline_state) {
    return true;
  }
  static constexpr char kDepthClearMsl[] = R"(
#include <metal_stdlib>
using namespace metal;

struct DepthClearConstants {
  float depth;
};

vertex float4 rex_depth_clear_vertex(uint vertex_id [[vertex_id]],
                                     constant DepthClearConstants& constants [[buffer(0)]]) {
  constexpr float2 positions[3] = {
    float2(-1.0, -1.0),
    float2( 3.0, -1.0),
    float2(-1.0,  3.0),
  };
  return float4(positions[vertex_id], constants.depth, 1.0);
}

fragment float4 rex_depth_clear_fragment() {
  return float4(0.0);
}
)";
  NSError* error = nil;
  id<MTLLibrary> library =
      [context->device newLibraryWithSource:[NSString stringWithUTF8String:kDepthClearMsl]
                                    options:nil
                                      error:&error];
  if (!library) {
    if (error_out) {
      *error_out = error ? [[error localizedDescription] UTF8String] : "depth clear library failed";
    }
    return false;
  }
  id<MTLFunction> vertex_function = [library newFunctionWithName:@"rex_depth_clear_vertex"];
  id<MTLFunction> fragment_function = [library newFunctionWithName:@"rex_depth_clear_fragment"];
  MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = vertex_function;
  descriptor.fragmentFunction = fragment_function;
  descriptor.rasterSampleCount = context->sample_count;
  descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
  descriptor.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
  descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
  descriptor.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
  if (vertex_function && fragment_function) {
    context->depth_clear_pipeline_state =
        [context->device newRenderPipelineStateWithDescriptor:descriptor error:&error];
  }
  [descriptor release];
  if (vertex_function) {
    [vertex_function release];
  }
  if (fragment_function) {
    [fragment_function release];
  }
  [library release];
  if (!context->depth_clear_pipeline_state) {
    if (error_out) {
      *error_out =
          error ? [[error localizedDescription] UTF8String] : "depth clear pipeline failed";
    }
    return false;
  }
  return true;
}

}  // namespace

namespace {

void* CreatePersistentRenderContext(void* metal_device, void* metal_command_queue,
                                    MTLStorageMode storage_mode, const char* label,
                                    std::string* error_out) {
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
  if (metal_command_queue) {
    id<MTLCommandQueue> command_queue = (id<MTLCommandQueue>)metal_command_queue;
    if ([command_queue device] != context->device) {
      if (error_out) {
        *error_out = std::string("persistent ") + label +
                     " command queue belongs to a different Metal device";
      }
      [context->device release];
      delete context;
      return nullptr;
    }
    context->command_queue = [command_queue retain];
  } else {
    context->command_queue = [context->device newCommandQueue];
  }
  if (!context->command_queue) {
    if (error_out) {
      *error_out = std::string("failed to create persistent ") + label + " command queue";
    }
    [context->device release];
    delete context;
    return nullptr;
  }
  if (!CreateProbeDepthStencilTarget(context)) {
    if (error_out) {
      *error_out = std::string("failed to create persistent ") + label + " depth/stencil target";
    }
    [context->command_queue release];
    [context->device release];
    delete context;
    return nullptr;
  }
  return context;
}

}  // namespace

void* CreatePipelineProbeContext(void* metal_device, std::string* error_out) {
  return CreatePipelineProbeContext(metal_device, nullptr, error_out);
}

void* CreatePipelineProbeContext(void* metal_device, void* metal_command_queue,
                                 std::string* error_out) {
  return CreatePersistentRenderContext(metal_device, metal_command_queue, MTLStorageModeShared,
                                       "probe", error_out);
}

void* CreateHostRenderTargetContext(void* metal_device, std::string* error_out) {
  return CreateHostRenderTargetContext(metal_device, nullptr, error_out);
}

void* CreateHostRenderTargetContext(void* metal_device, void* metal_command_queue,
                                    std::string* error_out) {
  return CreatePersistentRenderContext(metal_device, metal_command_queue, MTLStorageModePrivate,
                                       "host render target", error_out);
}

bool SharePipelineProbeDepthStencilTarget(void* opaque_destination_context,
                                          void* opaque_source_context, std::string* error_out) {
  auto* destination_context = static_cast<PipelineProbeContext*>(opaque_destination_context);
  auto* source_context = static_cast<PipelineProbeContext*>(opaque_source_context);
  if (!destination_context || !source_context || !source_context->depth_stencil_target) {
    if (error_out) {
      *error_out = "missing source or destination persistent probe context";
    }
    return false;
  }
  if (destination_context == source_context ||
      destination_context->depth_stencil_target == source_context->depth_stencil_target) {
    return true;
  }
  if (destination_context->device != source_context->device ||
      destination_context->command_queue != source_context->command_queue) {
    if (error_out) {
      *error_out = "shared depth/stencil contexts must use the same Metal device and command queue";
    }
    return false;
  }
  if (destination_context->sample_count != source_context->sample_count ||
      source_context->depth_stencil_target->sample_count != source_context->sample_count) {
    if (error_out) {
      *error_out = "shared depth/stencil contexts must use the same sample count";
    }
    return false;
  }
  ProbeDepthStencilTarget* source_target = source_context->depth_stencil_target;
  ProbeDepthStencilTarget* destination_target = destination_context->depth_stencil_target;
  if (source_target->texture && ((destination_context->render_texture &&
                                  (destination_context->width != source_target->width ||
                                   destination_context->height != source_target->height)) ||
                                 (destination_target && destination_target->texture &&
                                  (destination_target->width != source_target->width ||
                                   destination_target->height != source_target->height)))) {
    if (error_out) {
      *error_out = "shared depth/stencil contexts have incompatible target dimensions";
    }
    return false;
  }
  if (!WaitPendingPipelineProbeCommands(destination_context, error_out, nullptr)) {
    return false;
  }
  AttachProbeDepthStencilTarget(destination_context, source_target);
  return true;
}

bool SetPipelineProbeContextSampleCount(void* opaque_context, uint32_t sample_count,
                                        std::string* error_out) {
  @autoreleasepool {
    auto* context = static_cast<PipelineProbeContext*>(opaque_context);
    if (!context || !context->device || !context->depth_stencil_target) {
      if (error_out) {
        *error_out = "missing persistent probe context";
      }
      return false;
    }
    if ((sample_count != 1 && sample_count != 2 && sample_count != 4) ||
        ![context->device supportsTextureSampleCount:sample_count]) {
      if (error_out) {
        *error_out = "unsupported persistent probe sample count";
      }
      return false;
    }
    if (context->sample_count == sample_count) {
      return true;
    }
    ProbeDepthStencilTarget* target = context->depth_stencil_target;
    if (target->attached_contexts.size() != 1 || target->attached_contexts.front() != context) {
      if (error_out) {
        *error_out = "cannot change the sample count of an already shared depth/stencil target";
      }
      return false;
    }
    if (!WaitPendingPipelineProbeCommands(context, error_out, nullptr)) {
      return false;
    }
    if (context->render_texture) {
      [context->render_texture release];
      context->render_texture = nil;
    }
    if (context->multisample_render_texture) {
      [context->multisample_render_texture release];
      context->multisample_render_texture = nil;
    }
    if (context->clear_pipeline_state) {
      [context->clear_pipeline_state release];
      context->clear_pipeline_state = nil;
    }
    if (context->depth_clear_pipeline_state) {
      [context->depth_clear_pipeline_state release];
      context->depth_clear_pipeline_state = nil;
    }
    if (target->texture) {
      [target->texture release];
      target->texture = nil;
    }
    context->sample_count = sample_count;
    context->width = 0;
    context->height = 0;
    context->initialized = false;
    context->color_resolve_dirty = false;
    target->sample_count = sample_count;
    target->width = 0;
    target->height = 0;
    target->initialized = false;
    return true;
  }
}

void* CreatePipelineProbeSnapshotTexture(void* metal_device, uint32_t width, uint32_t height,
                                         std::string* error_out) {
  @autoreleasepool {
    id<MTLDevice> device = (id<MTLDevice>)metal_device;
    if (!device || !width || !height) {
      if (error_out) {
        *error_out = "missing Metal device or snapshot dimensions";
      }
      return nullptr;
    }
    MTLTextureDescriptor* descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    descriptor.storageMode = MTLStorageModePrivate;
    descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    if (!texture) {
      if (error_out) {
        *error_out = "failed to allocate the private presentation snapshot texture";
      }
      return nullptr;
    }
    texture.label = @"ReX Metal exact resolved presentation snapshot";
    return (void*)texture;
  }
}

void ReleasePipelineProbeSnapshotTexture(void* snapshot_texture) {
  if (snapshot_texture) {
    [(id<MTLTexture>)snapshot_texture release];
  }
}

bool QueuePipelineProbeSnapshotCopy(void* metal_command_queue, void* source_texture,
                                    void* destination_texture, uint32_t width, uint32_t height,
                                    std::string* error_out) {
  @autoreleasepool {
    id<MTLCommandQueue> command_queue = (id<MTLCommandQueue>)metal_command_queue;
    id<MTLTexture> source = (id<MTLTexture>)source_texture;
    id<MTLTexture> destination = (id<MTLTexture>)destination_texture;
    if (!command_queue || !source || !destination || !width || !height ||
        [source device] != [command_queue device] ||
        [destination device] != [command_queue device] ||
        [source pixelFormat] != MTLPixelFormatBGRA8Unorm ||
        [destination pixelFormat] != MTLPixelFormatBGRA8Unorm || width > [source width] ||
        height > [source height] || width > [destination width] || height > [destination height]) {
      if (error_out) {
        *error_out = "invalid snapshot source, destination, queue, or dimensions";
      }
      return false;
    }
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    id<MTLBlitCommandEncoder> encoder = command_buffer ? [command_buffer blitCommandEncoder] : nil;
    if (!command_buffer || !encoder) {
      if (error_out) {
        *error_out = "failed to create the presentation snapshot copy command";
      }
      return false;
    }
    command_buffer.label = @"ReX Metal presentation snapshot mailbox copy";
    [encoder copyFromTexture:source
                 sourceSlice:0
                 sourceLevel:0
                sourceOrigin:MTLOriginMake(0, 0, 0)
                  sourceSize:MTLSizeMake(width, height, 1)
                   toTexture:destination
            destinationSlice:0
            destinationLevel:0
           destinationOrigin:MTLOriginMake(0, 0, 0)];
    [encoder endEncoding];
    [command_buffer commit];
    return true;
  }
}

bool FinalizePipelineProbeContext(void* opaque_context, std::string* error_out) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  if (!context) {
    if (error_out) {
      *error_out = "missing probe context";
    }
    return false;
  }
  return FinalizeOpenPipelineProbeCommandBuffer(context, error_out);
}

bool WaitPipelineProbeContext(void* opaque_context, std::string* error_out,
                              uint32_t* waited_submission_count_out) {
  return WaitPendingPipelineProbeCommands(static_cast<PipelineProbeContext*>(opaque_context),
                                          error_out, waited_submission_count_out);
}

uint32_t GetPipelineProbeContextPendingSubmissionCount(void* opaque_context) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  return GetPendingProbeSubmissionCount(context);
}

bool GetPipelineProbeContextUploadStats(void* opaque_context, PipelineProbeUploadStats* stats_out) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  if (!context || !stats_out) {
    return false;
  }
  *stats_out = context->upload_stats;
  return true;
}

uint64_t GetPipelineProbeContextMultisampleResolveCount(void* opaque_context) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  return context ? context->multisample_resolve_count : 0;
}

void ResetPipelineProbeContext(void* opaque_context) {
  auto* context = static_cast<PipelineProbeContext*>(opaque_context);
  if (context) {
    std::string finalize_error;
    if (!FinalizeOpenPipelineProbeCommandBuffer(context, &finalize_error)) {
      std::fprintf(stderr, "[metal] probe context reset failed to finalize: %s\n",
                   finalize_error.c_str());
    } else if (GetCommittedProbeDrawCommandBufferCount(context) >=
               kMaxCommittedProbeCommandBuffers) {
      uint32_t waited_submission_count = 0;
      if (!WaitPendingPipelineProbeCommands(context, &finalize_error, &waited_submission_count)) {
        std::fprintf(stderr, "[metal] probe context reset drained %u submissions with error: %s\n",
                     waited_submission_count, finalize_error.c_str());
      }
    }
    InvalidateProbeContextTargets(context);
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
  if (context->depth_clear_pipeline_state) {
    [context->depth_clear_pipeline_state release];
  }
  if (context->tiled_resolve_pipeline_state) {
    [context->tiled_resolve_pipeline_state release];
  }
  if (context->render_texture) {
    [context->render_texture release];
  }
  if (context->multisample_render_texture) {
    [context->multisample_render_texture release];
  }
  DetachProbeDepthStencilTarget(context);
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
  for (auto& depth_stencil_state : context->depth_stencil_state_cache) {
    [depth_stencil_state.second release];
  }
  context->depth_stencil_state_cache.clear();
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
    if (!PrepareProbeDepthStencilSubmission(context, false, error_out)) {
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
    ConfigureProbeColorPass(pass, context, MTLLoadActionClear,
                            MTLClearColorMake(red, green, blue, alpha));
    ConfigureProbeDepthStencilPass(pass, context->depth_stencil_target->texture,
                                   MTLLoadActionClear);

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
      context->color_resolve_dirty = context->sample_count > 1;
      context->depth_stencil_target->initialized = true;
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
    if (!PrepareProbeDepthStencilSubmission(context, false, error_out)) {
      return false;
    }
    id<MTLDepthStencilState> disabled_depth_stencil_state = nil;
    if (!GetCachedProbeDepthStencilState(context, nullptr, disabled_depth_stencil_state,
                                         error_out)) {
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
    ConfigureProbeColorPass(pass, context,
                            context->initialized ? MTLLoadActionLoad : MTLLoadActionClear,
                            MTLClearColorMake(0.0, 0.0, 0.0, 0.0));
    ConfigureProbeDepthStencilPass(
        pass, context->depth_stencil_target->texture,
        context->depth_stencil_target->initialized ? MTLLoadActionLoad : MTLLoadActionClear);

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
    [encoder setDepthStencilState:disabled_depth_stencil_state];
    [encoder setFragmentBuffer:constants_buffer offset:0 atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [command_buffer commit];
    [command_buffer waitUntilCompleted];

    bool succeeded = [command_buffer status] == MTLCommandBufferStatusCompleted;
    if (succeeded) {
      context->initialized = true;
      context->color_resolve_dirty = context->sample_count > 1;
      context->depth_stencil_target->initialized = true;
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
    id<MTLDepthStencilState> disabled_depth_stencil_state = nil;
    if (!GetCachedProbeDepthStencilState(context, nullptr, disabled_depth_stencil_state,
                                         error_out)) {
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
    [encoder setDepthStencilState:disabled_depth_stencil_state];
    [encoder setStencilFrontReferenceValue:0 backReferenceValue:0];
    [encoder setFragmentBuffer:constants.buffer offset:constants.offset atIndex:0];
    TrackOpenProbeBufferBinding(context, false, 0);
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    ++context->open_draw_submission_count;
    context->initialized = true;
    context->color_resolve_dirty = context->sample_count > 1;
    context->depth_stencil_target->initialized = true;

    if (context->open_draw_submission_count >= kMaxProbeDrawsPerCommandBuffer &&
        !FinalizeOpenPipelineProbeCommandBuffer(context, error_out)) {
      return false;
    }
    if (GetCommittedProbeDrawCommandBufferCount(context) >= kMaxCommittedProbeCommandBuffers) {
      return WaitOldestPipelineProbeCommand(context, error_out);
    }
    return true;
  }
}

bool QueuePipelineProbeContextDepthStencilClearRect(void* opaque_context, uint32_t width,
                                                    uint32_t height, uint32_t x, uint32_t y,
                                                    uint32_t clear_width, uint32_t clear_height,
                                                    float depth, uint8_t stencil,
                                                    std::string* error_out) {
  @autoreleasepool {
    auto* context = static_cast<PipelineProbeContext*>(opaque_context);
    if (!context || !std::isfinite(depth)) {
      if (error_out) {
        *error_out = "missing probe context or invalid depth clear value";
      }
      return false;
    }
    if (!clear_width || !clear_height || x >= width || y >= height) {
      return true;
    }
    clear_width = std::min(clear_width, width - x);
    clear_height = std::min(clear_height, height - y);
    if (!EnsureProbeContextTexture(context, width, height, error_out) ||
        !EnsureDepthClearPipelineState(context, error_out)) {
      return false;
    }

    ProbeDepthStencilState clear_state;
    clear_state.depth_test_enabled = true;
    clear_state.depth_write_enabled = true;
    clear_state.depth_compare_function = uint8_t(MTLCompareFunctionAlways);
    clear_state.stencil_test_enabled = true;
    clear_state.front.compare_function = uint8_t(MTLCompareFunctionAlways);
    clear_state.front.depth_stencil_pass_operation = uint8_t(MTLStencilOperationReplace);
    clear_state.front.read_mask = 0xFF;
    clear_state.front.write_mask = 0xFF;
    clear_state.front.reference = stencil;
    clear_state.back = clear_state.front;
    id<MTLDepthStencilState> metal_clear_state = nil;
    if (!GetCachedProbeDepthStencilState(context, &clear_state, metal_clear_state, error_out) ||
        !EnsureOpenPipelineProbeEncoder(context, error_out)) {
      return false;
    }

    depth = std::clamp(depth, 0.0f, 1.0f);
    ProbeUploadAllocation constants =
        UploadProbeDrawData(context, &depth, sizeof(depth), error_out);
    if (!constants.buffer) {
      DiscardEmptyOpenPipelineProbeCommandBuffer(context);
      if (error_out && error_out->empty()) {
        *error_out = "failed to upload depth clear constants";
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
    [encoder setRenderPipelineState:context->depth_clear_pipeline_state];
    [encoder setDepthStencilState:metal_clear_state];
    [encoder setStencilFrontReferenceValue:stencil backReferenceValue:stencil];
    [encoder setVertexBuffer:constants.buffer offset:constants.offset atIndex:0];
    TrackOpenProbeBufferBinding(context, true, 0);
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    ++context->open_draw_submission_count;
    context->initialized = true;
    // A first depth-only operation initializes the multisample color
    // attachment through the render pass clear. Make that deterministic black
    // initialization visible to any immediate single-sample read or resolve.
    context->color_resolve_dirty = context->sample_count > 1;
    context->depth_stencil_target->initialized = true;

    if (context->open_draw_submission_count >= kMaxProbeDrawsPerCommandBuffer &&
        !FinalizeOpenPipelineProbeCommandBuffer(context, error_out)) {
      return false;
    }
    if (GetCommittedProbeDrawCommandBufferCount(context) >= kMaxCommittedProbeCommandBuffers) {
      return WaitOldestPipelineProbeCommand(context, error_out);
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
    const ProbeRasterizationState* rasterization_state,
    const ProbeDepthStencilState* depth_stencil_state,
    uint32_t fragment_shared_memory_buffer_index) {
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
    if (!IsProbeDepthStencilStateValid(depth_stencil_state)) {
      if (error_out) {
        *error_out = "invalid probe depth/stencil state";
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

    id<MTLDepthStencilState> metal_depth_stencil_state = nil;
    if (!GetCachedProbeDepthStencilState(context, depth_stencil_state, metal_depth_stencil_state,
                                         error_out)) {
      DiscardEmptyOpenPipelineProbeCommandBuffer(context);
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
        ((vertex_shared_memory_buffer_index != UINT32_MAX ||
          fragment_shared_memory_buffer_index != UINT32_MAX) &&
         !shared_memory_buffer) ||
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
          *error_out = index_buffer && !index_buffer_object
                           ? "failed to upload persistent probe index data"
                           : ((vertex_shared_memory_buffer_index != UINT32_MAX ||
                               fragment_shared_memory_buffer_index != UINT32_MAX) &&
                                      !shared_memory_buffer
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
    [encoder setCullMode:ToMetalCullMode(rasterization_state ? rasterization_state->cull_mode
                                                             : ProbeCullMode::kNone)];
    [encoder setFrontFacingWinding:rasterization_state && rasterization_state->front_face_clockwise
                                       ? MTLWindingClockwise
                                       : MTLWindingCounterClockwise];
    [encoder setDepthStencilState:metal_depth_stencil_state];
    [encoder
        setStencilFrontReferenceValue:depth_stencil_state ? depth_stencil_state->front.reference : 0
                   backReferenceValue:depth_stencil_state ? depth_stencil_state->back.reference
                                                          : 0];
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
    if (fragment_shared_memory_buffer_index != UINT32_MAX) {
      [encoder setFragmentBuffer:shared_memory_buffer
                          offset:0
                         atIndex:fragment_shared_memory_buffer_index];
      TrackOpenProbeBufferBinding(context, false, fragment_shared_memory_buffer_index);
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
    context->color_resolve_dirty = context->sample_count > 1;
    context->depth_stencil_target->initialized = true;

    // Normal command buffers retain every encoded resource. Balance all local
    // ownership immediately; the retained command buffer owns the in-flight
    // lifetime until the context is drained.
    release_submission_resources();

    // A no-copy buffer aliases caller-owned bytes rather than snapshotting them.
    // Preserve the old synchronous contract whenever that buffer is actually
    // bound to the vertex stage. The resident MTLBuffer path remains asynchronous.
    bool raw_nocopy_buffer_bound = !shared_memory_metal_buffer && shared_memory &&
                                   (vertex_shared_memory_buffer_index != UINT32_MAX ||
                                    fragment_shared_memory_buffer_index != UINT32_MAX);
    if (context->open_draw_submission_count >= kMaxProbeDrawsPerCommandBuffer &&
        !FinalizeOpenPipelineProbeCommandBuffer(context, error_out)) {
      return false;
    }
    bool committed_limit_reached =
        GetCommittedProbeDrawCommandBufferCount(context) >= kMaxCommittedProbeCommandBuffers;
    if (raw_nocopy_buffer_bound) {
      return WaitPendingPipelineProbeCommands(context, error_out, nullptr);
    }
    if (committed_limit_reached) {
      return WaitOldestPipelineProbeCommand(context, error_out);
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
      if (!FinalizeProbeColorForConsumer(context, error_out)) {
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
    if (!FinalizeProbeColorForConsumer(context, error_out) ||
        !WaitPendingPipelineProbeCommands(context, error_out, nullptr)) {
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
    id<MTLBuffer> guest_memory_buffer = (id<MTLBuffer>)destination.guest_memory_metal_buffer;
    id<MTLTexture> presentation_snapshot =
        (id<MTLTexture>)destination.presentation_snapshot_texture;
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
    if (guest_memory_buffer) {
      uint64_t mirror_source_end = uint64_t(destination.guest_memory_copy_source_offset) +
                                   destination.guest_memory_copy_length;
      uint64_t mirror_destination_end = uint64_t(destination.guest_memory_copy_destination_offset) +
                                        destination.guest_memory_copy_length;
      if (!destination.guest_memory_copy_length ||
          [guest_memory_buffer storageMode] != MTLStorageModeShared ||
          mirror_source_end > [destination_buffer length] ||
          mirror_destination_end > [guest_memory_buffer length]) {
        return reject_and_drain(
            "tiled resolve guest mirror range is invalid or not backed by shared storage");
      }
    } else if (destination.guest_memory_copy_length) {
      return reject_and_drain("tiled resolve guest mirror range has no destination buffer");
    }
    if (presentation_snapshot &&
        ([presentation_snapshot device] != context->device ||
         [presentation_snapshot pixelFormat] != MTLPixelFormatBGRA8Unorm ||
         destination.presentation_snapshot_x >= [presentation_snapshot width] ||
         destination.presentation_snapshot_y >= [presentation_snapshot height] ||
         resolve_width > [presentation_snapshot width] - destination.presentation_snapshot_x ||
         resolve_height > [presentation_snapshot height] - destination.presentation_snapshot_y)) {
      return reject_and_drain("tiled resolve presentation snapshot texture is incompatible");
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
    if (!FinalizeProbeColorForConsumer(context, &finalize_error)) {
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
    if (presentation_snapshot) {
      [blit_encoder copyFromTexture:context->render_texture
                        sourceSlice:0
                        sourceLevel:0
                       sourceOrigin:MTLOriginMake(source_x, source_y, 0)
                         sourceSize:MTLSizeMake(resolve_width, resolve_height, 1)
                          toTexture:presentation_snapshot
                   destinationSlice:0
                   destinationLevel:0
                  destinationOrigin:MTLOriginMake(destination.presentation_snapshot_x,
                                                  destination.presentation_snapshot_y, 0)];
    }
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

    if (guest_memory_buffer) {
      id<MTLBlitCommandEncoder> mirror_encoder = [command_buffer blitCommandEncoder];
      if (!mirror_encoder) {
        return reject_and_drain("failed to create tiled resolve guest mirror blit encoder");
      }
      [mirror_encoder copyFromBuffer:destination_buffer
                        sourceOffset:destination.guest_memory_copy_source_offset
                            toBuffer:guest_memory_buffer
                   destinationOffset:destination.guest_memory_copy_destination_offset
                                size:destination.guest_memory_copy_length];
      [mirror_encoder endEncoding];
    }

    if (!bgra_out) {
      CommittedProbeCommandBuffer committed;
      committed.command_buffer = [command_buffer retain];
      committed.auxiliary_submission_count = 1;
      committed.async_failure_callback = destination.async_failure_callback;
      committed.async_failure_callback_context = destination.async_failure_callback_context;
      committed.async_failure_start = destination.async_failure_start;
      committed.async_failure_length = destination.async_failure_length;
      try {
        context->committed_command_buffers.push_back(committed);
      } catch (...) {
        [committed.command_buffer release];
        if (error_out) {
          *error_out = "failed to retain asynchronous tiled resolve submission";
        }
        return false;
      }
      [command_buffer commit];
      return true;
    }

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
    const ProbeRasterizationState* rasterization_state,
    const ProbeDepthStencilState* depth_stencil_state) {
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
  if (!IsProbeDepthStencilStateValid(depth_stencil_state)) {
    if (error_out) {
      *error_out = "invalid probe depth/stencil state";
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
  MTLTextureDescriptor* depth_stencil_descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  depth_stencil_descriptor.usage = MTLTextureUsageRenderTarget;
  depth_stencil_descriptor.storageMode = MTLStorageModePrivate;
  id<MTLTexture> depth_stencil_texture = [device newTextureWithDescriptor:depth_stencil_descriptor];
  if (!render_texture || !depth_stencil_texture) {
    if (render_texture) {
      [render_texture release];
    }
    if (depth_stencil_texture) {
      [depth_stencil_texture release];
    }
    [command_queue release];
    if (error_out) {
      *error_out = "failed to create color or depth/stencil render texture";
    }
    return false;
  }

  ProbeDepthStencilState disabled_depth_stencil_state;
  id<MTLDepthStencilState> metal_depth_stencil_state = CreateProbeDepthStencilState(
      device, depth_stencil_state ? *depth_stencil_state : disabled_depth_stencil_state);
  if (!metal_depth_stencil_state) {
    [depth_stencil_texture release];
    [render_texture release];
    [command_queue release];
    if (error_out) {
      *error_out = "failed to create probe depth/stencil state";
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
    [depth_stencil_texture release];
    if (metal_depth_stencil_state) {
      [metal_depth_stencil_state release];
    }
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
  ConfigureProbeDepthStencilPass(pass, depth_stencil_texture, MTLLoadActionClear);

  id<MTLRenderCommandEncoder> encoder = [command_buffer renderCommandEncoderWithDescriptor:pass];
  [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline_state];
  [encoder setCullMode:ToMetalCullMode(rasterization_state ? rasterization_state->cull_mode
                                                           : ProbeCullMode::kNone)];
  [encoder setFrontFacingWinding:rasterization_state && rasterization_state->front_face_clockwise
                                     ? MTLWindingClockwise
                                     : MTLWindingCounterClockwise];
  [encoder setDepthStencilState:metal_depth_stencil_state];
  [encoder
      setStencilFrontReferenceValue:depth_stencil_state ? depth_stencil_state->front.reference : 0
                 backReferenceValue:depth_stencil_state ? depth_stencil_state->back.reference : 0];
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
  [depth_stencil_texture release];
  if (metal_depth_stencil_state) {
    [metal_depth_stencil_state release];
  }
  [command_queue release];
  return succeeded;
}

}  // namespace rex::graphics::metal
