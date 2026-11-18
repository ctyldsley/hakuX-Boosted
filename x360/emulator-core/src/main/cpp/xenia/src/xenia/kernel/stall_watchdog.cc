/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * hakuX-Boosted: stall_watchdog.                                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// hakuX-Boosted: the stall watchdog. See stall_watchdog.h for what it does.
//
// The question it answers is the one a freeze like Gears of War's at the
// door leaves open: the picture still moves (the swap thread presents) and
// audio plays (host threads), but the game's own tick has stopped. Which
// thread is everyone waiting for, and what is it waiting for?
//
// Everything read here belongs to live threads and is read without their
// locks. For a thread that is truly stalled it does not change, which is the
// case that matters; for a thread that is moving, a line may be stale or
// mixed. Guest memory outside a thread's own stack is only read after the
// heap says the pages are readable.

#include "xenia/kernel/stall_watchdog.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/mutex.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xobject.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

DEFINE_bool(
    stall_watchdog, true,
    "Watch for a game whose picture still moves but whose own threads have "
    "stopped (no kernel call from any guest thread but the one presenting "
    "frames for stall_watchdog_seconds), and write one STALL REPORT to the "
    "log saying what every guest thread is doing, waiting on and last "
    "called. Only observes; costs a few stores per kernel call.",
    "Kernel");
DEFINE_uint32(
    stall_watchdog_seconds, 10,
    "With stall_watchdog: how long every guest thread other than the "
    "presenting one must go without a kernel call before the report is "
    "written. A thread blocked in a wait for that long counts as stopped.",
    "Kernel");

namespace xe {
namespace kernel {

bool g_kernel_call_trace_enabled = false;

// The name pointer of VdSwap, learnt by the watchdog the first time it sees
// the call in a ring (names are cpu::Export::name, one pointer per export),
// and the guest thread id of the last thread to call it.
static std::atomic<const char*> g_swap_name{nullptr};
static std::atomic<uint32_t> g_swap_thread_id{0};

uint32_t KernelCallTraceEnter(cpu::ppc::PPCContext* ppc_context,
                              const char* name) {
  XThread* thread = XThread::FromContext(ppc_context);
  if (!thread) {
    return 0;
  }
  KernelCallTrace& trace = thread->kernel_call_trace();
  const uint32_t seq = trace.count.load(std::memory_order_relaxed) + 1;
  KernelCallRecord& rec = trace.records[(seq - 1) % KernelCallTrace::kSize];
  const uint64_t now = Clock::QueryHostTickCount();
  rec.seq = 0;
  rec.name = name;
  rec.host_tick = now;
  rec.lr = uint32_t(ppc_context->lr);
  for (uint32_t i = 0; i < 6; ++i) {
    rec.args[i] = uint32_t(ppc_context->r[3 + i]);
  }
  rec.result = 0;
  rec.frame = xe::logging::GetFrameNumber();
  rec.returned = 0;
  rec.seq = seq;
  trace.count.store(seq, std::memory_order_release);
  trace.last_activity_tick.store(now, std::memory_order_relaxed);
  if (name == g_swap_name.load(std::memory_order_relaxed)) {
    g_swap_thread_id.store(thread->thread_id(), std::memory_order_relaxed);
  }
  return seq;
}

void KernelCallTraceLeave(cpu::ppc::PPCContext* ppc_context, uint32_t seq,
                          uint32_t result) {
  if (!seq) {
    return;
  }
  XThread* thread = XThread::FromContext(ppc_context);
  if (!thread) {
    return;
  }
  KernelCallTrace& trace = thread->kernel_call_trace();
  KernelCallRecord& rec = trace.records[(seq - 1) % KernelCallTrace::kSize];
  // A nested call (an APC run inside a wait) may have reused the slot.
  if (rec.seq == seq) {
    rec.result = result;
    rec.returned = 1;
  }
  trace.last_activity_tick.store(Clock::QueryHostTickCount(),
                                 std::memory_order_relaxed);
}

namespace {

struct Watchdog {
  KernelState* kernel_state = nullptr;
  std::unique_ptr<xe::threading::Thread> thread;
  std::unique_ptr<xe::threading::Event> wake;
  std::atomic<bool> stop{false};

