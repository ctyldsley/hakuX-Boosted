// hakuX-Boosted: the fork's own file. Part of the first-run rig (README.md
// here), not of the app: a small Xenos microcode assembler for the rig's own
// test shaders. Bit layouts are those of xenia/gpu/ucode.h. No game's
// microcode is, or may be, in the repository.

#ifndef XENIA_GPU_FIRST_RUN_RIG_RIG_ASM_H_
#define XENIA_GPU_FIRST_RUN_RIG_RIG_ASM_H_

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace rig {

struct Ins {
  uint32_t dwords[3] = {};
  bool is_fetch = false;
};

inline uint32_t ComponentIndex(char c) {
  switch (c) {
    case 'x':
      return 0;
    case 'y':
      return 1;
    case 'z':
      return 2;
    default:
      return 3;
  }
}

// ALU source swizzles are relative: component i reads (i + bits) & 3.
inline uint32_t AluSwizzle(const char* swizzle) {
  uint32_t bits = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    bits |= ((ComponentIndex(swizzle[i]) - i) & 3) << (2 * i);
  }
  return bits;
}

inline uint32_t WriteMask(const char* mask) {
  uint32_t bits = 0;
  for (uint32_t i = 0; mask[i]; ++i) {
    if (mask[i] != '_') {
      bits |= 1u << i;
    }
  }
  return bits;
}

struct Src {
  // The 8-bit source field: a float constant's index, or for a register its
  // index with bit 6 for aL-relative and bit 7 for the absolute value.
  uint32_t field = 0;
  bool is_temp = true;
  uint32_t swizzle = 0;
  bool negate = false;
};
inline Src R(uint32_t index, const char* swizzle = "xyzw", bool negate = false,
             bool absolute = false, bool relative = false) {
  return Src{index | (relative ? 0x40u : 0) | (absolute ? 0x80u : 0), true,
             AluSwizzle(swizzle), negate};
}
inline Src C(uint32_t index, const char* swizzle = "xyzw",
             bool negate = false) {
  return Src{index, false, AluSwizzle(swizzle), negate};
}

struct Alu {
  uint32_t vector_op = 2;  // max
  uint32_t scalar_op = 50;  // retain_prev
  uint32_t vector_dest = 0, scalar_dest = 0;
  bool vector_dest_relative = false, scalar_dest_relative = false;
  uint32_t vector_mask = 0, scalar_mask = 0;
  bool vector_clamp = false, scalar_clamp = false;
  bool is_export = false;
  bool abs_constants = false;
  Src src1, src2, src3;
  bool predicated = false, predicate_condition = false;
  // How addressed constants are addressed: by a0 (true) or by aL.
  bool const_a0_relative = false;
  bool const_0_addressed = false, const_1_addressed = false;

  Ins Encode() const {
    Ins ins;
    ins.dwords[0] =
        vector_dest | (vector_dest_relative ? 1u << 6 : 0) |
        (abs_constants ? 1u << 7 : 0) | (scalar_dest << 8) |
        (scalar_dest_relative ? 1u << 14 : 0) | (is_export ? 1u << 15 : 0) |
        (vector_mask << 16) | (scalar_mask << 20) |
        (vector_clamp ? 1u << 24 : 0) | (scalar_clamp ? 1u << 25 : 0) |
        (scalar_op << 26);
    ins.dwords[1] =
        src3.swizzle | (src2.swizzle << 8) | (src1.swizzle << 16) |
        (src3.negate ? 1u << 24 : 0) | (src2.negate ? 1u << 25 : 0) |
        (src1.negate ? 1u << 26 : 0) | (predicate_condition ? 1u << 27 : 0) |
        (predicated ? 1u << 28 : 0) | (const_a0_relative ? 1u << 29 : 0) |
        (const_1_addressed ? 1u << 30 : 0) | (const_0_addressed ? 1u << 31 : 0);
    ins.dwords[2] = src3.field | (src2.field << 8) | (src1.field << 16) |
                    (vector_op << 24) | (src3.is_temp ? 1u << 29 : 0) |
                    (src2.is_temp ? 1u << 30 : 0) |
                    (src1.is_temp ? 1u << 31 : 0);
    return ins;
  }
};

// A vector operation alone.
inline Alu Vec(uint32_t op, uint32_t dest, const char* mask, Src a,
               Src b = Src(), Src c = Src()) {
  Alu alu;
  alu.vector_op = op;
  alu.vector_dest = dest;
  alu.vector_mask = WriteMask(mask);
  alu.src1 = a;
  alu.src2 = b;
  alu.src3 = c;
  return alu;
}
// A scalar operation alone (the vector half an unwritten max of r0).
inline Alu Scal(uint32_t op, uint32_t dest, const char* mask, Src a) {
  Alu alu;
  alu.scalar_op = op;
  alu.scalar_dest = dest;
  alu.scalar_mask = WriteMask(mask);
  alu.src3 = a;
  return alu;
}
// An export of a register: oPos (62), an interpolator or a colour (0 to 15).
inline Alu Export(uint32_t export_register, Src a, const char* mask = "xyzw") {
  Alu alu = Vec(2, export_register, mask, a, a);
  alu.is_export = true;
  return alu;
}

