/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * hakuX-Boosted: where the host runs the emulator's threads, and how the     *
 * phone is told what they need.                                              *
 *                                                                            *
 * Three things, each behind its own setting, all off by default:             *
 *                                                                            *
 *   host_perf_log          a line a second: every core's frequency, the      *
 *                          phone's thermal state, and how long each guest    *
 *                          CPU dispatch thread ran and waited for a core.    *
 *   host_perf_hint         an Android performance hint session (ADPF) over   *
 *                          the six dispatch threads and the GPU command      *
 *                          thread, told every presented frame how long the   *
 *                          frame's work took against the title's frame time. *
 *   host_perf_core_policy  affinity masks: the guest's threads on the        *
 *                          biggest cores, the compile threads off them.      *
 *                                                                            *
 * Nothing here is called by the code that creates the threads. They are      *
 * found by name in /proc/self/task once a second, so a thread that is        *
 * created again is found again, and the kernel and GPU sources are left as   *
 * they are. The one hook is the call from xe::RecordGuestPresent             *
 * (base/frame_stats.h). Why, what the other emulators do and what to measure:*
 * boosted/notes/thread-placement.md.                                         *
 *                                                                            *
 * Not run on a device. What was run: this file on Linux x86-64 against       *
 * threads with the same names and a stand-in libandroid.so.                  *
 ******************************************************************************
 */

#include "xenia/base/host_perf.h"

#include <atomic>
#include <cstdint>
#include <string>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

DEFINE_bool(
    host_perf_log, false,
    "Log once a second: each core's current frequency and its present limit, "
    "the thermal status and headroom Android reports, and for each guest CPU "
    "dispatch thread, the GPU command thread and the compile threads the time "
    "on a core and the time spent waiting for one since the last line.",
    "Kernel");

DEFINE_bool(
    host_perf_hint, false,
    "Keep an Android performance hint session (ADPF) over the six guest CPU "
    "dispatch threads and the GPU command thread: the target is the title's "
    "frame time and the work duration is reported once per presented frame. "
    "Does nothing where the phone has no hint support.",
    "Kernel");

DEFINE_uint32(
    host_perf_hint_target_fps, 0,
    "The frame rate the performance hint session aims for. 0 works it out "
    "from the title's own presents (the quicker quarter of the last 64, as a "
    "whole number of 60 Hz refreshes); set 30 or 60 when the title is known.",
    "Kernel");

DEFINE_uint32(
    host_perf_hint_report, 0,
    "What the performance hint session is told a frame took. 0: the CPU time "
    "of the busiest thread in the session, against nine tenths of the frame "
    "time, and the whole interval when the frame was late. 1: always the "
    "interval between presents, against the frame time itself.",
    "Kernel");

DEFINE_uint32(
    host_perf_core_policy, 0,
    "Where the host may run the emulator's threads. 0: anywhere (the "
    "kernel's choice). 1: the guest CPU dispatch threads and the GPU command "
    "thread on the biggest cores, the compile threads on the cores kept for "
    "them. 2: as 1, and each dispatch thread on a core of its own, the "
    "busiest on the fastest. Lets go by itself when too few cores are "
    "allowed or the phone reports severe throttling.",
    "Kernel");

DEFINE_uint32(
    host_perf_helper_cores, 2,
    "With host_perf_core_policy: how many cores are kept for the compile "
    "threads, at least. Small cores always are.",
    "Kernel");

DEFINE_string(
    host_perf_helper_threads, "Vulkan Pipeline,Shader Translat",
    "With host_perf_core_policy: the threads kept off the guest's cores, as "
    "comma-separated beginnings of thread names (a name is at most 15 "
    "characters).",
    "Kernel");

#if defined(__linux__)

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

// A build of this file for a test can point it at a made-up
// /sys/devices/system/cpu; /proc is always the real one.
#ifndef XE_HOST_PERF_SYS_ROOT
#define XE_HOST_PERF_SYS_ROOT ""
#endif
#define XE_HOST_PERF_CPU_DIR XE_HOST_PERF_SYS_ROOT "/sys/devices/system/cpu"

