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

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

#if REX_PLATFORM_PS5
// [ps5 port] clock_gettime is a system call on the PS5; the guest reads the
// clock (mftb, KeQueryPerformanceCounter) tens of thousands of times a second.
// The TSC is invariant and in step across the cores, as titles rely on.
extern "C" uint64_t sceKernelReadTsc(void);
extern "C" uint64_t sceKernelGetTscFrequency(void);
namespace {
uint64_t Ps5TscFrequency() {
  static const uint64_t frequency = sceKernelGetTscFrequency();
  return frequency;
}
}  // namespace
#endif

namespace rex::chrono {

uint64_t Clock::host_tick_frequency_platform() {
#if REX_PLATFORM_PS5
  if (const uint64_t frequency = Ps5TscFrequency()) return frequency;
#endif
#ifdef __APPLE__
  mach_timebase_info_data_t info;
  mach_timebase_info(&info);
  return (uint64_t)((1000000000ull * (uint64_t)info.denom) / (uint64_t)info.numer);
#else
  // host_tick_count_platform() counts nanoseconds, so this is 1 GHz whatever
  // the clock's resolution. (It used to be 1e9 / clock_getres(): 1 ns on Linux,
  // but FreeBSD reports 1e9 / timecounter frequency + 1, more than 1 ns on a
  // PS5, and the guest clock ran that many times too fast.)
  return 1000000000ull;
#endif
}

uint64_t Clock::host_tick_count_platform() {
#ifdef __APPLE__
  // mach_absolute_time() returns raw hardware ticks; host_tick_frequency_platform()
  // expresses the rate of these same ticks via mach_timebase_info.
  // clock_gettime(CLOCK_MONOTONIC_RAW) returns nanoseconds, which would be
  // inconsistent with the 24 MHz mach frequency and cause ~41x time acceleration.
  return mach_absolute_time();
#else
#if REX_PLATFORM_PS5
  if (Ps5TscFrequency()) return sceKernelReadTsc();
#endif
  timespec tp;
  int error = clock_gettime(CLOCK_MONOTONIC_RAW, &tp);
  assert_zero(error);

  return tp.tv_nsec + tp.tv_sec * 1000000000ull;
#endif
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
