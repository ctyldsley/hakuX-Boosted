/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_FUNCTION_H_
#define XENIA_CPU_BACKEND_A64_A64_FUNCTION_H_

#include <atomic>

#include <atomic>
#include <utility>
#include <vector>

#include "xenia/cpu/function.h"
#include "xenia/cpu/thread_state.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

class A64Function : public GuestFunction {
 public:
  A64Function(Module* module, uint32_t address);
  ~A64Function() override;

  uint8_t* machine_code() const override {
    return machine_code_.load(std::memory_order_acquire);
  }
  size_t machine_code_length() const override {
    return machine_code_length_.load(std::memory_order_acquire);
  }

  void Setup(uint8_t* machine_code, size_t machine_code_length);

  // hakuX-Boosted: for functions translated with pinned guest registers
  // (pin_guest_registers). Such a function cannot be entered at a return
  // site's ordinary host address, as ResolveFunction does when a longjmp
  // lands in it, because its host registers would not hold the guest
  // registers; each return site has a stub in the function's tail code that
  // reads them from the context first. Set by the emitter before the code is
  // placed in the cache, so before any thread can run it.
  void SetPinnedResumeStubs(
      std::vector<std::pair<uint32_t, uint32_t>> guest_address_to_code_offset);
  bool has_pinned_guest_registers() const {
    return has_pinned_guest_registers_.load(std::memory_order_acquire);
  }
  // Host address of the stub for a return site, or 0 if it has none.
  uintptr_t MapPinnedResumeAddress(uint32_t guest_address) const;

 protected:
  bool CallImpl(ThreadState* thread_state, uint32_t return_address) override;

 private:
  std::atomic<uint8_t*> machine_code_{nullptr};
  std::atomic<size_t> machine_code_length_{0};
  // hakuX-Boosted
  std::vector<std::pair<uint32_t, uint32_t>> pinned_resume_stubs_;
  std::atomic<bool> has_pinned_guest_registers_{false};
};

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_FUNCTION_H_
