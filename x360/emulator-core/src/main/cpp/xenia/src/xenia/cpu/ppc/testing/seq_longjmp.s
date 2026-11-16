# hakuX-Boosted: non-local returns (setjmp and longjmp, written out here),
# which the tests could not make before raw modules kept their return
# sites. A longjmp is a blr to the address after an earlier call, in a
# frame further up: the backend has to find that frame's host stack and
# carry on there, and a function that keeps guest registers in host
# registers has to read them back, because the code that jumped changed
# them. Every test changes registers in the frames it jumps out of and
# reads them after landing.
#
# The expected values are worked out by hand.

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
.macro JMPBUF reg
  lis \reg, 0x1000
  ori \reg, \reg, 0x1100
.endm

# r3 = buffer. Returns 0; returns again with longjmp's value. (The branch
# keeps it out of the leaf inliner, as a real setjmp's size does: an inlined
# call leaves no return site to come back to.)
lj_setjmp:
  b lj_setjmp_body
lj_setjmp_body:
  mflr r0
  stw r0, 0(r3)
  stw r1, 4(r3)
  li r3, 0
  blr
# r3 = buffer, r4 = value.
lj_longjmp:
  lwz r0, 0(r3)
  lwz r1, 4(r3)
  mtlr r0
  mr r3, r4
  blr

# Out of three frames.
test_longjmp_three_frames:
  #_ REGISTER_IN r5 7
  FRAME_IN
  li r6, 1
  li r8, 0
  JMPBUF r3
  bl lj_setjmp
  cmpwi r3, 0
  bne lj3_landed
  addi r6, r6, 1
  bl lj3_a
  li r8, 99
lj3_landed:
  add r7, r3, r5
  add r9, r6, r10
  FRAME_OUT
  blr
  #_ REGISTER_OUT r6 2
  #_ REGISTER_OUT r7 49
  #_ REGISTER_OUT r8 0
  #_ REGISTER_OUT r9 302
  #_ REGISTER_OUT r10 300
lj3_a:
  FRAME_IN
  li r10, 100
  bl lj3_b
  li r8, 98
  FRAME_OUT
  blr
lj3_b:
  FRAME_IN
  addi r10, r10, 100
  bl lj3_c
  li r8, 97
  FRAME_OUT
  blr
lj3_c:
  FRAME_IN
  addi r10, r10, 100
  JMPBUF r3
  li r4, 42
  bl lj_longjmp
  li r8, 96
  FRAME_OUT
  blr

# Out of one frame (the callee itself jumps).
test_longjmp_one_frame:
  FRAME_IN
  li r6, 5
  JMPBUF r3
  bl lj_setjmp
  cmpwi r3, 0
  bne lj1_landed
  bl lj1_a
  li r6, 99
lj1_landed:
  add r7, r3, r6
  FRAME_OUT
  blr
  #_ REGISTER_OUT r6 6
  #_ REGISTER_OUT r7 15
lj1_a:
  FRAME_IN
  addi r6, r6, 1
  JMPBUF r3
  li r4, 9
  bl lj_longjmp
  FRAME_OUT
  blr

# Three jumps to one buffer, from a loop that calls two frames down each
# time; the count is kept in memory and in a register the callees change.
test_longjmp_in_loop:
  FRAME_IN
  li r6, 0
  li r11, 0
  stw r6, 80(r1)
  JMPBUF r3
  bl lj_setjmp
  add r11, r11, r3
  lwz r6, 80(r1)
  cmpwi r6, 3
  bge ljl_done
  addi r6, r6, 1
  stw r6, 80(r1)
  mr r5, r6
  bl ljl_a
  li r11, 99
ljl_done:
  mr r7, r6
  FRAME_OUT
  blr
  #_ REGISTER_OUT r7 3
  #_ REGISTER_OUT r11 60
ljl_a:
  FRAME_IN
  bl ljl_b
  FRAME_OUT
  blr
ljl_b:
  FRAME_IN
  JMPBUF r3
  mulli r4, r5, 10
  bl lj_longjmp
  FRAME_OUT
  blr

# Float and vector registers set before the jump, in the frames jumped out
# of, and read after landing; and a float register live across the setjmp.
test_longjmp_float_vector:
  #_ REGISTER_IN f1 1.5
  #_ REGISTER_IN f2 2.0
  #_ REGISTER_IN v1 [00000001, 00000002, 00000003, 00000004]
  #_ REGISTER_IN v2 [00000010, 00000020, 00000030, 00000040]
  FRAME_IN
  fadd f3, f1, f2
  JMPBUF r3
  bl lj_setjmp
  cmpwi r3, 0
  bne ljf_landed
  bl ljf_a
