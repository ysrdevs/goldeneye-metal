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

bool NormalizeByteRanges(const std::vector<std::pair<uint32_t, uint32_t>>& input,
                         std::vector<std::pair<uint32_t, uint32_t>>& output) {
  output.clear();
  output.reserve(input.size());
  for (const auto& range : input) {
    if (!range.second) {
      continue;
    }
    if (range.first >= SharedMemory::kBufferSize ||
        range.second > SharedMemory::kBufferSize - range.first) {
      output.clear();
      return false;
    }
    output.push_back(range);
  }
  std::sort(output.begin(), output.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  size_t merged_count = 0;
  for (const auto& range : output) {
    if (merged_count) {
      auto& previous = output[merged_count - 1];
      uint64_t previous_end = uint64_t(previous.first) + previous.second;
      uint64_t range_end = uint64_t(range.first) + range.second;
      if (uint64_t(range.first) <= previous_end) {
        previous.second = uint32_t(std::max(previous_end, range_end) - previous.first);
        continue;
      }
    }
    output[merged_count++] = range;
  }
  output.resize(merged_count);
  return true;
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
  id<MTLBuffer> guest_memory_buffer = [device newBufferWithBytesNoCopy:memory().physical_membase()
                                                                length:kBufferSize
                                                               options:MTLResourceStorageModeShared
                                                           deallocator:nil];
  if (!guest_memory_buffer) {
    std::fprintf(stderr, "[metal] MetalSharedMemory: guest physical Metal alias unavailable; "
                         "GPU resolve publication will use the synchronous fallback\n");
    std::fflush(stderr);
  } else {
    guest_memory_buffer.label = @"ReX Metal guest physical memory alias";
  }
  // newBufferWithLength returns a +1 object. This target is built without ARC,
  // so store that ownership directly and balance it in Shutdown(). A bridging
  // retain here would add a second reference and leak the 512 MiB buffer.
  command_queue_ = (void*)command_queue;
  buffer_ = (void*)gpu_buffer;
  guest_memory_buffer_ = (void*)guest_memory_buffer;
  return true;
}

void MetalSharedMemory::Shutdown(bool from_destructor) {
  // The render contexts release before the explicit command-processor shutdown
  // reaches here. Waiting for the last upload also completes all earlier work
  // on the common queue, allowing the staging buffers and destination to go.
  WaitForPendingUploads();
  host_resource_mutation_callback_ = {};
  gpu_resource_mutation_callback_ = {};
  if (guest_memory_buffer_) {
    [(id<MTLBuffer>)guest_memory_buffer_ release];
    guest_memory_buffer_ = nullptr;
  }
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

  uint32_t original_start = start;
  uint32_t original_length = length;
  uint32_t page_mask = uint32_t(rex::memory::page_size()) - 1;
  uint32_t copy_start = start & ~page_mask;
  uint64_t copy_end_unclamped = (uint64_t(start) + length + page_mask) & ~uint64_t(page_mask);
  uint32_t copy_end = uint32_t(std::min<uint64_t>(copy_end_unclamped, uint64_t(kBufferSize)));
  uint32_t copy_length = copy_end - copy_start;
  const uint8_t* guest_source = memory().TranslatePhysical<const uint8_t*>(copy_start);
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
  RangeWrittenByGpu(original_start, original_length);
  std::memcpy(buffer_contents + copy_start, guest_source, copy_length);
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

bool MetalSharedMemory::PublishGpuBufferWritesToGuest(
    const std::vector<std::pair<uint32_t, uint32_t>>& byte_ranges) {
  std::vector<std::pair<uint32_t, uint32_t>> normalized_ranges;
  if (!NormalizeByteRanges(byte_ranges, normalized_ranges)) {
    return false;
  }
  if (normalized_ranges.empty()) {
    return true;
  }
  if (!buffer_) {
    return false;
  }

  id<MTLBuffer> gpu_buffer = (id<MTLBuffer>)buffer_;
  const uint8_t* buffer_contents = reinterpret_cast<const uint8_t*>([gpu_buffer contents]);
  if (!buffer_contents || !SynchronizeBeforeHostResourceMutation()) {
    return false;
  }

  // RangeWrittenByGpu was called when each producer was enqueued. Copying the
  // already-published ranges here avoids firing watches a second time (which
  // would invalidate exact resolve metadata after it had just been built).
  for (const auto& range : normalized_ranges) {
    uint8_t* guest_destination = memory().TranslatePhysical<uint8_t*>(range.first);
    if (!guest_destination) {
      return false;
    }
    std::memcpy(guest_destination, buffer_contents + range.first, range.second);
  }
  return true;
}

bool MetalSharedMemory::EnqueueGpuOrderedGuestMemoryWrite(uint32_t start, const void* data,
                                                          size_t length) {
  static const std::vector<std::pair<uint32_t, uint32_t>> no_publication_ranges;
  return EnqueueGpuOrderedGuestMemoryPublicationAndWrite(no_publication_ranges, start, data,
                                                         length);
}

bool MetalSharedMemory::EnqueueGpuOrderedGuestMemoryPublicationAndWrite(
    const std::vector<std::pair<uint32_t, uint32_t>>& publication_byte_ranges,
    uint32_t completion_start, const void* completion_data, size_t completion_length) {
  OrderedGuestMemoryWrite write = {
      completion_start,
      0,
      uint32_t(std::min<size_t>(completion_length, UINT32_MAX)),
  };
  if (completion_length > UINT32_MAX) {
    return false;
  }
  const std::vector<OrderedGuestMemoryWrite> writes = {write};
  return EnqueueGpuOrderedGuestMemoryPublicationAndWrites(publication_byte_ranges, writes,
                                                          completion_data, completion_length);
}

bool MetalSharedMemory::EnqueueGpuOrderedGuestMemoryPublicationAndWrites(
    const std::vector<std::pair<uint32_t, uint32_t>>& publication_byte_ranges,
    const std::vector<OrderedGuestMemoryWrite>& completion_writes, const void* completion_data,
    size_t completion_data_length) {
  if (!completion_data || !completion_data_length || completion_writes.empty() || !buffer_ ||
      !guest_memory_buffer_ || !command_queue_ || !metal_device_) {
    return false;
  }
  std::vector<std::pair<uint32_t, uint32_t>> publication_ranges;
  if (!NormalizeByteRanges(publication_byte_ranges, publication_ranges)) {
    return false;
  }
  std::vector<std::pair<uint32_t, uint32_t>> completion_ranges;
  completion_ranges.reserve(completion_writes.size());
  for (const OrderedGuestMemoryWrite& write : completion_writes) {
    if (!write.length || write.start >= kBufferSize || write.length > kBufferSize - write.start ||
        write.data_offset > completion_data_length ||
        write.length > completion_data_length - write.data_offset) {
      return false;
    }
    completion_ranges.emplace_back(write.start, write.length);
  }
  // RangeWrittenByGpu protects whole host pages. Preserve every byte outside
  // these small packet writes before each page becomes GPU-owned, otherwise a
  // later RequestRange may trust stale resident bytes.
  if (!RequestRanges(completion_ranges.data(), completion_ranges.size())) {
    return false;
  }
  if (!ReapPendingUploads(false)) {
    return false;
  }
  if ((pending_uploads_.size() >= kMaxPendingUploadCount ||
       pending_upload_bytes_ + completion_data_length > kMaxPendingUploadBytes) &&
      !WaitForPendingUploads()) {
    return false;
  }

  @autoreleasepool {
    id<MTLDevice> device = (id<MTLDevice>)metal_device_;
    id<MTLBuffer> resident_buffer = (id<MTLBuffer>)buffer_;
    id<MTLBuffer> guest_buffer = (id<MTLBuffer>)guest_memory_buffer_;
    id<MTLCommandQueue> command_queue = (id<MTLCommandQueue>)command_queue_;
    id<MTLBuffer> staging_buffer = [device newBufferWithBytes:completion_data
                                                       length:completion_data_length
                                                      options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> command_buffer = [command_queue commandBuffer];
    id<MTLBlitCommandEncoder> blit_encoder =
        command_buffer ? [command_buffer blitCommandEncoder] : nil;
    if (!staging_buffer || !command_buffer || !blit_encoder) {
      if (blit_encoder) {
        [blit_encoder endEncoding];
      }
      if (staging_buffer) {
        [staging_buffer release];
      }
      return false;
    }
    command_buffer.label = publication_ranges.empty()
                               ? @"ReX Metal ordered guest memory write"
                               : @"ReX Metal resolve publication and completion";

    // Commit all open render encoders first. Every participant uses this
    // command queue, so publication and the completion write become the
    // guest-visible completion point for everything encoded before this packet.
    if (gpu_resource_mutation_callback_ && !gpu_resource_mutation_callback_()) {
      [blit_encoder endEncoding];
      [staging_buffer release];
      return false;
    }
    for (const auto& range : publication_ranges) {
      [blit_encoder copyFromBuffer:resident_buffer
                      sourceOffset:range.first
                          toBuffer:guest_buffer
                 destinationOffset:range.first
                              size:range.second];
    }
    for (const OrderedGuestMemoryWrite& write : completion_writes) {
      [blit_encoder copyFromBuffer:staging_buffer
                      sourceOffset:write.data_offset
                          toBuffer:resident_buffer
                 destinationOffset:write.start
                              size:write.length];
      [blit_encoder copyFromBuffer:staging_buffer
                      sourceOffset:write.data_offset
                          toBuffer:guest_buffer
                 destinationOffset:write.start
                              size:write.length];
    }
    [blit_encoder endEncoding];

    PendingUpload pending;
    pending.command_buffer = (void*)[command_buffer retain];
    pending.staging_buffer = (void*)staging_buffer;
    pending.staging_size = completion_data_length;
    pending.byte_ranges = publication_ranges;
    pending.byte_ranges.insert(pending.byte_ranges.end(), completion_ranges.begin(),
                               completion_ranges.end());
    try {
      pending_uploads_.push_back(std::move(pending));
    } catch (...) {
      [(id<MTLCommandBuffer>)pending.command_buffer release];
      [(id<MTLBuffer>)pending.staging_buffer release];
      return false;
    }

    // Publish before commit, matching UploadRanges' invalidation race
    // contract. If the command later fails, ReapPendingUploads invalidates the
    // range and a future RequestRange restores it from guest memory.
    for (const auto& range : completion_ranges) {
      RangeWrittenByGpu(range.first, range.second);
    }
    [command_buffer commit];
    pending_upload_bytes_ += completion_data_length;
    return true;
  }
}

}  // namespace rex::graphics::metal
