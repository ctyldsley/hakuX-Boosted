/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2014 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_BACKEND_MACHINE_INFO_H_
#define XENIA_CPU_BACKEND_MACHINE_INFO_H_

#include <cstdint>

namespace xe {
namespace cpu {
namespace backend {

struct MachineInfo {
  bool supports_extended_load_store;
  // hakuX-Boosted: OPCODE_VECTOR_DENORMFLUSH is a plain copy on this machine
  // (its flush-to-zero mode covers inputs), so the register allocator may
  // give its result the operand's register (register_allocation_elide_copies).
  bool denormflush_is_copy = false;
  // hakuX-Boosted: a64_shared_guest_registers. Guest general registers
  // (their numbers) that every translated function keeps in the same host
  // register: entry n in register n of the integer set. The pinning pass
  // adds them to every function's pinned slots and leaves them out of its
  // own choice.
  uint8_t shared_gpr_count = 0;
  uint8_t shared_gprs[8] = {};

  struct RegisterSet {
    enum Types {
      INT_TYPES = (1 << 1),
      FLOAT_TYPES = (1 << 2),
      VEC_TYPES = (1 << 3),
    };
    uint8_t id;
    char name[4];
    uint32_t types;
    uint32_t count;
  } register_sets[8];
};

}  // namespace backend
}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_BACKEND_MACHINE_INFO_H_
