#pragma once

#include <cstdint>
#include <string>

namespace rex::perf {

// Thread-safe performance data shared by the Metal command processor,
// presenter, GoldenEye settings UI, and diagnostic reporting. Frame samples
// are guest frame arrivals (not display repaints), while GPU samples come from
// completed Metal command buffers.
struct MetalPerformanceSnapshot {
  double fps = 0.0;
  double frame_time_ms = 0.0;
  double one_percent_low_fps = 0.0;
  double gpu_time_ms = 0.0;
  double report_elapsed_seconds = 0.0;
  double report_duration_seconds = 0.0;
  uint64_t presented_frames = 0;
  uint64_t gpu_samples = 0;
  uint64_t shader_compiles = 0;
  uint64_t shader_compile_hitches = 0;
  uint64_t pipeline_compiles = 0;
  uint64_t pipeline_compile_hitches = 0;
  uint64_t persistent_cache_hits = 0;
  uint64_t persistent_cache_misses = 0;
  bool report_active = false;
  bool report_paused = false;
  bool last_report_available = false;
};

// Returns the monotonically increasing ID assigned to this guest frame. The
// presenter carries this ID alongside the exact mailbox or CPU image it draws
// so asynchronous GPU completion samples can't be attributed to a newer
// producer frame.
uint64_t RecordMetalGuestFrame(uint64_t timestamp_ns = 0);
// guest_frame_id deduplicates display repaints of the same guest image (for
// example while the host menu is open). Pass 0 for a standalone sample.
void RecordMetalGpuTime(uint64_t duration_ns, uint64_t guest_frame_id = 0);
void RecordMetalShaderCompile(uint64_t duration_ns, bool succeeded);
void RecordMetalPipelineCompile(uint64_t duration_ns, bool succeeded);
void RecordMetalPersistentCacheLookup(bool hit);

// Starts a fresh bounded capture. Completion is automatic once enough guest
// frame time has elapsed; the final one-line summary is also written to the
// normal runtime log, so it is included by the launcher diagnostic exporter.
bool StartMetalPerformanceReport(uint32_t duration_seconds = 60);
void PauseMetalPerformanceReport();
void ResumeMetalPerformanceReport();
void CancelMetalPerformanceReport();

MetalPerformanceSnapshot GetMetalPerformanceSnapshot();
uint64_t GetMetalPresentedFrameCount();
std::string GetLastMetalPerformanceReport();

}  // namespace rex::perf
