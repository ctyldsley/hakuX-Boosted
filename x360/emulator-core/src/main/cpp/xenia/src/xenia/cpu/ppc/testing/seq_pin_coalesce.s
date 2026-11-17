# hakuX-Boosted: guest registers read, overwritten and read again inside one
# block, in the shapes that matter when a loaded value is used straight from
# the host register its guest register is pinned in and a result is computed
# straight into one (pin_guest_registers_coalesce): the old value of a
# register used after the register has been written, results whose
# destination is one of their own operands, one value stored to two
# registers, and compares whose operands change before the branch that uses
# them (the compare-and-branch forms of a64_fuse_compare_branch).
#
# The expected values are worked out by hand; none depends on the
# translator.

# The old value of r3, copied to r4, is used after r3 has changed twice.
test_pin_old_value_after_write:
  #_ REGISTER_IN r3 10
  mr r4, r3
  addi r3, r3, 1
  add r5, r4, r3
  addi r3, r3, 100
  add r6, r4, r3
  subf r7, r4, r3
  blr
  #_ REGISTER_OUT r3 111
  #_ REGISTER_OUT r4 10
  #_ REGISTER_OUT r5 21
  #_ REGISTER_OUT r6 121
  #_ REGISTER_OUT r7 101

# Destination equal to the first, the second and both operands.
test_pin_dest_is_operand:
  #_ REGISTER_IN r3 7
  #_ REGISTER_IN r4 5
  add r3, r3, r4
  add r4, r3, r4
  subf r3, r4, r3
  add r3, r3, r3
  subf r5, r3, r3
  xor r4, r4, r4
  blr
  #_ REGISTER_OUT r3 0xFFFFFFFFFFFFFFF6
  #_ REGISTER_OUT r4 0
  #_ REGISTER_OUT r5 0

# A swap through a third register, twice, then through xor.
test_pin_swap:
  #_ REGISTER_IN r3 1
  #_ REGISTER_IN r4 2
  mr r5, r3
  mr r3, r4
  mr r4, r5
  mr r5, r3
  mr r3, r4
  mr r4, r5
  mr r5, r3
  mr r3, r4
  mr r4, r5
  xor r3, r3, r4
  xor r4, r3, r4
  xor r3, r3, r4
  blr
  #_ REGISTER_OUT r3 1
  #_ REGISTER_OUT r4 2
  #_ REGISTER_OUT r5 1

# One value in two registers; then each register changes on its own, and the
# value itself is used again after both have.
test_pin_one_value_two_registers:
  #_ REGISTER_IN r3 40
  addi r4, r3, 2
  mr r5, r4
  mr r6, r4
  addi r5, r5, 1
  add r7, r4, r5
  li r4, 0
  add r8, r6, r4
  add r9, r5, r6
  blr
  #_ REGISTER_OUT r4 0
  #_ REGISTER_OUT r5 43
  #_ REGISTER_OUT r6 42
  #_ REGISTER_OUT r7 85
  #_ REGISTER_OUT r8 42
  #_ REGISTER_OUT r9 85

# A value used after the next store to its register (through the copy made
# of it), with a constant stored between.
test_pin_use_after_next_store:
  #_ REGISTER_IN r3 3
  addi r3, r3, 1
  mr r5, r3
  li r3, 0
  add r6, r5, r3
  addi r3, r5, 10
  add r7, r5, r3
  blr
  #_ REGISTER_OUT r3 14
  #_ REGISTER_OUT r5 4
  #_ REGISTER_OUT r6 4
  #_ REGISTER_OUT r7 18

# The compared register changes between the compare and its branch: the
# branch must go by the values compared. Each of the three bits, with the
# first operand, the second and both rewritten.
test_cmp_operand_changes_before_branch:
  #_ REGISTER_IN r3 5
  #_ REGISTER_IN r4 9
  li r10, 0
  cmpw cr6, r3, r4
  li r3, 100
  bge cr6, 1f
  ori r10, r10, 1
1:
  cmpw cr6, r3, r4
  li r4, 1000
  ble cr6, 2f
  ori r10, r10, 2
2:
  cmplw cr6, r3, r4
  mr r3, r4
  li r4, 0
  bne cr6, 3f
  ori r10, r10, 4
