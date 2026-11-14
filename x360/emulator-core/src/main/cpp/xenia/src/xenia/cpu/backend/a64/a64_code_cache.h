/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_A64_A64_CODE_CACHE_H_
#define XENIA_CPU_BACKEND_A64_A64_CODE_CACHE_H_

#include <cstdio>
#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>

#include "xenia/cpu/backend/code_cache_base.h"

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

class A64CodeCache : public CodeCacheBase<A64CodeCache> {
 public:
  ~A64CodeCache() override;

  static std::unique_ptr<A64CodeCache> Create();

  void* LookupUnwindInfo(uint64_t host_pc) override { return nullptr; }

  // CRTP hooks for CodeCacheBase.
  void FillCode(void* write_address, size_t size);
  void FlushCodeRange(void* address, size_t size);

  // Writes one simpleperf "generic JIT symbols" map entry per placed
  // function (perf-<pid>.map) so profilers can attribute guest code by name
  // instead of showing an anonymous "unknown" DSO. Gated by the a64_perf_map
  // cvar; Linux/Android only.
  void OnCodePlaced(uint32_t guest_address, GuestFunction* function_info,
                    void* code_execute_address, size_t code_size);

  // Virtual for platform-specific overrides (_win.cc / _posix.cc).
  virtual UnwindReservation RequestUnwindReservation(uint8_t* entry_address) {
    return UnwindReservation();
  }
  virtual void PlaceCode(uint32_t guest_address, void* machine_code,
                         const EmitFunctionInfo& func_info,
                         void* code_execute_address,
                         UnwindReservation unwind_reservation) {}

 protected:
  A64CodeCache() = default;

  // hakuX-Boosted: a64_linked_calls. A call site that did not know its
  // callee's host address when it was emitted loads it from an 8-byte
  // literal in its own function's tail code, which starts out as the
  // resolve thunk's address. LinkCallSites, called for every guest function
  // as it is placed, points the new function's literals at the callees that
  // already have code, remembers the rest, and points every remembered
  // literal that was waiting for the new function at it. A literal is data:
  // changing it is one aligned 8-byte store, and a caller that reads it a
  // moment too early just goes through the thunk once more.
  void LinkCallSites(uint32_t guest_address, GuestFunction* function_info,
                     void* code_execute_address);
  std::mutex link_mutex_;
  // Guest address -> host address, for every guest function placed.
  std::unordered_map<uint32_t, uint64_t> linked_targets_;
  // Guest address -> literals (write view) waiting for that function.
  std::unordered_map<uint32_t, std::vector<uint64_t*>> pending_literals_;

  std::mutex perf_map_mutex_;
  FILE* perf_map_file_ = nullptr;
  bool perf_map_open_failed_ = false;
};

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_A64_A64_CODE_CACHE_H_
