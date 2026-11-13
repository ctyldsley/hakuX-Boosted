# hakuX-Boosted: guest registers that live across blocks, loops and calls.
#
# Written for the translator's register pinning (pin_guest_registers and
# what widens it): every test here keeps values in registers over a point
# where a translator that holds guest registers in host registers must write
# them back or read them again. A callee changes a register its caller goes
# on to use; a caller sets a register directly before the call that reads
# it; calls are conditional, indirect, recursive, tail calls; a function
# returns from the middle of a loop; one leaves by a non-local jump.
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

# The callee changes r31, which the caller reads on every trip.
test_callee_changes_loop_register:
  #_ REGISTER_IN r31 5
  #_ REGISTER_IN r30 100
  FRAME_IN
  li r29, 0
callee_changes_loop:
  add r29, r29, r31
  bl callee_bump_r31
  addi r30, r30, -1
  cmpwi r30, 0
  bne callee_changes_loop
  FRAME_OUT
  blr
  #_ REGISTER_OUT r29 5450
  #_ REGISTER_OUT r31 105
  #_ REGISTER_OUT r30 0
callee_bump_r31:
  addi r31, r31, 1
  blr

# The caller changes r28 directly before the call that reads it.
test_callee_reads_fresh_register:
  #_ REGISTER_IN r28 3
  #_ REGISTER_IN r27 50
  FRAME_IN
  li r26, 0
callee_reads_loop:
  mulli r28, r28, 3
  addi r28, r28, 1
  bl callee_fold_r28
  addic. r27, r27, -1
  bne callee_reads_loop
  FRAME_OUT
  blr
  #_ REGISTER_OUT r26 0xBADBADBAD
  #_ REGISTER_OUT r28 0xBADBADBAD
callee_fold_r28:
  xor r26, r26, r28
  rlwinm r26, r26, 5, 0, 31
  blr

# A call on some trips only; both paths change the same registers.
test_conditional_call:
  #_ REGISTER_IN r31 0x1234
  #_ REGISTER_IN r30 64
  FRAME_IN
  li r29, 1
conditional_call_loop:
  andi. r0, r31, 4
  bnel cond_callee
  add r29, r29, r31
  srwi r31, r31, 1
  xori r31, r31, 0x5A5
  addic. r30, r30, -1
  bne conditional_call_loop
  FRAME_OUT
  blr
  #_ REGISTER_OUT r29 0xBADBADBAD
  #_ REGISTER_OUT r31 0xBADBADBAD
cond_callee:
  mulli r31, r31, 7
  addi r29, r29, 3
  clrldi r31, r31, 40
  blr

# An indirect call through CTR; the callee changes CTR and a loop register.
test_indirect_call:
  #_ REGISTER_IN r31 9
  #_ REGISTER_IN r30 20
  FRAME_IN
  lis r25, indirect_callee@h
  ori r25, r25, indirect_callee@l
  li r29, 0
indirect_call_loop:
  mtctr r25
  bctrl
  add r29, r29, r31
  mfctr r24
  add r29, r29, r24
  addic. r30, r30, -1
  bne indirect_call_loop
  FRAME_OUT
  li r25, 0
  blr
  #_ REGISTER_OUT r29 0xBADBADBAD
  #_ REGISTER_OUT r31 0xBADBADBAD
  #_ REGISTER_OUT r24 77
indirect_callee:
  addi r31, r31, 2
  li r0, 77
  mtctr r0
  blr

# A counted loop on CTR with a call that leaves CTR alone, then one whose
# callee reloads it.
test_ctr_loop_with_calls:
  #_ REGISTER_IN r31 1
  FRAME_IN
  li r0, 30
  mtctr r0
ctr_loop_a:
  bl ctr_callee_plain
  bdnz ctr_loop_a
  li r0, 10
  mtctr r0
  li r30, 0
ctr_loop_b:
  addi r30, r30, 1
  bl ctr_callee_sets_ctr
  bdnz ctr_loop_b
  FRAME_OUT
  blr
  #_ REGISTER_OUT r31 0xBADBADBAD
  #_ REGISTER_OUT r30 0xBADBADBAD
ctr_callee_plain:
  rlwinm r31, r31, 1, 0, 31
  addi r31, r31, 3
  clrldi r31, r31, 36
  blr
ctr_callee_sets_ctr:
  cmpwi r30, 4
  bnelr
  li r0, 3
  mtctr r0
  blr

# Returns from the middle of a loop with changed registers.
test_return_inside_loop:
  #_ REGISTER_IN r3 0x40
  #_ REGISTER_IN r4 7
  li r5, 0
  li r6, 100
return_inside_loop:
  add r5, r5, r4
  mulli r4, r4, 3
  clrldi r4, r4, 48
  cmplw r5, r3
  bgtlr
  addic. r6, r6, -1
  bne return_inside_loop
  li r5, -1
  blr
  #_ REGISTER_OUT r5 0xBADBADBAD
  #_ REGISTER_OUT r4 0xBADBADBAD
  #_ REGISTER_OUT r6 0xBADBADBAD

# A tail call (a plain branch to another function) with changed registers.
test_tail_call:
  #_ REGISTER_IN r3 11
  #_ REGISTER_IN r4 13
  li r5, 6
tail_call_loop:
  add r3, r3, r4
  addi r4, r4, 1
  addic. r5, r5, -1
  bne tail_call_loop
  b tail_callee
  #_ REGISTER_OUT r3 0xBADBADBAD
  #_ REGISTER_OUT r6 0xBADBADBAD
tail_callee:
  mullw r6, r3, r4
  addi r3, r3, 1000
  blr

