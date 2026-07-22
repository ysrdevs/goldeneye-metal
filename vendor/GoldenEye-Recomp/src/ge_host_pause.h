#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

struct PPCContext;

namespace ge::host_pause {

// GoldenEye treats the retail word as a boolean everywhere in the supported
// build. A distinct nonzero token proves host ownership. While it is present,
// the strong retail setter wrapper records GoldenEye's latest 0/1 intent and
// keeps the token asserted; closing host settings restores that recorded value.
inline constexpr uint32_t kHostPauseToken = 0x47454850u;  // "GEHP"

inline constexpr bool IsEligibleLocalMission(int32_t level, int32_t players,
                                             bool network_session) noexcept {
  return level > 0 && level < 90 && players >= 1 && players <= 4 && !network_session;
}

enum class Action : uint8_t {
  kNone,
  kAcquire,
  kRelease,
  kRelinquish,
};

struct Snapshot {
  bool requested = false;
  bool request_applied = true;
  bool available = false;
  bool gameplay_paused = false;
  bool host_owned = false;
  uint64_t generation = 0;
  uint64_t applied_generation = 0;
};

struct ProcessResult {
  Action action = Action::kNone;
  bool action_succeeded = true;
  bool request_applied = false;
  bool gameplay_paused = false;
  bool host_owned = false;
  bool input_resume_pulse = false;
  uint64_t generation = 0;
  uint32_t pause_value = 0;
};

// Small game-thread latch used after the pause is actually released. It waits
// for complete neutral input rather than only the controller button that closed
// the host menu, covering mouse, keyboard, triggers and either stick. Two
// consecutive neutral polls are required and the final neutral poll is still
// swallowed, so host input cannot leak across the menu-to-game transition.
struct InputSample {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t thumb_lx = 0;
  int16_t thumb_ly = 0;
  int16_t thumb_rx = 0;
  int16_t thumb_ry = 0;
};

class ResumeInputLatch {
 public:
  static constexpr uint8_t kTriggerThreshold = 30;
  // Dear ImGui's standard gamepad navigation starts immediately above 20%.
  // Keep the close-frame latch on that same raw-axis boundary so a stick that
  // can navigate the host menu cannot leak into gameplay on the next poll.
  static constexpr int16_t kStickDeadzone = 6553;
  static constexpr uint32_t kRequiredNeutralPolls = 2;
  static constexpr uint32_t kMaximumSuppressedPolls = 120;

  void Arm(const InputSample& /*sample*/) noexcept {
    suppressed_polls_ = 0;
    neutral_polls_ = 0;
    active_ = true;
  }

  bool active() const noexcept { return active_; }

  bool ShouldSuppress(const InputSample& sample) noexcept {
    if (!active_) {
      return false;
    }

    ++suppressed_polls_;
    if (IsNeutral(sample)) {
      ++neutral_polls_;
    } else {
      neutral_polls_ = 0;
    }
    if (neutral_polls_ >= kRequiredNeutralPolls || suppressed_polls_ >= kMaximumSuppressedPolls) {
      active_ = false;
    }

    // The poll that observes release is neutral. A fresh press on the next
    // poll is the first input allowed back into GoldenEye.
    return true;
  }

 private:
  static bool IsNeutral(const InputSample& sample) noexcept {
    return sample.buttons == 0 && sample.left_trigger < kTriggerThreshold &&
           sample.right_trigger < kTriggerThreshold && sample.thumb_lx <= kStickDeadzone &&
           sample.thumb_lx >= -kStickDeadzone && sample.thumb_ly <= kStickDeadzone &&
           sample.thumb_ly >= -kStickDeadzone && sample.thumb_rx <= kStickDeadzone &&
           sample.thumb_rx >= -kStickDeadzone && sample.thumb_ry <= kStickDeadzone &&
           sample.thumb_ry >= -kStickDeadzone;
  }

