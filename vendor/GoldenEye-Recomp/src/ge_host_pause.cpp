#include "ge_host_pause.h"

#include "ge_init.h"

#include <rex/logging.h>

namespace ge::host_pause {
namespace {

constexpr uint32_t kNetworkSessionFlagAddress = 0x830CAEA0u;
State g_state;
std::mutex g_pause_word_mutex;

class ScopedContextRestore {
 public:
  explicit ScopedContextRestore(PPCContext& context) : context_(context), saved_(context) {}

  ~ScopedContextRestore() {
    const uint32_t saved_fpscr = saved_.fpscr.csr;
    context_ = saved_;
    context_.fpscr.setcsr(saved_fpscr);
  }

 private:
  PPCContext& context_;
  PPCContext saved_;
};

bool ActiveLocalMission(PPCContext& context, uint8_t* base, int32_t* level_out,
                        int32_t* players_out) {
  sub_820AE360(context, base);
  const int32_t level = context.r3.s32;
  sub_820C9AF0(context, base);
  const int32_t players = context.r3.s32;
  if (level_out) {
    *level_out = level;
  }
  if (players_out) {
    *players_out = players;
  }
  return IsEligibleLocalMission(level, players, base[kNetworkSessionFlagAddress] != 0);
}

uint32_t QueryRetailPauseValue(PPCContext& context, uint8_t* base) {
  sub_8209F588(context, base);
  return context.r3.u32;
}

void SetRetailPauseValue(PPCContext& context, uint8_t* base, uint32_t value) {
  context.r3.u64 = value;
  // Host ownership changes bypass the strong retail wrapper below. The whole
  // State::Process transaction holds g_pause_word_mutex, so its compare/write/
  // verify sequence is atomic with respect to every retail setter call.
  __imp__sub_8209F578(context, base);
}

detail::RetryLogGate g_failure_log;

void LogProcessResult(const ProcessResult& result, int32_t level, int32_t players) {
  const detail::RetryLogDecision retry = g_failure_log.Observe(result);
  if (retry.warn) {
    REXLOG_WARN("GEPAUSE retail {} request was not accepted; retrying",
                result.action == Action::kAcquire ? "pause" : "resume");
  }
  if (!result.action_succeeded) {
    return;
  }

  if (retry.recovered) {
    REXLOG_INFO("GEPAUSE retail {} request succeeded after {} failed attempt{}",
                result.action == Action::kAcquire ? "pause" : "resume", retry.failed_attempts,
                retry.failed_attempts == 1 ? "" : "s");
  }

  if (result.action == Action::kAcquire) {
    REXLOG_INFO("GEPAUSE retail pause acquired (level={}, players={})", level, players);
  } else if (result.action == Action::kRelease) {
    REXLOG_INFO("GEPAUSE retail pause released");
  } else if (result.action == Action::kRelinquish) {
    REXLOG_INFO("GEPAUSE retail pause ownership preserved for the game");
  }
}

}  // namespace

void ApplyRetailPauseWrite(PPCContext& context, uint8_t* base) noexcept {
  std::lock_guard lock(g_pause_word_mutex);
  const PPCRegister requested = context.r3;
  const uint32_t filtered_value = g_state.FilterRetailWrite(requested.u32);

  // Run the original tiny setter even for a shadowed retail write, preserving
  // its r11/memory behavior while reasserting the existing host token.
  context.r3.u64 = filtered_value;
  __imp__sub_8209F578(context, base);
  context.r3 = requested;
}

void RequestPaused(bool paused) noexcept {
  if (g_state.RequestPaused(paused)) {
    REXLOG_INFO("GEPAUSE host settings requested {}", paused ? "pause" : "resume");
  }
}

Snapshot GetSnapshot() noexcept {
  return g_state.GetSnapshot();
}

ProcessResult ProcessGameThread(PPCContext& context, uint8_t* base) noexcept {
  // A false request with no owned retail pause can be acknowledged without
  // touching guest state. This is the normal startup and shutdown fast path.
  ProcessResult idle_result;
  if (g_state.TryProcessIdle(&idle_result)) {
    // An idle result may be a close that canceled an unsuccessful acquisition.
    // Feed it through the gate so that stale retry state cannot be attributed
    // to a later menu generation.
    LogProcessResult(idle_result, 0, 0);
    return idle_result;
  }
  if (!base) {
    return {};
  }

  const Snapshot before = g_state.GetSnapshot();
  ScopedContextRestore restore(context);
  int32_t level = 0;
  int32_t players = 0;
  const bool eligible = ActiveLocalMission(context, base, &level, &players);
  ProcessResult result;
  {
    std::lock_guard pause_word_lock(g_pause_word_mutex);
    result = g_state.Process(
        eligible, [&] { return QueryRetailPauseValue(context, base); },
        [&](uint32_t value) { SetRetailPauseValue(context, base, value); });
  }

  LogProcessResult(result, level, players);
  if (before.requested && !eligible && !before.request_applied) {
    REXLOG_INFO(
        "GEPAUSE host settings opened outside an active local mission "
        "(level={}, players={})",
        level, players);
  }
  return result;
}

}  // namespace ge::host_pause

// Strong override for the generated weak retail setter. All original guest
// call sites land here; host writes call __imp__sub_8209F578 directly.
extern "C" void sub_8209F578(PPCContext& context, uint8_t* base) {
  ge::host_pause::ApplyRetailPauseWrite(context, base);
}