  // Loop state, touched only by the watchdog thread.
  uint32_t last_frame = 0;
  uint64_t last_frame_tick = 0;
  bool reported = false;
  uint64_t reported_tick = 0;

  void Loop();
  void Tick();
  void Report(const std::vector<object_ref<XThread>>& threads,
              uint32_t swap_tid, uint64_t now, uint64_t freq,
              uint32_t seconds);
};

Watchdog* g_watchdog = nullptr;

// Reads guest memory only if the heap says every byte is readable. Takes the
// global critical region (without blocking for long) so the pages cannot be
// released between the check and the copy.
bool SafeReadGuest(Memory* memory, uint32_t address, void* out,
                   uint32_t size) {
  if (!address || !size || address + size < address) {
    return false;
  }
  BaseHeap* heap = memory->LookupHeap(address);
  if (!heap) {
    return false;
  }
  auto& mutex = xe::global_critical_region::mutex();
  bool locked = false;
  for (int i = 0; i < 20; ++i) {
    if (mutex.try_lock()) {
      locked = true;
      break;
    }
    xe::threading::Sleep(std::chrono::milliseconds(1));
  }
  if (!locked) {
    return false;
  }
  bool ok = heap->QueryRangeAccess(address, address + size - 1) !=
            xe::memory::PageAccess::kNoAccess;
  if (ok) {
    std::memcpy(out, memory->TranslateVirtual<const uint8_t*>(address), size);
  }
  mutex.unlock();
  return ok;
}

bool SafeReadGuest32(Memory* memory, uint32_t address, uint32_t* out) {
  uint8_t bytes[4];
  if (!SafeReadGuest(memory, address, bytes, 4)) {
    return false;
  }
  *out = xe::load_and_swap<uint32_t>(bytes);
  return true;
}

// A word of |thread|'s own stack: in range it is committed and read
// directly, outside it goes through the heap check.
bool ReadStack32(Memory* memory, XThread* thread, uint32_t address,
                 uint32_t* out) {
  const uint32_t low = thread->stack_limit();
  const uint32_t high = thread->stack_base();
  if (low && high > low && address >= low && address <= high - 4) {
    *out = xe::load_and_swap<uint32_t>(
        memory->TranslateVirtual<const uint8_t*>(address));
    return true;
  }
  return SafeReadGuest32(memory, address, out);
}

const char* ObjectTypeName(XObject::Type type) {
  switch (type) {
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
    case XObject::Type::File:
      return "file";
    case XObject::Type::IOCompletion:
      return "io-completion";
    case XObject::Type::NotifyListener:
      return "notify-listener";
    default:
      return "other";
  }
}

const char* DispatcherTypeName(uint8_t type) {
  switch (type) {
    case DISPATCHER_MANUAL_RESET_EVENT:
      return "manual-reset event";
    case DISPATCHER_AUTO_RESET_EVENT:
      return "auto-reset event";
    case DISPATCHER_MUTANT:
      return "mutant";
    case DISPATCHER_QUEUE:
      return "queue";
    case DISPATCHER_SEMAPHORE:
      return "semaphore";
    case DISPATCHER_THREAD:
      return "thread";
    case DISPATCHER_MANUAL_RESET_TIMER:
      return "manual-reset timer";
    case DISPATCHER_AUTO_RESET_TIMER:
      return "auto-reset timer";
    default:
      return "unknown";
  }
}

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

// Everything the report needs to name threads and signallers.
struct ReportContext {
  KernelState* kernel_state;
  Memory* memory;
  uint64_t now;
  uint64_t freq;
  // Thread handle -> guest thread id, KTHREAD address -> thread id.
  std::unordered_map<uint32_t, uint32_t> tid_by_handle;
  std::unordered_map<uint32_t, uint32_t> tid_by_kthread;
  // Handle -> the latest cooperative signal recorded for it.
  std::unordered_map<uint32_t, XObject::SignalRecord> last_signal;