  uint32_t suppressed_polls_ = 0;
  uint32_t neutral_polls_ = 0;
  bool active_ = false;
};

namespace detail {

struct RetryLogDecision {
  bool warn = false;
  bool recovered = false;
  uint32_t failed_attempts = 0;
};

// Game-thread-only warning gate. A retry episode is identified by the exact
// request generation and action. Cancellation, eligibility loss, or a newer
// request clears the old episode silently; only a matching successful action
// is reported as recovery.
class RetryLogGate {
 public:
  RetryLogDecision Observe(const ProcessResult& result) noexcept {
    const bool retryable_action =
        result.action == Action::kAcquire || result.action == Action::kRelease;
    if (!result.action_succeeded && retryable_action) {
      if (!active_ || generation_ != result.generation || action_ != result.action) {
        active_ = true;
        generation_ = result.generation;
        action_ = result.action;
        failed_attempts_ = 1;
        return {.warn = true, .failed_attempts = failed_attempts_};
      }
      ++failed_attempts_;
      return {.failed_attempts = failed_attempts_};
    }

    RetryLogDecision decision;
    if (active_ && result.action_succeeded && result.generation == generation_ &&
        result.action == action_) {
      decision.recovered = true;
      decision.failed_attempts = failed_attempts_;
    }
    Reset();
    return decision;
  }

  bool active() const noexcept { return active_; }

 private:
  void Reset() noexcept {
    active_ = false;
    generation_ = 0;
    action_ = Action::kNone;
    failed_attempts_ = 0;
  }

  bool active_ = false;
  uint64_t generation_ = 0;
  Action action_ = Action::kNone;
  uint32_t failed_attempts_ = 0;
};

}  // namespace detail

// Thread-safe request bridge between the host UI and GoldenEye's game thread.
// Only ProcessGameThread (or a test standing in for it) may call Process.
class State {
 public:
  bool RequestPaused(bool paused) noexcept {
    std::lock_guard lock(operation_mutex_);
    const uint64_t current = request_.load(std::memory_order_acquire);
    if ((current & 1u) == (paused ? 1u : 0u)) {
      return false;
    }
    const uint64_t next = (((current >> 1) + 1) << 1) | (paused ? 1u : 0u);
    request_.store(next, std::memory_order_release);
    return true;
  }

  Snapshot GetSnapshot() const noexcept {
    const uint64_t request = request_.load(std::memory_order_acquire);
    const uint64_t applied = applied_request_.load(std::memory_order_acquire);
    const uint8_t status = status_.load(std::memory_order_acquire);
    return {
        .requested = (request & 1u) != 0,
        .request_applied = request == applied,
        .available = (status & kAvailable) != 0,
        .gameplay_paused = (status & kGameplayPaused) != 0,
        .host_owned = (status & kHostOwned) != 0,
        .generation = request >> 1,
        .applied_generation = applied >> 1,
    };
  }

  // Called only by the strong retail setter wrapper while the pause-word
  // transaction mutex is held. GoldenEye may continue writing its normal 0/1
  // state behind host settings; remember the latest retail intent while
  // keeping the host token continuously asserted. On host release Process
  // restores that remembered value instead of blindly clearing a retail pause.
  uint32_t FilterRetailWrite(uint32_t requested_value) noexcept {
    std::lock_guard lock(operation_mutex_);
    if (owns_pause_) {
      retail_restore_value_ = requested_value != 0 ? 1u : 0u;
      return kHostPauseToken;
    }
    return requested_value;
  }

  // Handles the common no-request/no-ownership case without touching guest
  // state. The same mutex used by RequestPaused makes the acknowledgement and
  // all retail setter actions generation-consistent.
  bool TryProcessIdle(ProcessResult* out_result) noexcept {
    std::lock_guard lock(operation_mutex_);
    const uint64_t request = request_.load(std::memory_order_acquire);
    if ((request & 1u) != 0 || owns_pause_) {
      return false;
    }
    const bool pending = request != applied_request_.load(std::memory_order_acquire);
    Publish(request, false, false, false, true);
    if (out_result) {
      *out_result = {
          .request_applied = true,
          .input_resume_pulse = pending,
          .generation = request >> 1,
      };
    }
    return true;
  }

