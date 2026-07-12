#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace rex::graphics::metal::profiling {

constexpr uint32_t kReportInterval = 64;

inline bool IsEnabled() {
  static const bool enabled = []() {
    const char* value = std::getenv("GOLDENEYE_METAL_PROFILE");
    return value && std::strcmp(value, "1") == 0;
  }();
  return enabled;
}

inline uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

inline uint64_t ElapsedNs(uint64_t start_ns) {
  uint64_t end_ns = NowNs();
  return end_ns >= start_ns ? end_ns - start_ns : 0;
}

struct DurationAggregate {
  uint64_t call_count = 0;
  uint64_t total_ns = 0;
  uint64_t max_call_ns = 0;

  void Add(uint64_t duration_ns) {
    ++call_count;
    total_ns += duration_ns;
    max_call_ns = std::max(max_call_ns, duration_ns);
  }
};

struct DurationWindow {
  DurationAggregate total;
  uint64_t max_swap_ns = 0;
  uint64_t max_calls_per_swap = 0;

  void Add(uint64_t duration_ns) {
    total.Add(duration_ns);
    current_swap_ns_ += duration_ns;
    ++current_swap_calls_;
  }

  void EndSwap() {
    max_swap_ns = std::max(max_swap_ns, current_swap_ns_);
    max_calls_per_swap = std::max(max_calls_per_swap, current_swap_calls_);
    current_swap_ns_ = 0;
    current_swap_calls_ = 0;
  }

  void Reset() { *this = {}; }

 private:
  uint64_t current_swap_ns_ = 0;
  uint64_t current_swap_calls_ = 0;
};

enum class CommandEvent : uint8_t {
  kIssueDraw,
  kIssueCopy,
  kIssueSwap,
  kTextureFallbackDecode,
  kWaitRegMem,
  kCount,
};

constexpr const char* CommandEventName(CommandEvent event) {
  switch (event) {
    case CommandEvent::kIssueDraw:
      return "draw";
    case CommandEvent::kIssueCopy:
      return "copy";
    case CommandEvent::kIssueSwap:
      return "swap";
    case CommandEvent::kTextureFallbackDecode:
      return "texture_fallback_decode";
    case CommandEvent::kWaitRegMem:
      return "wait_reg_mem";
    default:
      return "unknown";
  }
}

enum class WaitReason : uint8_t {
  kSwap,
  kResourceMutation,
  kClearCaches,
  kShutdown,
  kMemExport,
  kResolveBufferWriter,
  kSharedMemoryWriter,
  kGlobalCap,
  kOther,
  kCount,
};

inline WaitReason GetWaitReason(const char* reason) {
  if (!reason) {
    return WaitReason::kOther;
  }
  if (std::strcmp(reason, "swap") == 0) {
    return WaitReason::kSwap;
  }
  if (std::strcmp(reason, "resource-mutation") == 0) {
    return WaitReason::kResourceMutation;
  }
  if (std::strcmp(reason, "clear-caches") == 0) {
    return WaitReason::kClearCaches;
  }
  if (std::strcmp(reason, "shutdown") == 0) {
    return WaitReason::kShutdown;
  }
  if (std::strcmp(reason, "memexport") == 0) {
    return WaitReason::kMemExport;
  }
  if (std::strcmp(reason, "resolve-buffer-writer") == 0) {
    return WaitReason::kResolveBufferWriter;
  }
  if (std::strcmp(reason, "shared-memory-writer") == 0) {
    return WaitReason::kSharedMemoryWriter;
  }
  if (std::strcmp(reason, "global-cap") == 0) {
    return WaitReason::kGlobalCap;
  }
  return WaitReason::kOther;
}

constexpr const char* WaitReasonName(WaitReason reason) {
  switch (reason) {
    case WaitReason::kSwap:
      return "swap";
    case WaitReason::kResourceMutation:
      return "resource-mutation";
    case WaitReason::kClearCaches:
      return "clear-caches";
    case WaitReason::kShutdown:
      return "shutdown";
    case WaitReason::kMemExport:
      return "memexport";
    case WaitReason::kResolveBufferWriter:
      return "resolve-buffer-writer";
    case WaitReason::kSharedMemoryWriter:
      return "shared-memory-writer";
    case WaitReason::kGlobalCap:
      return "global-cap";
    default:
      return "other";
  }
}

struct WaitWindow {
  DurationWindow duration;
  uint64_t waited_call_count = 0;
  uint64_t waited_submission_count = 0;
  uint64_t max_waited_submissions_per_call = 0;
  uint64_t max_waited_submissions_per_swap = 0;

