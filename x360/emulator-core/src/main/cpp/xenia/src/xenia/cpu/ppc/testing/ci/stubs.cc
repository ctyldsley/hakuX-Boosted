// hakuX-Boosted: stand-ins for the symbols of other subsystems that the CPU
// sources name, so that the instruction tests link without the kernel, the
// GPU and the rest (see CMakeLists.txt beside this file).
//
// Nothing here runs in a test. The tests load a RawModule, so every
// dynamic_cast to XexModule in the translator gives null and the XexModule
// members below are never reached; they abort if they ever are.

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "xenia/base/cvar.h"
#include "xenia/cpu/xex_module.h"

// Defined in kernel/kernel_flags.cc in the app. The translator reads it to
// decide whether to emit the scheduler's preemption checks. The app's default
// is true; no scheduler exists here, so the tests run without it unless a run
// asks for it (the check's fast path is then exercised, never the yield).
DEFINE_bool(guest_scheduler, false,
            "Emit the cooperative scheduler's preemption checks.", "Kernel");

namespace xe {
namespace cpu {

namespace {
[[noreturn]] void Unreachable(const char* what) {
  std::fprintf(stderr, "ppc tests: stub reached: %s\n", what);
  std::abort();
}
}  // namespace

XexModule::~XexModule() = default;
bool XexModule::ContainsAddress(uint32_t address) {
  Unreachable("XexModule::ContainsAddress");
}
void XexModule::Precompile() { Unreachable("XexModule::Precompile"); }
std::unique_ptr<Function> XexModule::CreateFunction(uint32_t address) {
  Unreachable("XexModule::CreateFunction");
}
InfoCacheFlags* XexModule::GetInstructionAddressFlags(uint32_t guest_addr) {
  Unreachable("XexModule::GetInstructionAddressFlags");
}
bool XexModule::GetOptHeader(xex2_header_keys key, void** out_ptr) const {
  Unreachable("XexModule::GetOptHeader");
}

}  // namespace cpu
}  // namespace xe

// The allocator abort under qemu with 16 KiB pages ("Fatal glibc error:
// malloc.c (sysmalloc): assertion failed ... (old_end & (pagesize - 1)) ==
// 0"), which failed the "everything, 16k" line of the matrix.
//
// qemu-user on a 4 KiB host, asked for 16 KiB pages (-p 16384), starts the
// program with a break that is only 4 KiB aligned: it is the end of the
// executable's data. glibc's allocator copes with that as long as its first
// brk() succeeds (it then extends the break to a page boundary itself). When
// the first brk() fails, because something of qemu's own sits just above the
// executable, the allocator turns to mmap and from then on takes whatever a
// later brk() returns to be page aligned, as it would be on a real kernel.
// If that mapping of qemu's is freed while the tests run, the next brk()
// succeeds from the unaligned break, the allocator's top chunk ends off a
// page boundary, and the growth after that aborts on the assertion. Whether
// and when it happens depends on where the host put qemu's mappings, which
// is why it came and went between machines and runs, with every translator
// setting off as well, and why shorter processes only made it rarer.
//
// So, before anything else runs: move the break up to a page boundary (a
// brk() that stays inside the break's own page always succeeds under qemu),
// and put an inaccessible page at it, so that brk() can never grow and the
// allocator stays on mmap for the whole run. Nothing is done when the break
// is page aligned already, which it is on a real kernel.
namespace {
__attribute__((constructor(101))) void AlignProgramBreak() {
  const uintptr_t page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
  const uintptr_t brk_now = reinterpret_cast<uintptr_t>(sbrk(0));
  if (!page || brk_now == static_cast<uintptr_t>(-1) ||
      !(brk_now & (page - 1))) {
    return;
  }
  const uintptr_t aligned = (brk_now + page - 1) & ~(page - 1);
  const bool moved =
      sbrk(static_cast<intptr_t>(aligned - brk_now)) != reinterpret_cast<void*>(-1);
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
  void* guard = mmap(reinterpret_cast<void*>(aligned), page, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  std::fprintf(stderr,
               "ppc tests: the program break %#lx is not aligned to the "
               "%lu-byte page (qemu): %s, %s\n",
               static_cast<unsigned long>(brk_now),
               static_cast<unsigned long>(page),
               moved ? "moved up to a page boundary" : "could not be moved",
               guard == reinterpret_cast<void*>(aligned)
                   ? "guard page placed above it"
                   : "no guard page (the address is taken)");
}
}  // namespace
