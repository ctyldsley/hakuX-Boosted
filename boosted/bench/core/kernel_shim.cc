// hakuX-Boosted bench: stands in for kernel/kernel_state.cc, which cannot be
// built without the rest of the emulator (the system libraries, the file
// systems, audio, graphics). Everything the guest scheduler and the kernel's
// thread, event and wait code are measured through is the core's own source,
// compiled unmodified (core/CMakeLists.txt lists the files). This file gives
// them the one object they all hang off, a KernelState, without its
// constructor: the memory is zeroed and only the members those sources read
// are built.
//
// What is the harness's own here, and so not measured as the app has it:
//  - RegisterThread, UnregisterThread, OnThreadExit, kernel_state(): copied
//    in substance from kernel_state.cc (a map insert under the global lock, a
//    call to the processor).
//  - OnThreadExecute: the app calls each loaded DLL's entry point; no module
//    is loaded here, so it does nothing.
//  - the title process block that every thread is attached to: set up as
//    KernelState::InitializeProcess does for the title (priority class 10,
//    default priority 13, maximum 17, quantum 60).
//  - RegisterExport_xboxkrnl: the app files exports in its export resolver;
//    here they go in a table by name.
// Everything else below is a function those sources refer to on paths the
// benchmarks never take; each stops the program if it is ever reached.
#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <csetjmp>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <list>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <regex>
#include <set>
#include <shared_mutex>
#include <span>
#include <sstream>
#include <stack>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

// The members set up below are private to their classes. Access specifiers
// do not change a class's layout, and nothing here is compiled into the
// measured sources.
#define private public
#define protected public
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xthread.h"
#undef private
#undef protected

#include "xenia/base/cvar.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/processor.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/user_module.h"
#include "xenia/kernel/util/native_list.h"
#include "xenia/kernel/xfile.h"
#include "xenia/kernel/xmodule.h"
#include "xenia/kernel/xnotifylistener.h"
#include "xenia/kernel/xsymboliclink.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"

#include "bench_env.h"

// cpu/xex_module.cc reads this; emulator.cc, which defines it, is not built.
DEFINE_bool(allow_plugins, false, "Not used by the bench.", "General");

namespace {

[[noreturn]] void NotReached(const char* what) {
  std::fprintf(stderr,
               "bench: %s was called. The kernel stand-in (core/"
               "kernel_shim.cc) does not provide it; a benchmark took a path "
               "it was not written for.\n",
               what);
  std::abort();
}

xe::kernel::KernelState* g_kernel_state = nullptr;

std::map<std::string, xe::cpu::Export*>& ExportTable() {
  static auto* table = new std::map<std::string, xe::cpu::Export*>();
  return *table;
}

}  // namespace

namespace xe {
namespace kernel {

KernelState* kernel_state() { return g_kernel_state; }
KernelState* KernelState::shared() { return g_kernel_state; }

void KernelState::RegisterThread(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();
  threads_by_id_[thread->thread_id()] = thread;
}

void KernelState::UnregisterThread(XThread* thread) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = threads_by_id_.find(thread->thread_id());
  if (it != threads_by_id_.end()) {
    threads_by_id_.erase(it);
  }
}

void KernelState::OnThreadExecute(XThread* thread) {}

void KernelState::OnThreadExit(XThread* thread) {
  processor()->OnThreadExit(thread->thread_id());
}

object_ref<UserModule> KernelState::GetExecutableModule() { return nullptr; }

uint32_t KernelState::AllocateTLS(cpu::ppc::PPCContext* context) {
  NotReached("KernelState::AllocateTLS");
}
void KernelState::FreeTLS(cpu::ppc::PPCContext* context, uint32_t slot) {
  NotReached("KernelState::FreeTLS");
}
bool KernelState::IsKernelModule(const std::string_view name) {
  NotReached("KernelState::IsKernelModule");
}
bool KernelState::IsModuleLoaded(const std::string_view name) {
  NotReached("KernelState::IsModuleLoaded");
}
object_ref<XModule> KernelState::GetModule(const std::string_view name,
                                           bool user_only) {
  NotReached("KernelState::GetModule");
}
object_ref<UserModule> KernelState::LoadUserModule(const std::string_view name,
                                                   bool call_entry) {
  NotReached("KernelState::LoadUserModule");
}
X_RESULT KernelState::FinishLoadingUserModule(
    const object_ref<UserModule> module, bool call_entry) {
  NotReached("KernelState::FinishLoadingUserModule");
}
uint32_t KernelState::GetKeTimestampBundle() {
  NotReached("KernelState::GetKeTimestampBundle");
}
void KernelState::BeginDPCImpersonation(cpu::ppc::PPCContext* context,
                                        DPCImpersonationScope& scope) {
  NotReached("KernelState::BeginDPCImpersonation");
}
void KernelState::EndDPCImpersonation(cpu::ppc::PPCContext* context,
                                      DPCImpersonationScope& end_scope) {
  NotReached("KernelState::EndDPCImpersonation");
}

X_STATUS UserModule::GetOptHeader(xex2_header_keys key, void** out_ptr) {
  NotReached("UserModule::GetOptHeader");
}
object_ref<XSymbolicLink> XSymbolicLink::Restore(KernelState* kernel_state,
                                                 ByteStream* stream) {
  NotReached("XSymbolicLink::Restore");
}
object_ref<XNotifyListener> XNotifyListener::Restore(KernelState* kernel_state,
                                                     ByteStream* stream) {
  NotReached("XNotifyListener::Restore");
}
object_ref<XFile> XFile::Restore(KernelState* kernel_state,
                                 ByteStream* stream) {
  NotReached("XFile::Restore");
}
object_ref<XModule> XModule::Restore(KernelState* kernel_state,
                                     ByteStream* stream) {
  NotReached("XModule::Restore");
}

namespace xboxkrnl {
xe::cpu::Export* RegisterExport_xboxkrnl(xe::cpu::Export* export_entry) {
  ExportTable()[export_entry->name] = export_entry;
  return export_entry;
}
}  // namespace xboxkrnl

}  // namespace kernel
}  // namespace xe

