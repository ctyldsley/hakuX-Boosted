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
#include <mutex>
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
  kShared = 32,     // address of the block the threads of one run share
  kParamSize = 64,
};

// The shared block of a busy-wait run (see "Busy waits" below).
enum SharedOffset : int {
  kFlag = 0,  // the word a busy-waiting thread polls; nonzero ends its wait
  kSharedSize = 64,
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
  uint32_t pace = 0;          // harness: sleeps the calling host thread
  uint32_t produce = 0;       // harness: after a delay, makes the flag true
  uint32_t consume = 0;       // harness: the waiter saw the flag
};

enum class Body {
  kNothing,     // the loop alone, no kernel call
  kKernelCall,  // a kernel export that does nothing
  kYield,       // NtYieldExecution
  kZeroDelay,   // KeDelayExecutionThread with a zero interval
  kPing,        // set the partner's event, then wait on its own
  kPong,        // wait on its own event, then set the partner's
  // Busy waits (boosted/notes/park-rework.md). A waiter spins until the
  // shared flag is nonzero, tells the harness, and spins again.
  kSpinYield,      // ... calling NtYieldExecution on every pass
  kSpinZeroDelay,  // ... calling KeDelayExecutionThread(0) on every pass
  kSpinZeroWait,   // ... polling an event with a zero timeout on every pass
  kSpinPollSled,   // ... a counted loop of no-op register moves, then the load
  kSpinPollHint,   // ... the pause hint (or r31,r31,r31) inline, one block
  kSpinPollPlain,  // ... load, compare, branch and nothing else
  kSpinPollClock,  // ... with a time-base read each pass (a deadline loop)
  kSpinLock,       // ... a lwarx/stwcx. lock with a pause-and-load wait loop
  kNoise,          // sets an event nobody waits on, several thousand a second
  kProducer,       // after a known delay makes the flag true (a plain store)
  kProducerSignal, // the same, and then sets an event nobody waits on
  kProducerSignalWaiter,  // the same, and then sets the event the waiter polls
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

// --- Busy waits ---------------------------------------------------------------
// What the phone showed (Fable II, 2026-11-14): a title sets events and
// leaves critical sections thousands of times a second, and a thread that
// busy-waits beside that was never parked. These threads reproduce it: a
// waiter that spins on a word of memory, a thread that sets an unrelated
// event at a steady rate, and a producer that makes the word true a known
// time after the waiter last saw it. The harness's part is three calls.
std::atomic<uint64_t> g_produce_ns{0};  // when the flag was last made true
std::atomic<bool> g_run_over{false};
std::mutex g_latency_mutex;
std::vector<double> g_latency_us;
std::atomic<uint64_t> g_noise_events{0};

uint64_t EnvUs(const char* name, uint64_t fallback) {
  const char* env = std::getenv(name);
  uint64_t value = env ? std::strtoull(env, nullptr, 10) : fallback;
  return value ? value : fallback;
}

std::atomic<uint32_t>* SharedFlag(PPCContext* context) {
  uint8_t* params = g_env->Translate(uint32_t(context->r[3]));
  uint32_t shared = xe::load_and_swap<uint32_t>(params + kShared);
  return reinterpret_cast<std::atomic<uint32_t>*>(g_env->Translate(shared) +
                                                   kFlag);
}

// The noise thread's pause between two events: it sleeps its own host
// thread, so the events come at a steady few thousand a second and the
// thread itself costs next to nothing (what is left in "cores busy" is the
// waiter).
void HostPace(PPCContext*, xe::kernel::KernelState*) {
  g_noise_events.fetch_add(1, std::memory_order_relaxed);
  std::this_thread::sleep_for(
      std::chrono::microseconds(EnvUs("BENCH_NOISE_GAP_US", 100)));
}

// The producer: waits until the waiter has taken the last one, sleeps the
// known delay, then makes the flag true and notes when.
void HostProduce(PPCContext* context, xe::kernel::KernelState*) {
  auto* flag = SharedFlag(context);
  while (flag->load() != 0 && !g_run_over.load()) {
    std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  const uint64_t until = NowNs() + EnvUs("BENCH_PRODUCE_DELAY_US", 3000) * 1000;
  while (NowNs() < until && !g_run_over.load()) {
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  if (g_run_over.load()) {
    return;
  }
  g_produce_ns.store(NowNs());
  flag->store(0x01000000);  // nonzero whichever way round it is read
}

// The waiter saw the flag: how long after it was made true.
void HostConsume(PPCContext* context, xe::kernel::KernelState*) {
  const uint64_t now = NowNs();
  const uint64_t produced = g_produce_ns.exchange(0);
  if (produced && now > produced) {
    std::lock_guard<std::mutex> lock(g_latency_mutex);
    g_latency_us.push_back(double(now - produced) / 1000.0);
  }
  SharedFlag(context)->store(0);
}

// The waiters. r31 = the thread's block, r30 = the shared block, r29 = the
// count reported (waits completed). The poll loops keep no counter of their
// passes on purpose: a wait that counts its passes is not one that can be
// parked on "nothing of its own changes" (the retry loop of a spin count is
// the case that must be left alone).
uint32_t BuildWaiter(Env& env, const Thunks& t, Body body) {
  Asm a(env.NextCodeAddress());
  Asm::Label loop, pause, got, out, retry;
  Prologue(a);
  a.lwz(30, kShared, 31);
  a.mr(3, 31);
  a.bl_abs(t.begin);
  a.Bind(loop);
  switch (body) {
    case Body::kSpinYield:
    case Body::kSpinZeroDelay:
    case Body::kSpinZeroWait:
      a.lwz(9, kFlag, 30);
      a.cmpwi(6, 9, 0);
      a.bne(6, got);
      a.lwz(9, kStop, 31);
      a.cmpwi(6, 9, 0);
      a.bne(6, out);
      if (body == Body::kSpinYield) {
        a.bl_abs(t.yield);
      } else if (body == Body::kSpinZeroDelay) {
        a.li(3, 0);
        a.li(4, 0);
        a.addi(5, 31, kZeroDelay);
        a.bl_abs(t.delay);
      } else {
        // NtWaitForSingleObjectEx(handle, wait mode, alertable, &zero).
        a.lwz(3, kWaitOn, 31);
        a.li(4, 0);
        a.li(5, 0);
        a.addi(6, 31, kZeroDelay);
        a.bl_abs(t.wait);
      }
      a.b(loop);
      break;
    case Body::kSpinPollSled:
      // The shape of the phone's profile: a counted loop of no-op register
      // moves (not r31: the other register numbers of the console's idiom),
      // then loads and compares, and round again.
      a.li(10, 8);
      a.mtctr(10);
      a.Bind(pause);
      a.or_(1, 1, 1);
      a.or_(1, 1, 1);
      a.bdnz(pause);
      a.lwz(9, kFlag, 30);
      a.cmpwi(6, 9, 0);
      a.bne(6, got);
      a.lwz(9, kStop, 31);
      a.cmpwi(6, 9, 0);
      a.beq(6, loop);
      a.b(out);
      break;
    case Body::kSpinPollHint:
      a.or_(31, 31, 31);  // db16cyc, the console's pause hint
      a.lwz(9, kFlag, 30);
      a.cmpwi(6, 9, 0);
      a.bne(6, got);
      a.lwz(9, kStop, 31);
      a.cmpwi(6, 9, 0);
      a.beq(6, loop);
      a.b(out);
      break;
    case Body::kSpinPollPlain:
      a.lwz(9, kFlag, 30);
      a.cmpwi(6, 9, 0);
      a.bne(6, got);
      a.lwz(9, kStop, 31);
      a.cmpwi(6, 9, 0);
      a.beq(6, loop);
      a.b(out);
      break;
    case Body::kSpinPollClock:
      a.Raw(0x7D4C42E6);  // mftb r10
      a.or_(31, 31, 31);
      a.lwz(9, kFlag, 30);
      a.cmpwi(6, 9, 0);
      a.bne(6, got);
      a.lwz(9, kStop, 31);
      a.cmpwi(6, 9, 0);
      a.beq(6, loop);
      a.b(out);
      break;
    case Body::kSpinLock:
      // while (flag == 0) pause;  then take the word with lwarx/stwcx. and,
      // having it, treat that as the flag seen. The wait is the inner loop;
      // the atomic part belongs to the outer one.
      a.or_(31, 31, 31);
      a.lwz(9, kFlag, 30);
      a.cmpwi(6, 9, 0);
      a.bne(6, retry);
      a.lwz(9, kStop, 31);
      a.cmpwi(6, 9, 0);
      a.beq(6, loop);
      a.b(out);
      a.Bind(retry);
      a.li(10, 0);
      a.Raw(0x7D20F028);  // lwarx r9, 0, r30
      a.cmpwi(6, 9, 0);
      a.beq(6, loop);
      a.addi(11, 30, 60);  // a scratch word: the flag is the harness's
      a.Raw(0x7D40592D);   // stwcx. r10, 0, r11
      a.b(got);
      break;
    default:
      break;
  }
  a.Bind(got);
  a.addi(29, 29, 1);
  a.mr(3, 31);
  a.bl_abs(t.consume);
  a.b(loop);
  a.Bind(out);
  a.stw(29, kCount, 31);
  a.mr(3, 31);
  a.bl_abs(t.end);
  Epilogue(a);
  return env.PlaceCode(a);
}

// The noise thread and the producers: one harness call a pass.
uint32_t BuildHelper(Env& env, const Thunks& t, Body body) {
  Asm a(env.NextCodeAddress());
  Asm::Label loop;
  Prologue(a);
  a.mr(3, 31);
  a.bl_abs(t.begin);
  a.Bind(loop);
  if (body == Body::kNoise) {
    EmitSet(a, t);
    a.bl_abs(t.pace);
  } else {
    a.mr(3, 31);
    a.bl_abs(t.produce);
    if (body == Body::kProducerSignal ||
        body == Body::kProducerSignalWaiter) {
      EmitSet(a, t);
    }
  }
  a.addi(29, 29, 1);
  a.lwz(9, kStop, 31);
  a.cmpwi(6, 9, 0);
  a.beq(6, loop);
  a.stw(29, kCount, 31);
  a.mr(3, 31);
  a.bl_abs(t.end);
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
  std::vector<double> latency_us;  // busy-wait runs: flag true to flag seen
  uint64_t noise_events = 0;
};

uint64_t SampleMs() {
  const char* env = std::getenv("BENCH_SCHED_MS");
  uint64_t ms = env ? std::strtoull(env, nullptr, 10) : 60;
  return ms ? ms : 60;
}

// Runs the planned threads for one sample.
Sample RunSample(Env& env, const std::vector<ThreadPlan>& plan,
                 bool with_events, uint64_t sample_ms = SampleMs()) {
  using namespace xe::kernel;
  Sample sample;
  auto* ks = env.kernel_state;
  const size_t n = plan.size();
  std::vector<uint32_t> params(n);
  std::vector<object_ref<XEvent>> events;
  if (with_events) {
    // One more than the threads: the last is the event of a busy-wait run
    // that is set and never waited on.
    for (size_t i = 0; i <= n; ++i) {
      auto ev = object_ref<XEvent>(new XEvent(ks));
      ev->Initialize(false, false);  // auto-reset, not signalled
      events.push_back(ev);
    }
  }
  const uint32_t shared = env.AllocData(kSharedSize);
  g_run_over.store(false);
  g_produce_ns.store(0);
  g_noise_events.store(0);
  {
    std::lock_guard<std::mutex> lock(g_latency_mutex);
    g_latency_us.clear();
  }
  for (size_t i = 0; i < n; ++i) {
    params[i] = env.AllocData(kParamSize);
    uint8_t* p = env.Translate(params[i]);
    xe::store_and_swap<uint32_t>(p + kSlot, uint32_t(i));
    xe::store_and_swap<uint32_t>(p + kShared, shared);
    if (with_events) {
      xe::store_and_swap<uint32_t>(p + kWaitOn, events[i]->handle());
      // In a busy-wait run every set goes to the spare event, except the
      // producer's whose job is to signal what the waiter (thread 0) polls.
      xe::store_and_swap<uint32_t>(
          p + kSignal,
          plan[i].body == Body::kProducerSignalWaiter
              ? events[0]->handle()
              : plan[0].body >= Body::kSpinYield
                    ? events[n]->handle()
                    : events[(i + 1) % n]->handle());
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
  std::this_thread::sleep_for(std::chrono::milliseconds(sample_ms));
  const uint64_t cpu1 = ProcessCpuNs();
  const uint64_t wall1 = NowNs();
  g_run_over.store(true);
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
  env.memory->SystemHeapFree(shared);
  {
    std::lock_guard<std::mutex> lock(g_latency_mutex);
    sample.latency_us = g_latency_us;
  }
  sample.noise_events = g_noise_events.load();
  if (plan[0].body >= Body::kSpinYield) {
    // A busy-wait run: a waiter that was never answered has no passes to
    // report, and that is a result, not a failure.
    sample.ok = end > begin;
    sample.passes = passes;
    sample.ns_per_pass = 0;
    sample.cores = double(cpu1 - cpu0) / double(wall1 - wall0);
    return sample;
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

// A busy-wait run: what it costs in cores, and how long after the flag
// became true the waiter saw it. All samples' latencies are taken together.
bool RunWaitBenchmark(Env& env, const std::string& name,
                      const std::string& label,
                      const std::vector<ThreadPlan>& plan) {
  const std::string s = "sched";
  std::vector<double> cores;
  std::vector<double> latency;
  uint64_t noise = 0;
  double seconds = 0;
  // Shorter and fewer than the other benchmarks' samples: there are many of
  // these runs, and the job that runs them has twenty minutes.
  const uint64_t wait_ms = EnvUs("BENCH_WAIT_MS", 100);
  const int wait_samples = int(EnvUs("BENCH_WAIT_SAMPLES", 2));
  for (int i = 0; i < wait_samples; ++i) {
    Sample sample = RunSample(env, plan, true, wait_ms);
    if (!sample.ok) {
      EmitNote(s, name + " (" + label + ")",
               "NOT MEASURED: the guest threads did not run to the end (see "
               "the log of the run)");
      return false;
    }
    cores.push_back(sample.cores);
    latency.insert(latency.end(), sample.latency_us.begin(),
                   sample.latency_us.end());
    noise += sample.noise_events;
    seconds += double(wait_ms) / 1000.0;
  }
  std::sort(cores.begin(), cores.end());
  Emit(s, name, label, "cores_busy", cores[cores.size() / 2]);
  if (noise && seconds > 0) {
    Emit(s, name, label, "unrelated_events_per_s", double(noise) / seconds);
  }
  if (!latency.empty()) {
    std::sort(latency.begin(), latency.end());
    Emit(s, name, label, "wake_latency_us", latency[latency.size() / 2]);
    Emit(s, name, label, "wake_latency_us_p95",
         latency[std::min(latency.size() - 1, latency.size() * 95 / 100)]);
    Emit(s, name, label, "wake_latency_us_worst", latency.back());
    Emit(s, name, label, "waits_answered", double(latency.size()));
  }
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
#ifdef BENCH_CXX_LIBRARY
  EmitNote(s, "c++ library", BENCH_CXX_LIBRARY);
#endif

  Thunks t;
  t.begin = env->ImportThunk("BenchBegin", HostBegin);
  t.end = env->ImportThunk("BenchEnd", HostEnd);
  t.process_type = env->ImportKernelExport("KeGetCurrentProcessType");
  t.yield = env->ImportKernelExport("NtYieldExecution");
  t.delay = env->ImportKernelExport("KeDelayExecutionThread");
  t.set_event = env->ImportKernelExport("NtSetEvent");
  t.wait = env->ImportKernelExport("NtWaitForSingleObjectEx");
  t.pace = env->ImportThunk("BenchPace", HostPace);
  t.produce = env->ImportThunk("BenchProduce", HostProduce);
  t.consume = env->ImportThunk("BenchConsume", HostConsume);
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
  // BENCH_SCHED_ONLY=<name> runs one benchmark, for looking at it alone.
  const char* only = std::getenv("BENCH_SCHED_ONLY");
  auto run = [&](const std::string& name, std::vector<ThreadPlan> plan,
                 bool with_events) {
    if (only && name != only) {
      return;
    }
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

  // --- Busy waits (boosted/notes/park-rework.md) ---
  // The waiter on guest CPU 0; the thread that sets unrelated events on 1;
  // the producer of what the waiter waits for on 2. "cores busy" is then
  // the waiter's cost: the other two sleep in the harness between calls.
  // Only for the sets of settings that ask for them (BENCH_BUSY_WAITS=1,
  // which run-core.sh gives the rows these runs are about), or by name.
  const bool busy_waits = std::getenv("BENCH_BUSY_WAITS") != nullptr;
  auto run_wait = [&](const std::string& name, std::vector<ThreadPlan> plan) {
    if (only ? name != only : !busy_waits) {
      return;
    }
    if (!RunWaitBenchmark(*env, name, label, plan)) {
      status = 1;
    }
  };
  struct WaiterKind {
    const char* name;
    Body body;
  };
  const WaiterKind kinds[] = {
      {"yield", Body::kSpinYield},
      {"zero_delay", Body::kSpinZeroDelay},
      {"zero_timeout_wait", Body::kSpinZeroWait},
      {"poll_sled", Body::kSpinPollSled},
      {"poll_hint", Body::kSpinPollHint},
      {"poll_plain", Body::kSpinPollPlain},
      {"poll_clock", Body::kSpinPollClock},
      {"spin_lock", Body::kSpinLock},
  };
  const uint32_t f_noise = BuildHelper(*env, t, Body::kNoise);
  const uint32_t f_produce = BuildHelper(*env, t, Body::kProducer);
  const uint32_t f_produce_signal =
      BuildHelper(*env, t, Body::kProducerSignal);
  const uint32_t f_produce_signal_waiter =
      BuildHelper(*env, t, Body::kProducerSignalWaiter);
  for (const WaiterKind& kind : kinds) {
    const uint32_t f = BuildWaiter(*env, t, kind.body);
    const std::string base = std::string("busy_wait_") + kind.name;
    const bool main_kind =
        kind.body == Body::kSpinYield || kind.body == Body::kSpinPollSled;
    // Nothing ever answers and nothing else happens: is the shape parked?
    run_wait(base + "_quiet", {{kind.body, f, 0, true}});
    // The phone's case: never answered, unrelated events all the time.
    run_wait(base + "_noisy",
             {{kind.body, f, 0, true}, {Body::kNoise, f_noise, 1, false}});
    // Answered after a known delay by a plain store, under the same noise.
    run_wait(base + "_answered_noisy",
             {{kind.body, f, 0, true},
              {Body::kNoise, f_noise, 1, false},
              {Body::kProducer, f_produce, 2, false}});
    if (kind.body == Body::kSpinZeroWait) {
      // Answered by a store followed by a signal of the very event the
      // waiter polls with its zero timeout.
      run_wait(base + "_answered_with_its_event_noisy",
               {{kind.body, f, 0, true},
                {Body::kNoise, f_noise, 1, false},
                {Body::kProducerSignalWaiter, f_produce_signal_waiter, 2,
                 false}});
    }
    if (main_kind) {
      run_wait(base + "_answered_quiet",
               {{kind.body, f, 0, true},
                {Body::kProducer, f_produce, 2, false}});
      // Answered by a store that is followed by an event.
      run_wait(base + "_answered_with_event_noisy",
               {{kind.body, f, 0, true},
                {Body::kNoise, f_noise, 1, false},
                {Body::kProducerSignal, f_produce_signal, 2, false}});
    }
  }

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
