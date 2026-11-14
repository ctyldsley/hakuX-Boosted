// hakuX-Boosted bench, section 3: the translator.
//
// Six small PowerPC programs, written for this rig with core/ppc_asm.h, are
// translated by the core's own frontend, compiler passes and ARM64 backend
// and then run. For each: how much host code the translator emits per guest
// instruction, how long translation takes, how long one guest instruction
// takes to run and, where the runner's CPU counter is exposed, how many host
// instructions run per guest instruction.
//
// Every program computes something the harness also computes in C++, and the
// two are compared after each run: a translator setting that produced wrong
// code shows up as a "WRONG RESULT" note instead of a number.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "xenia/base/byte_order.h"
#if BENCH_HAVE_TRANSLATION_STATS
#include "xenia/cpu/compiler/translation_stats.h"
#endif
#include "xenia/cpu/function.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"

#include "bench.h"
#include "bench_env.h"
#include "insn_counter.h"
#include "ppc_asm.h"

namespace bench {
namespace {

using xe::cpu::ppc::PPCContext;
using Asm = ppc::Assembler;

struct Program {
  std::string name;
  uint32_t entry = 0;
  std::vector<uint32_t> functions;  // every function entry, callees first
  uint32_t guest_words = 0;         // instructions in all of them
  // Guest instructions run by a call with |iterations|.
  std::function<uint64_t(uint32_t iterations)> guest_insns;
  // Sets up registers and memory other than r3 for a run.
  std::function<void(PPCContext*)> prepare;
  // True if the run produced what the C++ model produces.
  std::function<bool(PPCContext*, uint32_t iterations)> check;
};

// --- 1. Integer arithmetic and a data-dependent branch ----------------------
Program BuildInteger(Env& env) {
  Asm a(env.NextCodeAddress());
  Asm::Label loop, skip, done;
  a.li(6, 0);
  a.cmpwi(6, 3, 0);  // no passes asked for: straight to the exit
  a.beq(6, done);
  a.mtctr(3);
  a.li(4, 1);
  a.li(5, 7);
  a.Bind(loop);
  a.mulli(4, 4, 33);
  a.add(4, 4, 5);
  a.rotlwi(7, 4, 5);
  a.xor_(5, 5, 7);
  a.srwi(8, 4, 3);
  a.add(6, 6, 8);
  a.andi_(9, 6, 0xFF);
  a.cmplwi(6, 9, 127);
  a.blt(6, skip);
  a.addi(6, 6, 1);
  a.Bind(skip);
  a.bdnz(loop);
  a.Bind(done);
  a.mr(3, 6);
  a.blr();
  Program p;
  p.name = "integer";
  p.entry = env.PlaceCode(a);
  p.functions = {p.entry};
  p.guest_words = uint32_t(a.count());
  auto model = [](uint32_t iterations, uint64_t* insns) {
    uint32_t r4 = 1, r5 = 7, r6 = 0;
    uint64_t count = 6;
    for (uint32_t i = 0; i < iterations; ++i) {
      r4 = r4 * 33 + r5;
      uint32_t r7 = (r4 << 5) | (r4 >> 27);
      r5 ^= r7;
      r6 += r4 >> 3;
      count += 10;
      if ((r6 & 0xFF) >= 127) {
        r6 += 1;
        count += 1;
      }
    }
    count += 2;
    if (insns) {
      *insns = count;
    }
    return r6;
  };
  p.guest_insns = [model](uint32_t iterations) {
    uint64_t count = 0;
    model(iterations, &count);
    return count;
  };
  p.prepare = [](PPCContext*) {};
  p.check = [model](PPCContext* ctx, uint32_t iterations) {
    return uint32_t(ctx->r[3]) == model(iterations, nullptr);
  };
  return p;
}

// --- 2. Double-precision floating point --------------------------------------
Program BuildFloat(Env& env) {
  uint32_t data = env.AllocData(64);
  Asm a(env.NextCodeAddress());
  Asm::Label loop;
  a.mtctr(3);
  a.lfd(1, 0, 4);
  a.lfd(2, 8, 4);
  a.lfd(3, 16, 4);
  a.lfd(4, 24, 4);
  a.Bind(loop);
  a.fmadd(1, 1, 2, 3);  // f1 = f1 * f2 + f3
  a.fadd(5, 1, 4);
  a.fmul(6, 5, 2);
  a.fsub(1, 6, 5);
  a.fabs_(7, 1);
  a.fmul(3, 7, 2);
  a.fneg(8, 3);
  a.fadd(1, 1, 8);
  a.bdnz(loop);
  a.stfd(1, 32, 4);
  a.blr();
  Program p;
  p.name = "float";
  p.entry = env.PlaceCode(a);
  p.functions = {p.entry};
  p.guest_words = uint32_t(a.count());
  p.guest_insns = [](uint32_t iterations) {
    return 5 + uint64_t(iterations) * 9 + 2;
  };
  Env* e = &env;
  p.prepare = [e, data](PPCContext* ctx) {
    uint8_t* host = e->Translate(data);
    xe::store_and_swap<double>(host + 0, 1.25);
    xe::store_and_swap<double>(host + 8, 0.5);
    xe::store_and_swap<double>(host + 16, 0.75);
    xe::store_and_swap<double>(host + 24, 3.0);
    xe::store_and_swap<double>(host + 32, 0.0);
    ctx->r[4] = data;
  };
  p.check = [e, data](PPCContext*, uint32_t iterations) {
    double f1 = 1.25, f2 = 0.5, f3 = 0.75, f4 = 3.0;
    for (uint32_t i = 0; i < iterations; ++i) {
      f1 = std::fma(f1, f2, f3);
      double f5 = f1 + f4;
      double f6 = f5 * f2;
      f1 = f6 - f5;
      double f7 = std::fabs(f1);
      f3 = f7 * f2;
      f1 = f1 + (-f3);
    }
    double got = xe::load_and_swap<double>(e->Translate(data) + 32);
    return std::fabs(got - f1) <= 1e-9 * (std::fabs(f1) + 1.0);
  };
  return p;
}

// --- 3. AltiVec single-precision vectors -------------------------------------
Program BuildVector(Env& env) {
  constexpr uint32_t kVectors = 64;  // 1 KiB each way
  uint32_t src = env.AllocData(kVectors * 16 + 16, 0x1000);
  uint32_t dst = env.AllocData(kVectors * 16 + 32, 0x1000);
  Asm a(env.NextCodeAddress());
  Asm::Label loop;
  a.mtctr(3);
  a.li(6, 0);
  a.vxor(0, 0, 0);
  a.lvx(1, 0, 4);
  a.Bind(loop);
  a.lvx(2, 6, 4);
  a.vmaddfp(3, 2, 1, 0);  // v3 = v2 * v1 + v0
  a.vaddfp(0, 0, 2);
  a.vmaxfp(4, 3, 2);
  a.vsubfp(4, 4, 1);
  a.stvx(4, 6, 5);
  a.addi(6, 6, 16);
  a.andi_(6, 6, 0x3F0);
  a.bdnz(loop);
  a.li(6, kVectors * 16);
  a.stvx(0, 6, 5);
  a.blr();
  Program p;
  p.name = "vector";
  p.entry = env.PlaceCode(a);
  p.functions = {p.entry};
  p.guest_words = uint32_t(a.count());
  p.guest_insns = [](uint32_t iterations) {
    return 4 + uint64_t(iterations) * 9 + 3;
  };
  Env* e = &env;
  auto source = [](uint32_t index) { return 0.001f * float(index % 97 + 1); };
  p.prepare = [e, src, dst, source](PPCContext* ctx) {
    uint8_t* host = e->Translate(src);
    for (uint32_t i = 0; i < kVectors * 4; ++i) {
      xe::store_and_swap<float>(host + i * 4, source(i));
    }
    std::memset(e->Translate(dst), 0, kVectors * 16 + 32);
    ctx->r[4] = src;
    ctx->r[5] = dst;
  };
  p.check = [e, dst, source](PPCContext*, uint32_t iterations) {
    // The running sum, lane by lane. Not exact to the last bit: the sum is
    // long and the translator may or may not fuse the multiply-add.
    double acc[4] = {0, 0, 0, 0};
    for (uint32_t i = 0; i < iterations; ++i) {
      uint32_t vector = i % kVectors;
      for (int lane = 0; lane < 4; ++lane) {
        acc[lane] += source(vector * 4 + lane);
      }
    }
    uint8_t* host = e->Translate(dst) + kVectors * 16;
    for (int lane = 0; lane < 4; ++lane) {
      double got = xe::load_and_swap<float>(host + lane * 4);
      if (std::fabs(got - acc[lane]) > 2e-2 * (std::fabs(acc[lane]) + 1.0)) {
        return false;
      }
    }
    return true;
  };
  return p;
}

// --- 4. Calls: a loop that calls a function that calls a leaf twice ----------
Program BuildCalls(Env& env) {
  Program p;
  p.name = "calls";
  uint32_t leaf, mid;
  {
    Asm a(env.NextCodeAddress());
    a.mulli(3, 3, 3);
    a.addi(3, 3, 1);
    a.blr();
    leaf = env.PlaceCode(a);
    p.guest_words += uint32_t(a.count());
  }
  {
    Asm a(env.NextCodeAddress());
    a.mflr(0);
    a.stw(0, -8, 1);
    a.stwu(1, -96, 1);
    a.stw(29, 84, 1);
    a.mr(29, 3);
    a.bl_abs(leaf);
    a.add(29, 29, 3);
    a.mr(3, 29);
    a.bl_abs(leaf);
    a.xor_(3, 3, 29);
    a.lwz(29, 84, 1);
    a.addi(1, 1, 96);
    a.lwz(0, -8, 1);
    a.mtlr(0);
    a.blr();
    mid = env.PlaceCode(a);
    p.guest_words += uint32_t(a.count());
  }
  {
    Asm a(env.NextCodeAddress());
    Asm::Label loop;
    a.mflr(0);
    a.stw(0, -8, 1);
    a.stwu(1, -96, 1);
    a.stw(30, 80, 1);
    a.stw(31, 84, 1);
    a.mr(31, 3);
    a.li(30, 5);
    a.Bind(loop);
    a.mr(3, 30);
    a.bl_abs(mid);
    a.mr(30, 3);
    a.addi(31, 31, -1);
    a.cmpwi(6, 31, 0);
    a.bgt(6, loop);
    a.mr(3, 30);
    a.lwz(30, 80, 1);
    a.lwz(31, 84, 1);
    a.addi(1, 1, 96);
    a.lwz(0, -8, 1);
    a.mtlr(0);
    a.blr();
    p.entry = env.PlaceCode(a);
    p.guest_words += uint32_t(a.count());
  }
  p.functions = {leaf, mid, p.entry};
  p.guest_insns = [](uint32_t iterations) {
    return 7 + uint64_t(iterations) * (6 + 15 + 2 * 3) + 7;
  };
  p.prepare = [](PPCContext*) {};
  p.check = [](PPCContext* ctx, uint32_t iterations) {
    uint32_t acc = 5;
    for (uint32_t i = 0; i < iterations; ++i) {
      uint32_t x = acc;
      x += x * 3 + 1;
      acc = (x * 3 + 1) ^ x;
    }
    return uint32_t(ctx->r[3]) == acc;
  };
  return p;
}

// --- 5. Memory copy: a word loop and a byte loop with update forms -----------
Program BuildCopy(Env& env) {
  constexpr uint32_t kWords = 64, kBytes = 32;
  uint32_t src = env.AllocData(kWords * 4 + kBytes + 16, 0x1000);
  // Not at the same offset within a page as the source: a store and a load
  // 4 KiB apart look like the same address to the CPU's first check, and the
  // loop then runs at one of two speeds depending on where the heap put
  // the buffers.
  uint32_t dst = env.AllocData(0x1000 + kWords * 4 + kBytes + 16, 0x1000) + 0x7C0;
  Asm a(env.NextCodeAddress());
  Asm::Label outer, words, bytes;
  a.Bind(outer);
  a.li(6, 0);
  a.li(7, kWords);
  a.mtctr(7);
  a.Bind(words);
  a.lwzx(8, 6, 4);
  a.stwx(8, 6, 5);
  a.addi(6, 6, 4);
  a.bdnz(words);
  a.addi(9, 4, kWords * 4 - 1);
  a.addi(10, 5, kWords * 4 - 1);
  a.li(7, kBytes);
  a.mtctr(7);
  a.Bind(bytes);
  a.lbzu(8, 1, 9);
  a.stbu(8, 1, 10);
  a.bdnz(bytes);
  a.addi(3, 3, -1);
  a.cmpwi(6, 3, 0);
  a.bgt(6, outer);
  a.blr();
  Program p;
  p.name = "copy";
  p.entry = env.PlaceCode(a);
  p.functions = {p.entry};
  p.guest_words = uint32_t(a.count());
  p.guest_insns = [](uint32_t iterations) {
    return uint64_t(iterations) * (3 + kWords * 4 + 4 + kBytes * 3 + 3) + 1;
  };
  Env* e = &env;
  p.prepare = [e, src, dst](PPCContext* ctx) {
    uint8_t* host = e->Translate(src);
    for (uint32_t i = 0; i < kWords * 4 + kBytes; ++i) {
      host[i] = uint8_t(i * 31 + 7);
    }
    std::memset(e->Translate(dst), 0, kWords * 4 + kBytes);
    ctx->r[4] = src;
    ctx->r[5] = dst;
  };
  p.check = [e, src, dst](PPCContext*, uint32_t) {
    return std::memcmp(e->Translate(src), e->Translate(dst),
                       kWords * 4 + kBytes) == 0;
  };
  return p;
}

// --- 6. Calls out of translated code into a host function --------------------
uint64_t g_host_calls = 0;
void HostNothing(PPCContext* /*context*/, xe::kernel::KernelState*) {
  ++g_host_calls;
}

Program BuildHostCalls(Env& env) {
  uint32_t thunk = env.ImportThunk("BenchHostNothing", HostNothing);
  Asm a(env.NextCodeAddress());
  Asm::Label loop;
  a.mflr(0);
  a.stw(0, -8, 1);
  a.stwu(1, -96, 1);
  a.stw(31, 84, 1);
  a.mr(31, 3);
  a.Bind(loop);
  a.bl_abs(thunk);
  a.addi(31, 31, -1);
  a.cmpwi(6, 31, 0);
  a.bgt(6, loop);
  a.lwz(31, 84, 1);
  a.addi(1, 1, 96);
  a.lwz(0, -8, 1);
  a.mtlr(0);
  a.blr();
  Program p;
  p.name = "host_calls";
  p.entry = env.PlaceCode(a);
  p.functions = {thunk, p.entry};
  p.guest_words = uint32_t(a.count()) + 2;
  p.guest_insns = [](uint32_t iterations) {
    return 5 + uint64_t(iterations) * (4 + 2) + 5;
  };
  p.prepare = [](PPCContext*) { g_host_calls = 0; };
  p.check = [](PPCContext*, uint32_t iterations) {
    return g_host_calls == iterations;
  };
  return p;
}

using Builder = Program (*)(Env&);
const Builder kBuilders[] = {BuildInteger, BuildFloat, BuildVector,
                             BuildCalls,   BuildCopy,  BuildHostCalls};

}  // namespace

int RunXlatSuite(const std::string& label) {
  const std::string s = "xlat";
  Env* env = CreateEnv(false);
  uint32_t stack_size = 256 * 1024;
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
  for (Builder build : kBuilders) {
    // Translation time: several copies of the program at different
    // addresses, each translated once. The last copy is the one run.
    constexpr int kCopies = 12;
    std::vector<double> translate_us;
    Program program;
    uint64_t host_bytes = 0;
    for (int copy = 0; copy < kCopies; ++copy) {
      program = build(*env);
      host_bytes = 0;
      uint64_t t0 = NowNs();
      bool ok = true;
      for (uint32_t address : program.functions) {
        auto* fn = env->processor->ResolveFunction(address);
        if (!fn) {
          ok = false;
          break;
        }
        if (fn->is_guest()) {
          host_bytes +=
              static_cast<xe::cpu::GuestFunction*>(fn)->machine_code_length();
        }
      }
      uint64_t t1 = NowNs();
      if (!ok) {
        EmitNote(s, program.name + " (" + label + ")",
                 "NOT MEASURED: the translator refused the program");
        status = 1;
        program.entry = 0;
        break;
      }
      translate_us.push_back(double(t1 - t0) / 1000.0);
    }
    if (!program.entry) {
      continue;
    }
    std::sort(translate_us.begin(), translate_us.end());
    auto* fn = env->processor->ResolveFunction(program.entry);

    auto run = [&](uint32_t iterations) {
      ctx->r[1] = stack_top;
      ctx->r[3] = iterations;
      ctx->lr = 0xBCBCBCBC;
      fn->Call(thread_state, uint32_t(ctx->lr));
    };

    // Is the translated code right?
    bool correct = true;
    for (uint32_t iterations : {1u, 2u, 3u, 100u, 1000u}) {
      program.prepare(ctx);
      run(iterations);
      if (!program.check(ctx, iterations)) {
        correct = false;
      }
    }
    if (!correct) {
      EmitNote(s, program.name + " (" + label + ")",
               "WRONG RESULT: the translated program did not compute what "
               "the reference model computes; its timings are not reported");
      status = 1;
      continue;
    }

    // Time per guest instruction, from two run lengths so the cost of
    // entering and leaving translated code cancels.
    const uint32_t small = 64;
    program.prepare(ctx);
    uint32_t large = 4096;
    for (;;) {
      uint64_t t0 = NowNs();
      run(large);
      uint64_t dt = NowNs() - t0;
      if (dt > 2000000 || large >= (1u << 24)) {
        break;
      }
      large *= 4;
    }
    Timing t_small = Measure([&] { run(small); });
    Timing t_large = Measure([&] { run(large); });
    const double guest_delta =
        double(program.guest_insns(large) - program.guest_insns(small));
    const double ns_per_insn = (t_large.min_ns - t_small.min_ns) / guest_delta;
    const double ns_per_insn_median =
        (t_large.median_ns - t_small.median_ns) / guest_delta;

    Emit(s, program.name, label, "ns_per_guest_insn", ns_per_insn);
    Emit(s, program.name, label, "ns_per_guest_insn_median",
         std::max(ns_per_insn, ns_per_insn_median));
    if (counter.ok()) {
      uint64_t best_small = ~0ull, best_large = ~0ull;
      for (int i = 0; i < 5; ++i) {
        counter.Start();
        run(small);
        best_small = std::min(best_small, counter.Stop());
        counter.Start();
        run(large);
        best_large = std::min(best_large, counter.Stop());
      }
      Emit(s, program.name, label, "host_insn_per_guest_insn",
           double(best_large - best_small) / guest_delta);
    }
    Emit(s, program.name, label, "host_code_insn_per_guest_insn",
         double(host_bytes) / 4.0 / double(program.guest_words));
    Emit(s, program.name, label, "translate_us", translate_us.front());
    Emit(s, program.name, label, "translate_us_median",
         translate_us[translate_us.size() / 2]);
  }
#if BENCH_HAVE_TRANSLATION_STATS
  // The translator's own counters over everything translated above (each
  // program several times): whether a setting did anything at all.
  {
    auto& stats = xe::cpu::compiler::translation_stats;
    auto load = [](const std::atomic<uint64_t>& value) {
      return double(value.load(std::memory_order_relaxed));
    };
    const std::string all = "(translator counters)";
    if (load(stats.pin_functions_considered) == 0 &&
        load(stats.simplify_block_walks) == 0) {
      // Neither setting is on: nothing was counted.
    } else {
    Emit(s, all, label, "functions considered for pinning",
         load(stats.pin_functions_considered));
    Emit(s, all, label, "of those, functions with a loop",
         load(stats.pin_functions_with_loops));
    Emit(s, all, label, "functions with a register pinned",
         load(stats.pin_functions_pinned));
    Emit(s, all, label, "pin verify mismatches",
         load(stats.pin_verify_mismatches));
    Emit(s, all, label, "simplification block walks",
         load(stats.simplify_block_walks));
    Emit(s, all, label, "simplification block walks skipped",
         load(stats.simplify_block_skips));
    Emit(s, all, label, "simplification skip mismatches",
         load(stats.simplify_skip_mismatches));
    }
  }
#else
  EmitNote(s, "(translator counters)",
           "not reported: this tree's cpu/compiler/translation_stats.h does "
           "not have the counters the harness reads");
#endif
  if (!counter.ok()) {
    EmitNote(s, "host_insn_per_guest_insn",
             "not measured: the runner does not expose an instruction "
             "counter (" + counter.why_not() + ")");
  }
  return status;
}

}  // namespace bench
