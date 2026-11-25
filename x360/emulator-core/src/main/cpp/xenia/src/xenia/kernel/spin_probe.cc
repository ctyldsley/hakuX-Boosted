/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * hakuX-Boosted: spin probe.                                                 *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// hakuX-Boosted: the spin probe. See spin_probe.h for what it does.
//
// The tables are fixed, open-addressed and written without a lock. Every key
// holds a guest thread id, and a guest thread runs on one host thread at a
// time, so each slot has one writer once claimed; claiming is a CAS. The
// report reads them after recording stops (a writer still inside a note at
// that moment can leave one count off, which does not matter here).

#include "xenia/kernel/spin_probe.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

#if XE_PLATFORM_LINUX
#include <fcntl.h>
#include <unistd.h>
#endif

DEFINE_uint32(
    spin_probe_seconds, 3,
    "hakuX-Boosted: how long the spin probe records once requested (create a "
    "file called spin_probe beside the log file). Per running guest thread: "
    "the guest addresses of the safepoints it reaches, its kernel calls per "
    "call site with arguments and results, and the loads of the loop it "
    "stands at with their addresses and values. Nothing is recorded until "
    "the file appears.",
    "Kernel");

namespace xe {
namespace kernel {

std::atomic<bool> g_spin_probe_active{false};

namespace {

constexpr uint32_t kPcSlots = 4096;
constexpr uint32_t kCallSlots = 4096;
constexpr uint32_t kLoadSlots = 2048;
constexpr uint32_t kCallInstances = 4;
constexpr uint32_t kLoadValues = 4;
// Guest instructions decoded from a sampled safepoint looking for the loop's
// back edge.
constexpr uint32_t kScanInstructions = 64;

struct PcSlot {
  std::atomic<uint64_t> key{0};  // thread_id << 32 | pc
  std::atomic<uint32_t> count{0};
  uint32_t lr = 0;  // guest LR at the first sample
};

struct CallSlot {
  std::atomic<uint32_t> tid{0};  // 0 = free; claimed by CAS
  uint32_t site = 0;             // guest LR: the return address of the call
  const char* name = nullptr;
  std::atomic<uint32_t> count{0};
  uint32_t args[kCallInstances][6] = {};
  uint32_t result[kCallInstances] = {};
};

struct LoadSlot {
  std::atomic<uint64_t> key{0};  // thread_id << 32 | guest address of the load
  std::atomic<uint32_t> count{0};
  uint32_t insn = 0;
  uint32_t loop_pc = 0;     // the safepoint the scan started at
  uint32_t first_ea = 0;
  uint32_t last_ea = 0;
  uint32_t ea_changes = 0;  // reads whose address differed from the last
  uint32_t unreadable = 0;  // reads that returned EFAULT
  uint32_t guessed = 0;     // base register written earlier in the loop
  uint32_t value_count = 0; // distinct values kept, up to kLoadValues
  uint32_t more_values = 0; // distinct values seen past those kept
  uint64_t last_value = 0;
  uint64_t values[kLoadValues] = {};
};

struct Tables {
  PcSlot pcs[kPcSlots];
  CallSlot calls[kCallSlots];
  LoadSlot loads[kLoadSlots];
  std::atomic<uint32_t> samples_unknown_pc{0};
  std::atomic<uint32_t> dropped{0};
};

Tables* g_tables = nullptr;

inline uint32_t Mix(uint64_t key) {
  key ^= key >> 33;
  key *= 0xFF51AFD7ED558CCDull;
  key ^= key >> 33;
  key *= 0xC4CEB9FE1A85EC53ull;
  key ^= key >> 33;
  return uint32_t(key);
}

template <typename Slot, uint32_t kSlots>
Slot* FindKeyed(Slot (&slots)[kSlots], uint64_t key) {
  uint32_t index = Mix(key) & (kSlots - 1);
  for (uint32_t probe = 0; probe < 64; ++probe) {
    Slot& slot = slots[(index + probe) & (kSlots - 1)];
    uint64_t current = slot.key.load(std::memory_order_acquire);
    if (current == key) {
      return &slot;
    }
    if (!current) {
      uint64_t expected = 0;
      if (slot.key.compare_exchange_strong(expected, key,
                                           std::memory_order_acq_rel) ||
          expected == key) {
        return &slot;
      }
    }
  }
  return nullptr;
}

CallSlot* FindCall(Tables& t, uint32_t tid, uint32_t site, const char* name) {
  uint64_t hash_key = (uint64_t(tid) << 32 | site) ^ uint64_t(uintptr_t(name));
  uint32_t index = Mix(hash_key) & (kCallSlots - 1);
  for (uint32_t probe = 0; probe < 64; ++probe) {
    CallSlot& slot = t.calls[(index + probe) & (kCallSlots - 1)];
    uint32_t owner = slot.tid.load(std::memory_order_acquire);
    if (owner == tid) {
      // Written by this same guest thread, so plain reads are current.
      if (slot.site == site && slot.name == name) {
        return &slot;
      }
      continue;
    }
    if (!owner) {
      uint32_t expected = 0;
      if (slot.tid.compare_exchange_strong(expected, tid,
                                           std::memory_order_acq_rel)) {
        slot.site = site;
        slot.name = name;
        return &slot;
      }
      if (expected == tid && slot.site == site && slot.name == name) {
        return &slot;
      }
    }
  }
  return nullptr;
}

// Copies up to n bytes of host memory that may not be mapped. Returns how
// many were readable from the start. A write() of the range into a pipe
// fails with EFAULT (or stops short) where a plain load would fault.
std::mutex g_pipe_mutex;
int g_pipe[2] = {-1, -1};

size_t SafeRead(const void* src, void* dst, size_t n) {
#if XE_PLATFORM_LINUX
  std::lock_guard<std::mutex> lock(g_pipe_mutex);
  if (g_pipe[0] < 0) {
    if (pipe2(g_pipe, O_NONBLOCK | O_CLOEXEC) != 0) {
      g_pipe[0] = g_pipe[1] = -1;
      return 0;
    }
  }
  ssize_t written = write(g_pipe[1], src, n);
  if (written <= 0) {
    return 0;
  }
  ssize_t got = read(g_pipe[0], dst, size_t(written));
  return got > 0 ? size_t(got) : 0;
#else
  return 0;
#endif
}

// What a guest integer load is: its size, whether it is indexed (X-form),
// whether its value is byte-reversed, and its mnemonic for the report.
struct LoadKind {
  const char* name;
  uint32_t size;
  bool indexed;
  bool reversed;
  int32_t displacement;
};

bool DecodeLoad(uint32_t insn, LoadKind* out) {
  const uint32_t op = insn >> 26;
  const int32_t d = int16_t(insn & 0xFFFF);
  switch (op) {
    case 32: *out = {"lwz", 4, false, false, d}; return true;
    case 33: *out = {"lwzu", 4, false, false, d}; return true;
    case 34: *out = {"lbz", 1, false, false, d}; return true;
    case 35: *out = {"lbzu", 1, false, false, d}; return true;
    case 40: *out = {"lhz", 2, false, false, d}; return true;
    case 41: *out = {"lhzu", 2, false, false, d}; return true;
    case 42: *out = {"lha", 2, false, false, d}; return true;
    case 43: *out = {"lhau", 2, false, false, d}; return true;
    case 58: {
      const int32_t ds = int16_t(insn & 0xFFFC);
      switch (insn & 3) {
        case 0: *out = {"ld", 8, false, false, ds}; return true;
        case 1: *out = {"ldu", 8, false, false, ds}; return true;
        case 2: *out = {"lwa", 4, false, false, ds}; return true;
      }
      return false;
    }
    case 31: {
      switch ((insn >> 1) & 0x3FF) {
        case 23: *out = {"lwzx", 4, true, false, 0}; return true;
        case 55: *out = {"lwzux", 4, true, false, 0}; return true;
        case 87: *out = {"lbzx", 1, true, false, 0}; return true;
        case 119: *out = {"lbzux", 1, true, false, 0}; return true;
        case 279: *out = {"lhzx", 2, true, false, 0}; return true;
        case 311: *out = {"lhzux", 2, true, false, 0}; return true;
        case 343: *out = {"lhax", 2, true, false, 0}; return true;
        case 375: *out = {"lhaux", 2, true, false, 0}; return true;
        case 341: *out = {"lwax", 4, true, false, 0}; return true;
        case 373: *out = {"lwaux", 4, true, false, 0}; return true;
        case 21: *out = {"ldx", 8, true, false, 0}; return true;
        case 53: *out = {"ldux", 8, true, false, 0}; return true;
        case 20: *out = {"lwarx", 4, true, false, 0}; return true;
        case 84: *out = {"ldarx", 8, true, false, 0}; return true;
        case 534: *out = {"lwbrx", 4, true, true, 0}; return true;
        case 790: *out = {"lhbrx", 2, true, true, 0}; return true;
        case 532: *out = {"ldbrx", 8, true, true, 0}; return true;
      }
      return false;
    }
  }
  return false;
}

// The general-purpose registers an instruction may write, as a mask. Errs
// on the side of more: a load whose base is in the mask is flagged as a
// guess in the report, nothing worse.
uint32_t WrittenGprs(uint32_t insn) {
  const uint32_t op = insn >> 26;
  const uint32_t rd = 1u << ((insn >> 21) & 31);
  const uint32_t ra = 1u << ((insn >> 16) & 31);
  switch (op) {
    case 2: case 3:                      // tdi, twi
    case 10: case 11:                    // cmpli, cmpi
    case 16: case 17: case 18: case 19:  // branches, sc, CR ops
    case 4: case 59: case 63:            // vector, float
    case 36: case 38: case 44:           // stw, stb, sth
    case 52: case 54:                    // stfs, stfd
    case 50: case 48:                    // lfd, lfs
      return 0;
    case 37: case 39: case 45:           // stwu, stbu, sthu
    case 53: case 55: case 49: case 51:  // stfsu, stfdu, lfsu, lfdu
      return ra;
    case 62:                             // std, stdu
      return (insn & 3) == 1 ? ra : 0;
    case 20: case 21: case 23: case 24: case 25: case 26: case 27:
    case 28: case 29: case 30:           // rotates and immediate logic
      return ra;
    case 33: case 35: case 41: case 43:  // update loads
      return rd | ra;
    case 58:
      return (insn & 3) == 1 ? (rd | ra) : rd;
    case 31: {
      switch ((insn >> 1) & 0x3FF) {
        case 0: case 32: case 4: case 68:          // cmp, cmpl, tw, td
        case 151: case 215: case 407: case 149:    // stwx stbx sthx stdx
        case 150: case 214: case 662: case 918:    // stwcx stdcx stwbrx sthbrx
        case 660:                                  // stdbrx
        case 278: case 246: case 54: case 86:      // dcbt dcbtst dcbst dcbf
        case 1014: case 982: case 598: case 854:   // dcbz icbi sync eieio
        case 467: case 144: case 146: case 178:    // mtspr mtcrf mtmsr mtmsrd
        case 663: case 727: case 695: case 759:    // float/vector stores
        case 135: case 167: case 199: case 231:
        case 487: case 519: case 551: case 583:    // vector element loads
        case 103: case 7: case 39: case 71:
        case 6: case 38: case 535: case 599:      // lvsl lvsr lfsx lfdx
          return 0;
        case 183: case 247: case 439: case 181:    // update stores
          return ra;
        case 28: case 60: case 444: case 124: case 316: case 284:
        case 412: case 476: case 24: case 536: case 792: case 824:
        case 27: case 539: case 794: case 826: case 827: case 26:
        case 58: case 922: case 954: case 986:     // logic, shifts, extends
          return ra;
        case 55: case 119: case 311: case 375: case 53: case 373:
          return rd | ra;                          // update loads
      }
      return rd | ra;
    }
  }
  return rd;
}

void NoteLoad(Tables& t, uint32_t tid, uint32_t loop_pc, uint32_t addr,
              uint32_t insn, uint32_t ea, bool guessed, bool readable,
              uint64_t value) {
  LoadSlot* slot = FindKeyed(t.loads, uint64_t(tid) << 32 | addr);
  if (!slot) {
    t.dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t n = slot->count.fetch_add(1, std::memory_order_relaxed);
  if (!n) {
    slot->insn = insn;
    slot->loop_pc = loop_pc;
    slot->first_ea = ea;
    slot->last_ea = ea;
  } else if (ea != slot->last_ea) {
    ++slot->ea_changes;
    slot->last_ea = ea;
  }
  if (guessed) {
    slot->guessed = 1;
  }
  if (!readable) {
    ++slot->unreadable;
    return;
  }
  slot->last_value = value;
  for (uint32_t i = 0; i < slot->value_count; ++i) {
    if (slot->values[i] == value) {
      return;
    }
  }
  if (slot->value_count < kLoadValues) {
    slot->values[slot->value_count++] = value;
  } else {
    ++slot->more_values;
  }
}

// Decodes the guest code from the safepoint at pc to the loop's back edge
// (or the first unconditional way out) and reads every integer load's
// address as the registers stand now. The safepoint is a block head where
// the context holds every guest register, so for a load whose base register
// nothing earlier in the scan writes the address is exact.
void ScanLoads(Tables& t, Memory* memory, uint32_t tid, uint32_t pc,
               const cpu::ppc::PPCContext* context) {
  uint32_t words[kScanInstructions];
  const size_t got =
      SafeRead(memory->TranslateVirtual<const uint8_t*>(pc), words,
               sizeof(words)) /
      4;
  uint32_t dirty = 0;
  for (size_t i = 0; i < got; ++i) {
    const uint32_t insn = xe::byte_swap(words[i]);
    const uint32_t addr = pc + uint32_t(i) * 4;
    const uint32_t op = insn >> 26;
    if (op == 18) {  // b, bl: unconditional
      if (insn & 1) {
        // bl: the callee may write any volatile register.
        dirty |= 0x00001FF9u;  // r0, r3..r12
        continue;
      }
      break;
    }
    if (op == 17) {  // sc
      break;
    }
    if (op == 16) {  // bc
      const uint32_t bo = (insn >> 21) & 31;
      const int32_t bd = int16_t(insn & 0xFFFC);
      const uint32_t target = (insn & 2) ? uint32_t(bd) : addr + bd;
      if (insn & 1) {
        dirty |= 0x00001FF9u;
        continue;
      }
      if ((bo & 0x14) == 0x14 || target <= pc) {
        break;  // always taken, or the back edge
      }
      continue;
    }
    if (op == 19) {
      const uint32_t xo = (insn >> 1) & 0x3FF;
      if (xo == 16 || xo == 528) {  // bclr, bcctr
        if (insn & 1) {
          dirty |= 0x00001FF9u;
          continue;
        }
        if ((((insn >> 21) & 31) & 0x14) == 0x14) {
          break;
        }
      }
      continue;
    }
    LoadKind kind;
    if (DecodeLoad(insn, &kind)) {
      const uint32_t ra = (insn >> 16) & 31;
      const uint32_t rb = (insn >> 11) & 31;
      uint32_t ea = ra ? uint32_t(context->r[ra]) : 0;
      bool guessed = ra && (dirty & (1u << ra));
      if (kind.indexed) {
        ea += uint32_t(context->r[rb]);
        guessed = guessed || (dirty & (1u << rb));
      } else {
        ea += uint32_t(kind.displacement);
      }
      uint8_t bytes[8] = {};
      const bool readable =
          SafeRead(memory->TranslateVirtual<const uint8_t*>(ea), bytes,
                   kind.size) == kind.size;
      uint64_t value = 0;
      if (readable) {
        for (uint32_t b = 0; b < kind.size; ++b) {
          const uint32_t from = kind.reversed ? kind.size - 1 - b : b;
          value = (value << 8) | bytes[from];
        }
      }
      NoteLoad(t, tid, pc, addr, insn, ea, guessed, readable, value);
    }
    dirty |= WrittenGprs(insn);
  }
}

std::string LoadName(uint32_t insn) {
  LoadKind kind;
  if (!DecodeLoad(insn, &kind)) {
    return "load";
  }
  const uint32_t rd = (insn >> 21) & 31;
  const uint32_t ra = (insn >> 16) & 31;
  if (kind.indexed) {
    return fmt::format("{} r{},r{},r{}", kind.name, rd, ra, (insn >> 11) & 31);
  }
  return fmt::format("{} r{},{}{:X}(r{})", kind.name, rd,
                     kind.displacement < 0 ? "-0x" : "0x",
                     kind.displacement < 0 ? -kind.displacement
                                           : kind.displacement,
                     ra);
}

std::string FormatValue(uint64_t value, uint32_t size) {
  switch (size) {
    case 1: return fmt::format("{:02X}", uint32_t(value));
    case 2: return fmt::format("{:04X}", uint32_t(value));
    case 8: return fmt::format("{:016X}", value);
    default: return fmt::format("{:08X}", uint32_t(value));
  }
}

}  // namespace

void SpinProbeNoteCall(uint32_t thread_id, const char* name, uint32_t lr,
                       const uint32_t* args, uint32_t result) {
  Tables* t = g_tables;
  if (!t || !g_spin_probe_active.load(std::memory_order_relaxed)) {
    return;
  }
  CallSlot* slot = FindCall(*t, thread_id, lr, name);
  if (!slot) {
    t->dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t n = slot->count.fetch_add(1, std::memory_order_relaxed);
  if (n < kCallInstances) {
    std::memcpy(slot->args[n], args, sizeof(slot->args[n]));
    slot->result[n] = result;
  }
}

void SpinProbeNoteSafepoint(KernelState* kernel_state, XThread* self) {
  Tables* t = g_tables;
  if (!t || !g_spin_probe_active.load(std::memory_order_relaxed)) {
    return;
  }
  const auto* context = self->thread_state()->context();
  const uint32_t pc = context->last_safepoint_pc;
  const uint32_t tid = self->thread_id();
  if (!pc) {
    t->samples_unknown_pc.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  PcSlot* slot = FindKeyed(t->pcs, uint64_t(tid) << 32 | pc);
  if (!slot) {
    t->dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint32_t n = slot->count.fetch_add(1, std::memory_order_relaxed);
  if (!n) {
    slot->lr = uint32_t(context->lr);
  }
  // The loads are read at the first sample of a site and every fourth
  // after, enough to see a value change without a syscall per sample.
  if ((n & 3) == 0) {
    ScanLoads(*t, kernel_state->memory(), tid, pc, context);
  }
}

void SpinProbeBegin() {
  if (!g_tables) {
    g_tables = new Tables();
  } else {
    // Recording is off and the previous window's report is written: no
    // writer is left (one still inside a note would only add a count).
    Tables& t = *g_tables;
    for (auto& s : t.pcs) {
      s.key.store(0, std::memory_order_relaxed);
      s.count.store(0, std::memory_order_relaxed);
      s.lr = 0;
    }
    for (auto& s : t.calls) {
      s.tid.store(0, std::memory_order_relaxed);
      s.count.store(0, std::memory_order_relaxed);
      s.site = 0;
      s.name = nullptr;
    }
    for (auto& s : t.loads) {
      s.key.store(0, std::memory_order_relaxed);
      s.count.store(0, std::memory_order_relaxed);
      s.ea_changes = s.unreadable = s.guessed = 0;
      s.value_count = s.more_values = 0;
    }
    t.samples_unknown_pc.store(0, std::memory_order_relaxed);
    t.dropped.store(0, std::memory_order_relaxed);
  }
  std::atomic_thread_fence(std::memory_order_seq_cst);
  g_spin_probe_active.store(true, std::memory_order_release);
}

void SpinProbeEnd(KernelState* kernel_state, double seconds,
                  uint32_t watchdog_period_us) {
  g_spin_probe_active.store(false, std::memory_order_release);
  Tables* t = g_tables;
  if (!t) {
    return;
  }
  if (seconds <= 0.0) {
    seconds = 1.0;
  }

  struct ThreadSummary {
    uint32_t tid = 0;
    uint64_t samples = 0;
    uint64_t calls = 0;
    std::vector<const PcSlot*> pcs;
    std::vector<const CallSlot*> calls_by_site;
    std::vector<const LoadSlot*> loads;
  };
  std::unordered_map<uint32_t, ThreadSummary> threads;
  for (const auto& s : t->pcs) {
    const uint64_t key = s.key.load(std::memory_order_acquire);
    const uint32_t count = s.count.load(std::memory_order_relaxed);
    if (!key || !count) continue;
    auto& summary = threads[uint32_t(key >> 32)];
    summary.samples += count;
    summary.pcs.push_back(&s);
  }
  for (const auto& s : t->calls) {
    const uint32_t tid = s.tid.load(std::memory_order_acquire);
    const uint32_t count = s.count.load(std::memory_order_relaxed);
    if (!tid || !count) continue;
    auto& summary = threads[tid];
    summary.calls += count;
    summary.calls_by_site.push_back(&s);
  }
  for (const auto& s : t->loads) {
    const uint64_t key = s.key.load(std::memory_order_acquire);
    if (!key || !s.count.load(std::memory_order_relaxed)) continue;
    threads[uint32_t(key >> 32)].loads.push_back(&s);
  }

  // Names, looked up now (outside the scheduler's lock).
  std::unordered_map<uint32_t, std::string> names;
  if (kernel_state) {
    for (auto& thread :
         kernel_state->object_table()->GetObjectsByType<XThread>()) {
      names[thread->thread_id()] = thread->thread_name();
    }
  }

  std::vector<ThreadSummary*> order;
  for (auto& [tid, summary] : threads) {
    summary.tid = tid;
    order.push_back(&summary);
  }
  std::sort(order.begin(), order.end(),
            [](const ThreadSummary* a, const ThreadSummary* b) {
              if (a->samples != b->samples) return a->samples > b->samples;
              return a->calls > b->calls;
            });

  XELOGW(
      "SpinProbe: {:.1f} s recorded (frame {}), a sample per running thread "
      "every {} us. Per guest thread: safepoints reached (guest address, "
      "count, LR at the first), kernel calls per call site (LR = return "
      "address; first calls as (r3..r8)=result), and the loads of the loop "
      "at each top site (address read as the registers stood at the "
      "safepoint; '?' = base register written earlier in the loop, so a "
      "guess). Samples with no address: {}; entries dropped (table full): {}",
      seconds, xe::logging::GetFrameNumber(), watchdog_period_us,
      t->samples_unknown_pc.load(), t->dropped.load());

  constexpr size_t kMaxThreads = 24;
  constexpr size_t kMaxPcs = 8;
  constexpr size_t kMaxSites = 16;
  constexpr size_t kLoadSites = 3;
  constexpr size_t kMaxLoadsPerSite = 12;
  for (size_t ti = 0; ti < order.size() && ti < kMaxThreads; ++ti) {
    ThreadSummary& s = *order[ti];
    auto name_it = names.find(s.tid);
    XELOGW(
        "  tid={:08X} '{}': {} safepoint samples, {} kernel calls ({:.0f}/s)",
        s.tid, name_it != names.end() ? name_it->second : std::string("?"),
        s.samples, s.calls, double(s.calls) / seconds);
    std::sort(s.pcs.begin(), s.pcs.end(), [](const PcSlot* a, const PcSlot* b) {
      return a->count.load() > b->count.load();
    });
    for (size_t i = 0; i < s.pcs.size() && i < kMaxPcs; ++i) {
      const PcSlot& p = *s.pcs[i];
      const uint32_t count = p.count.load();
      XELOGW("    at {:08X}: {} ({:.1f}%), lr {:08X}",
             uint32_t(p.key.load()), count,
             s.samples ? 100.0 * count / double(s.samples) : 0.0, p.lr);
    }
    if (s.pcs.size() > kMaxPcs) {
      XELOGW("    ... {} more sites", s.pcs.size() - kMaxPcs);
    }
    std::sort(s.calls_by_site.begin(), s.calls_by_site.end(),
              [](const CallSlot* a, const CallSlot* b) {
                return a->count.load() > b->count.load();
              });
    for (size_t i = 0; i < s.calls_by_site.size() && i < kMaxSites; ++i) {
      const CallSlot& c = *s.calls_by_site[i];
      const uint32_t count = c.count.load();
      std::string first;
      for (uint32_t k = 0; k < count && k < kCallInstances; ++k) {
        first += fmt::format(
            " ({:08X},{:08X},{:08X},{:08X},{:08X},{:08X})={:08X}", c.args[k][0],
            c.args[k][1], c.args[k][2], c.args[k][3], c.args[k][4],
            c.args[k][5], c.result[k]);
      }
      XELOGW("    call {} from {:08X}: {} ({:.0f}/s);{}",
             c.name ? c.name : "?", c.site, count, double(count) / seconds,
             first);
    }
    if (s.calls_by_site.size() > kMaxSites) {
      XELOGW("    ... {} more call sites", s.calls_by_site.size() - kMaxSites);
    }
    for (size_t i = 0; i < s.pcs.size() && i < kLoadSites; ++i) {
      const uint32_t loop_pc = uint32_t(s.pcs[i]->key.load());
      std::vector<const LoadSlot*> loads;
      for (const LoadSlot* l : s.loads) {
        if (l->loop_pc == loop_pc) loads.push_back(l);
      }
      std::sort(loads.begin(), loads.end(),
                [](const LoadSlot* a, const LoadSlot* b) {
                  return uint32_t(a->key.load()) < uint32_t(b->key.load());
                });
      if (loads.empty()) {
        continue;
      }
      XELOGW("    loads of the loop at {:08X}:", loop_pc);
      for (size_t k = 0; k < loads.size() && k < kMaxLoadsPerSite; ++k) {
        const LoadSlot& l = *loads[k];
        LoadKind kind = {"load", 4, false, false, 0};
        DecodeLoad(l.insn, &kind);
        std::string values;
        for (uint32_t v = 0; v < l.value_count; ++v) {
          values += (v ? "," : "") + FormatValue(l.values[v], kind.size);
        }
        if (l.more_values) {
          values += fmt::format(",+{} more", l.more_values);
        }
        const uint32_t count = l.count.load();
        XELOGW(
            "      {:08X} {}{}: [{:08X}]{} = {} (read {}x{}); values {}",
            uint32_t(l.key.load()), LoadName(l.insn), l.guessed ? " ?" : "",
            l.last_ea,
            l.ea_changes
                ? fmt::format(" (address changed {}x, first {:08X})",
                              l.ea_changes, l.first_ea)
                : std::string(),
            l.unreadable == count ? std::string("unreadable")
                                  : FormatValue(l.last_value, kind.size),
            count, l.unreadable ? fmt::format(", {} unreadable", l.unreadable)
                                : std::string(),
            values.empty() ? std::string("none") : values);
      }
    }
  }
  if (order.size() > kMaxThreads) {
    XELOGW("  ... {} more threads", order.size() - kMaxThreads);
  }
  XELOGW("SpinProbe: end of report");
}

}  // namespace kernel
}  // namespace xe
