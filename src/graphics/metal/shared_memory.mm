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
constexpr size_t kMaxPendingUploadCount = 128;
constexpr size_t kMaxPendingUploadBytes = size_t(128) << 20;

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

void MetalSharedMemory::SetGpuResourceMutationCallback(GpuResourceMutationCallback callback) {
  gpu_resource_mutation_callback_ = std::move(callback);
}

bool MetalSharedMemory::SynchronizeBeforeHostResourceMutation() {
  bool contexts_synchronized =
      !host_resource_mutation_callback_ || host_resource_mutation_callback_();
  // Uploads are committed on the same queue as persistent draws, but they are
  // owned here rather than by a render context. Host reads and mutations must
  // therefore reap them explicitly even after the context callback drains.
  bool uploads_synchronized = WaitForPendingUploads();
  return contexts_synchronized && uploads_synchronized;
}

bool MetalSharedMemory::Initialize(void* metal_device) {
  if (!metal_device) {
    return false;
  }
  metal_device_ = metal_device;
  InitializeCommon();

  // Non-owning device handle (mirrors texture_cache.mm cast style).
  id<MTLDevice> device = (id<MTLDevice>)metal_device_;
  id<MTLCommandQueue> command_queue = [device newCommandQueue];
  if (!command_queue) {
    std::fprintf(stderr, "[metal] MetalSharedMemory: failed to create shared command queue\n");
    std::fflush(stderr);
    return false;
  }
  command_queue.label = @"ReX Metal shared submission queue";
  // MTLStorageModeShared is mandatory on Apple Silicon UMA: the resolve compute
  // write, the texture-cache read, and CPU readback all observe the same bytes.
  // MTLStorageModeMemoryless is never valid here.
  id<MTLBuffer> gpu_buffer = [device newBufferWithLength:kBufferSize
                                                 options:MTLResourceStorageModeShared];
  if (!gpu_buffer) {
    std::fprintf(stderr, "[metal] MetalSharedMemory: failed to allocate %u-byte shared buffer\n",
                 kBufferSize);
    std::fflush(stderr);
    [command_queue release];
    return false;
  }
  // newBufferWithLength returns a +1 object. This target is built without ARC,
  // so store that ownership directly and balance it in Shutdown(). A bridging
  // retain here would add a second reference and leak the 512 MiB buffer.
  command_queue_ = (void*)command_queue;
  buffer_ = (void*)gpu_buffer;
  return true;
}

void MetalSharedMemory::Shutdown(bool from_destructor) {
  // The render contexts release before the explicit command-processor shutdown
  // reaches here. Waiting for the last upload also completes all earlier work
  // on the common queue, allowing the staging buffers and destination to go.
  WaitForPendingUploads();
  host_resource_mutation_callback_ = {};
  gpu_resource_mutation_callback_ = {};
  if (buffer_) {
    [(id<MTLBuffer>)buffer_ release];
    buffer_ = nullptr;
  }
  if (command_queue_) {
    [(id<MTLCommandQueue>)command_queue_ release];
    command_queue_ = nullptr;
  }
  metal_device_ = nullptr;
  if (!from_destructor) {
    ShutdownCommon();
  }
}

void MetalSharedMemory::ClearCache() {
  if (!SynchronizeBeforeHostResourceMutation()) {
    std::fprintf(stderr,
                 "[metal] MetalSharedMemory: synchronization failed while clearing cache\n");
    std::fflush(stderr);
  }
  SharedMemory::ClearCache();
}

void MetalSharedMemory::InvalidateUploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& byte_ranges) {
  for (const auto& range : byte_ranges) {
    if (range.second) {
      MemoryInvalidationCallback(range.first, range.second, true);
    }
  }
}

bool MetalSharedMemory::ReapPendingUploads(bool wait_for_all) {
  @autoreleasepool {
    if (wait_for_all && !pending_uploads_.empty()) {
      id<MTLCommandBuffer> newest = (id<MTLCommandBuffer>)pending_uploads_.back().command_buffer;
      if (newest && [newest status] != MTLCommandBufferStatusCompleted &&
          [newest status] != MTLCommandBufferStatusError) {
        [newest waitUntilCompleted];
      }
    }

    bool succeeded = true;
    while (!pending_uploads_.empty()) {
      PendingUpload& front = pending_uploads_.front();
      id<MTLCommandBuffer> command_buffer = (id<MTLCommandBuffer>)front.command_buffer;
      MTLCommandBufferStatus status =
          command_buffer ? [command_buffer status] : MTLCommandBufferStatusError;
      if (!wait_for_all && status != MTLCommandBufferStatusCompleted &&
          status != MTLCommandBufferStatusError) {
        break;
      }
      if (wait_for_all && command_buffer && status != MTLCommandBufferStatusCompleted &&
          status != MTLCommandBufferStatusError) {
        [command_buffer waitUntilCompleted];
        status = [command_buffer status];
      }

      bool upload_succeeded = status == MTLCommandBufferStatusCompleted;
      if (!upload_succeeded) {
        succeeded = false;
        InvalidateUploadRanges(front.byte_ranges);
        static std::atomic<uint32_t> upload_failure_logs{0};
        uint32_t failure_index = upload_failure_logs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failure_index <= 16 || (failure_index & 0x3F) == 0) {
          NSError* error = command_buffer ? [command_buffer error] : nil;
          const char* description = error ? [[error localizedDescription] UTF8String] : nullptr;
          std::fprintf(stderr, "[metal] shared-memory upload failed#%u: %s\n", failure_index,
                       description ? description : "missing or incomplete command buffer");
          std::fflush(stderr);
        }
      }
      if (front.command_buffer) {
        [(id<MTLCommandBuffer>)front.command_buffer release];
      }
      if (front.staging_buffer) {
        [(id<MTLBuffer>)front.staging_buffer release];
      }
      pending_upload_bytes_ -= std::min(pending_upload_bytes_, front.staging_size);
      pending_uploads_.pop_front();
    }
    return succeeded;
  }
}