namespace xe {
namespace host_perf {
namespace {

constexpr int kMaxCpus = 64;
// One dispatch thread per hardware thread of the console
// (kernel/guest_scheduler.cc names them "Guest CPU 0" to "Guest CPU 5").
constexpr int kDispatchCount = 6;
constexpr int kSessionThreadsMax = kDispatchCount + 1;
constexpr int64_t kNsPerSec = 1000000000;
constexpr int64_t kNsPerMs = 1000000;
// The console's display refresh, which a title's frame time is a multiple of.
constexpr int64_t kVblankNs = 16666667;
// Fewer cores than this for the seven guest-side threads and a placement
// policy would squeeze them harder than no policy does: it lets go instead.
constexpr int kMinGuestCores = 4;
// How often the one-core-each policy looks again at which dispatch thread is
// the busiest, in ticks of the worker (seconds).
constexpr int kRankPeriodTicks = 30;
constexpr int kFirstRankTicks = 5;

// -- small readers -----------------------------------------------------------

int64_t NowNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * kNsPerSec + ts.tv_nsec;
}

// The CPU time a thread of this process has used. The clock id is the
// kernel's per-thread CPU clock, built from the tid the way bionic's
// pthread_getcpuclockid builds it. -1 when the thread is gone.
int64_t ThreadCpuNs(pid_t tid) {
  const clockid_t id =
      static_cast<clockid_t>((~static_cast<unsigned int>(tid) << 3) | 6u);
  timespec ts;
  if (clock_gettime(id, &ts) != 0) {
    return -1;
  }
  return int64_t(ts.tv_sec) * kNsPerSec + ts.tv_nsec;
}

// Reads a small file whole. The text is NUL-terminated; false when the file
// cannot be read or is empty.
bool ReadSmallFile(const char* path, char* buffer, size_t buffer_size) {
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  size_t length = 0;
  while (length + 1 < buffer_size) {
    const ssize_t got = read(fd, buffer + length, buffer_size - 1 - length);
    if (got < 0 && errno == EINTR) {
      continue;
    }
    if (got <= 0) {
      break;
    }
    length += size_t(got);
  }
  close(fd);
  buffer[length] = '\0';
  return length != 0;
}

bool ReadNumberFile(const char* path, long long* value) {
  char text[64];
  if (!ReadSmallFile(path, text, sizeof(text))) {
    return false;
  }
  char* end = nullptr;
  const long long parsed = strtoll(text, &end, 10);
  if (end == text) {
    return false;
  }
  *value = parsed;
  return true;
}

bool ReadCpuNumber(int cpu, const char* leaf, long long* value) {
  char path[160];
  snprintf(path, sizeof(path), XE_HOST_PERF_CPU_DIR "/cpu%d/%s", cpu, leaf);
  return ReadNumberFile(path, value);
}

uint64_t CpuBit(int cpu) { return uint64_t(1) << cpu; }

int CountBits(uint64_t mask) { return __builtin_popcountll(mask); }

uint64_t FullMask(int cpu_count) {
  return cpu_count >= 64 ? ~uint64_t(0) : CpuBit(cpu_count) - 1;
}

// "0-3,6" as the kernel writes a list of CPUs, to a mask.
uint64_t ParseCpuList(const char* text) {
  uint64_t mask = 0;
  const char* at = text;
  while (*at) {
    char* end = nullptr;
    const long first = strtol(at, &end, 10);
    if (end == at) {
      break;
    }
    long last = first;
    at = end;
    if (*at == '-') {
      ++at;
      last = strtol(at, &end, 10);
      if (end == at) {
        break;
      }
      at = end;
    }
    for (long cpu = first; cpu <= last && cpu < kMaxCpus; ++cpu) {
      if (cpu >= 0) {
        mask |= CpuBit(int(cpu));
      }
    }
    if (*at != ',') {
      break;
    }
    ++at;
  }
  return mask;
}

// A mask as the kernel would write it, for the log.
std::string MaskToList(uint64_t mask) {
  std::string text;
  int cpu = 0;
  while (cpu < kMaxCpus) {
    if (!(mask & CpuBit(cpu))) {
      ++cpu;
      continue;
    }
    int last = cpu;
    while (last + 1 < kMaxCpus && (mask & CpuBit(last + 1))) {
      ++last;
    }
    if (!text.empty()) {
      text += ',';
    }
    text += std::to_string(cpu);
    if (last != cpu) {
      text += '-';
      text += std::to_string(last);
    }
    cpu = last + 1;
  }
  return text.empty() ? std::string("none") : text;
}

__attribute__((format(printf, 2, 3))) void Appendf(std::string& text,
                                                   const char* format, ...) {
  char piece[256];
  va_list args;
  va_start(args, format);
  vsnprintf(piece, sizeof(piece), format, args);
  va_end(args);
  text += piece;
}

bool GetAffinity(pid_t tid, uint64_t* mask) {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(tid, sizeof(set), &set) != 0) {
    return false;
  }
  uint64_t bits = 0;
  for (int cpu = 0; cpu < kMaxCpus; ++cpu) {
    if (CPU_ISSET(cpu, &set)) {
      bits |= CpuBit(cpu);
    }
  }
  *mask = bits;
  return true;
}

bool SetAffinity(pid_t tid, uint64_t mask) {
  cpu_set_t set;
  CPU_ZERO(&set);
  for (int cpu = 0; cpu < kMaxCpus; ++cpu) {
    if (mask & CpuBit(cpu)) {
      CPU_SET(cpu, &set);
    }
  }
  return sched_setaffinity(tid, sizeof(set), &set) == 0;
}

// -- the cores ---------------------------------------------------------------

struct Topology {
  int cpu_count = 0;
  // The kernel's relative capacity of each core, 1024 for the biggest. Where
  // cpu_capacity cannot be read it is made from the highest frequencies, and
  // where those cannot be read either every core counts the same.
  int capacity[kMaxCpus] = {};
  long long max_khz[kMaxCpus] = {};
  bool capacity_from_kernel = false;
  bool have_max_khz = false;
};

Topology ReadTopology() {
  Topology topology;
  long count = sysconf(_SC_NPROCESSORS_CONF);
  if (count < 1) {
    count = 1;
  }
  topology.cpu_count = int(std::min<long>(count, kMaxCpus));
  bool every_capacity = true;
  long long highest_khz = 0;
  for (int cpu = 0; cpu < topology.cpu_count; ++cpu) {
    long long value = 0;
    if (ReadCpuNumber(cpu, "cpu_capacity", &value) && value > 0) {
      topology.capacity[cpu] = int(value);
    } else {
      every_capacity = false;
    }
    if (ReadCpuNumber(cpu, "cpufreq/cpuinfo_max_freq", &value) && value > 0) {
      topology.max_khz[cpu] = value;
      highest_khz = std::max(highest_khz, value);
      topology.have_max_khz = true;
    }
  }
  topology.capacity_from_kernel = every_capacity;
  if (!every_capacity) {
    for (int cpu = 0; cpu < topology.cpu_count; ++cpu) {
      topology.capacity[cpu] =
          highest_khz > 0 && topology.max_khz[cpu] > 0
              ? int(topology.max_khz[cpu] * 1024 / highest_khz)
              : 1024;
    }
  }
  return topology;
}

// -- where the threads may run ----------------------------------------------

struct PlacementInput {
  int cpu_count = 0;
  const int* capacity = nullptr;
  // Capacity scaled by the frequency each core is limited to at present, in
  // steps of 32: what a core is worth while the phone is throttling it.
  const int* limited_capacity = nullptr;
  // The cores this process may use now: its cpuset less what is offline.
  uint64_t allowed = 0;
  uint32_t policy = 0;
  uint32_t helper_cores = 2;
  bool thermal_severe = false;
  // For policy 2: each dispatch thread's time on a core over the last
  // ranking period, and the core it has now (-1 for none).
  const int64_t* run_ns = nullptr;
  const int* home_core = nullptr;
};

struct Placement {
  // 0: no masks. 1: two sets of cores. 2: one core for each dispatch thread.
  uint32_t policy_in_force = 0;
  // Why less than what was asked for is in force; empty when it is.
  const char* note = "";
  uint64_t guest_mask = 0;
  uint64_t helper_mask = 0;
  // 0 where a thread is to be left alone.
  uint64_t dispatch_mask[kDispatchCount] = {};
  uint64_t gpu_mask = 0;
  int home_core[kDispatchCount] = {-1, -1, -1, -1, -1, -1};
};

// The policy, with nothing read and nothing set, so it can be tested.
//
// The helpers get every small core (less than half the capacity of the
// biggest) and never fewer than helper_cores, taken from the bottom: lowest
// capacity first, and of equals the lowest numbered, which is where interrupts
// and the system's own housekeeping mostly are. The guest's threads get the
// rest. Both are sets a thread may move within, so the kernel still balances
// and still steers round a core it has slowed down.
//
// Policy 2 goes further: each dispatch thread on a core of its own, as the
// console has, the busiest threads on the cores worth most at present. The
// GPU command thread is then left alone: with six of eight cores each given
// to one dispatch thread it would otherwise share the helpers' two.
Placement ComputePlacement(const PlacementInput& in) {
  Placement placement;
  if (in.policy == 0) {
    return placement;
  }
  int cpus[kMaxCpus];
  int count = 0;
  int biggest = 0;
  for (int cpu = 0; cpu < in.cpu_count && cpu < kMaxCpus; ++cpu) {
    if (in.allowed & CpuBit(cpu)) {
      cpus[count++] = cpu;
      biggest = std::max(biggest, in.capacity[cpu]);
    }
  }
  std::sort(cpus, cpus + count, [&](int a, int b) {
    return in.capacity[a] != in.capacity[b] ? in.capacity[a] < in.capacity[b]
                                            : a < b;
  });
  const int wanted_helpers = int(std::clamp<uint32_t>(in.helper_cores, 1, 16));
  int helpers = 0;
  while (helpers < count && (in.capacity[cpus[helpers]] * 2 < biggest ||
                             helpers < wanted_helpers)) {
    ++helpers;
  }
  const int guests = count - helpers;
  if (guests < kMinGuestCores) {
    placement.note = "too few cores would be left for the guest's threads";
    return placement;
  }
  for (int i = 0; i < count; ++i) {
    (i < helpers ? placement.helper_mask : placement.guest_mask) |=
        CpuBit(cpus[i]);
  }
  placement.policy_in_force = 1;
  placement.gpu_mask = placement.guest_mask;
  for (int i = 0; i < kDispatchCount; ++i) {
    placement.dispatch_mask[i] = placement.guest_mask;
  }
  if (in.policy < 2) {
    return placement;
  }
  if (in.thermal_severe) {
    placement.note =
        "severe throttling: the dispatch threads share their cores again";
    return placement;
  }
  if (guests < kDispatchCount) {
    placement.note = "fewer than six cores for the dispatch threads";
    return placement;
  }

  // The six guest cores worth most at present, best first.
  int homes[kMaxCpus];
  std::copy(cpus + helpers, cpus + count, homes);
  std::sort(homes, homes + guests, [&](int a, int b) {
    return in.limited_capacity[a] != in.limited_capacity[b]
               ? in.limited_capacity[a] > in.limited_capacity[b]
               : a > b;
  });
  // The dispatch threads, busiest first.
  int order[kDispatchCount];
  for (int i = 0; i < kDispatchCount; ++i) {
    order[i] = i;
  }
  std::stable_sort(order, order + kDispatchCount,
                   [&](int a, int b) { return in.run_ns[a] > in.run_ns[b]; });
  // Cores of equal worth are one class. The busiest threads get the best
  // class; within a class a thread keeps the core it has, so equals are never
  // swapped for nothing.
  bool taken[kDispatchCount] = {};
  int given[kDispatchCount] = {-1, -1, -1, -1, -1, -1};
  int first = 0;
  while (first < kDispatchCount) {
    int end = first + 1;
    while (end < kDispatchCount && in.limited_capacity[homes[end]] ==
                                       in.limited_capacity[homes[first]]) {
      ++end;
    }
    for (int k = first; k < end; ++k) {
      const int thread = order[k];
      for (int h = first; h < end; ++h) {
        if (!taken[h] && homes[h] == in.home_core[thread]) {
          given[thread] = h;
          taken[h] = true;
          break;
        }
      }
    }
    for (int k = first; k < end; ++k) {
      const int thread = order[k];
      for (int h = first; h < end && given[thread] < 0; ++h) {
        if (!taken[h]) {
          given[thread] = h;
          taken[h] = true;
        }
      }
    }
    first = end;
  }
  for (int i = 0; i < kDispatchCount; ++i) {
    placement.home_core[i] = homes[given[i]];
    placement.dispatch_mask[i] = CpuBit(homes[given[i]]);
  }
  placement.gpu_mask = 0;
  placement.policy_in_force = 2;
  return placement;
}

bool SamePlacement(const Placement& a, const Placement& b) {
  return a.policy_in_force == b.policy_in_force &&
         a.guest_mask == b.guest_mask && a.helper_mask == b.helper_mask &&
         a.gpu_mask == b.gpu_mask &&
         std::equal(a.dispatch_mask, a.dispatch_mask + kDispatchCount,
                    b.dispatch_mask) &&
         std::strcmp(a.note, b.note) == 0;
}

// -- the title's frame time --------------------------------------------------

constexpr int kIntervalRing = 64;

// The frame time a run of present-to-present intervals says the title aims
// for: the quicker quarter of them, as a whole number of refreshes. The
// quicker quarter because a title that hitches still shows its pace in its
// good frames; rounded down (with 15% of a refresh of slack for jitter)
// because a title held below its pace must not have its target moved to
// where it is.
int64_t EstimateFrameNs(const int64_t* intervals, int count) {
  int64_t sorted[kIntervalRing];
  std::copy(intervals, intervals + count, sorted);
  std::nth_element(sorted, sorted + count / 4, sorted + count);
  const int64_t quick = sorted[count / 4];
  const int64_t refreshes = (quick + kVblankNs * 15 / 100) / kVblankNs;
  return std::clamp<int64_t>(refreshes, 1, 4) * kVblankNs;
}

// -- Android's own interfaces, looked up at run time -------------------------
//
// The functions are found in libandroid.so by name: the app runs back to
// API 29, the hint session came with API 33, changing its threads with 34 and
// the thermal headroom with 31. Prototypes as in the NDK's
// android/performance_hint.h and android/thermal.h.

struct AndroidApi {
  void* (*hint_get_manager)() = nullptr;
  void* (*hint_create_session)(void* manager, const int32_t* thread_ids,
                               size_t size, int64_t initial_target_ns) =
      nullptr;
  int64_t (*hint_preferred_rate_ns)(void* manager) = nullptr;
  int (*hint_update_target)(void* session, int64_t target_ns) = nullptr;
  int (*hint_report_actual)(void* session, int64_t actual_ns) = nullptr;
  void (*hint_close_session)(void* session) = nullptr;
  int (*hint_set_threads)(void* session, const pid_t* thread_ids,
                          size_t size) = nullptr;
  // API 36. Preferred where present: see CreateHintSession.
  void* (*config_create)() = nullptr;
  void (*config_release)(void* config) = nullptr;
  void (*config_set_tids)(void* config, const pid_t* thread_ids,
                          size_t size) = nullptr;
  void (*config_set_target)(void* config, int64_t target_ns) = nullptr;
  int (*hint_create_with_config)(void* manager, void* config,
                                 void** session_out) = nullptr;
  void* hint_manager = nullptr;
  const char* hint_missing = "not looked up";

