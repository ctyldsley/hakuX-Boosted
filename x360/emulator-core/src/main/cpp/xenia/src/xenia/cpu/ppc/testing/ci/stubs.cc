// hakuX-Boosted: stand-ins for the symbols of other subsystems that the CPU
// sources name, so that the instruction tests link without the kernel, the
// GPU and the rest (see CMakeLists.txt beside this file).
//
// Nothing here runs in a test. The tests load a RawModule, so every
// dynamic_cast to XexModule in the translator gives null and the XexModule
// members below are never reached; they abort if they ever are.

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
