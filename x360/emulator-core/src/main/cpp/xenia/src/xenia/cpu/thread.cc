/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2017 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/thread.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/thread_state.h"

namespace xe {
namespace cpu {

thread_local Thread* Thread::current_thread_ = nullptr;

Thread::Thread() {}
Thread::~Thread() {}

// hakuX-Boosted: never inlined (a thread_local read by code that fibers carry
// between host threads: kernel/xthread.cc, XThread::GetCurrentThread).
XE_NOINLINE bool Thread::IsInThread() { return current_thread_ != nullptr; }

XE_NOINLINE Thread* Thread::GetCurrentThread() { return current_thread_; }
uint32_t Thread::GetCurrentThreadId() {
  return Thread::GetCurrentThread()->thread_state()->thread_id();
}

}  // namespace cpu
}  // namespace xe
