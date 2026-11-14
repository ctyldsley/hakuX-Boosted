// hakuX-Boosted bench: the least of an emulator that the core's CPU and
// guest scheduler will run in. Guest memory, a processor with the ARM64
// backend, one module of guest code the harness writes, and (when asked) a
// kernel state that guest threads can be created against.
#ifndef BOOSTED_BENCH_CORE_BENCH_ENV_H_
#define BOOSTED_BENCH_CORE_BENCH_ENV_H_

#include <cstdint>
#include <string>

#include "ppc_asm.h"

namespace xe {
class Memory;
namespace cpu {
class Export;
class Function;
class Processor;
class RawModule;
namespace ppc {
struct PPCContext_s;
}
}  // namespace cpu
namespace kernel {
class KernelState;
}
}  // namespace xe

namespace bench {

// A host function that guest code calls through an import thunk, with the
// signature the core gives kernel exports.
using HostHandler = void (*)(xe::cpu::ppc::PPCContext_s* context,
                             xe::kernel::KernelState* kernel_state);

struct Env {
  xe::Memory* memory = nullptr;
  xe::cpu::Processor* processor = nullptr;
  xe::cpu::RawModule* module = nullptr;
  xe::kernel::KernelState* kernel_state = nullptr;

  static constexpr uint32_t kCodeBase = 0x82000000;
  static constexpr uint32_t kCodeSize = 4 * 1024 * 1024;
  uint32_t code_cursor = kCodeBase;

  // Where the next program will be placed: give this to ppc::Assembler.
  uint32_t NextCodeAddress() const { return code_cursor; }
  // Copies an assembled program into guest memory at NextCodeAddress().
  uint32_t PlaceCode(const ppc::Assembler& assembler);
  // Guest data, zeroed. Returns its guest address.
  uint32_t AllocData(uint32_t size, uint32_t alignment = 0x20);
  uint8_t* Translate(uint32_t guest_address);

  // An import thunk as cpu/xex_module.cc writes them ("sc 2; blr") bound to
  // a host function. Guest code reaches the function with "bl <address>".
  uint32_t ImportThunk(const std::string& name, HostHandler handler,
                       xe::cpu::Export* export_data = nullptr);
  // The same for one of the core's own kernel exports, by name. 0 if this
  // tree does not have it.
  uint32_t ImportKernelExport(const std::string& name);
};

// Builds the environment. with_kernel adds the kernel state.
Env* CreateEnv(bool with_kernel);

// kernel_shim.cc
xe::kernel::KernelState* CreateKernelState(Env* env);
xe::cpu::Export* FindKernelExport(const std::string& name);

}  // namespace bench

#endif  // BOOSTED_BENCH_CORE_BENCH_ENV_H_