3:
  cmpwi cr6, r4, 0
  addi r4, r4, 1
  bne cr6, 4f
  ori r10, r10, 8
4:
  blr
  #_ REGISTER_OUT r3 1000
  #_ REGISTER_OUT r4 1
  #_ REGISTER_OUT r10 11

# A compare with zero whose register is rewritten before the branch, in a
# loop: the branch must test what was compared, not what the register holds
# when the branch runs.
test_cmp_zero_register_reused:
  #_ REGISTER_IN r3 4
  li r5, 0
  li r6, 0
1:
  addi r5, r5, 1
  subi r3, r3, 1
  cmpwi cr6, r3, 0
  mr r7, r3
  li r3, 77
  beq cr6, 2f
  mr r3, r7
  addi r6, r6, 10
  b 1b
2:
  blr
  #_ REGISTER_OUT r3 77
  #_ REGISTER_OUT r5 4
  #_ REGISTER_OUT r6 30
  #_ REGISTER_OUT r7 0

# Two compares of the same operands, signed and unsigned, into two fields,
# and branches on both after the operands have changed.
test_cmp_same_operands_two_fields:
  #_ REGISTER_IN r3 0xFFFFFFFFFFFFFFFF
  #_ REGISTER_IN r4 1
  li r10, 0
  cmpw cr6, r3, r4
  cmplw cr7, r3, r4
  li r3, 0
  li r4, 0
  bge cr6, 1f
  ori r10, r10, 1
1:
  ble cr7, 2f
  ori r10, r10, 2
2:
  cmpw cr6, r3, r4
  cmplw cr7, r3, r4
  bne cr6, 3f
  ori r10, r10, 4
3:
  bne cr7, 4f
  ori r10, r10, 8
4:
  blr
  #_ REGISTER_OUT r10 15

# The count register tested by bdnz while r3 mirrors it and is rewritten:
# the truncated compare of the count must not be confused with a register
# that changes afterwards.
test_bdnz_with_register_reuse:
  #_ REGISTER_IN r3 6
  mtctr r3
  li r4, 0
1:
  mfctr r5
  add r4, r4, r5
  li r5, 0
  bdnz 1b
  mfctr r6
  blr
  #_ REGISTER_OUT r4 21
  #_ REGISTER_OUT r5 0
  #_ REGISTER_OUT r6 0

# Loads into a register that is then an operand and a destination, and a
# store of a register rewritten right after.
test_pin_memory_round_trip:
  #_ REGISTER_IN r3 0x11223344
  #_ REGISTER_IN r4 0x55667788
  stw r3, -16(r1)
  mr r3, r4
  stw r3, -12(r1)
  lwz r4, -16(r1)
  lwz r3, -12(r1)
  add r5, r3, r4
  lwz r3, -16(r1)
  subf r6, r3, r4
  blr
  #_ REGISTER_OUT r3 0x11223344
  #_ REGISTER_OUT r4 0x11223344
  #_ REGISTER_OUT r5 0x6688AACC
  #_ REGISTER_OUT r6 0

# Between a compare and the branch that uses it: instructions whose
# translation uses the host's flags for something else (a float add and a
# float compare, each of which tests for NaN; a load and a store, whose
# address is adjusted with a compare on a 16 KiB host). The branch must go
# by the integer compare. All three bits.
test_cmp_then_flag_users_then_branch:
  #_ REGISTER_IN r3 5
  #_ REGISTER_IN r4 9
  #_ REGISTER_IN f1 2.0
  #_ REGISTER_IN f2 1.0
  li r10, 0
  cmpw cr6, r3, r4
  fadd f3, f1, f2
  fcmpu cr1, f1, f2
  bge cr6, 1f
  ori r10, r10, 1
1:
  cmpw cr6, r4, r3
  stw r3, -16(r1)
  lwz r5, -16(r1)
  fsub f4, f2, f1
  ble cr6, 2f
  ori r10, r10, 2
2:
  cmpw cr6, r5, r3
  fmul f5, f3, f4
  fcmpu cr1, f2, f1
  bne cr6, 3f
  ori r10, r10, 4
