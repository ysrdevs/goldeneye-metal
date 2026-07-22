#include <rex/perf/metal_performance.h>

#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>

namespace rex::perf {
namespace {

constexpr size_t kRecentFrameCapacity = 600;
constexpr size_t kRecentGpuCapacity = 240;
constexpr uint64_t kCompileHitchThresholdNs = UINT64_C(8) * 1000 * 1000;

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

struct CircularSamples {
  explicit CircularSamples(size_t capacity) : values(capacity) {}

  void Add(double value) {
    if (values.empty() || !std::isfinite(value) || value <= 0.0) {
      return;
    }
    values[next] = value;
    next = (next + 1) % values.size();
    count = std::min(count + 1, values.size());
  }

  std::vector<double> Copy() const {
    std::vector<double> result;
    result.reserve(count);
    const size_t start = count == values.size() ? next : 0;
    for (size_t index = 0; index < count; ++index) {
      result.push_back(values[(start + index) % values.size()]);
    }
    return result;
  }

  void Clear() {
    next = 0;
    count = 0;
  }

  std::vector<double> values;
  size_t next = 0;
  size_t count = 0;
};

struct ReportState {
  bool active = false;
  bool paused = false;
  uint64_t duration_ns = 0;
  uint64_t sampled_duration_ns = 0;
  uint64_t frames = 0;
  uint64_t shader_hitches = 0;
  uint64_t pipeline_hitches = 0;
  uint64_t cache_hits = 0;
  uint64_t cache_misses = 0;
  std::vector<double> frame_times_ms;
  std::vector<double> gpu_times_ms;
};

struct State {
  std::mutex mutex;
  CircularSamples recent_frame_times{kRecentFrameCapacity};
  CircularSamples recent_gpu_times{kRecentGpuCapacity};
  uint64_t last_frame_ns = 0;
  uint64_t presented_frames = 0;
  uint64_t gpu_samples = 0;
  uint64_t last_gpu_guest_frame_id = 0;
  uint64_t shader_compiles = 0;
  uint64_t shader_compile_hitches = 0;
  uint64_t pipeline_compiles = 0;
  uint64_t pipeline_compile_hitches = 0;
  uint64_t persistent_cache_hits = 0;
  uint64_t persistent_cache_misses = 0;
  ReportState report;
  std::string last_report;
};

State& GetState() {
  static State state;
  return state;
}

double Mean(const std::vector<double>& samples) {
  if (samples.empty()) {
    return 0.0;
  }
  return std::accumulate(samples.begin(), samples.end(), 0.0) / double(samples.size());
}

double Percentile(std::vector<double> samples, double percentile) {
  if (samples.empty()) {
    return 0.0;
  }
  std::sort(samples.begin(), samples.end());
  const double clamped = std::clamp(percentile, 0.0, 1.0);
  const size_t index = std::min(samples.size() - 1,
                                size_t(std::floor(clamped * double(samples.size() - 1))));
  return samples[index];
}

double OnePercentLowFps(std::vector<double> frame_times_ms) {
  if (frame_times_ms.empty()) {
    return 0.0;
  }
  std::sort(frame_times_ms.begin(), frame_times_ms.end(), std::greater<double>());
  const size_t worst_count = std::max<size_t>(1, (frame_times_ms.size() + 99) / 100);
  frame_times_ms.resize(worst_count);
  const double worst_mean = Mean(frame_times_ms);
  return worst_mean > 0.0 ? 1000.0 / worst_mean : 0.0;
}

std::string BuildReport(const State& state, uint64_t elapsed_ns) {
  const auto& report = state.report;
  const double elapsed_seconds = double(elapsed_ns) / 1.0e9;
  const double mean_frame_ms = Mean(report.frame_times_ms);
  const double average_fps = mean_frame_ms > 0.0 ? 1000.0 / mean_frame_ms : 0.0;
  const double low_fps = OnePercentLowFps(report.frame_times_ms);
  const double mean_gpu_ms = Mean(report.gpu_times_ms);
  const double p95_gpu_ms = Percentile(report.gpu_times_ms, 0.95);
  std::array<char, 512> text{};
  std::snprintf(
      text.data(), text.size(),
      "Metal performance report: duration=%.1fs frames=%llu avg_fps=%.2f 1%%_low_fps=%.2f "
      "avg_frame_ms=%.3f avg_present_gpu_ms=%.3f p95_present_gpu_ms=%.3f shader_hitches=%llu "
      "pipeline_hitches=%llu persistent_cache_hits=%llu persistent_cache_misses=%llu",
      elapsed_seconds, static_cast<unsigned long long>(report.frames), average_fps, low_fps,
      mean_frame_ms, mean_gpu_ms, p95_gpu_ms,
      static_cast<unsigned long long>(report.shader_hitches),
      static_cast<unsigned long long>(report.pipeline_hitches),
      static_cast<unsigned long long>(report.cache_hits),
      static_cast<unsigned long long>(report.cache_misses));
  return text.data();
}

}  // namespace

uint64_t RecordMetalGuestFrame(uint64_t timestamp_ns) {
  if (!timestamp_ns) {
    timestamp_ns = NowNs();
  }
  State& state = GetState();
  std::string completed_report;
  uint64_t guest_frame_id = 0;
  {
    std::lock_guard lock(state.mutex);
    if (state.last_frame_ns && timestamp_ns > state.last_frame_ns) {
      const uint64_t frame_duration_ns = timestamp_ns - state.last_frame_ns;
      const double frame_ms = double(frame_duration_ns) / 1.0e6;
      // Ignore suspend/debugger gaps, which are not useful render-performance
      // samples and would dominate 1% lows for minutes afterward.
      if (frame_ms <= 1000.0) {
        state.recent_frame_times.Add(frame_ms);
      }
      // A report is explicitly requested by the player and must include real
      // long stalls and very-low-FPS intervals. Its first interval is reset at
      // start, so time spent in the host menu before gameplay resumes is not
      // folded into this sample.
      if (state.report.active && !state.report.paused) {
        state.report.frame_times_ms.push_back(frame_ms);
        state.report.sampled_duration_ns += frame_duration_ns;
      }
    }
    state.last_frame_ns = timestamp_ns;
    guest_frame_id = ++state.presented_frames;
    if (state.report.active && !state.report.paused) {
      ++state.report.frames;
    }

    if (state.report.active &&
        state.report.sampled_duration_ns >= state.report.duration_ns) {
      const uint64_t elapsed_ns = state.report.sampled_duration_ns;
      completed_report = BuildReport(state, elapsed_ns);
      state.last_report = completed_report;
      state.report.active = false;
    }
  }
  if (!completed_report.empty()) {
    REXLOG_INFO("{}", completed_report);
  }
  return guest_frame_id;
}

void RecordMetalGpuTime(uint64_t duration_ns, uint64_t guest_frame_id) {
  if (!duration_ns) {
    return;
  }
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  if (guest_frame_id) {
    if (guest_frame_id <= state.last_gpu_guest_frame_id) {
      return;
    }
    state.last_gpu_guest_frame_id = guest_frame_id;
  }
  const double duration_ms = double(duration_ns) / 1.0e6;
  state.recent_gpu_times.Add(duration_ms);
  ++state.gpu_samples;
  if (state.report.active && !state.report.paused) {
    state.report.gpu_times_ms.push_back(duration_ms);
  }
}

void RecordMetalShaderCompile(uint64_t duration_ns, bool succeeded) {
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  ++state.shader_compiles;
  if (succeeded && duration_ns >= kCompileHitchThresholdNs) {
    ++state.shader_compile_hitches;
    if (state.report.active && !state.report.paused) {
      ++state.report.shader_hitches;
    }
  }
}

void RecordMetalPipelineCompile(uint64_t duration_ns, bool succeeded) {
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  ++state.pipeline_compiles;
  if (succeeded && duration_ns >= kCompileHitchThresholdNs) {
    ++state.pipeline_compile_hitches;
    if (state.report.active && !state.report.paused) {
      ++state.report.pipeline_hitches;
    }
  }
}

void RecordMetalPersistentCacheLookup(bool hit) {
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  if (hit) {
    ++state.persistent_cache_hits;
    if (state.report.active && !state.report.paused) {
      ++state.report.cache_hits;
    }
  } else {
    ++state.persistent_cache_misses;
    if (state.report.active && !state.report.paused) {
      ++state.report.cache_misses;
    }
  }
}

bool StartMetalPerformanceReport(uint32_t duration_seconds) {
  duration_seconds = std::clamp(duration_seconds, 5u, 600u);
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  state.report = {};
  state.report.active = true;
  state.report.duration_ns = uint64_t(duration_seconds) * UINT64_C(1000000000);
  // The report is normally started from a paused host menu. Ignore the gap
  // between the pre-menu frame and the first resumed frame rather than
  // counting menu time as gameplay.
  state.last_frame_ns = 0;
  state.last_gpu_guest_frame_id = state.presented_frames;
  state.report.frame_times_ms.reserve(size_t(duration_seconds) * 65);
  state.report.gpu_times_ms.reserve(size_t(duration_seconds) * 65);
  return true;
}

void PauseMetalPerformanceReport() {
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  if (state.report.active) {
    state.report.paused = true;
  }
}

void ResumeMetalPerformanceReport() {
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  if (!state.report.active || !state.report.paused) {
    return;
  }
  state.report.paused = false;
  // Exclude both the host-menu interval and any menu command buffers that
  // finish after gameplay resumes.
  state.last_frame_ns = 0;
  state.last_gpu_guest_frame_id = state.presented_frames;
}

void CancelMetalPerformanceReport() {
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  state.report.active = false;
  state.report.frame_times_ms.clear();
  state.report.gpu_times_ms.clear();
}

MetalPerformanceSnapshot GetMetalPerformanceSnapshot() {
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  MetalPerformanceSnapshot snapshot;
  const std::vector<double> frame_times = state.recent_frame_times.Copy();
  const std::vector<double> gpu_times = state.recent_gpu_times.Copy();
  snapshot.frame_time_ms = Mean(frame_times);
  snapshot.fps = snapshot.frame_time_ms > 0.0 ? 1000.0 / snapshot.frame_time_ms : 0.0;
  snapshot.one_percent_low_fps = OnePercentLowFps(frame_times);
  snapshot.gpu_time_ms = Mean(gpu_times);
  snapshot.presented_frames = state.presented_frames;
  snapshot.gpu_samples = state.gpu_samples;
  snapshot.shader_compiles = state.shader_compiles;
  snapshot.shader_compile_hitches = state.shader_compile_hitches;
  snapshot.pipeline_compiles = state.pipeline_compiles;
  snapshot.pipeline_compile_hitches = state.pipeline_compile_hitches;
  snapshot.persistent_cache_hits = state.persistent_cache_hits;
  snapshot.persistent_cache_misses = state.persistent_cache_misses;
  snapshot.report_active = state.report.active;
  snapshot.report_paused = state.report.paused;
  snapshot.last_report_available = !state.last_report.empty();
  if (state.report.active) {
    snapshot.report_elapsed_seconds =
        double(state.report.sampled_duration_ns) / 1.0e9;
    snapshot.report_duration_seconds = double(state.report.duration_ns) / 1.0e9;
  }
  return snapshot;
}

uint64_t GetMetalPresentedFrameCount() {
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  return state.presented_frames;
}

std::string GetLastMetalPerformanceReport() {
  State& state = GetState();
  std::lock_guard lock(state.mutex);
  return state.last_report;
}

}  // namespace rex::perf
