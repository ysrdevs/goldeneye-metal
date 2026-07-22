#include "ge_host_pause.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

namespace {

int failures = 0;

#define CHECK_TRUE(expression)                                                            \
  do {                                                                                    \
    if (!(expression)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expression); \
      ++failures;                                                                         \
    }                                                                                     \
  } while (false)

struct RetailPause {
  uint32_t value = 0;
  bool accept_writes = true;
  int set_host_count = 0;
  int set_zero_count = 0;
  std::optional<uint32_t> value_after_next_write;

  uint32_t Query() const { return value; }
  void Set(uint32_t new_value) {
    if (new_value == ge::host_pause::kHostPauseToken) {
      ++set_host_count;
    } else if (new_value == 0) {
      ++set_zero_count;
    }
    if (!accept_writes) {
      return;
    }
    value = value_after_next_write.value_or(new_value);
    value_after_next_write.reset();
  }
};

ge::host_pause::ProcessResult Process(ge::host_pause::State& state, bool eligible,
                                      RetailPause& retail) {
  return state.Process(
      eligible, [&] { return retail.Query(); }, [&](uint32_t value) { retail.Set(value); });
}

void TestLocalMissionEligibility() {
  using ge::host_pause::IsEligibleLocalMission;
  CHECK_TRUE(IsEligibleLocalMission(1, 1, false));
  CHECK_TRUE(IsEligibleLocalMission(89, 4, false));
  CHECK_TRUE(!IsEligibleLocalMission(0, 1, false));
  CHECK_TRUE(!IsEligibleLocalMission(90, 1, false));
  CHECK_TRUE(!IsEligibleLocalMission(1, 0, false));
  CHECK_TRUE(!IsEligibleLocalMission(1, 5, false));
  CHECK_TRUE(!IsEligibleLocalMission(1, 1, true));
  CHECK_TRUE(!IsEligibleLocalMission(89, 4, true));
}

void TestAcquireReleaseAndIdempotence() {
  ge::host_pause::State state;
  RetailPause retail;

  CHECK_TRUE(state.RequestPaused(true));
  CHECK_TRUE(!state.RequestPaused(true));
  auto result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kAcquire);
  CHECK_TRUE(result.action_succeeded);
  CHECK_TRUE(retail.value == ge::host_pause::kHostPauseToken);
  CHECK_TRUE(retail.set_host_count == 1);
  CHECK_TRUE(state.GetSnapshot().gameplay_paused);

  result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kNone);
  CHECK_TRUE(retail.set_host_count == 1);

  CHECK_TRUE(state.RequestPaused(false));
  CHECK_TRUE(!state.RequestPaused(false));
  result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kRelease);
  CHECK_TRUE(result.action_succeeded);
  CHECK_TRUE(retail.value == 0);
  CHECK_TRUE(retail.set_zero_count == 1);
  CHECK_TRUE(result.input_resume_pulse);
  CHECK_TRUE(state.GetSnapshot().request_applied);

  CHECK_TRUE(state.TryProcessIdle(&result));
  CHECK_TRUE(!result.input_resume_pulse);
}

void TestPreexistingRetailPauseIsRestored() {
  ge::host_pause::State state;
  RetailPause retail{.value = 1};

  state.RequestPaused(true);
  auto result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kAcquire);
  CHECK_TRUE(result.host_owned);
  CHECK_TRUE(result.gameplay_paused);
  CHECK_TRUE(retail.value == ge::host_pause::kHostPauseToken);
  CHECK_TRUE(retail.set_host_count == 1);

  state.RequestPaused(false);
  result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kRelease);
  CHECK_TRUE(retail.value == 1);
  CHECK_TRUE(retail.set_zero_count == 0);
  CHECK_TRUE(result.input_resume_pulse);
}

