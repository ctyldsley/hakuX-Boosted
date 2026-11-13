/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// hakuX-Boosted: see host_thread_state.h.

#include "xenia/base/host_thread_state.h"

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"

#if XE_HOST_THREAD_STATE_SLOT
#include <android/api-level.h>
#endif

DEFINE_bool(
    host_thread_state_slot, false,
    "Android arm64: keep the current guest thread, CPU thread, PPC context, "
    "fiber and logging thread id of each host thread in one struct reached "
    "through bionic's TLS_SLOT_APP (one mrs and two loads) instead of in "
    "thread_local variables, each read of which is a call into the dynamic "
    "linker because the core is a dlopen'ed library. Read once, at the first "
    "use; needs API level 29. Off: the thread_local variables, as before.",
    "CPU");

DEFINE_bool(
    host_thread_state_verify, false,
    "With host_thread_state_slot: keep the thread_local variables as well and "
    "compare the two on every read, logging 'HostThreadState mismatch' when "
    "they differ. For checking the slot path on a device (fibers moving "
    "between host threads); it costs more than either path alone.",
    "CPU");

namespace xe {
namespace threading {

namespace host_thread_state_internal {

std::atomic<uint8_t> mode{kUndecided};

HostThreadState unregistered;

namespace {

#if XE_HOST_THREAD_STATE_SLOT

// The storage of a host thread's state: a thread_local of this library, so
// that it needs no allocation, lives exactly as long as the thread_locals it
// replaces (bionic frees a thread's dynamic TLS after the thread_local
// destructors, the pthread cleanup handlers and the pthread key destructors
// have all run: pthread_exit.cpp) and is reached through the linker only
// once per thread, here.
thread_local HostThreadState thread_state_storage_;

std::atomic<uint32_t> registered_threads_{0};

__attribute__((always_inline)) inline void WriteSlot(HostThreadState* state) {
  __asm__ volatile("mrs x16, tpidr_el0\n\t"
                   "str %0, [x16, #16]"
                   :
                   : "r"(state)
                   : "x16", "memory");
}

#endif  // XE_HOST_THREAD_STATE_SLOT

}  // namespace

XE_NOINLINE HostThreadState* Register() {
#if XE_HOST_THREAD_STATE_SLOT
  HostThreadState* state = ReadSlot();
  if (state) {
    return state;
  }
  state = &thread_state_storage_;
  state->verify.store(cvars::host_thread_state_verify ? 1 : 0,
                      std::memory_order_relaxed);
  WriteSlot(state);
  uint32_t count =
      registered_threads_.fetch_add(1, std::memory_order_relaxed) + 1;
  XELOGI("HostThreadState: host thread {} registered ({} so far)",
         current_thread_system_id(), count);
  // Read again instead of returning |state|: nothing above is known to switch
  // fibers, and this way it would not matter if it did.
  state = ReadSlot();
  return XE_LIKELY(state != nullptr) ? state : Register();
#else
  return nullptr;
#endif
}

XE_NOINLINE HostThreadState* DecideAndAcquire() {
#if XE_HOST_THREAD_STATE_SLOT
  uint8_t wanted = kThreadLocals;
  const char* why = "host_thread_state_slot is off";
  if (cvars::host_thread_state_slot) {
    int api_level = android_get_device_api_level();
    if (api_level < 29) {
      // The slot held errno before API level 29.
      why = "the device is below API level 29";
    } else if (ReadSlot() != nullptr) {
      // Nothing of ours has been written yet, so somebody else in the
      // process uses the slot: leave it to them.
      why = "TLS_SLOT_APP is already in use on this thread";
    } else {
      wanted = kSlot;
      why = cvars::host_thread_state_verify
                ? "host_thread_state_slot is on, verifying against the "
                  "thread_local variables"
                : "host_thread_state_slot is on";
    }
  }
  if (wanted == kSlot) {
    // Before the mode is published: readers look at this only after they
    // have seen kSlot.
    unregistered.verify.store(cvars::host_thread_state_verify ? 1 : 0,
                              std::memory_order_relaxed);
  }
  uint8_t expected = kUndecided;
  if (mode.compare_exchange_strong(expected, wanted,
                                   std::memory_order_acq_rel)) {
    XELOGI("HostThreadState: {} ({})",
           wanted == kSlot ? "per-thread state through TLS_SLOT_APP"
                           : "thread_local variables",
           why);
  }
  if (mode.load(std::memory_order_acquire) == kSlot) {
    return Register();
  }
  return nullptr;
#else
  mode.store(kThreadLocals, std::memory_order_relaxed);
  return nullptr;
#endif
}

}  // namespace host_thread_state_internal

XE_NOINLINE XE_COLD void HostThreadStateMismatch(const char* what,
                                                 uint64_t state_value,
                                                 uint64_t thread_local_value) {
  static std::atomic<uint32_t> total{0};
  static std::atomic<bool> unregistered_busy{false};
  uint32_t count = total.fetch_add(1, std::memory_order_relaxed) + 1;
  // The first few in full, then every 4096th: a real fault repeats.
  if (count > 32 && (count & 0xFFF) != 0) {
    return;
  }
#if XE_HOST_THREAD_STATE_SLOT
  // Logging reads the thread id through the state: do not report from inside
  // a report.
  HostThreadState* state = host_thread_state_internal::ReadSlot();
  if (state) {
    if (state->in_mismatch_report) {
      return;
    }
    state->in_mismatch_report = 1;
  } else if (unregistered_busy.exchange(true, std::memory_order_acquire)) {
    return;
  }
  XELOGE(
      "HostThreadState mismatch #{}: {} is {:016X} in the state and {:016X} "
      "in the thread_local, host thread {}{}",
      count, what, state_value, thread_local_value, current_thread_system_id(),
      state ? "" : " (not registered)");
  if (state) {
    // Through the pointer taken above, not a fresh read: the flag that was
    // set is the one to clear.
    state->in_mismatch_report = 0;
  } else {
    unregistered_busy.store(false, std::memory_order_release);
  }
#else
  (void)what;
  (void)state_value;
  (void)thread_local_value;
  (void)unregistered_busy;
#endif
}

}  // namespace threading
}  // namespace xe