  double SecondsAgo(uint64_t tick) const {
    return tick && now > tick ? double(now - tick) / double(freq) : 0.0;
  }
  std::string ThreadName(uint32_t handle) const {
    if (handle == 0xFFFFFFFF) {
      return "host";
    }
    auto it = tid_by_handle.find(handle);
    if (it == tid_by_handle.end()) {
      return fmt::format("handle {:08X}", handle);
    }
    return fmt::format("tid {:08X}", it->second);
  }
};

std::string DescribeTimeout(const ReportContext& rc, uint32_t timeout_ptr) {
  if (!timeout_ptr) {
    return "timeout infinite";
  }
  uint8_t bytes[8];
  if (!SafeReadGuest(rc.memory, timeout_ptr, bytes, 8)) {
    return fmt::format("timeout at {:08X} unreadable", timeout_ptr);
  }
  int64_t value = xe::load_and_swap<int64_t>(bytes);
  if (value < 0) {
    return fmt::format("timeout {:.1f} ms relative", double(-value) / 1e4);
  }
  if (value == 0) {
    return "timeout 0 (poll)";
  }
  return fmt::format("timeout absolute {:016X}", uint64_t(value));
}

// Who last signalled |handle|, from what the kernel records.
std::string DescribeSignaller(const ReportContext& rc, XObject* object,
                              uint32_t handle) {
  std::string out;
  if (object && object->type() == XObject::Type::Event) {
    auto* ev = static_cast<XEvent*>(object);
    if (ev->last_set_thread()) {
      out += fmt::format(" last set by {} lr={:08X} {} ms ago",
                         rc.ThreadName(ev->last_set_thread()),
                         ev->last_set_lr(),
                         ev->last_set_uptime_ms()
                             ? int64_t(Clock::QueryGuestUptimeMillis()) -
                                   int64_t(ev->last_set_uptime_ms())
                             : -1);
    } else {
      out += " never set";
    }
    out += fmt::format(", created by {} lr={:08X}",
                       rc.ThreadName(ev->creator_thread()), ev->creator_lr());
  }
  auto it = rc.last_signal.find(handle);
  if (it != rc.last_signal.end()) {
    out += fmt::format(" (signal ring: by {} lr={:08X} at uptime {} ms)",
                       rc.ThreadName(it->second.signaler_thread),
                       it->second.signaler_lr, it->second.uptime_ms);
  } else if (!object || object->type() != XObject::Type::Event) {
    out += " (no signal in the recent signal ring)";
  }
  return out;
}

std::string DescribeHandle(ReportContext& rc, uint32_t handle) {
  if (!handle || handle >= 0xFFFFFFF0u) {
    return fmt::format("handle {:08X} (pseudo handle, not resolved)", handle);
  }
  auto object =
      rc.kernel_state->object_table()->LookupObject<XObject>(handle);
  if (!object) {
    return fmt::format("handle {:08X} no such object", handle);
  }
  std::string out = fmt::format("handle {:08X} {}", handle,
                                ObjectTypeName(object->type()));
  if (object->type() == XObject::Type::Event) {
    auto* ev = static_cast<XEvent*>(object.get());
    uint32_t ev_type = 0;
    uint32_t ev_state = 0;
    ev->Query(&ev_type, &ev_state);
    out += fmt::format(" {} signalled={}",
                       ev->manual_reset() ? "manual-reset" : "auto-reset",
                       ev_state);
  } else if (object->type() == XObject::Type::Thread) {
    auto* t = static_cast<XThread*>(object.get());
    out += fmt::format(" tid {:08X} '{}' (waiting for it to exit)",
                       t->thread_id(), t->thread_name());
  } else if (XObject::HasDispatcherHeader(object->type()) &&
             object->guest_object()) {
    uint32_t state = 0;
    if (SafeReadGuest32(rc.memory, object->guest_object() + 4, &state)) {
      out += fmt::format(" guest header signal_state={}", int32_t(state));
    }
  }
  if (!object->name().empty()) {
    out += fmt::format(" name '{}'", object->name());
  }
  out += DescribeSignaller(rc, object.get(), handle);
  return out;
}

// A Ke* wait names a guest dispatcher object by address.
std::string DescribeDispatcher(ReportContext& rc, uint32_t address) {
  uint8_t header[16];
  if (!SafeReadGuest(rc.memory, address, header, sizeof(header))) {
    return fmt::format("object {:08X} unreadable", address);
  }
  uint8_t type = header[0];
  int32_t signal_state = int32_t(xe::load_and_swap<uint32_t>(header + 4));
  uint32_t flink = xe::load_and_swap<uint32_t>(header + 8);
  uint32_t blink = xe::load_and_swap<uint32_t>(header + 12);
  std::string out =
      fmt::format("object {:08X} {} guest signal_state={}", address,
                  DispatcherTypeName(type), signal_state);
  if (flink == kXObjSignature) {
    out += " -> " + DescribeHandle(rc, blink);
  }
  return out;
}

// What a thread whose latest kernel call has not returned is waiting on,
// decoded from the call's arguments (r3..r8 at entry).
void DescribeInFlightWait(ReportContext& rc, const KernelCallRecord& rec) {
  const char* name = rec.name ? rec.name : "";
  const uint32_t* a = rec.args;
  auto handle_array = [&](uint32_t count, uint32_t ptr, bool pointers) {
    count = std::min<uint32_t>(count, 64);
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t value = 0;
      if (!SafeReadGuest32(rc.memory, ptr + i * 4, &value)) {
        XELOGI("        [{}] unreadable entry at {:08X}", i, ptr + i * 4);
        break;
      }
      XELOGI("        [{}] {}", i,
             pointers ? DescribeDispatcher(rc, value)
                      : DescribeHandle(rc, value));
    }
  };
  if (!std::strcmp(name, "KeWaitForSingleObject")) {
    XELOGI("      waits on {}, {}", DescribeDispatcher(rc, a[0]),
           DescribeTimeout(rc, a[4]));
  } else if (!std::strcmp(name, "NtWaitForSingleObjectEx")) {
    XELOGI("      waits on {}, {}", DescribeHandle(rc, a[0]),
           DescribeTimeout(rc, a[3]));
  } else if (!std::strcmp(name, "KeWaitForMultipleObjects")) {
    XELOGI("      waits for {} of {} objects, {}:",
           a[2] ? "any" : "all", a[0], DescribeTimeout(rc, a[6]));
    handle_array(a[0], a[1], true);
  } else if (!std::strcmp(name, "NtWaitForMultipleObjectsEx")) {
    XELOGI("      waits for {} of {} handles, {}:", a[2] ? "any" : "all",
           a[0], DescribeTimeout(rc, a[5]));
    handle_array(a[0], a[1], false);
  } else if (!std::strcmp(name, "NtSignalAndWaitForSingleObjectEx")) {
    XELOGI("      signalled {:08X} and waits on {}, {}", a[0],
           DescribeHandle(rc, a[1]), DescribeTimeout(rc, a[4]));
  } else if (!std::strcmp(name, "KeDelayExecutionThread")) {
    XELOGI("      sleeps, interval {}", DescribeTimeout(rc, a[2]));
  } else if (!std::strcmp(name, "RtlEnterCriticalSection")) {
    // X_RTL_CRITICAL_SECTION: dispatcher header, lock_count at 0x10,
    // recursion_count at 0x14, owning KTHREAD at 0x18.
    uint8_t cs[28];
    if (!SafeReadGuest(rc.memory, a[0], cs, sizeof(cs))) {
      XELOGI("      enters critical section {:08X} (unreadable)", a[0]);
    } else {
      uint32_t owner = xe::load_and_swap<uint32_t>(cs + 0x18);
      auto it = rc.tid_by_kthread.find(owner);
      XELOGI(
          "      enters critical section {:08X}: owner KTHREAD {:08X} ({}), "
          "lock_count={} recursion={}",
          a[0], owner,
          it != rc.tid_by_kthread.end()
              ? fmt::format("tid {:08X}", it->second)
              : std::string(owner ? "not a known thread" : "none"),
          int32_t(xe::load_and_swap<uint32_t>(cs + 0x10)),
          int32_t(xe::load_and_swap<uint32_t>(cs + 0x14)));
    }
  } else {
    XELOGI(
        "      not a wait the watchdog decodes; args r3..r8 = {:08X} {:08X} "
        "{:08X} {:08X} {:08X} {:08X}",
        a[0], a[1], a[2], a[3], a[4], a[5]);
  }
}

void ReportThread(ReportContext& rc, XThread* thread, uint32_t swap_tid) {
  KernelCallTrace& trace = thread->kernel_call_trace();
  const uint32_t count = trace.count.load(std::memory_order_acquire);
  const uint64_t activity =
      trace.last_activity_tick.load(std::memory_order_relaxed);

  uint32_t suspend_count = 0;
  if (thread->guest_object()) {
    suspend_count = thread->guest_object<X_KTHREAD>()->suspend_count;
  }
  XELOGI(
      "  thread tid={:08X} handle={:08X} '{}'{}{}{} prio={} "
      "suspend_count={} kernel_calls={} last_kernel_activity={:.1f} s ago",
      thread->thread_id(), thread->handle(), thread->thread_name(),
      thread->thread_id() == swap_tid ? " [swap thread]" : "",
      thread->main_thread() ? " [main]" : "",
      thread->is_host_thread() ? " [host]" : "", thread->priority(),
      suspend_count, count, rc.SecondsAgo(activity));

  // State, as the cooperative scheduler sees it (racy, no lock taken).
  std::string state;
  if (GuestScheduler::enabled()) {
    auto& links = thread->scheduler_links();
    if (links.running) {
      state = "running";
    } else if (links.blocked) {
      state = fmt::format("waiting (parked, {}",
                          CooperativeWaitKindName(links.wait_kind));
      uint32_t shown = std::min<uint32_t>(links.wait_handle_count, 8);
      for (uint32_t i = 0; i < shown; ++i) {
        state += fmt::format("{}{:08X}", i ? "," : " handles=",
                             links.wait_handles[i]);
      }
      state += ")";
    } else if (links.queued) {
      state = "ready";
    } else if (links.suspended) {
      state = "suspended";
    } else {
      state = "not on a scheduler list";
    }
  } else {
    state = "unknown (host-thread model, no scheduler state)";
  }
  if (!thread->is_running()) {
    state += ", not running (exited or not started)";
  }

  // The newest record says whether the thread is inside a kernel call.
  KernelCallRecord last = {};
  bool have_last = false;
  if (count) {
    last = trace.records[(count - 1) % KernelCallTrace::kSize];
    have_last = last.seq == count;
  }
  if (have_last && !last.returned) {
    state += fmt::format(", inside {} for {:.1f} s",
                         last.name ? last.name : "?",
                         rc.SecondsAgo(last.host_tick));
  } else if (have_last) {
    state += ", in guest code since its last kernel call returned";
  }
  XELOGI("    state: {}", state);

  if (auto* thread_state = thread->thread_state()) {
    auto* context = thread_state->context();
    // Racy for a thread that is moving. The guest PC is only known as the
    // last JIT safepoint, or as the LR of the kernel call it is in.
    XELOGI(
        "    pc: last_safepoint={:08X} lr={:08X} r1={:08X}{}",
        uint32_t(context->last_safepoint_pc), uint32_t(context->lr),
        uint32_t(context->r[1]),
        have_last && !last.returned
            ? fmt::format(" (in a kernel call: guest PC is its return "
                          "address {:08X})",
                          last.lr)
            : std::string());
    // Guest call chain from the PPC back chain: 0(r1) is the caller's frame
    // and the saved LR sits 8 below it (as GuestScheduler's dump walks it).
    std::string chain;
    uint32_t sp = uint32_t(context->r[1]);
    for (int depth = 0; depth < 16; ++depth) {
      if (!sp || (sp & 3)) {
        break;
      }
      uint32_t next = 0;
      if (!ReadStack32(rc.memory, thread, sp, &next)) {
        break;
      }
      if (next <= sp || (next & 3)) {
        break;
      }
      uint32_t ret = 0;
      if (!ReadStack32(rc.memory, thread, next - 8, &ret)) {
        break;
      }
      chain += fmt::format(" {:08X}", ret);
      sp = next;
    }
    XELOGI("    stack:{}", chain.empty() ? " (none readable)" : chain);
  }

  if (have_last && !last.returned) {
    DescribeInFlightWait(rc, last);
  }

  // The last kernel calls, oldest first.
  if (count) {
    XELOGI("    last kernel calls (oldest first):");
    uint32_t first = count > KernelCallTrace::kSize
                         ? count - KernelCallTrace::kSize + 1
                         : 1;
    for (uint32_t seq = first; seq <= count; ++seq) {
      KernelCallRecord rec = trace.records[(seq - 1) % KernelCallTrace::kSize];
      if (rec.seq != seq) {
        XELOGI("      #{} (being rewritten)", seq);
        continue;
      }
      XELOGI(
          "      #{} frame {} {:.2f} s ago {} lr={:08X} args={:08X} {:08X} "
          "{:08X} {:08X} -> {}",
          seq, rec.frame, rc.SecondsAgo(rec.host_tick),
          rec.name ? rec.name : "?", rec.lr, rec.args[0], rec.args[1],
          rec.args[2], rec.args[3],
          rec.returned ? fmt::format("{:08X}", rec.result)
                       : std::string("(not returned)"));
    }
  }
}

void Watchdog::Loop() {
  last_frame = xe::logging::GetFrameNumber();
  last_frame_tick = Clock::QueryHostTickCount();
  while (!stop.load()) {
    xe::threading::Wait(wake.get(), false, std::chrono::milliseconds(1000));
    if (stop.load()) {
      break;
    }
    Tick();
  }
}

void Watchdog::Tick() {
  const uint64_t freq = Clock::QueryHostTickFrequency();
  const uint64_t now = Clock::QueryHostTickCount();
  if (!freq) {
    return;
  }
  uint32_t seconds = cvars::stall_watchdog_seconds;
  if (seconds < 2) {
    seconds = 2;
  }
  const uint64_t window = uint64_t(seconds) * freq;

  // Frames still presented: the counter moved in the last two seconds.
  const uint32_t frame = xe::logging::GetFrameNumber();
  if (frame != last_frame) {
    last_frame = frame;
    last_frame_tick = now;
  }
  const bool presenting = now - last_frame_tick < 2 * freq;

  auto threads =
      kernel_state->object_table()->GetObjectsByType<XThread>();

  // Learn VdSwap's name pointer from any ring that holds the call, so the
  // trampoline can note the swap thread with a pointer compare.
  if (!g_swap_name.load(std::memory_order_relaxed)) {
    for (auto& thread : threads) {
      auto& trace = thread->kernel_call_trace();
      for (uint32_t i = 0; i < KernelCallTrace::kSize; ++i) {
        const char* name = trace.records[i].name;
        if (name && !std::strcmp(name, "VdSwap")) {
          g_swap_name.store(name, std::memory_order_relaxed);
          g_swap_thread_id.store(thread->thread_id(),
                                 std::memory_order_relaxed);
          break;
        }
      }
      if (g_swap_name.load(std::memory_order_relaxed)) {
        break;
      }
    }
  }
  const uint32_t swap_tid = g_swap_thread_id.load(std::memory_order_relaxed);

  // The watched threads: guest-code threads that have made a kernel call,
  // other than the one presenting frames (or, before VdSwap has been seen,
  // the title's main thread). Host threads (audio, XMA and the like, which
  // run guest callbacks) are left out: they keep going in a stalled game.
  size_t watched = 0;
  bool all_quiet = true;
  bool any_resumed = false;
  for (auto& thread : threads) {
    if (thread->is_host_thread() || !thread->is_running()) {
      continue;
    }
    if (swap_tid ? thread->thread_id() == swap_tid : thread->main_thread()) {
      continue;
    }
    auto& trace = thread->kernel_call_trace();
    if (!trace.count.load(std::memory_order_relaxed)) {
      continue;
    }
    ++watched;
    uint64_t activity =
        trace.last_activity_tick.load(std::memory_order_relaxed);
    if (activity > now || now - activity < window) {
      all_quiet = false;
    }
    if (reported && activity > reported_tick) {
      any_resumed = true;
    }
  }

  if (reported) {
    // Re-armed only once a watched thread moves again.
    if (any_resumed) {
      XELOGI("STALL WATCHDOG: guest threads are making kernel calls again "
             "(frame {})",
             frame);
      reported = false;
    }
    return;
  }
  if (watched && all_quiet && presenting) {
    reported = true;
    reported_tick = now;
    Report(threads, swap_tid, now, freq, seconds);
  }
}

void Watchdog::Report(const std::vector<object_ref<XThread>>& threads,
                      uint32_t swap_tid, uint64_t now, uint64_t freq,
                      uint32_t seconds) {
  ReportContext rc{kernel_state, kernel_state->memory(), now, freq};
  for (auto& thread : threads) {
    rc.tid_by_handle[thread->handle()] = thread->thread_id();
    if (thread->guest_object()) {
      rc.tid_by_kthread[thread->guest_object()] = thread->thread_id();
    }
  }
  for (const auto& rec : XObject::RecentCooperativeSignals(1024)) {
    rc.last_signal[rec.handle] = rec;  // oldest first, so the latest wins
  }

  XELOGE(
      "STALL REPORT (frame {}): frames are still presented, but no guest "
      "thread other than the {} has made or returned from a kernel call for "
      "{} s or more. {} threads below. Everything is read racily from live "
      "threads; a guest PC is only the last JIT safepoint or the return "
      "address of the kernel call a thread is in.",
      xe::logging::GetFrameNumber(),
      swap_tid ? fmt::format("swap thread (tid {:08X}, last to call VdSwap)",
                             swap_tid)
               : std::string("main thread (no VdSwap seen yet)"),
      seconds, threads.size());
  // The watched threads first, then the swap thread and host threads.
  for (int pass = 0; pass < 2; ++pass) {
    for (auto& thread : threads) {
      bool watched = !thread->is_host_thread() &&
                     thread->thread_id() != swap_tid;
      if (watched == (pass == 0)) {
        ReportThread(rc, thread.get(), swap_tid);
      }
    }
  }
  XELOGE("STALL REPORT end (frame {})", xe::logging::GetFrameNumber());
}

}  // namespace

void StallWatchdogStart(KernelState* kernel_state) {
  StallWatchdogStop();
  if (!cvars::stall_watchdog) {
    return;
  }
  g_kernel_call_trace_enabled = true;
  auto* watchdog = new Watchdog();
  watchdog->kernel_state = kernel_state;
  watchdog->wake = xe::threading::Event::CreateAutoResetEvent(false);
  xe::threading::Thread::CreationParameters params;
  watchdog->thread = xe::threading::Thread::Create(
      params, [watchdog]() { watchdog->Loop(); });
  if (!watchdog->thread) {
    delete watchdog;
    return;
  }
  watchdog->thread->set_name("Stall Watchdog");
  g_watchdog = watchdog;
}

void StallWatchdogStop() {
  g_kernel_call_trace_enabled = false;
  Watchdog* watchdog = g_watchdog;
  if (!watchdog) {
    return;
  }
  g_watchdog = nullptr;
  watchdog->stop.store(true);
  watchdog->wake->Set();
  xe::threading::Wait(watchdog->thread.get(), false);
  delete watchdog;
}

}  // namespace kernel
}  // namespace xe