void TestLaterRetailIntentIsPreserved() {
  ge::host_pause::State state;
  RetailPause retail;

  state.RequestPaused(true);
  auto result = Process(state, true, retail);
  CHECK_TRUE(result.host_owned);

  // GoldenEye asserts its own boolean pause after host acquisition. The strong
  // setter records that intent but keeps the host token continuously asserted.
  retail.value = state.FilterRetailWrite(1);
  result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kNone);
  CHECK_TRUE(result.host_owned);
  CHECK_TRUE(result.gameplay_paused);
  CHECK_TRUE(retail.value == ge::host_pause::kHostPauseToken);

  state.RequestPaused(false);
  result = Process(state, true, retail);
  CHECK_TRUE(retail.value == 1);
  CHECK_TRUE(retail.set_zero_count == 0);
  CHECK_TRUE(result.input_resume_pulse);
}

void TestRetailWritesStayPausedAndEligibilityLossIsSafe() {
  ge::host_pause::State state;
  RetailPause retail;

  state.RequestPaused(true);
  Process(state, true, retail);
  retail.value = state.FilterRetailWrite(1);
  CHECK_TRUE(retail.value == ge::host_pause::kHostPauseToken);
  retail.value = state.FilterRetailWrite(0);
  CHECK_TRUE(retail.value == ge::host_pause::kHostPauseToken);
  Process(state, true, retail);
  auto result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kNone);
  CHECK_TRUE(result.host_owned);
  CHECK_TRUE(retail.value == ge::host_pause::kHostPauseToken);

  result = Process(state, false, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kRelease);
  CHECK_TRUE(!result.host_owned);
  CHECK_TRUE(retail.value == 0);

  ge::host_pause::State foreign_state;
  RetailPause foreign_retail;
  foreign_state.RequestPaused(true);
  Process(foreign_state, true, foreign_retail);
  foreign_retail.value = foreign_state.FilterRetailWrite(1);
  result = Process(foreign_state, false, foreign_retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kRelease);
  CHECK_TRUE(!result.host_owned);
  CHECK_TRUE(foreign_retail.value == 1);
}

void TestUnavailableRapidCancellationAndResumePulse() {
  ge::host_pause::State state;
  RetailPause retail;

  state.RequestPaused(true);
  auto result = Process(state, false, retail);
  CHECK_TRUE(result.request_applied);
  const auto unavailable = state.GetSnapshot();
  CHECK_TRUE(!unavailable.available);
  CHECK_TRUE(unavailable.generation == unavailable.applied_generation);
  CHECK_TRUE(retail.value == 0);

  // Eligibility may appear without a new UI request (for example the mission
  // finishes loading while settings is already open). The still-true request
  // must acquire even though its earlier unavailable poll was acknowledged.
  result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kAcquire);
  CHECK_TRUE(result.host_owned);
  CHECK_TRUE(retail.value == ge::host_pause::kHostPauseToken);

  state.RequestPaused(false);
  result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kRelease);
  CHECK_TRUE(result.input_resume_pulse);
  CHECK_TRUE(state.TryProcessIdle(&result));
  CHECK_TRUE(!result.input_resume_pulse);

  state.RequestPaused(true);
  state.RequestPaused(false);
  state.RequestPaused(true);
  result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kAcquire);
  CHECK_TRUE(!result.input_resume_pulse);
}

void TestFailedWritesRetryAndReleaseRace() {
  ge::host_pause::State state;
  RetailPause retail{.accept_writes = false};

  state.RequestPaused(true);
  auto result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kAcquire);
  CHECK_TRUE(!result.action_succeeded);
  CHECK_TRUE(!result.request_applied);

  retail.accept_writes = true;
  result = Process(state, true, retail);
  CHECK_TRUE(result.action_succeeded);
  CHECK_TRUE(result.host_owned);

  state.RequestPaused(false);
  retail.accept_writes = false;
  result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kRelease);
  CHECK_TRUE(!result.action_succeeded);
  CHECK_TRUE(!result.input_resume_pulse);

  // A same-poll retail assertion wins over the host's zero write. Treat it as
  // successful ownership transfer and preserve the resulting retail pause.
  retail.accept_writes = true;
  retail.value_after_next_write = 1;
  result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kRelinquish);
  CHECK_TRUE(result.action_succeeded);
  CHECK_TRUE(!result.host_owned);
  CHECK_TRUE(retail.value == 1);
  CHECK_TRUE(result.input_resume_pulse);
}

