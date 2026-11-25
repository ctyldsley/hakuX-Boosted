# hakuX-Boosted: float and VMX float operations whose destination is one of
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
# The NaN an invalid operation makes was x86's (0xFFF8...) in that run;
# PowerPC's is 0x7FF8000000000000 (the difftest fix), and the ten such
# expected values were changed to it by hand.

test_fadd_snan_first_dest_a:
  #_ REGISTER_IN f1 0x7FF0000000000001
  #_ REGISTER_IN f2 0x3FF0000000000000
  fadd f1, f1, f2
  blr
  #_ REGISTER_OUT f1 0x7FF8000000000001

test_fadd_qnan_second_dest_b:
  #_ REGISTER_IN f1 0x3FF0000000000000
  #_ REGISTER_IN f2 0x7FF8000000000123
  fadd f2, f1, f2
  blr
  #_ REGISTER_OUT f2 0x7FF8000000000123

test_fadd_two_nans_dest_a:
  #_ REGISTER_IN f1 0xFFF8000000000456
  #_ REGISTER_IN f2 0x7FF0000000000001
  fadd f1, f1, f2
  blr
  #_ REGISTER_OUT f1 0xFFF8000000000456

test_fadd_two_nans_dest_b:
  #_ REGISTER_IN f1 0x7FF0000000000001
  #_ REGISTER_IN f2 0xFFF8000000000456
  fadd f2, f1, f2
  blr
  #_ REGISTER_OUT f2 0x7FF8000000000001

test_fsub_inf_inf_same_reg:
  #_ REGISTER_IN f1 0x7FF0000000000000
  fsub f1, f1, f1
  blr
  #_ REGISTER_OUT f1 0x7FF8000000000000

test_fmul_zero_inf_dest_b:
  #_ REGISTER_IN f1 0x0000000000000000
  #_ REGISTER_IN f2 0x7FF0000000000000
  fmul f2, f1, f2
  blr
  #_ REGISTER_OUT f2 0x7FF8000000000000

test_fdiv_zero_zero_dest_a:
  #_ REGISTER_IN f1 0x0000000000000000
  #_ REGISTER_IN f2 0x0000000000000000
  fdiv f1, f1, f2
  blr
  #_ REGISTER_OUT f1 0x7FF8000000000000

test_fdiv_normal_dest_b:
  #_ REGISTER_IN f1 0x4008000000000000
  #_ REGISTER_IN f2 0x3FF0000000000000
  fdiv f2, f1, f2
  blr
  #_ REGISTER_OUT f2 0x4008000000000000

test_fmadd_nan_in_b_dest_a:
  #_ REGISTER_IN f1 0x3FF0000000000000
  #_ REGISTER_IN f2 0x4008000000000000
  #_ REGISTER_IN f3 0x7FF8000000000123
  fmadd f1, f1, f2, f3
  blr
  #_ REGISTER_OUT f1 0x7FF8000000000123

test_fmadd_nan_in_c_and_b_dest_b:
  #_ REGISTER_IN f1 0x3FF0000000000000
  #_ REGISTER_IN f2 0x7FF0000000000001
  #_ REGISTER_IN f3 0xFFF8000000000456
  fmadd f3, f1, f2, f3
  blr
  #_ REGISTER_OUT f3 0x7FF8000000000001

test_fmadd_nan_in_a_and_b_dest_c:
  #_ REGISTER_IN f1 0x7FF8000000000123
  #_ REGISTER_IN f2 0x3FF0000000000000
  #_ REGISTER_IN f3 0x7FF0000000000001
  fmadd f2, f1, f2, f3
  blr
  #_ REGISTER_OUT f2 0x7FF8000000000123

test_fmadd_inf_times_zero_dest_c:
  #_ REGISTER_IN f1 0x7FF0000000000000
  #_ REGISTER_IN f2 0x0000000000000000
  #_ REGISTER_IN f3 0x3FF0000000000000
  fmadd f2, f1, f2, f3
  blr
  #_ REGISTER_OUT f2 0x7FF8000000000000

