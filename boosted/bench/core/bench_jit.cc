// hakuX-Boosted bench, section 4: the translator on whole images of
// compiled code (boosted/notes/jit-speed.md).
//
// An image is a flat file of PowerPC code and data for a fixed guest
// address, with a manifest beside it that lists its function entries and,
// for images made by jit/build-workloads.py, calls to make into it with the
// result each must return. For one image this program:
//
//  - translates every listed function once, through the processor's own
//    first-call path (Processor::ResolveFunction), and reports the guest
//    instructions translated, the host code emitted for them and the
//    thread CPU time it took;
//  - makes every "check" call and compares the result with the manifest's
//    (the host C compiler's result for the same source): a setting that
//    produced wrong code is a "WRONG RESULT" note, not a number;
//  - times every "timed" call (the lowest of several samples) and, where
//    the machine exposes an instruction counter, counts the host
//    instructions it retires.
//
// BENCH_JIT_MANIFEST names the manifest. BENCH_JIT_SAMPLES is the number of
// timed samples a call (7), BENCH_JIT_NO_TIMED=1 leaves the timed calls out
// (for a run under qemu, where only the code size means anything).
// BENCH_JIT_ONLY=name makes that workload's checked call only and prints the
// code cache's address range first: jit/jit-count-qemu.sh counts the host
// instructions translated code runs with it, for a machine without a
// counter.
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/function.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/raw_module.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"

#include "bench.h"
#include "bench_env.h"
#include "insn_counter.h"