void TestRetailPauseWriteShadow() {
  ge::host_pause::State state;
  RetailPause retail;

  // Without host ownership, retail writes pass through unchanged.
  CHECK_TRUE(state.FilterRetailWrite(1) == 1);
  CHECK_TRUE(state.FilterRetailWrite(0) == 0);

  state.RequestPaused(true);
  auto result = Process(state, true, retail);
  CHECK_TRUE(result.host_owned);
  CHECK_TRUE(retail.value == ge::host_pause::kHostPauseToken);

  // Both sides of a retail 1 -> 0 transition are shadowed without exposing a
  // running frame. The latest retail intent is restored when the host closes.
  retail.value = state.FilterRetailWrite(1);
  CHECK_TRUE(retail.value == ge::host_pause::kHostPauseToken);
  retail.value = state.FilterRetailWrite(0);
  CHECK_TRUE(retail.value == ge::host_pause::kHostPauseToken);
  state.RequestPaused(false);
  result = Process(state, true, retail);
  CHECK_TRUE(result.action == ge::host_pause::Action::kRelease);
  CHECK_TRUE(retail.value == 0);
}

void TestResumeInputLatch() {
  ge::host_pause::ResumeInputLatch latch;
  ge::host_pause::InputSample held{.buttons = 0x2000};
  latch.Arm(held);
  CHECK_TRUE(latch.ShouldSuppress(held));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress(held));
  CHECK_TRUE(latch.ShouldSuppress({}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({}));
  CHECK_TRUE(!latch.active());
  CHECK_TRUE(!latch.ShouldSuppress(held));

  // Input that first appears after arming (for example a mouse button reaching
  // the guest driver after the host UI closes) resets the neutral sequence.
  latch.Arm({});
  CHECK_TRUE(latch.ShouldSuppress({}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({.right_trigger = UINT8_MAX}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({}));
  CHECK_TRUE(!latch.active());

  ge::host_pause::InputSample analog{
      .left_trigger = ge::host_pause::ResumeInputLatch::kTriggerThreshold,
      .thumb_lx = 20000,
  };
  latch.Arm(analog);
  CHECK_TRUE(latch.ShouldSuppress(analog));
  CHECK_TRUE(latch.ShouldSuppress({.thumb_lx = 5000}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({.thumb_lx = 5000}));
  CHECK_TRUE(!latch.active());

  // Match Dear ImGui's strict 20% navigation boundary in both directions: the
  // exact deadzone is neutral, while the first raw value above it is not.
  constexpr int16_t deadzone = ge::host_pause::ResumeInputLatch::kStickDeadzone;
  latch.Arm({.thumb_ly = deadzone});
  CHECK_TRUE(latch.ShouldSuppress({.thumb_ly = deadzone}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({.thumb_ly = deadzone}));
  CHECK_TRUE(!latch.active());

  latch.Arm({.thumb_ly = static_cast<int16_t>(deadzone + 1)});
  CHECK_TRUE(latch.ShouldSuppress({.thumb_ly = static_cast<int16_t>(deadzone + 1)}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({.thumb_ly = deadzone}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({.thumb_ly = deadzone}));
  CHECK_TRUE(!latch.active());

  latch.Arm({.thumb_ly = static_cast<int16_t>(-deadzone)});
  CHECK_TRUE(latch.ShouldSuppress({.thumb_ly = static_cast<int16_t>(-deadzone)}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({.thumb_ly = static_cast<int16_t>(-deadzone)}));
  CHECK_TRUE(!latch.active());

  latch.Arm({.thumb_ly = static_cast<int16_t>(-deadzone - 1)});
  CHECK_TRUE(latch.ShouldSuppress({.thumb_ly = static_cast<int16_t>(-deadzone - 1)}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({.thumb_ly = static_cast<int16_t>(-deadzone)}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({.thumb_ly = static_cast<int16_t>(-deadzone)}));
  CHECK_TRUE(!latch.active());

  // Partial release is not enough; all controls must be neutral for two polls.
  latch.Arm({.buttons = 0x3000});
  CHECK_TRUE(latch.ShouldSuppress({.buttons = 0x3000}));
  CHECK_TRUE(latch.ShouldSuppress({.buttons = 0x1000}));
  CHECK_TRUE(latch.ShouldSuppress({}));
  CHECK_TRUE(latch.active());
  CHECK_TRUE(latch.ShouldSuppress({}));
  CHECK_TRUE(!latch.active());

  // Re-arming an active latch starts a fresh neutral sequence.
  latch.Arm({});
  CHECK_TRUE(latch.ShouldSuppress({}));
  latch.Arm(held);
  CHECK_TRUE(latch.ShouldSuppress(held));
  CHECK_TRUE(latch.active());

  latch.Arm(held);
  for (uint32_t poll = 0; poll < ge::host_pause::ResumeInputLatch::kMaximumSuppressedPolls;
       ++poll) {
    CHECK_TRUE(latch.ShouldSuppress(held));
  }
  CHECK_TRUE(!latch.active());
  CHECK_TRUE(!latch.ShouldSuppress(held));
}

void TestRetryLogGate() {
  ge::host_pause::detail::RetryLogGate gate;
  ge::host_pause::ProcessResult failed_acquire{
      .action = ge::host_pause::Action::kAcquire,
      .action_succeeded = false,
      .generation = 7,
  };

  auto decision = gate.Observe(failed_acquire);
  CHECK_TRUE(decision.warn);
  CHECK_TRUE(!decision.recovered);
  CHECK_TRUE(decision.failed_attempts == 1);

  decision = gate.Observe(failed_acquire);
  CHECK_TRUE(!decision.warn);
  CHECK_TRUE(!decision.recovered);
  CHECK_TRUE(decision.failed_attempts == 2);

  // A newer idle/canceled request must discard the old failure episode without
  // claiming that a later, unrelated success recovered it.
  decision = gate.Observe({.request_applied = true, .generation = 8});
  CHECK_TRUE(!decision.warn);
  CHECK_TRUE(!decision.recovered);
  CHECK_TRUE(!gate.active());

  decision = gate.Observe({
      .action = ge::host_pause::Action::kAcquire,
      .action_succeeded = true,
      .generation = 9,
  });
  CHECK_TRUE(!decision.recovered);

  ge::host_pause::ProcessResult failed_release{
      .action = ge::host_pause::Action::kRelease,
      .action_succeeded = false,
      .generation = 10,
  };
  CHECK_TRUE(gate.Observe(failed_release).warn);
  CHECK_TRUE(!gate.Observe(failed_release).warn);
  decision = gate.Observe({
      .action = ge::host_pause::Action::kRelease,
      .action_succeeded = true,
      .generation = 10,
  });
  CHECK_TRUE(decision.recovered);
  CHECK_TRUE(decision.failed_attempts == 2);
  CHECK_TRUE(!gate.active());

  // A different action under the same generation is an ownership transition,
  // not recovery of the failed action.
  CHECK_TRUE(gate.Observe(failed_acquire).warn);
  decision = gate.Observe({
      .action = ge::host_pause::Action::kRelinquish,
      .action_succeeded = true,
      .generation = 7,
  });
  CHECK_TRUE(!decision.recovered);
  CHECK_TRUE(!gate.active());
}

size_t CountOccurrences(const std::string& text, const std::string& needle) {
  size_t count = 0;
  size_t position = 0;
  while ((position = text.find(needle, position)) != std::string::npos) {
    ++count;
    position += needle.size();
  }
  return count;
}

void TestGeneratedPauseWordContract() {
#if defined(GOLDENEYE_GENERATED_DIRECTORY)
  const std::filesystem::path generated(GOLDENEYE_GENERATED_DIRECTORY);
  size_t getter_calls = 0;
  size_t setter_calls = 0;
  size_t raw_pause_word_references = 0;
  const std::string getter_call = "\tsub_8209F588(ctx, base);";
  const std::string setter_call = "\tsub_8209F578(ctx, base);";
  const std::string zero_compare = "ctx.cr6.compare<int32_t>(ctx.r3.s32, 0, ctx.xer);";
  const size_t compare_r3_offset = zero_compare.find("ctx.r3");

  for (const auto& entry : std::filesystem::directory_iterator(generated)) {
    const std::string filename = entry.path().filename().string();
    if (!entry.is_regular_file() || !filename.starts_with("ge_recomp.") ||
        entry.path().extension() != ".cpp") {
      continue;
    }
    std::ifstream stream(entry.path(), std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    raw_pause_word_references += CountOccurrences(text, "-6388");

    size_t position = 0;
    while ((position = text.find(getter_call, position)) != std::string::npos) {
      ++getter_calls;
      const size_t after_call = position + getter_call.size();
      const size_t function_end = text.find("\nDEFINE_REX_FUNC(", after_call);
      const size_t first_r3_use = text.find("ctx.r3", after_call);
      const size_t comparison = text.find(zero_compare, after_call);
      const bool contract_ok =
          first_r3_use != std::string::npos && comparison != std::string::npos &&
          (function_end == std::string::npos || comparison < function_end) &&
          first_r3_use == comparison + compare_r3_offset && comparison - after_call < 512;
      CHECK_TRUE(contract_ok);
      position = after_call;
    }

    position = 0;
    while ((position = text.find(setter_call, position)) != std::string::npos) {
      ++setter_calls;
      const size_t function_start = text.rfind("\nDEFINE_REX_FUNC(", position);
      const size_t zero_assignment = text.rfind("ctx.r3.s64 = 0;", position);
      const size_t one_assignment = text.rfind("ctx.r3.s64 = 1;", position);
      size_t reaching_assignment = zero_assignment;
      if (one_assignment != std::string::npos &&
          (reaching_assignment == std::string::npos || one_assignment > reaching_assignment)) {
        reaching_assignment = one_assignment;
      }
      const size_t intervening_call = reaching_assignment == std::string::npos
                                          ? std::string::npos
                                          : text.find("(ctx, base);", reaching_assignment);
      const bool setter_contract_ok =
          reaching_assignment != std::string::npos &&
          (function_start == std::string::npos || reaching_assignment > function_start) &&
          intervening_call >= position && intervening_call < position + setter_call.size();
      CHECK_TRUE(setter_contract_ok);
      position += setter_call.size();
    }
  }

  // Lock the exact supported generated build contract: 22 consumers all test
  // zero/nonzero as their first in-function r3 use, all five retail setter call
  // sites pass a locally established boolean 0/1 without an intervening call,
  // and only setter/getter contain the four textual raw-word references
  // (comment plus generated memory operation for each).
  CHECK_TRUE(getter_calls == 22);
  CHECK_TRUE(setter_calls == 5);
  CHECK_TRUE(raw_pause_word_references == 4);
#else
  CHECK_TRUE(false);
#endif
}

}  // namespace

int main() {
  TestLocalMissionEligibility();
  TestAcquireReleaseAndIdempotence();
  TestPreexistingRetailPauseIsRestored();
  TestLaterRetailIntentIsPreserved();
  TestRetailWritesStayPausedAndEligibilityLossIsSafe();
  TestUnavailableRapidCancellationAndResumePulse();
  TestFailedWritesRetryAndReleaseRace();
  TestRetailPauseWriteShadow();
  TestResumeInputLatch();
  TestRetryLogGate();
  TestGeneratedPauseWordContract();
  return failures == 0 ? 0 : 1;
}
