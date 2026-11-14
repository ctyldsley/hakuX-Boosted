// hakuX-Boosted bench: a very small PowerPC assembler, enough to write the
// synthetic guest loops in bench_xlat.cc and bench_sched.cc. Every program
// built with it is written for this rig; none of it comes from a title.
//
// Registers are plain numbers (r3 is 3, f1 is 1, v2 is 2). Branch targets are
// labels: Bind() fixes a label at the current position, and branches to a
// label not yet bound are patched when it is.
#ifndef BOOSTED_BENCH_CORE_PPC_ASM_H_
#define BOOSTED_BENCH_CORE_PPC_ASM_H_

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

namespace bench {
namespace ppc {

enum CrBit { kLT = 0, kGT = 1, kEQ = 2 };

class Assembler {
 public:
  explicit Assembler(uint32_t base_address) : base_(base_address) {}

  struct Label {
    int32_t position = -1;            // word index once bound
    std::vector<uint32_t> fixups;     // word indices of branches to patch
  };

  uint32_t base() const { return base_; }
  uint32_t here() const { return base_ + uint32_t(words_.size()) * 4; }
  size_t count() const { return words_.size(); }
  const std::vector<uint32_t>& words() const { return words_; }

  // Copies the program to guest memory, big-endian as the console stores it.
  void CopyTo(uint8_t* host) const {
    for (size_t i = 0; i < words_.size(); ++i) {
      uint32_t w = words_[i];
      host[i * 4 + 0] = uint8_t(w >> 24);
      host[i * 4 + 1] = uint8_t(w >> 16);
      host[i * 4 + 2] = uint8_t(w >> 8);
      host[i * 4 + 3] = uint8_t(w);
    }
  }

  void Bind(Label& label) {
    label.position = int32_t(words_.size());
    for (uint32_t at : label.fixups) {
      Patch(at, label.position);
    }
    label.fixups.clear();
  }

  void Raw(uint32_t word) { words_.push_back(word); }

  // --- Integer, immediate ---
  void addi(int rd, int ra, int simm) { D(14, rd, ra, simm); }
  void addis(int rd, int ra, int simm) { D(15, rd, ra, simm); }
  void li(int rd, int simm) { addi(rd, 0, simm); }
  void lis(int rd, int simm) { addis(rd, 0, simm); }
  void mulli(int rd, int ra, int simm) { D(7, rd, ra, simm); }
  void ori(int ra, int rs, uint32_t uimm) { D(24, rs, ra, int(uimm)); }
  void xori(int ra, int rs, uint32_t uimm) { D(26, rs, ra, int(uimm)); }
  void andi_(int ra, int rs, uint32_t uimm) { D(28, rs, ra, int(uimm)); }
  void nop() { ori(0, 0, 0); }
  // Loads a full 32-bit constant.
  void li32(int rd, uint32_t value) {
    lis(rd, int16_t(value >> 16));
    ori(rd, rd, value & 0xFFFF);
  }
  void cmpwi(int crf, int ra, int simm) {
    Raw(11u << 26 | uint32_t(crf) << 23 | uint32_t(ra) << 16 |
        (uint32_t(simm) & 0xFFFF));
  }
  void cmplwi(int crf, int ra, uint32_t uimm) {
    Raw(10u << 26 | uint32_t(crf) << 23 | uint32_t(ra) << 16 |
        (uimm & 0xFFFF));
  }

  // --- Integer, register ---
  void add(int rd, int ra, int rb) { X(31, rd, ra, rb, 266); }
  void subf(int rd, int ra, int rb) { X(31, rd, ra, rb, 40); }  // rb - ra
  void mullw(int rd, int ra, int rb) { X(31, rd, ra, rb, 235); }
  void neg(int rd, int ra) { X(31, rd, ra, 0, 104); }
  void and_(int ra, int rs, int rb) { X(31, rs, ra, rb, 28); }
  void or_(int ra, int rs, int rb) { X(31, rs, ra, rb, 444); }
  void xor_(int ra, int rs, int rb) { X(31, rs, ra, rb, 316); }
  void mr(int ra, int rs) { or_(ra, rs, rs); }
  void slw(int ra, int rs, int rb) { X(31, rs, ra, rb, 24); }
  void srw(int ra, int rs, int rb) { X(31, rs, ra, rb, 536); }
  void srawi(int ra, int rs, int sh) { X(31, rs, ra, sh, 824); }
  void extsb(int ra, int rs) { X(31, rs, ra, 0, 954); }
  void cntlzw(int ra, int rs) { X(31, rs, ra, 0, 26); }
  void cmpw(int crf, int ra, int rb) {
    Raw(31u << 26 | uint32_t(crf) << 23 | uint32_t(ra) << 16 |
        uint32_t(rb) << 11);
  }
  void cmplw(int crf, int ra, int rb) {
    Raw(31u << 26 | uint32_t(crf) << 23 | uint32_t(ra) << 16 |
        uint32_t(rb) << 11 | 32u << 1);
  }
  void rlwinm(int ra, int rs, int sh, int mb, int me) {
    Raw(21u << 26 | uint32_t(rs) << 21 | uint32_t(ra) << 16 |
        uint32_t(sh) << 11 | uint32_t(mb) << 6 | uint32_t(me) << 1);
  }
  void slwi(int ra, int rs, int n) { rlwinm(ra, rs, n, 0, 31 - n); }
  void srwi(int ra, int rs, int n) { rlwinm(ra, rs, 32 - n, n, 31); }
  void rotlwi(int ra, int rs, int n) { rlwinm(ra, rs, n, 0, 31); }