ljf_landed:
  fadd f5, f3, f4
  vadduwm v4, v3, v1
  FRAME_OUT
  blr
  #_ REGISTER_OUT f3 3.5
  #_ REGISTER_OUT f4 4.0
  #_ REGISTER_OUT f5 7.5
  #_ REGISTER_OUT v3 [00000011, 00000022, 00000033, 00000044]
  #_ REGISTER_OUT v4 [00000012, 00000024, 00000036, 00000048]
ljf_a:
  FRAME_IN
  fadd f4, f2, f2
  bl ljf_b
  FRAME_OUT
  blr
ljf_b:
  FRAME_IN
  vadduwm v3, v1, v2
  JMPBUF r3
  li r4, 1
  bl lj_longjmp
  FRAME_OUT
  blr

# The jump is made from a function reached through bctrl, and longjmp
# itself is reached through bctrl.
test_longjmp_indirect:
  FRAME_IN
  li r6, 0
  JMPBUF r3
  bl lj_setjmp
  cmpwi r3, 0
  bne lji_landed
  lis r12, lji_a@h
  ori r12, r12, lji_a@l
  mtctr r12
  bctrl
  li r6, 99
lji_landed:
  add r7, r3, r6
  FRAME_OUT
  blr
  #_ REGISTER_OUT r6 4
  #_ REGISTER_OUT r7 11
lji_a:
  FRAME_IN
  addi r6, r6, 4
  bl lji_b
  FRAME_OUT
  blr
lji_b:
  FRAME_IN
  JMPBUF r3
  li r4, 7
  lis r12, lj_longjmp@h
  ori r12, r12, lj_longjmp@l
  mtctr r12
  bctrl
  FRAME_OUT
  blr

# The landing function makes no stack frame (it keeps its return address in
# a register), so it was entered with the very stack pointer the jump
# restores, like the callee it jumps out of.
test_longjmp_frameless:
  mflr r30
  li r6, 1
  JMPBUF r3
  bl lj_setjmp
  cmpwi r3, 0
  bne ljn_landed
  addi r6, r6, 1
  bl ljn_a
  li r6, 99
ljn_landed:
  add r7, r3, r6
  mtlr r30
  blr
  #_ REGISTER_OUT r6 5
  #_ REGISTER_OUT r7 26
ljn_a:
  FRAME_IN
  addi r6, r6, 1
  bl ljn_b
  li r6, 98
  FRAME_OUT
  blr
ljn_b:
  FRAME_IN
  addi r6, r6, 2
  JMPBUF r3
  li r4, 21
  bl lj_longjmp
  FRAME_OUT
  blr

# A function that calls itself three deep with a setjmp in each activation
# (buffer n at 16*n). The innermost jumps to activation r9's buffer: the
# dead activations between were called from the same function, and the one
# landed in has to be the right one (it reads its own depth from its frame
# and, when it is not the outermost, returns to the one above).
.macro RECURSIVE_BODY
  FRAME_IN
  li r5, 0
  li r6, 0
  bl ljr_f
  mr r7, r3
  FRAME_OUT
  blr
.endm
test_longjmp_recursive_outermost:
  #_ REGISTER_IN r9 0
  RECURSIVE_BODY
  #_ REGISTER_OUT r6 20
  #_ REGISTER_OUT r7 25
test_longjmp_recursive_middle:
  #_ REGISTER_IN r9 1
  RECURSIVE_BODY
  #_ REGISTER_OUT r6 1020
  #_ REGISTER_OUT r7 281
ljr_f:
  FRAME_IN
  stw r5, 80(r1)
  JMPBUF r3
  slwi r0, r5, 4
  add r3, r3, r0
  bl lj_setjmp
  cmpwi r3, 0
  bne ljr_landed
  lwz r5, 80(r1)
  cmpwi r5, 2
  bge ljr_jump
  addi r5, r5, 1
  addi r6, r6, 10
  bl ljr_f
  addi r6, r6, 1000
  FRAME_OUT
  blr
ljr_jump:
  JMPBUF r3
  slwi r0, r9, 4
  add r3, r3, r0
  li r4, 5
  bl lj_longjmp
ljr_landed:
  lwz r5, 80(r1)
  slwi r5, r5, 8
  add r3, r3, r5
  add r3, r3, r6
  FRAME_OUT
  blr