namespace bench {

xe::cpu::Export* FindKernelExport(const std::string& name) {
  auto it = ExportTable().find(name);
  return it == ExportTable().end() ? nullptr : it->second;
}

xe::threading::WaitHandle* ThreadExitHandle(xe::kernel::XThread* thread) {
  return thread->GetWaitHandle();
}

xe::kernel::KernelState* CreateKernelState(Env* env) {
  using namespace xe::kernel;
  // The emulator object is only ever asked for its processor and memory.
  void* emulator_raw = std::calloc(1, sizeof(xe::Emulator));
  auto* emulator = static_cast<xe::Emulator*>(emulator_raw);
  new (&emulator->memory_) std::unique_ptr<xe::Memory>(env->memory);
  new (&emulator->processor_)
      std::unique_ptr<xe::cpu::Processor>(env->processor);

  void* raw = std::calloc(1, sizeof(KernelState));
  auto* ks = static_cast<KernelState*>(raw);
  ks->emulator_ = emulator;
  ks->memory_ = env->memory;
  ks->processor_ = env->processor;
  new (&ks->global_critical_region_) xe::global_critical_region();
  new (&ks->object_table_) util::ObjectTable();
  new (&ks->threads_by_id_) std::unordered_map<uint32_t, XThread*>();
  new (&ks->dpc_list_) util::NativeList(env->memory);
  new (&ks->guest_scheduler_)
      std::unique_ptr<GuestScheduler>(new GuestScheduler(ks));
  g_kernel_state = ks;

  // The guest-side globals: only the title process block is filled in.
  ks->kernel_guest_globals_ =
      env->memory->SystemHeapAlloc(sizeof(KernelGuestGlobals));
  auto* globals = env->memory->TranslateVirtual<KernelGuestGlobals*>(
      ks->kernel_guest_globals_);
  std::memset(globals, 0, sizeof(KernelGuestGlobals));
  auto* process =
      env->memory->TranslateVirtual<X_KPROCESS*>(ks->GetTitleProcess());
  uint32_t guest_process = ks->GetTitleProcess();
  process->process_priority_class = 10;
  process->default_thread_priority = 13;
  process->max_dynamic_priority = 17;
  util::XeInitializeListHead(&process->thread_list,
                             guest_process + offsetof(X_KPROCESS, thread_list));
  process->quantum = 60;
  process->thread_count = 0;
  process->disable_quantum_decay = 0x06;
  process->kernel_stack_size = 16 * 1024;
  process->tls_slot_size = 0x80;
  process->process_type = X_PROCTYPE_TITLE;
  util::XeInitializeListHead(&process->unk_54,
                             guest_process + offsetof(X_KPROCESS, unk_54));
  return ks;
}

}  // namespace bench
