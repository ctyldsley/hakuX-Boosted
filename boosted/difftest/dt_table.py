#!/usr/bin/env python3
# hakuX-Boosted: writes the differential tester's instruction table
# (dt_table.inc) from the core's own list of PowerPC instructions
# (tools/ppc-instructions.xml gives each mnemonic its encoding) and the
# operand descriptions below.
#
#   dt_table.py <ppc-instructions.xml> <dt_table.inc> [known-issues.txt]
#
# One line an instruction: "mnemonic  class  operands  [=> reference]  [!flags]".
# The operands say which fields the generator fills and with what; see OPS.
# "=> name FIELD=n ..." gives the instruction the reference runs instead (QEMU
# has no VMX128): the plain VMX instruction and which operand goes in which of
# its fields. "=> @name" is an instruction with no equivalent, whose reference
# is the C function of that name in dt_pseudo.h.
import re
import sys

SPEC = r"""
# ---- integer arithmetic
addx      int  D A B oe rc
addcx     int  D A B oe rc
addex     int  D A B oe rc
addmex    int  D A oe rc
addzex    int  D A oe rc
subfx     int  D A B oe rc
subfcx    int  D A B oe rc
subfex    int  D A B oe rc
subfmex   int  D A oe rc
subfzex   int  D A oe rc
negx      int  D A oe rc
mullwx    int  D A B oe rc
mulhwx    int  D A B rc            !hi32
mulhwux   int  D A B rc            !hi32
mulldx    int  D A B oe rc
mulhdx    int  D A B rc
mulhdux   int  D A B rc
divwx     int  D A B oe rc         !hi32 !divw
divwux    int  D A B oe rc         !hi32 !divwu
divdx     int  D A B oe rc         !divd
divdux    int  D A B oe rc         !divdu
addi      int  D A si
addis     int  D A si
addic     int  D A si
addic.    int  D A si
subficx   int  D A si
mulli     int  D A si
# ---- logical, shifts
andx      int  a S B rc
andcx     int  a S B rc
orx       int  a S B rc
orcx      int  a S B rc
xorx      int  a S B rc
norx      int  a S B rc
nandx     int  a S B rc
eqvx      int  a S B rc
andi.     int  a S ui
andis.    int  a S ui
ori       int  a S ui
oris      int  a S ui
xori      int  a S ui
xoris     int  a S ui
extsbx    int  a S rc
extshx    int  a S rc
extswx    int  a S rc
cntlzwx   int  a S rc
cntlzdx   int  a S rc
slwx      rot  a S B rc
srwx      rot  a S B rc
srawx     rot  a S B rc
srawix    rot  a S sh rc
sldx      rot  a S B rc
srdx      rot  a S B rc
sradx     rot  a S B rc
sradix    rot  a S sh6 rc
# ---- rotate and mask
rlwimix   rot  ad S sh mb me rc
rlwinmx   rot  a S sh mb me rc
rlwnmx    rot  a S B mb me rc
rldiclx   rot  a S sh6 mb6 rc
rldicrx   rot  a S sh6 mb6 rc
rldicx    rot  a S sh6 mb6 rc
rldimix   rot  ad S sh6 mb6 rc
rldclx    rot  a S B mb6 rc
rldcrx    rot  a S B mb6 rc
# ---- compares, condition register, special registers
cmp       cr   cf L A B
cmpl      cr   cf L A B
cmpi      cr   cf L A si
cmpli     cr   cf L A ui
crand     cr   bt ba bb
crandc    cr   bt ba bb
creqv     cr   bt ba bb
crnand    cr   bt ba bb
crnor     cr   bt ba bb
cror      cr   bt ba bb
crorc     cr   bt ba bb
crxor     cr   bt ba bb
mcrf      cr   cf cs
mfcr      cr   D                   !t1
mtcrf     cr   S fxm
mfspr     cr   D spr
mtspr     cr   S spr
# ---- loads and stores
lbz       mem  D P d
lbzu      mem  D PU du
lbzx      mem  D P X
lbzux     mem  D PU X
lhz       mem  D P d
lhzu      mem  D PU du
lhzx      mem  D P X
lhzux     mem  D PU X
lha       mem  D P d
lhau      mem  D PU du
lhax      mem  D P X
lhaux     mem  D PU X
lwz       mem  D P d
lwzu      mem  D PU du
lwzx      mem  D P X
lwzux     mem  D PU X
lwa       mem  D P ds
lwax      mem  D P X
lwaux     mem  D PU X
ld        mem  D P ds
ldu       mem  D PU dsu
ldx       mem  D P X
ldux      mem  D PU X
stb       mem  S P d
stbu      mem  S PU du
stbx      mem  S P X
stbux     mem  S PU X
sth       mem  S P d
sthu      mem  S PU du
sthx      mem  S P X
sthux     mem  S PU X
stw       mem  S P d
stwu      mem  S PU du
stwx      mem  S P X
stwux     mem  S PU X
std       mem  S P ds
stdu      mem  S PU dsu
stdx      mem  S P X
stdux     mem  S PU X
lhbrx     mem  D P X
lwbrx     mem  D P X
sthbrx    mem  S P X
stwbrx    mem  S P X
stmw      mem  S P d
lfs       mem  FD P d
lfsu      mem  FD PU du
lfsx      mem  FD P X
lfsux     mem  FD PU X
lfd       mem  FD P d
lfdu      mem  FD PU du
lfdx      mem  FD P X
lfdux     mem  FD PU X
stfs      mem  FS P d
stfsu     mem  FS PU du
stfsx     mem  FS P X
stfsux    mem  FS PU X
stfd      mem  FS P d
stfdu     mem  FS PU du
stfdx     mem  FS P X
stfdux    mem  FS PU X
stfiwx    mem  FS P X
dcbt      mem  P X
dcbtst    mem  P X
# ---- reservations (the generator pairs them: see dt_gen.h)
lwarx     resv D P X               !resl
ldarx     resv D P X               !resl
stwcx     resv S P X               !ress
stdcx     resv S P X               !ress
# ---- floating point
faddx     fpu  FD FA FB frc
faddsx    fpu  FD FA FB frc
fsubx     fpu  FD FA FB frc
fsubsx    fpu  FD FA FB frc
fmulx     fpu  FD FA FC frc
fmulsx    fpu  FD FA FC frc
fdivx     fpu  FD FA FB frc
fdivsx    fpu  FD FA FB frc
fmaddx    fpu  FD FA FC FB frc
fmaddsx   fpu  FD FA FC FB frc
fmsubx    fpu  FD FA FC FB frc
fmsubsx   fpu  FD FA FC FB frc
fnmaddx   fpu  FD FA FC FB frc
fnmaddsx  fpu  FD FA FC FB frc
fnmsubx   fpu  FD FA FC FB frc
fnmsubsx  fpu  FD FA FC FB frc
fsqrtx    fpu  FD FB frc
fsqrtsx   fpu  FD FB frc
fselx     fpu  FD FA FC FB frc
fresx     fpu  FD FB frc           !tol
frsqrtex  fpu  FD FB frc           !tol
fabsx     fpu  FD FB frc
fnabsx    fpu  FD FB frc
fnegx     fpu  FD FB frc
fmrx      fpu  FD FB frc
frspx     fpu  FD FB frc
fctiwx    fpu  FD FB frc           !fhi32
fctiwzx   fpu  FD FB frc           !fhi32
fctidx    fpu  FD FB frc
fctidzx   fpu  FD FB frc
fcfidx    fpu  FD FB frc
fcmpu     fpu  cf FA FB
fcmpo     fpu  cf FA FB
mtfsfx    fpu  fm FB               !mtfsf
mtfsfix   fpu  rn
# ---- VMX, integer
vaddubm   vmx  VD VA VB
vadduhm   vmx  VD VA VB
vadduwm   vmx  VD VA VB
vaddcuw   vmx  VD VA VB
vaddubs   vmx  VD VA VB
vadduhs   vmx  VD VA VB
vadduws   vmx  VD VA VB
vaddsbs   vmx  VD VA VB
vaddshs   vmx  VD VA VB
vaddsws   vmx  VD VA VB
vsububm   vmx  VD VA VB
vsubuhm   vmx  VD VA VB
vsubuwm   vmx  VD VA VB
vsubcuw   vmx  VD VA VB
vsububs   vmx  VD VA VB
vsubuhs   vmx  VD VA VB
vsubuws   vmx  VD VA VB
vsubsbs   vmx  VD VA VB
vsubshs   vmx  VD VA VB
vsubsws   vmx  VD VA VB
vavgub    vmx  VD VA VB
vavguh    vmx  VD VA VB
vavguw    vmx  VD VA VB
vavgsb    vmx  VD VA VB
vavgsh    vmx  VD VA VB
vavgsw    vmx  VD VA VB
vmaxub    vmx  VD VA VB
vmaxuh    vmx  VD VA VB
vmaxuw    vmx  VD VA VB
vmaxsb    vmx  VD VA VB
vmaxsh    vmx  VD VA VB
vmaxsw    vmx  VD VA VB
vminub    vmx  VD VA VB
vminuh    vmx  VD VA VB
vminuw    vmx  VD VA VB
vminsb    vmx  VD VA VB
vminsh    vmx  VD VA VB
vminsw    vmx  VD VA VB
vmuleub   vmx  VD VA VB
vmuleuh   vmx  VD VA VB
vmulesb   vmx  VD VA VB
vmulesh   vmx  VD VA VB
vmuloub   vmx  VD VA VB
vmulouh   vmx  VD VA VB
vmulosb   vmx  VD VA VB
vmulosh   vmx  VD VA VB
vmhaddshs vmx  VD VA VB VC
vmhraddshs vmx VD VA VB VC
vmladduhm vmx  VD VA VB VC
vmsumubm  vmx  VD VA VB VC
vmsummbm  vmx  VD VA VB VC
vmsumuhm  vmx  VD VA VB VC
vmsumuhs  vmx  VD VA VB VC
vmsumshm  vmx  VD VA VB VC
vmsumshs  vmx  VD VA VB VC
vsum4ubs  vmx  VD VA VB
vsum4sbs  vmx  VD VA VB
vsum4shs  vmx  VD VA VB
vsum2sws  vmx  VD VA VB
vsumsws   vmx  VD VA VB
vand      vmx  VD VA VB
vandc     vmx  VD VA VB
vor       vmx  VD VA VB
vnor      vmx  VD VA VB
vxor      vmx  VD VA VB
vrlb      vmx  VD VA VB
vrlh      vmx  VD VA VB
vrlw      vmx  VD VA VB
vslb      vmx  VD VA VB
vslh      vmx  VD VA VB
vslw      vmx  VD VA VB
vsrb      vmx  VD VA VB
vsrh      vmx  VD VA VB
vsrw      vmx  VD VA VB
vsrab     vmx  VD VA VB
vsrah     vmx  VD VA VB
vsraw     vmx  VD VA VB
vsl       vmx  VD VA VBsh
vsr       vmx  VD VA VBsh
vslo      vmx  VD VA VB
vsro      vmx  VD VA VB
vsldoi    vmx  VD VA VB shb
vmrghb    vmx  VD VA VB
vmrghh    vmx  VD VA VB
vmrghw    vmx  VD VA VB
vmrglb    vmx  VD VA VB
vmrglh    vmx  VD VA VB
vmrglw    vmx  VD VA VB
vpkuhum   vmx  VD VA VB
vpkuwum   vmx  VD VA VB
vpkuhus   vmx  VD VA VB
vpkuwus   vmx  VD VA VB
vpkshus   vmx  VD VA VB
vpkswus   vmx  VD VA VB
vpkshss   vmx  VD VA VB
vpkswss   vmx  VD VA VB
vpkpx     vmx  VD VA VB
vupkhsb   vmx  VD VB
vupkhsh   vmx  VD VB
vupklsb   vmx  VD VB
vupklsh   vmx  VD VB
vupkhpx   vmx  VD VB
vupklpx   vmx  VD VB
vspltb    vmx  VD VB u4
vsplth    vmx  VD VB u3
vspltw    vmx  VD VB u2
vspltisb  vmx  VD s5
vspltish  vmx  VD s5
vspltisw  vmx  VD s5
vperm     vmx  VD VA VB VC
vsel      vmx  VD VA VB VC
vcmpequb  vmx  VD VA VB vrc
vcmpequh  vmx  VD VA VB vrc
vcmpequw  vmx  VD VA VB vrc
vcmpgtub  vmx  VD VA VB vrc
vcmpgtuh  vmx  VD VA VB vrc
vcmpgtuw  vmx  VD VA VB vrc
vcmpgtsb  vmx  VD VA VB vrc
vcmpgtsh  vmx  VD VA VB vrc
vcmpgtsw  vmx  VD VA VB vrc
mfvscr    vmx  VD                  !t1
mtvscr    vmx  VBnj                !first
# ---- VMX, float
vaddfp    vmxf VD VA VB
vsubfp    vmxf VD VA VB
vmaddfp   vmxf VD VA VC VB
vnmsubfp  vmxf VD VA VC VB
vmaxfp    vmxf VD VA VB
vminfp    vmxf VD VA VB
vrefp     vmxf VD VB               !tol
vrsqrtefp vmxf VD VB               !tol
vexptefp  vmxf VD VB               !tol
vlogefp   vmxf VD VB               !tol
vrfin     vmxf VD VB
vrfiz     vmxf VD VB
vrfip     vmxf VD VB
vrfim     vmxf VD VB
vcfsx     vmxf VD VB u5
vcfux     vmxf VD VB u5
vctsxs    vmxf VD VB u5            !vint
vctuxs    vmxf VD VB u5            !vint
vcmpeqfp  vmxf VD VA VB vrc
vcmpgefp  vmxf VD VA VB vrc
vcmpgtfp  vmxf VD VA VB vrc
vcmpbfp   vmxf VD VA VB vrc
# ---- VMX loads and stores
lvx       vmem VD P X
lvxl      vmem VD P X
stvx      vmem VS P X
stvxl     vmem VS P X
lvebx     vmem VD P X              !elem
lvehx     vmem VD P X              !elem
lvewx     vmem VD P X              !elem
stvebx    vmem VS P X
stvehx    vmem VS P X
stvewx    vmem VS P X
lvsl      vmem VD P X
lvsr      vmem VD P X
lvlx      vmem VD P X              => @lvlx
lvlxl     vmem VD P X              => @lvlx
lvrx      vmem VD P X              => @lvrx
lvrxl     vmem VD P X              => @lvrx
stvlx     vmem VS P X              => @stvlx
stvlxl    vmem VS P X              => @stvlx
stvrx     vmem VS P X              => @stvrx
stvrxl    vmem VS P X              => @stvrx
# ---- VMX128
vaddfp128   v128f VD128 VA128 VB128        => vaddfp D=0 A=1 B=2
vsubfp128   v128f VD128 VA128 VB128        => vsubfp D=0 A=1 B=2
vmulfp128   v128f VD128 VA128 VB128        => @@vmulfp
vmaddfp128  v128f VDS128 VA128 VB128       => vmaddfp D=0 A=1 C=2 B=0
vmaddcfp128 v128f VDS128 VA128 VB128       => vmaddfp D=0 A=1 C=0 B=2
vnmsubfp128 v128f VDS128 VA128 VB128       => vnmsubfp D=0 A=1 C=2 B=0
vmaxfp128   v128f VD128 VA128 VB128        => vmaxfp D=0 A=1 B=2
vminfp128   v128f VD128 VA128 VB128        => vminfp D=0 A=1 B=2
vmsum3fp128 v128f VD128 VA128 VB128        => @vmsum3fp     !tol
vmsum4fp128 v128f VD128 VA128 VB128        => @vmsum4fp     !tol
vrefp128    v128f VD128 VB128              => vrefp D=0 B=1 !tol
vrsqrtefp128 v128f VD128 VB128             => vrsqrtefp D=0 B=1 !tol
vexptefp128 v128f VD128 VB128              => vexptefp D=0 B=1 !tol
vlogefp128  v128f VD128 VB128              => vlogefp D=0 B=1 !tol
vrfin128    v128f VD128 VB128              => vrfin D=0 B=1
vrfiz128    v128f VD128 VB128              => vrfiz D=0 B=1
vrfip128    v128f VD128 VB128              => vrfip D=0 B=1
vrfim128    v128f VD128 VB128              => vrfim D=0 B=1
vcsxwfp128  v128f VD128 VB128 u5           => vcfsx D=0 B=1 I=2
vcuxwfp128  v128f VD128 VB128 u5           => vcfux D=0 B=1 I=2
vcfpsxws128 v128f VD128 VB128 u5           => vctsxs D=0 B=1 I=2 !vint
vcfpuxws128 v128f VD128 VB128 u5           => vctuxs D=0 B=1 I=2 !vint
vcmpeqfp128 v128f VD128 VA128 VB128 rc6    => vcmpeqfp D=0 A=1 B=2 RC=3
vcmpgefp128 v128f VD128 VA128 VB128 rc6    => vcmpgefp D=0 A=1 B=2 RC=3
vcmpgtfp128 v128f VD128 VA128 VB128 rc6    => vcmpgtfp D=0 A=1 B=2 RC=3
vcmpbfp128  v128f VD128 VA128 VB128 rc6    => vcmpbfp D=0 A=1 B=2 RC=3
vcmpequw128 v128  VD128 VA128 VB128 rc6    => vcmpequw D=0 A=1 B=2 RC=3
vand128     v128  VD128 VA128 VB128        => vand D=0 A=1 B=2
vandc128    v128  VD128 VA128 VB128        => vandc D=0 A=1 B=2
vor128      v128  VD128 VA128 VB128        => vor D=0 A=1 B=2
vnor128     v128  VD128 VA128 VB128        => vnor D=0 A=1 B=2
vxor128     v128  VD128 VA128 VB128        => vxor D=0 A=1 B=2
vsel128     v128  VDS128 VA128 VB128       => vsel D=0 A=1 B=2 C=0
vrlw128     v128  VD128 VA128 VB128        => vrlw D=0 A=1 B=2
vslw128     v128  VD128 VA128 VB128        => vslw D=0 A=1 B=2
vsrw128     v128  VD128 VA128 VB128        => vsrw D=0 A=1 B=2
vsraw128    v128  VD128 VA128 VB128        => vsraw D=0 A=1 B=2
vslo128     v128  VD128 VA128 VB128        => vslo D=0 A=1 B=2
vsro128     v128  VD128 VA128 VB128        => vsro D=0 A=1 B=2
vmrghw128   v128  VD128 VA128 VB128        => vmrghw D=0 A=1 B=2
vmrglw128   v128  VD128 VA128 VB128        => vmrglw D=0 A=1 B=2
vpkshss128  v128  VD128 VA128 VB128        => vpkshss D=0 A=1 B=2
vpkshus128  v128  VD128 VA128 VB128        => vpkshus D=0 A=1 B=2
vpkswss128  v128  VD128 VA128 VB128        => vpkswss D=0 A=1 B=2
vpkswus128  v128  VD128 VA128 VB128        => vpkswus D=0 A=1 B=2
vpkuhum128  v128  VD128 VA128 VB128        => vpkuhum D=0 A=1 B=2
vpkuhus128  v128  VD128 VA128 VB128        => vpkuhus D=0 A=1 B=2
vpkuwum128  v128  VD128 VA128 VB128        => vpkuwum D=0 A=1 B=2
vpkuwus128  v128  VD128 VA128 VB128        => vpkuwus D=0 A=1 B=2
vupkhsb128  v128  VD128 VB128              => vupkhsb D=0 B=1
vupklsb128  v128  VD128 VB128              => vupklsb D=0 B=1
vspltw128   v128  VD128 VB128 u2           => vspltw D=0 B=1 I=2
vspltisw128 v128  VD128 s5                 => vspltisw D=0 I=1
vsldoi128   v128  VD128 VA128 VB128 sh4    => vsldoi D=0 A=1 B=2 SHB=3
vperm128    v128  VD128 VA128 VB128 VC3    => vperm D=0 A=1 B=2 C=3
vpermwi128  v128  VD128 VB128 perm         => @vpermwi
vrlimi128   v128  VDS128 VB128 u5z z2      => @vrlimi
lvx128      v128m VD128 P X                => lvx D=0 A=1 B=2
lvxl128     v128m VD128 P X                => lvx D=0 A=1 B=2
stvx128     v128m VS128 P X                => stvx D=0 A=1 B=2
stvxl128    v128m VS128 P X                => stvx D=0 A=1 B=2
lvewx128    v128m VD128 P X                => lvewx D=0 A=1 B=2  !elem
stvewx128   v128m VS128 P X                => stvewx D=0 A=1 B=2
lvsl128     v128m VD128 P X                => lvsl D=0 A=1 B=2
lvsr128     v128m VD128 P X                => lvsr D=0 A=1 B=2
lvlx128     v128m VD128 P X                => @lvlx
lvlxl128    v128m VD128 P X                => @lvlx
lvrx128     v128m VD128 P X                => @lvrx
lvrxl128    v128m VD128 P X                => @lvrx
stvlx128    v128m VS128 P X                => @stvlx
stvlxl128   v128m VS128 P X                => @stvlx
stvrx128    v128m VS128 P X                => @stvrx
stvrxl128   v128m VS128 P X                => @stvrx
"""