  void* (*thermal_acquire)() = nullptr;
  int (*thermal_status)(void* manager) = nullptr;
  float (*thermal_headroom)(void* manager, int forecast_seconds) = nullptr;
  void* thermal_manager = nullptr;
};

template <typename T>
void Lookup(void* library, const char* name, T* function) {
  *function = reinterpret_cast<T>(dlsym(library, name));
}

void LoadAndroidApi(AndroidApi* api) {
  void* library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
  if (!library) {
    api->hint_missing = "no libandroid.so";
    return;
  }
  Lookup(library, "APerformanceHint_getManager", &api->hint_get_manager);
  Lookup(library, "APerformanceHint_createSession", &api->hint_create_session);
  Lookup(library, "APerformanceHint_getPreferredUpdateRateNanos",
         &api->hint_preferred_rate_ns);
  Lookup(library, "APerformanceHint_updateTargetWorkDuration",
         &api->hint_update_target);
  Lookup(library, "APerformanceHint_reportActualWorkDuration",
         &api->hint_report_actual);
  Lookup(library, "APerformanceHint_closeSession", &api->hint_close_session);
  Lookup(library, "APerformanceHint_setThreads", &api->hint_set_threads);
  Lookup(library, "ASessionCreationConfig_create", &api->config_create);
  Lookup(library, "ASessionCreationConfig_release", &api->config_release);
  Lookup(library, "ASessionCreationConfig_setTids", &api->config_set_tids);
  Lookup(library, "ASessionCreationConfig_setTargetWorkDurationNanos",
         &api->config_set_target);
  Lookup(library, "APerformanceHint_createSessionUsingConfig",
         &api->hint_create_with_config);
  if (!api->hint_get_manager || !api->hint_create_session ||
      !api->hint_update_target || !api->hint_report_actual ||
      !api->hint_close_session) {
    api->hint_missing = "this Android has no APerformanceHint (before API 33)";
  } else {
    api->hint_manager = api->hint_get_manager();
    api->hint_missing =
        api->hint_manager ? "" : "the phone gives no performance hint manager";
  }

  Lookup(library, "AThermal_acquireManager", &api->thermal_acquire);
  Lookup(library, "AThermal_getCurrentThermalStatus", &api->thermal_status);
  Lookup(library, "AThermal_getThermalHeadroom", &api->thermal_headroom);
  if (api->thermal_acquire && api->thermal_status) {
    api->thermal_manager = api->thermal_acquire();
  }
}

// A session over these threads, or nullptr.
//
// Through APerformanceHint_createSessionUsingConfig where the phone has it
// (API 36). Android 16's APerformanceHint_createSession returns a pointer it
// never set when the service turns the request down, for one a thread that
// has just ended (frameworks/base native/android/performance_hint.cpp,
// APerformanceHintManager::createSession: "APerformanceHintSession*
// sessionOut;" and no assignment on that path); the newer call sets it to
// nullptr first.
void* CreateHintSession(const AndroidApi& api, const pid_t* tids, int count,
                        int64_t target_ns, int* error) {
  *error = 0;
  if (api.config_create && api.config_release && api.config_set_tids &&
      api.config_set_target && api.hint_create_with_config) {
    void* config = api.config_create();
    if (!config) {
      *error = ENOMEM;
      return nullptr;
    }
    api.config_set_tids(config, tids, size_t(count));
    api.config_set_target(config, target_ns);
    void* session = nullptr;
    *error = api.hint_create_with_config(api.hint_manager, config, &session);
    api.config_release(config);
    return *error == 0 ? session : nullptr;
  }
  int32_t ids[kSessionThreadsMax];
  std::copy(tids, tids + count, ids);
  return api.hint_create_session(api.hint_manager, ids, size_t(count),
                                 target_ns);
}

const char* ThermalStatusName(int status) {
  switch (status) {
    case 0:
      return "none";
    case 1:
      return "light";
    case 2:
      return "moderate";
    case 3:
      return "severe";
    case 4:
      return "critical";
    case 5:
      return "emergency";
    case 6:
      return "shutdown";
    default:
      return "unknown";
  }
}

// -- what the presenting thread and the worker share -------------------------

struct FrameShared {
  void* session = nullptr;
  int thread_count = 0;
  pid_t tids[kSessionThreadsMax] = {};
  int64_t last_cpu_ns[kSessionThreadsMax] = {};
  bool cpu_valid = false;
  // What the session's target was last set from; 0 to have it set again.
  int64_t frame_ns = 0;
  uint32_t report_mode = 0;
  int64_t target_ns = 0;
  // Since the worker's last line.
  uint32_t reports = 0;
  uint32_t late = 0;
  uint32_t errors = 0;
  int last_error = 0;
  int64_t sum_actual_ns = 0;
};

// The presenting thread only ever tries this lock, so the worker holding it
// across a call into the system costs a frame its report and nothing more.
std::mutex g_frame_mutex;
FrameShared g_frame;
AndroidApi g_api;
std::atomic<bool> g_worker_started{false};
std::atomic<bool> g_session_live{false};
std::atomic<uint32_t> g_frames{0};
std::atomic<uint32_t> g_longest_interval_us{0};
std::atomic<int64_t> g_estimated_frame_ns{2 * kVblankNs};

int64_t FixedFrameNs() {
  const uint32_t fps = cvars::host_perf_hint_target_fps;
  if (fps == 0) {
    return 0;
  }
  return kNsPerSec / std::clamp<uint32_t>(fps, 10, 240);
}

// -- the emulator's threads, by name -----------------------------------------

struct SchedSample {
  bool valid = false;
  long long run_ns = 0;
  long long wait_ns = 0;
  long long slices = 0;
};

SchedSample ReadSchedstat(pid_t tid) {
  SchedSample sample;
  char path[64];
  char text[128];
  snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", int(tid));
  if (ReadSmallFile(path, text, sizeof(text)) &&
      sscanf(text, "%lld %lld %lld", &sample.run_ns, &sample.wait_ns,
             &sample.slices) == 3) {
    sample.valid = true;
  }
  return sample;
}

struct StatSample {
  bool valid = false;
  int nice = 0;
  unsigned long long start_time = 0;
  int last_cpu = -1;
};

// The fields of /proc/<pid>/task/<tid>/stat wanted here: 19 nice,
// 22 starttime, 39 processor. The name, field 2, may hold anything, so the
// count starts after its closing bracket, at field 3.
StatSample ReadStat(pid_t tid) {
  StatSample sample;
  char path[64];
  char text[1024];
  snprintf(path, sizeof(path), "/proc/self/task/%d/stat", int(tid));
  if (!ReadSmallFile(path, text, sizeof(text))) {
    return sample;
  }
  char* at = strrchr(text, ')');
  if (!at) {
    return sample;
  }
  ++at;
  int field = 2;
  bool have_nice = false;
  bool have_start = false;
  while (*at) {
    while (*at == ' ') {
      ++at;
    }
    if (!*at) {
      break;
    }
    ++field;
    if (field == 19) {
      sample.nice = int(strtol(at, nullptr, 10));
      have_nice = true;
    } else if (field == 22) {
      sample.start_time = strtoull(at, nullptr, 10);
      have_start = true;
    } else if (field == 39) {
      sample.last_cpu = int(strtol(at, nullptr, 10));
      break;
    }
    while (*at && *at != ' ') {
      ++at;
    }
  }
  sample.valid = have_nice && have_start;
  return sample;
}

struct ThreadSet {
  pid_t dispatch[kDispatchCount] = {};
  pid_t gpu = 0;
  std::vector<pid_t> helpers;
  int total = 0;