  // Runs on the guest game thread. Zero means running, kHostPauseToken means
  // this bridge owns the pause, and every other nonzero value is retail-owned.
  // The host replaces the word only while serialized with the strong retail
  // setter wrapper, and restores the title's latest recorded boolean on close.
  template <typename QueryPauseValue, typename SetPauseValue>
  ProcessResult Process(bool eligible, QueryPauseValue&& query_pause_value,
                        SetPauseValue&& set_pause_value) noexcept {
    std::lock_guard lock(operation_mutex_);
    const uint64_t request = request_.load(std::memory_order_acquire);
    const bool desired = (request & 1u) != 0;
    const bool pending = request != applied_request_.load(std::memory_order_acquire);
    Action action = Action::kNone;
    bool action_succeeded = true;
    uint32_t pause_value = 0;

    if (!desired && !owns_pause_) {
      Publish(request, false, false, false, true);
      return {
          .request_applied = true,
          .input_resume_pulse = pending,
          .generation = request >> 1,
      };
    }

    if (desired && !eligible && !owns_pause_) {
      Publish(request, false, false, false, true);
      return {
          .request_applied = true,
          .generation = request >> 1,
      };
    }

    pause_value = query_pause_value();

    if (owns_pause_ && (!desired || !eligible)) {
      if (pause_value == kHostPauseToken) {
        action = Action::kRelease;
        set_pause_value(retail_restore_value_);
        pause_value = query_pause_value();
        if (pause_value == kHostPauseToken) {
          action_succeeded = false;
        } else {
          owns_pause_ = false;
          if (pause_value != retail_restore_value_ && pause_value != 0) {
            action = Action::kRelinquish;
          }
        }
      } else {
        // A later retail write replaced our token. It is now the game's pause,
        // so relinquish ownership without ever clearing the word.
        owns_pause_ = false;
        if (pause_value != 0) {
          action = Action::kRelinquish;
        }
      }
    } else if (desired && eligible) {
      if (!owns_pause_ || pause_value != kHostPauseToken) {
        // Save the title's current boolean pause before replacing it with the
        // host token. A direct/unwrapped write observed while already owning is
        // treated as updated retail intent and repaired immediately.
        retail_restore_value_ = pause_value != 0 ? 1u : 0u;
        action = Action::kAcquire;
        set_pause_value(kHostPauseToken);
        pause_value = query_pause_value();
        if (pause_value == kHostPauseToken) {
          owns_pause_ = true;
        } else if (pause_value != 0) {
          // Retail won a same-poll race with acquisition. The requested pause
          // is satisfied, but the host must not claim or later clear it.
          owns_pause_ = false;
          action = Action::kRelinquish;
        } else {
          action_succeeded = false;
        }
      }
    }

    const bool gameplay_paused = desired && eligible && pause_value != 0;
    const bool complete = desired ? (eligible ? pause_value != 0 : !owns_pause_) : !owns_pause_;
    Publish(request, eligible, gameplay_paused, owns_pause_, complete);
    return {
        .action = action,
        .action_succeeded = action_succeeded,
        .request_applied = complete,
        .gameplay_paused = gameplay_paused,
        .host_owned = owns_pause_,
        .input_resume_pulse = pending && !desired && complete,
        .generation = request >> 1,
        .pause_value = pause_value,
    };
  }

 private:
  static constexpr uint8_t kAvailable = 1u << 0;
  static constexpr uint8_t kGameplayPaused = 1u << 1;
  static constexpr uint8_t kHostOwned = 1u << 2;

  void Publish(uint64_t request, bool available, bool gameplay_paused, bool host_owned,
               bool applied) noexcept {
    uint8_t status = 0;
    if (available) {
      status |= kAvailable;
    }
    if (gameplay_paused) {
      status |= kGameplayPaused;
    }
    if (host_owned) {
      status |= kHostOwned;
    }
    status_.store(status, std::memory_order_release);
    if (applied) {
      applied_request_.store(request, std::memory_order_release);
    }
  }

  // Bit 0 is desired pause state; upper bits are the request generation.
  std::atomic<uint64_t> request_{0};
  std::atomic<uint64_t> applied_request_{0};
  std::atomic<uint8_t> status_{0};
  std::mutex operation_mutex_;
  bool owns_pause_ = false;  // game-thread-only, serialized with request changes
  uint32_t retail_restore_value_ = 0;
};

// Host/UI thread API.
void RequestPaused(bool paused) noexcept;
Snapshot GetSnapshot() noexcept;

// GoldenEye game-thread bridge. Called from ge_inject_keyboard before other
// host requests and never directly from the host render/UI thread.
ProcessResult ProcessGameThread(PPCContext& context, uint8_t* base) noexcept;

}  // namespace ge::host_pause
