/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2017 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/thread.h"
#include "xenia/base/host_thread_state.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/thread_state.h"

namespace xe {
namespace cpu {

thread_local Thread* Thread::current_thread_ = nullptr;

Thread::Thread() {}
Thread::~Thread() {}

// hakuX-Boosted: never inlined (a thread_local read by code that fibers carry
// between host threads: kernel/xthread.cc, XThread::GetCurrentThread).
// With host_thread_state_slot on they read the host thread's state instead
// (base/host_thread_state.h): its address is read afresh on each call.
XE_NOINLINE bool Thread::IsInThread() { return GetCurrentThread() != nullptr; }

XE_NOINLINE Thread* Thread::GetCurrentThread() {
  if (const auto* state = xe::threading::HostThreadStateForRead()) {
    Thread* thread = state->cpu_thread;
    xe::threading::HostThreadStateVerify(
        state, "the current cpu::Thread", reinterpret_cast<uintptr_t>(thread),
        reinterpret_cast<uintptr_t>(current_thread_));
    return thread;
  }
  return current_thread_;
}

XE_NOINLINE void Thread::SetCurrentThreadPointer(Thread* thread) {
  if (auto* state = xe::threading::HostThreadStateForWrite()) {
    state->cpu_thread = thread;
    if (!xe::threading::HostThreadStateKeepsThreadLocal(state)) {
      return;
    }
  }
  current_thread_ = thread;
}
uint32_t Thread::GetCurrentThreadId() {
  return Thread::GetCurrentThread()->thread_state()->thread_id();
}

}  // namespace cpu
}  // namespace xe