// vfetch (full or mini) dest.xyzw = vf<constant> at an offset in words.
inline Ins VertexFetch(uint32_t dest, uint32_t src, uint32_t fetch_constant,
                       uint32_t format, uint32_t stride_words,
                       int32_t offset_words, bool mini = false,
                       bool is_signed = false, bool is_integer = false,
                       int32_t exp_adjust = 0, bool no_zero = false,
                       bool round_index = false) {
  Ins ins;
  ins.is_fetch = true;
  const uint32_t kDestXyzw = 0 | (1 << 3) | (2 << 6) | (3 << 9);
  ins.dwords[0] = 0 /* kVertexFetch */ | (src << 5) | (dest << 12) |
                  (1u << 19) | ((fetch_constant / 3) << 20) |
                  ((fetch_constant % 3) << 25);
  ins.dwords[1] = kDestXyzw | (is_signed ? 1u << 12 : 0) |
                  (is_integer ? 1u << 13 : 0) | (no_zero ? 1u << 14 : 0) |
                  (round_index ? 1u << 15 : 0) | (format << 16) |
                  ((uint32_t(exp_adjust) & 0x3F) << 24) | (mini ? 1u << 30 : 0);
  ins.dwords[2] = stride_words | ((uint32_t(offset_words) & 0x7FFFFF) << 8);
  return ins;
}

// tfetch dest.xyzw = tf<constant> at src (dimension as
// xenos::FetchOpDimension), filters and LOD from the fetch constant.
inline Ins TextureFetch(uint32_t dest, uint32_t src, uint32_t fetch_constant,
                        uint32_t dimension, const char* src_swizzle = "xyz",
                        bool unnormalized = false,
                        bool use_computed_lod = true) {
  Ins ins;
  ins.is_fetch = true;
  uint32_t swizzle = ComponentIndex(src_swizzle[0]) |
                     (ComponentIndex(src_swizzle[1]) << 2) |
                     (ComponentIndex(src_swizzle[2]) << 4);
  const uint32_t kDestXyzw = 0 | (1 << 3) | (2 << 6) | (3 << 9);
  ins.dwords[0] = 1 /* kTextureFetch */ | (src << 5) | (dest << 12) |
                  (fetch_constant << 20) | (unnormalized ? 1u << 25 : 0) |
                  (swizzle << 26);
  ins.dwords[1] = kDestXyzw | (3u << 12) | (3u << 14) | (3u << 16) |
                  (7u << 18) | (7u << 21) | (3u << 24) | (3u << 26) |
                  (use_computed_lod ? 1u << 28 : 0);
  ins.dwords[2] = dimension << 14;
  return ins;
}

// Control flow in front of the instructions. Labels are control flow
// instruction indices, bound when the next one is added.
class Asm {
 public:
  enum class Condition {
    kAlways,
    kBool,
    kPredicate,
  };

  int NewLabel() {
    labels_.push_back(-1);
    return int(labels_.size()) - 1;
  }
  void Bind(int label) { labels_[label] = int(cfs_.size()); }

  // Up to six instructions an exec; more are split into as many execs as it
  // takes, each with the condition. `end`: the last of them ends the shader.
  void Exec(const std::vector<Ins>& instructions,
            Condition condition = Condition::kAlways, uint32_t bool_index = 0,
            bool expected = true, bool end = false) {
    size_t done = 0;
    do {
      size_t count = std::min(size_t(6), instructions.size() - done);
      Cf cf;
      cf.kind = Cf::Kind::kExec;
      cf.first_instruction = uint32_t(instructions_.size());
      cf.instruction_count = uint32_t(count);
      for (size_t i = 0; i < count; ++i) {
        const Ins& ins = instructions[done + i];
        if (ins.is_fetch) {
          cf.sequence |= 1u << (2 * i);
        }
        instructions_.push_back(ins);
      }
      done += count;
      cf.condition = condition;
      cf.bool_index = bool_index;
      cf.expected = expected;
      cf.end = end && done == instructions.size();
      cfs_.push_back(cf);
    } while (done < instructions.size());
  }
  void Exec(const std::vector<Alu>& alus,
            Condition condition = Condition::kAlways, uint32_t bool_index = 0,
            bool expected = true, bool end = false) {
    std::vector<Ins> instructions;
    for (const Alu& alu : alus) {
      instructions.push_back(alu.Encode());
    }
    Exec(instructions, condition, bool_index, expected, end);
  }
  void End() { Exec(std::vector<Ins>(), Condition::kAlways, 0, true, true); }

