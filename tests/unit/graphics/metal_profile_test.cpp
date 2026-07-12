#include <catch2/catch_test_macros.hpp>

#include <string_view>

#include <rex/graphics/metal/profile.h>

namespace {

namespace profiling = rex::graphics::metal::profiling;

TEST_CASE("Metal profile duration aggregation is swap-bounded", "[graphics][metal][profile]") {
  profiling::DurationWindow duration;
  duration.Add(10);
  duration.Add(30);
  duration.EndSwap();
  duration.Add(7);
  duration.EndSwap();

  CHECK(duration.total.call_count == 3);
  CHECK(duration.total.total_ns == 47);
  CHECK(duration.total.max_call_ns == 30);
  CHECK(duration.max_swap_ns == 40);
  CHECK(duration.max_calls_per_swap == 2);
}

TEST_CASE("Metal command profile reports exactly every 64 swaps", "[graphics][metal][profile]") {
  profiling::CommandProfileWindow window;
  for (uint32_t swap = 0; swap < profiling::kReportInterval; ++swap) {
    window.Record(profiling::CommandEvent::kIssueDraw, swap + 1);
    window.RecordWait(profiling::WaitReason::kGlobalCap, 100 + swap, swap % 3);
    CHECK(window.EndSwap() == (swap + 1 == profiling::kReportInterval));
  }

  const auto& draw = window.event(profiling::CommandEvent::kIssueDraw);
  CHECK(draw.total.call_count == profiling::kReportInterval);
  CHECK(draw.total.total_ns == 2080);
  CHECK(draw.total.max_call_ns == 64);
  CHECK(draw.max_swap_ns == 64);

  const auto& wait = window.wait(profiling::WaitReason::kGlobalCap);
  CHECK(wait.duration.total.call_count == profiling::kReportInterval);
  CHECK(wait.waited_call_count == 42);
  CHECK(wait.waited_submission_count == 63);
  CHECK(wait.max_waited_submissions_per_call == 2);
  CHECK(wait.max_waited_submissions_per_swap == 2);

  window.Reset();
  CHECK(window.swap_count() == 0);
  CHECK(window.event(profiling::CommandEvent::kIssueDraw).total.call_count == 0);
}

TEST_CASE("Metal wait reasons and presenter counters remain distinct",
          "[graphics][metal][profile]") {
  CHECK(std::string_view(profiling::CommandEventName(profiling::CommandEvent::kWaitRegMem)) ==
        "wait_reg_mem");
  CHECK(profiling::GetWaitReason("resource-mutation") == profiling::WaitReason::kResourceMutation);
  CHECK(profiling::GetWaitReason("global-cap") == profiling::WaitReason::kGlobalCap);
  CHECK(profiling::GetWaitReason("future-reason") == profiling::WaitReason::kOther);
  CHECK(profiling::GetWaitReason(nullptr) == profiling::WaitReason::kOther);

  profiling::PresenterProfileWindow presenter;
  presenter.RecordSource(false);
  presenter.RecordSource(true);
  presenter.Record(profiling::PresenterEvent::kNextDrawable, 40);
  presenter.Record(profiling::PresenterEvent::kUpload, 20);
  presenter.RecordUpload(4096);
  presenter.Record(profiling::PresenterEvent::kPresentCommit, 10);
  presenter.RecordCommit();
  CHECK_FALSE(presenter.EndAttempt());

  CHECK(presenter.source_count() == 2);
  CHECK(presenter.unchanged_source_count() == 1);
  CHECK(presenter.upload_count() == 1);
  CHECK(presenter.upload_byte_count() == 4096);
  CHECK(presenter.commit_count() == 1);
  CHECK(presenter.event(profiling::PresenterEvent::kUpload).total.total_ns == 20);
}

}  // namespace
