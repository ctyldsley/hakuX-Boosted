/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/guest_scheduler.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/clock.h"
#include "xenia/base/host_thread_state.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/stack_walker.h"
#include "xenia/kernel/kernel_flags.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xthread.h"

// hakuX-Boosted: a dispatch thread's CPU-time clock, for the skew bound's
// split of a hold (pthread_getcpuclockid, clock_gettime).
#if XE_PLATFORM_LINUX
#include <pthread.h>
#include <time.h>
#endif

DECLARE_path(log_file);

// hakuX-Boosted: defined in cpu/backend/a64/a64_seq_memory.cc, beside the
// site that calls it (park_pause_poll_loops). The a64 backend is the only
// one that has such sites.
#if XE_ARCH_ARM64
namespace xe {
namespace cpu {
namespace backend {
extern void (*spin_poll_site_handler)(void* raw_context, uint64_t site);
}  // namespace backend
}  // namespace cpu
}  // namespace xe
#endif

DEFINE_bool(
    guest_scheduler_stats, false,
    "Log guest scheduler counters once a second: blocked-waiter re-poll rate, "
    "fiber switches, forced preemptions, and how long offloaded blocking calls "
    "queue behind the single I/O worker.",
    "Kernel");

// hakuX-Boosted: the console gives each of its six hardware threads a core of
// its own; a title's threads keep pace with each other because nothing else
// runs there. Here the six dispatch threads and the GPU command thread ran at
// the same nice value as every helper in the process, the pipeline compile
// threads included, so a dispatch thread that woke up queued behind them for
// a core. Measured on the SM8850 (2026-11-12, run 43, 200 s): the dispatch
// thread of guest CPU 1 ran for 6.3 s and waited 15.1 s to be given a core,
// and the Fable II worker on it ran up to 137 ms after its signal, eleven
// times, all while pipelines compiled. That title joins that worker every
// frame while holding a lock the worker needs, and stalls for good when the
// worker is late (boosted/HANDOFF.md). With this on, the dispatch threads and
// the GPU command thread run above normal (nice -8, below the audio threads'
// -12) and the pipeline compile threads below (nice 8), so compiling takes
// what the title leaves.
DEFINE_bool(
    guest_threads_above_helpers, true,
    "Run the guest CPU dispatch threads and the GPU command thread above "
    "normal priority and the pipeline compile threads below it, so a guest "
    "thread that wakes up is given a core at once instead of queueing behind "
    "background work.",
    "Kernel");

// hakuX-Boosted: the scheduler's own cost (boosted/notes/guest-scheduler.md).
// Every one of these defaults to what ran before it existed.
DEFINE_bool(
    guest_scheduler_lean_yield, false,
    "NtYieldExecution that finds no other ready thread on its CPU returns at "
    "once, without the round trip through the dispatch loop (two fiber "
    "switches and four takes of the scheduler lock), as a zero delay already "
    "does. The dispatch loop still runs at the end of every time slice.",
    "Kernel");
DEFINE_bool(
    guest_scheduler_park_yield_spinners, false,
    "A guest thread that calls NtYieldExecution or a zero delay in quick "
    "succession for 2 ms with nothing else to run on its CPU is parked for "
    "about a millisecond at a time until something else runs there or it "
    "stops yielding. Implies guest_scheduler_lean_yield.",
    "Kernel");
// hakuX-Boosted: on device (run 78, Fable II) the spinner's runs were ended
// by its own short waits 300 to 700 times a second, so almost none reached
// 2 ms and it was parked about nine times a second. A setting, so the
// threshold can be tried on the phone without a build.
DEFINE_uint32(
    guest_scheduler_yield_park_after_us, 2000,
    "With guest_scheduler_park_yield_spinners: how long a run of quick "
    "empty yields lasts before the yielder is parked. Lower parks a thread "
    "that spins in short bursts between waits, at the cost of answering "
    "up to a millisecond or two later each time.",
    "Kernel");
DEFINE_bool(
    guest_scheduler_park_poll_loops, false,
    "A guest thread that stays in a recognised spin-wait (the loops the "
    "MemoryPollPark and spin-backoff passes mark) for half a millisecond is "
    "parked for about a millisecond per pass of the loop instead of spinning "
    "its dispatch thread, so what it polls is still seen within about a "
    "millisecond.",
    "Kernel");
DEFINE_bool(
    guest_scheduler_apc_poke, false,
    "Queueing an APC pokes the CPU its target thread is parked on, and "
    "alertable waits and sleeps are no longer re-polled every millisecond to "
    "look for one.",
    "Kernel");
DEFINE_uint32(
    guest_scheduler_gate_recheck_ms, 2,
    "How often a parked waiter's gate is looked at again without a poke, in "
    "milliseconds (a compare, nothing is re-polled unless it moved). 0 leaves "
    "it to the 64 ms backstop. Raise it or switch it off once the missed "
    "wake counters stay at nought: it is what wakes an otherwise idle "
    "dispatch thread every 2 ms.",
    "Kernel");

// hakuX-Boosted: busy waits that sleep (boosted/notes/guest-waits.md). The
// earlier parks slept a whole millisecond at a time, were woken by nothing
// but the clock, and needed 2 ms of unbroken spinning first, which a thread
// that spins in bursts of 1.5 to 3 ms between short waits never gives them
// (run 78: nine parks a second). These sleep from a tenth of a millisecond,
// are ended by whatever can end the wait, and judge each burst on its own.
DEFINE_bool(
    guest_scheduler_park_waits, false,
    "A guest thread that busy-waits (NtYieldExecution or a zero delay in "
    "quick succession with nothing else to run, or a recognised "
    "pause-and-poll loop in translated code, see park_pause_poll_loops) is "
    "parked once the wait has lasted guest_scheduler_park_wait_after_us: "
    "first for a tenth of a millisecond, then twice as long each time up to "
    "guest_scheduler_park_wait_max_us. A park ends early when something "
    "that can end the wait happens: a kernel object is signalled, a "
    "critical section is left, another guest thread stops running, an APC "
    "is queued, the GPU raises an interrupt. Implies "
    "guest_scheduler_lean_yield and replaces "
    "guest_scheduler_park_yield_spinners and guest_scheduler_park_poll_loops.",
    "Kernel");
DEFINE_uint32(
    guest_scheduler_park_wait_after_us, 200,
    "With guest_scheduler_park_waits: how long one burst of busy waiting "
    "lasts before the thread is first parked. A wait that ends sooner is "
    "never slowed.",
    "Kernel");
DEFINE_uint32(
    guest_scheduler_park_wait_max_us, 1000,
    "With guest_scheduler_park_waits: the longest single park, in "
    "microseconds (clamped to 100..4000). It is the most a busy wait can "
    "be answered late when what it waits for is a plain memory write that "
    "nothing reports.",
    "Kernel");

// hakuX-Boosted: the bound on the skew between guest CPUs
// (boosted/notes/guest-skew-bound.md). The console's six hardware threads
// all run at once; here each is a host thread the phone may leave without a
// core for tens of milliseconds while the others run on, which reorders the
// title's threads in a way the console never does. Measured on the SM8850
// (run 73, 2026-11-13, phone hot, four dispatch threads on one core): Fable
// II's worker ran 64 ms after the event that starts it, the main thread got
// a frame ahead of it and the title deadlocked. 0, the default, is what ran
// before any of this existed.
DEFINE_uint32(
    guest_scheduler_skew_bound_us, 0,
    "Bound, in microseconds, on how far a guest thread may run ahead of "
    "another guest CPU whose host thread is not being run. A thread that "
    "readies a thread on another guest CPU (an event set, a semaphore "
    "release, a critical section hand-over, a resume) and then runs on is "
    "held at a safepoint once that CPU has not acted on it for this long, "
    "and every running thread is held while another CPU's running thread "
    "has shown no progress for this long. Judged once per watchdog tick "
    "(guest_scheduler_quantum_us), so the effective bound is this plus up "
    "to one tick. 0 = off. Try 500 and 2000.",
    "Kernel");
DEFINE_uint32(
    guest_scheduler_skew_cap_multiple, 20,
    "The longest one hold of guest_scheduler_skew_bound_us may last, as a "
    "multiple of the bound (20 = 10 ms at a bound of 500). After it the "
    "held thread runs on and does not wait for the same lapse again, so a "
    "host thread that is truly stuck cannot hang the title.",
    "Kernel");
DEFINE_bool(
    guest_scheduler_skew_progress_gate, true,
    "With guest_scheduler_skew_bound_us set: also hold every running guest "
    "thread while another guest CPU's running thread shows no progress (its "
    "host thread descheduled, or busy in host code such as translation). "
    "False leaves only the hold of a thread that readied another.",
    "Kernel");

// hakuX-Boosted: run 90 measured what the progress gate costs before the
// first frame: 27 s against 8 s, 1,221 holds and 35.3 s held while the
// title's main thread was in the translator or a long kernel call. Such a
// thread does not answer its probe, but its host thread is on a core doing
// the guest's work: it is not the case the bound exists for (a dispatch
// thread the phone has taken off its core). The two are told apart by the
// dispatch thread's own CPU-time clock.
DEFINE_bool(
    guest_scheduler_skew_host_progress, true,
    "With guest_scheduler_skew_bound_us set: a guest CPU whose running "
    "thread has not reached a safepoint is not taken for stalled while its "
    "host thread is on a core (at least half of the last watchdog tick, by "
    "the thread's CPU-time clock): it is in the translator or a kernel "
    "call, working for the guest. Only a host thread that is not being run "
    "holds the others. False judges by the safepoint alone, as before.",
    "Kernel");

namespace xe {
namespace kernel {

// hakuX-Boosted: see the header. The clock is the raw counter (one register
// read on ARM64), and nothing is read at all with the counters off.
class GuestScheduler::TimedLock {
 public:
  explicit TimedLock(GuestScheduler* scheduler)
      : scheduler_(scheduler), timed_(cvars::guest_scheduler_stats) {
    if (timed_) {
      uint64_t before = Clock::host_tick_count_raw();
      scheduler_->lock_.lock();
      taken_ = Clock::host_tick_count_raw();
      scheduler_->stats_.lock_wait_ticks.fetch_add(taken_ - before,
                                                   std::memory_order_relaxed);
    } else {
      scheduler_->lock_.lock();
    }
  }
  ~TimedLock() {
    if (timed_) {
      scheduler_->stats_.lock_held_ticks.fetch_add(
          Clock::host_tick_count_raw() - taken_, std::memory_order_relaxed);
      scheduler_->stats_.lock_acquisitions.fetch_add(
          1, std::memory_order_relaxed);
    }
    scheduler_->lock_.unlock();
  }
  TimedLock(const TimedLock&) = delete;
  TimedLock& operator=(const TimedLock&) = delete;