  // --- Loads and stores ---
  void lwz(int rd, int d, int ra) { D(32, rd, ra, d); }
  void lwzu(int rd, int d, int ra) { D(33, rd, ra, d); }
  void lbz(int rd, int d, int ra) { D(34, rd, ra, d); }
  void lbzu(int rd, int d, int ra) { D(35, rd, ra, d); }
  void stw(int rs, int d, int ra) { D(36, rs, ra, d); }
  void stwu(int rs, int d, int ra) { D(37, rs, ra, d); }
  void stb(int rs, int d, int ra) { D(38, rs, ra, d); }
  void stbu(int rs, int d, int ra) { D(39, rs, ra, d); }
  void lhz(int rd, int d, int ra) { D(40, rd, ra, d); }
  void sth(int rs, int d, int ra) { D(44, rs, ra, d); }
  void lfs(int frd, int d, int ra) { D(48, frd, ra, d); }
  void lfd(int frd, int d, int ra) { D(50, frd, ra, d); }
  void stfs(int frs, int d, int ra) { D(52, frs, ra, d); }
  void stfd(int frs, int d, int ra) { D(54, frs, ra, d); }
  void lwzx(int rd, int ra, int rb) { X(31, rd, ra, rb, 23); }
  void stwx(int rs, int ra, int rb) { X(31, rs, ra, rb, 151); }
  void lbzx(int rd, int ra, int rb) { X(31, rd, ra, rb, 87); }
  void stbx(int rs, int ra, int rb) { X(31, rs, ra, rb, 215); }

  // --- Floating point (double unless the name ends in s) ---
  void fadd(int frd, int fra, int frb) { A(63, frd, fra, frb, 0, 21); }
  void fsub(int frd, int fra, int frb) { A(63, frd, fra, frb, 0, 20); }
  void fmul(int frd, int fra, int frc) { A(63, frd, fra, 0, frc, 25); }
  void fdiv(int frd, int fra, int frb) { A(63, frd, fra, frb, 0, 18); }
  void fmadd(int frd, int fra, int frc, int frb) {
    A(63, frd, fra, frb, frc, 29);
  }
  void fmsub(int frd, int fra, int frc, int frb) {
    A(63, frd, fra, frb, frc, 28);
  }
  void fadds(int frd, int fra, int frb) { A(59, frd, fra, frb, 0, 21); }
  void fmuls(int frd, int fra, int frc) { A(59, frd, fra, 0, frc, 25); }
  void fmadds(int frd, int fra, int frc, int frb) {
    A(59, frd, fra, frb, frc, 29);
  }
  void fmr(int frd, int frb) { X(63, frd, 0, frb, 72); }
  void fneg(int frd, int frb) { X(63, frd, 0, frb, 40); }
  void fabs_(int frd, int frb) { X(63, frd, 0, frb, 264); }
  void fcmpu(int crf, int fra, int frb) {
    Raw(63u << 26 | uint32_t(crf) << 23 | uint32_t(fra) << 16 |
        uint32_t(frb) << 11);
  }