namespace bench {
namespace {

using xe::cpu::ppc::PPCContext;

struct Call {
  std::string name;
  uint32_t address = 0;
  uint32_t n = 0;
  uint32_t seed = 0;
  uint32_t expected = 0;
  bool timed = false;
};

struct Image {
  std::string name;
  std::string file;
  uint32_t base = 0;
  uint32_t file_base = 0;
  uint32_t size = 0;
  std::vector<uint32_t> functions;
  std::vector<Call> calls;
};

bool ReadManifest(const std::string& path, Image* image) {
  std::ifstream in(path);
  if (!in) {
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream words(line);
    std::string key;
    words >> key;
    if (key == "image") {
      words >> image->name;
    } else if (key == "file") {
      words >> image->file;
    } else if (key == "base") {
      words >> std::hex >> image->base;
    } else if (key == "file_base") {
      words >> std::hex >> image->file_base;
    } else if (key == "size") {
      words >> std::hex >> image->size;
    } else if (key == "fn") {
      uint32_t address = 0;
      words >> std::hex >> address;
      image->functions.push_back(address);
    } else if (key == "run") {
      Call call;
      std::string kind;
      words >> call.name >> std::hex >> call.address >> std::dec >> call.n >>
          call.seed >> call.expected >> kind;
      call.timed = kind == "timed";
      image->calls.push_back(call);
    }
  }
  const size_t slash = path.rfind('/');
  if (slash != std::string::npos) {
    image->file = path.substr(0, slash + 1) + image->file;
  }
  return image->base && image->size && !image->file.empty();
}

// A function list made from a title's file names data as code now and then,
// and the translator does not survive all of it. A fault while translating
// writes the function's address to <manifest>.skip and stops the process
// with status 75; run-jit.sh runs the image again, which leaves the
// functions in that file out and counts them as refused.
volatile uint32_t g_translating = 0;
char g_skip_path[1024];

void TranslateFault(int signal_number) {
  int fd = open(g_skip_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd >= 0) {
    char line[16];
    uint32_t address = g_translating;
    for (int i = 0; i < 8; ++i) {
      line[i] = "0123456789ABCDEF"[(address >> (28 - 4 * i)) & 0xF];
    }
    line[8] = '\n';
    (void)!write(fd, line, 9);
    close(fd);
  }
  _exit(75);
}

uint64_t ThreadCpuNs() {
  timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

}  // namespace

int RunJitSuite(const std::string& label) {
  const std::string s = "jit";
  const char* manifest = std::getenv("BENCH_JIT_MANIFEST");
  Image image;
  if (!manifest || !ReadManifest(manifest, &image)) {
    EmitNote(s, "(" + label + ")",
             "NOT MEASURED: BENCH_JIT_MANIFEST does not name a manifest");
    return 1;
  }
  const uint32_t span = (image.size + 0xFFFFu) & ~0xFFFFu;
  Env* env = CreateEnv(false, image.base, span);
  FILE* file = std::fopen(image.file.c_str(), "rb");
  if (!file) {
    EmitNote(s, image.name + " (" + label + ")",
             "NOT MEASURED: the image file is missing");
    return 1;
  }
  std::fseek(file, 0, SEEK_END);
  const long file_size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  const uint32_t file_offset = image.file_base - image.base;
  if (file_size < 0 || file_offset + uint64_t(file_size) > span ||
      std::fread(env->Translate(image.file_base), 1, size_t(file_size),
                 file) != size_t(file_size)) {
    EmitNote(s, image.name + " (" + label + ")",
             "NOT MEASURED: the image file does not fit its manifest");
    return 1;
  }
  std::fclose(file);

  uint32_t stack_size = 1024 * 1024;
  uint32_t stack = env->AllocData(stack_size, 0x1000);
  uint32_t pcr = env->AllocData(0x2D8);
  auto* thread_state =
      new xe::cpu::ThreadState(env->processor, 0x100, stack + stack_size, pcr);
  xe::cpu::ThreadState::Bind(thread_state);
  PPCContext* ctx = thread_state->context();
  const uint32_t stack_top = uint32_t(ctx->r[1]);
  InsnCounter counter;
  PinToCurrentCpu();
  int status = 0;

  // --- Translation: every function, once -----------------------------------
  uint64_t guest_insns = 0, host_bytes = 0, refused = 0, translated = 0;
  std::vector<uint32_t> skip;
  std::snprintf(g_skip_path, sizeof(g_skip_path), "%s.skip", manifest);
  {
    std::ifstream in(g_skip_path);
    uint32_t address = 0;
    while (in >> std::hex >> address) {
      skip.push_back(address);
    }
  }
  struct sigaction fault = {}, old_actions[4];
  fault.sa_handler = TranslateFault;
  const int fault_signals[4] = {SIGSEGV, SIGBUS, SIGILL, SIGABRT};
  for (int i = 0; i < 4; ++i) {
    sigaction(fault_signals[i], &fault, &old_actions[i]);
  }
  const uint64_t t0 = ThreadCpuNs();
  for (uint32_t address : image.functions) {
    if (std::find(skip.begin(), skip.end(), address) != skip.end()) {
      ++refused;
      continue;
    }
    g_translating = address;
    auto* fn = env->processor->ResolveFunction(address);
    if (!fn || !fn->is_guest()) {
      ++refused;
      continue;
    }
    auto* guest = static_cast<xe::cpu::GuestFunction*>(fn);
    if (!guest->machine_code() || !guest->machine_code_length()) {
      ++refused;
      continue;
    }
    ++translated;
    // BENCH_JIT_DUMP=dir: each function's machine code, for a disassembler.
    if (const char* dump = std::getenv("BENCH_JIT_DUMP")) {
      char path[512];
      std::snprintf(path, sizeof(path), "%s/%s-%08X.bin", dump,
                    image.name.c_str(), address);
      if (FILE* out = std::fopen(path, "wb")) {
        std::fwrite(guest->machine_code(), 1, guest->machine_code_length(),
                    out);
        std::fclose(out);
      }
    }
    guest_insns += (fn->end_address() - fn->address()) / 4 + 1;
    host_bytes += guest->machine_code_length();
  }
  const uint64_t t1 = ThreadCpuNs();
  for (int i = 0; i < 4; ++i) {
    sigaction(fault_signals[i], &old_actions[i], nullptr);
  }
  const std::string all = image.name + " (whole image)";
  Emit(s, all, label, "functions", double(translated));
  Emit(s, all, label, "functions_refused", double(refused));
  Emit(s, all, label, "guest_insns", double(guest_insns));
  Emit(s, all, label, "host_bytes", double(host_bytes));
  if (guest_insns) {
    Emit(s, all, label, "host_bytes_per_guest_insn",
         double(host_bytes) / double(guest_insns));
    Emit(s, all, label, "translate_us_per_guest_insn",
         double(t1 - t0) / 1000.0 / double(guest_insns));
  }
  Emit(s, all, label, "translate_ms", double(t1 - t0) / 1e6);

  // --- Calls ----------------------------------------------------------------
  auto call = [&](const Call& c) {
    auto* fn = env->processor->ResolveFunction(c.address);
    ctx->r[1] = stack_top;
    ctx->r[3] = c.n;
    ctx->r[4] = c.seed;
    ctx->lr = 0xBCBCBCBC;
    fn->Call(thread_state, uint32_t(ctx->lr));
    return uint32_t(ctx->r[3]);
  };
  const char* only = std::getenv("BENCH_JIT_ONLY");
  const char* samples_env = std::getenv("BENCH_JIT_SAMPLES");
  const int samples = samples_env ? std::max(1, std::atoi(samples_env)) : 7;
  const bool no_timed = only || std::getenv("BENCH_JIT_NO_TIMED") != nullptr;
  if (only) {
    auto* cache = env->processor->backend()->code_cache();
    std::printf("jit-count code_cache=0x%llx..0x%llx\n",
                (unsigned long long)cache->execute_base_address(),
                (unsigned long long)(cache->execute_base_address() +
                                     cache->total_size()));
  }
  std::vector<std::string> wrong;
  for (const Call& c : image.calls) {
    if (only && (c.name != only || c.timed)) {
      continue;
    }
    const std::string name = image.name + "/" + c.name;
    if (std::find(wrong.begin(), wrong.end(), c.name) != wrong.end()) {
      continue;
    }
    if (!c.timed) {
      const uint32_t got = call(c);
      if (got != c.expected) {
        char text[160];
        std::snprintf(text, sizeof(text),
                      "WRONG RESULT: n=%u gave %08X, the host's C compiler "
                      "gives %08X; not timed",
                      c.n, got, c.expected);
        EmitNote(s, name + " (" + label + ")", text);
        wrong.push_back(c.name);
        status = 1;
      } else if (only) {
        std::printf("jit-count %s ok=1\n", c.name.c_str());
      }
      continue;
    }
    if (no_timed) {
      continue;
    }
    double best_ms = 1e30;
    uint64_t best_insns = ~0ull;
    bool ok = true;
    for (int i = 0; i < samples; ++i) {
      const uint64_t start = NowNs();
      if (counter.ok()) {
        counter.Start();
      }
      const uint32_t got = call(c);
      if (counter.ok()) {
        best_insns = std::min(best_insns, counter.Stop());
      }
      best_ms = std::min(best_ms, double(NowNs() - start) / 1e6);
      ok = ok && got == c.expected;
    }
    if (!ok) {
      EmitNote(s, name + " (" + label + ")",
               "WRONG RESULT in the timed run; its time is not reported");
      status = 1;
      continue;
    }
    Emit(s, name, label, "run_ms", best_ms);
    if (counter.ok()) {
      Emit(s, name, label, "host_insns_millions", double(best_insns) / 1e6);
    }
  }
  if (!counter.ok() && !no_timed) {
    EmitNote(s, "host_insns_millions",
             "not measured: the runner does not expose an instruction "
             "counter (" + counter.why_not() + ")");
  }
  return status;
}

}  // namespace bench