test_fmadd_all_one_register:
  #_ REGISTER_IN f1 0x4008000000000000
  fmadd f1, f1, f1, f1
  blr
  #_ REGISTER_OUT f1 0x4028000000000000

test_fmsub_inf_minus_inf_dest_a:
  #_ REGISTER_IN f1 0x7FF0000000000000
  #_ REGISTER_IN f2 0x3FF0000000000000
  #_ REGISTER_IN f3 0x7FF0000000000000
  fmsub f1, f1, f2, f3
  blr
  #_ REGISTER_OUT f1 0x7FF8000000000000

test_fmsub_nan_in_b_dest_b:
  #_ REGISTER_IN f1 0x4008000000000000
  #_ REGISTER_IN f2 0x4008000000000000
  #_ REGISTER_IN f3 0x7FF0000000000001
  fmsub f3, f1, f2, f3
  blr
  #_ REGISTER_OUT f3 0x7FF8000000000001

test_fnmsub_normal_dest_a:
  #_ REGISTER_IN f1 0x4008000000000000
  #_ REGISTER_IN f2 0x4008000000000000
  #_ REGISTER_IN f3 0x3FF0000000000000
  fnmsub f1, f1, f2, f3
  blr
  #_ REGISTER_OUT f1 0xC020000000000000

test_fadds_snan_first_dest_a:
  #_ REGISTER_IN f1 0x7FF0000000000001
  #_ REGISTER_IN f2 0x3FF0000000000000
  fadds f1, f1, f2
  blr
  #_ REGISTER_OUT f1 0x7FF8000000000000

test_fmuls_normal_same_reg:
  #_ REGISTER_IN f1 0x4008000000000000
  fmuls f1, f1, f1
  blr
  #_ REGISTER_OUT f1 0x4022000000000000

test_fmadds_nan_in_a_dest_b:
  #_ REGISTER_IN f1 0x7FF8000000000123
  #_ REGISTER_IN f2 0x4008000000000000
  #_ REGISTER_IN f3 0x3FF0000000000000
  fmadds f3, f1, f2, f3
  blr
  #_ REGISTER_OUT f3 0x7FF8000000000000

test_fdivs_zero_zero_dest_b:
  #_ REGISTER_IN f1 0x0000000000000000
  #_ REGISTER_IN f2 0x0000000000000000
  fdivs f2, f1, f2
  blr
  #_ REGISTER_OUT f2 0x7FF8000000000000

test_float_chain_nan_half_way:
  #_ REGISTER_IN f1 0x3FF0000000000000
  #_ REGISTER_IN f2 0x4008000000000000
  #_ REGISTER_IN f3 0x7FF0000000000000
  fadd f1, f1, f2
  fmul f2, f1, f2
  fsub f4, f3, f3
  fadd f1, f1, f4
  fmadd f2, f2, f1, f3
  fadd f3, f2, f2
  blr
  #_ REGISTER_OUT f1 0x7FF8000000000000
  #_ REGISTER_OUT f2 0x7FF8000000000000
  #_ REGISTER_OUT f3 0x7FF8000000000000
  #_ REGISTER_OUT f4 0x7FF8000000000000

test_vaddfp_nan_lanes_dest_a:
  #_ REGISTER_IN v1 [7F800001, 3F800000, 7F800000, 40000000]
  #_ REGISTER_IN v2 [3F800000, 7FC00123, FF800000, 40400000]
  vaddfp v1, v1, v2
  blr
  #_ REGISTER_OUT v1 [7FC00001, 7FC00123, FFC00000, 40A00000]

test_vaddfp_nan_lanes_dest_b:
  #_ REGISTER_IN v1 [7F800001, 3F800000, 7F800000, FFC00456]
  #_ REGISTER_IN v2 [7FC00123, 7FC00123, FF800000, 7F800001]
  vaddfp v2, v1, v2
  blr
  #_ REGISTER_OUT v2 [7FC00001, 7FC00123, FFC00000, FFC00456]

