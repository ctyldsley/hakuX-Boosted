/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// hakuX-Boosted: the hot per-host-thread pointers in one struct, reached
// without the dynamic linker.
//
// The core is a shared library loaded with dlopen, so every read of a
// thread_local is a call to the linker's tlsdesc_resolver_dynamic (bionic
// refuses the initial-exec model in a dlopen'ed library). The busiest guest
// thread spends several per cent of its time there, in the "who am I" readers
// (XThread::GetCurrentThread, cpu::ThreadState::Get, Fiber::GetCurrentFiber,
// ...).
//
// Bionic keeps one slot of its thread control block free for applications from
// API level 29: TLS_SLOT_APP, slot 2. On arm64 the thread pointer tpidr_el0
// points at slot 0 and a slot is one pointer, so the slot is the word at
// tpidr_el0 + 16. Source: bionic, libc/platform/bionic/tls_defines.h:
//   "TLS_SLOT_APP: Available for use by apps in API level 29 and later.
//    (This slot was used for errno in API level 28 and earlier.)"
//   "#define TLS_SLOT_APP              2 // was historically used for errno"
// and libc/platform/bionic/tls.h, where __get_tls() on aarch64 returns the
// slots as a void** read by "mrs %0, tpidr_el0".
//
// With the setting host_thread_state_slot on (off by default), each host
// thread that writes one of these values gets a HostThreadState whose address
// is kept in that slot, and the readers become "mrs; ldr; ldr". With it off,
// or on any other platform, the functions below return nullptr and the callers
// use their thread_local variables exactly as before.
//
// Guest threads are fibers that the scheduler moves between host threads, so
// a value read from this state before a fiber switch must not be reused after
// it, and neither may the state's address. Two things make sure of that:
//  - the slot is read by an "asm volatile" statement. The compiler has to
//    treat a volatile asm as having side effects it cannot see: it will not
//    merge two of them, move one out of a loop or above a call, or drop one,
//    so every call of the functions below reads the slot of the host thread
//    the code is on at that moment;
//  - the readers that use them (XThread::GetCurrentThread and the others)
//    stay XE_NOINLINE, as they were.
// Do not keep a HostThreadState* (or anything read from it) in a local over
// anything that can switch fibers: a wait, a yield, a guest call.

#ifndef XENIA_BASE_HOST_THREAD_STATE_H_
#define XENIA_BASE_HOST_THREAD_STATE_H_

#include <atomic>
#include <cstdint>

#include "xenia/base/platform.h"

// The slot exists, with this meaning, on bionic only; its place relative to
// the thread pointer is written here for arm64 only.
#if defined(__ANDROID__) && defined(__aarch64__)
#define XE_HOST_THREAD_STATE_SLOT 1
// Never a call: the point is two instructions.
#define XE_HOST_THREAD_STATE_INLINE __attribute__((always_inline)) inline
#else
#define XE_HOST_THREAD_STATE_SLOT 0
#define XE_HOST_THREAD_STATE_INLINE inline
#endif