 private:
  GuestScheduler* scheduler_;
  bool timed_;
  uint64_t taken_ = 0;
};

// hakuX-Boosted: how long a run of empty yields lasts before the yielder is
// parked, how close together they must come to count as spinning (a thread
// that works between its yields is never parked), and how long a thread
// stays in a spin-wait before it is.
static constexpr uint64_t kYieldParkAfterUs = 2000;
static constexpr uint64_t kYieldSpinGapUs = 20;
static constexpr uint64_t kPollParkAfterUs = 500;

// hakuX-Boosted: guest_scheduler_park_waits. The first park of a burst, the
// shortest time a park lasts even when an event asks for the thread sooner
// (which bounds how often a busy neighbour can wake a spinner), how long an
// armed thread has to reach its park before it must arm again, and the
// longest eight passes of a pause-and-poll loop may take to count as one
// unbroken wait.
static constexpr uint64_t kWaitParkFirstUs = 100;
static constexpr uint64_t kWaitParkMinUs = 50;
static constexpr uint64_t kWaitArmWindowUs = 2000;
static constexpr uint64_t kPollGapUs = 400;

// hakuX-Boosted: guest_scheduler_skew_bound_us. How long a held fiber sleeps
// before it looks again without having been woken (a preemption of the held
// thread is only seen this way), the hold length from which each hold is
// logged, and the watchdog's priority while the bound is on (nice -12 on
// Android, above the dispatch threads' -8: the watchdog raises the probes
// and must not queue behind the threads it is timing).
static constexpr uint64_t kSkewHoldSliceUs = 200;
static constexpr uint64_t kSkewHoldLogUs = 4000;
static constexpr int32_t kSkewWatchdogPriority = 28;
// guest_scheduler_skew_host_progress: the share of a tick on a core, in
// thousandths, from which a host thread counts as working. Four dispatch
// threads packed on one core get a quarter each (run 73) and stay below it.
static constexpr uint32_t kSkewOnCorePermille = 500;

// Defined with the yield spinner's park below; a hold has the same rule.
static bool CanParkSpinner(XThread* self);

// Logical CPU index of the host thread currently executing, or -1 on any
// non-dispatch thread. Set by each CPU's RunLoop.
static thread_local int t_current_cpu = -1;

// hakuX-Boosted: every read goes through this, never inlined. A fiber that
// yields can resume on another CPU's host thread, and code that reads the
// index on both sides of the yield (YieldCurrentThread compares them) must
// read the thread it is on now, not the TLS block the compiler found on entry
// (kernel/xthread.cc, XThread::GetCurrentThread).
// hakuX-Boosted: with host_thread_state_slot on, the index is read from the
// host thread's state (base/host_thread_state.h: one mrs and two loads)
// instead of through the dynamic TLS resolver, which this function was 2% of
// the title's main thread for, on top of the resolver's own 8% (run 90's
// profile). A host thread that never registered reads -1, as its thread_local
// would say. The state's address is read by a volatile asm on each call, so
// the rule above holds for it too.
XE_NOINLINE static int CurrentCpu() {
  if (const auto* state = xe::threading::HostThreadStateForRead()) {
    const int cpu = state->scheduler_cpu;
    if (xe::threading::HostThreadStateVerifies(state)) {
      xe::threading::HostThreadStateCheck(
          "the scheduler's CPU index", uint64_t(int64_t(cpu)),
          uint64_t(int64_t(t_current_cpu)));
    }
    return cpu;
  }
  return t_current_cpu;
}

// Written once, by a dispatch thread as it starts. The thread_local is kept
// as well: it is what the readers use with the setting off, and what the
// verify mode compares with.
static void SetCurrentCpu(int index) {
  if (auto* state = xe::threading::HostThreadStateForWrite()) {
    state->scheduler_cpu = index;
  }
  t_current_cpu = index;
}

// hakuX-Boosted: the fiber-backed guest thread that a JIT hook or a kernel
// export is running on, given the PPC context it was handed. With
// host_thread_state_slot on this is two loads from the context and no
// thread-local lookup; otherwise (or with no context) it is
// XThread::GetCurrentFiberThread(), as before. Guest code and kernel exports
// run on their own thread's context, so the two agree; the verify mode
// compares them on every call.
static XThread* FiberThreadOf(void* raw_context) {
  if (raw_context && xe::threading::HostThreadStateInUse()) {
    XThread* thread = XThread::FromContext(
        static_cast<const cpu::ppc::PPCContext*>(raw_context));
    if (thread && !thread->fiber()) {
      thread = nullptr;
    }
    if (const auto* state = xe::threading::HostThreadStateForRead()) {
      if (xe::threading::HostThreadStateVerifies(state)) {
        xe::threading::HostThreadStateCheck(
            "the fiber thread found from its context",
            reinterpret_cast<uintptr_t>(thread),
            reinterpret_cast<uintptr_t>(XThread::GetCurrentFiberThread()));
      }
    }
    return thread;
  }
  return XThread::GetCurrentFiberThread();
}

// Off a dispatch thread there is no CPU to index, so the caller must bail.
static bool OnDispatchThread(const char* what) {
  if (CurrentCpu() >= 0) {
    return true;
  }
  XELOGW("GuestScheduler: {} called off a dispatch thread, ignoring", what);
  return false;
}

// Clamps a thread's priority to the ready-queue index range [0, 31].
static int ClampPriority(int32_t priority) {
  return priority < 0 ? 0 : (priority > 31 ? 31 : priority);
}

// Safepoints that may decline to preempt before one is forced through anyway.
// A guest spinning at DISPATCH_LEVEL passes safepoints at roughly the loop
// rate, so this is a short wait in wall-clock terms, and the alternative is an
// unbounded livelock when the holder it spins on is co-resident.
static constexpr uint32_t kMaxIrqlPreemptDefers = 4096;
// Reporting threshold for the lock case, which is never forced.
static constexpr uint32_t kLockPreemptDeferReport = 65536;

// JIT safepoint handler. The cold path cleared the flag, so the deferred
// cases re-set it to retry at the next safepoint.
static void PreemptCurrentFiber(void* raw_context) {
  // hakuX-Boosted: from the context the safepoint passes (FiberThreadOf).
  XThread* self = FiberThreadOf(raw_context);
  if (!self) {
    return;
  }
  auto* context = self->thread_state()->context();
  auto& links = self->scheduler_links();
  // hakuX-Boosted: guest_scheduler_skew_bound_us. First, because it must see
  // every safepoint the watchdog's probe reaches, whatever the thread is
  // doing: it answers the probe and, where a hold is allowed, waits for a
  // guest CPU that has fallen behind. It never yields the fiber.
  self->kernel_state()->guest_scheduler()->SkewAtSafepoint(self);
  // hakuX-Boosted: guest_scheduler_park_poll_loops, see PollWatchAtBackoff.
  if (self->kernel_state()->guest_scheduler()->PollWatchAtSafepoint(self)) {
    return;
  }
  // hakuX-Boosted: the flag was up for the probe alone. Before the deferral
  // counters below, which would otherwise count every probe at raised IRQL
  // or under the global lock as a declined preemption.
  if (self->kernel_state()->guest_scheduler()->SkewOnlyProbed(self)) {
    return;
  }
  // A co-resident fiber would re-enter the recursive lock on this host thread,
  // silently breaking mutual exclusion, so this one is never forced. Report a
  // fiber stuck here instead - it means guest code is spinning under the global
  // lock, which the lock's own holder has to resolve.
  if (xe::global_critical_region::is_held_by_current_thread()) {
    context->preempt_requested = 1;
    if (++links.preempt_defers_lock == kLockPreemptDeferReport) {
      XELOGW(
          "GuestScheduler: fiber tid={:08X} '{}' has declined {} preemptions "
          "holding the global critical region; co-resident fibers cannot run",
          self->thread_id(), self->thread_name(), kLockPreemptDeferReport);
    }
    return;
  }
  links.preempt_defers_lock = 0;
  // At DISPATCH_LEVEL the console masks the decrementer, but it also runs the
  // lock holder on another core. Here the holder may be a fiber queued behind
  // this one, so honoring the mask indefinitely livelocks. Defer a bounded
  // number of times, then switch anyway - IRQL still orders guest APCs.
  auto* kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  bool forced_at_irql = false;
  if (kpcr->current_irql >= 2) {
    if (++links.preempt_defers_irql < kMaxIrqlPreemptDefers) {
      context->preempt_requested = 1;
      return;
    }
    forced_at_irql = true;
    self->kernel_state()->guest_scheduler()->NoteForcedPreempt();
    if (!links.forced_preempt_logged) {
      links.forced_preempt_logged = true;
      XELOGW(
          "GuestScheduler: forcing preemption of tid={:08X} '{}' at IRQL {} "
          "after {} declined safepoints (first time for this thread)",
          self->thread_id(), self->thread_name(), uint32_t(kpcr->current_irql),
          links.preempt_defers_irql);
    }
  }
  links.preempt_defers_irql = 0;
  // Involuntary quantum end, so no yield to a lower-priority thread - except
  // on the forced path, where the whole point is to reach a holder the strict
  // priority order would keep queued behind us.
  auto* scheduler = self->kernel_state()->guest_scheduler();
  scheduler->YieldCurrentThread(true, forced_at_irql,
                                GuestScheduler::YieldCause::kQuantumEnd);
  if (!forced_at_irql) {
    scheduler->PollWatchArm(self);
  }
}

// Spin-backoff hook. A collapsed guest spin-wait calls this instead of
// burning its dispatch CPU: the producer it waits on may be a fiber queued
// behind the caller on this same thread, which only a yield can let run. A
// plain voluntary yield, not a quantum end, so a preempted-at-head fiber
// keeps its slice semantics.
static void SpinBackoffYieldFiber(void* raw_context) {
  // hakuX-Boosted: the backoff site passes its context (a64_seq_memory.cc).
  XThread* self = FiberThreadOf(raw_context);
  if (!self) {
    // Guest code on a non-fiber thread (interpreter, early init).
    for (uint32_t n = 0; n < 8; ++n) {
#if XE_ARCH_ARM64
      __asm__ __volatile__("isb sy" ::: "memory");
#endif
    }
    return;
  }
  auto* scheduler = self->kernel_state()->guest_scheduler();
  // hakuX-Boosted: guest_scheduler_skew_bound_us (the poll-loop watch below
  // can return without reaching YieldCurrentThread, which answers it too).
  scheduler->SkewAtBackoff(self);
  // hakuX-Boosted: guest_scheduler_park_poll_loops.
  if (scheduler->PollWatchAtBackoff(self)) {
    return;
  }
  scheduler->YieldCurrentThread(false, true,
                                GuestScheduler::YieldCause::kSpinSite);
}

// hakuX-Boosted: a pause-and-poll loop's site in translated code
// (park_pause_poll_loops), called on every pass of the loop.
XE_MAYBE_UNUSED static void PollSiteVisitFiber(void* raw_context,
                                               uint64_t site) {
  XThread* self = FiberThreadOf(raw_context);
  if (!self) {
    return;  // guest code on a thread the scheduler does not run
  }
  self->kernel_state()->guest_scheduler()->PollSiteVisit(self, raw_context,
                                                         uint32_t(site));
}

// Raw host ticks per us for the watchdog's deadline math, 0 if unusable.
static double CalibrateTicksPerUs() {
  // Median of three short samples. A single busy-spin sample can be stretched
  // by a migration between big and little cores or a DVFS transition mid-loop,
  // and the accept range below is far too wide to catch a merely skewed one.
  double samples[3] = {};
  for (double& sample : samples) {
    uint64_t qpc_freq = Clock::host_tick_frequency_platform();
    uint64_t qpc0 = Clock::host_tick_count_platform();
    uint64_t tsc0 = Clock::host_tick_count_raw();
    uint64_t qpc_end = qpc0 + qpc_freq / 2000;  // ~0.5 ms
    while (Clock::host_tick_count_platform() < qpc_end) {
    }
    uint64_t qpc1 = Clock::host_tick_count_platform();
    uint64_t tsc1 = Clock::host_tick_count_raw();
    double secs = qpc1 > qpc0 ? double(qpc1 - qpc0) / double(qpc_freq) : 0.0;
    sample = secs > 0.0 ? double(tsc1 - tsc0) / (secs * 1e6) : 0.0;
  }
  if (samples[0] > samples[1]) {
    std::swap(samples[0], samples[1]);
  }
  if (samples[1] > samples[2]) {
    std::swap(samples[1], samples[2]);
  }
  if (samples[0] > samples[1]) {
    std::swap(samples[0], samples[1]);
  }
  double per_us = samples[1];
  // Spans an x86 TSC at 1-6 GHz and an ARM64 generic timer at 1-100 MHz.
  if (per_us < 0.5 || per_us > 100000.0) {
    return 0.0;
  }
  return per_us;
}

XThread* GuestScheduler::FiberThreadOfContext(void* raw_context) {
  return FiberThreadOf(raw_context);
}

GuestScheduler::GuestScheduler(KernelState* kernel_state)
    : kernel_state_(kernel_state) {}

GuestScheduler::~GuestScheduler() { Shutdown(); }

bool GuestScheduler::enabled() { return cvars::guest_scheduler; }

int GuestScheduler::DispatchCpuOf(uint8_t guest_cpu) const {
  // Wrap rather than fold to 0: an out-of-range guest CPU is already unusual,
  // and folding every one of them onto CPU 0 stacks them on the dispatch
  // thread the main thread already uses.
  return guest_cpu % kMaxCpus;
}

int GuestScheduler::CpuOf(XThread* thread) const {
  return DispatchCpuOf(thread->guest_object<X_KTHREAD>()->current_cpu);
}

void GuestScheduler::EnsureStarted() {
  bool expected = false;
  if (!started_.compare_exchange_strong(expected, true)) {
    return;
  }
  xe::cpu::backend::preempt_yield_handler = &PreemptCurrentFiber;
  xe::cpu::backend::spin_backoff_yield_handler = &SpinBackoffYieldFiber;
#if XE_ARCH_ARM64
  xe::cpu::backend::spin_poll_site_handler = &PollSiteVisitFiber;  // hakuX-Boosted
#endif
  // Not in the ctor, which runs before per-title cvar overrides are applied.
  double ticks_per_us = CalibrateTicksPerUs();
  ticks_per_us_ = ticks_per_us;
  // hakuX-Boosted: 0 (the counter did not calibrate) switches the parks off.
  yield_park_after_ticks_ = uint64_t(
      ticks_per_us * (cvars::guest_scheduler_yield_park_after_us
                          ? cvars::guest_scheduler_yield_park_after_us
                          : kYieldParkAfterUs));
  yield_gap_ticks_ = uint64_t(ticks_per_us * kYieldSpinGapUs);
  poll_park_after_ticks_ = uint64_t(ticks_per_us * kPollParkAfterUs);
  // hakuX-Boosted: guest_scheduler_park_waits, read once, before any
  // dispatch thread exists. It needs the counter (0 = did not calibrate).
  park_waits_ = cvars::guest_scheduler_park_waits && ticks_per_us > 0.0;
  lean_yield_ = park_waits_ || cvars::guest_scheduler_lean_yield ||
                cvars::guest_scheduler_park_yield_spinners;
  if (park_waits_) {
    const uint64_t max_us = std::min<uint64_t>(
        4000, std::max<uint64_t>(kWaitParkFirstUs,
                                 cvars::guest_scheduler_park_wait_max_us));
    wait_park_after_ticks_ = std::max<uint64_t>(
        1, uint64_t(ticks_per_us * cvars::guest_scheduler_park_wait_after_us));
    wait_park_first_ticks_ =
        std::max<uint64_t>(1, uint64_t(ticks_per_us * kWaitParkFirstUs));
    wait_park_min_ticks_ =
        std::max<uint64_t>(1, uint64_t(ticks_per_us * kWaitParkMinUs));
    wait_park_max_ticks_ = std::max<uint64_t>(
        wait_park_first_ticks_, uint64_t(ticks_per_us * max_us));
    wait_arm_window_ticks_ =
        std::max<uint64_t>(1, uint64_t(ticks_per_us * kWaitArmWindowUs));
    poll_gap_ticks_ =
        std::max<uint64_t>(1, uint64_t(ticks_per_us * kPollGapUs));
    XELOGI(
        "GuestScheduler: busy waits are parked after {} us, for {} us at "
        "first and {} us at most, and woken by events",
        uint32_t(cvars::guest_scheduler_park_wait_after_us), kWaitParkFirstUs,
        max_us);
  }
  if (cvars::guest_scheduler_lean_yield ||
      cvars::guest_scheduler_park_yield_spinners ||
      cvars::guest_scheduler_park_poll_loops ||
      cvars::guest_scheduler_apc_poke ||
      cvars::guest_scheduler_gate_recheck_ms != kGateRecheckMs) {
    XELOGI(
        "GuestScheduler: lean_yield={} park_yield_spinners={} "
        "park_poll_loops={} apc_poke={} gate_recheck_ms={}",
        cvars::guest_scheduler_lean_yield,
        cvars::guest_scheduler_park_yield_spinners,
        cvars::guest_scheduler_park_poll_loops, cvars::guest_scheduler_apc_poke,
        uint32_t(cvars::guest_scheduler_gate_recheck_ms));
  }
  quantum_ticks_ =
      static_cast<uint64_t>(ticks_per_us * cvars::guest_scheduler_quantum_us);
  if (quantum_ticks_) {
    XELOGI("GuestScheduler: preemption slice = {} us ({} ticks)",
           uint32_t(cvars::guest_scheduler_quantum_us), quantum_ticks_);
  } else {
    // Priority and wake preemption still work, they raise the flag directly.
    XELOGW(
        "GuestScheduler: no timeslice preemption ({}), a fiber that never "
        "yields or waits can hog its CPU",
        ticks_per_us > 0.0 ? "guest_scheduler_quantum_us is 0"
                           : "host tick counter did not calibrate");
  }

  // hakuX-Boosted: guest_scheduler_skew_bound_us. Set before any dispatch
  // thread exists. It needs the watchdog (the probes are raised on its
  // tick), so without a timeslice it stays off.
  if (cvars::guest_scheduler_skew_bound_us) {
    if (!quantum_ticks_) {
      XELOGW(
          "GuestScheduler: guest_scheduler_skew_bound_us is set but there is "
          "no watchdog tick (see the line above), so the skew bound is off");
    } else {
      skew_bound_ticks_ = std::max<uint64_t>(
          1, uint64_t(ticks_per_us * cvars::guest_scheduler_skew_bound_us));
      const uint64_t multiple = std::max<uint64_t>(
          1, uint64_t(cvars::guest_scheduler_skew_cap_multiple));
      skew_cap_ticks_ = skew_bound_ticks_ * multiple;
      skew_progress_gate_ = cvars::guest_scheduler_skew_progress_gate;
      // hakuX-Boosted: guest_scheduler_skew_host_progress.
      skew_host_progress_ = cvars::guest_scheduler_skew_host_progress;
      const uint64_t tick_us =
          std::max(uint64_t(cvars::guest_scheduler_quantum_us),
                   uint64_t(kMinWatchdogPeriodUs));
      skew_sample_fresh_ticks_ = uint64_t(ticks_per_us * double(tick_us) * 3);
      XELOGI(
          "GuestScheduler: skew bound {} us, one hold at most {} us, "
          "progress gate {}, judged every {} us (the watchdog tick), a host "
          "thread busy in host code {}",
          uint32_t(cvars::guest_scheduler_skew_bound_us),
          uint64_t(cvars::guest_scheduler_skew_bound_us) * multiple,
          skew_progress_gate_ ? "on" : "off", tick_us,
          skew_host_progress_ ? "counts as progress" : "counts as stalled");
    }
  }

  for (int i = 0; i < kMaxCpus; ++i) {
    cpus_[i].ready_event = xe::threading::Event::CreateAutoResetEvent(false);
  }
  if (quantum_ticks_) {
    watchdog_event_ = xe::threading::Event::CreateAutoResetEvent(false);
    xe::threading::Thread::CreationParameters params;
    watchdog_thread_ =
        xe::threading::Thread::Create(params, [this]() { WatchdogLoop(); });
    watchdog_thread_->set_name("Guest Scheduler Watchdog");
    // hakuX-Boosted: only with the skew bound on.
    if (skew_bound_ticks_) {
      watchdog_thread_->set_priority(kSkewWatchdogPriority);
    }
  }
  for (int i = 0; i < kMaxCpus; ++i) {
    xe::threading::Thread::CreationParameters params;
    cpus_[i].host_thread =
        xe::threading::Thread::Create(params, [this, i]() { RunLoop(i); });
    cpus_[i].host_thread->set_name(std::string("Guest CPU ") +
                                   std::to_string(i));
    if (cvars::guest_threads_above_helpers) {
      cpus_[i].host_thread->set_priority(
          xe::threading::ThreadPriority::kAboveNormal);
    }
  }
}

void GuestScheduler::Shutdown() {
  if (!started_.load() && !io_started_.load()) {
    return;
  }
  if (stopped_.load()) {
    return;
  }
  shutting_down_.store(true);
  for (Cpu& cpu : cpus_) {
    if (cpu.ready_event) {
      cpu.ready_event->Set();
      // hakuX-Boosted: and the sub-millisecond sleep, if it is in that.
      WakeDispatchThread(cpu);
    }
  }
  if (io_event_) {
    io_event_->Set();
  }
  if (watchdog_event_) {
    watchdog_event_->Set();
  }
  for (Cpu& cpu : cpus_) {
    if (!cpu.host_thread) {
      continue;
    }
    // Join before reset(), which only closes the handle. A spinning fiber
    // only leaves via the preempt flag, so keep raising it until the loop
    // drains.
    int waited_ms = 0;
    while (xe::threading::Wait(cpu.host_thread.get(), false,
                               std::chrono::milliseconds(50)) ==
           xe::threading::WaitResult::kTimeout) {
      {
        TimedLock lock(this);
        for (int i = 0; i < kMaxCpus; ++i) {
          if (XThread* running = cpus_[i].current_thread) {
            running->thread_state()->context()->preempt_requested = 1;
          }
        }
      }
      for (int i = 0; i < kMaxCpus; ++i) {
        if (cpus_[i].ready_event) {
          cpus_[i].ready_event->Set();
        }
      }
      waited_ms += 50;
      if (waited_ms % 2000 == 0) {
        XELOGW(
            "GuestScheduler: shutdown has waited {} ms for a dispatch thread, "
            "its fiber is not reaching a safepoint",
            waited_ms);
      }
    }
    cpu.host_thread.reset();
  }
  if (watchdog_thread_) {
    xe::threading::Wait(watchdog_thread_.get(), false);
    watchdog_thread_.reset();
  }
  // After the dispatch threads, so no fiber is still watching a BlockingCall.
  if (io_thread_) {
    xe::threading::Wait(io_thread_.get(), false);
    io_thread_.reset();
  }
  // Everything still linked is unreachable now that the dispatch threads are
  // gone. Reclaim each thread so a relaunch does not leak it and its stack.
  std::vector<XThread*> leftovers;
  {
    TimedLock lock(this);
    auto drain = [&leftovers](XThread*& head, XThread*& tail) {
      for (XThread* t = head; t;) {
        auto& links = t->scheduler_links();
        XThread* next = links.ready_next;
        links.queued = false;
        links.blocked = false;
        links.suspended = false;
        links.ready_next = nullptr;
        leftovers.push_back(t);
        t = next;
      }
      head = nullptr;
      tail = nullptr;
    };
    for (Cpu& cpu : cpus_) {
      for (int prio = 0; prio < 32; ++prio) {
        drain(cpu.ready_head[prio], cpu.ready_tail[prio]);
      }
      cpu.ready_summary.store(0, std::memory_order_relaxed);
      drain(cpu.blocked_head, cpu.blocked_tail);
      drain(cpu.suspended_head, cpu.suspended_tail);
      if (cpu.exited_thread) {
        leftovers.push_back(cpu.exited_thread);
        cpu.exited_thread = nullptr;
      }
      cpu.yield_to_other = nullptr;
      cpu.current_thread = nullptr;
      cpu.has_blocked.store(false, std::memory_order_relaxed);
    }
  }
  if (!leftovers.empty()) {
    XELOGI("GuestScheduler: reclaiming {} parked fibers on shutdown",
           leftovers.size());
  }
  for (XThread* t : leftovers) {
    // A parked waiter's registration would otherwise dangle on the object.
    XObject::AbandonCooperativeWait(t);
    t->ReclaimExited();
  }
  stopped_.store(true);
}

// hakuX-Boosted: with guest_scheduler_park_waits a dispatch thread whose
// next deadline is a parked spinner's sleeps on its condition variable (the
// ready event's wait only takes whole milliseconds). fine_parked is stored
// before parked, so a waker that saw parked sees which sleep it is; a waker
// that did not see parked has made its work visible under lock_ first, and
// the dispatch thread looks at its queues under lock_ after it stores
// parked (the rule the ready event already relies on).
void GuestScheduler::WakeDispatchThread(Cpu& cpu) {
  if (cpu.fine_parked.load()) {
    {
      std::lock_guard<std::mutex> lock(cpu.fine_mutex);
      cpu.fine_signal = true;
    }
    cpu.fine_cv.notify_one();
    return;
  }
  if (cpu.ready_event) {
    cpu.ready_event->Set();
  }
}

void GuestScheduler::FineSleep(Cpu& cpu, uint64_t micros) {
  std::unique_lock<std::mutex> lock(cpu.fine_mutex);
  cpu.fine_cv.wait_for(lock, std::chrono::microseconds(micros),
                       [&cpu] { return cpu.fine_signal; });
}

void GuestScheduler::EnqueueReady(XThread* thread, int cpu_index,
                                  bool yield_to_other) {
  // hakuX-Boosted: guest_scheduler_skew_bound_us. A guest thread that makes
  // another runnable on another guest CPU (a resume, a new thread) is in
  // that CPU's debt until it acts on it, as for a signal (WakeForSignal).
  XThread* skew_debtor = nullptr;
  int skew_debtor_cpu = -1;
  uint64_t skew_now = 0;
  if (skew_bound_ticks_) {
    skew_debtor_cpu = CurrentCpu();
    if (skew_debtor_cpu >= 0 && skew_debtor_cpu != cpu_index) {
      skew_debtor = XThread::GetCurrentFiberThread();
      if (skew_debtor == thread) {
        skew_debtor = nullptr;
      }
      if (skew_debtor) {
        skew_now = Clock::host_tick_count_raw();
      }
    }
  }
  {
    TimedLock lock(this);
    auto& links = thread->scheduler_links();
    // The single gate for every "make it runnable" request, so a state that
    // already owns its wake-up is a silent no-op. Blocked and suspended move
    // via RereadyBlocked and ResumeThread, all three lists sharing ready_next.
    if (links.blocked || links.suspended) {
      return;
    }
    // A running fiber's context is not saved until it yields, so only the
    // dispatch thread that owns it, links.cpu, may re-queue it.
    if (links.running && links.cpu != CurrentCpu()) {
      return;
    }
    if (links.queued) {
      return;
    }
    links.queued = true;
    links.cpu = cpu_index;
    bool at_head = links.preempted;
    links.preempted = false;
    LinkReadyLocked(cpus_[cpu_index], thread, at_head);
    if (yield_to_other) {
      cpus_[cpu_index].yield_to_other = thread;
    }
    if (skew_debtor) {
      SkewAddDebtLocked(skew_debtor, skew_debtor_cpu, cpu_index,
                        thread->thread_id(), skew_now);
    }
  }
  // Only a parked dispatch thread needs the syscall.
  if (cpus_[cpu_index].parked.load() && cpus_[cpu_index].ready_event) {
    WakeDispatchThread(cpus_[cpu_index]);
  }
}

void GuestScheduler::MarkReady(XThread* thread) {
  assert_not_null(thread);
  // Don't re-enqueue a terminated thread, or a stray Resume could revive a
  // zombie.
  if (thread->guest_object<X_KTHREAD>()->thread_state ==
      KTHREAD_STATE_TERMINATED) {
    return;
  }
  EnqueueReady(thread, CpuOf(thread));
}

void GuestScheduler::ResumeThread(XThread* thread) {
  assert_not_null(thread);
  {
    TimedLock lock(this);
    auto& links = thread->scheduler_links();
    if (links.suspended) {
      Cpu& cpu = cpus_[links.cpu];
      UnlinkLocked(cpu.suspended_head, cpu.suspended_tail, thread);
      links.suspended = false;
      links.ready_next = nullptr;
    }
  }
  // Only enqueues if it was never queued, e.g. created suspended.
  MarkReady(thread);
}

bool GuestScheduler::ParkSuspended(XThread* thread, int cpu_index) {
  TimedLock lock(this);
  auto& links = thread->scheduler_links();
  // Re-read under the lock, a Resume racing the dispatcher's check would have
  // found us not yet parked and parking anyway would strand the thread.
  // Termination overrides suspension, run it so it can exit.
  if (thread->suspend_count() == 0 ||
      links.terminate_pending.load(std::memory_order_relaxed)) {
    return false;
  }
  // Clearing running last, so it is never both unowned and unlisted.
  links.suspended = true;
  links.cpu = cpu_index;
  links.ready_next = nullptr;
  links.quantum_deadline_tick = 0;
  Cpu& cpu = cpus_[cpu_index];
  LinkTailLocked(cpu.suspended_head, cpu.suspended_tail, thread);
  links.running = false;
  return true;
}

XThread* GuestScheduler::HighestReadyExcept(const Cpu& cpu, XThread* except) {
  uint32_t summary = cpu.ready_summary.load(std::memory_order_relaxed);
  while (summary) {
    int level = 31 - xe::lzcnt(summary);
    summary &= ~(uint32_t(1) << level);
    XThread* head = cpu.ready_head[level];
    if (head != except) {
      return head;
    }
    // Its successor outranks anything on a lower level.
    if (except->scheduler_links().ready_next) {
      return except->scheduler_links().ready_next;
    }
  }
  return nullptr;
}

XThread* GuestScheduler::DequeueReady(int cpu_index) {
  TimedLock lock(this);
  Cpu& cpu = cpus_[cpu_index];
  if (cpu.ready_summary.load(std::memory_order_relaxed) == 0) {
    return nullptr;
  }
  // Strict priority alone lets a high-priority yield-spinner deadlock on the
  // lower-priority co-resident it depends on, so a voluntary yield opts out.
  XThread* yielder = cpu.yield_to_other;
  cpu.yield_to_other = nullptr;

  // Highest set bit = highest ready priority.
  int level =
      31 - xe::lzcnt(cpu.ready_summary.load(std::memory_order_relaxed));
  XThread* thread = cpu.ready_head[level];
  if (yielder && thread == yielder) {
    if (XThread* other = HighestReadyExcept(cpu, yielder)) {
      // |other| may sit mid-list, so unlink it generally rather than as a head.
      int other_level = other->scheduler_links().queued_prio;
      UnlinkLocked(cpu.ready_head[other_level], cpu.ready_tail[other_level],
                   other);
      if (!cpu.ready_head[other_level]) {
        ClearReadyLevel(cpu, other_level);
      }
      auto& other_links = other->scheduler_links();
      other_links.ready_next = nullptr;
      other_links.queued = false;
      other_links.running = true;
      return other;
    }
  }

  auto& links = thread->scheduler_links();
  cpu.ready_head[level] = links.ready_next;
  if (!cpu.ready_head[level]) {
    cpu.ready_tail[level] = nullptr;
    ClearReadyLevel(cpu, level);
  }
  links.ready_next = nullptr;
  links.queued = false;
  // Owned from here, not from SwitchTo, because in between it is in no list and
  // a concurrent MarkReady would queue it onto another CPU.
  links.running = true;
  return thread;
}

void GuestScheduler::LinkTailLocked(XThread*& head, XThread*& tail,
                                    XThread* thread) {
  if (tail) {
    tail->scheduler_links().ready_next = thread;
  } else {
    head = thread;
  }
  tail = thread;
}

void GuestScheduler::LinkHeadLocked(XThread*& head, XThread*& tail,
                                    XThread* thread) {
  thread->scheduler_links().ready_next = head;
  head = thread;
  if (!tail) {
    tail = thread;
  }
}

void GuestScheduler::LinkReadyLocked(Cpu& cpu, XThread* thread, bool at_head) {
  auto& links = thread->scheduler_links();
  int prio = ClampPriority(thread->priority());
  links.queued_prio = prio;
  links.ready_next = nullptr;
  if (at_head) {
    LinkHeadLocked(cpu.ready_head[prio], cpu.ready_tail[prio], thread);
  } else {
    LinkTailLocked(cpu.ready_head[prio], cpu.ready_tail[prio], thread);
  }
  cpu.ready_summary.store(
      cpu.ready_summary.load(std::memory_order_relaxed) | (uint32_t(1) << prio),
      std::memory_order_relaxed);
  // Outranking the running fiber flags it, so its next JIT safepoint yields
  // and the dispatcher picks us.
  XThread* running = cpu.current_thread;
  if (running && running != thread &&
      prio > ClampPriority(running->priority())) {
    running->scheduler_links().preempted = true;
    running->thread_state()->context()->preempt_requested = 1;
  }
}

void GuestScheduler::UnlinkLocked(XThread*& head, XThread*& tail,
                                  XThread* thread) {
  XThread** link = &head;
  XThread* prev = nullptr;
  while (*link) {
    if (*link == thread) {
      *link = thread->scheduler_links().ready_next;
      if (tail == thread) {
        tail = prev;
      }
      return;
    }
    prev = *link;
    link = &(*link)->scheduler_links().ready_next;
  }
}

void GuestScheduler::RequeueForPriority(XThread* thread) {
  TimedLock lock(this);
  auto& links = thread->scheduler_links();
  if (!links.queued || links.cpu < 0) {
    return;
  }
  Cpu& cpu = cpus_[links.cpu];
  int old = links.queued_prio;
  UnlinkLocked(cpu.ready_head[old], cpu.ready_tail[old], thread);
  if (!cpu.ready_head[old]) {
    ClearReadyLevel(cpu, old);
  }
  LinkReadyLocked(cpu, thread, false);
}

bool GuestScheduler::ForgetThread(XThread* thread) {
  TimedLock lock(this);
  auto& links = thread->scheduler_links();
  // A thread that ever ran has a live fiber stack and one a dispatch thread
  // owns is about to be switched to, so neither may be freed.
  const bool reclaimable = !links.has_run && !links.running;
  if (links.cpu >= 0) {
    Cpu& cpu = cpus_[links.cpu];
    if (links.queued) {
      int prio = links.queued_prio;
      UnlinkLocked(cpu.ready_head[prio], cpu.ready_tail[prio], thread);
      if (!cpu.ready_head[prio]) {
        ClearReadyLevel(cpu, prio);
      }
    } else if (links.blocked) {
      UnlinkLocked(cpu.blocked_head, cpu.blocked_tail, thread);
      DropWaitParkLocked(thread);  // hakuX-Boosted
    } else if (links.suspended) {
      UnlinkLocked(cpu.suspended_head, cpu.suspended_tail, thread);
    }
  }
  links.queued = false;
  links.blocked = false;
  links.suspended = false;
  links.ready_next = nullptr;
  // Drop every raw pointer a CPU may still hold to it. A fiber detaching itself
  // keeps current_thread, which SwitchTo clears on the way out.
  for (Cpu& cpu : cpus_) {
    if (cpu.yield_to_other == thread) {
      cpu.yield_to_other = nullptr;
    }
    if (cpu.exited_thread == thread) {
      cpu.exited_thread = nullptr;
    }
    if (cpu.current_thread == thread && !links.running) {
      cpu.current_thread = nullptr;
    }
  }
  return reclaimable;
}

bool GuestScheduler::TerminateThread(XThread* thread) {
  int wake_cpu = -1;
  {
    TimedLock lock(this);
    auto& links = thread->scheduler_links();
    links.terminate_pending.store(true, std::memory_order_relaxed);
    if (stopped_.load() || !started_.load()) {
      // No dispatcher will ever run it again, detach it and let the caller
      // free the stack, parked frames and all.
      if (links.cpu >= 0) {
        Cpu& cpu = cpus_[links.cpu];
        if (links.queued) {
          int prio = links.queued_prio;
          UnlinkLocked(cpu.ready_head[prio], cpu.ready_tail[prio], thread);
          if (!cpu.ready_head[prio]) {
            ClearReadyLevel(cpu, prio);
          }
        } else if (links.blocked) {
          UnlinkLocked(cpu.blocked_head, cpu.blocked_tail, thread);
          DropWaitParkLocked(thread);  // hakuX-Boosted
        } else if (links.suspended) {
          UnlinkLocked(cpu.suspended_head, cpu.suspended_tail, thread);
        }
      }
      links.queued = false;
      links.blocked = false;
      links.suspended = false;
      links.ready_next = nullptr;
      assert_false(links.running);
      return !links.running;
    }
    if (links.running) {
      // Force it to a safepoint, where ExitIfTerminated ends it.
      thread->thread_state()->context()->preempt_requested = 1;
      return false;
    }
    if (links.blocked || links.suspended) {
      // Termination overrides a wait or suspend. Dispatch it so it exits on
      // its own stack and the idle loop reclaims it.
      Cpu& cpu = cpus_[links.cpu];
      if (links.blocked) {
        UnlinkLocked(cpu.blocked_head, cpu.blocked_tail, thread);
        DropWaitParkLocked(thread);  // hakuX-Boosted
      } else {
        UnlinkLocked(cpu.suspended_head, cpu.suspended_tail, thread);
      }
      links.blocked = false;
      links.suspended = false;
      links.ready_next = nullptr;
      links.queued = true;
      LinkReadyLocked(cpus_[links.cpu], thread, true);
      wake_cpu = links.cpu;
    } else if (!links.queued && !links.has_run) {
      // Created suspended and never queued, nothing is on its stack.
      return true;
    }
    // A queued thread diverts at its resume point, and one that already
    // exited or crashed is the dispatcher's to reclaim.
  }
  if (wake_cpu >= 0 && cpus_[wake_cpu].parked.load() &&
      cpus_[wake_cpu].ready_event) {
    WakeDispatchThread(cpus_[wake_cpu]);
  }
  return false;
}

void GuestScheduler::SwitchTo(XThread* next) {
  assert_not_null(next);
  assert_not_null(next->fiber());
  auto& links = next->scheduler_links();
  if (!links.has_run) {
    links.has_run = true;
    dispatched_any_.store(true);
    XELOGI("GuestScheduler: first run tid={:08X} '{}'", next->thread_id(),
           next->thread_name());
  }
  {
    TimedLock lock(this);
    assert_true(links.running);
    cpus_[CurrentCpu()].switch_seq.fetch_add(1, std::memory_order_relaxed);
    cpus_[CurrentCpu()].current_thread = next;
    // Grant a fresh slice only if the previous one was consumed. A preempted
    // thread resumes with its remainder, so its quantum end still arrives.
    if (!links.quantum_deadline_tick) {
      links.quantum_deadline_tick =
          Clock::host_tick_count_raw() + quantum_ticks_;
    }
    cpus_[CurrentCpu()].quantum_deadline_tick = links.quantum_deadline_tick;
    // hakuX-Boosted: guest_scheduler_skew_bound_us. A dispatch is a
    // scheduling decision: whoever readied a thread here has been answered.
    if (skew_bound_ticks_) {
      Cpu& cpu = cpus_[CurrentCpu()];
      cpu.skew_serve_seq.store(
          cpu.skew_serve_seq.load(std::memory_order_relaxed) + 1);
      cpu.skew_running.store(true);
    }
  }
  if (skew_bound_ticks_ && skew_holders_.load()) {
    SkewNotify();
  }
  stats_.switches.fetch_add(1, std::memory_order_relaxed);
  XThread::SetCurrentThread(next);
  next->guest_object<X_KTHREAD>()->thread_state = KTHREAD_STATE_RUNNING;
  // A flag raised while this fiber was off-CPU is stale, the dispatcher
  // already served it. A raise racing this clear is restored by the watchdog.
  next->thread_state()->context()->preempt_requested = 0;
  // hakuX-Boosted: guest_scheduler_skew_bound_us. A probe the watchdog
  // raised since the lock above was released must not lose its flag to that
  // clear, or it would stay unanswered for a tick and this CPU would be
  // taken for stalled.
  if (skew_bound_ticks_ && cpus_[CurrentCpu()].skew_probe_tick.load()) {
    next->thread_state()->context()->preempt_requested = 1;
  }
  next->fiber()->SwitchTo();
  // Back on the idle fiber.
  {
    TimedLock lock(this);
    links.running = false;
    cpus_[CurrentCpu()].current_thread = nullptr;
    // hakuX-Boosted: guest_scheduler_skew_bound_us. No fiber runs here now:
    // a probe it left unanswered is void, and nobody waits for this CPU's
    // progress. The time is stored before the flag is lowered, so a reader
    // that sees no fiber running sees since when.
    if (skew_bound_ticks_) {
      Cpu& cpu = cpus_[CurrentCpu()];
      cpu.skew_idle_since_tick.store(Clock::host_tick_count_raw());
      cpu.skew_probe_tick.store(0);
      cpu.skew_running.store(false);
    }
  }
  if (skew_bound_ticks_ && skew_holders_.load()) {
    SkewNotify();
  }
  XThread::SetCurrentThread(nullptr);
}

void GuestScheduler::ReportGlobalLockHazard() {
  static constexpr size_t kMaxReports = 32;
  static constexpr size_t kMaxFrames = 32;

  XThread* self = XThread::GetCurrentThread();
  uint32_t tid = self ? self->thread_id() : 0;
  const char* name = self ? self->thread_name().c_str() : "?";

  cpu::StackWalker* stack_walker =
      kernel_state_->processor() ? kernel_state_->processor()->stack_walker()
                                 : nullptr;
  if (!stack_walker) {
    if (!global_lock_hazard_saturated_.exchange(true)) {
      XELOGW(
          "GuestScheduler: fiber tid={:08X} '{}' yielded while holding the "
          "global critical region (no stack walker to name the shim).",
          tid, name);
    }
    return;
  }

  uint64_t frame_pcs[kMaxFrames] = {};
  uint64_t stack_hash = 0;
  size_t frame_count =
      stack_walker->CaptureStackTrace(frame_pcs, 0, kMaxFrames, &stack_hash);
  if (!frame_count) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(global_lock_hazard_mutex_);
    if (!global_lock_hazard_stacks_.insert(stack_hash).second) {
      return;  // Already reported.
    }
    if (global_lock_hazard_stacks_.size() >= kMaxReports) {
      global_lock_hazard_saturated_.store(true, std::memory_order_relaxed);
    }
  }

