/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/threading.h"

#include "xenia/base/host_thread_state.h"
#include "xenia/base/platform.h"

namespace xe {
namespace threading {

uint32_t logical_processor_count() {
  static uint32_t value = 0;
  if (!value) {
    value = std::thread::hardware_concurrency();
  }
  return value;
}

thread_local uint32_t current_thread_id_ = UINT_MAX;

// hakuX-Boosted: never inlined (thread_local reads by code that fibers carry
// between host threads: kernel/xthread.cc, XThread::GetCurrentThread).
// hakuX-Boosted: through the host thread's state when host_thread_state_slot
// is on (base/host_thread_state.h), else the thread_local, as before.
XE_NOINLINE uint32_t current_thread_id() {
  if (const HostThreadState* state = HostThreadStateForRead()) {
    uint32_t id = state->thread_id;
    if (HostThreadStateVerifies(state)) {
      HostThreadStateCheck("the logging thread id", id, current_thread_id_);
    }
    return id == UINT_MAX ? current_thread_system_id() : id;
  }
  return current_thread_id_ == UINT_MAX ? current_thread_system_id()
                                        : current_thread_id_;
}

XE_NOINLINE void set_current_thread_id(uint32_t id) {
  if (HostThreadState* state = HostThreadStateForWrite()) {
    state->thread_id = id;
    if (!HostThreadStateVerifies(state)) {
      return;
    }
  }
  current_thread_id_ = id;
}

}  // namespace threading
}  // namespace xe