# Recursion: every level keeps values in the same callee-saved registers.
test_recursion:
  #_ REGISTER_IN r3 12
  FRAME_IN
  bl rec_sum
  FRAME_OUT
  blr
  #_ REGISTER_OUT r3 0xBADBADBAD
rec_sum:
  mflr r0
  stwu r1, -32(r1)
  stw r0, 36(r1)
  stw r31, 28(r1)
  stw r30, 24(r1)
  mr r31, r3
  li r30, 0
  cmpwi r3, 2
  blt rec_sum_done
  addi r3, r31, -1
  bl rec_sum
  add r30, r30, r3
  addi r3, r31, -2
  bl rec_sum
  add r30, r30, r3
  mullw r30, r30, r31
  clrldi r30, r30, 40
rec_sum_done:
  add r3, r30, r31
  lwz r30, 24(r1)
  lwz r31, 28(r1)
  lwz r0, 36(r1)
  addi r1, r1, 32
  mtlr r0
  blr

# Float registers over a loop with a call; the callee changes one of them.
test_float_across_calls:
  #_ REGISTER_IN f31 1.5
  #_ REGISTER_IN f30 0.25
  #_ REGISTER_IN f29 2.0
  #_ REGISTER_IN r31 40
  FRAME_IN
float_across_loop:
  fmadd f31, f31, f30, f29
  bl float_callee
  fadd f29, f29, f30
  addic. r31, r31, -1
  bne float_across_loop
  FRAME_OUT
  blr
  #_ REGISTER_OUT f31 0xBADBADBAD
  #_ REGISTER_OUT f29 0xBADBADBAD
  #_ REGISTER_OUT f30 0xBADBADBAD
float_callee:
  fmul f30, f30, f30
  fadd f30, f30, f30
  fsub f31, f31, f30
  blr

# Vector registers over a loop with a call; the callee changes one of them.
test_vector_across_calls:
  #_ REGISTER_IN v31 [00000001, 00000002, 00000003, 00000004]
  #_ REGISTER_IN v30 [00000010, 00000020, 00000030, 00000040]
  #_ REGISTER_IN r31 25
  FRAME_IN
vector_across_loop:
  vadduwm v31, v31, v30
  bl vector_callee
  vxor v29, v31, v30
  vadduwm v28, v28, v29
  addic. r31, r31, -1
  bne vector_across_loop
  FRAME_OUT
  blr
  #_ REGISTER_OUT v31 0xBADBADBAD
  #_ REGISTER_OUT v30 0xBADBADBAD
  #_ REGISTER_OUT v28 0xBADBADBAD
vector_callee:
  vadduwm v30, v30, v31
  vspltisw v27, 3
  vslw v30, v30, v27
  blr

# Many blocks, no loop: a register used in most of them.
test_straight_line_blocks:
  #_ REGISTER_IN r3 0x1357
  #_ REGISTER_IN r4 0x2468
  li r5, 0
  andi. r0, r3, 1
  beq straight_1
  add r5, r5, r3
  xor r4, r4, r5
straight_1:
  andi. r0, r3, 2
  beq straight_2
  subf r5, r4, r5
  addi r4, r4, 9
straight_2:
  andi. r0, r3, 4
  beq straight_3
  mullw r5, r5, r3
  rlwinm r4, r4, 3, 0, 31
straight_3:
  andi. r0, r3, 16
  beq straight_4
  or r5, r5, r4
  srwi r4, r4, 2
straight_4:
  andi. r0, r3, 64
  beq straight_5
  nor r5, r5, r3
  neg r4, r4
straight_5:
  add r6, r5, r4
  blr
  #_ REGISTER_OUT r5 0xBADBADBAD
  #_ REGISTER_OUT r4 0xBADBADBAD
  #_ REGISTER_OUT r6 0xBADBADBAD

# A nested loop whose inner loop makes calls and whose outer loop does not.
test_nested_loops_with_calls:
  #_ REGISTER_IN r31 7
  FRAME_IN
  li r30, 0
  li r29, 6
nested_outer:
  li r28, 5
nested_inner:
  add r30, r30, r31
  mr r3, r30
  bl nested_callee
  xor r31, r31, r3
  addic. r28, r28, -1
  bne nested_inner
  rlwinm r30, r30, 2, 0, 31
  clrldi r30, r30, 36
  addic. r29, r29, -1
  bne nested_outer
  FRAME_OUT
  blr
  #_ REGISTER_OUT r30 0xBADBADBAD
  #_ REGISTER_OUT r31 0xBADBADBAD
nested_callee:
  mulli r3, r3, 13
  addi r3, r3, 7
  clrldi r3, r3, 44
  blr

# A non-local jump, as longjmp makes it: the innermost function restores the
# stack pointer of two frames up and branches to that frame's return site.
# Registers changed below the jump must be the ones seen after it.
test_nonlocal_jump:
  #_ REGISTER_IN r31 3
  FRAME_IN
  li r30, 10
nonlocal_loop:
  mr r27, r1
  lis r26, nonlocal_landing@h
  ori r26, r26, nonlocal_landing@l
  bl nonlocal_middle
nonlocal_landing:
  add r31, r31, r29
  addic. r30, r30, -1
  bne nonlocal_loop
  FRAME_OUT
  blr
  #_ REGISTER_OUT r31 0xBADBADBAD
  #_ REGISTER_OUT r29 0xBADBADBAD
nonlocal_middle:
  mflr r0
  stwu r1, -48(r1)
  stw r0, 52(r1)
  addi r29, r31, 1
  bl nonlocal_inner
  # not reached on the trips that jump
  addi r29, r29, 100
  lwz r0, 52(r1)
  addi r1, r1, 48
  mtlr r0
  blr
nonlocal_inner:
  andi. r0, r30, 1
  beqlr
  mulli r29, r29, 5
  mr r1, r27
  mtlr r26
  blr
