/* hakuX-Boosted: the reference harness's machine code: the reset entry, the
 * exception vectors and the routine that runs one case. See dt_ref.c.
 *
 * Runs as the "firmware" of QEMU's pseries machine (-bios), in real mode.
 * Everything a case's instructions could disturb is addressed absolutely
 * (base register 0), so that all 32 general registers belong to the case. */

#define HOSTSAVE 0x2000   /* the C code's registers while a case runs */
#define TRAPVEC  0x2140   /* vector of the exception a case took, or 0 */
#define TRAPPC   0x2148
#define MSRVAL   0x2150   /* the MSR cases run with (32-bit mode, FP, VMX) */
#define VSCRTMP  0x2180
#define ST       0x2200   /* the case's registers: state words 0 to 135 */

  .machine "970"
  .machine altivec
  .section .text.vectors,"ax"
  .globl _vectors
_vectors:
  . = 0x100
  b _start

  /* Any exception a case takes ends the case: note the vector and where,
   * go back to the MSR cases run with (the exception turned FP and VMX off)
   * and save the state as if the case had returned. */
  .macro VECTOR at
  . = \at
  mtsprg 2, 3
  li 3, \at
  std 3, TRAPVEC(0)
  mfsrr0 3
  std 3, TRAPPC(0)
  b exception_common
  .endm
  VECTOR 0x200
  VECTOR 0x300
  VECTOR 0x380
  VECTOR 0x400
  VECTOR 0x480
  VECTOR 0x500
  VECTOR 0x600
  VECTOR 0x700
  VECTOR 0x800
  VECTOR 0x900
  VECTOR 0xA00
  VECTOR 0xB00
  VECTOR 0xC00
  VECTOR 0xD00
  VECTOR 0xE00
  VECTOR 0xF00
  VECTOR 0xF20
  VECTOR 0xF40
  VECTOR 0x1300
  VECTOR 0x1700

  . = 0x1800
exception_common:
  ld 3, MSRVAL(0)
  mtmsrd 3
  isync
  mfsprg 3, 2
  b case_return

  .section .text,"ax"
  .globl _start
_start:
  lis 1, stack_top@ha
  addi 1, 1, stack_top@l
  lis 2, .TOC.@ha
  addi 2, 2, .TOC.@l
  /* FP and VMX on, for the C code as well */
  mfmsr 3
  li 4, 0
  ori 4, 4, 0x8000      /* EE off: no decrementer or external interrupts */
  andc 3, 3, 4
  ori 3, 3, 0x2000      /* FP */
  oris 3, 3, 0x0200     /* VEC */
  /* 64-bit mode for the C code: the machine starts this firmware in 32-bit
   * mode, where 64-bit compiled code goes subtly wrong (record forms and
   * CTR tests take 32 bits). Without this the first case of a run, the only
   * one generated before a case had returned in 64-bit mode, could differ
   * from the runner's. */
  li 4, 1
  sldi 4, 4, 63
  or 3, 3, 4
  mtmsrd 3
  isync
  rldicl 3, 3, 0, 1     /* SF off: the mode the console runs titles in */
  std 3, MSRVAL(0)
  bl dt_main
  nop
1:
  b 1b

  /* uint64_t dt_hcall(uint64_t token, a, b, c, d) */
  .globl dt_hcall
dt_hcall:
  .long 0x44000022      /* sc 1 */
  blr

  /* void dt_run_case(void* entry): loads the state at ST, runs the code at
   * entry in 32-bit mode, saves the state back when it returns (blr) or
   * takes an exception. */
  .globl dt_run_case
dt_run_case:
  mflr 0
  std 0, HOSTSAVE+0(0)
  mfcr 0
  std 0, HOSTSAVE+8(0)
  std 1, HOSTSAVE+16(0)
  std 2, HOSTSAVE+24(0)
  .irp n,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
  std \n, HOSTSAVE+8*\n+32(0)
  .endr
  mtsrr0 3
  ld 4, MSRVAL(0)
  mtsrr1 4
  /* FPSCR, VSCR */
  lfd 0, ST+8*35(0)
  mtfsf 0xFF, 0
  li 4, VSCRTMP
  lvx 0, 0, 4
  mtvscr 0
  .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
  li 4, ST+8*72+16*\n
  lvx \n, 0, 4
  lfd \n, ST+8*40+8*\n(0)
  .endr
  ld 4, ST+8*32(0)
  mtctr 4
  ld 4, ST+8*34(0)
  mtxer 4
  ld 4, ST+8*33(0)
  mtcr 4
  lis 4, case_return@ha
  addi 4, 4, case_return@l
  mtlr 4
  .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
  ld \n, ST+8*\n(0)
  .endr
  rfid

case_return:
  .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
  std \n, ST+8*\n(0)
  .endr
  mfcr 4
  std 4, ST+8*33(0)
  mfxer 4
  std 4, ST+8*34(0)
  mfctr 4
  std 4, ST+8*32(0)
  .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
  stfd \n, ST+8*40+8*\n(0)
  li 4, ST+8*72+16*\n
  stvx \n, 0, 4
  .endr
  mffs 0
  stfd 0, ST+8*35(0)
  mfvscr 0
  li 4, VSCRTMP
  stvx 0, 0, 4
  /* back to 64-bit mode for the C code */
  ld 4, MSRVAL(0)
  li 5, 1
  sldi 5, 5, 63
  or 4, 4, 5
  mtmsrd 4
  isync
  ld 0, HOSTSAVE+8(0)
  mtcr 0
  ld 1, HOSTSAVE+16(0)
  ld 2, HOSTSAVE+24(0)
  .irp n,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
  ld \n, HOSTSAVE+8*\n+32(0)
  .endr
  ld 0, HOSTSAVE+0(0)
  mtlr 0
  blr

  .section .bss
  .balign 16
  .space 0x10000
stack_top:
  .space 64
