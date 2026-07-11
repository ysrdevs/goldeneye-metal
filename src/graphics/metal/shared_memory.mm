#include <rex/graphics/metal/shared_memory.h>

#import <Metal/Metal.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

namespace rex::graphics::metal {
namespace {

constexpr uint32_t kWatchedFramebufferBase = 0x1ec30000u;
constexpr uint32_t kWatchedFramebufferLength = 1280u * 720u * 4u;
constexpr uint32_t kWatchedResolveBase = 0x1eeb0000u;
constexpr uint32_t kWatchedResolveLength = 1280u * 224u * 4u;
constexpr uint32_t kWatchedSwapBase = 0x1efc8000u;
constexpr uint32_t kWatchedSwapLength = 1280u * 720u * 4u;

bool RangesOverlap(uint32_t a_start, uint32_t a_length, uint32_t b_start, uint32_t b_length) {
  uint64_t a_end = uint64_t(a_start) + a_length;
  uint64_t b_end = uint64_t(b_start) + b_length;
  return uint64_t(a_start) < b_end && uint64_t(b_start) < a_end;
}

}  // namespace

MetalSharedMemory::MetalSharedMemory(memory::Memory& memory, TraceWriter& trace_writer)
    : SharedMemory(memory), trace_writer_(trace_writer) {}

MetalSharedMemory::~MetalSharedMemory() {
  Shutdown(true);
}

void MetalSharedMemory::SetHostResourceMutationCallback(HostResourceMutationCallback callback) {
  host_resource_mutation_callback_ = std::move(callback);
}

bool MetalSharedMemory::SynchronizeBeforeHostResourceMutation() {
  return !host_resource_mutation_callback_ || host_resource_mutation_callback_();
}

bool MetalSharedMemory::Initialize(void* metal_device) {
  if (!metal_device) {
    return false;
  }
  metal_device_ = metal_device;
  InitializeCommon();

  // Non-owning device handle (mirrors texture_cache.mm cast style).
  id<MTLDevice> device = (id<MTLDevice>)metal_device_;
  // MTLStorageModeShared is mandatory on Apple Silicon UMA: the resolve compute
  // write, the texture-cache read, and CPU readback all observe the same bytes.
  // MTLStorageModeMemoryless is never valid here.
  id<MTLBuffer> gpu_buffer = [device newBufferWithLength:kBufferSize
                                                 options:MTLResourceStorageModeShared];
  if (!gpu_buffer) {
    std::fprintf(stderr, "[metal] MetalSharedMemory: failed to allocate %u-byte shared buffer\n",
                 kBufferSize);
    std::fflush(stderr);
    return false;
  }
  // newBufferWithLength returns a +1 object. This target is built without ARC,
  // so store that ownership directly and balance it in Shutdown(). A bridging
  // retain here would add a second reference and leak the 512 MiB buffer.
  buffer_ = (void*)gpu_buffer;
  return true;
}

void MetalSharedMemory::Shutdown(bool from_destructor) {
  if (buffer_) {
    [(id<MTLBuffer>)buffer_ release];
    buffer_ = nullptr;
  }
  metal_device_ = nullptr;
  if (!from_destructor) {
    ShutdownCommon();
  }
}

bool MetalSharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  if (!buffer_) {
    return false;
  }

  if (!SynchronizeBeforeHostResourceMutation()) {
    return false;
  }

  id<MTLBuffer> gpu_buffer = (id<MTLBuffer>)buffer_;
  uint8_t* buffer_contents = reinterpret_cast<uint8_t*>([gpu_buffer contents]);
  if (!buffer_contents) {
    return false;
  }

