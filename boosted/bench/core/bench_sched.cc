// hakuX-Boosted bench, section 2: the guest scheduler's real paths.
//
// Guest threads are created with the core's own XThread::Create and run
// PowerPC loops written for this rig (core/ppc_asm.h). The loops call the
// core's own kernel exports through import thunks of the shape the core
// writes for a title's imports ("sc 2; blr"), so a call goes: translated
// code, the backend's guest-to-host thunk, the export's shim, the kernel
// function, GuestScheduler. Nothing between the thunk and the scheduler is
// the harness's.
//
// Every benchmark runs for a fixed time: the guest loop counts its passes
// and leaves when the harness raises a flag in guest memory. The figure is
// the time between two calls the loop makes to the harness (one before its
// first pass, one after its last) divided by the passes, so thread creation
// and exit are outside it. "cores" is the process's CPU time over the same
// stretch divided by the wall time: what the path costs in heat, which is
// what parking a spinner changes when the time per call does not.
#include <sys/auxv.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/host_thread_state.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/object_table.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

#include "bench.h"
#include "bench_env.h"
#include "ppc_asm.h"

DECLARE_bool(host_thread_state_slot);

namespace bench {
namespace {

using xe::cpu::ppc::PPCContext;
using Asm = ppc::Assembler;

// The block of guest memory a benchmark thread is started with (r3).
enum ParamOffset : int {
  kCount = 4,       // out: passes of the loop
  kStop = 8,        // in: nonzero ends the loop
  kSlot = 12,       // which Mark the thread's two harness calls fill in
  kZeroDelay = 16,  // a 64-bit zero, the interval of a zero delay
  kWaitOn = 24,     // handle of the event this thread waits on
  kSignal = 28,     // handle of the event this thread sets
  kParamSize = 64,
};

struct Mark {
  std::atomic<uint64_t> begin_ns{0};
  std::atomic<uint64_t> end_ns{0};
};
constexpr int kMaxThreads = 4;
Mark g_marks[kMaxThreads];
Env* g_env = nullptr;

void HostBegin(PPCContext* context, xe::kernel::KernelState*) {
  uint8_t* params = g_env->Translate(uint32_t(context->r[3]));
  uint32_t slot = xe::load_and_swap<uint32_t>(params + kSlot);
  g_marks[slot % kMaxThreads].begin_ns.store(NowNs());
}

void HostEnd(PPCContext* context, xe::kernel::KernelState*) {
  uint8_t* params = g_env->Translate(uint32_t(context->r[3]));
  uint32_t slot = xe::load_and_swap<uint32_t>(params + kSlot);
  g_marks[slot % kMaxThreads].end_ns.store(NowNs());
}

struct Thunks {
  uint32_t begin = 0, end = 0;
  uint32_t process_type = 0;  // KeGetCurrentProcessType: a call that does nothing
  uint32_t yield = 0;         // NtYieldExecution
  uint32_t delay = 0;         // KeDelayExecutionThread
  uint32_t set_event = 0;     // NtSetEvent
  uint32_t wait = 0;          // NtWaitForSingleObjectEx
};

enum class Body {
  kNothing,     // the loop alone, no kernel call
  kKernelCall,  // a kernel export that does nothing
  kYield,       // NtYieldExecution
  kZeroDelay,   // KeDelayExecutionThread with a zero interval
  kPing,        // set the partner's event, then wait on its own
  kPong,        // wait on its own event, then set the partner's
};

void Prologue(Asm& a) {
  a.mflr(0);
  a.stw(0, -8, 1);
  a.stwu(1, -112, 1);
  a.stw(29, 92, 1);
  a.stw(30, 96, 1);
  a.stw(31, 100, 1);
  a.mr(31, 3);
  a.li(29, 0);
}

void Epilogue(Asm& a) {
  a.stw(29, kCount, 31);
  a.lwz(29, 92, 1);
  a.lwz(30, 96, 1);
  a.lwz(31, 100, 1);
  a.addi(1, 1, 112);
  a.lwz(0, -8, 1);
  a.mtlr(0);
  a.li(3, 0);
  a.blr();
}

void EmitWait(Asm& a, const Thunks& t) {
  a.lwz(3, kWaitOn, 31);
  a.li(4, 0);
  a.li(5, 0);
  a.li(6, 0);
  a.bl_abs(t.wait);
}

void EmitSet(Asm& a, const Thunks& t) {
  a.lwz(3, kSignal, 31);
  a.li(4, 0);
  a.bl_abs(t.set_event);
}

// One guest thread function for a loop body.
uint32_t BuildThreadFunction(Env& env, const Thunks& t, Body body) {
  Asm a(env.NextCodeAddress());
  Asm::Label loop;
  Prologue(a);
  a.mr(3, 31);
  a.bl_abs(t.begin);
  a.Bind(loop);
  switch (body) {
    case Body::kNothing:
      break;
    case Body::kKernelCall:
      a.bl_abs(t.process_type);
      break;
    case Body::kYield:
      a.bl_abs(t.yield);
      break;
    case Body::kZeroDelay:
      a.li(3, 0);
      a.li(4, 0);
      a.addi(5, 31, kZeroDelay);
      a.bl_abs(t.delay);
      break;
    case Body::kPing:
      EmitSet(a, t);
      EmitWait(a, t);
      break;
    case Body::kPong:
      // Sets after it wakes and before it looks at the flag, so its partner
      // is never left waiting when the run ends.
      EmitWait(a, t);
      EmitSet(a, t);
      break;
  }
  a.addi(29, 29, 1);
  a.lwz(9, kStop, 31);
  a.cmpwi(6, 9, 0);
  a.beq(6, loop);
  a.stw(29, kCount, 31);
  a.mr(3, 31);
  a.bl_abs(t.end);
  if (body == Body::kPing) {
    // Lets the partner out of its last wait.
    EmitSet(a, t);
  }
  Epilogue(a);
  return env.PlaceCode(a);
}

uint64_t ProcessCpuNs() {
  timespec ts;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

struct ThreadPlan {
  Body body;
  uint32_t function;
  int guest_cpu;
  bool timed;  // its passes and its marks are what is reported
};

struct Sample {
  bool ok = false;
  double ns_per_pass = 0;
  double cores = 0;
  uint64_t passes = 0;
};

uint64_t SampleMs() {
  const char* env = std::getenv("BENCH_SCHED_MS");
  uint64_t ms = env ? std::strtoull(env, nullptr, 10) : 60;
  return ms ? ms : 60;
}

// Runs the planned threads for one sample.
Sample RunSample(Env& env, const std::vector<ThreadPlan>& plan,
                 bool with_events) {
  using namespace xe::kernel;
  Sample sample;
  auto* ks = env.kernel_state;
  const size_t n = plan.size();
  std::vector<uint32_t> params(n);
  std::vector<object_ref<XEvent>> events;
  if (with_events) {
    for (size_t i = 0; i < n; ++i) {
      auto ev = object_ref<XEvent>(new XEvent(ks));
      ev->Initialize(false, false);  // auto-reset, not signalled
      events.push_back(ev);
    }
  }
  for (size_t i = 0; i < n; ++i) {
    params[i] = env.AllocData(kParamSize);
    uint8_t* p = env.Translate(params[i]);
    xe::store_and_swap<uint32_t>(p + kSlot, uint32_t(i));
    if (with_events) {
      xe::store_and_swap<uint32_t>(p + kWaitOn, events[i]->handle());
      xe::store_and_swap<uint32_t>(p + kSignal, events[(i + 1) % n]->handle());
    }
    g_marks[i].begin_ns.store(0);
    g_marks[i].end_ns.store(0);
  }

  std::vector<object_ref<XThread>> threads;
  // The waiting side of a pair first, so the first set finds it waiting.
  for (size_t k = n; k-- > 0;) {
    const ThreadPlan& tp = plan[k];
    uint32_t flags = uint32_t(1u << tp.guest_cpu) << 24;
    auto thread = object_ref<XThread>(
        new XThread(ks, 64 * 1024, 0, tp.function, params[k], flags, true));
    thread->set_name("bench" + std::to_string(k));
    if (XFAILED(thread->Create())) {
      std::fprintf(stderr, "bench: XThread::Create failed\n");
      return sample;
    }
    threads.push_back(thread);
  }

  // Let every thread reach its loop, then measure CPU over a known stretch.
  const uint64_t settle_deadline = NowNs() + 2000000000ull;
  for (;;) {
    bool all = true;
    for (size_t i = 0; i < n; ++i) {
      if (plan[i].timed && !g_marks[i].begin_ns.load()) {
        all = false;
      }
    }
    if (all) {
      break;
    }
    if (NowNs() > settle_deadline) {
      std::fprintf(stderr, "bench: a guest thread never started its loop\n");
      return sample;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  const uint64_t wall0 = NowNs();
  const uint64_t cpu0 = ProcessCpuNs();
  std::this_thread::sleep_for(std::chrono::milliseconds(SampleMs()));
  const uint64_t cpu1 = ProcessCpuNs();
  const uint64_t wall1 = NowNs();
  for (size_t i = 0; i < n; ++i) {
    xe::store_and_swap<uint32_t>(env.Translate(params[i]) + kStop, 1);
  }

  bool exited = true;
  for (auto& thread : threads) {
    auto result = xe::threading::Wait(ThreadExitHandle(thread.get()), false,
                                      std::chrono::milliseconds(15000));
    if (result != xe::threading::WaitResult::kSuccess) {
      exited = false;
    }
  }
  if (!exited) {
    std::fprintf(stderr, "bench: a guest thread did not leave its loop\n");
    return sample;
  }

  uint64_t passes = 0;
  uint64_t begin = ~0ull, end = 0;
  for (size_t i = 0; i < n; ++i) {
    if (!plan[i].timed) {
      continue;
    }
    passes += xe::load_and_swap<uint32_t>(env.Translate(params[i]) + kCount);
    begin = std::min(begin, g_marks[i].begin_ns.load());
    end = std::max(end, g_marks[i].end_ns.load());
  }
  threads.clear();
  for (auto& ev : events) {
    ev->ReleaseHandle();
  }
  for (size_t i = 0; i < n; ++i) {
    env.memory->SystemHeapFree(params[i]);
  }
  if (!passes || end <= begin) {
    return sample;
  }
  sample.ok = true;
  sample.passes = passes;
  sample.ns_per_pass = double(end - begin) / double(passes);
  sample.cores = double(cpu1 - cpu0) / double(wall1 - wall0);
  return sample;
}

int SchedSamples() {
  const char* env = std::getenv("BENCH_SCHED_SAMPLES");
  int n = env ? std::atoi(env) : 5;
  return n > 0 ? n : 5;
}

// Runs one benchmark for several samples and reports the fastest.
bool RunBenchmark(Env& env, const std::string& name, const std::string& label,
                  const std::vector<ThreadPlan>& plan, bool with_events) {
  const std::string s = "sched";
  std::vector<Sample> samples;
  for (int i = 0; i < SchedSamples(); ++i) {
    Sample sample = RunSample(env, plan, with_events);
    if (!sample.ok) {
      EmitNote(s, name + " (" + label + ")",
               "NOT MEASURED: the guest threads did not run to the end (see "
               "the log of the run)");
      return false;
    }
    samples.push_back(sample);
  }
  std::sort(samples.begin(), samples.end(),
            [](const Sample& a, const Sample& b) {
              return a.ns_per_pass < b.ns_per_pass;
            });
  Emit(s, name, label, "ns_per_op", samples.front().ns_per_pass);
  Emit(s, name, label, "ns_per_op_median",
       samples[samples.size() / 2].ns_per_pass);
  std::vector<double> cores;
  for (const auto& sample : samples) {
    cores.push_back(sample.cores);
  }
  std::sort(cores.begin(), cores.end());
  Emit(s, name, label, "cores_busy", cores[cores.size() / 2]);
  return true;
}

// Is this library's thread-local storage reached through the loader's
// dynamic resolver (as on Android) or at a fixed offset?
thread_local int t_probe;
struct ProbeResult {
  intptr_t offset;
};
void ProbeThread(ProbeResult* out) {
  out->offset = reinterpret_cast<intptr_t>(&t_probe) -
                reinterpret_cast<intptr_t>(__builtin_thread_pointer());
}
std::string TlsMode() {
  ProbeResult here{0}, there{0};
  ProbeThread(&here);
  std::thread other(ProbeThread, &there);
  other.join();
  return here.offset == there.offset
             ? "static (fixed offset; not what Android gives the app)"
             : "dynamic (through the loader's resolver, as on Android)";
}

// The slot host_thread_state_slot uses must be the launcher's own word.
bool SlotUsable(void* slot_address) {
  return slot_address ==
         static_cast<char*>(__builtin_thread_pointer()) + 16;
}

std::atomic<bool> g_watchdog_done{false};
void StartWatchdog(const std::string& section, const std::string& label) {
  std::thread([section, label]() {
    for (int i = 0; i < 1800 && !g_watchdog_done.load(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!g_watchdog_done.load()) {
      EmitNote(section, "(" + label + ")",
               "NOT MEASURED: the benchmark program was still running after "
               "three minutes and was stopped");
      std::fflush(nullptr);
      _exit(4);
    }
  }).detach();
}

bool SlotRequestedButUnusable(const std::string& section,
                              const std::string& label, void* slot_address) {
  // Read through the registry by the core itself; here only the launcher's
  // side of the bargain is checked.
  if (SlotUsable(slot_address)) {
    return false;
  }
  EmitNote(section, "(" + label + ")",
           "host_thread_state_slot cannot be used here: the launcher's "
           "thread-local word is not at the thread pointer plus 16");
  return true;
}

}  // namespace

int RunSchedSuite(const std::string& label, void* slot_address) {
  const std::string s = "sched";
  if (cvars::host_thread_state_slot &&
      SlotRequestedButUnusable(s, label, slot_address)) {
    return 3;
  }
  StartWatchdog(s, label);
  Env* env = CreateEnv(true);
  g_env = env;
  EmitNote(s, "thread-locals (" + label + ")", TlsMode());

  Thunks t;
  t.begin = env->ImportThunk("BenchBegin", HostBegin);
  t.end = env->ImportThunk("BenchEnd", HostEnd);
  t.process_type = env->ImportKernelExport("KeGetCurrentProcessType");
  t.yield = env->ImportKernelExport("NtYieldExecution");
  t.delay = env->ImportKernelExport("KeDelayExecutionThread");
  t.set_event = env->ImportKernelExport("NtSetEvent");
  t.wait = env->ImportKernelExport("NtWaitForSingleObjectEx");
  if (!t.process_type || !t.yield || !t.delay || !t.set_event || !t.wait) {
    EmitNote(s, "(" + label + ")",
             "NOT MEASURED: this tree's xboxkrnl_threading.cc does not "
             "register one of the exports the benchmarks call");
    return 1;
  }

  const uint32_t f_nothing = BuildThreadFunction(*env, t, Body::kNothing);
  const uint32_t f_call = BuildThreadFunction(*env, t, Body::kKernelCall);
  const uint32_t f_yield = BuildThreadFunction(*env, t, Body::kYield);
  const uint32_t f_delay = BuildThreadFunction(*env, t, Body::kZeroDelay);
  const uint32_t f_ping = BuildThreadFunction(*env, t, Body::kPing);
  const uint32_t f_pong = BuildThreadFunction(*env, t, Body::kPong);

  int status = 0;
  auto run = [&](const std::string& name, std::vector<ThreadPlan> plan,
                 bool with_events) {
    if (!RunBenchmark(*env, name, label, plan, with_events)) {
      status = 1;
    }
  };
  // The loop alone, and a kernel call that does nothing: what to subtract.
  run("loop_alone", {{Body::kNothing, f_nothing, 0, true}}, false);
  run("kernel_call_that_does_nothing",
      {{Body::kKernelCall, f_call, 0, true}}, false);
  // One thread on its CPU with nothing else ready there.
  run("yield_nothing_ready", {{Body::kYield, f_yield, 0, true}}, false);
  run("zero_delay_nothing_ready", {{Body::kZeroDelay, f_delay, 0, true}},
      false);
  // Two threads of one priority on one CPU, both yielding: every yield
  // switches to the other fiber. Per yield.
  run("yield_other_ready",
      {{Body::kYield, f_yield, 0, true}, {Body::kYield, f_yield, 0, true}},
      false);
  // Two threads handing an event back and forth. One pass is a set and a
  // blocking wait on each side: a full round trip.
  run("wait_wake_round_trip_one_cpu",
      {{Body::kPing, f_ping, 0, true}, {Body::kPong, f_pong, 0, false}}, true);
  run("wait_wake_round_trip_two_cpus",
      {{Body::kPing, f_ping, 0, true}, {Body::kPong, f_pong, 1, false}}, true);

  g_watchdog_done.store(true);
  return status;
}

// --- Section 1b: the core's Fiber class -------------------------------------
namespace {
xe::threading::Fiber* g_main_fiber = nullptr;
xe::threading::Fiber* g_other_fiber = nullptr;
}  // namespace

int RunFiberSuite(const std::string& label, void* slot_address) {
  const std::string s = "fiber";
  if (cvars::host_thread_state_slot &&
      SlotRequestedButUnusable(s, label, slot_address)) {
    return 3;
  }
  PinToCurrentCpu();
  EmitNote(s, "thread-locals (" + label + ")", TlsMode());
  auto main_fiber = xe::threading::Fiber::CreateFromThread();
  g_main_fiber = main_fiber.get();
  xe::threading::Fiber::CreationParameters params;
  params.stack_size = 1024 * 1024;
  auto other = xe::threading::Fiber::Create(params, []() {
    for (;;) {
      g_main_fiber->SwitchTo();
    }
  });
  g_other_fiber = other.get();
  if (!g_main_fiber || !g_other_fiber) {
    EmitNote(s, "(" + label + ")", "NOT MEASURED: no fiber could be created");
    return 1;
  }
  g_other_fiber->SwitchTo();
  Timing timing = Measure([] { g_other_fiber->SwitchTo(); });
  // One operation is a switch away and a switch back.
  timing.min_ns /= 2;
  timing.median_ns /= 2;
  EmitTiming(s, "fiber_switch_to", label, timing);
  return 0;
}

}  // namespace bench
