/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_function.h"

#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

A64Function::A64Function(Module* module, uint32_t address)
    : GuestFunction(module, address) {}

A64Function::~A64Function() {
  // machine_code_ is freed by code cache.
}

void A64Function::Setup(uint8_t* machine_code, size_t machine_code_length) {
  machine_code_length_.store(machine_code_length, std::memory_order_relaxed);
  machine_code_.store(machine_code, std::memory_order_release);
}

// hakuX-Boosted
void A64Function::SetPinnedResumeStubs(
    std::vector<std::pair<uint32_t, uint32_t>> guest_address_to_code_offset) {
  pinned_resume_stubs_ = std::move(guest_address_to_code_offset);
  has_pinned_guest_registers_.store(true, std::memory_order_release);
}

uintptr_t A64Function::MapPinnedResumeAddress(uint32_t guest_address) const {
  auto* code = machine_code_.load(std::memory_order_acquire);
  if (!code || !has_pinned_guest_registers()) {
    return 0;
  }
  for (const auto& stub : pinned_resume_stubs_) {
    if (stub.first == guest_address) {
      return reinterpret_cast<uintptr_t>(code) + stub.second;
    }
  }
  return 0;
}

bool A64Function::CallImpl(ThreadState* thread_state, uint32_t return_address) {
  auto backend =
      reinterpret_cast<A64Backend*>(thread_state->processor()->backend());
  auto thunk = backend->host_to_guest_thunk();
  auto* code = machine_code_.load(std::memory_order_acquire);
  if (!thunk || !code) {
    return false;
  }
  thunk(code, thread_state->context(),
        reinterpret_cast<void*>(uintptr_t(return_address)));
  return true;
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
