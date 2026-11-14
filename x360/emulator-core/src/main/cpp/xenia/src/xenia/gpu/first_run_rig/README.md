# First-run rig

Checks the guest shader interpreter, version 2
(`gpu/shaders/ucode_program*.glsl*`, `gpu/ucode_program.cc`) against the
emulator's own shader translator, on a desktop or a cloud machine. It is not
part of the app and the Actions build does not build it (the 360 core's CMake
takes the sources of `gpu/` itself, not of its sub-directories).

For each test shader, written by hand or at random in `rig.cc` (no game's
microcode is in the repository and none may be added), it

1. translates the microcode with `SpirvShaderTranslator`, as the app does;
2. prepares the same microcode for the interpreter (`ucode_program::Build`);
3. draws the same vertices or pixels once through each on a Vulkan device;
4. compares what the two wrote, value by value.

The translated shader is what the real pipeline draws, so a difference is a
placeholder that draws something else than the pipeline it stands in for.

What it cannot say: anything about a phone's driver (it runs on Mesa's
lavapipe, a software implementation), anything about speed, and nothing
about the code that puts the interpreter into a draw
(`vulkan/vulkan_pipeline_cache.cc`, `vulkan/vulkan_command_processor.cc`),
which only the app runs.

## Build and run

Needs clang, cmake, ninja, python3, `glslang-tools`, `spirv-tools`,
`libvulkan-dev` and `mesa-vulkan-drivers` (Ubuntu 24.04 names), and the
submodules `boosted/tools/spirv-harness/README.md` lists (the rig links the
translator objects that harness builds). From the repository's root:

```bash
x360/emulator-core/src/main/cpp/xenia/src/xenia/gpu/first_run_rig/build.sh /tmp/first-run-rig
```

```bash
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json /tmp/first-run-rig/first-run-rig --spv /tmp/first-run-rig/spv
```

The last line is `N test shaders, M differ from the translated shader`, and
the exit status is 1 if M is not 0. `--filter TEXT` runs the tests whose name
contains the text, `--verbose` prints each test's worst difference and, for a
failing one, the microcode's disassembly, `--list` the names.

The interpreter's shaders are compiled the way the app's build compiles them
(`glslangValidator`, then `spirv-opt -O -O`), so what runs here is the module
the app would load.

## What the tests are

Vertex stage (`AddVertexTests`): every vector and scalar operation on ordinary
values and on infinities, NaN and signed zeros; the constant-and-register
scalar forms; both halves of an instruction and exports that mix them with
the constants 0 and 1; every vertex format, signed, unsigned, integer, with
an exponent and across the end of the buffer; execs conditional on bool
constants and on the predicate, predicated instructions and fetches; the
predicate stack operations; jumps; the address register and constants
addressed by it; loops (count, aL, registers and constants addressed by aL, a
loop that does not run, nested loops, a break on the predicate); the point
size and the kill flag; the three ways the guest divides the position by W;
and 160 random shaders.

Not tested here: subroutine calls (the translator does not translate them, so
there is nothing to compare with), memory export (the interpreter drops it),
the tessellation evaluation and point sprite forms of the vertex side (they
are the same core with another prologue, compiled and validated but not
drawn), and user clip planes (the interpreter has none).

## Adding a test

`rig_asm.h` has the assembler: `Vec`, `Scal`, `Export`, `VertexFetch`,
`TextureFetch` encode one instruction each and `Asm` puts control flow in
front (`Exec`, `LoopStart`, `LoopEnd`, `Jump`, with labels). A vertex test is
`MakeVertexTest(name, body)`: registers 1 to 4 are loaded from the vertex
buffer before the body and registers 1 to 13 are exported after it. Set
`prepare` to change the constants a test sees.