  bool operator==(const ThreadSet& other) const {
    return std::equal(dispatch, dispatch + kDispatchCount, other.dispatch) &&
           gpu == other.gpu && helpers == other.helpers;
  }
};

// A thread is created with its creator's name and renamed afterwards, if at
// all: the pipeline compile threads are "GPU Commands" until they are named,
// and anything a guest thread starts is "Guest CPU n" until then. Of two
// threads with one of the names looked for, the one that started first is
// the real one.
struct Claim {
  pid_t tid = 0;
  bool start_known = false;
  unsigned long long start_time = 0;

  void Offer(pid_t candidate) {
    if (!tid) {
      tid = candidate;
      return;
    }
    if (!start_known) {
      start_time = ReadStat(tid).start_time;
      start_known = true;
    }
    const unsigned long long other = ReadStat(candidate).start_time;
    if (other < start_time || (other == start_time && candidate < tid)) {
      tid = candidate;
      start_time = other;
    }
  }
};

ThreadSet ScanThreads(const std::vector<std::string>& helper_names) {
  ThreadSet set;
  DIR* dir = opendir("/proc/self/task");
  if (!dir) {
    return set;
  }
  Claim dispatch[kDispatchCount];
  Claim gpu;
  while (dirent* entry = readdir(dir)) {
    if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
      continue;
    }
    const pid_t tid = pid_t(atoi(entry->d_name));
    char path[64];
    char name[32];
    snprintf(path, sizeof(path), "/proc/self/task/%d/comm", int(tid));
    if (!ReadSmallFile(path, name, sizeof(name))) {
      continue;
    }
    name[strcspn(name, "\n")] = '\0';
    ++set.total;
    if (!strncmp(name, "Guest CPU ", 10) && name[10] >= '0' &&
        name[10] < '0' + kDispatchCount && name[11] == '\0') {
      dispatch[name[10] - '0'].Offer(tid);
    } else if (!strcmp(name, "GPU Commands")) {
      gpu.Offer(tid);
    } else {
      for (const std::string& helper : helper_names) {
        if (!strncmp(name, helper.c_str(), helper.size())) {
          set.helpers.push_back(tid);
          break;
        }
      }
    }
  }
  closedir(dir);
  for (int i = 0; i < kDispatchCount; ++i) {
    set.dispatch[i] = dispatch[i].tid;
  }
  set.gpu = gpu.tid;
  std::sort(set.helpers.begin(), set.helpers.end());
  return set;
}