# Instructions of the core's list that the table leaves out, with the reason
# (printed into dt_table.inc and from there into the report).
NOT_COVERED = {
    'bx': 'branches other than the generator\'s forward bc are not generated',
    'bcx': 'generated by the sequence generator itself (a forward bc over one instruction)',
    'bcctrx': 'indirect branches are not generated',
    'bclrx': 'every case ends in blr; other forms are not generated',
    'sc': 'needs a kernel',
    'tdi': 'traps are not generated', 'twi': 'traps are not generated',
    'td': 'traps are not generated', 'tw': 'traps are not generated',
    'dcbz': 'the block is 32 bytes on the console and 128 in the reference CPU',
    'dcbz128': 'no reference: not an instruction of the reference CPU',
    'dcbf': 'cache control, no visible state', 'dcbi': 'cache control, privileged',
    'dcbst': 'cache control, no visible state', 'icbi': 'cache control, no visible state',
    'eieio': 'no visible state', 'sync': 'no visible state', 'isync': 'no visible state',
    'lmw': 'would overwrite the registers the generator keeps as pointers',
    'lswi': 'string instructions are not generated', 'lswx': 'string instructions are not generated',
    'stswi': 'string instructions are not generated', 'stswx': 'string instructions are not generated',
    'ldbrx': 'not an instruction of the console\'s CPU', 'stdbrx': 'not an instruction of the console\'s CPU',
    'mcrxr': 'not an instruction of the reference CPU',
    'mcrfs': 'reads FPSCR status bits, which the translator does not keep',
    'mffsx': 'reads FPSCR status bits, which the translator does not keep',
    'mtfsb0x': 'the translator does not implement it (InstrEmit_mtfsb0x returns an error)',
    'mtfsb1x': 'the translator does not implement it (InstrEmit_mtfsb1x returns an error)',
    'mfmsr': 'privileged', 'mtmsr': 'privileged', 'mtmsrd': 'privileged',
    'mftb': 'the time base differs between any two machines',
    'vpkd3d128': 'no reference written yet (console only, no VMX equivalent)',
    'vupkd3d128': 'no reference written yet (console only, no VMX equivalent)',
}