test_vsubfp_inf_inf_same_reg:
  #_ REGISTER_IN v1 [7F800000, 3F800000, FF800000, 7FC00123]
  vsubfp v1, v1, v1
  blr
  #_ REGISTER_OUT v1 [FFC00000, 00000000, FFC00000, 7FC00123]

test_vmaddfp_nan_lanes_dest_a:
  #_ REGISTER_IN v1 [7FC00123, 3F800000, 7F800000, 40000000]
  #_ REGISTER_IN v2 [3F800000, 7F800001, 00000000, 40400000]
  #_ REGISTER_IN v3 [FFC00456, 3F800000, 3F800000, 7F800001]
  vmaddfp v1, v1, v2, v3
  blr
  #_ REGISTER_OUT v1 [7FC00123, 7FC00001, FFC00000, 7FC00001]

test_vmaddfp_nan_lanes_dest_c:
  #_ REGISTER_IN v1 [3F800000, 3F800000, 7F800000, 40000000]
  #_ REGISTER_IN v2 [3F800000, 7F800001, 3F800000, 40400000]
  #_ REGISTER_IN v3 [FFC00456, 7FC00123, FF800000, 3F800000]
  vmaddfp v2, v1, v2, v3
  blr
  #_ REGISTER_OUT v2 [FFC00456, 7FC00001, FFC00000, 40E00000]

test_vmaddfp_nan_lanes_dest_b:
  #_ REGISTER_IN v1 [3F800000, 7FC00123, 40000000, 40000000]
  #_ REGISTER_IN v2 [7F800001, 7F800001, 40400000, 40400000]
  #_ REGISTER_IN v3 [FFC00456, 3F800000, 3F800000, 7FC00123]
  vmaddfp v3, v1, v2, v3
  blr
  #_ REGISTER_OUT v3 [7FC00001, 7FC00123, 40E00000, 7FC00123]

test_vnmsubfp_nan_lanes_dest_a:
  #_ REGISTER_IN v1 [7F800000, 7FC00123, 40000000, 40000000]
  #_ REGISTER_IN v2 [3F800000, 3F800000, 40400000, 7F800001]
  #_ REGISTER_IN v3 [7F800000, 3F800000, 3F800000, FFC00456]
  vnmsubfp v1, v1, v2, v3
  blr
  #_ REGISTER_OUT v1 [7FC00000, FFC00123, C0A00000, FFC00001]

test_vnmsubfp_nan_lanes_dest_b:
  #_ REGISTER_IN v1 [40000000, 40000000, 40000000, 7F800001]
  #_ REGISTER_IN v2 [40400000, 7FC00123, 40400000, 3F800000]
  #_ REGISTER_IN v3 [3F800000, 7F800001, FFC00456, 3F800000]
  vnmsubfp v3, v1, v2, v3
  blr
  #_ REGISTER_OUT v3 [C0A00000, FFC00123, 7FC00456, FFC00001]

test_vmaddfp_all_one_register:
  #_ REGISTER_IN v1 [40000000, 40400000, 7F800000, 7FC00123]
  vmaddfp v1, v1, v1, v1
  blr
  #_ REGISTER_OUT v1 [40C00000, 41400000, 7F800000, 7FC00123]

test_vector_chain_nan_half_way:
  #_ REGISTER_IN v1 [3F800000, 40000000, 40400000, 3F800000]
  #_ REGISTER_IN v2 [40000000, 40000000, 7F800000, 40400000]
  #_ REGISTER_IN v3 [3F800000, 3F800000, FF800000, 00000000]
  vaddfp v1, v1, v2
  vmaddfp v2, v1, v2, v3
  vsubfp v3, v2, v2
  vaddfp v1, v1, v3
  vnmsubfp v2, v2, v1, v3
  blr
  #_ REGISTER_OUT v1 [40400000, 40800000, FFC00000, 40800000]
  #_ REGISTER_OUT v2 [C1A80000, C2100000, 7FC00000, C2400000]
  #_ REGISTER_OUT v3 [00000000, 00000000, FFC00000, 00000000]