std::vector<std::string> ParseHelperNames(const std::string& text) {
  std::vector<std::string> names;
  size_t at = 0;
  while (at <= text.size() && names.size() < 16) {
    size_t end = text.find(',', at);
    if (end == std::string::npos) {
      end = text.size();
    }
    size_t first = at;
    size_t last = end;
    while (first < last && text[first] == ' ') {
      ++first;
    }
    while (last > first && text[last - 1] == ' ') {
      --last;
    }
    if (last > first) {
      // The kernel keeps 15 characters of a thread's name.
      names.push_back(text.substr(first, std::min<size_t>(last - first, 15)));
    }
    at = end + 1;
  }
  return names;
}

// -- the worker --------------------------------------------------------------

class Worker {
 public:
  void Run() {
    pthread_setname_np(pthread_self(), "Host Perf");
    // Started by the GPU command thread, whose nice value and affinity a new
    // thread takes with it. This one measures; it is no part of the guest.
    setpriority(PRIO_PROCESS, 0, 0);
    topology_ = ReadTopology();
    SetAffinity(0, FullMask(topology_.cpu_count));
    helper_names_ = ParseHelperNames(cvars::host_perf_helper_threads);
    LoadAndroidApi(&g_api);

    int64_t next = NowNs();
    for (;;) {
      Tick();
      next += kNsPerSec;
      const int64_t now = NowNs();
      if (next <= now) {
        next = now + kNsPerSec;
      }
      timespec until;
      until.tv_sec = time_t(next / kNsPerSec);
      until.tv_nsec = long(next % kNsPerSec);
      while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until,
                             nullptr) == EINTR) {
      }
    }
  }

 private:
  struct Tracked {
    pid_t tid = 0;
    SchedSample before;
    // Since the last tick.
    bool have_delta = false;
    long long run_ns = 0;
    long long wait_ns = 0;
    long long slices = 0;
  };

  void Tick() {
    const bool log_on = cvars::host_perf_log;
    const bool hint_on = cvars::host_perf_hint;
    const uint32_t policy = std::min<uint32_t>(cvars::host_perf_core_policy, 2);
    if (!log_on && !hint_on && policy == 0) {
      if (!idle_) {
        placement_ = Placement();
        have_placement_ = false;
        ApplyPlacement(placement_);
        CloseSession("the setting is off");
        idle_ = true;
        XELOGI("host perf: every setting is off, nothing more is done");
      }
      return;
    }
    const int64_t now = NowNs();
    if (idle_) {
      // Back on: the first line starts from now, not from when it went off.
      idle_ = false;
      described_ = false;
      last_tick_ns_ = 0;
      for (Tracked& tracked : dispatch_) {
        tracked = Tracked();
      }
      gpu_ = Tracked();
      helpers_before_.clear();
      DrainFrameCounters(nullptr);
    }

    ThreadSet threads = ScanThreads(helper_names_);
    uint64_t allowed = 0;
    if (!GetAffinity(0, &allowed)) {
      allowed = FullMask(topology_.cpu_count);
    }
    allowed &= FullMask(topology_.cpu_count);
    char online[128];
    online_ = ReadSmallFile(XE_HOST_PERF_CPU_DIR "/online", online,
                            sizeof(online))
                  ? ParseCpuList(online)
                  : FullMask(topology_.cpu_count);
    ReadThermal();
    ReadFrequencies();

    if (!described_ || allowed != allowed_) {
      allowed_ = allowed;
      Describe();
      described_ = true;
    }
    SampleScheduling(threads);
    UpdatePlacement(policy);
    const bool threads_changed = !(threads == threads_);
    threads_ = std::move(threads);
    ApplyPlacement(placement_);
    if (threads_changed) {
      // After the masks, so the line shows them.
      DescribeThreads();
    }
    UpdateSession(hint_on, now);
    if (log_on) {
      LogLine(now);
    } else {
      DrainFrameCounters(nullptr);
    }
    last_tick_ns_ = now;
  }

  // -- reading ---------------------------------------------------------------

  void ReadThermal() {
    thermal_status_ = -1;
    if (g_api.thermal_manager) {
      thermal_status_ = g_api.thermal_status(g_api.thermal_manager);
      // The header says about once a second is the most that is any use;
      // Android's games guide says no more than once in ten, or the answer
      // is NaN. The stricter of the two; the last value is shown in between.
      if (g_api.thermal_headroom && headroom_age_ticks_++ % 10 == 0) {
        thermal_headroom_ = g_api.thermal_headroom(g_api.thermal_manager, 0);
      }
    }
    // Latched from severe down to light, so the one-core-each policy is not
    // put back and taken off again around the threshold.
    if (thermal_status_ >= 3) {
      thermal_severe_ = true;
    } else if (thermal_status_ <= 1) {
      thermal_severe_ = false;
    }
  }

  void ReadFrequencies() {
    for (int cpu = 0; cpu < topology_.cpu_count; ++cpu) {
      long long value = 0;
      now_khz_[cpu] =
          ReadCpuNumber(cpu, "cpufreq/scaling_cur_freq", &value) ? value : 0;
      limit_khz_[cpu] =
          ReadCpuNumber(cpu, "cpufreq/scaling_max_freq", &value) ? value : 0;
      int limited = topology_.capacity[cpu];
      if (limit_khz_[cpu] > 0 && topology_.max_khz[cpu] > 0 &&
          limit_khz_[cpu] < topology_.max_khz[cpu]) {
        limited = int(limited * limit_khz_[cpu] / topology_.max_khz[cpu]);
      }
      limited_capacity_[cpu] = (limited + 16) / 32 * 32;
    }
  }

  void SampleOne(Tracked* tracked, pid_t tid) {
    if (tracked->tid != tid) {
      *tracked = Tracked();
      tracked->tid = tid;
    }
    tracked->have_delta = false;
    if (!tid) {
      return;
    }
    const SchedSample now = ReadSchedstat(tid);
    if (now.valid && tracked->before.valid) {
      tracked->run_ns = now.run_ns - tracked->before.run_ns;
      tracked->wait_ns = now.wait_ns - tracked->before.wait_ns;
      tracked->slices = now.slices - tracked->before.slices;
      tracked->have_delta = true;
    }
    tracked->before = now;
  }

  void SampleScheduling(const ThreadSet& threads) {
    for (int i = 0; i < kDispatchCount; ++i) {
      SampleOne(&dispatch_[i], threads.dispatch[i]);
      if (dispatch_[i].have_delta) {
        rank_gathering_ns_[i] += dispatch_[i].run_ns;
      }
    }
    SampleOne(&gpu_, threads.gpu);
    // The helpers together: what the same threads had used a tick ago.
    std::map<pid_t, SchedSample> now;
    helpers_run_ns_ = 0;
    helpers_wait_ns_ = 0;
    for (pid_t tid : threads.helpers) {
      const SchedSample sample = ReadSchedstat(tid);
      if (!sample.valid) {
        continue;
      }
      auto before = helpers_before_.find(tid);
      if (before != helpers_before_.end()) {
        helpers_run_ns_ += sample.run_ns - before->second.run_ns;
        helpers_wait_ns_ += sample.wait_ns - before->second.wait_ns;
      }
      now[tid] = sample;
    }
    helpers_before_.swap(now);
  }

  // -- placement -------------------------------------------------------------

  void UpdatePlacement(uint32_t policy) {
    const uint32_t helper_cores = cvars::host_perf_helper_cores;
    bool rank_due = false;
    if (policy == 2) {
      if (++ticks_since_rank_ >=
          (ranked_once_ ? kRankPeriodTicks : kFirstRankTicks)) {
        rank_due = true;
      }
    } else {
      ticks_since_rank_ = 0;
      ranked_once_ = false;
    }
    if (have_placement_ && policy == placed_policy_ &&
        allowed_ == placed_allowed_ && helper_cores == placed_helper_cores_ &&
        thermal_severe_ == placed_severe_ && !rank_due) {
      return;
    }
    if (rank_due) {
      std::copy(rank_gathering_ns_, rank_gathering_ns_ + kDispatchCount,
                rank_run_ns_);
      std::fill(rank_gathering_ns_, rank_gathering_ns_ + kDispatchCount, 0);
      ticks_since_rank_ = 0;
      ranked_once_ = true;
    }
    PlacementInput in;
    in.cpu_count = topology_.cpu_count;
    in.capacity = topology_.capacity;
    in.limited_capacity = limited_capacity_;
    in.allowed = allowed_;
    in.policy = policy;
    in.helper_cores = helper_cores;
    in.thermal_severe = thermal_severe_;
    in.run_ns = rank_run_ns_;
    in.home_core = placement_.home_core;
    const Placement placement = ComputePlacement(in);
    have_placement_ = true;
    placed_policy_ = policy;
    placed_allowed_ = allowed_;
    placed_helper_cores_ = helper_cores;
    placed_severe_ = thermal_severe_;
    if (SamePlacement(placement, placement_)) {
      return;
    }
    placement_ = placement;
    if (policy == 0) {
      XELOGI("host perf: core policy off, the threads may run anywhere");
      return;
    }
    std::string text;
    Appendf(text, "host perf: core policy %u asked, %u in force", policy,
            placement.policy_in_force);
    if (placement.note[0]) {
      Appendf(text, " (%s)", placement.note);
    }
    if (placement.policy_in_force) {
      Appendf(text, ": guest threads on cpu %s, helpers on cpu %s",
              MaskToList(placement.guest_mask).c_str(),
              MaskToList(placement.helper_mask).c_str());
    }
    if (placement.policy_in_force == 2) {
      text += "; one core each:";
      for (int i = 0; i < kDispatchCount; ++i) {
        Appendf(text, " G%d on cpu%d", i, placement.home_core[i]);
      }
      text += ", GPU Commands anywhere";
    }
    XELOGI("{}", text);
  }

  // Makes the masks what the placement says, every tick: a thread that was
  // created again has none, and the system puts a thread's mask back to its
  // whole cpuset when it moves the app between cpusets (to the background and
  // back).
  void ApplyPlacement(const Placement& placement) {
    struct Want {
      pid_t tid;
      uint64_t mask;
    };
    std::vector<Want> wants;
    for (int i = 0; i < kDispatchCount; ++i) {
      if (threads_.dispatch[i]) {
        wants.push_back({threads_.dispatch[i], placement.dispatch_mask[i]});
      }
    }
    if (threads_.gpu) {
      wants.push_back({threads_.gpu, placement.gpu_mask});
    }
    for (pid_t tid : threads_.helpers) {
      wants.push_back({tid, placement.helper_mask});
    }
    const uint64_t everything = FullMask(topology_.cpu_count);
    int placed = 0;
    int released = 0;
    for (const Want& want : wants) {
      if (want.mask) {
        uint64_t now = 0;
        if (!GetAffinity(want.tid, &now)) {
          continue;
        }
        if (now != want.mask) {
          if (!SetAffinity(want.tid, want.mask)) {
            const int error = errno;
            if (refused_.insert({want.tid, error}).second) {
              XELOGW(
                  "host perf: thread {} could not be kept to cpu {}: errno "
                  "{} ({}); it is left where it was",
                  int(want.tid), MaskToList(want.mask), error,
                  std::strerror(error));
            }
            continue;
          }
          ++placed;
        }
        applied_[want.tid] = want.mask;
      } else if (applied_.erase(want.tid)) {
        SetAffinity(want.tid, everything);
        ++released;
      }
    }
    // A thread that had a mask and is no longer one of ours: gone, or renamed.
    for (auto it = applied_.begin(); it != applied_.end();) {
      const pid_t tid = it->first;
      const bool wanted = std::any_of(
          wants.begin(), wants.end(),
          [tid](const Want& want) { return want.tid == tid && want.mask; });
      if (wanted) {
        ++it;
        continue;
      }
      SetAffinity(tid, everything);
      ++released;
      it = applied_.erase(it);
    }
    if (placed || released) {
      XELOGI("host perf: {} thread(s) given their cores, {} let go", placed,
             released);
    }
  }

  // -- the hint session ------------------------------------------------------

  void CloseSession(const char* why) {
    void* session = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_frame_mutex);
      session = g_frame.session;
      g_frame.session = nullptr;
      g_frame.thread_count = 0;
      g_frame.cpu_valid = false;
    }
    g_session_live.store(false, std::memory_order_relaxed);
    if (session) {
      g_api.hint_close_session(session);
      XELOGI("host perf: performance hint session closed ({})", why);
    }
  }

  void UpdateSession(bool hint_on, int64_t now) {
    if (!hint_on) {
      CloseSession("the setting is off");
      return;
    }
    if (g_api.hint_missing[0]) {
      if (!hint_missing_logged_) {
        hint_missing_logged_ = true;
        XELOGW("host perf: host_perf_hint is on but does nothing: {}",
               g_api.hint_missing);
      }
      return;
    }
    pid_t tids[kSessionThreadsMax];
    int count = 0;
    for (int i = 0; i < kDispatchCount; ++i) {
      if (threads_.dispatch[i]) {
        tids[count++] = threads_.dispatch[i];
      }
    }
    if (threads_.gpu) {
      tids[count++] = threads_.gpu;
    }
    if (!count) {
      return;
    }
    bool live = false;
    bool same = false;
    {
      std::lock_guard<std::mutex> lock(g_frame_mutex);
      live = g_frame.session != nullptr;
      same = live && g_frame.thread_count == count &&
             std::equal(tids, tids + count, g_frame.tids);
      if (live && !same && g_api.hint_set_threads) {
        const int result = g_api.hint_set_threads(g_frame.session, tids,
                                                  size_t(count));
        if (result == 0) {
          std::copy(tids, tids + count, g_frame.tids);
          g_frame.thread_count = count;
          g_frame.cpu_valid = false;
          same = true;
          XELOGI("host perf: performance hint session now over {} thread(s)",
                 count);
        } else {
          XELOGW(
              "host perf: the hint session would not take its new threads "
              "(error {}); it is made again",
              result);
        }
      }
    }
    if (same) {
      return;
    }
    if (live) {
      CloseSession("its threads changed");
    }
    if (now < next_session_attempt_ns_) {
      return;
    }
    // Any positive target will do: the first presented frame sets the real
    // one.
    int error = 0;
    void* session =
        CreateHintSession(g_api, tids, count, 2 * kVblankNs, &error);
    if (!session) {
      next_session_attempt_ns_ = now + 30 * kNsPerSec;
      if (!session_refused_logged_) {
        session_refused_logged_ = true;
        XELOGW(
            "host perf: the phone gave no performance hint session for {} "
            "thread(s) (error {}); asked again every 30 s",
            count, error);
      }
      return;
    }
    session_refused_logged_ = false;
    {
      std::lock_guard<std::mutex> lock(g_frame_mutex);
      g_frame.session = session;
      std::copy(tids, tids + count, g_frame.tids);
      g_frame.thread_count = count;
      g_frame.cpu_valid = false;
      g_frame.frame_ns = 0;
    }
    g_session_live.store(true, std::memory_order_relaxed);
    const int64_t rate =
        g_api.hint_preferred_rate_ns
            ? g_api.hint_preferred_rate_ns(g_api.hint_manager)
            : -1;
    XELOGI(
        "host perf: performance hint session over {} thread(s) ({} dispatch, "
        "GPU Commands {}), preferred update rate {} ms",
        count, count - (threads_.gpu ? 1 : 0), threads_.gpu ? "in" : "missing",
        rate > 0 ? double(rate) / kNsPerMs : -1.0);
  }

  // -- the log ---------------------------------------------------------------

  void Describe() {
    std::string text = "host perf: ";
    Appendf(text, "%d cores, capacity%s", topology_.cpu_count,
            topology_.capacity_from_kernel
                ? ""
                : (topology_.have_max_khz ? " (from top frequency)"
                                          : " (unknown, taken as equal)"));
    for (int cpu = 0; cpu < topology_.cpu_count; ++cpu) {
      Appendf(text, " %d:%d", cpu, topology_.capacity[cpu]);
    }
    if (topology_.have_max_khz) {
      text += ", top MHz";
      for (int cpu = 0; cpu < topology_.cpu_count; ++cpu) {
        Appendf(text, " %d:%lld", cpu, topology_.max_khz[cpu] / 1000);
      }
    }
    char cpuset[128] = "?";
    if (ReadSmallFile("/proc/self/cpuset", cpuset, sizeof(cpuset))) {
      cpuset[strcspn(cpuset, "\n")] = '\0';
    }
    Appendf(text, "; allowed cpu %s (cpuset %s)", MaskToList(allowed_).c_str(),
            cpuset);
    Appendf(text, "; performance hint %s",
            g_api.hint_missing[0] ? g_api.hint_missing : "available");
    Appendf(text, "; thermal status %s, headroom %s",
            g_api.thermal_manager ? "available" : "not available",
            g_api.thermal_headroom && g_api.thermal_manager ? "available"
                                                            : "not available");
    XELOGI("{}", text);
  }

  void DescribeThread(std::string& text, const char* name, pid_t tid) {
    if (!tid) {
      Appendf(text, " %s missing;", name);
      return;
    }
    const StatSample stat = ReadStat(tid);
    uint64_t mask = 0;
    GetAffinity(tid, &mask);
    Appendf(text, " %s tid %d nice %d cpu %s;", name, int(tid), stat.nice,
            MaskToList(mask).c_str());
  }

  void DescribeThreads() {
    std::string text;
    Appendf(text, "host perf: %d threads in the process;", threads_.total);
    for (int i = 0; i < kDispatchCount; ++i) {
      char name[16];
      snprintf(name, sizeof(name), "G%d", i);
      DescribeThread(text, name, threads_.dispatch[i]);
    }
    DescribeThread(text, "GPU Commands", threads_.gpu);
    Appendf(text, " %d helper(s)", int(threads_.helpers.size()));
    for (pid_t tid : threads_.helpers) {
      const StatSample stat = ReadStat(tid);
      uint64_t mask = 0;
      GetAffinity(tid, &mask);
      Appendf(text, " [tid %d nice %d cpu %s]", int(tid), stat.nice,
              MaskToList(mask).c_str());
    }
    XELOGI("{}", text);
  }

  // Takes the frame counters since the last tick; into the line when there
  // is one.
  void DrainFrameCounters(std::string* text) {
    const uint32_t frames = g_frames.exchange(0, std::memory_order_relaxed);
    const uint32_t longest_us =
        g_longest_interval_us.exchange(0, std::memory_order_relaxed);
    FrameShared frame;
    {
      std::lock_guard<std::mutex> lock(g_frame_mutex);
      frame = g_frame;
      g_frame.reports = 0;
      g_frame.late = 0;
      g_frame.errors = 0;
      g_frame.sum_actual_ns = 0;
    }
    if (!text) {
      return;
    }
    Appendf(*text, " | frames %u longest %.1f ms", frames, longest_us / 1000.0);
    if (!frame.session) {
      *text += cvars::host_perf_hint ? " | hint no session" : " | hint off";
      return;
    }
    Appendf(*text,
            " | hint frame %.1f ms target %.1f ms reports %u mean %.1f ms "
            "late %u",
            double(frame.frame_ns) / kNsPerMs,
            double(frame.target_ns) / kNsPerMs, frame.reports,
            frame.reports
                ? double(frame.sum_actual_ns) / frame.reports / kNsPerMs
                : 0.0,
            frame.late);
    if (frame.errors) {
      Appendf(*text, " errors %u (last %d)", frame.errors, frame.last_error);
    }
  }

  void AppendTracked(std::string& text, const char* name,
                     const Tracked& tracked) {
    if (!tracked.tid) {
      Appendf(text, " %s missing,", name);
      return;
    }
    if (!tracked.have_delta) {
      Appendf(text, " %s -,", name);
      return;
    }
    const StatSample stat = ReadStat(tracked.tid);
    Appendf(text, " %s %.1f/%.1f %lld @%d,", name,
            double(tracked.run_ns) / kNsPerMs,
            double(tracked.wait_ns) / kNsPerMs, tracked.slices, stat.last_cpu);
  }

  void LogLine(int64_t now) {
    std::string text = "host perf";
    if (last_tick_ns_) {
      Appendf(text, " %lld ms", (long long)((now - last_tick_ns_) / kNsPerMs));
    }
    text += " | MHz now/limit";
    for (int cpu = 0; cpu < topology_.cpu_count; ++cpu) {
      if (!(online_ & CpuBit(cpu))) {
        Appendf(text, " %d:off", cpu);
      } else if (!now_khz_[cpu] && !limit_khz_[cpu]) {
        Appendf(text, " %d:?", cpu);
      } else {
        Appendf(text, " %d:%lld/%lld", cpu, now_khz_[cpu] / 1000,
                limit_khz_[cpu] / 1000);
      }
    }
    if (g_api.thermal_manager) {
      Appendf(text, " | thermal %d %s headroom ", thermal_status_,
              ThermalStatusName(thermal_status_));
      if (std::isnan(thermal_headroom_)) {
        text += "-";
      } else {
        Appendf(text, "%.2f", double(thermal_headroom_));
      }
    } else {
      text += " | thermal -";
    }
    text += " | run/wait ms, slices, @core:";
    for (int i = 0; i < kDispatchCount; ++i) {
      char name[16];
      snprintf(name, sizeof(name), "G%d", i);
      AppendTracked(text, name, dispatch_[i]);
    }
    AppendTracked(text, "GPU", gpu_);
    Appendf(text, " helpers(%d) %.1f/%.1f", int(threads_.helpers.size()),
            double(helpers_run_ns_) / kNsPerMs,
            double(helpers_wait_ns_) / kNsPerMs);
    DrainFrameCounters(&text);
    XELOGI("{}", text);
  }

  Topology topology_;
  std::vector<std::string> helper_names_;
  bool idle_ = false;
  bool described_ = false;
  int64_t last_tick_ns_ = 0;

  ThreadSet threads_;
  uint64_t allowed_ = 0;
  uint64_t online_ = 0;
  long long now_khz_[kMaxCpus] = {};
  long long limit_khz_[kMaxCpus] = {};
  int limited_capacity_[kMaxCpus] = {};
  int thermal_status_ = -1;
  float thermal_headroom_ = NAN;
  int headroom_age_ticks_ = 0;
  bool thermal_severe_ = false;

  Tracked dispatch_[kDispatchCount];
  Tracked gpu_;
  std::map<pid_t, SchedSample> helpers_before_;
  long long helpers_run_ns_ = 0;
  long long helpers_wait_ns_ = 0;

  Placement placement_;
  bool have_placement_ = false;
  uint32_t placed_policy_ = 0;
  uint64_t placed_allowed_ = 0;
  uint32_t placed_helper_cores_ = 0;
  bool placed_severe_ = false;
  int ticks_since_rank_ = 0;
  bool ranked_once_ = false;
  int64_t rank_gathering_ns_[kDispatchCount] = {};
  int64_t rank_run_ns_[kDispatchCount] = {};
  std::map<pid_t, uint64_t> applied_;
  std::map<pid_t, int> refused_;

  bool hint_missing_logged_ = false;
  bool session_refused_logged_ = false;
  int64_t next_session_attempt_ns_ = 0;
};