  cpu::StackFrame frames[kMaxFrames] = {};
  stack_walker->ResolveStack(frame_pcs, frames, frame_count);

  uint32_t guest_lr = self ? uint32_t(self->thread_state()->context()->lr) : 0;
  // The region is a scoped lock, so the acquiring shim is an ancestor frame.
  XELOGW(
      "GuestScheduler: fiber tid={:08X} '{}' yielded while holding the global "
      "critical region (guest lr={:08X}). A co-resident fiber can now re-enter "
      "the recursive lock. Yield path host stack:",
      tid, name, guest_lr);
  for (size_t i = 0; i < frame_count; ++i) {
    cpu::StackFrame& frame = frames[i];
    if (frame.type == cpu::StackFrame::Type::kHost) {
      XELOGW("  #{:02} host  {:016X} {}", i, frame.host_pc,
             frame.host_symbol.name[0] ? frame.host_symbol.name : "?");
    } else {
      XELOGW("  #{:02} guest {:016X} pc={:08X}", i, frame.host_pc,
             frame.guest_pc);
    }
  }
}

void GuestScheduler::YieldToScheduler() {
  if (!OnDispatchThread("YieldToScheduler")) {
    return;
  }
  if (!global_lock_hazard_saturated_.load(std::memory_order_relaxed) &&
      xe::global_critical_region::is_held_by_current_thread()) {
    ReportGlobalLockHazard();
  }
  cpus_[CurrentCpu()].idle_fiber->SwitchTo();
}

void GuestScheduler::ExitIfTerminated() {
  XThread* self = XThread::GetCurrentFiberThread();
  if (!self || !self->scheduler_links().terminate_pending.load(
                   std::memory_order_relaxed)) {
    return;
  }
  // The wait registration may be newer than the one Terminate abandoned.
  XObject::AbandonCooperativeWait(self);
  // A park or dispatch since the terminate may have overwritten this.
  self->guest_object<X_KTHREAD>()->thread_state = KTHREAD_STATE_TERMINATED;
  NotifyThreadExited(self);
  YieldToScheduler();  // never returns
}

bool GuestScheduler::YieldCurrentThread(bool quantum_end, bool to_lower,
                                        YieldCause cause,
                                        XThread* known_self) {
  // hakuX-Boosted: the empty yield first, and by itself. A title's main
  // thread makes half a million of these a second with nothing else to run
  // (run 90's profile: the yield and dispatch path was most of the 48% of
  // its core that was not translated code). Everything the long way does
  // for such a call is in TryEmptyYield; anything else falls through.
  if (lean_yield_ && (cause == YieldCause::kNtYield ||
                      cause == YieldCause::kZeroDelay)) {
    XThread* fiber_thread =
        known_self ? known_self : XThread::GetCurrentFiberThread();
    if (fiber_thread) {
      const int answer = TryEmptyYield(fiber_thread, quantum_end);
      if (answer >= 0) {
        return answer != 0;
      }
      known_self = fiber_thread;
    }
  }
  if (!OnDispatchThread("YieldCurrentThread")) {
    return false;
  }
  // An externally terminated thread stops here.
  ExitIfTerminated();
  XThread* self = known_self ? known_self : XThread::GetCurrentThread();
  auto& links = self->scheduler_links();
  // hakuX-Boosted: a yield the guest asked for means it is not sitting in a
  // spin-wait, which ends a poll-loop watch (the JIT hooks keep their own).
  const bool guest_call =
      cause == YieldCause::kZeroDelay || cause == YieldCause::kNtYield;
  if (guest_call) {
    cpus_[CurrentCpu()].poll_watch_thread = nullptr;
  }
  // hakuX-Boosted: guest_scheduler_skew_bound_us. A yield shows the thread
  // is being run, so it answers the watchdog's probe; the look at the other
  // CPUs that every probe is followed by is made here only for
  // NtYieldExecution, which guest code calls directly and where a guest
  // sleep could stand. Any other caller may be kernel code in the middle of
  // something: the look is then owed, and the flag raised on the way out
  // takes the thread to its next JIT safepoint for it. That also covers a
  // round trip through the dispatcher, which clears the flag.
  struct SkewOwedGuard {
    XThread* thread;
    ~SkewOwedGuard() {
      if (thread && thread->scheduler_links().skew_eval_owed) {
        thread->thread_state()->context()->preempt_requested = 1;
      }
    }
  } skew_owed_guard{skew_bound_ticks_ ? self : nullptr};
  if (skew_bound_ticks_) {
    if (SkewAnswerProbe(cpus_[CurrentCpu()])) {
      links.skew_eval_owed = true;
    }
    if (links.skew_eval_owed && cause == YieldCause::kNtYield &&
        CanParkSpinner(self)) {
      links.skew_eval_owed = false;
      SkewEvaluateAndHold(self);
    }
  }
  // hakuX-Boosted: NtYieldExecution asks for a quantum end, which kept it out
  // of the path below: every call went to the dispatch loop and back even
  // with nothing else to run. Behind the setting it takes the same way out
  // as a zero delay. What the round trip did besides switching is kept: the
  // priority decays as at any quantum end, NO_YIELD_PERFORMED is returned,
  // and the dispatch loop (the timed re-polls of this CPU's parked waiters,
  // the counters) still runs when the slice is used up, because the watchdog
  // then raises preempt_requested and the test below fails. The slice is
  // not renewed on the way, so that happens once a slice, not never.
  const bool lean = quantum_end && cause == YieldCause::kNtYield &&
                    (cvars::guest_scheduler_lean_yield ||
                     cvars::guest_scheduler_park_yield_spinners);
  // Nothing to yield to: skip the dispatcher round trip, which would otherwise
  // enqueue and immediately resume this same fiber. Sleep(0) spin-waits drive
  // this hundreds of thousands of times a second.
  //
  // Costs at most a quantum of latency - every condition below is re-armed
  // elsewhere (ready_summary, repoll_now, preempt_requested, the watchdog).
  // The reads are lock-free and may be stale; a stale "idle" just means the
  // next call takes the slow path.
  if ((!quantum_end || lean) && !links.preempted &&
      self->suspend_count() == 0 &&
      !self->thread_state()->context()->preempt_requested) {
    const int empty_cpu_index = CurrentCpu();
    Cpu& cpu = cpus_[empty_cpu_index];
    if (cpu.ready_summary.load(std::memory_order_relaxed) == 0 &&
        !cpu.repoll_now.load(std::memory_order_relaxed)) {
      if (lean) {
        self->OnQuantumEnd();
      }
      // hakuX-Boosted: counted, and a thread that does nothing but this is
      // parked behind guest_scheduler_park_yield_spinners.
      cpu.empty_yields.store(
          cpu.empty_yields.load(std::memory_order_relaxed) + 1,
          std::memory_order_relaxed);
      if (guest_call && cvars::guest_scheduler_park_yield_spinners &&
          !park_waits_) {
        uint64_t seq_before_park =
            cpu.switch_seq.load(std::memory_order_relaxed);
        if (NoteEmptyYield(cpu, self)) {
          // Parked and back. One dispatch is our own.
          return CurrentCpu() != empty_cpu_index ||
                 cpus_[empty_cpu_index].switch_seq.load(
                         std::memory_order_relaxed) -
                         seq_before_park >
                     1;
        }
      }
      return false;  // nothing else ran, which is exactly what we report
    }
  }
  stats_.leaves[size_t(cause)].fetch_add(1, std::memory_order_relaxed);
  // A slice cut short by a higher-priority thread is not a quantum end, that
  // thread re-runs at the head instead.
  if (quantum_end && !links.preempted) {
    self->OnQuantumEnd();
  }
  // Only a preemption keeps the remaining slice, anything else consumed it.
  if (!links.preempted) {
    links.quantum_deadline_tick = 0;
  }
  int cpu_index = CurrentCpu();
  uint64_t seq_before =
      cpus_[cpu_index].switch_seq.load(std::memory_order_relaxed);
  // Re-queue on the current CPU, not the affinity CPU, because our context is
  // not saved until the yield below and another CPU must not grab it yet.
  EnqueueReady(self, CurrentCpu(), to_lower);
  YieldToScheduler();
  // Terminated while queued.
  ExitIfTerminated();
  // One dispatch is our own resume, more means another fiber ran in between.
  // A migration to another CPU counts as scheduling activity outright.
  const uint64_t seq_after =
      cpus_[cpu_index].switch_seq.load(std::memory_order_relaxed);
  const bool ran_other =
      CurrentCpu() != cpu_index || seq_after - seq_before > 1;
  // hakuX-Boosted: a round trip that ran nobody else (the one a slice end
  // forces) does not break a run of empty yields.
  if (!ran_other) {
    Cpu& cpu = cpus_[cpu_index];
    if (cpu.streak_thread == self && cpu.streak_seq == seq_before) {
      cpu.streak_seq = seq_after;
    }
  }
  return ran_other;
}

// hakuX-Boosted: a spinner may only be parked where a guest sleep could have
// been: outside any wait, below DISPATCH_LEVEL, not under the global lock.
static bool CanParkSpinner(XThread* self) {
  if (self->cooperative_wait_object() || self->cooperative_wait_set_count()) {
    return false;
  }
  if (static_cast<XThread::CooperativeWaitKind>(
          self->scheduler_links().wait_kind) !=
      XThread::CooperativeWaitKind::kNone) {
    return false;
  }
  if (xe::global_critical_region::is_held_by_current_thread()) {
    return false;
  }
  auto* context = self->thread_state()->context();
  auto* kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
  return kpcr->current_irql < 2;
}

void GuestScheduler::ParkSpinner(XThread* self) {
  // A timed sleep to the scheduler: gated on its deadline alone, so this
  // CPU's dispatch thread sleeps in its ready event, which any fiber made
  // ready here and any poke ends at once for them. The deadline is the next
  // millisecond but one of the host's uptime: between 1 and 2 ms away, as
  // near 1 as the host's timer allows.
  self->set_cooperative_wait_shape(XThread::CooperativeWaitKind::kDelay,
                                   nullptr, 0);
  BlockCurrentThread(Clock::QueryHostUptimeMillis() + 1, 0, false);
  self->clear_cooperative_wait_shape();
}

bool GuestScheduler::NoteEmptyYield(Cpu& cpu, XThread* self) {
  if (!yield_park_after_ticks_) {
    return false;
  }
  const uint64_t seq = cpu.switch_seq.load(std::memory_order_relaxed);
  if (cpu.streak_thread != self) {
    // Another thread yielding here: a new run.
    stats_.yield_park_new_thread.fetch_add(1, std::memory_order_relaxed);
    cpu.streak_thread = self;
    cpu.streak_seq = seq;
    cpu.streak_calls = 0;
    cpu.streak_start_tick = 0;
    cpu.streak_check_tick = 0;
  } else if (cpu.streak_seq != seq) {
    // hakuX-Boosted: something else was dispatched on this CPU since. That
    // used to start the run again, and on device (run 73, Fable II) a
    // spinner making millions of empty yields a second was parked about
    // five times a second: other threads wake on its CPU more often than
    // every 2 ms, so its run never grew old enough. A short visit by
    // another thread does not make the spinner any less of a spinner; a
    // long one still restarts the run through the cadence test below.
    stats_.yield_park_others_ran.fetch_add(1, std::memory_order_relaxed);
    cpu.streak_seq = seq;
  }
  // The clock is read on every 32nd call.
  if (++cpu.streak_calls & 31) {
    return false;
  }
  const uint64_t now = Clock::host_tick_count_raw();
  const bool quick = cpu.streak_check_tick &&
                     now - cpu.streak_check_tick <= 32 * yield_gap_ticks_;
  cpu.streak_check_tick = now;
  if (!quick || !cpu.streak_start_tick) {
    // The first look, or the thread did something between its yields: the
    // run's age starts again.
    stats_.yield_park_slow.fetch_add(1, std::memory_order_relaxed);
    cpu.streak_start_tick = now;
    return false;
  }
  if (now - cpu.streak_start_tick < yield_park_after_ticks_) {
    stats_.yield_park_young.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  if (!CanParkSpinner(self)) {
    stats_.yield_park_barred.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  const uint64_t start = cpu.streak_start_tick;
  stats_.yield_parks.fetch_add(1, std::memory_order_relaxed);
  ParkSpinner(self);
  // Back, possibly on another CPU. The run keeps its age, so 32 more quick
  // empty yields park it again; anything slower starts it afresh.
  Cpu& here = cpus_[CurrentCpu()];
  here.streak_thread = self;
  here.streak_seq = here.switch_seq.load(std::memory_order_relaxed);
  here.streak_calls = 0;
  here.streak_start_tick = start;
  here.streak_check_tick = Clock::host_tick_count_raw();
  return true;
}

// hakuX-Boosted: the empty yield's short way.
//
// What an empty NtYieldExecution or zero delay cost, by run 90's profile of
// the title's main thread: the thread-local resolver (the thread, the CPU
// index, the bound context: six or seven lookups a call), and for
// NtYieldExecution the whole round trip through the dispatcher: four takes
// of the scheduler lock (the mutex and its compare-and-swap), two fiber
// switches, two clock reads, the counters' atomic adds, the thread id and
// context rebound twice.
//
// Here the thread comes from the caller (found from the PPC context), the
// CPU is the one the running fiber is linked to, and with nothing ready,
// poked or asked of the thread the call returns after a handful of loads and
// one plain store: no lock, no clock, no switch, no atomic read-modify-write.
// Every test is of something that is raised again by its own source (the
// ready summary, the poke, the preempt flag, the watchdog), so a stale read
// costs one call. The preempt flag is up at the end of every slice and for
// every probe of the skew bound, which sends the next call the long way:
// the dispatch loop still makes its timed passes once a slice.
int GuestScheduler::TryEmptyYield(XThread* self, bool quantum_end) {
  auto& links = self->scheduler_links();
  // A running fiber is linked to the CPU whose dispatch thread runs it
  // (every path that queues a thread sets links.cpu to the CPU it queues it
  // on, under the lock), so this is CurrentCpu() without the lookup.
  const int cpu_index = links.cpu;
  if (cpu_index < 0 || cpu_index >= kMaxCpus || !links.running) {
    return -1;
  }
  if (links.preempted ||
      links.terminate_pending.load(std::memory_order_relaxed) ||
      self->thread_state()->context()->preempt_requested ||
      self->suspend_count() != 0) {
    return -1;
  }
  Cpu& cpu = cpus_[cpu_index];
  if (cpu.ready_summary.load(std::memory_order_relaxed) != 0 ||
      cpu.repoll_now.load(std::memory_order_relaxed)) {
    return -1;
  }
  // The skew bound's probe and its owed look are the long way's business.
  if (skew_bound_ticks_ &&
      (links.skew_eval_owed ||
       cpu.skew_probe_tick.load(std::memory_order_relaxed))) {
    return -1;
  }
  if (cpu.poll_watch_thread) {
    cpu.poll_watch_thread = nullptr;
  }
  if (quantum_end) {
    self->OnQuantumEnd();
  }
  cpu.empty_yields.store(
      cpu.empty_yields.load(std::memory_order_relaxed) + 1,
      std::memory_order_relaxed);
  const uint64_t seq_before = cpu.switch_seq.load(std::memory_order_relaxed);
  bool parked = false;
  if (park_waits_) {
    parked = NoteWaitSpin(self);
  } else if (cvars::guest_scheduler_park_yield_spinners) {
    parked = NoteEmptyYield(cpu, self);
  }
  if (parked) {
    // Parked and back, perhaps on another CPU. One dispatch is our own.
    return (self->scheduler_links().cpu != cpu_index ||
            cpus_[cpu_index].switch_seq.load(std::memory_order_relaxed) -
                    seq_before >
                1)
               ? 1
               : 0;
  }
  return 0;  // nothing else ran, which is what the caller reports
}

// hakuX-Boosted: guest_scheduler_park_waits.
//
// Which threads. One that busy-waits: either it calls NtYieldExecution or a
// zero delay again and again with nothing else to run and no work between
// the calls (NoteWaitSpin: 32 calls within 640 us), or it goes round a loop
// in translated code that the translator has shown to do nothing but pause
// and read memory, with the same registers on every pass (PollSiteVisit).
// Each burst of waiting is judged on its own: a thread's own short waits
// between bursts end a burst and do nothing else, which is why a thread
// that spins for 2 ms at a time is parked at all.
//
// When. Once the burst is guest_scheduler_park_wait_after_us old. A wait
// that is over sooner is never touched.
//
// For how long. Until the first of:
// - an event that can end a busy wait (NoteWaitEvent: a kernel object
//   signalled, a critical section left, another guest thread parking in a
//   wait or exiting, an APC queued, a GPU interrupt delivered), but no
//   sooner than kWaitParkMinUs after the park began;
// - its deadline: kWaitParkFirstUs for the first park of a burst, twice the
//   last after each, at most guest_scheduler_park_wait_max_us;
// - a coarse millisecond deadline behind that, and the 64 ms backstop.
// The thread then runs its own test again. Nothing decides for it that the
// wait is over, so a park can only ever cost time, and never more than the
// longest park per pass: with the default 1 ms that is a thirtieth of a
// 30 Hz frame, and at the clamp (4 ms) still a quarter of a 60 Hz one.
//
// No event is slept through. Parking takes two looks. At the first the
// thread arms: from then on every event moves wait_wake_seq_. At the next
// look (the guest has run its test at least once in between) it parks only
// if the sequence has not moved, and BlockCurrentThread compares it once
// more under the lock after counting the thread as parked. An event's
// source moves the sequence and then reads the parked count; the parker
// counts itself and then reads the sequence: one of the two sees the other.
// An event in that last window shortens the park to kWaitParkMinUs.
//
// What is not seen: a plain store to memory by another running guest thread
// or by a host thread (the GPU's read pointer). For those the deadline is
// the answer, which is why it starts at a tenth of a millisecond.
//
// On the dispatch thread. A parked spinner is an entry of the blocked list
// with wait_park set. Its CPU's dispatch thread sleeps until the entry's
// deadline on a condition variable (FineSleep), and anything made ready on
// that CPU wakes it at once as before.
bool GuestScheduler::NoteWaitSpin(XThread* self) {
  auto& links = self->scheduler_links();
  // The clock is read on every 32nd call.
  if (++links.spin_calls & 31) {
    return false;
  }
  const uint64_t now = Clock::host_tick_count_raw();
  const uint64_t prev = links.spin_check_tick;
  links.spin_check_tick = now;
  if (links.poll_site) {
    // It was last seen in a poll loop: this is another wait.
    links.poll_site = 0;
    links.spin_burst_tick = 0;
  }
  if (!prev || now - prev > 32 * yield_gap_ticks_ || !links.spin_burst_tick) {
    // The first look of a burst (after a wait of the thread's own), or the
    // thread did something between its yields: a new burst.
    links.spin_burst_tick = now;
    links.spin_park_shift = 0;
    links.wait_armed = false;
    return false;
  }
  if (now - links.spin_burst_tick < wait_park_after_ticks_) {
    return false;
  }
  return TryWaitPark(self, now, false);
}

bool GuestScheduler::TryWaitPark(XThread* self, uint64_t now, bool poll) {
  auto& links = self->scheduler_links();
  if (!CanParkSpinner(self)) {
    stats_.wait_looks_barred.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  // Events are counted while now < wait_armed_until_ (or a thread is
  // parked). Raised here without a compare-and-swap: two threads arming at
  // once can leave the earlier of their two times, which costs the other at
  // most one event it then waits out its deadline for.
  const uint64_t until = now + wait_arm_window_ticks_;
  if (wait_armed_until_.load(std::memory_order_relaxed) < until) {
    wait_armed_until_.store(until);
  }
  const uint64_t seq = wait_wake_seq_.load();
  if (!links.wait_armed || now - links.wait_arm_tick >= wait_arm_window_ticks_) {
    links.wait_armed = true;
    links.wait_arm_tick = now;
    links.wait_arm_seq = seq;
    return false;
  }
  if (links.wait_arm_seq != seq) {
    // Something happened since the last look: the guest's next passes test
    // for it, and the look after them decides.
    links.wait_arm_tick = now;
    links.wait_arm_seq = seq;
    stats_.wait_looks_event.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  uint64_t length = wait_park_first_ticks_ << links.spin_park_shift;
  if (length >= wait_park_max_ticks_) {
    length = wait_park_max_ticks_;
  } else {
    ++links.spin_park_shift;
  }
  (poll ? stats_.wait_parks_poll : stats_.wait_parks_yield)
      .fetch_add(1, std::memory_order_relaxed);
  WaitPark(self, now, length);
  // Back, perhaps on another CPU. The burst keeps its age and the thread
  // stays armed with the sequence it parked on: events during the park
  // moved it, and the looks that follow see that.
  auto& after = self->scheduler_links();
  const uint64_t back = Clock::host_tick_count_raw();
  after.spin_check_tick = back;
  after.spin_calls = 0;
  after.poll_visits = 0;
  after.wait_arm_tick = back;
  const uint64_t again = back + wait_arm_window_ticks_;
  if (wait_armed_until_.load(std::memory_order_relaxed) < again) {
    wait_armed_until_.store(again);
  }
  return true;
}

void GuestScheduler::WaitPark(XThread* self, uint64_t now,
                              uint64_t length_ticks) {
  auto& links = self->scheduler_links();
  links.wait_park = true;
  links.wait_park_start_tick = now;
  links.wait_deadline_tick = now + length_ticks;
  links.wait_min_tick =
      now + std::min<uint64_t>(wait_park_min_ticks_, length_ticks);
  // Events stay counted for the whole park and the window after it.
  const uint64_t until = now + length_ticks + wait_arm_window_ticks_;
  if (wait_armed_until_.load(std::memory_order_relaxed) < until) {
    wait_armed_until_.store(until);
  }
  // A timed sleep to the rest of the scheduler, with a millisecond deadline
  // behind the fine one in case the fine one is not served.
  const double ticks_per_us = ticks_per_us_ > 0.0 ? ticks_per_us_ : 1.0;
  const uint64_t length_ms =
      uint64_t(double(length_ticks) / ticks_per_us) / 1000 + 2;
  self->set_cooperative_wait_shape(XThread::CooperativeWaitKind::kDelay,
                                   nullptr, 0);
  BlockCurrentThread(Clock::QueryHostUptimeMillis() + length_ms, 0, false);
  self->clear_cooperative_wait_shape();
}

void GuestScheduler::WakeParkedWaits() {
  const uint64_t until = wait_armed_until_.load(std::memory_order_relaxed);
  if (!wait_parked_total_.load(std::memory_order_relaxed) &&
      Clock::host_tick_count_raw() >= until) {
    // Nobody is parked or about to park.
    return;
  }
  wait_wake_seq_.fetch_add(1);
  stats_.wait_events.fetch_add(1, std::memory_order_relaxed);
  if (!wait_parked_total_.load()) {
    return;
  }
  // A wake already asked for and not yet carried out covers this one.
  bool any = false;
  for (int i = 0; i < kMaxCpus; ++i) {
    if (cpus_[i].wait_parked.load(std::memory_order_relaxed) &&
        !cpus_[i].wait_wake_noted.load(std::memory_order_relaxed)) {
      any = true;
      break;
    }
  }
  if (!any) {
    return;
  }
  bool wake[kMaxCpus] = {};
  {
    TimedLock lock(this);
    for (int i = 0; i < kMaxCpus; ++i) {
      Cpu& cpu = cpus_[i];
      if (!cpu.wait_parked.load(std::memory_order_relaxed) ||
          cpu.wait_wake_noted.load(std::memory_order_relaxed)) {
        continue;
      }
      cpu.wait_wake_noted.store(true, std::memory_order_relaxed);
      cpu.wake_wait_parks = true;
      cpu.repoll_now.store(true, std::memory_order_relaxed);
      wake[i] = true;
    }
  }
  for (int i = 0; i < kMaxCpus; ++i) {
    if (wake[i] && cpus_[i].parked.load() && cpus_[i].ready_event) {
      WakeDispatchThread(cpus_[i]);
    }
  }
}

void GuestScheduler::DropWaitParkLocked(XThread* thread) {
  auto& links = thread->scheduler_links();
  if (links.wait_park) {
    links.wait_park = false;
    wait_parked_total_.fetch_sub(1);
    // The CPU's own count is put right by its next RereadyBlocked pass.
  }
}

// hakuX-Boosted: a visit to a pause-and-poll loop's site (park_pause_poll_loops
// in the translator, MemoryPollParkPass). The pass has shown that the loop
// the site is in contains nothing but pauses, loads of guest memory, pure
// arithmetic and its own branches: no store to memory, no call, no atomic, no
// clock, no trap. So a pass through it changes nothing but the thread's own
// registers, and the site sits on the way back to the top.
//
// The thread is only parked when, on top of that, its registers are the same
// on every eighth visit for guest_scheduler_park_wait_after_us, with no more
// than kPollGapUs between two looks. A loop that advances anything of its
// own (a string scan's pointer, a retry counter) never has the same
// registers twice and is never parked; one that has can only be waiting for
// memory to change under it, or for nothing at all. Either way the park
// ends by an event or its deadline and the loop runs its own test again:
// the park cannot take the thread out of the loop or keep it in.
static uint64_t HashGuestRegisters(const cpu::ppc::PPCContext* context) {
  // Everything before thread_id is architectural state (the condition and
  // status registers, r0-r31, CTR, LR, MSR, the floating-point and vector
  // registers); what follows is the emulator's own. Padding is zeroed when
  // the context is made and never written.
  const size_t words =
      (offsetof(cpu::ppc::PPCContext, thread_id) + sizeof(uint64_t) - 1) /
      sizeof(uint64_t);
  const uint64_t* p = reinterpret_cast<const uint64_t*>(context);
  uint64_t h = 0x9E3779B97F4A7C15ull;
  for (size_t i = 0; i < words; ++i) {
    h = (h ^ p[i]) * 0xFF51AFD7ED558CCDull;
    h ^= h >> 29;
  }
  return h;
}

void GuestScheduler::PollSiteVisit(XThread* self, void* raw_context,
                                   uint32_t site) {
  auto* context = static_cast<cpu::ppc::PPCContext*>(raw_context);
  if (context->preempt_requested) {
    // Asked to give way, or probed: what any backoff site does.
    SpinBackoffYieldFiber(raw_context);
    return;
  }
  if (!park_waits_) {
    return;
  }
  auto& links = self->scheduler_links();
  if (links.poll_site != site) {
    links.poll_site = site;
    links.poll_visits = 0;
    links.spin_check_tick = 0;
    links.wait_armed = false;
  }
  if (++links.poll_visits & 7) {
    return;
  }
  const uint64_t now = Clock::host_tick_count_raw();
  const uint64_t prev = links.spin_check_tick;
  links.spin_check_tick = now;
  const uint64_t hash = HashGuestRegisters(context);
  stats_.poll_visits.fetch_add(1, std::memory_order_relaxed);
  if (!prev || now - prev > poll_gap_ticks_ || hash != links.poll_hash) {
    if (prev && hash != links.poll_hash) {
      stats_.poll_changed.fetch_add(1, std::memory_order_relaxed);
    }
    links.poll_hash = hash;
    links.spin_burst_tick = now;
    links.spin_park_shift = 0;
    links.wait_armed = false;
    return;
  }
  if (now - links.spin_burst_tick < wait_park_after_ticks_) {
    return;
  }
  TryWaitPark(self, now, true);
}

// hakuX-Boosted: guest_scheduler_park_poll_loops.
//
// The translator marks spin-waits (MemoryPollParkPass's load-test-branch loops,
// the collapsed spin-backoff loops) with a backoff site, which calls
// SpinBackoffYieldFiber, but only while preempt_requested is up; and the
// loop's own safepoint at its head usually sees the flag first and clears
// it. So under this scheduler such a loop was never parked: once a slice it
// yielded, found nothing else ready and went on spinning its dispatch
// thread (Fable II's 3D Engine thread spends half its time so).
//
// The watch finds a thread that stays in such a loop without touching the
// translator. After a slice-end yield the flag is raised again once
// (PollWatchArm). If the next hook to see it is a safepoint, the thread is in
// ordinary code and the watch ends there, one cheap call a slice. If it is a
// backoff site, the thread is in a spin-wait: the flag is then kept up, so
// every pass of the loop calls in, alternately from its safepoint and its
// backoff site. Two safepoints running mean it left the loop. A thread seen
// in the loop for kPollParkAfterUs is parked for about a millisecond per
// pass (ParkSpinner) until it leaves. The loop itself is untouched: it
// still tests what it polls on every pass, at most about a millisecond
// apart, and a wait shorter than the threshold is never slowed.
bool GuestScheduler::PollWatchAtSafepoint(XThread* self) {
  if (!cvars::guest_scheduler_park_poll_loops) {
    return false;
  }
  const int cpu_index = CurrentCpu();
  if (cpu_index < 0) {
    return false;
  }
  Cpu& cpu = cpus_[cpu_index];
  if (cpu.poll_watch_thread != self) {
    return false;
  }
  auto& links = self->scheduler_links();
  if (links.preempted || self->suspend_count() != 0 ||
      links.terminate_pending.load(std::memory_order_relaxed)) {
    // A real request came in under the watch's flag: the ordinary path.
    cpu.poll_watch_thread = nullptr;
    return false;
  }
  if (cpu.poll_watch_expects_backoff) {
    // Not a spin-wait. The translated code cleared the flag; a slice end the
    // watch's flag hid is raised again by the watchdog's next tick.
    cpu.poll_watch_thread = nullptr;
    return true;
  }
  cpu.poll_watch_expects_backoff = true;
  self->thread_state()->context()->preempt_requested = 1;
  return true;
}

void GuestScheduler::PollWatchArm(XThread* self) {
  if (!cvars::guest_scheduler_park_poll_loops || !poll_park_after_ticks_) {
    return;
  }
  const int cpu_index = CurrentCpu();
  if (cpu_index < 0) {
    return;
  }
  Cpu& cpu = cpus_[cpu_index];
  cpu.poll_watch_thread = self;
  cpu.poll_watch_expects_backoff = true;
  cpu.poll_watch_start_tick = 0;
  self->thread_state()->context()->preempt_requested = 1;
}

bool GuestScheduler::PollWatchAtBackoff(XThread* self) {
  if (!cvars::guest_scheduler_park_poll_loops || !poll_park_after_ticks_) {
    return false;
  }
  const int cpu_index = CurrentCpu();
  if (cpu_index < 0) {
    return false;
  }
  Cpu& cpu = cpus_[cpu_index];
  auto& links = self->scheduler_links();
  const uint64_t now = Clock::host_tick_count_raw();
  const uint64_t start =
      cpu.poll_watch_thread == self && cpu.poll_watch_start_tick
          ? cpu.poll_watch_start_tick
          : now;
  // What the plain yield here did comes first: give way to anything ready or
  // poked. With the flag held up by the watch the watchdog's raise at the
  // slice's end is not seen, so the slice is timed here: its end is where
  // this CPU's dispatch loop makes its timed passes.
  const bool give_way =
      links.preempted || self->suspend_count() != 0 ||
      links.terminate_pending.load(std::memory_order_relaxed) ||
      cpu.ready_summary.load(std::memory_order_relaxed) != 0 ||
      cpu.repoll_now.load(std::memory_order_relaxed) ||
      !links.quantum_deadline_tick || now >= links.quantum_deadline_tick;
  if (give_way) {
    YieldCurrentThread(false, true, YieldCause::kSpinSite);
  } else if (now - start >= poll_park_after_ticks_ && CanParkSpinner(self)) {
    stats_.poll_parks.fetch_add(1, std::memory_order_relaxed);
    ParkSpinner(self);
  }
  // Still watched, on whichever CPU this now is.
  Cpu& here = cpus_[CurrentCpu()];
  here.poll_watch_thread = self;
  here.poll_watch_expects_backoff = false;
  here.poll_watch_start_tick = start;
  self->thread_state()->context()->preempt_requested = 1;
  return true;
}

// hakuX-Boosted: guest_scheduler_skew_bound_us, the bound on the skew between
// guest CPUs (boosted/notes/guest-skew-bound.md has the reasoning at length).
//
// What it is for. Each guest CPU is a host thread, and the phone can leave
// one without a core for tens of milliseconds while the other five run on.
// The console never does that, and titles rely on it: Fable II starts a
// worker and comes back for its result a frame later holding a lock the
// worker needs. So a guest thread must not run far ahead of a guest CPU
// that should be running and is not.
//
// How a CPU is known to be behind, without a check in translated code:
// - The probe. Every watchdog tick each running fiber's CPU gets
//   skew_probe_tick = now (unless one is still outstanding) and the fiber's
//   preempt flag. The fiber answers at its next safepoint, yield or
//   backoff site by storing 0. A probe outstanding for longer than the
//   bound means that host thread is not reaching guest code: descheduled,
//   or deep in host code. That CPU is "stalled".
// - The debt. A guest thread whose signal readies a thread on another CPU
//   notes that CPU's skew_serve_seq, which moves at every dispatch there and
//   every time that CPU finds, under the lock, nothing to run. Until it
//   moves the signal has not been acted on. If meanwhile no fiber runs
//   there for longer than the bound (counted from the signal, or from when
//   its last fiber left if that is later), that CPU's host thread was woken
//   and has not turned up.
//
// The two gates, both judged by a fiber for itself, once per probe, at a
// place where a guest sleep could stand (CanParkSpinner: not in a wait,
// below DISPATCH_LEVEL, not under the global lock):
// - Start gate: it has a debt on a CPU that is behind as above. Only the
//   signaller waits.
// - Progress gate: another CPU is stalled. Every running thread waits.
//
// The hold. The fiber stays the running fiber of its CPU and its host
// thread sleeps on skew_cv_ until the awaited CPU acts or answers, the hard
// cap passes, or something asks this thread to give way (a preemption by a
// higher-priority thread, a terminate, a suspend, shutdown). Nothing else
// on the holder's CPU runs meanwhile: that CPU stands still in guest time,
// which is the point. It was chosen over running the readied fiber on the
// signaller's or an idle dispatch thread because a guest thread carries its
// CPU number, and two fibers of one guest CPU running at once on two host
// threads break titles that keep per-processor data without a lock.
//
// Why the scheduler itself cannot deadlock on this:
// - A hold takes no lock while it sleeps but skew_mutex_ (inside the
//   condition wait), which is a leaf: never taken under lock_, and nothing
//   is taken under it.
// - A CPU in a hold counts as on time (skew_holding): it is never the
//   target of a hold, and every target is a CPU that is not holding, so
//   there is no cycle of holders. The awaited side never waits for the
//   holder to release anything of the scheduler's.
// - The awaited condition is liveness of a host thread (a dispatch, a
//   verified idle pass, an answered probe), never progress of the guest
//   program, so a guest lock the holder owns cannot keep the hold from
//   ending: the awaited thread ends it by running into that lock.
// - Every hold ends at the cap, and a capped hold is not repeated for the
//   same lapse (the debt is dropped, the probe is marked).
// - A held thread is at a JIT safepoint or at the entry of NtYieldExecution
//   and holds no host lock there, the same assumption every fiber switch at
//   a safepoint already makes. The global critical region is tested.
bool GuestScheduler::SkewAnswerProbe(Cpu& cpu) {
  if (!cpu.skew_probe_tick.load(std::memory_order_relaxed)) {
    return false;
  }
  // Sequentially consistent with the holders' count: either the load below
  // sees a holder and wakes it, or that holder's look at this CPU, which
  // comes after its count went up, sees the probe answered.
  cpu.skew_probe_tick.store(0);
  if (skew_holders_.load()) {
    SkewNotify();
  }
  return true;
}

void GuestScheduler::SkewNotify() {
  // Through the mutex, so a holder between its look and its sleep is not
  // missed. A miss would cost one slice of kSkewHoldSliceUs, no more.
  { std::lock_guard<std::mutex> lock(skew_mutex_); }
  skew_cv_.notify_all();
}

bool GuestScheduler::SkewCpuStalled(const Cpu& cpu, uint64_t now,
                                    uint64_t* probe_out) const {
  if (!cpu.skew_running.load() || cpu.skew_holding.load()) {
    return false;
  }
  const uint64_t probe = cpu.skew_probe_tick.load();
  if (!probe || now <= probe || now - probe <= skew_bound_ticks_) {
    return false;
  }
  if (probe == cpu.skew_capped_probe_tick.load(std::memory_order_relaxed)) {
    return false;
  }
  // hakuX-Boosted: guest_scheduler_skew_host_progress. The probe is overdue,
  // so the thread is not reaching guest code. If the watchdog's last
  // reading of its CPU time, taken during this probe and no more than three
  // ticks ago, says it was on a core for half the tick or more, it is
  // working in host code and not behind in the sense that matters. A
  // descheduled thread gains no CPU time, so its verdict is as it was; with
  // no reading yet, or a stale one (the watchdog itself is late), the
  // safepoint alone decides, as before.
  if (skew_host_progress_) {
    const uint64_t sample =
        cpu.skew_oncore_sample_tick.load(std::memory_order_relaxed);
    if (sample > probe && now >= sample &&
        now - sample <= skew_sample_fresh_ticks_ &&
        cpu.skew_oncore_permille.load(std::memory_order_relaxed) >=
            kSkewOnCorePermille) {
      skew_host_busy_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
  }
  if (probe_out) {
    *probe_out = probe;
  }
  return true;
}

void GuestScheduler::SkewAddDebtLocked(XThread* debtor, int from_cpu,
                                       int to_cpu, uint32_t tid,
                                       uint64_t now) {
  if (to_cpu == from_cpu || to_cpu < 0 || to_cpu >= kMaxCpus) {
    return;
  }
  auto& links = debtor->scheduler_links();
  const uint8_t bit = uint8_t(1u << to_cpu);
  const uint64_t seq =
      cpus_[to_cpu].skew_serve_seq.load(std::memory_order_relaxed);
  // An older debt on that CPU that it has not acted on stands as it is: the
  // thread readied first is the one overdue.
  if ((links.skew_debt_mask & bit) && links.skew_debt_seq[to_cpu] == seq) {
    return;
  }
  links.skew_debt_mask |= bit;
  links.skew_debt_tick[to_cpu] = now;
  links.skew_debt_seq[to_cpu] = seq;
  links.skew_debt_tid[to_cpu] = tid;
  stats_.skew_debts.fetch_add(1, std::memory_order_relaxed);
}

void GuestScheduler::SkewAtSafepoint(XThread* self) {
  if (!skew_bound_ticks_) {
    return;
  }
  const int cpu_index = CurrentCpu();
  if (cpu_index < 0) {
    return;
  }
  auto& links = self->scheduler_links();
  const bool due = SkewAnswerProbe(cpus_[cpu_index]) || links.skew_eval_owed;
  if (!due) {
    return;
  }
  // Where the thread may not be held the look is dropped, not kept owed: a
  // thread at raised IRQL or under the global lock is one others may be
  // waiting for, and the next probe is a tick away.
  links.skew_eval_owed = false;
  if (CanParkSpinner(self)) {
    SkewEvaluateAndHold(self);
  }
}

void GuestScheduler::SkewAtBackoff(XThread* self) {
  if (!skew_bound_ticks_) {
    return;
  }
  const int cpu_index = CurrentCpu();
  if (cpu_index < 0) {
    return;
  }
  if (SkewAnswerProbe(cpus_[cpu_index])) {
    // The preempt flag is still up here (the backoff site only calls in
    // with it up and does not clear it), so the loop's safepoint is next.
    self->scheduler_links().skew_eval_owed = true;
  }
}

bool GuestScheduler::SkewOnlyProbed(XThread* self) {
  if (!skew_bound_ticks_) {
    return false;
  }
  // Every reason the flag is raised for, other than the probe, leaves a
  // mark that is tested here: a priority preemption or a wake that outranks
  // (preempted), a terminate, shutdown, the slice's end (the deadline). A
  // suspend raises no flag today and acts at the next yield; with the probe
  // it acts within a tick. Anything missed is raised again by its own
  // source or acts at the slice's end, as a flag lost to SwitchTo's clear
  // already does.
  auto& links = self->scheduler_links();
  if (links.preempted ||
      links.terminate_pending.load(std::memory_order_relaxed) ||
      shutting_down_.load(std::memory_order_relaxed) ||
      self->suspend_count() != 0) {
    return false;
  }
  const uint64_t deadline = links.quantum_deadline_tick;
  return deadline && Clock::host_tick_count_raw() < deadline;
}

void GuestScheduler::SkewEvaluateAndHold(XThread* self) {
  auto& links = self->scheduler_links();
  const int me = CurrentCpu();
  // One awaited CPU at a time; after a hold the look is taken afresh.
  for (int round = 0; round < kMaxCpus; ++round) {
    // Something already asks this thread to give way: that comes first,
    // and the look is made when the thread runs again.
    if (links.preempted ||
        links.terminate_pending.load(std::memory_order_relaxed) ||
        shutting_down_.load(std::memory_order_relaxed) ||
        self->suspend_count() != 0) {
      links.skew_eval_owed = true;
      return;
    }
    const uint64_t now = Clock::host_tick_count_raw();
    int target = -1;
    bool progress_gate = false;
    uint64_t behind_since = 0;
    // The start gate: the CPUs this thread readied a thread on.
    for (int i = 0; i < kMaxCpus && links.skew_debt_mask; ++i) {
      const uint8_t bit = uint8_t(1u << i);
      if (!(links.skew_debt_mask & bit)) {
        continue;
      }
      const Cpu& cpu = cpus_[i];
      if (i == me || cpu.skew_serve_seq.load() != links.skew_debt_seq[i]) {
        // Acted on (or this thread has moved there itself).
        links.skew_debt_mask &= uint8_t(~bit);
        continue;
      }
      uint64_t since = links.skew_debt_tick[i];
      if (cpu.skew_running.load()) {
        // A fiber runs there and the readied thread waits its turn behind
        // it, as it would on the console. Only if that fiber is itself not
        // being run is the CPU behind.
        uint64_t probe = 0;
        if (!SkewCpuStalled(cpu, now, &probe)) {
          continue;
        }
        since = std::max(since, probe);
      } else {
        // No fiber runs there: its host thread has been asked to come and
        // dispatch. Behind once that has lasted longer than the bound.
        since = std::max(since, cpu.skew_idle_since_tick.load());
        if (now <= since || now - since <= skew_bound_ticks_) {
          continue;
        }
      }
      target = i;
      behind_since = since;
      break;
    }
    // The progress gate: any other CPU whose running fiber is not being run.
    if (target < 0 && skew_progress_gate_) {
      for (int i = 0; i < kMaxCpus; ++i) {
        uint64_t probe = 0;
        if (i != me && SkewCpuStalled(cpus_[i], now, &probe)) {
          target = i;
          progress_gate = true;
          behind_since = probe;
          break;
        }
      }
    }
    if (target < 0) {
      return;
    }
    if (!SkewHold(self, target, progress_gate, behind_since)) {
      // Cut short: the thread gives way first and looks again afterwards.
      links.skew_eval_owed = true;
      return;
    }
  }
}

// Nanoseconds the dispatch thread of |cpu| has spent on a core, -1 unknown.
static int64_t SkewThreadCpuNs(bool valid, int cpu_clock) {
#if XE_PLATFORM_LINUX
  if (valid) {
    timespec ts;
    if (clock_gettime(clockid_t(cpu_clock), &ts) == 0) {
      return int64_t(ts.tv_sec) * 1000000000 + int64_t(ts.tv_nsec);
    }
  }
#endif
  return -1;
}

// hakuX-Boosted: guest_scheduler_skew_host_progress, the watchdog's part.
// Run at the top of each tick, before the new probes are raised and without
// lock_ (the readings are system calls): any probe still set here has been
// outstanding for a whole tick. The first reading of a probe is the base,
// each later one gives the share of the tick between them that the thread
// spent on a core.
void GuestScheduler::SkewSampleHostThreads() {
  const double ticks_per_us = ticks_per_us_ > 0.0 ? ticks_per_us_ : 1.0;
  for (int i = 0; i < kMaxCpus; ++i) {
    Cpu& cpu = cpus_[i];
    const uint64_t probe = cpu.skew_probe_tick.load(std::memory_order_relaxed);
    if (!probe || !cpu.skew_cpu_clock_valid.load(std::memory_order_relaxed)) {
      cpu.skew_sample_prev_ns = -1;
      continue;
    }
    const int64_t ns = SkewThreadCpuNs(true, cpu.skew_cpu_clock);
    const uint64_t tick = Clock::host_tick_count_raw();
    if (ns >= 0 && cpu.skew_sample_prev_ns >= 0 &&
        cpu.skew_sample_prev_probe == probe && tick > cpu.skew_sample_prev_tick &&
        ns >= cpu.skew_sample_prev_ns) {
      const double wall_ns =
          double(tick - cpu.skew_sample_prev_tick) / ticks_per_us * 1000.0;
      if (wall_ns > 0.0) {
        const double share =
            double(ns - cpu.skew_sample_prev_ns) * 1000.0 / wall_ns;
        cpu.skew_oncore_permille.store(
            share >= 1000.0 ? 1000u : uint32_t(share),
            std::memory_order_relaxed);
        cpu.skew_oncore_sample_tick.store(tick, std::memory_order_relaxed);
      }
    }
    cpu.skew_sample_prev_ns = ns;
    cpu.skew_sample_prev_tick = tick;
    cpu.skew_sample_prev_probe = probe;
  }
}

bool GuestScheduler::SkewDescribeThread(int cpu_index, uint32_t* tid,
                                        std::string* name) {
  TimedLock lock(this);
  if (!*tid) {
    XThread* running = cpus_[cpu_index].current_thread;
    if (!running) {
      return false;
    }
    *tid = running->thread_id();
    *name = running->thread_name();
    return true;
  }
  // A thread the scheduler has linked or running is alive under the lock.
  auto match = [&](XThread* t) {
    if (t && t->thread_id() == *tid) {
      *name = t->thread_name();
      return true;
    }
    return false;
  };
  for (Cpu& cpu : cpus_) {
    if (match(cpu.current_thread)) {
      return true;
    }
    for (int prio = 0; prio < 32; ++prio) {
      for (XThread* t = cpu.ready_head[prio]; t;
           t = t->scheduler_links().ready_next) {
        if (match(t)) {
          return true;
        }
      }
    }
    for (XThread* t = cpu.blocked_head; t;
         t = t->scheduler_links().ready_next) {
      if (match(t)) {
        return true;
      }
    }
    for (XThread* t = cpu.suspended_head; t;
         t = t->scheduler_links().ready_next) {
      if (match(t)) {
        return true;
      }
    }
  }
  return false;
}

bool GuestScheduler::SkewHold(XThread* self, int target, bool progress_gate,
                              uint64_t behind_since_tick) {
  auto& links = self->scheduler_links();
  const int me = CurrentCpu();
  Cpu& mine = cpus_[me];
  Cpu& theirs = cpus_[target];
  const uint8_t target_bit = uint8_t(1u << target);
  const bool has_debt = (links.skew_debt_mask & target_bit) != 0;
  const uint64_t debt_seq = links.skew_debt_seq[target];
  // Whom this waits for: the stalled CPU's running fiber, or the thread
  // this one readied.
  uint32_t target_tid = progress_gate ? 0 : links.skew_debt_tid[target];
  std::string target_name;
  bool target_named = false;
  if (progress_gate) {
    target_named = SkewDescribeThread(target, &target_tid, &target_name);
  }
  const uint64_t probe_at_start =
      theirs.skew_probe_tick.load(std::memory_order_relaxed);
  const int64_t on_core_before = SkewThreadCpuNs(
      theirs.skew_cpu_clock_valid.load(), theirs.skew_cpu_clock);
  const uint64_t start = Clock::host_tick_count_raw();
  const uint64_t start_us = NowMicros();
  const double ticks_per_us = ticks_per_us_ > 0.0 ? ticks_per_us_ : 1.0;

  // From here this CPU is on time for everyone else, and whoever waited
  // for it is told so.
  mine.skew_holding.store(true);
  skew_holders_.fetch_add(1);
  SkewNotify();

  bool capped = false;
  bool cut = false;
  {
    std::unique_lock<std::mutex> lock(skew_mutex_);
    for (;;) {
      const uint64_t now = Clock::host_tick_count_raw();
      if (now - start >= skew_cap_ticks_) {
        capped = true;
        break;
      }
      // Plain reads of flags others write under lock_; a stale one costs a
      // slice. A thread that outranks this one, a terminate, a suspend and
      // shutdown all end the hold: the caller then gives way as it would
      // have without it.
      if (links.preempted ||
          links.terminate_pending.load(std::memory_order_relaxed) ||
          shutting_down_.load(std::memory_order_relaxed) ||
          self->suspend_count() != 0) {
        cut = true;
        break;
      }
      bool behind;
      if (progress_gate) {
        behind = SkewCpuStalled(theirs, now);
      } else {
        // Until that CPU makes a scheduling decision; if a fiber runs there
        // without one, for as long as that fiber is not being run.
        behind = theirs.skew_serve_seq.load() == debt_seq &&
                 (!theirs.skew_running.load() || SkewCpuStalled(theirs, now));
      }
      if (!behind) {
        break;
      }
      const uint64_t left_us =
          uint64_t(double(skew_cap_ticks_ - (now - start)) / ticks_per_us) + 1;
      skew_cv_.wait_for(lock, std::chrono::microseconds(std::min<uint64_t>(
                                  left_us, kSkewHoldSliceUs)));
    }
  }

  const uint64_t end = Clock::host_tick_count_raw();
  const uint64_t held = end - start;
  mine.skew_hold_start_us.store(start_us, std::memory_order_relaxed);
  mine.skew_hold_end_us.store(NowMicros(), std::memory_order_relaxed);
  // Answered before the hold is lowered, so this CPU is never seen as
  // neither holding nor answering.
  mine.skew_probe_tick.store(0);
  mine.skew_holding.store(false);
  skew_holders_.fetch_sub(1);

  if (capped) {
    // Not again for the same lapse: the debt is dropped, and the probe that
    // stayed unanswered is marked so that nobody waits for it any more.
    if (has_debt) {
      links.skew_debt_mask &= uint8_t(~target_bit);
    }
    if (progress_gate && probe_at_start) {
      theirs.skew_capped_probe_tick.store(probe_at_start,
                                          std::memory_order_relaxed);
    }
  }
  // The held thread did not run: its slice is as long as it was.
  {
    TimedLock lock(this);
    if (links.quantum_deadline_tick) {
      links.quantum_deadline_tick += held;
      if (mine.current_thread == self) {
        mine.quantum_deadline_tick = links.quantum_deadline_tick;
      }
    }
  }

  (progress_gate ? stats_.skew_holds_progress : stats_.skew_holds_start)
      .fetch_add(1, std::memory_order_relaxed);
  stats_.skew_hold_ticks.fetch_add(held, std::memory_order_relaxed);
  skew_run_holds_.fetch_add(1, std::memory_order_relaxed);
  skew_run_hold_ticks_.fetch_add(held, std::memory_order_relaxed);
  if (capped) {
    stats_.skew_holds_capped.fetch_add(1, std::memory_order_relaxed);
    skew_run_capped_.fetch_add(1, std::memory_order_relaxed);
  }
  if (cut) {
    stats_.skew_holds_cut.fetch_add(1, std::memory_order_relaxed);
  }
  const int64_t on_core_after = SkewThreadCpuNs(
      theirs.skew_cpu_clock_valid.load(), theirs.skew_cpu_clock);
  int64_t on_core_ns = -1;
  if (on_core_before >= 0 && on_core_after >= on_core_before) {
    on_core_ns = on_core_after - on_core_before;
    stats_.skew_target_on_core_ns.fetch_add(uint64_t(on_core_ns),
                                            std::memory_order_relaxed);
  }
  uint64_t run_longest = skew_run_longest_ticks_.load(std::memory_order_relaxed);
  while (held > run_longest &&
         !skew_run_longest_ticks_.compare_exchange_weak(
             run_longest, held, std::memory_order_relaxed)) {
  }

  const uint64_t held_us = uint64_t(double(held) / ticks_per_us);
  const bool longest =
      held > skew_longest_ticks_.load(std::memory_order_relaxed);
  const bool log = capped || held_us >= kSkewHoldLogUs;
  if (longest || log) {
    if (!target_named) {
      target_named = SkewDescribeThread(target, &target_tid, &target_name);
    }
    if (!target_named) {
      target_name = "?";
    }
  }
  if (longest) {
    std::lock_guard<std::mutex> lock(skew_report_mutex_);
    if (held > skew_longest_.ticks) {
      skew_longest_.ticks = held;
      skew_longest_.holder_tid = self->thread_id();
      skew_longest_.holder_name = self->thread_name();
      skew_longest_.holder_cpu = me;
      skew_longest_.target_tid = target_tid;
      skew_longest_.target_name = target_name;
      skew_longest_.target_cpu = target;
      skew_longest_.progress_gate = progress_gate;
      skew_longest_.capped = capped;
      skew_longest_ticks_.store(held, std::memory_order_relaxed);
    }
  }
  if (log) {
    static std::atomic<int> budget{400};
    if (budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      const uint64_t behind_us =
          end > behind_since_tick
              ? uint64_t(double(end - behind_since_tick) / ticks_per_us)
              : held_us;
      XELOGW(
          "GuestScheduler: skew hold, tid={:08X} '{}' on cpu {} waited {:.1f} "
          "ms for cpu {} ({}: tid={:08X} '{}'; that CPU was {:.1f} ms behind "
          "in all, its host thread on a core for {:.1f} ms of the hold){} "
          "(frame {})",
          self->thread_id(), self->thread_name(), me,
          double(held_us) / 1000.0, target,
          progress_gate ? "progress gate, its running thread showed no "
                          "progress"
                        : "start gate, it had not dispatched the thread this "
                          "one readied",
          target_tid, target_name, double(behind_us) / 1000.0,
          on_core_ns >= 0 ? double(on_core_ns) / 1e6 : -1.0,
          capped ? ", ended by the cap" : (cut ? ", cut short" : ""),
          xe::logging::GetFrameNumber());
    }
  }
  return !cut;
}

void GuestScheduler::SpinYield(std::chrono::milliseconds host_sleep) {
  XThread* self = XThread::GetCurrentFiberThread();
  if (self) {
    // The holder we spin on may be a fiber queued behind us on this same
    // dispatch thread, so yielding the host thread would never let it run.
    auto* scheduler = self->kernel_state()->guest_scheduler();
    if (host_sleep.count()) {
      // Parking rather than re-queueing, so a lone fiber idles instead of
      // spinning its dispatch thread at full speed.
      scheduler->BlockCurrentThread();
    } else {
      scheduler->YieldCurrentThread(false, true, YieldCause::kSpinSite);
    }
    return;
  }
  if (host_sleep.count()) {
    xe::threading::Sleep(host_sleep);
  } else {
    xe::threading::MaybeYield();
  }
}

void GuestScheduler::EnsureIoWorker() {
  std::call_once(io_once_, [this]() {
    io_event_ = xe::threading::Event::CreateAutoResetEvent(false);
    xe::threading::Thread::CreationParameters params;
    io_thread_ =
        xe::threading::Thread::Create(params, [this]() { IoWorkerLoop(); });
    io_thread_->set_name("Guest I/O");
    io_started_.store(true);
  });
}

bool GuestScheduler::CurrentThreadOffloadsBlockingCalls() {
  if (!enabled() || !XThread::GetCurrentFiberThread()) {
    return false;
  }
  // The offloaded call can need the global critical region itself, and only
  // this fiber can release it, so holding it means running inline.
  return !xe::global_critical_region::is_held_by_current_thread();
}

void GuestScheduler::WaitOnFence(xe::threading::Fence& fence) {
  XThread* self = enabled() ? XThread::GetCurrentFiberThread() : nullptr;
  if (!self) {
    fence.Wait();
    return;
  }
  auto* scheduler = self->kernel_state()->guest_scheduler();
  self->set_cooperative_wait_shape(XThread::CooperativeWaitKind::kFence,
                                   nullptr, 0);
  while (!fence.TryWait()) {
    // The signaler touches the fence on this stack, terminate must not free
    // it.
    scheduler->BlockCurrentThread(0, 0, false, false);
  }
  self->clear_cooperative_wait_shape();
}

void GuestScheduler::RunBlockingHostCallOffloaded(
    const std::function<void()>& fn) {
  EnsureIoWorker();
  BlockingCall call;
  call.fn = &fn;
  call.queued_ns = Clock::host_tick_count_raw();
  {
    std::lock_guard<std::mutex> lock(io_lock_);
    io_queue_.push(&call);
  }
  io_event_->Set();
  XThread* self = XThread::GetCurrentFiberThread();
  if (self) {
    self->set_cooperative_wait_shape(XThread::CooperativeWaitKind::kIoOffload,
                                     nullptr, 0);
  }
  while (!call.done.load(std::memory_order_acquire)) {
    // The worker writes |call| on this stack, terminate must not free it.
    BlockCurrentThread(0, 0, false, false);
  }
  if (self) {
    self->clear_cooperative_wait_shape();
  }
}

void GuestScheduler::IoWorkerLoop() {
  while (!shutting_down_.load()) {
    BlockingCall* call = nullptr;
    {
      std::lock_guard<std::mutex> lock(io_lock_);
      if (!io_queue_.empty()) {
        call = io_queue_.front();
        io_queue_.pop();
      }
    }
    if (!call) {
      xe::threading::Wait(io_event_.get(), false);
      continue;
    }
    uint64_t started = Clock::host_tick_count_raw();
    (*call->fn)();
    uint64_t finished = Clock::host_tick_count_raw();
    // Raw ticks, converted only at report time.
    uint64_t queued_for = started - call->queued_ns;
    stats_.io_calls.fetch_add(1, std::memory_order_relaxed);
    stats_.io_queue_ns.fetch_add(queued_for, std::memory_order_relaxed);
    stats_.io_run_ns.fetch_add(finished - started, std::memory_order_relaxed);
    uint64_t prev_max = stats_.io_queue_max_ns.load(std::memory_order_relaxed);
    while (queued_for > prev_max &&
           !stats_.io_queue_max_ns.compare_exchange_weak(
               prev_max, queued_for, std::memory_order_relaxed)) {
    }
    call->done.store(true, std::memory_order_release);
    // Wake the parked caller instead of leaving it to the backoff timer.
    WakeAll();
  }
}

void GuestScheduler::WakeAll() {
  if (!started_.load()) {
    return;
  }
  // hakuX-Boosted: guest_scheduler_park_waits (an I/O completion, an APC).
  NoteWaitEvent();
  // Skip the lock when no CPU has a blocked waiter. A stale hint costs at
  // most one backoff interval.
  bool any_blocked = false;
  for (int i = 0; i < kMaxCpus; ++i) {
    if (cpus_[i].has_blocked.load()) {
      any_blocked = true;
      break;
    }
  }
  if (!any_blocked) {
    return;
  }
  // Ask each CPU with a blocked waiter to re-poll, preempting its runner only
  // when a waiter outranks it. An equal-priority preempt would head-requeue
  // the runner past ready threads on every signal and starve them.
  {
    TimedLock lock(this);
    for (int i = 0; i < kMaxCpus; ++i) {
      Cpu& cpu = cpus_[i];
      if (!cpu.blocked_head) {
        continue;
      }
      cpu.repoll_now.store(true, std::memory_order_relaxed);
      XThread* running = cpu.current_thread;
      if (running &&
          cpu.max_blocked_prio > ClampPriority(running->priority())) {
        running->scheduler_links().preempted = true;
        running->thread_state()->context()->preempt_requested = 1;
      }
    }
  }
  for (int i = 0; i < kMaxCpus; ++i) {
    if (cpus_[i].has_blocked.load(std::memory_order_relaxed) &&
        cpus_[i].parked.load() && cpus_[i].ready_event) {
      WakeDispatchThread(cpus_[i]);
    }
  }
}

void GuestScheduler::WakeForSignal(const XObject* object,
                                   XThread* sole_waiter) {
  if (!started_.load()) {
    return;
  }
  // hakuX-Boosted: guest_scheduler_park_waits. A signalled event, a released
  // semaphore or mutant, a handed-over critical section: before the test
  // below, because a busy-waiting thread is on no blocked list.
  NoteWaitEvent();
  bool any_blocked = false;
  for (int i = 0; i < kMaxCpus; ++i) {
    if (cpus_[i].has_blocked.load()) {
      any_blocked = true;
      break;
    }
  }
  if (!any_blocked) {
    return;
  }
  // hakuX-Boosted: guest_scheduler_skew_bound_us. A guest thread whose
  // signal readies a waiter parked on another guest CPU is in that CPU's
  // debt until it acts on the signal (the start gate, SkewEvaluateAndHold).
  // A signal from a host thread (audio, GPU, timers) has nobody to hold.
  XThread* skew_debtor = nullptr;
  int skew_debtor_cpu = -1;
  uint64_t skew_now = 0;
  if (skew_bound_ticks_) {
    skew_debtor_cpu = CurrentCpu();
    if (skew_debtor_cpu >= 0) {
      skew_debtor = XThread::GetCurrentFiberThread();
      if (skew_debtor) {
        skew_now = Clock::host_tick_count_raw();
      }
    }
  }
  bool wake[kMaxCpus] = {};
  {
    TimedLock lock(this);
    if (sole_waiter) {
      // Only the permit FIFO's front can take the permit; every other waiter
      // of this object would wake, poll, fail MayAcquire and re-park.
      auto& links = sole_waiter->scheduler_links();
      if (!links.blocked || links.cpu < 0) {
        // Not parked yet: it re-checks the (already bumped) signal epoch on
        // its own dispatch pass before it parks, so there is nothing to wake.
        return;
      }
      Cpu& cpu = cpus_[links.cpu];
      cpu.repoll_now.store(true, std::memory_order_relaxed);
      XThread* running = cpu.current_thread;
      if (running && ClampPriority(sole_waiter->priority()) >
                         ClampPriority(running->priority())) {
        running->scheduler_links().preempted = true;
        running->thread_state()->context()->preempt_requested = 1;
      }
      wake[links.cpu] = true;
      if (skew_debtor) {
        SkewAddDebtLocked(skew_debtor, skew_debtor_cpu, links.cpu,
                          sole_waiter->thread_id(), skew_now);
      }
    } else {
      // Wake the CPUs hosting a waiter whose wait includes this object. The
      // walk is bounded by the blocked population, which the empty-yield fast
      // path keeps small.
      for (int i = 0; i < kMaxCpus; ++i) {
        Cpu& cpu = cpus_[i];
        if (!cpu.blocked_head) {
          continue;
        }
        bool any_watcher = false;
        int watcher_prio = 0;
        uint32_t watcher_tid = 0;  // hakuX-Boosted: for the skew bound
        for (XThread* t = cpu.blocked_head; t;
             t = t->scheduler_links().ready_next) {
          auto& links = t->scheduler_links();
          bool watches = t->cooperative_wait_object() == object;
          if (!watches) {
            for (uint8_t j = 0; j < links.wait_gate_count; ++j) {
              if (links.wait_gate_objects[j] == object) {
                watches = true;
                break;
              }
            }
          }
          if (watches) {
            int prio = ClampPriority(t->priority());
            if (!any_watcher || prio > watcher_prio) {
              watcher_tid = t->thread_id();
            }
            watcher_prio = any_watcher ? std::max(watcher_prio, prio) : prio;
            any_watcher = true;
          }
        }
        if (!any_watcher) {
          continue;
        }
        cpu.repoll_now.store(true, std::memory_order_relaxed);
        XThread* running = cpu.current_thread;
        if (running && watcher_prio > ClampPriority(running->priority())) {
          running->scheduler_links().preempted = true;
          running->thread_state()->context()->preempt_requested = 1;
        }
        wake[i] = true;
        if (skew_debtor) {
          SkewAddDebtLocked(skew_debtor, skew_debtor_cpu, i, watcher_tid,
                            skew_now);
        }
      }
    }
  }
  for (int i = 0; i < kMaxCpus; ++i) {
    if (wake[i] && cpus_[i].parked.load() && cpus_[i].ready_event) {
      WakeDispatchThread(cpus_[i]);
    }
  }
}

bool GuestScheduler::apc_poke_enabled() {
  return cvars::guest_scheduler_apc_poke;
}

bool GuestScheduler::WakeForApc(XThread* thread) {
  if (!cvars::guest_scheduler_apc_poke) {
    return false;
  }
  if (!thread || !started_.load()) {
    return true;
  }
  NoteWaitEvent();  // hakuX-Boosted: guest_scheduler_park_waits
  int wake_cpu = -1;
  {
    TimedLock lock(this);
    auto& links = thread->scheduler_links();
    // A thread that is running or queued looks at its APC queue before it
    // parks (CooperativeWait, XThread::Delay) and again under this lock as
    // it parks (BlockCurrentThread), so only a parked one needs the poke.
    if (links.blocked && links.cpu >= 0) {
      Cpu& cpu = cpus_[links.cpu];
      cpu.repoll_now.store(true, std::memory_order_relaxed);
      XThread* running = cpu.current_thread;
      if (running && ClampPriority(thread->priority()) >
                         ClampPriority(running->priority())) {
        running->scheduler_links().preempted = true;
        running->thread_state()->context()->preempt_requested = 1;
      }
      wake_cpu = links.cpu;
    }
  }
  stats_.apc_pokes.fetch_add(1, std::memory_order_relaxed);
  if (wake_cpu >= 0 && cpus_[wake_cpu].parked.load() &&
      cpus_[wake_cpu].ready_event) {
    WakeDispatchThread(cpus_[wake_cpu]);
  }
  return true;
}

void GuestScheduler::NotifyThreadExited(XThread* thread) {
  if (!OnDispatchThread("NotifyThreadExited")) {
    return;
  }
  XELOGI("GuestScheduler: exited tid={:08X} '{}'", thread->thread_id(),
         thread->thread_name());
  // This CPU's dispatch loop reclaims it, since we can't drop the last handle
  // while running on its fiber.
  cpus_[CurrentCpu()].exited_thread = thread;
  NoteWaitEvent();  // hakuX-Boosted: guest_scheduler_park_waits
}

void GuestScheduler::BlockCurrentThread(uint64_t deadline_ms,
                                        uint32_t wait_epoch, bool alertable,
                                        bool interruptible) {
  if (!OnDispatchThread("BlockCurrentThread")) {
    return;
  }
  if (interruptible) {
    ExitIfTerminated();
  }
  XThread* self = XThread::GetCurrentThread();
  int cpu_index = CurrentCpu();
  // hakuX-Boosted: a thread that waits is not spinning. The parks themselves
  // come through here and put their own state back afterwards.
  cpus_[cpu_index].poll_watch_thread = nullptr;
  cpus_[cpu_index].streak_thread = nullptr;
  stats_.blocks.fetch_add(1, std::memory_order_relaxed);
  // hakuX-Boosted: guest_scheduler_park_waits. A wait of the thread's own
  // ends its burst of busy waiting; and a thread that stops running has
  // usually just finished what a busy-waiting thread is waiting for (a
  // worker going back to sleep), so it counts as an event for those. A
  // spinner's own park is neither.
  if (park_waits_ && !self->scheduler_links().wait_park) {
    auto& own = self->scheduler_links();
    own.spin_check_tick = 0;
    own.wait_armed = false;
    own.poll_site = 0;
    NoteWaitEvent();
  }
  // hakuX-Boosted: with the poke (WakeForApc) an alertable waiter is told of
  // its APC, so it needs no timed look of its own.
  const bool apc_poke = cvars::guest_scheduler_apc_poke;
  // Gate only types whose every satisfying transition calls
  // WakeCooperativeWaiters, anything else polls every pass.
  XObject* wait_object = self->cooperative_wait_object();
  bool gated = false;
  if (wait_object) {
    switch (wait_object->type()) {
      case XObject::Type::Event:
      case XObject::Type::Semaphore:
      case XObject::Type::Mutant:
        gated = true;
        break;
      default:
        break;
    }
  } else if (self->cooperative_wait_set_count()) {
    // Multi-wait: gated on the summed epoch of its whole set. Without this it
    // re-readied on every pass, which is most of the scheduler's churn in
    // titles that park worker pools on WaitForMultipleObjects.
    gated = true;
  } else if (deadline_ms && (!alertable || apc_poke) &&
             static_cast<XThread::CooperativeWaitKind>(
                 self->scheduler_links().wait_kind) ==
                 XThread::CooperativeWaitKind::kDelay) {
    // Pure timed sleep: only the clock can end it, so park until the deadline
    // instead of waking every kPollBackoffMs. Keyed to the delay wait kind -
    // a timed wait whose objects could not be tracked (a 9+ object
    // WaitMultiple) also arrives here objectless with a deadline, and gating
    // that would leave its signals unseen until the deadline or backstop.
    gated = true;
  }
  {
    TimedLock lock(this);
    // hakuX-Boosted: raised BEFORE the epoch is read below, not after the
    // waiter is linked. A signaller bumps the epoch and then reads this flag
    // without the lock (WakeForSignal's first test); the waiter read the
    // epoch and then wrote the flag, so both could miss each other:
    //   waiter reads the epoch (old) -> signaller bumps it -> signaller
    //   reads has_blocked on all six CPUs (all false) and returns ->
    //   waiter stores has_blocked and parks, signalled, until the backstop.
    // Write-then-read on both sides closes it. It needed every CPU's
    // blocked list to be empty at that moment, so it was rare. RereadyBlocked
    // rewrites the flag from the list under this same lock.
    cpus_[cpu_index].has_blocked.store(true);
    // A signal between the caller's failed poll and this park bumped the epoch
    // but walked the blocked list before we joined it, so nothing would re-poll
    // us before the backstop. Compare under the walk's own lock and poke this
    // CPU on a mismatch; returning to re-poll instead livelocks when the epoch
    // churns faster than a park roundtrip.
    if (gated) {
      uint32_t epoch_now = 0;
      bool epoch_gated = false;
      if (wait_object) {
        epoch_now = wait_object->cooperative_signal_epoch();
        epoch_gated = true;
      } else if (self->cooperative_wait_set_count()) {
        epoch_now = self->cooperative_wait_set_epoch();
        epoch_gated = true;
      }
      if (epoch_gated && epoch_now != wait_epoch) {
        cpus_[cpu_index].repoll_now.store(true, std::memory_order_relaxed);
        stats_.park_catches.fetch_add(1, std::memory_order_relaxed);
      }
    }
    // hakuX-Boosted: the same for an APC queued between the caller's look at
    // its queue and this park. WakeForApc takes this lock after the APC is
    // in the queue: either it finds this thread parked, or the queue is
    // already non-empty here.
    if (apc_poke && alertable && self->HasPendingUserApc()) {
      cpus_[cpu_index].repoll_now.store(true, std::memory_order_relaxed);
    }
    auto& links = self->scheduler_links();
    // Park self (running, in no list) on this CPU's blocked list.
    links.blocked = true;
    links.preempted = false;
    links.cpu = cpu_index;
    links.ready_next = nullptr;
    links.wait_gated = gated;
    links.wait_alertable = alertable;
    links.wait_epoch = wait_epoch;
    links.wait_deadline_ms = deadline_ms;
    // A wait consumes the slice.
    links.quantum_deadline_tick = 0;
    Cpu& cpu = cpus_[cpu_index];
    LinkTailLocked(cpu.blocked_head, cpu.blocked_tail, self);
    int prio = ClampPriority(self->priority());
    if (prio > cpu.max_blocked_prio) {
      cpu.max_blocked_prio = prio;
    }
    // Timed need of this waiter: the poll cadence for ungated and alertable
    // waits, a gated deadline, nothing for a quiet gated wait.
    uint64_t due = gated ? deadline_ms : 0;
    if (!gated || (alertable && !apc_poke)) {
      uint64_t cadence = Clock::QueryHostUptimeMillis() + kPollBackoffMs;
      if (!due || cadence < due) {
        due = cadence;
      }
    }
    if (due && due < cpu.next_timed_repoll_ms) {
      cpu.next_timed_repoll_ms = due;
    }
    // hakuX-Boosted: guest_scheduler_park_waits. Counted as parked first,
    // then the event sequence is read (see WaitPark's comment): an event
    // since the thread's last look shortens the park to its minimum instead
    // of being slept through.
    if (links.wait_park) {
      wait_parked_total_.fetch_add(1);
      cpu.wait_parked.store(
          cpu.wait_parked.load(std::memory_order_relaxed) + 1,
          std::memory_order_relaxed);
      cpu.wait_wake_noted.store(false, std::memory_order_relaxed);
      if (wait_wake_seq_.load() != links.wait_arm_seq) {
        links.wait_arm_seq = wait_wake_seq_.load();
        links.wait_deadline_tick = links.wait_min_tick;
      }
      if (!cpu.next_fine_repoll_tick ||
          links.wait_deadline_tick < cpu.next_fine_repoll_tick) {
        cpu.next_fine_repoll_tick = links.wait_deadline_tick;
      }
    }
    // has_blocked was raised at the top of this block (seq_cst, pairing
    // with the wake pre-filters' loads).
  }
  self->guest_object<X_KTHREAD>()->thread_state = KTHREAD_STATE_WAITING;
  YieldToScheduler();
  // Terminated while parked, TerminateThread re-readied us to exit here.
  if (interruptible) {
    ExitIfTerminated();
  }
}

void GuestScheduler::RereadyBlocked(int cpu_index, bool poked) {
  uint32_t wake_mask = 0;
  stats_.repolls.fetch_add(1, std::memory_order_relaxed);
  // Missed-wake tripwire, logged after the lock is released.
  // hakuX-Boosted: who was rescued, for the log.
  static constexpr size_t kRescueNotes = 4;
  struct RescueNote {
    uint32_t tid;
    uint32_t handle;
    uint64_t signal_us;
    bool forced;
  };
  RescueNote rescue_notes[kRescueNotes] = {};
  size_t rescue_note_count = 0;
  const uint64_t rescue_now_us = NowMicros();
  uint64_t rescued = 0;
  uint64_t rescued_by_look = 0;
  bool warn = false;
  uint64_t warn_count = 0;
  uint32_t warn_tid = 0;
  uint32_t warn_handle = 0;
  // hakuX-Boosted: both read once a pass.
  const bool apc_poke = cvars::guest_scheduler_apc_poke;
  const uint64_t gate_recheck_ms = cvars::guest_scheduler_gate_recheck_ms;
  {
    TimedLock lock(this);
    Cpu& cpu = cpus_[cpu_index];
    uint64_t now_ms = Clock::QueryHostUptimeMillis();
    bool force_all = now_ms >= cpu.next_force_repoll_ms;
    if (force_all) {
      cpu.next_force_repoll_ms = now_ms + kRepollBackstopMs;
    }
    // Earliest timed need among the waiters kept parked, the backstop bounds
    // it. Waiters re-readied below re-park through BlockCurrentThread, which
    // lowers it again before this CPU can sleep.
    uint64_t next_due = cpu.next_force_repoll_ms;
    XThread* kept_head = nullptr;
    XThread* kept_tail = nullptr;
    int kept_max_prio = -1;
    // hakuX-Boosted: guest_scheduler_park_waits. The parked spinners here:
    // the clock is only read when there are any.
    const bool have_parks =
        cpu.wait_parked.load(std::memory_order_relaxed) != 0;
    const uint64_t now_tick = have_parks ? Clock::host_tick_count_raw() : 0;
    const bool wake_parks = cpu.wake_wait_parks;
    cpu.wake_wait_parks = false;
    uint32_t kept_parks = 0;
    uint64_t fine_next = 0;
    XThread* t = cpu.blocked_head;
    while (t) {
      auto& links = t->scheduler_links();
      XThread* next = links.ready_next;
      // hakuX-Boosted: a parked spinner stays until its deadline, or, when
      // an event asked for it, until its shortest park is over.
      bool park_released = false;
      if (links.wait_park) {
        uint64_t due_tick = links.wait_deadline_tick;
        if (wake_parks && links.wait_min_tick < due_tick) {
          due_tick = links.wait_min_tick;
        }
        if (!force_all && now_tick < due_tick &&
            !(links.wait_deadline_ms && now_ms >= links.wait_deadline_ms)) {
          links.wait_deadline_tick = due_tick;
          links.ready_next = nullptr;
          LinkTailLocked(kept_head, kept_tail, t);
          int prio = ClampPriority(t->priority());
          if (prio > kept_max_prio) {
            kept_max_prio = prio;
          }
          if (links.wait_deadline_ms && links.wait_deadline_ms < next_due) {
            next_due = links.wait_deadline_ms;
          }
          if (!fine_next || due_tick < fine_next) {
            fine_next = due_tick;
          }
          ++kept_parks;
          t = next;
          continue;
        }
        (due_tick < links.wait_deadline_tick || wake_parks
             ? stats_.wait_ended_event
             : stats_.wait_ended_timer)
            .fetch_add(1, std::memory_order_relaxed);
        if (now_tick > links.wait_park_start_tick) {
          stats_.wait_park_ticks.fetch_add(
              now_tick - links.wait_park_start_tick,
              std::memory_order_relaxed);
        }
        links.wait_park = false;
        wait_parked_total_.fetch_sub(1);
        park_released = true;
      }
      // Skip a gated waiter whose wait cannot have resolved yet.
      if (links.wait_gated && !force_all && !park_released) {
        // Three gate kinds: a single object's epoch, a multi-wait's summed
        // epoch, or (neither) a pure timed sleep that only the deadline below
        // can resolve.
        XObject* obj = t->cooperative_wait_object();
        bool may_have_resolved = false;
        if (obj) {
          may_have_resolved =
              obj->cooperative_signal_epoch() != links.wait_epoch;
        } else if (links.wait_gate_count) {
          may_have_resolved = t->cooperative_wait_set_epoch() != links.wait_epoch;
        }
        if (!may_have_resolved &&
            !(links.wait_deadline_ms && now_ms >= links.wait_deadline_ms) &&
            !(links.wait_alertable && t->HasPendingUserApc())) {
          links.ready_next = nullptr;
          LinkTailLocked(kept_head, kept_tail, t);
          int prio = ClampPriority(t->priority());
          if (prio > kept_max_prio) {
            kept_max_prio = prio;
          }
          if (links.wait_deadline_ms && links.wait_deadline_ms < next_due) {
            next_due = links.wait_deadline_ms;
          }
          if (links.wait_alertable && !apc_poke &&
              now_ms + kPollBackoffMs < next_due) {
            // APCs inserted without a WakeAll are only found by polling.
            next_due = now_ms + kPollBackoffMs;
          }
          // hakuX-Boosted: see kGateRecheckMs.
          if (gate_recheck_ms && (obj || links.wait_gate_count) &&
              now_ms + gate_recheck_ms < next_due) {
            next_due = now_ms + gate_recheck_ms;
          }
          t = next;
          continue;
        }
      }
      // Only the backstop reached this waiter, on a pass no wake poked, yet
      // its gate's epoch moved: the signal's own wake never re-readied it.
      // hakuX-Boosted: counted on any pass no wake poked (the timed look of
      // kGateRecheckMs as well as the backstop), and the first few of a pass
      // are named in the log with how old the signal was.
      if (!poked && links.wait_gated) {
        XObject* obj = t->cooperative_wait_object();
        bool epoch_moved = false;
        uint64_t signal_us = 0;
        if (obj) {
          epoch_moved = obj->cooperative_signal_epoch() != links.wait_epoch;
          signal_us = obj->cooperative_signal_time_us();
        } else if (links.wait_gate_count) {
          epoch_moved =
              t->cooperative_wait_set_epoch() != links.wait_epoch;
          signal_us = t->cooperative_wait_set_signal_time_us();
        }
        // A signal under half a millisecond old is one whose own poke is
        // still on its way (the signaller takes this lock next), not a miss.
        const bool poke_in_flight =
            signal_us && rescue_now_us < signal_us + 500;
        if (epoch_moved && !poke_in_flight &&
            !(links.wait_deadline_ms && now_ms >= links.wait_deadline_ms)) {
          ++rescued;
          if (!force_all) {
            ++rescued_by_look;
          }
          backstop_rescue_tid_ = t->thread_id();
          backstop_rescue_handle_ =
              links.wait_handle_count ? links.wait_handles[0] : 0;
          if (rescue_note_count < kRescueNotes) {
            rescue_notes[rescue_note_count].tid = t->thread_id();
            rescue_notes[rescue_note_count].handle = backstop_rescue_handle_;
            rescue_notes[rescue_note_count].signal_us = signal_us;
            rescue_notes[rescue_note_count].forced = force_all;
            ++rescue_note_count;
          }
        }
      }
      links.blocked = false;
      links.queued = true;
      stats_.rereadied.fetch_add(1, std::memory_order_relaxed);
      // Its current guest CPU, not the one it blocked on, since
      // KeSetAffinityThread may have moved it while blocked.
      int target = CpuOf(t);
      links.cpu = target;
      bool at_head = links.preempted;
      links.preempted = false;
      LinkReadyLocked(cpus_[target], t, at_head);
      if (target != cpu_index) {
        wake_mask |= uint32_t(1) << target;
      }
      t = next;
    }
    cpu.blocked_head = kept_head;
    cpu.blocked_tail = kept_tail;
    cpu.max_blocked_prio = kept_max_prio;
    cpu.next_timed_repoll_ms = next_due;
    cpu.has_blocked.store(kept_head != nullptr, std::memory_order_relaxed);
    // hakuX-Boosted: guest_scheduler_park_waits.
    if (have_parks || kept_parks) {
      cpu.wait_parked.store(kept_parks, std::memory_order_relaxed);
      cpu.next_fine_repoll_tick = fine_next;
      if (!kept_parks) {
        cpu.wait_wake_noted.store(false, std::memory_order_relaxed);
      }
    }
    if (rescued) {
      stats_.backstop_rescues.fetch_add(rescued - rescued_by_look,
                                        std::memory_order_relaxed);
      stats_.timed_look_rescues.fetch_add(rescued_by_look,
                                          std::memory_order_relaxed);
      backstop_rescue_count_ += rescued;
    }
    if (!backstop_rescue_window_ms_) {
      backstop_rescue_window_ms_ = now_ms;
    }
    if (now_ms - backstop_rescue_window_ms_ >= 5000) {
      if (backstop_rescue_count_ >= kBackstopRescueWarnThreshold) {
        warn = true;
        warn_count = backstop_rescue_count_;
        warn_tid = backstop_rescue_tid_;
        warn_handle = backstop_rescue_handle_;
      }
      backstop_rescue_window_ms_ = now_ms;
      backstop_rescue_count_ = 0;
    }
  }
  if (warn) {
    XELOGW(
        "GuestScheduler: {} gated waits were only re-readied by the {}ms "
        "backstop in the last 5s despite a signal having arrived - wakes are "
        "being lost (last: tid={:08X} handle={:08X})",
        warn_count, kRepollBackstopMs, warn_tid, warn_handle);
  }
  if (rescue_note_count) {
    static std::atomic<int> budget{400};
    const uint64_t now_us = NowMicros();
    for (size_t i = 0; i < rescue_note_count; ++i) {
      if (budget.fetch_sub(1, std::memory_order_relaxed) <= 0) {
        break;
      }
      const RescueNote& note = rescue_notes[i];
      XELOGW(
          "GuestScheduler: missed wake made good by the {}: tid={:08X} "
          "waiting on handle {:08X}, signalled {:.1f} ms earlier (cpu {}, "
          "frame {})",
          note.forced ? "64 ms backstop" : "timed look", note.tid,
          note.handle,
          note.signal_us && now_us > note.signal_us
              ? double(now_us - note.signal_us) / 1000.0
              : -1.0,
          cpu_index, xe::logging::GetFrameNumber());
    }
  }
  // Wake any other dispatch thread that received a ready fiber (this one runs).
  for (int i = 0; i < kMaxCpus; ++i) {
    if ((wake_mask & (uint32_t(1) << i)) && cpus_[i].parked.load() &&
        cpus_[i].ready_event) {
      WakeDispatchThread(cpus_[i]);
    }
  }
}

void GuestScheduler::RunLoop(int cpu_index) {
  SetCurrentCpu(cpu_index);
  Cpu& cpu = cpus_[cpu_index];
  // Adopt this host thread's stack as this CPU's idle fiber.
  cpu.idle_fiber = xe::threading::Fiber::CreateFromThread();
  XELOGI("GuestScheduler: CPU {} dispatch loop started", cpu_index);
  // hakuX-Boosted: this thread's CPU-time clock, read by a fiber that holds
  // for this CPU (guest_scheduler_skew_bound_us).
#if XE_PLATFORM_LINUX
  if (skew_bound_ticks_) {
    clockid_t cpu_clock;
    if (pthread_getcpuclockid(pthread_self(), &cpu_clock) == 0) {
      cpu.skew_cpu_clock = int(cpu_clock);
      cpu.skew_cpu_clock_valid.store(true);
    }
  }
#endif

  while (!shutting_down_.load()) {
    if (cpu_index == 0) {
      ReportStatsIfDue();
    }
    // Re-poll blocked waiters on a timer even while other fibers run, or a
    // busy fiber that rarely waits would starve them. The timer runs at what
    // the parked waiters actually need, a wake skips it entirely.
    uint64_t now = Clock::QueryHostUptimeMillis();
    bool poked = cpu.repoll_now.exchange(false, std::memory_order_relaxed);
    // hakuX-Boosted: a parked spinner's deadline (0 with
    // guest_scheduler_park_waits off, and then nothing more is read).
    const bool fine_due =
        cpu.next_fine_repoll_tick &&
        Clock::host_tick_count_raw() >= cpu.next_fine_repoll_tick;
    if (poked || fine_due || now >= cpu.next_timed_repoll_ms) {
      RereadyBlocked(cpu_index, poked);
    }

    XThread* next = DequeueReady(cpu_index);
    if (next) {
      // Honor an affinity change that landed while it was queued or running
      // here. Safe now, an off-CPU thread's context is saved.
      int home = CpuOf(next);
      auto& links = next->scheduler_links();
      if (home != cpu_index &&
          !links.terminate_pending.load(std::memory_order_relaxed)) {
        {
          TimedLock lock(this);
          links.running = false;
          links.queued = true;
          links.cpu = home;
          bool at_head = links.preempted;
          links.preempted = false;
          LinkReadyLocked(cpus_[home], next, at_head);
        }
        if (cpus_[home].parked.load() && cpus_[home].ready_event) {
          WakeDispatchThread(cpus_[home]);
        }
        XELOGD("GuestScheduler: migrated tid={:08X} to CPU {}",
               next->thread_id(), home);
        continue;
      }
      // A suspend landing while the thread ran or was queued takes effect here.
      if (next->suspend_count() > 0 && ParkSuspended(next, cpu_index)) {
        continue;
      }
      cpu.exited_thread = nullptr;
      SwitchTo(next);
      if (cpu.exited_thread) {
        // On the idle fiber with the exited fiber parked on its final yield, so
        // reclaiming never frees a stack still in use.
        XThread* dead = cpu.exited_thread;
        cpu.exited_thread = nullptr;
        dead->ReclaimExited();
      }
      continue;
    }

    // Nothing ready, so sleep until the next re-poll if waiters are blocked (a
    // MarkReady wakes us sooner), otherwise idle until something is runnable.
    // Park before re-checking the queues, so a wake that saw parked still
    // false is caught here instead of slept through.
    // hakuX-Boosted: which sleep this will be is stored first
    // (WakeDispatchThread). Only this thread and the fibers it runs write
    // next_fine_repoll_tick, so it cannot change before the sleep below.
    const uint64_t fine_deadline = cpu.next_fine_repoll_tick;
    if (fine_deadline) {
      {
        std::lock_guard<std::mutex> fine_lock(cpu.fine_mutex);
        cpu.fine_signal = false;
      }
      cpu.fine_parked.store(true);
    }
    cpu.parked.store(true);
    bool have_blocked;
    bool have_work;
    {
      TimedLock lock(this);
      have_blocked = cpu.blocked_head != nullptr;
      have_work = cpu.ready_summary.load(std::memory_order_relaxed) != 0 ||
                  cpu.repoll_now.load(std::memory_order_relaxed);
      // hakuX-Boosted: guest_scheduler_skew_bound_us. Nothing ready and no
      // poke pending, seen under the lock every signaller pokes under: this
      // CPU has looked at everything sent to it so far and has nothing to
      // run. That too is a scheduling decision (a readied thread can have
      // gone to another CPU, or have been terminated), and a signaller that
      // waited for a dispatch here would otherwise wait out its cap.
      if (!have_work && skew_bound_ticks_) {
        cpu.skew_serve_seq.store(
            cpu.skew_serve_seq.load(std::memory_order_relaxed) + 1);
      }
    }
    if (have_work) {
      cpu.parked.store(false);
      cpu.fine_parked.store(false);
      continue;
    }
    if (skew_bound_ticks_ && skew_holders_.load()) {
      SkewNotify();
    }
    // hakuX-Boosted: guest_scheduler_park_waits. A parked spinner's deadline
    // is under a millisecond away, which the ready event cannot time: sleep
    // on the condition variable until it, the millisecond deadline, or a
    // wake, whichever is first.
    if (fine_deadline) {
      const uint64_t tick_now = Clock::host_tick_count_raw();
      uint64_t sleep_us = 0;
      if (fine_deadline > tick_now && ticks_per_us_ > 0.0) {
        sleep_us = uint64_t(double(fine_deadline - tick_now) / ticks_per_us_);
      }
      const uint64_t ms_now = Clock::QueryHostUptimeMillis();
      const uint64_t ms_due = cpu.next_timed_repoll_ms;
      const uint64_t ms_left = ms_due > ms_now ? ms_due - ms_now : 0;
      sleep_us = std::min<uint64_t>(sleep_us, ms_left * 1000);
      if (sleep_us) {
        FineSleep(cpu, sleep_us);
      }
      cpu.parked.store(false);
      cpu.fine_parked.store(false);
      stats_.idle_wakes.fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    if (!have_blocked) {
      if (dispatched_any_.load()) {
        xe::threading::Wait(cpu.ready_event.get(), false);
        cpu.parked.store(false);
        continue;
      }
      // Nothing has ever run, so poll instead of sleeping forever and say so.
      xe::threading::Wait(cpu.ready_event.get(), false,
                          std::chrono::seconds(1));
      cpu.parked.store(false);
      bool warned = false;
      if (!dispatched_any_.load() &&
          never_dispatched_warned_.compare_exchange_strong(warned, true)) {
        XELOGW(
            "GuestScheduler: no guest fiber dispatched after 1s, every guest "
            "thread is unqueued (created suspended and never resumed?)");
      }
      continue;
    }
    now = Clock::QueryHostUptimeMillis();
    uint64_t due = cpu.next_timed_repoll_ms;
    uint64_t sleep_ms = due > now ? due - now : 0;
    xe::threading::Wait(cpu.ready_event.get(), false,
                        std::chrono::milliseconds(sleep_ms));
    cpu.parked.store(false);
    stats_.idle_wakes.fetch_add(1, std::memory_order_relaxed);
  }
  XELOGI("GuestScheduler: CPU {} dispatch loop exited (shutting_down={})",
         cpu_index, shutting_down_.load());
}

void GuestScheduler::NoteForcedPreempt() {
  stats_.forced_preempts.fetch_add(1, std::memory_order_relaxed);
}

// hakuX-Boosted: see the header.
static void RaiseMax(std::atomic<uint64_t>& max, uint64_t value) {
  uint64_t seen = max.load(std::memory_order_relaxed);
  while (value > seen &&
         !max.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
  }
}

void GuestScheduler::NoteWakeLatency(uint64_t latency_us, XThread* thread) {
  static constexpr uint64_t kBounds[kWakeLatencyBuckets - 1] = {
      250, 1000, 4000, 16000, 33000, 64000};
  size_t bucket = kWakeLatencyBuckets - 1;
  for (size_t i = 0; i < kWakeLatencyBuckets - 1; ++i) {
    if (latency_us <= kBounds[i]) {
      bucket = i;
      break;
    }
  }
  wake_latency_counts_[bucket].fetch_add(1, std::memory_order_relaxed);
  RaiseMax(wake_latency_max_us_, latency_us);
  // hakuX-Boosted: guest_scheduler_skew_bound_us. The delay is counted as it
  // was, hold or no hold: the bound does not make a wake any earlier, it
  // keeps the signaller from running ahead of it. But a hold freezes the
  // holder's whole CPU, so a thread parked there that was signalled during
  // the hold runs late because of the bound, not because of the host. How
  // much of this wake's delay overlaps the last hold on its CPU is counted
  // apart and named in the late wake's line, so the two are not confused.
  uint64_t behind_hold_us = 0;
  if (skew_bound_ticks_ && thread) {
    const int cpu_index = thread->scheduler_links().cpu;
    if (cpu_index >= 0 && cpu_index < kMaxCpus) {
      const uint64_t hold_start =
          cpus_[cpu_index].skew_hold_start_us.load(std::memory_order_relaxed);
      const uint64_t hold_end =
          cpus_[cpu_index].skew_hold_end_us.load(std::memory_order_relaxed);
      const uint64_t now_us = NowMicros();
      const uint64_t signal_us = now_us > latency_us ? now_us - latency_us : 0;
      const uint64_t from = std::max(hold_start, signal_us);
      const uint64_t to = std::min(hold_end, now_us);
      if (hold_end > hold_start && to > from) {
        behind_hold_us = std::min(to - from, latency_us);
        stats_.skew_wakes_behind_hold.fetch_add(1, std::memory_order_relaxed);
        stats_.skew_wakes_behind_hold_us.fetch_add(behind_hold_us,
                                                   std::memory_order_relaxed);
      }
    }
  }
  if (latency_us >= kLateWakeLogUs && thread) {
    static std::atomic<int> budget{400};
    if (budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      const auto& links = thread->scheduler_links();
      XELOGW(
          "GuestScheduler: late wake, tid={:08X} '{}' ran {:.1f} ms after the "
          "signal it waited for (handle {:08X}, {} handle(s), cpu {}, frame "
          "{}){}",
          thread->thread_id(), thread->thread_name(),
          double(latency_us) / 1000.0,
          links.wait_handle_count ? links.wait_handles[0] : 0,
          uint32_t(links.wait_handle_count), links.cpu,
          xe::logging::GetFrameNumber(),
          behind_hold_us
              ? fmt::format(", {:.1f} ms of it behind a skew hold on its CPU",
                            double(behind_hold_us) / 1000.0)
              : std::string());
    }
  }
}

void GuestScheduler::NoteUnsignalledWake(XThread* thread) {
  stats_.unsignalled_wakes.fetch_add(1, std::memory_order_relaxed);
  static std::atomic<int> budget{200};
  if (thread && budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
    const auto& links = thread->scheduler_links();
    XELOGW(
        "GuestScheduler: wait satisfied with no wake sent: tid={:08X} '{}' "
        "found handle {:08X} ({} handle(s)) takeable on a re-poll although "
        "its gate never moved (cpu {}, frame {})",
        thread->thread_id(), thread->thread_name(),
        links.wait_handle_count ? links.wait_handles[0] : 0,
        uint32_t(links.wait_handle_count), links.cpu,
        xe::logging::GetFrameNumber());
  }
}

void GuestScheduler::NoteCriticalSectionWait(uint64_t wait_us,
                                             uint32_t section_address,
                                             uint32_t caller_lr,
                                             XThread* thread) {
  section_waits_.fetch_add(1, std::memory_order_relaxed);
  section_wait_us_.fetch_add(wait_us, std::memory_order_relaxed);
  RaiseMax(section_wait_max_us_, wait_us);
  if (wait_us >= kLateWakeLogUs) {
    section_waits_late_.fetch_add(1, std::memory_order_relaxed);
    static std::atomic<int> budget{400};
    if (budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      XELOGW(
          "GuestScheduler: tid={:08X} '{}' waited {:.1f} ms for critical "
          "section {:08X} (lr={:08X}, frame {})",
          thread ? thread->thread_id() : 0,
          thread ? thread->thread_name() : std::string("none"),
          double(wait_us) / 1000.0, section_address, caller_lr,
          xe::logging::GetFrameNumber());
    }
  }
}

std::string GuestScheduler::LatencyTotalsLine(const uint64_t* since_counts,
                                              uint64_t since_section_waits,
                                              uint64_t since_section_wait_us) {
  uint64_t counts[kWakeLatencyBuckets];
  for (size_t i = 0; i < kWakeLatencyBuckets; ++i) {
    counts[i] = wake_latency_counts_[i].load(std::memory_order_relaxed) -
                (since_counts ? since_counts[i] : 0);
  }
  uint64_t waits =
      section_waits_.load(std::memory_order_relaxed) - since_section_waits;
  uint64_t wait_us =
      section_wait_us_.load(std::memory_order_relaxed) - since_section_wait_us;
  return fmt::format(
      "wakes by delay after the signal: <=0.25ms {}, <=1ms {}, <=4ms {}, "
      "<=16ms {}, <=33ms {}, <=64ms {}, more {} (longest of the run {:.1f} "
      "ms) | critical sections: {} contended, {} us each on average (longest "
      "of the run {:.1f} ms, {} of 16 ms or more in the run)",
      counts[0], counts[1], counts[2], counts[3], counts[4], counts[5],
      counts[6],
      double(wake_latency_max_us_.load(std::memory_order_relaxed)) / 1000.0,
      waits, waits ? wait_us / waits : 0,
      double(section_wait_max_us_.load(std::memory_order_relaxed)) / 1000.0,
      section_waits_late_.load(std::memory_order_relaxed));
}

void GuestScheduler::ReportStatsIfDue() {
  if (!cvars::guest_scheduler_stats) {
    return;
  }
  uint64_t now_ms = Clock::QueryHostUptimeMillis();
  if (now_ms - stats_last_report_ms_ < 1000) {
    return;
  }
  stats_last_report_ms_ = now_ms;
  auto take = [](std::atomic<uint64_t>& v) {
    return v.exchange(0, std::memory_order_relaxed);
  };
  uint64_t repolls = take(stats_.repolls);
  uint64_t rereadied = take(stats_.rereadied);
  uint64_t backstop_rescues = take(stats_.backstop_rescues);
  uint64_t idle_wakes = take(stats_.idle_wakes);
  uint64_t switches = take(stats_.switches);
  uint64_t forced = take(stats_.forced_preempts);
  uint64_t io_calls = take(stats_.io_calls);
  uint64_t io_queue = take(stats_.io_queue_ns);
  uint64_t io_run = take(stats_.io_run_ns);
  uint64_t io_queue_max = take(stats_.io_queue_max_ns);
  double ticks_per_us = ticks_per_us_ > 0.0 ? ticks_per_us_ : 1.0;
  auto to_us = [ticks_per_us](uint64_t ticks) {
    return uint64_t(double(ticks) / ticks_per_us);
  };
  XELOGI(
      "GuestScheduler: repolls {}/s (rereadied {}, backstop rescues {}), idle "
      "wakes {}, switches "
      "{}, forced preempts {} | io {} calls, queued avg {} us max {} us, ran "
      "avg {} us",
      repolls, rereadied, backstop_rescues, idle_wakes, switches, forced,
      io_calls,
      io_calls ? to_us(io_queue / io_calls) : 0, to_us(io_queue_max),
      io_calls ? to_us(io_run / io_calls) : 0);
  // hakuX-Boosted: why fibers left their CPU, what the cheaper paths did
  // and what the one lock cost.
  uint64_t empty_yields = 0;
  for (int i = 0; i < kMaxCpus; ++i) {
    uint64_t total = cpus_[i].empty_yields.load(std::memory_order_relaxed);
    empty_yields += total - cpus_[i].empty_yields_reported;
    cpus_[i].empty_yields_reported = total;
  }
  XELOGI(
      "GuestScheduler: left the CPU: for a wait or sleep {}, NtYieldExecution "
      "{}, zero delay {}, spin site {}, slice end {} | yields that found "
      "nothing to run and returned at once {} | parked: yield spinners {}, "
      "poll loops {} | APC pokes {}",
      take(stats_.blocks),
      take(stats_.leaves[size_t(YieldCause::kNtYield)]),
      take(stats_.leaves[size_t(YieldCause::kZeroDelay)]),
      take(stats_.leaves[size_t(YieldCause::kSpinSite)]),
      take(stats_.leaves[size_t(YieldCause::kQuantumEnd)]), empty_yields,
      take(stats_.yield_parks), take(stats_.poll_parks),
      take(stats_.apc_pokes));
  if (cvars::guest_scheduler_park_yield_spinners) {
    XELOGI(
        "GuestScheduler: yield spinner looks that did not park: another "
        "yielder {}, work between yields {}, run too young {}, not "
        "allowed here {} | dispatches during a run {}",
        take(stats_.yield_park_new_thread), take(stats_.yield_park_slow),
        take(stats_.yield_park_young), take(stats_.yield_park_barred),
        take(stats_.yield_park_others_ran));
  }
  // hakuX-Boosted: guest_scheduler_park_waits.
  if (park_waits_) {
    XELOGI(
        "GuestScheduler: busy waits: parked yield spinners {}, poll loops {}, "
        "{} us parked in all | parks ended by the deadline {}, by an event {} "
        "| looks that did not park: an event since the last look {}, not "
        "allowed here {} | events counted {} | poll site looks {}, registers "
        "changed {}",
        take(stats_.wait_parks_yield), take(stats_.wait_parks_poll),
        to_us(take(stats_.wait_park_ticks)), take(stats_.wait_ended_timer),
        take(stats_.wait_ended_event), take(stats_.wait_looks_event),
        take(stats_.wait_looks_barred), take(stats_.wait_events),
        take(stats_.poll_visits), take(stats_.poll_changed));
  }
  {
    uint64_t acquisitions = take(stats_.lock_acquisitions);
    uint64_t wait_ticks = take(stats_.lock_wait_ticks);
    uint64_t held_ticks = take(stats_.lock_held_ticks);
    XELOGI(
        "GuestScheduler: scheduler lock: taken {} times, {} us waiting for it "
        "({} ns each), {} us holding it ({} ns each)",
        acquisitions, to_us(wait_ticks),
        acquisitions ? to_us(wait_ticks * 1000) / acquisitions : 0,
        to_us(held_ticks),
        acquisitions ? to_us(held_ticks * 1000) / acquisitions : 0);
  }
  // hakuX-Boosted: guest_scheduler_skew_bound_us
  // (boosted/notes/guest-skew-bound.md says how to read it).
  if (skew_bound_ticks_) {
    SkewLongest longest;
    {
      std::lock_guard<std::mutex> lock(skew_report_mutex_);
      longest = skew_longest_;
      skew_longest_ = SkewLongest();
      skew_longest_ticks_.store(0, std::memory_order_relaxed);
    }
    const uint64_t holds_start = take(stats_.skew_holds_start);
    const uint64_t holds_progress = take(stats_.skew_holds_progress);
    XELOGI(
        "GuestScheduler: skew bound: holds {} (start gate {}, progress gate "
        "{}), held {} us in all, longest {} us ({}), ended by the cap {}, cut "
        "short by a preemption {} | awaited host threads on a core for {} us "
        "of the holds | signals that readied a thread on another CPU {} | "
        "wakes that waited behind a hold {} ({} us) | overdue probes of a "
        "host thread busy in host code, not held for {}",
        holds_start + holds_progress, holds_start, holds_progress,
        to_us(take(stats_.skew_hold_ticks)), to_us(longest.ticks),
        longest.ticks
            ? fmt::format(
                  "tid={:08X} '{}' on cpu {} waited for cpu {} tid={:08X} "
                  "'{}', {} gate{}",
                  longest.holder_tid, longest.holder_name, longest.holder_cpu,
                  longest.target_cpu, longest.target_tid, longest.target_name,
                  longest.progress_gate ? "progress" : "start",
                  longest.capped ? ", ended by the cap" : "")
            : std::string("none"),
        take(stats_.skew_holds_capped), take(stats_.skew_holds_cut),
        take(stats_.skew_target_on_core_ns) / 1000, take(stats_.skew_debts),
        take(stats_.skew_wakes_behind_hold),
        take(stats_.skew_wakes_behind_hold_us), take(skew_host_busy_));
  }
  // hakuX-Boosted: the missed wakes by kind (boosted/notes/guest-scheduler.md
  // says what each one is and which should read nought).
  XELOGI(
      "GuestScheduler: missed wakes: made good by the timed look {}, by the "
      "64 ms backstop {}, satisfied with no wake sent {} | not misses: queue "
      "hand-ons {}, signals caught at the park {}",
      take(stats_.timed_look_rescues), backstop_rescues,
      take(stats_.unsignalled_wakes), take(stats_.queue_hand_ons),
      take(stats_.park_catches));
  // hakuX-Boosted: the last second's wakes and critical section waits.
  XELOGI("GuestScheduler: {}",
         LatencyTotalsLine(wake_latency_reported_, section_waits_reported_,
                           section_wait_us_reported_));
  for (size_t i = 0; i < kWakeLatencyBuckets; ++i) {
    wake_latency_reported_[i] =
        wake_latency_counts_[i].load(std::memory_order_relaxed);
  }
  section_waits_reported_ = section_waits_.load(std::memory_order_relaxed);
  section_wait_us_reported_ = section_wait_us_.load(std::memory_order_relaxed);
}

// Names what a fiber is parked on, for the no-progress dump.
static const char* WaitObjectKind(XObject* object) {
  if (!object) {
    return "none";
  }
  switch (object->type()) {
    case XObject::Type::Event:
      return "event";
    case XObject::Type::Semaphore:
      return "semaphore";
    case XObject::Type::Mutant:
      return "mutant";
    case XObject::Type::Thread:
      return "thread";
    case XObject::Type::Timer:
      return "timer";
    default:
      return "other";
  }
}

namespace {
const char* CooperativeWaitKindName(uint8_t kind) {
  switch (static_cast<XThread::CooperativeWaitKind>(kind)) {
    case XThread::CooperativeWaitKind::kSingle:
      return "single";
    case XThread::CooperativeWaitKind::kMultiAny:
      return "multi-any";
    case XThread::CooperativeWaitKind::kMultiAll:
      return "multi-all";
    case XThread::CooperativeWaitKind::kDelay:
      return "delay";
    case XThread::CooperativeWaitKind::kFence:
      return "fence";
    case XThread::CooperativeWaitKind::kIoOffload:
      return "io-offload";
    default:
      return "none";
  }
}

// "multi-any[4] handles=F8000030,F8000034,..." - the handles cross-reference
// against the signal ring dumped alongside.
std::string FormatWaitShape(const XThread::SchedulerLinks& links) {
  std::string out = CooperativeWaitKindName(links.wait_kind);
  if (!links.wait_handle_count) {
    return out;
  }
  out += fmt::format("[{}] handles=", links.wait_handle_count);
  uint32_t shown = links.wait_handle_count < 8 ? links.wait_handle_count : 8;
  for (uint32_t i = 0; i < shown; ++i) {
    out += fmt::format("{}{:08X}", i ? "," : "", links.wait_handles[i]);
  }
  if (links.wait_handle_count > shown) {
    out += ",...";
  }
  return out;
}
}  // namespace

void GuestScheduler::ReportNoProgress(bool on_request) {
  if (on_request) {
    XELOGW(
        "GuestScheduler: thread dump on request (frame {}). What every guest "
        "thread is running or waiting on:",
        xe::logging::GetFrameNumber());
  } else {
    XELOGW(
        "GuestScheduler: no guest frame presented in {} watchdog ticks while "
        "the dispatch threads keep switching. Every fiber below is waiting on "
        "something none of them is producing:",
        no_progress_ticks_);
  }
  uint64_t now_ms = Clock::QueryHostUptimeMillis();
  // hakuX-Boosted: the whole run's wake delays and critical section waits.
  XELOGW("  {}", LatencyTotalsLine(nullptr, 0, 0));
  // hakuX-Boosted: the skew bound's totals of the run, and below, per CPU,
  // whether its running thread is in a hold or has a probe outstanding.
  const double skew_ticks_per_us = ticks_per_us_ > 0.0 ? ticks_per_us_ : 1.0;
  const uint64_t skew_now = Clock::host_tick_count_raw();
  if (skew_bound_ticks_) {
    XELOGW(
        "  skew bound: {} holds in the run, {:.1f} ms held in all, longest "
        "{:.1f} ms, {} ended by the cap",
        skew_run_holds_.load(std::memory_order_relaxed),
        double(skew_run_hold_ticks_.load(std::memory_order_relaxed)) /
            skew_ticks_per_us / 1000.0,
        double(skew_run_longest_ticks_.load(std::memory_order_relaxed)) /
            skew_ticks_per_us / 1000.0,
        skew_run_capped_.load(std::memory_order_relaxed));
  }
  // hakuX-Boosted: the handles the parked fibers wait on, described once
  // lock_ is released (looking an object up takes the kernel's own locks).
  std::vector<uint32_t> waited_handles;
  std::unique_lock<std::mutex> lock(lock_);
  for (int i = 0; i < kMaxCpus; ++i) {
    Cpu& cpu = cpus_[i];
    if (XThread* running = cpu.current_thread) {
      auto* context = running->thread_state()->context();
      auto* kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
      XELOGW(
          "  CPU {} running tid={:08X} '{}' last_safepoint={:08X} lr={:08X} "
          "irql={} preempt_requested={} ready_summary={:#x}",
          i, running->thread_id(), running->thread_name(),
          uint32_t(context->last_safepoint_pc), uint32_t(context->lr),
          uint32_t(kpcr->current_irql),
          uint32_t(context->preempt_requested),
          cpu.ready_summary.load(std::memory_order_relaxed));
      if (skew_bound_ticks_) {
        const uint64_t probe =
            cpu.skew_probe_tick.load(std::memory_order_relaxed);
        XELOGW(
            "    skew bound: {}, probe {}",
            cpu.skew_holding.load() ? "in a hold" : "not in a hold",
            probe && skew_now > probe
                ? fmt::format("unanswered for {:.1f} ms",
                              double(skew_now - probe) / skew_ticks_per_us /
                                  1000.0)
                : std::string("answered"));
      }
    } else {
      XELOGW("  CPU {} idle, ready_summary={:#x}", i,
             cpu.ready_summary.load(std::memory_order_relaxed));
    }
    // Parked fibers are the interesting half: the cycle is whatever they are
    // all waiting for.
    int listed = 0;
    for (XThread* t = cpu.blocked_head; t && listed < 8;
         t = t->scheduler_links().ready_next, ++listed) {
      auto& links = t->scheduler_links();
      XObject* obj = t->cooperative_wait_object();
      auto* context = t->thread_state()->context();
      int64_t due_in =
          links.wait_deadline_ms
              ? int64_t(links.wait_deadline_ms) - int64_t(now_ms)
              : -1;
      XELOGW(
          "    blocked tid={:08X} '{}' last_safepoint={:08X} lr={:08X} on {} "
          "obj={} wait={} gated={} alertable={} epoch={} deadline_in_ms={}",
          t->thread_id(), t->thread_name(),
          uint32_t(context->last_safepoint_pc), uint32_t(context->lr),
          WaitObjectKind(obj), static_cast<const void*>(obj),
          FormatWaitShape(links), links.wait_gated ? 1 : 0,
          links.wait_alertable ? 1 : 0, links.wait_epoch, due_in);
      // hakuX-Boosted: the guest call chain of the parked fiber, from the PPC
      // back chain (0(r1) is the caller's frame, its saved LR 8 below that):
      // the wait's lr is a library routine, the callers say which lock or
      // job the game is waiting in.
      {
        std::string chain;
        uint32_t sp = uint32_t(context->r[1]);
        for (int depth = 0; depth < 8; ++depth) {
          if (sp < 0x40000000u || sp >= 0x80000000u || (sp & 3)) {
            break;
          }
          uint32_t next = xe::load_and_swap<uint32_t>(
              context->TranslateVirtual<const uint8_t*>(sp));
          if (next <= sp || next >= 0x80000000u || (next & 3)) {
            break;
          }
          uint32_t ret = xe::load_and_swap<uint32_t>(
              context->TranslateVirtual<const uint8_t*>(next - 8));
          chain += fmt::format(" {:08X}", ret);
          sp = next;
        }
        if (!chain.empty()) {
          XELOGW("      called from:{}", chain);
        }
      }
      for (uint32_t h = 0; h < links.wait_handle_count && h < 8; ++h) {
        if (links.wait_handles[h]) {
          waited_handles.push_back(links.wait_handles[h]);
        }
      }
    }
    // Ready-but-not-running fibers matter too: a queue that never drains
    // means the CPU is rotating between the same few spinners.
    for (int prio = 31; prio >= 0; --prio) {
      for (XThread* t = cpu.ready_head[prio]; t;
           t = t->scheduler_links().ready_next) {
        auto* context = t->thread_state()->context();
        XELOGW("    ready   tid={:08X} '{}' last_safepoint={:08X} lr={:08X} "
               "prio={}",
               t->thread_id(), t->thread_name(),
               uint32_t(context->last_safepoint_pc), uint32_t(context->lr),
               prio);
      }
    }
  }
  lock.unlock();
  // hakuX-Boosted: what each waited object is and whether it is signalled
  // now. A waiter parked on an event that IS signalled is the scheduler's
  // fault (the wake or the turn in the queue was lost); one that is not
  // signalled was never set by the game, and its last setter and creator say
  // where to look in the guest.
  if (!waited_handles.empty()) {
    std::sort(waited_handles.begin(), waited_handles.end());
    waited_handles.erase(
        std::unique(waited_handles.begin(), waited_handles.end()),
        waited_handles.end());
    XELOGW("  waited objects:");
    uint32_t guest_now_ms = Clock::QueryGuestUptimeMillis();
    for (uint32_t handle : waited_handles) {
      auto object =
          kernel_state()->object_table()->LookupObject<XObject>(handle);
      if (!object) {
        XELOGW("    {:08X} no such object any more", handle);
        continue;
      }
      if (object->type() == XObject::Type::Event) {
        auto* ev = static_cast<XEvent*>(object.get());
        uint32_t ev_type = 0;
        uint32_t ev_state = 0;
        ev->Query(&ev_type, &ev_state);
        XThread* front = ev->CooperativeFrontWaiter();
        XELOGW(
            "    {:08X} event {} signalled={} first_in_queue={:08X} last_set "
            "by={:08X} lr={:08X} {} ms ago, created by={:08X} lr={:08X}",
            handle, ev->manual_reset() ? "manual-reset" : "auto-reset",
            ev_state, front ? front->handle() : 0, ev->last_set_thread(),
            ev->last_set_lr(),
            ev->last_set_uptime_ms()
                ? int64_t(guest_now_ms) - int64_t(ev->last_set_uptime_ms())
                : -1,
            ev->creator_thread(), ev->creator_lr());
      } else {
        XELOGW("    {:08X} {}", handle, WaitObjectKind(object.get()));
      }
    }
  }
  // The other half of the picture: what was actually signalled recently. A
  // handle the fibers above wait on that never appears here names the producer
  // that stopped; one that appears repeatedly while a waiter stays parked
  // points at the wake being lost instead of never sent.
  auto signals = XObject::RecentCooperativeSignals(64);
  if (signals.empty()) {
    XELOGW("  recent cooperative signals: none recorded");
    return;
  }
  XELOGW("  last {} cooperative signals (oldest first):", signals.size());
  for (const auto& rec : signals) {
    XELOGW(
        "    #{} handle={:08X} type={} by_tid={:08X} lr={:08X} uptime_ms={}",
        rec.seq, rec.handle, uint32_t(rec.type), rec.signaler_thread,
        rec.signaler_lr, rec.uptime_ms);
  }
}

void GuestScheduler::WatchdogLoop() {
  // Microseconds, not milliseconds: an integer division to ms floors every
  // sub-millisecond quantum to the same 1 ms tick, so the setting would stop
  // meaning anything below 1000. The host still adds wakeup slack, so a short
  // quantum is a target rather than a guarantee.
  uint64_t period_us = cvars::guest_scheduler_quantum_us;
  if (period_us < kMinWatchdogPeriodUs) {
    period_us = kMinWatchdogPeriodUs;
  }
  while (!shutting_down_.load()) {
    if (period_us >= 1000) {
      // Event wait so Shutdown's Set is observed immediately.
      xe::threading::Wait(watchdog_event_.get(), false,
                          std::chrono::milliseconds(period_us / 1000));
    } else {
      // Wait only takes milliseconds, so a sub-millisecond period sleeps
      // instead. Shutdown latency is then bounded by the period itself.
      xe::threading::PreciseSleep(std::chrono::microseconds(period_us));
    }
    if (shutting_down_.load()) {
      break;
    }
    // hakuX-Boosted: a dump of every guest thread on request. The report
    // below only comes when no frame is presented, and a game whose picture
    // still moves while its script waits for ever (Fable II at the trader:
    // frames presented, one thread polling) leaves nothing in the log. Create
    // a file called dump_threads beside the log file and the next second's
    // tick writes the report and removes the file:
    //   adb shell touch <storage root>/dump_threads
    // Checked about once a second, outside lock_.
    if (++dump_request_ticks_ >= 1000000 / period_us) {
      dump_request_ticks_ = 0;
      if (!cvars::log_file.empty()) {
        std::error_code ec;
        auto trigger = cvars::log_file.parent_path() / "dump_threads";
        if (std::filesystem::exists(trigger, ec)) {
          std::filesystem::remove(trigger, ec);
          ReportNoProgress(true);
        }
      }
    }
    // No-progress detection, outside lock_ (ReportNoProgress takes it).
    // Only meaningful once something has been dispatched, so a title still
    // loading is not reported.
    uint32_t frame = xe::logging::GetFrameNumber();
    if (frame != last_frame_number_ || !dispatched_any_.load()) {
      last_frame_number_ = frame;
      no_progress_ticks_ = 0;
      no_progress_reported_ = false;
    } else if (++no_progress_ticks_ >= kNoProgressReportTicks &&
               !no_progress_reported_) {
      no_progress_reported_ = true;
      ReportNoProgress();
    }

    // hakuX-Boosted: guest_scheduler_skew_host_progress.
    if (skew_bound_ticks_ && skew_host_progress_) {
      SkewSampleHostThreads();
    }
    uint64_t now = Clock::host_tick_count_raw();
    TimedLock lock(this);
    for (int i = 0; i < kMaxCpus; ++i) {
      XThread* running = cpus_[i].current_thread;
      // hakuX-Boosted: guest_scheduler_skew_bound_us, the probe. Every tick
      // each running fiber is asked to show that its host thread is being
      // run: the tick is noted (unless the last probe is still unanswered,
      // whose age is the measure of how far the CPU is behind) and the
      // preempt flag takes the fiber to its next safepoint, which answers
      // and, the flag having been up for nothing else, returns at once
      // (SkewAtSafepoint, SkewOnlyProbed). The time is stored before the
      // flag is raised, so a safepoint that sees the flag sees the probe.
      if (skew_bound_ticks_ && running) {
        if (!cpus_[i].skew_probe_tick.load(std::memory_order_relaxed)) {
          cpus_[i].skew_probe_tick.store(now);
        }
        running->thread_state()->context()->preempt_requested = 1;
      }
      if (running && now >= cpus_[i].quantum_deadline_tick) {
        running->thread_state()->context()->preempt_requested = 1;
      }
      // Stall detector: a dispatch count that has not moved for a whole
      // window separates the wedge modes - flag still set means the fiber
      // never reaches a safepoint, flag cleared means it yields but makes no
      // progress.
      // hakuX-Boosted: a yield that returns at once is progress too, or a
      // lone thread in a yield loop would read as a stalled CPU.
      uint64_t seq = cpus_[i].switch_seq.load(std::memory_order_relaxed) +
                     cpus_[i].empty_yields.load(std::memory_order_relaxed);
      if (!running || seq != stall_last_seq_[i]) {
        stall_last_seq_[i] = seq;
        stall_ticks_[i] = 0;
        stall_reported_[i] = false;
        continue;
      }
      if (++stall_ticks_[i] < kStallReportTicks || stall_reported_[i]) {
        continue;
      }
      stall_reported_[i] = true;
      auto* context = running->thread_state()->context();
      auto* kpcr = context->TranslateVirtualGPR<X_KPCR*>(context->r[13]);
      XELOGW(
          "GuestScheduler: CPU {} has not switched fibers in {} watchdog "
          "ticks. Running tid={:08X} '{}' last_safepoint={:08X} lr={:08X} "
          "irql={} preempt_requested={} irql_defers={} lock_defers={} "
          "ready_summary={:#x}",
          i, stall_ticks_[i], running->thread_id(), running->thread_name(),
          uint32_t(context->last_safepoint_pc), uint32_t(context->lr),
          uint32_t(kpcr->current_irql),
          uint32_t(context->preempt_requested),
          running->scheduler_links().preempt_defers_irql,
          running->scheduler_links().preempt_defers_lock,
          cpus_[i].ready_summary.load(std::memory_order_relaxed));
    }
  }
}

}  // namespace kernel
}  // namespace xe
