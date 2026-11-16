# hakuX-Boosted: a64_fpcr_known_block_entry. Scalar float code runs with
# FPCR holding the guest's rounding mode, VMX float code with a fixed one
# (round to nearest); a block that starts in the mode its predecessors left
# does not write FPCR again. Every test sets the guest to round up and
# divides 1.0 by 3.0 in a block reached in more than one way: the quotient
# ends in ...5556 rounded up and in ...5555 rounded to nearest, which is
# what it would be if the block wrongly took FPCR for already scalar.

# (The mode is set with mtfsf from a value built in memory. Not mtfsfi:
# the front end takes that instruction's immediate from a five-bit field
# of which it is the upper four bits, so "mtfsfi 7, 2" does not select
# rounding up here. Recorded in boosted/notes/translator-overhaul.md.)
.macro ROUND_UP
  lis r4, 0x1000
  ori r4, r4, 0x1100
  li r0, 2
  std r0, 8(r4)
  lfd f7, 8(r4)
  mtfsf 0xFF, f7
.endm
.macro QUOTIENT_OUT
  stfd f4, 0(r4)
  ld r7, 0(r4)
  li r0, 0
  std r0, 8(r4)
  lfd f7, 8(r4)
  mtfsf 0xFF, f7
.endm

# The two ways into the last block disagree: scalar on one, VMX on the
# other. Taken through the scalar arm.
test_fpcr_join_scalar_arm:
  #_ REGISTER_IN r3 1
  #_ REGISTER_IN f2 1.5
  #_ REGISTER_IN f3 2.5
  #_ REGISTER_IN f5 1.0
  #_ REGISTER_IN f6 3.0
  ROUND_UP
  cmpwi r3, 0
  beq fj1_vector
  fadd f1, f2, f3
  b fj1_join
fj1_vector:
  vaddfp v1, v2, v3
fj1_join:
  fdiv f4, f5, f6
  QUOTIENT_OUT
  blr
  #_ REGISTER_OUT r7 0x3FD5555555555556

# The same, through the VMX arm.
test_fpcr_join_vector_arm:
  #_ REGISTER_IN r3 0
  #_ REGISTER_IN f2 1.5
  #_ REGISTER_IN f3 2.5
  #_ REGISTER_IN f5 1.0
  #_ REGISTER_IN f6 3.0
  ROUND_UP
  cmpwi r3, 0
  beq fj2_vector
  fadd f1, f2, f3
  b fj2_join
fj2_vector:
  vaddfp v1, v2, v3
fj2_join:
  fdiv f4, f5, f6
  QUOTIENT_OUT
  blr
  #_ REGISTER_OUT r7 0x3FD5555555555556

# Both ways in are scalar: the last block starts scalar and writes nothing.
test_fpcr_join_agrees:
  #_ REGISTER_IN r3 1
  #_ REGISTER_IN f2 1.5
  #_ REGISTER_IN f3 2.5
  #_ REGISTER_IN f5 1.0
  #_ REGISTER_IN f6 3.0
  ROUND_UP
  fadd f1, f2, f3
  cmpwi r3, 0
  beq fj3_join
  fmul f1, f1, f2
fj3_join:
  fdiv f4, f5, f6
  QUOTIENT_OUT
  blr
  #_ REGISTER_OUT r7 0x3FD5555555555556

# A loop whose body ends in VMX: its first block is entered from above in
# scalar mode and from its own end in VMX mode.
test_fpcr_loop_header:
  #_ REGISTER_IN f2 1.5
  #_ REGISTER_IN f3 2.5
  #_ REGISTER_IN f5 1.0
  #_ REGISTER_IN f6 3.0
  ROUND_UP
  fadd f1, f2, f3
  li r6, 3
  mtctr r6
fj4_loop:
  fdiv f4, f5, f6
  vaddfp v1, v2, v3
  bdnz fj4_loop
  QUOTIENT_OUT
  blr
  #_ REGISTER_OUT r7 0x3FD5555555555556

# Both ways in are VMX, the block does VMX and then scalar.
test_fpcr_vector_arms:
  #_ REGISTER_IN r3 1
  #_ REGISTER_IN f5 1.0
  #_ REGISTER_IN f6 3.0
  ROUND_UP
  cmpwi r3, 0
  beq fj5_other
  vaddfp v1, v2, v3
  b fj5_join
fj5_other:
  vsubfp v1, v2, v3
fj5_join:
  vmaddfp v4, v1, v2, v3
  fdiv f4, f5, f6
  QUOTIENT_OUT
  blr
  #_ REGISTER_OUT r7 0x3FD5555555555556

# After a call whose callee ends in VMX code.
test_fpcr_after_call:
  #_ REGISTER_IN r3 1
  #_ REGISTER_IN f2 1.5
  #_ REGISTER_IN f3 2.5
  #_ REGISTER_IN f5 1.0
  #_ REGISTER_IN f6 3.0
  mflr r30
  ROUND_UP
  fadd f1, f2, f3
  bl fj6_callee
  fdiv f4, f5, f6
  QUOTIENT_OUT
  mtlr r30
  blr
  #_ REGISTER_OUT r7 0x3FD5555555555556
fj6_callee:
  cmpwi r3, 0
  beq fj6_skip
  vaddfp v1, v2, v3
fj6_skip:
  vsubfp v5, v2, v3
  blr
