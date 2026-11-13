# SPIR-V harness

Builds the Xbox 360 side's shader translator (`gpu/spirv_shader_translator*`)
on a Linux desktop, translates Xenos microcode that `harness.cc` assembles
itself, and counts the SPIR-V words that come out, by opcode, with and
without the settings `spirv_debug_names` and `spirv_optimize`. It is how the
numbers in `boosted/notes/spirv-slimming.md` were made. It is not part of the
app and the Actions build does not build it.

The test shaders are written by hand in `harness.cc` (texture fetches,
swizzled and masked arithmetic, predicated blocks, vertex fetches). No game's
microcode is in the repository and none may be added: to look at a game's
shaders, point the harness's scripts at a `dump_shaders` folder kept outside
the repository.

It measures size and composition only. How long a driver takes to compile a
module is measured on the phone.

## Build and run

Needs clang, cmake, ninja and python3. From the repository's root:

```bash
T=x360/emulator-core/src/main/cpp/xenia/third_party
git submodule update --init --depth 1 -- $T/SPIRV-Tools $T/SPIRV-Headers $T/glslang $T/fmt $T/Vulkan-Headers $T/tomlplusplus $T/cxxopts $T/utfcpp $T/xxhash $T/VulkanMemoryAllocator $T/date
```

```bash
boosted/tools/spirv-harness/build.sh /tmp/spirv-harness
```

```bash
/tmp/spirv-harness/spirv-harness --histogram --out /tmp/spirv-harness/out
```

The first build compiles SPIRV-Tools (about eight minutes on four cores).
`build.sh` only recompiles sources that changed; after changing a header run
it with `REBUILD=1`.

Each row is one test shader: its instruction count, the module's size in
bytes as translated today, without names, at `spirv_optimize` 1 and 2, and
the optimiser's own time on this machine. `--histogram` adds the words by
opcode. `--out DIR` writes every module (`<shader>.<config>.spv`) and the
disassembly of the microcode that was assembled (`<shader>.ucode.txt`, to
check the test shader is what was meant). The last line says whether every
optimised module still has the bindings, interface variables, execution
modes, capabilities and `NoContraction` decorations of the unoptimised one;
the exit status is 1 if not.

Other switches: `--no-float-controls` (a device without the float controls
features), `--spirv15`, `--depth-stencil-mode N` (the pixel shader's
`DepthStencilMode`), `--no-precise-interpolation`.

To validate what it wrote:

```bash
for f in /tmp/spirv-harness/out/*.spv; do /tmp/spirv-harness/spirv-tools/tools/spirv-val --target-env vulkan1.0 "$f" || echo "INVALID $f"; done
```

## Adding a test shader

`Alu`, `AluScalar`, `TextureFetch` and `VertexFetch` in `harness.cc` encode
one instruction each (bit layouts from `gpu/ucode.h`); `Assemble` puts the
control flow in front. Add a function that returns a `TestShader` and push it
in `main`. A series of the same shape at several lengths gives the words per
instruction by subtraction.

Keep every result live (read by a later instruction or by the output):
compiled game shaders have no dead code, and a test shader that has some
makes dead code elimination look better than it is.
