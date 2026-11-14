/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/backend/a64/a64_code_cache.h"

#include "xenia/cpu/backend/a64/a64_function.h"
#include "xenia/cpu/compiler/translation_stats.h"

#include <cstdint>

#include "xenia/base/cvar.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/function.h"
#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif
#if XE_PLATFORM_xendroid
#include "../aarch64_disasm.h"
#endif
#if defined(__linux__)
#include <unistd.h>

#include <cinttypes>
#include <cstring>
#endif

// hakuX-Boosted: see A64CodeCache::LinkCallSites.
DEFINE_bool(
    a64_linked_calls, false,
    "A call to a guest function that had no translated code yet when its "
    "caller was translated loads the callee's address from a literal beside "
    "the caller's code, which is filled in when the callee is translated, "
    "instead of looking the callee up in the indirection table on every "
    "call. Set before launch: it must be on when a function is placed for "
    "its callers to be linked.",
    "a64");

DEFINE_bool(a64_perf_map, true,
            "Write a simpleperf/perf 'generic JIT symbols' map file "
            "(perf-<pid>.map) mapping generated code to guest function "
            "names, so profilers can attribute JIT samples.",
            "CPU");

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

A64CodeCache::~A64CodeCache() {
  if (perf_map_file_) {
    std::fclose(perf_map_file_);
  }
}

// hakuX-Boosted: see the declaration.
void A64CodeCache::LinkCallSites(uint32_t guest_address,
                                 GuestFunction* function_info,
                                 void* code_execute_address) {
  if (!guest_address || !function_info) {
    return;
  }
  const auto to_write_view = [this](uint64_t execute_address) {
    return reinterpret_cast<uint64_t*>(
        generated_code_write_base_ +
        (execute_address -
         reinterpret_cast<uint64_t>(generated_code_execute_base_)));
  };
  const auto store = [](uint64_t* literal, uint64_t value) {
    // Aligned (the emitter and the cache see to that), so one store that
    // another thread's load sees whole.
    __atomic_store_n(literal, value, __ATOMIC_RELEASE);
  };
  const uint64_t self_address =
      reinterpret_cast<uint64_t>(code_execute_address);
  uint64_t linked_now = 0;
  std::lock_guard<std::mutex> lock(link_mutex_);
  // The new function's own call sites.
  for (const auto& literal :
       static_cast<A64Function*>(function_info)->call_literals()) {
    uint64_t* slot = to_write_view(self_address + literal.second);
    auto known = linked_targets_.find(literal.first);
    if (literal.first == guest_address) {
      store(slot, self_address);  // recursion
      ++linked_now;
    } else if (known != linked_targets_.end()) {
      store(slot, known->second);
      ++linked_now;
    } else {
      pending_literals_[literal.first].push_back(slot);
    }
  }
  // The call sites that were waiting for it. A function placed a second time
  // at the same guest address (it can be translated again) takes over for
  // calls linked from now on; literals already pointed at the first copy
  // keep it, which stays valid: code is never freed.
  linked_targets_[guest_address] = self_address;
  auto waiting = pending_literals_.find(guest_address);
  if (waiting != pending_literals_.end()) {
    for (uint64_t* slot : waiting->second) {
      store(slot, self_address);
    }
    linked_now += waiting->second.size();
    pending_literals_.erase(waiting);
  }
  compiler::translation_stats.linked_call_sites_resolved.fetch_add(
      linked_now, std::memory_order_relaxed);
}

void A64CodeCache::OnCodePlaced(uint32_t guest_address,
                                GuestFunction* function_info,
                                void* code_execute_address, size_t code_size) {
  if (cvars::a64_linked_calls) {
    LinkCallSites(guest_address, function_info, code_execute_address);
  }
#if defined(__linux__)
  if (!cvars::a64_perf_map) {
    return;
  }
  std::lock_guard<std::mutex> lock(perf_map_mutex_);
  if (!perf_map_file_) {
    if (perf_map_open_failed_) {
      return;
    }
    // simpleperf looks for the map in the app's data directory
    // (/data/data/<package>/perf-<pid>.map); the package name is the process
    // name up to the ':' (this runs in the :emu process). Fall back to the
    // standalone-program locations.
    char process_name[256] = {0};
    if (FILE* cmdline = std::fopen("/proc/self/cmdline", "rb")) {
      std::fread(process_name, 1, sizeof(process_name) - 1, cmdline);
      std::fclose(cmdline);
    }
    if (char* colon = std::strchr(process_name, ':')) {
      *colon = '\0';
    }
    const int pid = getpid();
    char path[512];
    std::snprintf(path, sizeof(path), "/data/data/%s/perf-%d.map",
                  process_name, pid);
    perf_map_file_ = std::fopen(path, "we");
    if (!perf_map_file_) {
      std::snprintf(path, sizeof(path), "/data/local/tmp/perf-%d.map", pid);
      perf_map_file_ = std::fopen(path, "we");
    }
    if (!perf_map_file_) {
      std::snprintf(path, sizeof(path), "/tmp/perf-%d.map", pid);
      perf_map_file_ = std::fopen(path, "we");
    }
    if (!perf_map_file_) {
      perf_map_open_failed_ = true;
      return;
    }
  }
  const uint64_t address = reinterpret_cast<uint64_t>(code_execute_address);
  if (function_info && !function_info->name().empty()) {
    std::fprintf(perf_map_file_, "0x%" PRIx64 " 0x%zx %s\n", address,
                 code_size, function_info->name().c_str());
  } else if (guest_address) {
    std::fprintf(perf_map_file_, "0x%" PRIx64 " 0x%zx sub_%08X\n", address,
                 code_size, guest_address);
  } else {
    std::fprintf(perf_map_file_, "0x%" PRIx64 " 0x%zx a64_host_code\n",
                 address, code_size);
  }
  std::fflush(perf_map_file_);
#endif
}

void A64CodeCache::FillCode(void* write_address, size_t size) {
  // Fill with BRK #0 (0xD4200000), 4-byte aligned.
  constexpr uint32_t kBrk0 = 0xD4200000;
  auto* p = reinterpret_cast<uint32_t*>(write_address);
  auto* end =
      reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(write_address) + size);
  for (; p < end; ++p) {
    *p = kBrk0;
  }
}

void A64CodeCache::FlushCodeRange(void* address, size_t size) {
#if XE_PLATFORM_WIN32
  FlushInstructionCache(GetCurrentProcess(), address, size);
#else


#if XE_PLATFORM_xendroid
    //XELOGI("ASM:\n{}", aarch64_disasm(reinterpret_cast<uint64_t>(address),reinterpret_cast<uint32_t*>(address),size/4));
#endif
    __builtin___clear_cache(
      reinterpret_cast<char*>(address),
      reinterpret_cast<char*>(static_cast<uint8_t*>(address) + size));
#endif
}

}  // namespace a64
}  // namespace backend
}  // namespace cpu
}  // namespace xe