# operand token -> (type, placement on the console side)
F = lambda shift, width: shift | (width << 5)
OPS = {
    'D': ('T_GD', F(21, 5)), 'S': ('T_GS', F(21, 5)), 'A': ('T_GS', F(16, 5)),
    'a': ('T_GD', F(16, 5)), 'ad': ('T_GDS', F(16, 5)), 'B': ('T_GS', F(11, 5)),
    'si': ('T_SI16', F(0, 16)), 'ui': ('T_UI16', F(0, 16)),
    'rc': ('T_RC', F(0, 1)), 'oe': ('T_OE', F(10, 1)), 'frc': ('T_FRC', F(0, 1)),
    'vrc': ('T_RC6', F(10, 1)), 'rc6': ('T_RC6', F(6, 1)),
    'P': ('T_PTR', F(16, 5)), 'PU': ('T_PTRU', F(16, 5)), 'X': ('T_IDX', F(11, 5)),
    'd': ('T_DISP', F(0, 16)), 'du': ('T_DISPU', F(0, 16)),
    'ds': ('T_DISP4', F(0, 16)), 'dsu': ('T_DISPU4', F(0, 16)),
    'sh': ('T_IMM', F(11, 5)), 'mb': ('T_IMM', F(6, 5)), 'me': ('T_IMM', F(1, 5)),
    'sh6': ('T_IMM6', 'PL_SH6'), 'mb6': ('T_IMM6', 'PL_MB6'),
    'cf': ('T_CRFD', F(23, 3)), 'cs': ('T_IMM', F(18, 3)), 'L': ('T_IMM', F(21, 1)),
    'bt': ('T_CRBD', F(21, 5)), 'ba': ('T_CRBS', F(16, 5)), 'bb': ('T_CRBS', F(11, 5)),
    'fxm': ('T_FXM', F(12, 8)), 'spr': ('T_SPR', 'PL_SPR'),
    'FD': ('T_FD', F(21, 5)), 'FS': ('T_FS', F(21, 5)), 'FA': ('T_FS', F(16, 5)),
    'FB': ('T_FS', F(11, 5)), 'FC': ('T_FS', F(6, 5)),
    'fm': ('T_FM', F(17, 8)), 'rn': ('T_RN', F(12, 4)),
    'VD': ('T_VD', F(21, 5)), 'VS': ('T_VS', F(21, 5)), 'VA': ('T_VS', F(16, 5)),
    'VB': ('T_VS', F(11, 5)), 'VC': ('T_VS', F(6, 5)),
    'VBsh': ('T_VSSH', F(11, 5)), 'VBnj': ('T_VSNJ', F(11, 5)),
    'u5': ('T_IMM', F(16, 5)), 's5': ('T_IMM', F(16, 5)), 'u4': ('T_IMM', F(16, 4)),
    'u3': ('T_IMM', F(16, 3)), 'u2': ('T_IMM', F(16, 2)), 'shb': ('T_IMM', F(6, 4)),
    'sh4': ('T_IMM', F(6, 4)), 'u5z': ('T_IMM', F(16, 5)), 'z2': ('T_IMM', F(6, 2)),
    'VD128': ('T_VD', 'PL_VD128'), 'VS128': ('T_VS', 'PL_VD128'),
    'VDS128': ('T_VDS', 'PL_VD128'), 'VA128': ('T_VS', 'PL_VA128'),
    'VB128': ('T_VS', 'PL_VB128'), 'VC3': ('T_VS8', F(6, 3)),
    'perm': ('T_IMM8', 'PL_PERM'),
}
# field of the reference instruction -> placement
REF_FIELDS = {'D': F(21, 5), 'A': F(16, 5), 'B': F(11, 5), 'C': F(6, 5),
              'I': F(16, 5), 'SHB': F(6, 4), 'RC': F(10, 1)}
