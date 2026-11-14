/**
 ******************************************************************************
 * hakuX-Boosted: a test of the guest memory watches, kept beside the code it  *
 * tests. Not part of any build: the build skips *_test.cc, and the whole file *
 * is empty unless XE_MEMORY_WATCH_HARNESS is defined.                         *
 ******************************************************************************
 */

// What it checks. It stands in for the GPU's shared memory: it keeps a copy
// of guest physical pages, marks a page valid when it has copied it and asked
// for a watch (EnablePhysicalMemoryAccessCallbacks), and marks it invalid
// when the invalidation callback fires. After every operation it compares
// each valid page's copy with guest memory. A watch that fails to fire leaves
// a valid page that differs, which is the stale texture or vertex buffer on
// the device. The operations are random uploads, guest writes through the
// virtual views (which fault), guest protection changes, announced writes,
// releases and allocations, with optional writer threads. "pattern=lag" and
// "pattern=interleaved" instead replay a vertex buffer rewritten every frame
// and print host calls and faults per frame.
//
// Arguments: bridge, ahead=<KB>, limit=<n>, uffd, lean, log (the settings of
// the same names), nowiden (the callback does not widen the range), seed=<n>,
// ops=<n>, threads=<n>, pattern=lag, pattern=interleaved.
//
// How it was built (x86-64 Linux, clang, 2026-11-14), from xenia/: compile
// src/xenia/memory.cc, src/xenia/cpu/mmio_handler.cc and the POSIX files of
// src/xenia/base (not *_win, *_mac, *_android, console_app_main*, main_init*,
// embedded_bundle, platform_amd64, platform_arm64) with
//   -std=c++20 -O1 -march=x86-64-v3 -DXE_MEMORY_WATCH_HARNESS -DFMT_HEADER_ONLY
//   -I. -Isrc -Ithird_party -Ithird_party/fmt/include -Ithird_party/utfcpp/source
// (the fmt, cxxopts, tomlplusplus, date, utfcpp and disruptorplus submodules
// are needed), then link them with -lpthread. The few symbols the rest of the
// tree would provide are stubbed below.
//
// It proves nothing about a phone: other kernel, other processor, no title.

namespace xe {
namespace memory {}  // Keeps this from being an empty translation unit.
}  // namespace xe

#if defined(XE_MEMORY_WATCH_HARNESS)

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/system.h"
#include "xenia/memory.h"

DECLARE_bool(memory_watch_log);
DECLARE_bool(memory_watch_bridge_runs);
DECLARE_uint32(memory_watch_arm_ahead_kb);
DECLARE_uint32(memory_watch_arm_ahead_spurious_limit);
DECLARE_bool(memory_watch_uffd);
DECLARE_bool(memory_watch_lean_fault_handler);

// Stubs for parts of the tree not linked in.
namespace xe {
void ShowSimpleMessageBox(SimpleMessageBoxType, std::string_view m) {
  fprintf(stderr, "MSGBOX: %.*s\n", int(m.size()), m.data());
}
uint64_t Clock::host_tick_count_raw() { return 0; }
uint64_t Clock::host_tick_frequency_raw() { return 1000; }
namespace amd64 { uint64_t GetFeatureFlags() { return 0; } }
}
extern "C" {
void* jump_fcontext() { abort(); }
void* make_fcontext() { abort(); }
int stbi_info_from_file() { return 0; }
}

using namespace xe;
static Memory* mem;
static xe::global_critical_region gcr;
constexpr uint32_t kPage = 4096;
constexpr uint32_t kPhysPages = 0x20000000 / kPage;
static std::vector<uint8_t> valid(kPhysPages);  // under global lock
static uint8_t* shadow;
static std::atomic<uint64_t> callbacks{0};
static bool widen = true;

