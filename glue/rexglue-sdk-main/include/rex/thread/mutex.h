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

#pragma once

#include <mutex>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

#if defined(__PROSPERO__)
#include <pthread.h>

#include <atomic>
#include <cstdint>
#endif

namespace rex::thread {

#if defined(__PROSPERO__)
// [ps5 port] The global critical region's lock on the PS5: a recursive spin
// lock (owner thread + depth). The guest takes this lock constantly - every
// interrupt-disabled stretch and lwarx/stwcx. retry loop of the recompiled
// code enters and leaves it - and std::recursive_mutex costs the PS5's
// kernel a round trip each time; under contention its sleeping waiters wake
// late, and the whole game ran slow (frame rate fine, game speed off).
// Uncontended, this is a compare-exchange; contended, it spins, then
// yields, then naps 50 us at a time (mutex.cpp).
class global_recursive_mutex {
 public:
  void lock() {
    const uintptr_t self = Self();
    if (owner_.load(std::memory_order_relaxed) == self) {
      ++depth_;
      return;
    }
    uintptr_t expected = 0;
    if (!owner_.compare_exchange_strong(expected, self, std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
      LockSlow(self);
    }
    depth_ = 1;
  }
  bool try_lock() {
    const uintptr_t self = Self();
    if (owner_.load(std::memory_order_relaxed) == self) {
      ++depth_;
      return true;
    }
    uintptr_t expected = 0;
    if (!owner_.compare_exchange_strong(expected, self, std::memory_order_acquire,
                                        std::memory_order_relaxed)) {
      return false;
    }
    depth_ = 1;
    return true;
  }
  void unlock() {
    if (owner_.load(std::memory_order_relaxed) != Self()) {
      return;  // (not this thread's: nothing to release)
    }
    if (--depth_ == 0) {
      owner_.store(0, std::memory_order_release);
    }
  }
  // The guest's MSR[EE] check: inside the region only on the thread holding it.
  bool owned_by_current_thread() const { return owner_.load(std::memory_order_relaxed) == Self(); }

 private:
  // (pthread_self(): 0.6b2 read the thread pointer at %fs:0 instead, and on
  // the PS5 two threads could see the same value there - the lock let both in.)
  static uintptr_t Self() { return reinterpret_cast<uintptr_t>(pthread_self()); }
  void LockSlow(uintptr_t self);

  std::atomic<uintptr_t> owner_{0};
  uint32_t depth_ = 0;
};
using global_mutex = global_recursive_mutex;
#else
using global_mutex = std::recursive_mutex;
#endif
// A held lock of the global critical region (callbacks that run inside it get one).
using global_lock_t = std::unique_lock<global_mutex>;

// The global critical region mutex singleton.
// This must guard any operation that may suspend threads or be sensitive to
// being suspended such as global table locks and such.
// To prevent deadlocks this should be the first lock acquired and be held
// for the entire duration of the critical region (longer than any other lock).
//
// As a general rule if some code can only be accessed from the guest you can
// guard it with only the global critical region and be assured nothing else
// will touch it. If it will be accessed from non-guest threads you may need
// some additional protection.
//
// You can think of this as disabling interrupts in the guest. The thread in the
// global critical region has exclusive access to the entire system and cannot
// be preempted. This also means that all activity done while in the critical
// region must be extremely fast (no IO!), as it has the chance to block any
// other thread until its done.
//
// For example, in the following situation thread 1 will not be able to suspend
// thread 0 until it has exited its critical region, preventing it from being
// suspended while holding the table lock:
//   [thread 0]:
//     DoKernelStuff():
//       auto global_lock = global_critical_region_.Acquire();
//       std::lock_guard<std::mutex> table_lock(table_mutex_);
//       table_->InsertStuff();
//   [thread 1]:
//     MySuspendThread():
//       auto global_lock = global_critical_region_.Acquire();
//       ::SuspendThread(thread0);
//
// To use the region it's strongly recommended that you keep an instance near
// the data requiring it. This makes it clear to those reading that the data
// is protected by the global critical region. For example:
// class MyType {
//   // Implies my_list_ is protected:
//   xe::global_critical_region global_critical_region_;
//   std::list<...> my_list_;
// };
class global_critical_region {
 public:
  static global_mutex& mutex();

  // Locks, spinning briefly before blocking. Guest interrupt-disabled
  // sections last a few instructions; sleeping on the futex for one (and
  // waking the sleeper on unlock) costs far more than the wait.
  static void LockSpinning() {
#if defined(__PROSPERO__)
    mutex().lock();  // [ps5 port] global_recursive_mutex spins on its own
    return;
#endif
    auto& global_mutex = mutex();
    for (int i = 0; i < 128; ++i) {
      if (global_mutex.try_lock()) return;
#if defined(__aarch64__)
      __asm__ volatile("yield");
#elif defined(__x86_64__) || defined(_M_X64)
      _mm_pause();
#endif
    }
    global_mutex.lock();
  }

  // Acquires a lock on the global critical section.
  // Use this when keeping an instance is not possible. Otherwise, prefer
  // to keep an instance of global_critical_region near the members requiring
  // it to keep things readable.
  static std::unique_lock<global_mutex> AcquireDirect() {
    return std::unique_lock<global_mutex>(mutex());
  }

  // Acquires a lock on the global critical section.
  inline std::unique_lock<global_mutex> Acquire() {
    return std::unique_lock<global_mutex>(mutex());
  }

  // Acquires a deferred lock on the global critical section.
  inline std::unique_lock<global_mutex> AcquireDeferred() {
    return std::unique_lock<global_mutex>(mutex(), std::defer_lock);
  }

  // Tries to acquire a lock on the glboal critical section.
  // Check owns_lock() to see if the lock was successfully acquired.
  inline std::unique_lock<global_mutex> TryAcquire() {
    return std::unique_lock<global_mutex>(mutex(), std::try_to_lock);
  }
};

}  // namespace rex::thread
