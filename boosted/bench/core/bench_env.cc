// hakuX-Boosted bench: see bench_env.h.
#include "bench_env.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "xenia/base/byte_order.h"
#include "xenia/cpu/backend/a64/a64_backend.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/raw_module.h"
#include "xenia/memory.h"

namespace bench {

uint8_t* Env::Translate(uint32_t guest_address) {
  return memory->TranslateVirtual(guest_address);
}

uint32_t Env::PlaceCode(const ppc::Assembler& assembler) {
  if (assembler.base() != code_cursor) {
    std::fprintf(stderr, "bench: program assembled for %08X, placed at %08X\n",
                 assembler.base(), code_cursor);
    std::abort();
  }
  uint32_t address = code_cursor;
  assembler.CopyTo(Translate(address));
  // Keep functions apart so the scanner never runs one into the next.
  code_cursor += (uint32_t(assembler.count()) * 4 + 0x40 + 0xF) & ~0xFu;
  if (code_cursor >= kCodeBase + kCodeSize) {
    std::fprintf(stderr, "bench: out of guest code space\n");
    std::abort();
  }
  return address;
}

uint32_t Env::AllocData(uint32_t size, uint32_t alignment) {
  uint32_t address = memory->SystemHeapAlloc(size, alignment);
  if (!address) {
    std::fprintf(stderr, "bench: guest allocation of %u bytes failed\n", size);
    std::abort();
  }
  std::memset(Translate(address), 0, size);
  return address;
}

uint32_t Env::ImportThunk(const std::string& name, HostHandler handler,
                          xe::cpu::Export* export_data) {
  uint32_t address = code_cursor;
  code_cursor += 0x20;
  uint8_t* p = Translate(address);
  xe::store_and_swap<uint32_t>(p + 0x0, 0x44000042);  // sc 2
  xe::store_and_swap<uint32_t>(p + 0x4, 0x4E800020);  // blr
  xe::store_and_swap<uint32_t>(p + 0x8, 0x60000000);  // nop
  xe::store_and_swap<uint32_t>(p + 0xC, 0x60000000);  // nop
  xe::cpu::Function* function = nullptr;
  module->DeclareFunction(address, &function);
  function->set_end_address(address + 16 - 4);
  function->set_name(name);
  static_cast<xe::cpu::GuestFunction*>(function)->SetupExtern(
      reinterpret_cast<xe::cpu::GuestFunction::ExternHandler>(handler),
      export_data);
  function->set_status(xe::cpu::Symbol::Status::kDeclared);
  return address;
}

uint32_t Env::ImportKernelExport(const std::string& name) {
  xe::cpu::Export* export_data = FindKernelExport(name);
  if (!export_data || !export_data->function_data.trampoline) {
    return 0;
  }
  return ImportThunk(
      name, reinterpret_cast<HostHandler>(export_data->function_data.trampoline),
      export_data);
}

Env* CreateEnv(bool with_kernel) {
  auto* env = new Env();
  env->memory = new xe::Memory();
  if (!env->memory->Initialize()) {
    std::fprintf(stderr, "bench: guest memory could not be set up\n");
    std::abort();
  }
  env->processor = new xe::cpu::Processor(env->memory, nullptr);
  if (!env->processor->Setup(
          std::make_unique<xe::cpu::backend::a64::A64Backend>())) {
    std::fprintf(stderr, "bench: the ARM64 backend could not be set up\n");
    std::abort();
  }
  auto* heap = env->memory->LookupHeap(Env::kCodeBase);
  if (!heap ||
      !heap->AllocFixed(Env::kCodeBase, Env::kCodeSize, 0,
                        xe::kMemoryAllocationReserve |
                            xe::kMemoryAllocationCommit,
                        xe::kMemoryProtectRead | xe::kMemoryProtectWrite)) {
    std::fprintf(stderr, "bench: guest code space could not be allocated\n");
    std::abort();
  }
  auto module = std::make_unique<xe::cpu::RawModule>(env->processor);
  module->set_name("bench");
  module->set_executable(true);
  module->SetAddressRange(Env::kCodeBase, Env::kCodeSize);
  env->module = module.get();
  env->processor->AddModule(std::move(module));
  if (with_kernel) {
    env->kernel_state = CreateKernelState(env);
  }
  return env;
}

}  // namespace bench