bool MetalSharedMemory::WaitForPendingUploads() {
  return ReapPendingUploads(true);
}

bool MetalSharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  if (!buffer_ || !command_queue_) {
    return false;
  }

  // Discover asynchronous failures before accepting another request. Failed
  // ranges are invalidated, and returning false makes the caller retry from a
  // freshly computed validity snapshot rather than accidentally omitting them.
  if (!ReapPendingUploads(false)) {
    return false;
  }

  std::vector<std::pair<uint32_t, uint32_t>> byte_ranges;
  byte_ranges.reserve(upload_page_ranges.size());
  uint64_t total_length = 0;
  for (const auto& range : upload_page_ranges) {
    uint64_t start_unclamped = uint64_t(range.first) << page_size_log2();
    uint64_t length_unclamped = uint64_t(range.second) << page_size_log2();
    uint32_t start = uint32_t(std::min<uint64_t>(start_unclamped, kBufferSize));
    uint32_t length =
        uint32_t(std::min<uint64_t>(length_unclamped, uint64_t(kBufferSize) - uint64_t(start)));
    if (length) {
      byte_ranges.emplace_back(start, length);
      total_length += length;
    }
  }
  if (byte_ranges.empty()) {
    return true;
  }
  if (total_length > kBufferSize || total_length > SIZE_MAX) {
    return false;
  }
  if ((pending_uploads_.size() >= kMaxPendingUploadCount ||
       pending_upload_bytes_ + size_t(total_length) > kMaxPendingUploadBytes) &&
      !WaitForPendingUploads()) {
    return false;
  }

  @autoreleasepool {
    id<MTLDevice> device = (id<MTLDevice>)metal_device_;
    id<MTLCommandQueue> command_queue = (id<MTLCommandQueue>)command_queue_;
    id<MTLBuffer> gpu_buffer = (id<MTLBuffer>)buffer_;
    id<MTLBuffer> staging_buffer = [device newBufferWithLength:NSUInteger(total_length)
                                                       options:MTLResourceStorageModeShared];
    uint8_t* staging_contents =
        staging_buffer ? reinterpret_cast<uint8_t*>([staging_buffer contents]) : nullptr;
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit_encoder =
        command_buffer ? [command_buffer blitCommandEncoder] : nil;
    if (!staging_contents || !command_buffer || !blit_encoder) {
      if (blit_encoder) {
        [blit_encoder endEncoding];
      }
      if (staging_buffer) {
        [staging_buffer release];
      }
      return false;
    }
    command_buffer.label = @"ReX Metal shared-memory upload";

    // Commit every draw encoded before this request, but don't wait. All probe
    // contexts retain command_queue_, so commit order provides the read ->
    // upload -> later-read dependency without serializing the CPU and GPU.
    if (gpu_resource_mutation_callback_ && !gpu_resource_mutation_callback_()) {
      [blit_encoder endEncoding];
      [staging_buffer release];
      return false;
    }

    std::vector<std::pair<uint32_t, uint32_t>> marked_valid_ranges;
    marked_valid_ranges.reserve(byte_ranges.size());
    NSUInteger staging_offset = 0;
    for (const auto& range : byte_ranges) {
      uint32_t start = range.first;
      uint32_t length = range.second;

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

      // GPU trace fidelity (mirrors VulkanSharedMemory::UploadRanges).
      trace_writer_.WriteMemoryRead(start, length);

      // Preserve SharedMemory's invalidation race contract: publish validity
      // before snapshotting guest bytes. A concurrent guest write invalidates
      // the page again, causing a later request to enqueue a newer upload.
      MakeRangeValid(start, length, false);
      marked_valid_ranges.emplace_back(start, length);
      const uint8_t* guest_source = memory().TranslatePhysical<const uint8_t*>(start);
      if (!guest_source) {
        [blit_encoder endEncoding];
        InvalidateUploadRanges(marked_valid_ranges);
        [staging_buffer release];
        return false;
      }
      std::memcpy(staging_contents + staging_offset, guest_source, length);
      [blit_encoder copyFromBuffer:staging_buffer
                      sourceOffset:staging_offset
                          toBuffer:gpu_buffer
                 destinationOffset:start
                              size:length];
      staging_offset += length;
    }

    [blit_encoder endEncoding];
    PendingUpload pending;
    pending.command_buffer = (void*)[command_buffer retain];
    pending.staging_buffer = (void*)staging_buffer;
    pending.staging_size = size_t(total_length);
    pending.byte_ranges = std::move(byte_ranges);
    try {
      pending_uploads_.push_back(std::move(pending));
    } catch (...) {
      [(id<MTLCommandBuffer>)pending.command_buffer release];
      [(id<MTLBuffer>)pending.staging_buffer release];
      InvalidateUploadRanges(marked_valid_ranges);
      return false;
    }
    [command_buffer commit];
    pending_upload_bytes_ += size_t(total_length);
    return true;
  }
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