void WorkerMain() {
  // Never freed: the thread lives as long as the process.
  Worker* worker = new Worker();
  worker->Run();
}

// The part of a presented frame that concerns the hint session. Under the
// frame lock, which was only tried.
void ReportFrame(FrameShared& frame, int64_t interval_ns, bool baseline_stale) {
  const uint32_t mode = cvars::host_perf_hint_report ? 1 : 0;
  int64_t frame_ns = FixedFrameNs();
  if (!frame_ns) {
    frame_ns = g_estimated_frame_ns.load(std::memory_order_relaxed);
  }
  if (frame_ns != frame.frame_ns || mode != frame.report_mode) {
    // Mode 0 measures the busiest thread's CPU time, which reaches the whole
    // frame only when that thread never rests: the target is nine tenths of
    // the frame, so the session is asked for more before a frame is lost.
    // Mode 1 measures present to present, which equals the frame time when
    // all is well: its target is the frame time itself.
    const int64_t target_ns = mode ? frame_ns : frame_ns * 9 / 10;
    const int result = g_api.hint_update_target(frame.session, target_ns);
    if (result != 0) {
      ++frame.errors;
      frame.last_error = result;
    }
    frame.frame_ns = frame_ns;
    frame.report_mode = mode;
    frame.target_ns = target_ns;
  }
  int64_t busiest_ns = 0;
  const bool had_baseline = frame.cpu_valid && !baseline_stale;
  for (int i = 0; i < frame.thread_count; ++i) {
    const int64_t cpu_ns = ThreadCpuNs(frame.tids[i]);
    if (had_baseline && cpu_ns >= 0 && frame.last_cpu_ns[i] >= 0) {
      busiest_ns = std::max(busiest_ns, cpu_ns - frame.last_cpu_ns[i]);
    }
    frame.last_cpu_ns[i] = cpu_ns;
  }
  frame.cpu_valid = true;
  const bool late = interval_ns > frame_ns + frame_ns / 10;
  int64_t actual_ns = interval_ns;
  if (!mode && !late) {
    if (!had_baseline) {
      return;
    }
    actual_ns = std::clamp<int64_t>(busiest_ns, 1, interval_ns);
  }
  const int result = g_api.hint_report_actual(frame.session, actual_ns);
  if (result != 0) {
    ++frame.errors;
    frame.last_error = result;
  }
  ++frame.reports;
  frame.late += late ? 1 : 0;
  frame.sum_actual_ns += actual_ns;
}

}  // namespace

