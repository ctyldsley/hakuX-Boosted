/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/threading.h"

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
XE_NOINLINE uint32_t current_thread_id() {
  return current_thread_id_ == UINT_MAX ? current_thread_system_id()
                                        : current_thread_id_;
}

XE_NOINLINE void set_current_thread_id(uint32_t id) { current_thread_id_ = id; }

}  // namespace threading
}  // namespace xe