  // --- AltiVec ---
  void lvx(int vd, int ra, int rb) { X(31, vd, ra, rb, 103); }
  void stvx(int vs, int ra, int rb) { X(31, vs, ra, rb, 231); }
  void vaddfp(int vd, int va, int vb) { VX(vd, va, vb, 10); }
  void vsubfp(int vd, int va, int vb) { VX(vd, va, vb, 74); }
  void vmaxfp(int vd, int va, int vb) { VX(vd, va, vb, 1034); }
  void vminfp(int vd, int va, int vb) { VX(vd, va, vb, 1098); }
  void vadduwm(int vd, int va, int vb) { VX(vd, va, vb, 128); }
  void vand(int vd, int va, int vb) { VX(vd, va, vb, 1028); }
  void vor(int vd, int va, int vb) { VX(vd, va, vb, 1156); }
  void vxor(int vd, int va, int vb) { VX(vd, va, vb, 1220); }
  void vspltisw(int vd, int simm) { VX(vd, simm & 0x1F, 0, 908); }
  // vd = va * vc + vb
  void vmaddfp(int vd, int va, int vc, int vb) {
    Raw(4u << 26 | uint32_t(vd) << 21 | uint32_t(va) << 16 |
        uint32_t(vb) << 11 | uint32_t(vc) << 6 | 46u);
  }

  // --- Branches and special registers ---
  void mflr(int rd) { Raw(0x7C0802A6u | uint32_t(rd) << 21); }
  void mtlr(int rs) { Raw(0x7C0803A6u | uint32_t(rs) << 21); }
  void mtctr(int rs) { Raw(0x7C0903A6u | uint32_t(rs) << 21); }
  void blr() { Raw(0x4E800020u); }
  void bctr() { Raw(0x4E800420u); }
  // The core's import thunk: "sc 2", see cpu/xex_module.cc.
  void sc2() { Raw(0x44000042u); }

  void b(Label& target) { Branch(18u << 26, target, false); }
  void bl(Label& target) { Branch(18u << 26 | 1u, target, false); }
  void bl_abs(uint32_t address) {
    int32_t offset = int32_t(address - here());
    Raw(18u << 26 | (uint32_t(offset) & 0x03FFFFFCu) | 1u);
  }
  void bdnz(Label& target) { Branch(16u << 26 | 16u << 21, target, true); }
  void bt(int crf, CrBit bit, Label& target) {
    Branch(16u << 26 | 12u << 21 | uint32_t(crf * 4 + bit) << 16, target,
           true);
  }
  void bf(int crf, CrBit bit, Label& target) {
    Branch(16u << 26 | 4u << 21 | uint32_t(crf * 4 + bit) << 16, target, true);
  }
  void blt(int crf, Label& target) { bt(crf, kLT, target); }
  void bgt(int crf, Label& target) { bt(crf, kGT, target); }
  void beq(int crf, Label& target) { bt(crf, kEQ, target); }
  void bne(int crf, Label& target) { bf(crf, kEQ, target); }
  void bge(int crf, Label& target) { bf(crf, kLT, target); }

 private:
  void D(uint32_t op, int rt, int ra, int imm) {
    Raw(op << 26 | uint32_t(rt) << 21 | uint32_t(ra) << 16 |
        (uint32_t(imm) & 0xFFFF));
  }
  void X(uint32_t op, int rt, int ra, int rb, uint32_t xo) {
    Raw(op << 26 | uint32_t(rt) << 21 | uint32_t(ra) << 16 |
        uint32_t(rb) << 11 | xo << 1);
  }
  void A(uint32_t op, int frt, int fra, int frb, int frc, uint32_t xo) {
    Raw(op << 26 | uint32_t(frt) << 21 | uint32_t(fra) << 16 |
        uint32_t(frb) << 11 | uint32_t(frc) << 6 | xo << 1);
  }
  void VX(int vd, int va, int vb, uint32_t xo) {
    Raw(4u << 26 | uint32_t(vd) << 21 | uint32_t(va) << 16 |
        uint32_t(vb) << 11 | xo);
  }
  void Branch(uint32_t word, Label& target, bool conditional) {
    uint32_t at = uint32_t(words_.size());
    conditional_.resize(at + 1, false);
    conditional_[at] = conditional;
    Raw(word);
    if (target.position >= 0) {
      Patch(at, target.position);
    } else {
      target.fixups.push_back(at);
    }
  }
  void Patch(uint32_t at, int32_t target_position) {
    int32_t offset = (target_position - int32_t(at)) * 4;
    if (conditional_[at]) {
      assert(offset >= -32768 && offset < 32768);
      words_[at] |= uint32_t(offset) & 0xFFFCu;
    } else {
      words_[at] |= uint32_t(offset) & 0x03FFFFFCu;
    }
  }

  uint32_t base_;
  std::vector<uint32_t> words_;
  std::vector<bool> conditional_;
};

}  // namespace ppc
}  // namespace bench

#endif  // BOOSTED_BENCH_CORE_PPC_ASM_H_