3:
  cmplw cr6, r3, r4
  fdiv f6, f1, f1
  lwz r6, -16(r1)
  blt cr6, 4f
  ori r10, r10, 8
4:
  blr
  #_ REGISTER_OUT r5 5
  #_ REGISTER_OUT r6 5
  #_ REGISTER_OUT r10 7
  #_ REGISTER_OUT f3 3.0
  #_ REGISTER_OUT f4 -1.0
  #_ REGISTER_OUT f5 -3.0
  #_ REGISTER_OUT f6 1.0

# A float compare's own branch after an integer compare of other things,
# and the integer compare's branch after that.
test_float_cmp_between_integer_cmp_and_branch:
  #_ REGISTER_IN r3 9
  #_ REGISTER_IN r4 5
  #_ REGISTER_IN f1 1.0
  #_ REGISTER_IN f2 2.0
  li r10, 0
  cmpw cr6, r3, r4
  fcmpu cr7, f1, f2
  bge cr7, 1f
  ori r10, r10, 1
1:
  ble cr6, 2f
  ori r10, r10, 2
2:
  blr
  #_ REGISTER_OUT r10 3

# register_allocation_elide_copies also takes the zero extension after a
# byte or halfword load for a copy (the load clears the rest of the
# register). Loads into registers that held all ones, from memory that
# holds all ones, with the results used as 64-bit values.
test_byte_and_halfword_loads_extend:
  lis r4, 0x1000
  ori r4, r4, 0x1100
  li r0, -1
  std r0, 0(r4)
  std r0, 8(r4)
  li r5, -1
  li r6, -1
  li r8, -1
  li r9, -1
  li r10, 9
  lbz r5, 3(r4)
  lhz r6, 2(r4)
  lbzx r8, r4, r10
  lhzx r9, r4, r10
  add r7, r5, r6
  add r7, r7, r8
  add r7, r7, r9
  sldi r11, r5, 56
  srdi r11, r11, 8
  blr
  #_ REGISTER_OUT r5 0xFF
  #_ REGISTER_OUT r6 0xFFFF
  #_ REGISTER_OUT r7 0x201FC
  #_ REGISTER_OUT r8 0xFF
  #_ REGISTER_OUT r9 0xFFFF
  #_ REGISTER_OUT r11 0x00FF000000000000

# hakuX-Boosted: a64_scratch_gpr_pool. Values held in registers across an
# instruction whose sequence calls a host function (vexptefp and vlogefp
# do): a host function keeps x19 to x28 and nothing else, so whatever the
# allocator put in x9 to x15 or x4 to x7 has to be saved round the call by
# the thunk. With register_allocation_from_top the first values go to the
# last registers of the pool, which is how a short test reaches them.
test_values_across_host_helper:
  #_ REGISTER_IN r3 0x1111
  #_ REGISTER_IN r4 0x2222
  #_ REGISTER_IN r8 0x0101
  #_ REGISTER_IN r9 0x0202
  #_ REGISTER_IN r10 0x0404
  #_ REGISTER_IN r11 0x0808
  add r5, r3, r4
  add r6, r8, r9
  add r7, r10, r11
  add r12, r5, r6
  vexptefp v1, v2
  add r14, r5, r5
  add r15, r6, r6
  add r16, r7, r7
  add r17, r12, r12
  vlogefp v3, v2
  add r18, r5, r6
  add r19, r7, r12
  blr
  #_ REGISTER_OUT r3 0x1111
  #_ REGISTER_OUT r4 0x2222
  #_ REGISTER_OUT r8 0x0101
  #_ REGISTER_OUT r9 0x0202
  #_ REGISTER_OUT r10 0x0404
  #_ REGISTER_OUT r11 0x0808
  #_ REGISTER_OUT r5 0x3333
  #_ REGISTER_OUT r6 0x0303
  #_ REGISTER_OUT r7 0x0C0C
  #_ REGISTER_OUT r12 0x3636
  #_ REGISTER_OUT r14 0x6666
  #_ REGISTER_OUT r15 0x0606
  #_ REGISTER_OUT r16 0x1818
  #_ REGISTER_OUT r17 0x6C6C
  #_ REGISTER_OUT r18 0x3636
  #_ REGISTER_OUT r19 0x4242
