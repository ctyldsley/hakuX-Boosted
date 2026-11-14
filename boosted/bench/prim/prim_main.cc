// hakuX-Boosted bench: the host primitives the profile of the guest main
// thread names, each timed alone. See boosted/notes/bench-harness.md.
//
// Every operation is one call of a function that is not inlined, so each
// figure includes a call and a return; "call_empty" is that overhead alone.
#include <dlfcn.h>
#include <pthread.h>
#include <sys/auxv.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

#if defined(__GLIBC__)
#include <gnu/libc-version.h>
#elif defined(__BIONIC__)
#include <android/api-level.h>
#endif

#include "bench.h"
#include "insn_counter.h"

// The fixed slot. On bionic it is TLS_SLOT_APP itself, the word at the
// thread pointer plus 16 that Android leaves to apps. glibc has no such
// slot, but the same address is the first word of the executable's own
// static TLS block, so the executable's only thread-local is put there.
// Checked at start-up either way.
#if !defined(__BIONIC__)
__thread void* prim_slot_app;
#endif

extern "C" {
// prim/tls_lib.c, loaded at start-up.
void* prim_tls_static_read(void);
void prim_tls_static_write(void*);
void* prim_tls_static_address(void);

// prim/atomics.cc, three builds.
bool prim_outline_cas16_acquire(std::atomic<uint16_t>*, uint16_t, uint16_t);
uint64_t prim_outline_add64_relaxed(std::atomic<uint64_t>*);
bool prim_lse_cas16_acquire(std::atomic<uint16_t>*, uint16_t, uint16_t);
uint64_t prim_lse_add64_relaxed(std::atomic<uint64_t>*);
bool prim_llsc_cas16_acquire(std::atomic<uint16_t>*, uint16_t, uint16_t);
uint64_t prim_llsc_add64_relaxed(std::atomic<uint64_t>*);

// The project's own context switch: Boost.Context's fcontext assembly, the
// same three files third_party/boost_context builds into the core.
typedef void* fcontext_t;
struct transfer_t {
  fcontext_t fctx;
  void* data;
};
transfer_t jump_fcontext(fcontext_t const to, void* vp);
fcontext_t make_fcontext(void* sp, size_t size, void (*fn)(transfer_t));
}

namespace {

__attribute__((noinline)) void CallEmpty() { __asm__ __volatile__(""); }

__thread void* tls_initial_exec __attribute__((tls_model("initial-exec")));

__attribute__((noinline)) void* ReadInitialExec() { return tls_initial_exec; }

// mrs + one load at a fixed offset: what host_thread_state_slot does.
__attribute__((noinline)) void* ReadFixedSlot() {
  void* value;
  __asm__ __volatile__(
      "mrs %0, tpidr_el0\n\t"
      "ldr %0, [%0, #16]"
      : "=r"(value));
  return value;
}

// A pointer the caller already holds in a register (as translated code
// holds the guest context): one load.
struct HeldState {
  void* pad;
  void* current;
};
__attribute__((noinline)) void* ReadThroughRegister(const HeldState* state) {
  return state->current;
}

pthread_key_t g_key;
__attribute__((noinline)) void* ReadPthreadKey() {
  return pthread_getspecific(g_key);
}

__attribute__((noinline)) uint64_t ReadCounterRegister() {
  uint64_t value;
  __asm__ __volatile__("mrs %0, cntvct_el0" : "=r"(value));
  return value;
}

__attribute__((noinline)) uint64_t ReadClockMonotonic() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_nsec);
}

__attribute__((noinline)) uint64_t ReadSteadyClock() {
  return uint64_t(
      std::chrono::steady_clock::now().time_since_epoch().count());
}

pthread_mutex_t g_pthread_mutex = PTHREAD_MUTEX_INITIALIZER;
std::mutex g_std_mutex;
std::recursive_mutex g_recursive_mutex;

void FiberBody(transfer_t from) {
  for (;;) {
    from = jump_fcontext(from.fctx, nullptr);
  }
}

std::string ReadCpuModel() {
  std::ifstream cpuinfo("/proc/cpuinfo");
  std::string line, implementer, part, model;
  while (std::getline(cpuinfo, line)) {
    auto value = [&line]() {
      auto colon = line.find(':');
      return colon == std::string::npos ? std::string()
                                        : line.substr(colon + 2);
    };
    if (line.rfind("CPU implementer", 0) == 0 && implementer.empty()) {
      implementer = value();
    } else if (line.rfind("CPU part", 0) == 0 && part.empty()) {
      part = value();
    } else if (line.rfind("model name", 0) == 0 && model.empty()) {
      model = value();
    }
  }
  std::string out = "implementer " + implementer + " part " + part;
  if (!model.empty()) {
    out += " (" + model + ")";
  }
  return out;
}