namespace xe {
namespace kernel {
class XThread;
}  // namespace kernel
namespace cpu {
class Thread;
class ThreadState;
}  // namespace cpu

namespace threading {

class Fiber;

// One per host thread, never shared between threads. Each field replaces one
// thread_local and has the value that thread_local has when nothing was
// written: the owners of the fields are named beside them.
struct HostThreadState {
  // kernel/xthread.cc, current_xthread_tls_: the guest thread whose fiber
  // (or whose own host thread) is running here.
  kernel::XThread* xthread = nullptr;
  // cpu/thread.cc, cpu::Thread::current_thread_: the same object, as the CPU
  // sees it.
  cpu::Thread* cpu_thread = nullptr;
  // cpu/thread_state.cc, thread_state_: the PPC context bound here.
  cpu::ThreadState* cpu_thread_state = nullptr;
  // base/threading_fiber.cc, current_fiber_: the fiber executing here.
  Fiber* fiber = nullptr;
  // base/threading.cc, current_thread_id_: the id logging uses, UINT32_MAX
  // for "the host thread's own".
  uint32_t thread_id = UINT32_MAX;
  // Non-zero with host_thread_state_verify: the owners also keep their
  // thread_local and compare the two on every read.
  std::atomic<uint32_t> verify{0};
  // Set while this thread reports a mismatch (the report logs, and logging
  // reads the thread id through here).
  uint32_t in_mismatch_report = 0;
  // Not used yet. Kept for kernel/guest_scheduler.cc, t_current_cpu: the
  // logical CPU whose dispatch thread this is, -1 on any other thread
  // (boosted/notes/host-thread-state.md has the change to make there).
  int32_t scheduler_cpu = -1;
};

namespace host_thread_state_internal {

// Decided once, by the first write of any of the values on any thread, and
// never changed afterwards: a thread must not have some values in its
// thread_locals and the rest here.
enum Mode : uint8_t {
  kUndecided = 0,
  kThreadLocals = 1,
  kSlot = 2,
};
// What a host thread that never wrote any of the values reads.
#if XE_HOST_THREAD_STATE_SLOT
// Hidden: used inside this library only, so reached without the GOT.
extern std::atomic<uint8_t> mode __attribute__((visibility("hidden")));
extern HostThreadState unregistered __attribute__((visibility("hidden")));
#else
extern std::atomic<uint8_t> mode;
extern HostThreadState unregistered;
#endif

// The first write in the process: reads the setting, fixes the mode.
HostThreadState* DecideAndAcquire();
// The first write on this host thread while the mode is kSlot.
HostThreadState* Register();

#if XE_HOST_THREAD_STATE_SLOT
// TLS_SLOT_APP of the calling host thread. Volatile, with the load inside:
// see the top of the file. always_inline, so that no call is paid for it.
XE_HOST_THREAD_STATE_INLINE HostThreadState* ReadSlot() {
  HostThreadState* state;
  __asm__ volatile(
      "mrs %0, tpidr_el0\n\t"
      "ldr %0, [%0, #16]"
      : "=r"(state));
  return state;
}
#endif  // XE_HOST_THREAD_STATE_SLOT

}  // namespace host_thread_state_internal

// For a reader. nullptr: use the thread_local (the setting is off, the
// platform has no slot, or nothing was written anywhere yet, in which case
// the thread_local still has its initial value). Otherwise never null: a host
// thread that wrote nothing gets a state of initial values.
XE_HOST_THREAD_STATE_INLINE const HostThreadState* HostThreadStateForRead() {
#if XE_HOST_THREAD_STATE_SLOT
  using namespace host_thread_state_internal;
  if (mode.load(std::memory_order_relaxed) == kSlot) {
    const HostThreadState* state = ReadSlot();
    return XE_LIKELY(state != nullptr) ? state : &unregistered;
  }
#endif
  return nullptr;
}

// For a writer. nullptr: write the thread_local. Otherwise this host thread's
// own state, made on its first write.
XE_HOST_THREAD_STATE_INLINE HostThreadState* HostThreadStateForWrite() {
#if XE_HOST_THREAD_STATE_SLOT
  using namespace host_thread_state_internal;
  uint8_t current_mode = mode.load(std::memory_order_relaxed);
  if (current_mode == kSlot) {
    HostThreadState* state = ReadSlot();
    return XE_LIKELY(state != nullptr) ? state : Register();
  }
  if (XE_UNLIKELY(current_mode == kUndecided)) {
    return DecideAndAcquire();
  }
#endif
  return nullptr;
}

// With host_thread_state_verify: the value read from the state and the one in
// the thread_local differ. Logs the first few, counts all.
void HostThreadStateMismatch(const char* what, uint64_t state_value,
                             uint64_t thread_local_value);

// True with host_thread_state_verify: writers keep the thread_local as well,
// readers compare it with what they read from the state. Readers must test
// this before they touch the thread_local: reading it is the cost this file
// exists to avoid.
inline bool HostThreadStateVerifies(const HostThreadState* state) {
  return XE_UNLIKELY(state->verify.load(std::memory_order_relaxed) != 0);
}

inline void HostThreadStateCheck(const char* what, uint64_t state_value,
                                 uint64_t thread_local_value) {
  if (state_value != thread_local_value) {
    HostThreadStateMismatch(what, state_value, thread_local_value);
  }
}

}  // namespace threading
}  // namespace xe

#endif  // XENIA_BASE_HOST_THREAD_STATE_H_
