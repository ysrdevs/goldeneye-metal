/**
 * @file        core/clock_posix.cpp
 * @brief       POSIX platform clock implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/platform.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <sys/time.h>

namespace rex::chrono {

constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;

#if defined(CLOCK_MONOTONIC_RAW)
constexpr clockid_t kHostMonotonicClock = CLOCK_MONOTONIC_RAW;
#else
constexpr clockid_t kHostMonotonicClock = CLOCK_MONOTONIC;
#endif

uint64_t Clock::host_tick_frequency_platform() {
  // host_tick_count_platform returns an absolute count expressed in
  // nanoseconds. clock_getres describes the clock's precision, not the units
  // of that count; using it as the frequency under-reports time by 42x on
  // current macOS, where CLOCK_MONOTONIC_RAW has 42 ns resolution.
  return kNanosecondsPerSecond;
}

uint64_t Clock::host_tick_count_platform() {
  timespec tp;
  int error = clock_gettime(kHostMonotonicClock, &tp);
  assert_zero(error);

  return tp.tv_nsec + tp.tv_sec * kNanosecondsPerSecond;
}

uint64_t Clock::QueryHostSystemTime() {
  // https://docs.microsoft.com/en-us/windows/win32/sysinfo/converting-a-time-t-value-to-a-file-time
  constexpr uint64_t seconds_per_day = 3600 * 24;
  // Don't forget the 89 leap days.
  constexpr uint64_t seconds_1601_to_1970 = ((369 * 365 + 89) * seconds_per_day);

  timeval now;
  int error = gettimeofday(&now, nullptr);
  assert_zero(error);

  // NT systems use 100ns intervals.
  return static_cast<uint64_t>(
      (static_cast<int64_t>(now.tv_sec) + seconds_1601_to_1970) * 10000000ull + now.tv_usec * 10);
}

uint64_t Clock::QueryHostUptimeMillis() {
  return host_tick_count_platform() * 1000 / host_tick_frequency_platform();
}

}  // namespace rex::chrono