void OnGuestFramePresented() {
  if (!g_worker_started.load(std::memory_order_relaxed)) {
    if (!cvars::host_perf_log && !cvars::host_perf_hint &&
        cvars::host_perf_core_policy == 0) {
      return;
    }
    bool expected = false;
    if (g_worker_started.compare_exchange_strong(expected, true)) {
      std::thread(WorkerMain).detach();
    }
  }

  // Everything below is this thread's alone, but for the atomics and what is
  // under the frame lock.
  static int64_t last_present_ns = 0;
  static int64_t intervals[kIntervalRing];
  static int interval_count = 0;
  static int interval_next = 0;
  static int since_estimate = 0;
  static int64_t pending_estimate_ns = 0;
  static bool baseline_stale = true;

  const int64_t now = NowNs();
  const int64_t interval_ns = last_present_ns ? now - last_present_ns : 0;
  last_present_ns = now;
  g_frames.fetch_add(1, std::memory_order_relaxed);
  if (interval_ns <= 0 || interval_ns > kNsPerSec) {
    // The first frame, or the first after a pause or a load: not a frame time.
    baseline_stale = true;
    return;
  }
  const uint32_t interval_us = uint32_t(interval_ns / 1000);
  if (interval_us > g_longest_interval_us.load(std::memory_order_relaxed)) {
    g_longest_interval_us.store(interval_us, std::memory_order_relaxed);
  }

  intervals[interval_next] = interval_ns;
  interval_next = (interval_next + 1) % kIntervalRing;
  interval_count = std::min(interval_count + 1, kIntervalRing);
  if (++since_estimate >= 16 && interval_count >= 32) {
    since_estimate = 0;
    // Taken up only when two estimates in a row agree.
    const int64_t estimate_ns = EstimateFrameNs(intervals, interval_count);
    if (estimate_ns == pending_estimate_ns) {
      g_estimated_frame_ns.store(estimate_ns, std::memory_order_relaxed);
    }
    pending_estimate_ns = estimate_ns;
  }

  if (!g_session_live.load(std::memory_order_relaxed)) {
    baseline_stale = true;
    return;
  }
  std::unique_lock<std::mutex> lock(g_frame_mutex, std::try_to_lock);
  if (!lock.owns_lock()) {
    baseline_stale = true;
    return;
  }
  if (!g_frame.session) {
    baseline_stale = true;
    return;
  }
  ReportFrame(g_frame, interval_ns, baseline_stale);
  baseline_stale = false;
}

}  // namespace host_perf
}  // namespace xe

#else  // !defined(__linux__)

namespace xe {
namespace host_perf {

void OnGuestFramePresented() {}

}  // namespace host_perf
}  // namespace xe

#endif  // defined(__linux__)
