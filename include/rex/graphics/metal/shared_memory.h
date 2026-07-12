#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <utility>
#include <vector>

#include <rex/graphics/shared_memory.h>
#include <rex/graphics/trace_writer.h>
#include <rex/memory.h>

namespace rex::graphics::metal {

class MetalSharedMemory final : public SharedMemory {
 public:
  using HostResourceMutationCallback = std::function<bool()>;
  using GpuResourceMutationCallback = std::function<bool()>;

  // trace_writer records every uploaded guest page range into the GPU trace
  // (mirrors VulkanSharedMemory). Without it a Metal-captured trace omits the
  // vertex/texture backing memory the draws fetch, poisoning replay.
  MetalSharedMemory(memory::Memory& memory, TraceWriter& trace_writer);
  ~MetalSharedMemory() override;

  // metal_device must be a non-owning id<MTLDevice> passed as void* (the command
  // processor stores the device as void*). Allocates the backing MTLBuffer with
  // MTLStorageModeShared (Apple Silicon UMA: resolve compute write, texture-cache
  // read, and CPU readback all see the same bytes).
  bool Initialize(void* metal_device);
  void Shutdown(bool from_destructor = false);
  void ClearCache() override;

  memory::Memory& guest_memory() const { return memory(); }

  // Returns the GPU buffer backing this shared memory as an opaque pointer.
  // Valid after a successful Initialize() and before Shutdown(). ObjC++ callers
  // must cast to id<MTLBuffer>. Returned as void* so this header stays usable
  // from plain C++ translation units (e.g. command_processor.cpp). Mirrors
  // VulkanSharedMemory::buffer() / D3D12SharedMemory::GetBuffer().
  void* buffer() const { return buffer_; }
  // The command queue used for shared-memory uploads. Persistent render
  // contexts retain this queue so uploads and draws have one total GPU order.
  void* command_queue() const { return command_queue_; }

  // Installs the command-processor synchronization callback used before the
  // CPU reads or mutates resources that may still be referenced by Metal
  // command buffers. With no callback installed, synchronization is a no-op.
  void SetHostResourceMutationCallback(HostResourceMutationCallback callback);
  // Called before an upload command buffer is enqueued. The command processor
  // finalizes open draw encoders without waiting so all earlier reads are
  // committed before the upload on command_queue().
  void SetGpuResourceMutationCallback(GpuResourceMutationCallback callback);
  bool SynchronizeBeforeHostResourceMutation();
  bool WaitForPendingUploads();

  // Commits bytes written through the guest CPU mapping to the separate Metal
  // shared-memory buffer, then publishes the range as GPU-produced data. This
  // is used by the current CPU readback/resolve path: RangeWrittenByGpu alone
  // would mark the Metal copy valid without actually updating its bytes.
  bool CommitGuestCpuWriteAsGpu(uint32_t start, uint32_t length);

  // Publishes a completed GPU write already present in the Metal buffer back
  // to the guest physical mapping. The caller must have waited for the Metal
  // command that produced the bytes and must serialize guest writers from the
  // preceding RequestRange through this call. Unlike CommitGuestCpuWriteAsGpu,
  // this copies Metal -> guest and does not copy the same bytes back again.
  bool CommitGpuBufferWriteToGuest(uint32_t start, uint32_t length);

 protected:
  bool UploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

 private:
  struct PendingUpload {
    void* command_buffer = nullptr;  // Owned id<MTLCommandBuffer>.
    void* staging_buffer = nullptr;  // Owned id<MTLBuffer>.
    size_t staging_size = 0;
    std::vector<std::pair<uint32_t, uint32_t>> byte_ranges;
  };

  bool ReapPendingUploads(bool wait_for_all);
  void InvalidateUploadRanges(const std::vector<std::pair<uint32_t, uint32_t>>& byte_ranges);

  TraceWriter& trace_writer_;      // Records uploaded ranges into the GPU trace.
  void* metal_device_ = nullptr;   // Non-owning id<MTLDevice>.
  void* buffer_ = nullptr;         // Owned id<MTLBuffer>, MTLStorageModeShared.
  void* command_queue_ = nullptr;  // Owned id<MTLCommandQueue>.
  HostResourceMutationCallback host_resource_mutation_callback_;
  GpuResourceMutationCallback gpu_resource_mutation_callback_;
  std::deque<PendingUpload> pending_uploads_;
  size_t pending_upload_bytes_ = 0;
};

}  // namespace rex::graphics::metal