  for (const auto& range : upload_page_ranges) {
    uint32_t start = std::min(range.first << page_size_log2(), kBufferSize);
    uint32_t length = std::min(range.second << page_size_log2(), kBufferSize - start);
    if (!length) {
      continue;
    }

    const bool watched_framebuffer =
        RangesOverlap(start, length, kWatchedFramebufferBase, kWatchedFramebufferLength);
    const bool watched_resolve =
        RangesOverlap(start, length, kWatchedResolveBase, kWatchedResolveLength);
    const bool watched_swap = RangesOverlap(start, length, kWatchedSwapBase, kWatchedSwapLength);
    if (watched_framebuffer || watched_resolve || watched_swap) {
      static std::atomic<uint32_t> watched_upload_logs{0};
      uint32_t upload_index = watched_upload_logs.fetch_add(1, std::memory_order_relaxed) + 1;
      if (upload_index <= 32 || (upload_index & 0xFF) == 0) {
        std::fprintf(stderr,
                     "[metal] watched shared-memory upload#%u range=0x%08x+0x%x "
                     "framebuffer=%u resolve=%u swap=%u\n",
                     upload_index, start, length, watched_framebuffer ? 1u : 0u,
                     watched_resolve ? 1u : 0u, watched_swap ? 1u : 0u);
        std::fflush(stderr);
      }
    }

    // GPU trace fidelity (mirrors vulkan/shared_memory.cpp:364): record the guest
    // bytes this upload reads into the trace BEFORE the memcpy. host_ptr=nullptr =>
    // TraceWriter reads membase_+start itself. No-op (early return) when not
    // tracing, so the normal path pays nothing. Without this a Metal-captured
    // trace omits all vertex/texture backing memory -> replay reads stale/zero
    // (the "type-2 invalid vertex fetch" poisoning).
    trace_writer_.WriteMemoryRead(start, length);

    // Contract (shared_memory.h; Vulkan ref vulkan/shared_memory.cpp): mark valid
    // BEFORE the memcpy so an invalidation occurring during the copy is not missed.
    MakeRangeValid(start, length, false);

    // UMA: copy guest physical bytes straight into the shared buffer. No staging
    // buffer, no blit, no didModifyRange/synchronize - MTLStorageModeShared is
    // CPU/GPU coherent on Apple Silicon.
    const uint8_t* guest_src = memory().TranslatePhysical<const uint8_t*>(start);
    if (!guest_src) {
      return false;
    }
    std::memcpy(buffer_contents + start, guest_src, length);
  }

  return true;
}

bool MetalSharedMemory::CommitGuestCpuWriteAsGpu(uint32_t start, uint32_t length) {
  if (!length || start >= kBufferSize || !buffer_) {
    return false;
  }
  length = std::min(length, kBufferSize - start);

  const uint8_t* guest_source = memory().TranslatePhysical<const uint8_t*>(start);
  id<MTLBuffer> gpu_buffer = (id<MTLBuffer>)buffer_;
  uint8_t* buffer_contents = reinterpret_cast<uint8_t*>([gpu_buffer contents]);
  if (!guest_source || !buffer_contents) {
    return false;
  }

  if (!SynchronizeBeforeHostResourceMutation()) {
    return false;
  }

  // Publish and protect before copying, matching UploadRanges' race contract.
  // If the guest CPU writes this range during the copy, the protection callback
  // invalidates it again and the next RequestRange performs a fresh upload.
  // Texture watches only mark resources outdated here; their reload happens on
  // a later command-processor request, after this synchronous copy completes.
  RangeWrittenByGpu(start, length);
  std::memcpy(buffer_contents + start, guest_source, length);
  return true;
}

bool MetalSharedMemory::CommitGpuBufferWriteToGuest(uint32_t start, uint32_t length) {
  if (!length || start >= kBufferSize || !buffer_) {
    return false;
  }
  length = std::min(length, kBufferSize - start);

  uint8_t* guest_destination = memory().TranslatePhysical<uint8_t*>(start);
  id<MTLBuffer> gpu_buffer = (id<MTLBuffer>)buffer_;
  const uint8_t* buffer_contents = reinterpret_cast<const uint8_t*>([gpu_buffer contents]);
  if (!guest_destination || !buffer_contents) {
    return false;
  }

  // The producer helper waits for its command buffer before reaching here.
  // Drain every other context that may still reference shared memory before
  // publishing the new bytes and invalidating its cached consumers.
  if (!SynchronizeBeforeHostResourceMutation()) {
    return false;
  }

  // Publish and protect before copying. TranslatePhysical returns the physical
  // bypass mapping, so this host memcpy does not trip the guest write watch;
  // a concurrent guest write through a virtual alias does and leaves the
  // shared range invalid for the next RequestRange.
  RangeWrittenByGpu(start, length);
  std::memcpy(guest_destination, buffer_contents + start, length);
  return true;
}

}  // namespace rex::graphics::metal