static std::pair<uint32_t, uint32_t> Invalidate(void*, uint32_t start,
                                                uint32_t length, bool exact) {
  callbacks++;
  uint32_t first = start / kPage, last = (start + length - 1) / kPage;
  if (!exact && widen) {
    first &= ~63u;
    last |= 63u;
  }
  last = std::min(last, kPhysPages - 1);
  for (uint32_t i = first; i <= last; ++i) valid[i] = 0;
  return {first * kPage, (last - first + 1) * kPage};
}

struct Region { uint32_t vaddr; uint32_t size; bool e_heap; bool writable; bool live; };
static std::vector<Region> regions;

static uint32_t Phys(uint32_t vaddr) { return mem->GetPhysicalAddress(vaddr); }

static void Upload(uint32_t phys, uint32_t len) {
  uint32_t first = phys / kPage, last = (phys + len - 1) / kPage;
  uint32_t i = first;
  while (i <= last) {
    uint32_t run_first, run_last;
    {
      auto lock = gcr.Acquire();
      while (i <= last && valid[i]) ++i;
      if (i > last) break;
      run_first = i;
      while (i <= last && !valid[i]) { valid[i] = 1; ++i; }
      run_last = i - 1;
    }
    mem->EnablePhysicalMemoryAccessCallbacks(
        run_first * kPage, (run_last - run_first + 1) * kPage, true, false);
    memcpy(shadow + size_t(run_first) * kPage,
           mem->TranslatePhysical(run_first * kPage),
           size_t(run_last - run_first + 1) * kPage);
  }
}

static Region* only_region = nullptr;
static uint64_t Verify(const char* when) {
  auto lock = gcr.Acquire();
  uint64_t bad = 0, checked = 0;
  for (auto& r : regions) {
    if (!r.live) continue;
    if (only_region && &r != only_region) continue;
    uint32_t p = Phys(r.vaddr);
    for (uint32_t o = 0; o < r.size; o += kPage) {
      uint32_t page = (p + o) / kPage;
      if (!valid[page]) continue;
      ++checked;
      if (memcmp(shadow + size_t(page) * kPage,
                 mem->TranslatePhysical(page * kPage), kPage)) {
        if (bad < 5)
          fprintf(stderr, "STALE page %08X (region %08X) %s\n", page * kPage,
                  r.vaddr, when);
        ++bad;
      }
    }
  }
  if (bad) { fprintf(stderr, "FAILED: %lu stale of %lu valid pages\n", bad, checked); exit(2); }
  return checked;
}

static bool AllocRegion(Region& r, std::mt19937& rng) {
  auto heap = mem->LookupHeapByType(true, r.e_heap ? 4096 : 64 * 1024);
  uint32_t addr = 0;
  if (!heap->Alloc(r.size, r.e_heap ? 4096 : 65536,
                   kMemoryAllocationReserve | kMemoryAllocationCommit,
                   kMemoryProtectRead | kMemoryProtectWrite, (rng() & 1) != 0, &addr))
    return false;
  r.vaddr = addr; r.writable = true; r.live = true;
  return true;
}

