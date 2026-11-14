# hakuX-Boosted: condition register bits, the carry and CTR/LR set in one
# block and used in another, or over a call.
#
# Written for the translator's work on flags (stores of flag bits nothing
# reads, compares fused with branches, flags held in host registers): every
# test reads, somewhere later and often on one path only, a bit that a
# shorter-sighted translator could take for dead.
#
# The expected values were taken from a run of the unmodified translator
# (all settings off) and checked by hand for the short ones.

.macro FRAME_IN
  mflr r0
  stwu r1, -96(r1)
  stw r0, 100(r1)
.endm
.macro FRAME_OUT
  lwz r0, 100(r1)
  addi r1, r1, 96
  mtlr r0
.endm

# One compare, three later branches on its three bits, in other blocks.
test_cr_bits_used_later:
  #_ REGISTER_IN r3 5
  #_ REGISTER_IN r4 9
  li r5, 0
  cmpw r3, r4
  addi r6, r3, 1
  b cr_later_1
cr_later_1:
  bge cr_later_2
  ori r5, r5, 1
cr_later_2:
  addi r6, r6, 1
  ble cr_later_3
  ori r5, r5, 2
cr_later_3:
  bne cr_later_4
  ori r5, r5, 4
cr_later_4:
  mfcr r7
  blr
  #_ REGISTER_OUT r5 1
  #_ REGISTER_OUT r7 0x80000000

# The same with equal and greater inputs, through one function, to cover the
# other two bits.
test_cr_bits_used_later_eq:
  #_ REGISTER_IN r3 9
  #_ REGISTER_IN r4 9
  li r5, 0
  cmpw r3, r4
  b cr_later_eq_1
cr_later_eq_1:
  bge cr_later_eq_2
  ori r5, r5, 1
cr_later_eq_2:
  ble cr_later_eq_3
  ori r5, r5, 2
cr_later_eq_3:
  bne cr_later_eq_4
  ori r5, r5, 4
cr_later_eq_4:
  mfcr r7
  blr
  #_ REGISTER_OUT r5 4
  #_ REGISTER_OUT r7 0x20000000

test_cr_bits_used_later_gt:
  #_ REGISTER_IN r3 10
  #_ REGISTER_IN r4 9
  li r5, 0
  cmpw r3, r4
  b cr_later_gt_1
cr_later_gt_1:
  bge cr_later_gt_2
  ori r5, r5, 1
cr_later_gt_2:
  ble cr_later_gt_3
  ori r5, r5, 2
cr_later_gt_3:
  bne cr_later_gt_4
  ori r5, r5, 4
cr_later_gt_4:
  mfcr r7
  blr
  #_ REGISTER_OUT r5 2
  #_ REGISTER_OUT r7 0x40000000

# A compare overwritten on one path only: the first compare's bits must
# survive on the other.
test_cr_overwritten_on_one_path:
  #_ REGISTER_IN r3 1
  #_ REGISTER_IN r4 2
  #_ REGISTER_IN r8 0
  cmpw r3, r4
  cmpwi cr1, r8, 0
  bne cr1, cr_one_path_skip
  cmpw r4, r3
cr_one_path_skip:
  li r5, 0
  bge cr_one_path_done
  li r5, 1
cr_one_path_done:
  mfcr r7
  blr
  #_ REGISTER_OUT r5 0
  #_ REGISTER_OUT r7 0x42000000

test_cr_overwritten_on_one_path_other:
  #_ REGISTER_IN r3 1
  #_ REGISTER_IN r4 2
  #_ REGISTER_IN r8 1
  cmpw r3, r4
  cmpwi cr1, r8, 0
  bne cr1, cr_one_path_o_skip
  cmpw r4, r3
cr_one_path_o_skip:
  li r5, 0
  bge cr_one_path_o_done
  li r5, 1
cr_one_path_o_done:
  mfcr r7
  blr
  #_ REGISTER_OUT r5 1
  #_ REGISTER_OUT r7 0x84000000

# Flags set before a call and read after it; the callee leaves cr0 alone
# and changes cr7.
test_cr_across_call:
  #_ REGISTER_IN r3 3
  #_ REGISTER_IN r4 4
  FRAME_IN
  cmpw r3, r4
  cmpwi cr7, r3, 3
  bl cr_callee
  li r5, 0
  bge cr_across_1
  ori r5, r5, 1
cr_across_1:
  bne cr7, cr_across_2
  ori r5, r5, 2