  void LoopStart(uint32_t loop_id, int skip_label, bool repeat = false) {
    Cf cf;
    cf.kind = Cf::Kind::kLoopStart;
    cf.loop_id = loop_id;
    cf.label = skip_label;
    cf.repeat = repeat;
    cfs_.push_back(cf);
  }
  void LoopEnd(uint32_t loop_id, int body_label, bool predicated_break = false,
               bool expected = false) {
    Cf cf;
    cf.kind = Cf::Kind::kLoopEnd;
    cf.loop_id = loop_id;
    cf.label = body_label;
    cf.condition =
        predicated_break ? Condition::kPredicate : Condition::kAlways;
    cf.expected = expected;
    cfs_.push_back(cf);
  }
  void Jump(int label, Condition condition = Condition::kAlways,
            uint32_t bool_index = 0, bool expected = true) {
    Cf cf;
    cf.kind = Cf::Kind::kJump;
    cf.label = label;
    cf.condition = condition;
    cf.bool_index = bool_index;
    cf.expected = expected;
    cfs_.push_back(cf);
  }

  std::vector<uint32_t> Assemble() const {
    size_t cf_count = (cfs_.size() + 1) & ~size_t(1);
    uint32_t first_instruction = uint32_t(cf_count / 2);
    std::vector<uint32_t> words_0(cf_count), words_1(cf_count);
    for (size_t i = 0; i < cfs_.size(); ++i) {
      const Cf& cf = cfs_[i];
      uint32_t condition_bit = cf.expected ? 1u << 10 : 0;
      uint32_t target = 0;
      if (cf.kind != Cf::Kind::kExec) {
        if (labels_[cf.label] < 0) {
          std::fprintf(stderr, "rig: unbound label\n");
          std::exit(1);
        }
        target = uint32_t(labels_[cf.label]);
      }
      switch (cf.kind) {
        case Cf::Kind::kExec: {
          words_0[i] = (first_instruction + cf.first_instruction) |
                       (cf.instruction_count << 12) | (cf.sequence << 16);
          uint32_t opcode;
          switch (cf.condition) {
            case Condition::kBool:
              opcode = cf.end ? 4 : 3;
              words_1[i] = (cf.bool_index << 2) | condition_bit;
              break;
            case Condition::kPredicate:
              opcode = cf.end ? 6 : 5;
              words_1[i] = condition_bit;
              break;
            default:
              opcode = cf.end ? 2 : 1;
              break;
          }
          words_1[i] |= opcode << 12;
        } break;
        case Cf::Kind::kLoopStart:
          words_0[i] =
              target | (cf.repeat ? 1u << 13 : 0) | (cf.loop_id << 16);
          words_1[i] = 7u << 12;
          break;
        case Cf::Kind::kLoopEnd:
          words_0[i] =
              target | (cf.loop_id << 16) |
              (cf.condition == Condition::kPredicate ? 1u << 21 : 0);
          words_1[i] = (8u << 12) | condition_bit;
          break;
        case Cf::Kind::kJump:
          words_0[i] = target |
                       (cf.condition == Condition::kAlways ? 1u << 13 : 0) |
                       (cf.condition == Condition::kPredicate ? 1u << 14 : 0);
          words_1[i] = (11u << 12) | condition_bit | (cf.bool_index << 2);
          break;
      }
    }
    std::vector<uint32_t> dwords;
    for (size_t i = 0; i < cf_count; i += 2) {
      dwords.push_back(words_0[i]);
      dwords.push_back((words_1[i] & 0xFFFF) | (words_0[i + 1] << 16));
      dwords.push_back((words_0[i + 1] >> 16) | (words_1[i + 1] << 16));
    }
    for (const Ins& ins : instructions_) {
      dwords.insert(dwords.end(), ins.dwords, ins.dwords + 3);
    }
    return dwords;
  }

 private:
  struct Cf {
    enum class Kind { kExec, kLoopStart, kLoopEnd, kJump };
    Kind kind = Kind::kExec;
    uint32_t first_instruction = 0, instruction_count = 0, sequence = 0;
    Condition condition = Condition::kAlways;
    uint32_t bool_index = 0;
    bool expected = true;
    bool end = false;
    uint32_t loop_id = 0;
    int label = 0;
    bool repeat = false;
  };
  std::vector<Cf> cfs_;
  std::vector<Ins> instructions_;
  std::vector<int> labels_;
};

}  // namespace rig

#endif  // XENIA_GPU_FIRST_RUN_RIG_RIG_ASM_H_