  void Add(uint64_t duration_ns, uint64_t waited_submissions) {
    duration.Add(duration_ns);
    if (waited_submissions) {
      ++waited_call_count;
    }
    waited_submission_count += waited_submissions;
    max_waited_submissions_per_call = std::max(max_waited_submissions_per_call, waited_submissions);
    current_swap_waited_submissions_ += waited_submissions;
  }

  void EndSwap() {
    duration.EndSwap();
    max_waited_submissions_per_swap =
        std::max(max_waited_submissions_per_swap, current_swap_waited_submissions_);
    current_swap_waited_submissions_ = 0;
  }

  void Reset() { *this = {}; }

 private:
  uint64_t current_swap_waited_submissions_ = 0;
};

class CommandProfileWindow {
 public:
  void Record(CommandEvent event, uint64_t duration_ns) { events_[size_t(event)].Add(duration_ns); }

  void RecordWait(WaitReason reason, uint64_t duration_ns, uint64_t waited_submissions) {
    waits_[size_t(reason)].Add(duration_ns, waited_submissions);
  }

  bool EndSwap() {
    for (DurationWindow& event : events_) {
      event.EndSwap();
    }
    for (WaitWindow& wait : waits_) {
      wait.EndSwap();
    }
    ++swap_count_;
    return swap_count_ == kReportInterval;
  }

  const DurationWindow& event(CommandEvent event) const { return events_[size_t(event)]; }
  const WaitWindow& wait(WaitReason reason) const { return waits_[size_t(reason)]; }
  uint32_t swap_count() const { return swap_count_; }

  void Reset() { *this = {}; }

 private:
  std::array<DurationWindow, size_t(CommandEvent::kCount)> events_;
  std::array<WaitWindow, size_t(WaitReason::kCount)> waits_;
  uint32_t swap_count_ = 0;
};

class ScopedCommandEvent {
 public:
  ScopedCommandEvent(CommandProfileWindow* window, CommandEvent event)
      : window_(window), event_(event), start_ns_(window ? NowNs() : 0) {}
  ~ScopedCommandEvent() {
    if (window_) {
      window_->Record(event_, ElapsedNs(start_ns_));
    }
  }

  ScopedCommandEvent(const ScopedCommandEvent&) = delete;
  ScopedCommandEvent& operator=(const ScopedCommandEvent&) = delete;

 private:
  CommandProfileWindow* window_;
  CommandEvent event_;
  uint64_t start_ns_;
};

enum class PresenterEvent : uint8_t {
  kNextDrawable,
  kUpload,
  kPresentCommit,
  kCount,
};

constexpr const char* PresenterEventName(PresenterEvent event) {
  switch (event) {
    case PresenterEvent::kNextDrawable:
      return "next_drawable";
    case PresenterEvent::kUpload:
      return "upload";
    case PresenterEvent::kPresentCommit:
      return "present_commit";
    default:
      return "unknown";
  }
}

class PresenterProfileWindow {
 public:
  void Record(PresenterEvent event, uint64_t duration_ns) {
    events_[size_t(event)].Add(duration_ns);
  }

  void RecordSource(bool unchanged) {
    ++source_count_;
    unchanged_source_count_ += unchanged ? 1 : 0;
  }

  void RecordDrawableNil() { ++drawable_nil_count_; }

  void RecordUpload(uint64_t byte_count) {
    ++upload_count_;
    upload_byte_count_ += byte_count;
  }

  void RecordCommit() { ++commit_count_; }

  bool EndAttempt() {
    for (DurationWindow& event : events_) {
      event.EndSwap();
    }
    ++attempt_count_;
    return attempt_count_ == kReportInterval;
  }

  const DurationWindow& event(PresenterEvent event) const { return events_[size_t(event)]; }
  uint64_t source_count() const { return source_count_; }
  uint64_t unchanged_source_count() const { return unchanged_source_count_; }
  uint64_t drawable_nil_count() const { return drawable_nil_count_; }
  uint64_t upload_count() const { return upload_count_; }
  uint64_t upload_byte_count() const { return upload_byte_count_; }
  uint64_t commit_count() const { return commit_count_; }
  uint32_t attempt_count() const { return attempt_count_; }

  void Reset() { *this = {}; }

 private:
  std::array<DurationWindow, size_t(PresenterEvent::kCount)> events_;
  uint64_t source_count_ = 0;
  uint64_t unchanged_source_count_ = 0;
  uint64_t drawable_nil_count_ = 0;
  uint64_t upload_count_ = 0;
  uint64_t upload_byte_count_ = 0;
  uint64_t commit_count_ = 0;
  uint32_t attempt_count_ = 0;
};

}  // namespace rex::graphics::metal::profiling