CLASSES = ['int', 'rot', 'cr', 'mem', 'resv', 'fpu', 'vmx', 'vmxf', 'vmem',
           'v128', 'v128f', 'v128m']
FLAGS = ['hi32', 'divw', 'divwu', 'divd', 'divdu', 'tol', 'fhi32', 'mtfsf',
         'first', 'resl', 'ress', 'elem', 'vint', 'vmulfp', 't1', 'known']
PSEUDO = ['none', 'lvlx', 'lvrx', 'stvlx', 'stvrx', 'vmsum3fp', 'vmsum4fp',
          'vpermwi', 'vrlimi']


def pl(p):
    return p if isinstance(p, str) else '0x%x' % p


def main():
    xml = open(sys.argv[1]).read()
    opcodes = {}
    for m in re.finditer(r'<insn mnem="([^"]+)"\s+opcode="([^"]+)"', xml):
        opcodes[m.group(1)] = int(m.group(2), 16)
    # Instructions with a known difference stay out of the sequences, so that
    # a sequence that differs is always news.
    known = set()
    if len(sys.argv) > 3:
        for line in open(sys.argv[3]):
            line = line.split('#')[0].split()
            if line and line[0] != '*':
                known.add(line[0].split('+')[0].split(':')[-1])
    out = ['// Written by dt_table.py: do not edit.', '#ifdef DT_ROWS']
    seen = set()
    count = 0
    for line in SPEC.splitlines():
        line = line.split('#')[0].strip()
        if not line:
            continue
        flags = re.findall(r'!(\w+)', line)
        line = re.sub(r'!\w+', '', line)
        ref = None
        if '=>' in line:
            line, ref = [s.strip() for s in line.split('=>')]
        toks = line.split()
        name, cls, ops = toks[0], toks[1], toks[2:]
        if name not in opcodes:
            sys.exit('dt_table.py: %s is not in the instruction list' % name)
        if name in seen:
            sys.exit('dt_table.py: %s twice' % name)
        seen.add(name)
        assert cls in CLASSES, cls
        for f in flags:
            assert f in FLAGS, f
        opx = opcodes[name]
        opr = opx
        pseudo = 'none'
        refpl = {}
        fl = ['IF_' + f.upper() for f in flags]
        if name in known:
            fl.append('IF_KNOWN')
        if ref:
            if ref.startswith('@@'):
                fl.append('IF_' + ref[2:].upper())
                opr = opcodes['vmaddfp']
            elif ref.startswith('@'):
                pseudo = ref[1:]
                assert pseudo in PSEUDO, pseudo
                opr = 0
            else:
                rt = ref.split()
                opr = opcodes[rt[0]]
                for kv in rt[1:]:
                    k, v = kv.split('=')
                    refpl.setdefault(int(v), []).append(REF_FIELDS[k])
        entries = []
        for i, o in enumerate(ops):
            t, px = OPS[o]
            if ref and not ref.startswith('@'):
                r = refpl.get(i, [])
                pr = pl(r[0]) if r else 'PL_NONE'
                pr2 = pl(r[1]) if len(r) > 1 else 'PL_NONE'
            elif ref and ref.startswith('@@'):
                pr, pr2 = 'PL_NONE', 'PL_NONE'
            else:
                pr, pr2 = pl(px), 'PL_NONE'
            entries.append('{%s, %s, %s, %s}' % (t, pl(px), pr, pr2))
        out.append('{"%s", 0x%08xu, 0x%08xu, CL_%s, %s, PS_%s, %d, {%s}},' % (
            name, opx, opr, cls.upper(), ' | '.join(fl) if fl else '0',
            pseudo.upper(), len(entries), ', '.join(entries)))
        count += 1
    missing = sorted(set(opcodes) - seen - set(NOT_COVERED))
    if missing:
        sys.exit('dt_table.py: neither in the table nor in NOT_COVERED: ' + ' '.join(missing))
    out.append('#endif')
    out.append('#ifdef DT_NOT_COVERED')
    for name in sorted(NOT_COVERED):
        out.append('DT_NOT_COVERED("%s", "%s")' % (name, NOT_COVERED[name].replace('"', "'")))
    out.append('#endif')
    open(sys.argv[2], 'w').write('\n'.join(out) + '\n')
    print('dt_table.py: %d instructions, %d left out' % (count, len(NOT_COVERED)))


if __name__ == '__main__':
    main()
