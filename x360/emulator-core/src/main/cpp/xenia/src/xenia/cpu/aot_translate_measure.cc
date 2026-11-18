/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * hakuX-Boosted: aot_translate_measure. Released under the BSD license.      *
 ******************************************************************************
 */

#include "xenia/cpu/aot_translate_measure.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <string>
#include <utility>

#include "xenia/base/platform.h"

#if XE_PLATFORM_LINUX
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/module.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/symbol.h"

namespace xe {
namespace cpu {

namespace {

thread_local bool t_is_measure_thread = false;
// The measuring thread's kernel thread id while it is translating a
// function, for BoostIfBusy; 0 otherwise. One measurement at a time.
std::atomic<int> g_measure_busy_tid{0};
std::atomic<bool> g_measure_boosted{false};

// The lowest priority an unprivileged thread can give itself. Not
// xe::threading's ThreadPriority::kLowest: on Linux that asks for SCHED_FIFO
// priority 1 first, which, where it is allowed, outranks every normal
// thread.
constexpr int kMeasureNice = 19;

int CurrentTid() {
#if XE_PLATFORM_LINUX
  return static_cast<int>(syscall(SYS_gettid));
#else
  return 0;
#endif
}

void SetThreadNice(int tid, int nice_value) {
#if XE_PLATFORM_LINUX
  // Failure (no permission to raise it back) is harmless: the thread keeps
  // the priority it has.
  setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice_value);
#else
  (void)tid;
  (void)nice_value;
#endif
}

// Seconds of CPU time used by the calling thread, or a negative value where
// the host does not say.
double ThreadCpuSeconds() {
#if XE_PLATFORM_LINUX
  timespec ts;
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
    return static_cast<double>(ts.tv_sec) +
           static_cast<double>(ts.tv_nsec) * 1e-9;
  }
#endif
  return -1.0;
}

// A "Name:   value kB" line of /proc/self/status, or "n/a".
std::string ProcStatusField(const char* field) {
#if XE_PLATFORM_LINUX
  std::ifstream status("/proc/self/status");
  std::string line;
  const std::string prefix = std::string(field) + ":";
  while (std::getline(status, line)) {
    if (line.compare(0, prefix.size(), prefix) == 0) {
      std::string value = line.substr(prefix.size());
      const size_t first = value.find_first_not_of(" \t");
      return first == std::string::npos ? std::string("n/a")
                                        : value.substr(first);
    }
  }
#else
  (void)field;
#endif
  return "n/a";
}

size_t CodeCacheUsed(Processor* processor) {
  auto backend = processor->backend();
  if (!backend || !backend->code_cache()) {
    return 0;
  }
  return backend->code_cache()->used_size();
}

double ToMs(uint64_t ns) { return static_cast<double>(ns) * 1e-6; }

}  // namespace

AotTranslateMeasure::AotTranslateMeasure(Processor* processor, Module* module,
                                         std::vector<uint32_t> addresses)
    : processor_(processor),
      module_(module),
      addresses_(std::move(addresses)) {}

AotTranslateMeasure::~AotTranslateMeasure() { Stop(); }

void AotTranslateMeasure::Start() {
  if (thread_.joinable()) {
    return;
  }
  thread_ = std::thread([this]() { Run(); });
}

void AotTranslateMeasure::Stop() {
  stop_.store(true, std::memory_order_release);
  if (!thread_.joinable()) {
    return;
  }
  if (thread_.get_id() == std::this_thread::get_id()) {
    thread_.detach();
    return;
  }
  if (xe::global_critical_region::is_held_by_current_thread()) {
    // The thread takes the global lock at every function; joining while
    // holding it would never return.
    XELOGW(
        "AOT measure: stopped by a thread holding the global lock; not "
        "waiting for it");
    thread_.detach();
    return;
  }
  thread_.join();
}

bool AotTranslateMeasure::IsMeasureThread() { return t_is_measure_thread; }

void AotTranslateMeasure::BoostIfBusy() {
  const int tid = g_measure_busy_tid.load(std::memory_order_acquire);
  if (!tid || t_is_measure_thread) {
    return;
  }
  if (g_measure_boosted.exchange(true, std::memory_order_acq_rel)) {
    return;  // already raised for this function
  }
#if XE_PLATFORM_LINUX
  errno = 0;
  const int mine = getpriority(PRIO_PROCESS, static_cast<id_t>(CurrentTid()));
  if (errno == 0) {
    SetThreadNice(tid, mine);
  }
#endif
}

