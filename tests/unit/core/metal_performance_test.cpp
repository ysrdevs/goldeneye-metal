#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <rex/graphics/metal/msl_compiler.h>
#include <rex/perf/metal_performance.h>

#include <chrono>

TEST_CASE("Metal pipeline cache miss hitch timing includes archive lookup",
          "[core][perf][metal]") {
  rex::graphics::metal::RenderPipelineCacheTelemetry telemetry;
  telemetry.archive_lookup_ns = UINT64_C(7) * 1000 * 1000;
  telemetry.archive_add_ns = UINT64_C(1) * 1000 * 1000;
  telemetry.pipeline_build_ns = 1;
  const uint64_t end_to_end_ns =
      rex::graphics::metal::GetRenderPipelineCacheMissDurationNs(telemetry);
  CHECK(end_to_end_ns == UINT64_C(8) * 1000 * 1000 + 1);

  const auto before = rex::perf::GetMetalPerformanceSnapshot();
  rex::perf::RecordMetalPipelineCompile(end_to_end_ns, true);
  const auto after = rex::perf::GetMetalPerformanceSnapshot();
  CHECK(after.pipeline_compiles >= before.pipeline_compiles + 1);
  CHECK(after.pipeline_compile_hitches >= before.pipeline_compile_hitches + 1);
}

TEST_CASE("Metal performance telemetry reports guest cadence and GPU work",
          "[core][perf][metal]") {
  const auto before = rex::perf::GetMetalPerformanceSnapshot();
  const uint64_t start_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
  constexpr uint64_t kSixtyFpsFrameNs = UINT64_C(16666667);
  for (uint64_t frame = 0; frame < 180; ++frame) {
    rex::perf::RecordMetalGuestFrame(start_ns + frame * kSixtyFpsFrameNs);
  }
  rex::perf::RecordMetalGpuTime(UINT64_C(5) * 1000 * 1000);
  rex::perf::RecordMetalGpuTime(UINT64_C(7) * 1000 * 1000);
  rex::perf::RecordMetalShaderCompile(UINT64_C(20) * 1000 * 1000, true);
  rex::perf::RecordMetalPipelineCompile(UINT64_C(2) * 1000 * 1000, true);
  rex::perf::RecordMetalPersistentCacheLookup(true);
  rex::perf::RecordMetalPersistentCacheLookup(false);

  const auto after = rex::perf::GetMetalPerformanceSnapshot();
  CHECK(after.presented_frames >= before.presented_frames + 180);
  CHECK(after.fps == Catch::Approx(60.0).margin(0.2));
  CHECK(after.frame_time_ms == Catch::Approx(16.666667).margin(0.05));
  CHECK(after.one_percent_low_fps == Catch::Approx(60.0).margin(0.2));
  CHECK(after.gpu_samples >= before.gpu_samples + 2);
  CHECK(after.gpu_time_ms == Catch::Approx(6.0).margin(0.01));
  CHECK(after.shader_compile_hitches >= before.shader_compile_hitches + 1);
  CHECK(after.pipeline_compiles >= before.pipeline_compiles + 1);
  CHECK(after.persistent_cache_hits >= before.persistent_cache_hits + 1);
  CHECK(after.persistent_cache_misses >= before.persistent_cache_misses + 1);

  const uint64_t unique_guest_frame = after.presented_frames + 1000;
  const uint64_t gpu_samples_before_dedup = after.gpu_samples;
  rex::perf::RecordMetalGpuTime(UINT64_C(4) * 1000 * 1000, unique_guest_frame);
  rex::perf::RecordMetalGpuTime(UINT64_C(9) * 1000 * 1000, unique_guest_frame);
  CHECK(rex::perf::GetMetalPerformanceSnapshot().gpu_samples ==
        gpu_samples_before_dedup + 1);
}

TEST_CASE("Metal performance report completes from guest frame time",
          "[core][perf][metal]") {
  REQUIRE(rex::perf::StartMetalPerformanceReport(5));
  const uint64_t start_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
  constexpr uint64_t kSixtyFpsFrameNs = UINT64_C(16666667);
  for (uint64_t frame = 0; frame <= 300; ++frame) {
    rex::perf::RecordMetalGuestFrame(start_ns + frame * kSixtyFpsFrameNs);
  }

  const auto completed = rex::perf::GetMetalPerformanceSnapshot();
  CHECK_FALSE(completed.report_active);
  CHECK(completed.last_report_available);
  const std::string report = rex::perf::GetLastMetalPerformanceReport();
  CHECK(report.find("Metal performance report:") != std::string::npos);
  CHECK(report.find("1%_low_fps=") != std::string::npos);
}

TEST_CASE("Metal performance report excludes paused host-menu frames",
          "[core][perf][metal]") {
  REQUIRE(rex::perf::StartMetalPerformanceReport(5));
  constexpr uint64_t kSecond = UINT64_C(1000000000);
  constexpr uint64_t kStart = UINT64_C(100) * kSecond;
  rex::perf::RecordMetalGuestFrame(kStart);
  rex::perf::RecordMetalGuestFrame(kStart + 2 * kSecond);

  rex::perf::PauseMetalPerformanceReport();
  CHECK(rex::perf::GetMetalPerformanceSnapshot().report_paused);
  rex::perf::RecordMetalGuestFrame(kStart + 30 * kSecond);
  rex::perf::RecordMetalGuestFrame(kStart + 40 * kSecond);
  rex::perf::RecordMetalShaderCompile(UINT64_C(20) * 1000 * 1000, true);
  rex::perf::RecordMetalPipelineCompile(UINT64_C(20) * 1000 * 1000, true);
  rex::perf::RecordMetalPersistentCacheLookup(true);
  rex::perf::RecordMetalPersistentCacheLookup(false);
  rex::perf::ResumeMetalPerformanceReport();

  const auto resumed = rex::perf::GetMetalPerformanceSnapshot();
  CHECK(resumed.report_active);
  CHECK_FALSE(resumed.report_paused);
  CHECK(resumed.report_elapsed_seconds == Catch::Approx(2.0));

  // The first resumed frame establishes a fresh baseline. Only the following
  // three seconds complete the five-second gameplay capture.
  rex::perf::RecordMetalGuestFrame(kStart + 41 * kSecond);
  rex::perf::RecordMetalPipelineCompile(UINT64_C(20) * 1000 * 1000, true);
  rex::perf::RecordMetalPersistentCacheLookup(true);
  rex::perf::RecordMetalGuestFrame(kStart + 44 * kSecond);
  CHECK_FALSE(rex::perf::GetMetalPerformanceSnapshot().report_active);
  const std::string report = rex::perf::GetLastMetalPerformanceReport();
  CHECK(report.find("frames=4 ") != std::string::npos);
  CHECK(report.find("shader_hitches=0 ") != std::string::npos);
  CHECK(report.find("pipeline_hitches=1 ") != std::string::npos);
  CHECK(report.find("persistent_cache_hits=1 ") != std::string::npos);
  CHECK(report.find("persistent_cache_misses=0") != std::string::npos);
}