// Where a library's thread-local sits relative to the thread pointer. With
// static TLS the loader gives every thread the same offset; a dynamically
// allocated block comes from the heap, so the offset differs between threads.
struct OffsetProbe {
  void* (*address)();
  intptr_t offset;
};
void* ProbeOffset(void* raw) {
  auto* probe = static_cast<OffsetProbe*>(raw);
  probe->offset =
      reinterpret_cast<intptr_t>(probe->address()) -
      reinterpret_cast<intptr_t>(__builtin_thread_pointer());
  return nullptr;
}
bool LooksStatic(void* (*address)()) {
  OffsetProbe here{address, 0}, there{address, 0};
  ProbeOffset(&here);
  pthread_t thread;
  pthread_create(&thread, nullptr, ProbeOffset, &there);
  pthread_join(thread, nullptr);
  return here.offset == there.offset;
}

}  // namespace

int main(int argc, char** argv) {
  const char* dyn_path = argc > 1 ? argv[1] : "./libprim_tls_dyn.so";
  // The same program is run against bionic in a container: another name
  // for its section keeps the two sets of numbers apart.
  const char* section = std::getenv("BENCH_PRIM_SECTION");
  const std::string s = section ? section : "prim";
  bench::PinToCurrentCpu();

  utsname uts{};
  uname(&uts);
  bench::EmitNote(s, "cpu", ReadCpuModel());
  bench::EmitNote(s, "kernel", std::string(uts.release) + " " + uts.machine);
#if defined(__GLIBC__)
  bench::EmitNote(s, "libc", std::string("glibc ") + gnu_get_libc_version());
#elif defined(__BIONIC__)
  bench::EmitNote(s, "libc",
                  "bionic (Android's libc and linker), API level " +
                      std::to_string(android_get_device_api_level()));
#endif
  const bool has_lse = (getauxval(AT_HWCAP) & HWCAP_ATOMICS) != 0;
  bench::EmitNote(s, "lse_atomics", has_lse ? "yes" : "no");
  bench::EmitNote(s, "cores", std::to_string(sysconf(_SC_NPROCESSORS_ONLN)));

  {
    bench::InsnCounter counter;
    bench::EmitNote(s, "instruction_counter",
                    counter.ok() ? "available"
                                 : "not available (" + counter.why_not() + ")");
  }

  bench::EmitTiming(s, "call_empty", "", bench::Measure([] { CallEmpty(); }));

  // --- Thread-local reads -------------------------------------------------
  void* marker = &g_key;
  {
    void* handle = dlopen(dyn_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
      bench::EmitNote(s, "tls_tlsdesc_dynamic",
                      std::string("not measured: dlopen failed: ") + dlerror());
    } else {
      auto read = reinterpret_cast<void* (*)()>(dlsym(handle, "prim_tls_dyn_read"));
      auto write =
          reinterpret_cast<void (*)(void*)>(dlsym(handle, "prim_tls_dyn_write"));
      auto address =
          reinterpret_cast<void* (*)()>(dlsym(handle, "prim_tls_dyn_address"));
      write(marker);
      const bool is_static = LooksStatic(address);
      bench::EmitNote(s, "tls_tlsdesc_dynamic_resolver",
                      is_static ? "WRONG: the loader gave the dlopen'ed "
                                  "library static TLS, this row is the "
                                  "static resolver"
                                : "dynamic (the block's offset from the thread "
                                  "pointer differs between threads)");
      bench::EmitTiming(s, "tls_read_tlsdesc_dynamic", "", bench::Measure([&] {
        bench::Keep(read());
      }));
      bench::EmitTiming(s, "tls_write_tlsdesc_dynamic", "",
                        bench::Measure([&] { write(marker); }));
    }
  }
  prim_tls_static_write(marker);
  bench::EmitNote(s, "tls_tlsdesc_static_resolver",
                  LooksStatic(prim_tls_static_address)
                      ? "static (the same offset from the thread pointer in every "
                        "thread)"
                      : "WRONG: not in the static TLS area");
  bench::EmitTiming(s, "tls_read_tlsdesc_static", "", bench::Measure([] {
    bench::Keep(prim_tls_static_read());
  }));
  tls_initial_exec = marker;
  bench::EmitTiming(s, "tls_read_initial_exec", "", bench::Measure([] {
    bench::Keep(ReadInitialExec());
  }));
  pthread_key_create(&g_key, nullptr);
  pthread_setspecific(g_key, marker);
  bench::EmitTiming(s, "tls_read_pthread_key", "", bench::Measure([] {
    bench::Keep(ReadPthreadKey());
  }));
#if defined(__BIONIC__)
  bool slot_ok = ReadFixedSlot() == nullptr;  // nobody else may be using it
  if (slot_ok) {
    __asm__ __volatile__(
        "mrs x16, tpidr_el0\n\t"
        "str %0, [x16, #16]"
        :
        : "r"(marker)
        : "x16", "memory");
  }
#else
  prim_slot_app = marker;
#endif
  if (ReadFixedSlot() == marker) {
    bench::EmitTiming(s, "tls_read_fixed_slot", "", bench::Measure([] {
      bench::Keep(ReadFixedSlot());
    }));
  } else {
    bench::EmitNote(s, "tls_read_fixed_slot",
                    "not measured: the word at the thread pointer plus 16 "
                    "is not free on this loader");
  }
  HeldState held{nullptr, marker};
  bench::EmitTiming(s, "tls_read_register_held", "", bench::Measure([&] {
    bench::Keep(ReadThroughRegister(&held));
  }));

  // --- Clocks -------------------------------------------------------------
  bench::EmitTiming(s, "clock_gettime_monotonic", "", bench::Measure([] {
    bench::Keep(ReadClockMonotonic());
  }));
  bench::EmitTiming(s, "clock_steady_clock_now", "", bench::Measure([] {
    bench::Keep(ReadSteadyClock());
  }));
  bench::EmitTiming(s, "clock_counter_register", "", bench::Measure([] {
    bench::Keep(ReadCounterRegister());
  }));

  // --- Mutexes, uncontended -----------------------------------------------
  bench::EmitTiming(s, "mutex_pthread_lock_unlock", "", bench::Measure([] {
    pthread_mutex_lock(&g_pthread_mutex);
    pthread_mutex_unlock(&g_pthread_mutex);
  }));
  bench::EmitTiming(s, "mutex_std_lock_unlock", "", bench::Measure([] {
    g_std_mutex.lock();
    g_std_mutex.unlock();
  }));
  bench::EmitTiming(s, "mutex_std_recursive_lock_unlock", "",
                    bench::Measure([] {
                      g_recursive_mutex.lock();
                      g_recursive_mutex.unlock();
                    }));

  // --- Atomics ------------------------------------------------------------
  std::atomic<uint16_t> flag{0};
  std::atomic<uint64_t> counter{0};
  bench::EmitTiming(s, "atomic_cas16_acquire", "outline helper",
                    bench::Measure([&] {
                      prim_outline_cas16_acquire(&flag, 0, 1);
                      flag.store(0, std::memory_order_relaxed);
                    }));
  bench::EmitTiming(s, "atomic_add64_relaxed", "outline helper",
                    bench::Measure([&] {
                      bench::Keep(prim_outline_add64_relaxed(&counter));
                    }));
  if (has_lse) {
    bench::EmitTiming(s, "atomic_cas16_acquire", "inline LSE",
                      bench::Measure([&] {
                        prim_lse_cas16_acquire(&flag, 0, 1);
                        flag.store(0, std::memory_order_relaxed);
                      }));
    bench::EmitTiming(s, "atomic_add64_relaxed", "inline LSE",
                      bench::Measure([&] {
                        bench::Keep(prim_lse_add64_relaxed(&counter));
                      }));
  } else {
    bench::EmitNote(s, "atomic_inline_lse",
                    "not measured: this CPU has no LSE atomics");
  }
  bench::EmitTiming(s, "atomic_cas16_acquire", "inline LL/SC",
                    bench::Measure([&] {
                      prim_llsc_cas16_acquire(&flag, 0, 1);
                      flag.store(0, std::memory_order_relaxed);
                    }));
  bench::EmitTiming(s, "atomic_add64_relaxed", "inline LL/SC",
                    bench::Measure([&] {
                      bench::Keep(prim_llsc_add64_relaxed(&counter));
                    }));

  // --- Fiber switch, the fcontext assembly alone ---------------------------
  {
    const size_t stack_size = 256 * 1024;
    char* stack = static_cast<char*>(std::malloc(stack_size));
    fcontext_t fiber = make_fcontext(stack + stack_size, stack_size, FiberBody);
    transfer_t there = jump_fcontext(fiber, nullptr);
    auto timing = bench::Measure([&] {
      there = jump_fcontext(there.fctx, nullptr);
    });
    // One operation above is a switch out and a switch back.
    timing.min_ns /= 2;
    timing.median_ns /= 2;
    bench::EmitTiming(s, "fiber_jump_fcontext", "per switch", timing);
  }
  return 0;
}
