# SPIR-V slimming (`claude/spirv-slimming`)

Cloud session, night of 2026-11-12 to 13. Base: `7648e9c` (run 44, green).
Nothing here has run on a device. No speed-up is claimed.

## Done / in progress / next

- **Done:** setting `spirv_debug_names` (step 1) and setting
  `spirv_optimize` with the SPIRV-Tools optimiser linked into the 360 core
  (step 2), written and compiled on the cloud machine (Linux x86-64, clang
  18). First push made to see whether SPIRV-Tools builds with the NDK.
- **In progress:** a harness that builds the translator on Linux and counts
  SPIR-V words on hand-written test microcode, to choose the level 1 pass
  list by measurement and for the step 3 write-up.
- **Next:** the step 3 analysis; the measuring steps for the phone; the list
  of upstream files changed.

## What is built so far

Two GPU settings (config section `[GPU]`), declared at the end of
`gpu/gpu_flags.cc`:

| Setting | Default | What |
|---|---|---|
| `spirv_debug_names` | `true` (today's output) | `false` leaves `OpName` and `OpMemberName` out of every translated module |
| `spirv_optimize` | `0` (off) | `1` a short SPIRV-Tools pass list that does not change floating-point results, `2` SPIRV-Tools' full performance list |

With both at their defaults the module handed to the driver is byte for byte
what `7648e9c` produces.

The brief suggested `spirv_debug_names` default `false`. It is `true` because
the overnight rule is that a default is today's behaviour, and because
dropping the names changes the bytes of every module: the driver's stored
pipeline cache stops matching and every pipeline is compiled again once.
Flipping the default later is one word in `gpu_flags.cc`.

(The rest of this file is filled in as the work goes.)