cr_across_2:
  mfcr r7
  FRAME_OUT
  blr
  #_ REGISTER_OUT r5 1
  #_ REGISTER_OUT r7 0x80000008
cr_callee:
  cmpwi cr7, r4, 100
  blr

# The callee sets flags its caller branches on (a function returning its
# answer in cr0, as hand-written leaf code does).
test_cr_returned_by_callee:
  #_ REGISTER_IN r3 20
  FRAME_IN
  li r5, 0
  li r6, 30
cr_returned_loop:
  bl cr_returning_callee
  bne cr_returned_skip
  addi r5, r5, 1
cr_returned_skip:
  bgt cr_returned_skip2
  addi r5, r5, 16
cr_returned_skip2:
  addi r3, r3, -3
  addic. r6, r6, -1
  bne cr_returned_loop
  FRAME_OUT
  blr
  #_ REGISTER_OUT r5 0x88
cr_returning_callee:
  andi. r0, r3, 3
  blr

# A loop whose compare bits are all rewritten every trip, left on a
# different bit than it loops on, with the remaining bits read afterwards.
test_cr_loop_exit_bits:
  #_ REGISTER_IN r3 0
  #_ REGISTER_IN r4 37
  li r5, 0
cr_loop_exit:
  addi r3, r3, 5
  add r5, r5, r3
  cmpw r3, r4
  blt cr_loop_exit
  li r6, 0
  beq cr_loop_exit_eq
  li r6, 7
cr_loop_exit_eq:
  mfcr r7
  blr
  #_ REGISTER_OUT r3 40
  #_ REGISTER_OUT r5 180
  #_ REGISTER_OUT r6 7
  #_ REGISTER_OUT r7 0x40000000

# Condition register logic on bits from two compares, used two blocks on.
test_cr_logic:
  #_ REGISTER_IN r3 8
  #_ REGISTER_IN r4 8
  #_ REGISTER_IN r5 2
  cmpw cr1, r3, r4
  cmpwi cr6, r5, 3
  crand 4*cr0+eq, 4*cr1+eq, 4*cr6+lt
  cror 4*cr0+gt, 4*cr1+gt, 4*cr6+gt
  crxor 4*cr0+lt, 4*cr1+eq, 4*cr6+eq
  b cr_logic_1
cr_logic_1:
  li r6, 0
  bne cr_logic_2
  ori r6, r6, 1
cr_logic_2:
  ble cr_logic_3
  ori r6, r6, 2
cr_logic_3:
  bge cr_logic_4
  ori r6, r6, 4
cr_logic_4:
  mfcr r7
  blr
  #_ REGISTER_OUT r6 5
  #_ REGISTER_OUT r7 0xA2000080

# Record forms (the dot instructions) feeding a later branch, with a second
# record form on one path.
test_record_form_later:
  #_ REGISTER_IN r3 0x80000000
  #_ REGISTER_IN r4 1
  #_ REGISTER_IN r9 1
  add. r5, r3, r4
  cmpwi cr5, r9, 0
  beq cr5, record_later_skip
  and. r6, r3, r4
record_later_skip:
  li r8, 0
  bne record_later_1
  ori r8, r8, 1
record_later_1:
  bge record_later_2
  ori r8, r8, 2
record_later_2:
  mfcr r7
  blr
  #_ REGISTER_OUT r8 1
  #_ REGISTER_OUT r7 0x20000400

# The carry, set in one block and consumed in another, and over a call
# that leaves it alone.
test_carry_across_blocks:
  #_ REGISTER_IN r3 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_IN r4 1
  #_ REGISTER_IN r5 10
  #_ REGISTER_IN r6 20
  #_ REGISTER_IN r9 1
  FRAME_IN
  addc r7, r3, r4
  cmpwi cr5, r9, 0
  beq cr5, carry_across_skip
  bl carry_callee
carry_across_skip:
  adde r8, r5, r6
  FRAME_OUT
  blr
  #_ REGISTER_OUT r7 0
  #_ REGISTER_OUT r8 31
  #_ REGISTER_OUT r10 5
carry_callee:
  li r10, 5
  blr

# The carry changed by the callee.
test_carry_changed_by_callee:
  #_ REGISTER_IN r3 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_IN r4 1
  #_ REGISTER_IN r5 10
  #_ REGISTER_IN r6 20
  FRAME_IN
  addc r7, r3, r4
  bl carry_clearing_callee
  adde r8, r5, r6
  FRAME_OUT
  blr
  #_ REGISTER_OUT r8 30
