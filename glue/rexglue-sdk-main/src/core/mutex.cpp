/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/thread/mutex.h>

#if defined(__PROSPERO__)
#include <sched.h>
#include <time.h>
#endif

namespace rex::thread {

global_mutex& global_critical_region::mutex() {
  static global_mutex the_mutex;
  return the_mutex;
}

#if defined(__PROSPERO__)
// Contended: spin (the holder is usually out within a few microseconds),
// then yield, then nap 50 us at a time.
void global_recursive_mutex::LockSlow(uintptr_t self) {
  for (uint32_t tries = 0;; ++tries) {
    if (owner_.load(std::memory_order_relaxed) == 0) {
      uintptr_t expected = 0;
      if (owner_.compare_exchange_strong(expected, self, std::memory_order_acquire,
                                         std::memory_order_relaxed)) {
        return;
      }
    }
    if (tries < 2000) {
      __builtin_ia32_pause();
    } else if (tries < 2200) {
      sched_yield();
    } else {
      const timespec nap{0, 50000};
      nanosleep(&nap, nullptr);
    }
  }
}
#endif

}  // namespace rex::thread
