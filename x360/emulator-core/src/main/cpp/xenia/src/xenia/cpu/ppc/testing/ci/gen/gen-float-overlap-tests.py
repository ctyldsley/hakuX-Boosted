# Writes seq_float_overlap.s. Expected values are filled in from a file of
# "name reg value" lines when given (taken from a run with every setting off).
import sys
known = {}
if len(sys.argv) > 1:
    for line in open(sys.argv[1]):
        p = line.split(None, 2)
        if len(p) == 3:
            known[(p[0], p[1])] = p[2].strip()
SN = '0x7FF0000000000001'; QN = '0x7FF8000000000123'; QN2 = '0xFFF8000000000456'
INF = '0x7FF0000000000000'; NINF = '0xFFF0000000000000'
ONE = '0x3FF0000000000000'; ZERO = '0x0000000000000000'; THREE = '0x4008000000000000'
tests = []
def scalar(name, ins, code, outs):
    tests.append((name, ins, code, outs))
D = [
 ('fadd_snan_first_dest_a', {'f1': SN, 'f2': ONE}, 'fadd f1, f1, f2', ['f1']),
 ('fadd_qnan_second_dest_b', {'f1': ONE, 'f2': QN}, 'fadd f2, f1, f2', ['f2']),
 ('fadd_two_nans_dest_a', {'f1': QN2, 'f2': SN}, 'fadd f1, f1, f2', ['f1']),
 ('fadd_two_nans_dest_b', {'f1': SN, 'f2': QN2}, 'fadd f2, f1, f2', ['f2']),
 ('fsub_inf_inf_same_reg', {'f1': INF}, 'fsub f1, f1, f1', ['f1']),
 ('fmul_zero_inf_dest_b', {'f1': ZERO, 'f2': INF}, 'fmul f2, f1, f2', ['f2']),
 ('fdiv_zero_zero_dest_a', {'f1': ZERO, 'f2': ZERO}, 'fdiv f1, f1, f2', ['f1']),
 ('fdiv_normal_dest_b', {'f1': THREE, 'f2': ONE}, 'fdiv f2, f1, f2', ['f2']),
 ('fmadd_nan_in_b_dest_a', {'f1': ONE, 'f2': THREE, 'f3': QN}, 'fmadd f1, f1, f2, f3', ['f1']),
 ('fmadd_nan_in_c_and_b_dest_b', {'f1': ONE, 'f2': SN, 'f3': QN2}, 'fmadd f3, f1, f2, f3', ['f3']),
 ('fmadd_nan_in_a_and_b_dest_c', {'f1': QN, 'f2': ONE, 'f3': SN}, 'fmadd f2, f1, f2, f3', ['f2']),
 ('fmadd_inf_times_zero_dest_c', {'f1': INF, 'f2': ZERO, 'f3': ONE}, 'fmadd f2, f1, f2, f3', ['f2']),
 ('fmadd_all_one_register', {'f1': THREE}, 'fmadd f1, f1, f1, f1', ['f1']),
 ('fmsub_inf_minus_inf_dest_a', {'f1': INF, 'f2': ONE, 'f3': INF}, 'fmsub f1, f1, f2, f3', ['f1']),
 ('fmsub_nan_in_b_dest_b', {'f1': THREE, 'f2': THREE, 'f3': SN}, 'fmsub f3, f1, f2, f3', ['f3']),
 ('fnmsub_normal_dest_a', {'f1': THREE, 'f2': THREE, 'f3': ONE}, 'fnmsub f1, f1, f2, f3', ['f1']),
 ('fadds_snan_first_dest_a', {'f1': SN, 'f2': ONE}, 'fadds f1, f1, f2', ['f1']),
 ('fmuls_normal_same_reg', {'f1': THREE}, 'fmuls f1, f1, f1', ['f1']),
 ('fmadds_nan_in_a_dest_b', {'f1': QN, 'f2': THREE, 'f3': ONE}, 'fmadds f3, f1, f2, f3', ['f3']),
 ('fdivs_zero_zero_dest_b', {'f1': ZERO, 'f2': ZERO}, 'fdivs f2, f1, f2', ['f2']),
 # A chain in one block: each result feeds the next, with a NaN appearing
 # half way.
 ('float_chain_nan_half_way', {'f1': ONE, 'f2': THREE, 'f3': INF},
  'fadd f1, f1, f2\n  fmul f2, f1, f2\n  fsub f4, f3, f3\n  fadd f1, f1, f4\n  fmadd f2, f2, f1, f3\n  fadd f3, f2, f2', ['f1', 'f2', 'f3', 'f4']),
]
def v(a, b, c, d):
    return '[%s, %s, %s, %s]' % (a, b, c, d)