carry_clearing_callee:
  li r10, 0
  addic r10, r10, 1
  blr

# A multi-word add in a loop: the carry of each trip feeds the next.
test_carry_chain_loop:
  #_ REGISTER_IN r3 0xFFFFFFFFFFFFFFF0
  #_ REGISTER_IN r4 0x33
  li r5, 0
  li r6, 12
  li r7, 0
  addic r0, r7, 0
carry_chain_loop:
  adde r5, r5, r3
  adde r7, r7, r4
  addi r6, r6, -1
  cmpwi cr1, r6, 0
  bne cr1, carry_chain_loop
  addze r8, r7
  blr
  #_ REGISTER_OUT r5 0xFFFFFFFFFFFFFF40
  #_ REGISTER_OUT r7 0x26F
  #_ REGISTER_OUT r8 0x26F

# CTR and LR as plain registers: moved through, changed by a callee, used
# for a computed branch within the function.
test_ctr_lr_as_registers:
  #_ REGISTER_IN r3 4
  FRAME_IN
  mtctr r3
  li r5, 0
ctr_lr_loop:
  mfctr r6
  add r5, r5, r6
  bl ctr_lr_callee
  bdnz ctr_lr_loop
  mfctr r7
  FRAME_OUT
  blr
  #_ REGISTER_OUT r5 0x16
  #_ REGISTER_OUT r7 0
ctr_lr_callee:
  mfctr r9
  cmpwi r9, 3
  bnelr
  cmpwi r10, 0
  bnelr
  li r10, 1
  li r9, 6
  mtctr r9
  blr

# bdnz combined with a condition (bdnzt/bdnzf), flags from an earlier block.
test_bdnz_with_condition:
  #_ REGISTER_IN r3 50
  #_ REGISTER_IN r4 0
  mtctr r3
  li r5, 0
bdnz_cond_loop:
  addi r4, r4, 3
  add r5, r5, r4
  cmpwi r4, 60
  bdnzf eq, bdnz_cond_loop
  mfctr r6
  mfcr r7
  blr
  #_ REGISTER_OUT r4 60
  #_ REGISTER_OUT r6 30
  #_ REGISTER_OUT r5 630
  #_ REGISTER_OUT r7 0x20000000

# mtcrf moving whole fields that a later block branches on.
test_field_moves:
  #_ REGISTER_IN r3 0x4000000B
  mtcrf 0x81, r3
  b field_moves_1
field_moves_1:
  li r5, 0
  ble field_moves_2
  ori r5, r5, 1
field_moves_2:
  bns cr7, field_moves_3
  ori r5, r5, 2
field_moves_3:
  bge cr7, field_moves_4
  ori r5, r5, 4
field_moves_4:
  mfcr r7
  blr
  #_ REGISTER_OUT r5 7
  #_ REGISTER_OUT r7 0x4000000B

# Float compare bits used in a later block and after a call.
test_float_compare_later:
  #_ REGISTER_IN f1 1.5
  #_ REGISTER_IN f2 2.5
  FRAME_IN
  fcmpu cr0, f1, f2
  fcmpu cr6, f2, f1
  bl float_compare_callee
  li r5, 0
  bge float_cmp_1
  ori r5, r5, 1
float_cmp_1:
  ble cr6, float_cmp_2
  ori r5, r5, 2
float_cmp_2:
  mfcr r7
  FRAME_OUT
  blr
  #_ REGISTER_OUT r5 3
  #_ REGISTER_OUT r7 0x80000040
float_compare_callee:
  fadd f3, f1, f2
  blr

# Vector compare record form (cr6) used a block later.
test_vector_compare_later:
  #_ REGISTER_IN v1 [00000001, 00000002, 00000003, 00000004]
  #_ REGISTER_IN v2 [00000001, 00000002, 00000003, 00000004]
  #_ REGISTER_IN v3 [00000001, 00000002, 00000003, 00000005]
  vcmpequw. v4, v1, v2
  li r5, 0
  b vec_cmp_1
vec_cmp_1:
  bnl cr6, vec_cmp_2
  ori r5, r5, 1
vec_cmp_2:
  vcmpequw. v5, v1, v3
  b vec_cmp_3
vec_cmp_3:
  blt cr6, vec_cmp_4
  ori r5, r5, 2
vec_cmp_4:
  beq cr6, vec_cmp_5
  ori r5, r5, 4
vec_cmp_5:
  mfcr r7
  blr
  #_ REGISTER_OUT r5 7
  #_ REGISTER_OUT r7 0
