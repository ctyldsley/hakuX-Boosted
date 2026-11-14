# hakuX-Boosted: mcrf between fields other than cr0, which upstream's
# translation gets wrong (see InstrEmit_mcrf). These pass only with
# ppc_mcrf_copies_field on and are skipped otherwise (ci/known-failures.txt).

test_mcrf_fields_cr7_to_cr3:
  #_ REGISTER_IN r3 0x4000000B
  mtcrf 0x81, r3
  mcrf cr3, cr7
  b mcrf_fields_1
mcrf_fields_1:
  li r5, 0
  ble mcrf_fields_2
  ori r5, r5, 1
mcrf_fields_2:
  bns cr3, mcrf_fields_3
  ori r5, r5, 2
mcrf_fields_3:
  bge cr3, mcrf_fields_4
  ori r5, r5, 4
mcrf_fields_4:
  mfcr r7
  blr
  #_ REGISTER_OUT r5 7
  #_ REGISTER_OUT r7 0x4000B00B

test_mcrf_fields_gt:
  #_ REGISTER_IN r3 9
  #_ REGISTER_IN r4 2
  cmpw cr5, r3, r4
  mcrf cr0, cr5
  mcrf cr2, cr0
  mfcr r7
  blr
  #_ REGISTER_OUT r7 0x40400400

test_mcrf_fields_zero_source:
  #_ REGISTER_IN r3 0x0F000000
  mtcrf 0xFF, r3
  mcrf cr1, cr6
  mfcr r7
  blr
  #_ REGISTER_OUT r7 0
