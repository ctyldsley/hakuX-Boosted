# hakuX-Boosted: constants stored to a register in one block and read in
# another.
#
# Written for propagate_context_constants: each test is a case where a
# translator that carries constants across blocks must get the join right.
# Two paths store different constants; both store the same one; a loop
# changes a register that held a constant on entry; a call may change it; a
# branch leaves the middle of a block before a later store.
#
# The expected values are worked out by hand; every test is short.

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

# Different constants on the two paths: the join holds neither.
test_const_differs_taken:
  #_ REGISTER_IN r3 1
  li r5, 10
  cmpwi r3, 0
  beq const_differs_a_join
  li r5, 20
const_differs_a_join:
  addi r6, r5, 1
  blr
  #_ REGISTER_OUT r5 20
  #_ REGISTER_OUT r6 21

test_const_differs_not_taken:
  #_ REGISTER_IN r3 0
  li r5, 10
  cmpwi r3, 0
  beq const_differs_b_join
  li r5, 20
const_differs_b_join:
  addi r6, r5, 1
  blr
  #_ REGISTER_OUT r5 10
  #_ REGISTER_OUT r6 11

# An if and an else that each store a constant.
test_const_if_else_then:
  #_ REGISTER_IN r3 5
  cmpwi r3, 5
  bne const_if_else_a_else
  li r5, 111
  b const_if_else_a_join
const_if_else_a_else:
  li r5, 222
const_if_else_a_join:
  mulli r6, r5, 2
  blr
  #_ REGISTER_OUT r6 222

test_const_if_else_else:
  #_ REGISTER_IN r3 6
  cmpwi r3, 5
  bne const_if_else_b_else
  li r5, 111
  b const_if_else_b_join
const_if_else_b_else:
  li r5, 222
const_if_else_b_join:
  mulli r6, r5, 2
  blr
  #_ REGISTER_OUT r6 444

# The same constant on both paths: the join may use it.
test_const_same_both_paths:
  #_ REGISTER_IN r3 9
  cmpwi r3, 0
  beq const_same_else
  li r5, 77
  li r7, 1
  b const_same_join
const_same_else:
  li r5, 77
  li r7, 2
const_same_join:
  add r6, r5, r7
  blr
  #_ REGISTER_OUT r6 78

# A register that is constant on entry to a loop and changed inside it.
test_const_changed_in_loop:
  #_ REGISTER_IN r3 6
  li r5, 100
  mtctr r3
const_loop_head:
  addi r6, r5, 0
  addi r5, r5, 7
  bdnz const_loop_head
  add r7, r5, r6
  blr
  #_ REGISTER_OUT r5 142
  #_ REGISTER_OUT r6 135
  #_ REGISTER_OUT r7 277

# A constant that a loop leaves alone: it is the same on every trip.
test_const_kept_in_loop:
  #_ REGISTER_IN r3 5
  li r5, 3
  li r6, 0
  mtctr r3
const_kept_head:
  add r6, r6, r5
  bdnz const_kept_head
  blr
  #_ REGISTER_OUT r6 15

# A call between the store and the load: the callee changes the register.
test_const_over_call:
  #_ REGISTER_IN r3 1
  FRAME_IN
  li r31, 50
  cmpwi r3, 0
  beq const_over_call_skip
  bl const_callee
const_over_call_skip:
  addi r6, r31, 1
  FRAME_OUT
  blr
  #_ REGISTER_OUT r6 60
const_callee:
  cmpwi cr7, r3, 100
  bgt cr7, const_callee_out
  addi r31, r31, 9
const_callee_out:
  blr

test_const_over_call_not_made:
  #_ REGISTER_IN r3 0
  FRAME_IN
  li r31, 50
  cmpwi r3, 0
  beq const_over_call_b_skip
  bl const_callee
const_over_call_b_skip:
  addi r6, r31, 1
  FRAME_OUT
  blr
  #_ REGISTER_OUT r6 51

# A conditional return in the middle, then a second store: the block after
# sees the second constant only.
test_const_two_stores:
  #_ REGISTER_IN r3 4
  li r5, 1
  cmpwi r3, 99
  beqlr
  li r5, 2
  b const_two_next
const_two_next:
  slwi r6, r5, 4
  blr
  #_ REGISTER_OUT r6 32

# Flag bits that are constant on one path and computed on the other.
test_const_flags_differ:
  #_ REGISTER_IN r3 3
  #_ REGISTER_IN r4 3
  crclr 4*cr7+eq
  cmpwi r3, 0
  beq const_flags_join
  cmpw cr7, r3, r4
const_flags_join:
  li r5, 0
  bne cr7, const_flags_out
  li r5, 1
const_flags_out:
  blr
  #_ REGISTER_OUT r5 1

test_const_flags_same:
  #_ REGISTER_IN r3 0
  #_ REGISTER_IN r4 3
  crclr 4*cr7+eq
  cmpwi r3, 0
  beq const_flags_b_join
  cmpw cr7, r3, r4
const_flags_b_join:
  li r5, 0
  bne cr7, const_flags_b_out
  li r5, 1
const_flags_b_out:
  blr
  #_ REGISTER_OUT r5 0

# CTR and LR as carriers of a constant across blocks.
test_const_in_ctr:
  #_ REGISTER_IN r3 2
  li r0, 40
  mtctr r0
  cmpwi r3, 2
  bne const_ctr_join
  li r0, 41
  mtctr r0
const_ctr_join:
  mfctr r6
  blr
  #_ REGISTER_OUT r6 41