int main(int argc, char** argv) {
  uint32_t seed = 1, ops = 200000, threads = 0;
  int pattern = 0;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "bridge") cvars::memory_watch_bridge_runs = true;
    else if (a == "uffd") cvars::memory_watch_uffd = true;
    else if (a == "log") cvars::memory_watch_log = true;
    else if (a == "lean") cvars::memory_watch_lean_fault_handler = true;
    else if (a == "pattern=lag") pattern = 1;
    else if (a == "pattern=interleaved") pattern = 2;
    else if (a == "nowiden") widen = false;
    else if (a.rfind("ahead=", 0) == 0) cvars::memory_watch_arm_ahead_kb = atoi(a.c_str() + 6);
    else if (a.rfind("limit=", 0) == 0) cvars::memory_watch_arm_ahead_spurious_limit = atoi(a.c_str() + 6);
    else if (a.rfind("seed=", 0) == 0) seed = atoi(a.c_str() + 5);
    else if (a.rfind("ops=", 0) == 0) ops = atoi(a.c_str() + 4);
    else if (a.rfind("threads=", 0) == 0) threads = atoi(a.c_str() + 8);
  }
  xe::InitializeLogging("harness");
  shadow = (uint8_t*)mmap(nullptr, 0x20000000, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  mem = new Memory();
  if (!mem->Initialize()) { fprintf(stderr, "init failed\n"); return 1; }
  mem->RegisterPhysicalMemoryInvalidationCallback(Invalidate, nullptr);
  std::mt19937 rng(seed);
  for (int i = 0; i < 6; ++i) {
    Region r{0, (i < 3) ? 0x100000u : 0x40000u, i >= 3, true, false};
    if (!AllocRegion(r, rng)) { fprintf(stderr, "alloc failed\n"); return 1; }
    regions.push_back(r);
  }
  if (pattern) {
    // A dynamic vertex buffer rewritten at the same addresses every frame:
    // 40 chunks of 7.5 KB written one after another, each then drawn (one
    // upload request per chunk). "lag": the title writes the whole frame
    // before the GPU thread uploads any of it. "interleaved": each chunk is
    // uploaded as soon as it is written.
    Region& r = regions[0];
    const uint32_t chunk = 7500, chunks = 40, frames = 300;
    auto& c = xe::memory::watch_counters();
    uint64_t checked = 0;
    for (uint32_t f = 0; f < frames; ++f) {
      for (uint32_t i = 0; i < chunks; ++i) {
        volatile uint8_t* p = mem->TranslateVirtual<uint8_t*>(r.vaddr + 0x30000 + i * chunk);
        for (uint32_t j = 0; j < chunk; ++j) p[j] = uint8_t(rng());
        if (pattern == 2) Upload(Phys(r.vaddr + 0x30000 + i * chunk), chunk);
      }
      if (pattern == 1)
        for (uint32_t i = 0; i < chunks; ++i) Upload(Phys(r.vaddr + 0x30000 + i * chunk), chunk);
      only_region = &r; checked += Verify("frame"); only_region = nullptr;
    }
    printf("PATTERN %s: per frame: mprotect %.1f, uffd ioctls %.1f, host calls to arm %.1f, to lift %.1f, faults %.1f (of them on pages armed ahead %.1f); pages verified %lu\n",
           pattern == 1 ? "lag" : "interleaved", double(c.mprotect_calls) / frames, double(c.uffd_calls) / frames,
           double(c.arm_calls) / frames, double(c.unwatch_calls) / frames,
           double(c.fault_watch_hits + c.fault_ahead_hits + c.fault_races) / frames, double(c.fault_ahead_hits) / frames, checked);
    fflush(stdout);
    _exit(0);
  }
  std::atomic<bool> stop{false};
  std::atomic<bool> pause{false};
  std::atomic<int> paused{0};
  std::atomic<uint64_t> writes{0};
  // Writer threads only write to regions 0, 1, 3, 4, which the main thread
  // never re-protects or releases in threaded mode.
  auto writer = [&](uint32_t tseed) {
    std::mt19937 trng(tseed);
    while (!stop) {
      if (pause) { paused++; while (pause) std::this_thread::yield(); paused--; }
      static const int pick[4] = {0, 1, 3, 4};
      Region& r = regions[pick[trng() % 4]];
      uint32_t off = trng() % r.size;
      uint32_t n = 1 + trng() % 64;
      if (trng() % 8 == 0) n = 1 + trng() % 20000;
      n = std::min(n, r.size - off);
      volatile uint8_t* p = mem->TranslateVirtual<uint8_t*>(r.vaddr + off);
      for (uint32_t k = 0; k < n; ++k) p[k] = uint8_t(trng());
      writes++;
    }
  };
  std::vector<std::thread> ts;
  for (uint32_t t = 0; t < threads; ++t) ts.emplace_back(writer, seed * 977 + t);
  uint64_t checked = 0;
  for (uint32_t op = 0; op < ops; ++op) {
    uint32_t k = rng() % 100;
    Region& r = regions[rng() % regions.size()];
    bool shared_with_writers = threads && (&r == &regions[0] || &r == &regions[1] || &r == &regions[3] || &r == &regions[4]);
    if (!r.live) {
      if (k < 30) AllocRegion(r, rng);
      continue;
    }
    uint32_t off = rng() % r.size;
    if (k < 40) {  // upload
      uint32_t n = 1 + rng() % 30000;
      if (rng() % 16 == 0) n = r.size;
      n = std::min(n, r.size - off);
      Upload(Phys(r.vaddr + off), n);
    } else if (k < 85) {  // guest write
      if (!r.writable) continue;
      uint32_t n = 1 + rng() % 64;
      if (rng() % 8 == 0) n = 1 + rng() % 20000;
      n = std::min(n, r.size - off);
      volatile uint8_t* p = mem->TranslateVirtual<uint8_t*>(r.vaddr + off);
      for (uint32_t j = 0; j < n; ++j) p[j] = uint8_t(rng());
      writes++;
    } else if (k < 90) {  // guest protect
      if (shared_with_writers) continue;
      auto heap = mem->LookupHeap(r.vaddr);
      uint32_t ps = heap->page_size();
      uint32_t a = (r.vaddr + off) & ~(ps - 1);
      uint32_t n = std::min<uint32_t>(ps * (1 + rng() % 3), r.vaddr + r.size - a);
      // Keep it simple for the model: whole region toggles.
      (void)a; (void)n;
      bool to_rw = !r.writable;
      heap->Protect(r.vaddr, r.size, to_rw ? (kMemoryProtectRead | kMemoryProtectWrite) : kMemoryProtectRead);
      r.writable = to_rw;
    } else if (k < 93) {  // announced write through the physical alias
      uint32_t n = std::min<uint32_t>(1 + rng() % 9000, r.size - off);
      if (shared_with_writers || !r.writable) continue;
      mem->TriggerPhysicalMemoryCallbacks(gcr.Acquire(), r.vaddr + off, n, true, (rng() & 1) != 0, true);
      // unprotect=false leaves the host protection for the caller: write via the alias.
      uint8_t* p = mem->TranslatePhysical(Phys(r.vaddr + off));
      for (uint32_t j = 0; j < n; ++j) p[j] = uint8_t(rng());
    } else if (k < 95) {  // release, realloc later
      if (shared_with_writers) continue;
      mem->LookupHeap(r.vaddr)->Release(r.vaddr);
      r.live = false;
    }
    if (!threads && r.live) { only_region = &r; checked += Verify("op"); only_region = nullptr; }
    if (op % (threads ? 50 : 2000) == 49) {
      if (threads) { pause = true; while (paused != int(threads)) std::this_thread::yield(); }
      checked += Verify("mid");
      if (threads) pause = false;
    }
  }
  stop = true;
  for (auto& t : ts) t.join();
  checked += Verify("end");
  auto& c = xe::memory::watch_counters();
  printf("OK seed=%u ops=%u threads=%u writes=%lu callbacks=%lu checked=%lu | mprotect=%lu arm_calls=%lu arm_pages=%lu ahead=%lu bridged=%lu unwatch_calls=%lu uffd=%lu uffd_fail=%lu faults: watch=%lu ahead=%lu race=%lu unhandled=%lu\n",
         seed, ops, threads, writes.load(), callbacks.load(), checked,
         c.mprotect_calls.load(), c.arm_calls.load(), c.arm_pages.load(), c.arm_ahead_pages.load(), c.arm_bridged_pages.load(),
         c.unwatch_calls.load(), c.uffd_calls.load(), c.uffd_failures.load(),
         c.fault_watch_hits.load(), c.fault_ahead_hits.load(), c.fault_races.load(), c.fault_unhandled.load());
  fflush(stdout);
  _exit(0);
}

#endif  // XE_MEMORY_WATCH_HARNESS