fSN = '7F800001'; fQN = '7FC00123'; fQN2 = 'FFC00456'; fINF = '7F800000'; fNINF = 'FF800000'
f1 = '3F800000'; f2 = '40000000'; f3 = '40400000'; f0 = '00000000'
V = [
 ('vaddfp_nan_lanes_dest_a', {'v1': v(fSN, f1, fINF, f2), 'v2': v(f1, fQN, fNINF, f3)}, 'vaddfp v1, v1, v2', ['v1']),
 ('vaddfp_nan_lanes_dest_b', {'v1': v(fSN, f1, fINF, fQN2), 'v2': v(fQN, fQN, fNINF, fSN)}, 'vaddfp v2, v1, v2', ['v2']),
 ('vsubfp_inf_inf_same_reg', {'v1': v(fINF, f1, fNINF, fQN)}, 'vsubfp v1, v1, v1', ['v1']),
 ('vmaddfp_nan_lanes_dest_a', {'v1': v(fQN, f1, fINF, f2), 'v2': v(f1, fSN, f0, f3), 'v3': v(fQN2, f1, f1, fSN)}, 'vmaddfp v1, v1, v2, v3', ['v1']),
 ('vmaddfp_nan_lanes_dest_c', {'v1': v(f1, f1, fINF, f2), 'v2': v(f1, fSN, f1, f3), 'v3': v(fQN2, fQN, fNINF, f1)}, 'vmaddfp v2, v1, v2, v3', ['v2']),
 ('vmaddfp_nan_lanes_dest_b', {'v1': v(f1, fQN, f2, f2), 'v2': v(fSN, fSN, f3, f3), 'v3': v(fQN2, f1, f1, fQN)}, 'vmaddfp v3, v1, v2, v3', ['v3']),
 ('vnmsubfp_nan_lanes_dest_a', {'v1': v(fINF, fQN, f2, f2), 'v2': v(f1, f1, f3, fSN), 'v3': v(fINF, f1, f1, fQN2)}, 'vnmsubfp v1, v1, v2, v3', ['v1']),
 ('vnmsubfp_nan_lanes_dest_b', {'v1': v(f2, f2, f2, fSN), 'v2': v(f3, fQN, f3, f1), 'v3': v(f1, fSN, fQN2, f1)}, 'vnmsubfp v3, v1, v2, v3', ['v3']),
 ('vmaddfp_all_one_register', {'v1': v(f2, f3, fINF, fQN)}, 'vmaddfp v1, v1, v1, v1', ['v1']),
 ('vector_chain_nan_half_way', {'v1': v(f1, f2, f3, f1), 'v2': v(f2, f2, fINF, f3), 'v3': v(f1, f1, fNINF, f0)},
  'vaddfp v1, v1, v2\n  vmaddfp v2, v1, v2, v3\n  vsubfp v3, v2, v2\n  vaddfp v1, v1, v3\n  vnmsubfp v2, v2, v1, v3', ['v1', 'v2', 'v3']),
]
out = ['''# hakuX-Boosted: float and VMX float operations whose destination is one of
# their own operands, with NaNs among the operands and NaNs made by the
# operation (inf - inf, 0 * inf, 0 / 0). The generated instruction tests
# cover PowerPC's choice of NaN with three distinct registers; these cover
# it where the result overwrites an operand, which is where a translation
# that does the operation first and looks at the operands afterwards
# (a64_tight_sequences) could go wrong, and in chains inside one block.
#
# Written by a script (the values are bit patterns); the expected values
# were taken from a run of the translator with every setting off, which
# passes the generated tests of the same instructions.
''']
for name, ins, code, outs in D + V:
    out.append('test_%s:' % name)
    for reg in sorted(ins):
        out.append('  #_ REGISTER_IN %s %s' % (reg, ins[reg]))
    out.append('  ' + code)
    out.append('  blr')
    for reg in outs:
        default = '[00000000, 00000000, 00000000, 00000000]' if reg[0] == 'v' else '0x0000000000000000'
        out.append('  #_ REGISTER_OUT %s %s' % (reg, known.get((name, reg), default)))
    out.append('')
open('seq_float_overlap.s', 'w').write('\n'.join(out))