void AotTranslateMeasure::Run() {
  using clock = std::chrono::steady_clock;
  t_is_measure_thread = true;
  xe::threading::set_name("AOT Measure");
  const int tid = CurrentTid();
  SetThreadNice(tid, kMeasureNice);

  std::sort(addresses_.begin(), addresses_.end());
  addresses_.erase(std::unique(addresses_.begin(), addresses_.end()),
                   addresses_.end());
  const size_t total = addresses_.size();

  struct Timed {
    uint32_t address;
    uint64_t ns;
  };
  std::vector<Timed> timed;
  timed.reserve(total);
  std::vector<std::pair<uint32_t, const char*>> failures;
  size_t already = 0;
  size_t skipped = 0;
  size_t processed = 0;
  uint64_t ppc_instructions = 0;
  uint64_t machine_code_bytes = 0;
  bool stopped = false;

  const size_t cache_before = CodeCacheUsed(processor_);
  const double cpu_start = ThreadCpuSeconds();
  const auto start = clock::now();
  auto next_log = start + std::chrono::seconds(5);
  XELOGI(
      "AOT measure: started, {} function starts in {} (nice {}), code cache "
      "{} bytes used",
      total, module_->name(), kMeasureNice, cache_before);

  for (size_t i = 0; i < total; ++i) {
    if (stop_.load(std::memory_order_acquire)) {
      stopped = true;
      break;
    }
    const uint32_t address = addresses_[i];
    ++processed;

    // Translated already (by a guest thread or the translate-ahead helper):
    // nothing to measure.
    if (processor_->QueryFunction(address)) {
      ++already;
    } else {
      Function* function = processor_->LookupFunction(module_, address);
      if (!function) {
        failures.emplace_back(address, "could not be declared");
      } else if (!function->is_guest() ||
                 function->behavior() != Function::Behavior::kDefault) {
        // Save/restore helpers, import thunks and the like.
        ++skipped;
      } else if (function->status() == Symbol::Status::kDefined) {
        ++already;
      } else if (function->status() == Symbol::Status::kFailed) {
        failures.emplace_back(address, "failed earlier");
      } else {
        g_measure_boosted.store(false, std::memory_order_release);
        g_measure_busy_tid.store(tid, std::memory_order_release);
        const auto t0 = clock::now();
        Function* resolved = processor_->ResolveFunction(address);
        const auto t1 = clock::now();
        g_measure_busy_tid.store(0, std::memory_order_release);
        if (g_measure_boosted.exchange(false, std::memory_order_acq_rel)) {
          SetThreadNice(tid, kMeasureNice);
        }
        if (!resolved) {
          failures.emplace_back(address, "translation failed");
        } else {
          const uint64_t ns = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                  .count());
          timed.push_back({address, ns});
          if (resolved->end_address() > address) {
            ppc_instructions += (resolved->end_address() - address) / 4;
          }
          if (resolved->is_guest()) {
            machine_code_bytes +=
                static_cast<GuestFunction*>(resolved)->machine_code_length();
          }
        }
      }
    }

    const auto now = clock::now();
    if (now >= next_log) {
      next_log = now + std::chrono::seconds(5);
      const double elapsed =
          std::chrono::duration<double>(now - start).count();
      XELOGI("AOT measure: {}/{} done, {:.1f} s elapsed, {:.1f} functions/s",
             processed, total, elapsed,
             elapsed > 0.0 ? static_cast<double>(processed) / elapsed : 0.0);
    }
  }

  const auto end = clock::now();
  const double wall = std::chrono::duration<double>(end - start).count();
  const double cpu_end = ThreadCpuSeconds();
  const size_t cache_after = CodeCacheUsed(processor_);

  XELOGI(
      "AOT measure: {}: {} of {} function starts processed; {} translated, "
      "{} already translated, {} skipped (not ordinary guest functions), {} "
      "failed",
      stopped ? "stopped early" : "finished", processed, total, timed.size(),
      already, skipped, failures.size());
  if (cpu_start >= 0.0 && cpu_end >= 0.0) {
    XELOGI("AOT measure: wall {:.2f} s, thread CPU {:.2f} s", wall,
           cpu_end - cpu_start);
  } else {
    XELOGI("AOT measure: wall {:.2f} s, thread CPU n/a", wall);
  }
  XELOGI(
      "AOT measure: {} PPC instructions translated (function end - start, "
      "/4, as the translator's scan found them), {} bytes of host code",
      ppc_instructions, machine_code_bytes);

  if (!timed.empty()) {
    std::vector<uint64_t> ns;
    ns.reserve(timed.size());
    uint64_t sum = 0;
    for (const auto& t : timed) {
      ns.push_back(t.ns);
      sum += t.ns;
    }
    std::sort(ns.begin(), ns.end());
    const size_t n = ns.size();
    const size_t p99_index = std::min(n - 1, (n * 99) / 100);
    XELOGI(
        "AOT measure: per function: mean {:.3f} ms, median {:.3f} ms, p99 "
        "{:.3f} ms, max {:.3f} ms",
        ToMs(sum / n), ToMs(ns[n / 2]), ToMs(ns[p99_index]), ToMs(ns[n - 1]));

    const size_t slowest = std::min<size_t>(10, timed.size());
    std::partial_sort(
        timed.begin(), timed.begin() + slowest, timed.end(),
        [](const Timed& a, const Timed& b) { return a.ns > b.ns; });
    for (size_t i = 0; i < slowest; ++i) {
      XELOGI("AOT measure: slowest {}: {:08X} {:.3f} ms", i + 1,
             timed[i].address, ToMs(timed[i].ns));
    }
  }

  constexpr size_t kMaxFailuresLogged = 200;
  for (size_t i = 0; i < failures.size() && i < kMaxFailuresLogged; ++i) {
    XELOGI("AOT measure: failed {:08X}: {}", failures[i].first,
           failures[i].second);
  }
  if (failures.size() > kMaxFailuresLogged) {
    XELOGI("AOT measure: {} more failures not listed",
           failures.size() - kMaxFailuresLogged);
  }

  XELOGI(
      "AOT measure: peak RSS (VmHWM) {}, RSS now {}; code cache used {} "
      "bytes before, {} after (includes other threads' translations)",
      ProcStatusField("VmHWM"), ProcStatusField("VmRSS"), cache_before,
      cache_after);
}

}  // namespace cpu
}  // namespace xe
