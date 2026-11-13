# SPIR-V slimming (`claude/spirv-slimming`)

Cloud session, night of 2026-11-12 to 13. Base: `7648e9c` (run 44, green).
Nothing here has run on a device. No speed-up is claimed: the sizes below
are of the SPIR-V module, measured on a Linux machine on test shaders, and
say nothing yet about Turnip's compile time.

## Done / in progress / next

- **Done:** step 1 (`spirv_debug_names`), step 2 (`spirv_optimize`, the
  SPIRV-Tools optimiser linked into the 360 core), a Linux harness that
  builds the translator and counts words on hand-written test microcode
  (`boosted/tools/spirv-harness`), step 3's analysis (below).
- **In progress:** waiting for a green Actions build. State of the build:
  see "What the build verified" at the end, updated last.
- **Next (for whoever picks this up):** measure on the phone (steps below);
  then the translator rewrites in "What to build next", in that order.

## The short version

- Both settings default to today's output. With them untouched the module
  handed to the driver is byte for byte what `7648e9c` produces.
- `spirv_optimize = 1` shrinks arithmetic-heavy shaders by about a third and
  texture-heavy ones by about 4%, and costs about 0.3 ms per KB of module on
  the translating thread (cloud machine; the phone will differ). A 300 KB
  shader is about 90 ms of optimiser. Run 37's largest quartile took the
  driver a median 371 ms, so level 1 only pays there if the driver gets more
  than about a quarter faster. **I expect it to pay on arithmetic-heavy
  shaders and to lose on texture-heavy ones; tomorrow's log decides.**
- The optimiser cannot remove the bulk of a texture fetch: it is real code
  that switches on run-time state. That needs a translator change (step 3,
  (b)), and by the harness it is the largest single saving available.
- A trap found on the way: SPIRV-Tools' dead code pass silently does nothing
  on the phone's kind of module (SPIR-V 1.0 with the float controls
  extension). Worked around, see "The float controls trap".

## The settings

Config section `[GPU]`. Declared at the end of `gpu/gpu_flags.cc`. There is
no entry in the app's settings screen (`x360/app` belongs to another
session): set them in the config file, as `shader_profiling` is.

| Setting | Default | What |
|---|---|---|
| `spirv_debug_names` | `true` (today's output) | `false` leaves `OpName` and `OpMemberName` out of every translated module |
| `spirv_optimize` | `0` (off) | `1`: a short SPIRV-Tools pass list that does not change floating-point results. `2`: SPIRV-Tools' full performance list (what `spirv-opt -O` runs) |

The brief suggested `spirv_debug_names` default `false`. It is `true`
because the overnight rule is that a default is today's behaviour, and
because dropping the names changes the bytes of every module: the driver's
stored pipeline cache stops matching and every pipeline compiles again once.
Flipping the default later is one word in `gpu_flags.cc`.

With `shader_profiling = true` and `spirv_optimize` above 0, each
translation logs one new line:

```
shader_profiling: pixel 0123456789ABCDEF SPIR-V optimise (level 1) 300048 B -> 203412 B 97.180 ms
```

The existing line `ucode->SPIR-V N ms B B SPIR-V` (logged by
`vulkan_pipeline_cache.cc`, not mine to edit) now includes the optimiser's
time in N and reports the size after it in B.

If the optimiser refuses a module the line ends ` FAILED, not used`, a
warning with SPIRV-Tools' message is logged, and the module is used as
translated. A build without the library logs one warning and ignores the
setting.

## Measuring on the phone

What to compare is the driver's time per pipeline against the optimiser's
time per shader, on the same shaders.

1. Install the APK of the green run of `claude/spirv-slimming`.
2. Config: `shader_profiling = true`. Keep `vulkan_async_skip_draws` and
   everything else as in the run you compare with.
3. Four runs, each from a cold cache as in run 37 (delete
   `compose/cache/shaders` and the driver's pipeline cache over adb), each
   the same ten minutes of the title:
   - A: defaults (`spirv_debug_names = true`, `spirv_optimize = 0`). This is
     the baseline and should match run 37's numbers.
   - B: `spirv_debug_names = false`.
   - C: `spirv_debug_names = false`, `spirv_optimize = 1`.
   - D: `spirv_debug_names = false`, `spirv_optimize = 2`.
4. From each `xe.log`:
   - `shader_profiling: pipeline create (real) VS ... PS ... N ms`: count,
     sum, median, p90, max. This is the driver's time.
   - `SPIR-V optimise (level L) X B -> Y B N ms` (runs C and D): sum of N is
     what the optimiser cost; X and Y per shader are the sizes.
   - any `FAILED, not used` or `SPIRV-Tools optimiser` warning.

```bash
grep -o 'pipeline create (real).*ms' xe.log | awk '{s+=$(NF-1); n++} END {print n " pipelines, " s " ms"}'
```

```bash
grep 'SPIR-V optimise' xe.log | grep -v FAILED | awk '{t+=$(NF-1); a+=$(NF-3); b+=$(NF-6); n++} END {print n " shaders, " b " B -> " a " B, " t " ms"}'
```

The verdict for a level: (sum of pipeline create in A) minus (sum in that
run), against (sum of optimise ms in that run). Both lines carry the shader
hashes, so the same pipeline can be compared across runs; gameplay differs
between runs, so compare per pipeline or per size class rather than totals
alone.

A more repeatable variant, **not checked by me**: keep xenia's own shader
storage (`shareable/*.xsh`, `*.xpso`) from a finished run and delete only
the driver's pipeline cache. The title then translates and creates the same
stored list at boot every time, with no gameplay in the measurement. The
pipeline create line is in the common creation path, so it should be logged
there too; confirm that the count matches before trusting it.

What to look at as well as the numbers:

- Level 1 must look identical to level 0. A visual difference at level 1 is
  a bug in my reasoning: please keep the log and a `dump_shaders` folder.
- Level 2 is SPIRV-Tools' own list, not audited here. Differences are
  possible in principle.
- With `async_shader_translate_on_draw` a shader can be translated on the
  draw thread. The optimiser then runs there too, so a draw-thread hitch
  that was about 2 ms becomes 2 ms plus the optimiser. Watch for it in C
  and D.
- Changing either setting changes module bytes, so each change starts from
  an empty driver cache whether or not you delete it.

## What was built

### Step 1: debug names

`OpName` and `OpMemberName` are the only debug instructions the translator
emits: the source language is set to unknown, so glslang's builder writes no
`OpSource`, and there are no strings, lines or module-processed records.
The names live in the builder's `names` list until `dump()`. With
`spirv_debug_names = false`, `CompleteTranslation` empties that list just
before `dump()` (`SpirvBuilder::ClearDebugNames`).

Who reads names back:

- Vulkan path: nobody. The pipeline layout comes from the translator's own
  binding lists (`PostTranslation`), not from reflection. No RenderDoc
  marker or object name uses them.
- `dump_shaders`: writes the binary as stored, so a dump made with names
  off has none, and `spirv-dis` shows numbered ids. Turn names on to read a
  dump.
- **Metal backend** (`gpu/metal/msl_shader.cc`): finds textures by parsing
  the variable names `xe_texture<N>_<dim>_<sign>` through SPIRV-Cross. Not
  built for Android, but the file is upstream's, so names always stay on
  Apple platforms (`#if !XE_PLATFORM_MAC` around the call).
- The built-in geometry shaders (`spirv_builtin_geometry_shader.cc`) have
  their own names and are not touched (not my file, and they are small).

Size, harness: 2.4 to 3.8 KB a module (45 member names of the system
constants block, the variables, one name per texture and sampler). On the
phone's dump names were 4.8% of words.

### Step 2: the optimiser

`gpu/spirv_shader_translator_optimize.{h,cc}` (the fork's own files) hold
`OptimizeTranslatedSpirv(module, level, spirv_version)`.
`CompleteTranslation` calls it between `dump()` and the copy into the
returned bytes, so it runs on whichever thread translates and before
anything stores the module. Each call makes its own `spvtools::Optimizer`;
nothing is shared between threads. The optimised words replace the original
only on success and the original is freed before the call returns, so two
copies exist only for the length of the call. (SPIRV-Tools' own working
memory during the call is larger than either: tens of MB for a 300 KB
module at level 2.)

**Was the library built for Android?** No. `third_party/CMakeLists.txt`
already ran `add_subdirectory(SPIRV-Tools EXCLUDE_FROM_ALL)` for every
platform (for the host shader tool), so the targets existed in the Android
configuration, but nothing linked them, so nothing compiled them.
`gpu/CMakeLists.txt` now links `SPIRV-Tools-opt` into `xenia-gpu` when the
target exists and defines `XE_GPU_SPIRV_TOOLS_OPT=1`; without it the
function compiles to a stub. The first build compiles about 200 more
source files (ccache holds them afterwards) and `libe.so` grows by the
optimiser's size.

Options used at both levels: validator off (the module is our own, and
validating costs about as much as the passes), `preserve_bindings` on,
`preserve_spec_constants` on, dead code elimination told to keep every
entry point interface variable.

**Level 1, in order, and why each is there:**

| Pass | Why |
|---|---|
| Scalar replacement (limit 100) | The guest's registers are one function-local array of `vec4`. This splits it into one variable per register so the next passes can see each. Only possible when no instruction indexes registers with `a0` or `aL`; otherwise the array stays as it is. 100 is the pass's default and above the 64 registers a shader can have |
| Local access chain conversion | The remaining constant-index access chains into locals (the loop stacks) become loads, inserts and extracts of the whole variable |
| SSA rewrite | Locals that are only loaded and stored become SSA values with `OpPhi`. This removes nearly all `OpAccessChain`, `OpLoad` and `OpStore` on registers, the work the driver otherwise redoes |
| Conditional constant propagation | Constants through the new SSA values and across branches. In practice integers and booleans (see below) |
| Dead branch elimination | Branches on what is now constant, and the blocks only they reached |
| Aggressive dead code elimination | Everything unused, including variables, types and constants that are not bindings or interface |
| Block merge | A block with one predecessor whose predecessor has one successor is merged into it |
| Local redundancy elimination | Within a block, an instruction that repeats an earlier one on the same operands reuses its result (the absolute value taken once per multiply, the same component extracted again, a system constant loaded again) |

Left out of level 1 on purpose: function-wide redundancy elimination (finds
little more in arithmetic, and on a 44-fetch pixel shader took 110 ms of a
210 ms run), instruction simplification (2% more, and it is the pass whose
folding rules I would have to audit one by one), if-conversion, loop
unrolling, vector dead code elimination. All of those are in level 2.

**Why level 1 does not change a floating-point result.** Six of the eight
passes only rewrite memory traffic (loads, stores, access chains become SSA
values and `OpPhi`) or move, merge and delete instructions: an arithmetic
instruction that survives keeps its id, the values of its operands and its
decorations. Redundancy elimination replaces an instruction only by an
identical one (same opcode, operands and decorations), which gives the same
bits. The only pass that evaluates anything is constant propagation, and
SPIRV-Tools' folding goes through `Instruction::IsFloatingPointFoldingAllowed`,
which refuses (a) every floating-point instruction of a module that
declares `DenormFlushToZero`, `SignedZeroInfNanPreserve` or
`RoundingModeRTE`, which the translator declares whenever the device has
them (Turnip does), and (b) any instruction decorated `NoContraction`,
which the translator puts on all guest arithmetic. I read that function and
checked which folding rules call it; I did not audit every one of
SPIRV-Tools' 83 folding rules, which is why simplification is not in level
1. One caveat: a register read before anything wrote it used to be a load
of uninitialised memory and becomes `OpUndef`. Both are undefined in
SPIR-V and Mesa turns the first into the second itself, so I expect no
difference, but it is the one place where the module's meaning is "the
same undefined" rather than "the same".

**What is preserved, and how it was checked.** For every test shader at
both levels the harness compares the optimised module with the unoptimised
one: capabilities, extensions, entry points with their interface lists,
execution modes (float controls, depth, early fragment tests), every
(set, binding) pair, every `Location` and `BuiltIn`, the number of texture
sample instructions, and that every `OpFAdd`, `OpFSub`, `OpFMul`, `OpFDiv`
and `OpFNegate` still has `NoContraction`. All identical, also with float
controls off and at SPIR-V 1.5. All 88 modules it writes pass `spirv-val`.
The translator emits no specialization constants (those are in the render
target cache's own shaders), so that option is a belt only.

An unused binding cannot be removed: `preserve_bindings` forbids it, so the
question of whether the layout code would tolerate it does not arise.

### The float controls trap

SPIRV-Tools' aggressive dead code elimination, local access chain
conversion and two other local-variable passes each carry a list of
extensions they know, and **return without doing anything** if the module
uses one that is not on it. `SPV_KHR_float_controls` is on none of the
lists. The translator declares that extension for SPIR-V below 1.4 whenever
the device has a float controls feature: the app's default
(`spirv_version_override = "1.0"`) on Turnip. First harness run: the
optimiser removed 3% of a module it shrinks by a third with the extension
absent.

The fix (`TakeFloatControlsExtension` / `RestoreExtension`): the
`OpExtension` instruction is taken out of the module for the optimiser and
put back afterwards. The extension only permits the float controls
execution modes and capabilities, which those passes do not look at. The
capabilities and execution modes stay in throughout, and the capabilities
are what `IsFloatingPointFoldingAllowed` checks, so the folding guard
above is unaffected. The harness confirms the extension, capabilities and
modes are all present afterwards.

Any other route that runs `spirv-opt` on these modules (a desktop
experiment on a `dump_shaders` folder, for instance) will hit the same
trap: use `--spirv15` modules or strip the extension first.

## The harness and what it measured

`boosted/tools/spirv-harness` (README there): builds the translator, the
few `base` files it needs, glslang's builder and SPIRV-Tools with clang on
Linux, assembles test microcode in memory with a small assembler
(`Alu`, `AluScalar`, `TextureFetch`, `VertexFetch`, `Assemble`; bit layouts
from `gpu/ucode.h`), translates each shader at four configurations and
counts words by opcode. The translator is told what Turnip on a recent
Adreno reports (all features, SPIR-V 1.0). The test shaders are mine: no
game's microcode was used or added.

Nothing blocked it. The sources are portable; the stubs are a logging
sink, three cvars defined in files it does not build, and a few symbols
(`stubs.cc`).

Sizes in bytes; times on the cloud machine (4 shared x86-64 cores, they
vary by a third between runs).

| Test shader | guest instr | today | no names | level 1 | level 2 | L1 ms | L2 ms |
|---|---|---|---|---|---|---|---|
| `ps_minimal` | 1 | 10,572 | 8,168 | 7,016 | 6,680 | 2 | 4 |
| `ps_tex2d_1` | 4 | 16,764 | 14,148 | 12,704 | 12,400 | 3 | 7 |
| `ps_tex2d_16` | 34 | 92,608 | 88,768 | 84,744 | 82,892 | 20 | 88 |
| `ps_tex2d_44` (shared coordinates) | 90 | 227,120 | 223,280 | 214,440 | 182,524 | 68 | 363 |
| `ps_tex2d_chained_44` | 90 | 227,120 | 223,280 | 214,212 | 187,040 | 68 | 398 |
| `ps_tex3d_4` | 10 | 40,888 | 37,808 | 35,848 | 35,036 | 8 | 24 |
| `ps_texcube_4` | 10 | 33,920 | 31,064 | 29,104 | 27,992 | 7 | 18 |
| `ps_alu_64` | 80 | 44,232 | 41,696 | 29,796 | 25,656 | 8 | 17 |
| `ps_alu_256` | 272 | 136,776 | 134,240 | 91,924 | 57,056 | 33 | 74 |
| `ps_alu_600` | 616 | 302,584 | 300,048 | 203,412 | 105,020 | 97 | 222 |
| `ps_alu_predicated_256` | 272 | 129,592 | 127,056 | 101,572 | 96,620 | 35 | 77 |
| `vs_minimal` | 2 | 8,456 | 6,176 | 5,256 | 5,164 | 1 | 3 |
| `vs_alu_256` | 275 | 136,728 | 134,292 | 92,336 | 56,736 | 33 | 66 |
| `vs_alu_800` | 819 | 398,936 | 396,500 | 268,604 | 130,220 | 143 | 268 |

How these compare with the phone: the 44-fetch test shader is 227 KB
against 282 KB for the title's 44-fetch, 636-line pixel shader; the
819-instruction vertex test is 399 KB against 281 KB for the title's
822-line one. Same order, so the per-instruction figures below are usable;
the title's real mix differs. (The brief's "about 340 words per guest
instruction" for that vertex shader is 342 bytes, about 85 words. The test
mix is 120 words, 480 bytes, an instruction.)

**Caveat on the level 2 column for the `alu` shaders.** Level 2 removes
unused vector components, and my arithmetic mix leaves some components
unread (a register written `xyzw` whose reader takes `xyz`). A compiled
game shader has almost none, so level 2's extra gain over level 1 there is
flattered. Level 1's is not: it only removes whole unused instructions and
every instruction's result is read.

## Step 3: where the bulk comes from

Shares are of words. "Test" figures are from the harness; "phone" figures
are run 37's histogram of 169 dumped modules. What a module is made of,
`no names`, in words:

| | declarations | prologue | body | tail |
|---|---|---|---|---|
| `ps_minimal` | 1,013 | 177 | 47 | 804 |
| `ps_tex2d_44` | 7,780 | 258 | 46,977 | 804 |
| `ps_alu_600` | 5,179 | 306 | 68,722 | 804 |
| `vs_minimal` | 818 | 249 | 333 | 143 |
| `vs_alu_800` | 6,345 | 319 | 92,317 | 143 |

Declarations grow with the body because every `NoContraction` is an
`OpDecorate` up there. The fixed part is about 1,000 words: types, and the
system constants block with its 47 member offsets.

One arithmetic instruction of the test mix is 120 words:

| Part | Words | Share | Removed by level 1 |
|---|---|---|---|
| Multiply's zero rule: `FAbs`, `FAbs`, `NMin`, compare, select | 40 | 34% | 4 (shared `FAbs`) |
| Registers in memory: `OpAccessChain`, `OpLoad`, `OpStore` | 30 | 25% | 29 |
| Swizzle and write mask: extract, shuffle, insert | 24 | 20% | 5 |
| The arithmetic itself: `FMul`, `FAdd` | 11 | 9% | 0 |
| `NoContraction` decorations | 7 | 6% | 0 |
| Rest (scalar ops' bit tests, constants) | 8 | 6% | 1 |

After level 1 an instruction is 81 words.

### (a) Registers in function-local memory

- **Today:** `xe_var_registers` is one local `float4[register_count]`.
  Every operand is `OpAccessChain` + `OpLoad` (`LoadOperandStorage`), every
  result `OpAccessChain` + `OpStore`, and a partial write mask adds a load
  of the old value (`StoreResult`). Test: 25% of an arithmetic instruction.
  Phone: `OpAccessChain` 9.7% + `OpLoad` 7.7% + stores, so 18 to 20% of all
  words (texture-heavy shaders dilute it).
- **What level 1 already does:** removes it (30 words to 1). Scalar
  replacement is its most expensive pass (30 to 40 ms of 140 on
  `vs_alu_800`), and it is there only to undo the array.
- **What a rewrite would emit, cheapest first:**
  1. One `vec4` variable per register instead of an array, when the shader
     has no `a0`/`aL` relative register addressing
     (`dynamic_addressable_register_count` is 0 and no operand uses a
     relative mode). Removes every `OpAccessChain` on registers at once
     (15 words an instruction, 13%) with a change of a few lines in
     `LoadOperandStorage`, `StoreResult` and the variable's creation, and
     makes scalar replacement unnecessary.
  2. On top of that, a per-block cache in the translator: the current
     `spv::Id` of each register within the block being built, loaded at
     first use, stored once when the block ends or control flow closes
     (`CloseInstructionPredication`, `CloseExecConditionals`,
     `ProcessLabel`, loop and jump instructions). Removes most loads and
     stores with no `OpPhi` to build. This is what makes level 1 pointless
     for arithmetic.
  3. Full SSA with `OpPhi` in the translator. Not worth writing: the
     driver's own variable-to-SSA pass is cheap once the array is gone.
- **What could go wrong:** relative addressing must keep the array (leave
  that path exactly as it is). Every place that ends a block has to flush
  the cache, including the ones that are easy to miss: `exece` breaking out
  of the main loop, `KillPixel`, memexport, the rectangle-list path that
  runs the guest shader three times in a loop
  (`ResetUcodeInvocationStateInMain`), and `WriteVertexIndexToRegister0`. A
  missed flush is a wrong register value in some shader, silently. The
  harness's preservation check would not catch it; a per-shader comparison
  of rendered output would.

### (b) The texture fetch

One `tfetch2D` whose four components are all used is about 1,080 words
(1,200 with the `mad` that uses it); 3D or stacked about 1,660; cube about
1,250. Level 1 removes 4%. What it is made of (test, 2D, all four
components):

| Part | Words | What |
|---|---|---|
| Sign, bias and gamma, per component | about 660 | Four `OpSwitch`es, one per result component, on that component's 2-bit mode from the system constant `texture_swizzled_signs`: take the signed sample, or `x * 2 - 1`, or the piecewise gamma-to-linear curve (clamp, 3 compares, 6 selects, 4 multiplies, 2 adds, a truncate), each written out as scalars |
| Coordinates, size, gradients, LOD | about 250 | Texture size from the fetch constant for the half-pixel offset, `dFdx`/`dFdy` scaled by the LOD bias, the exponent bias |
| Two samples | about 60 | The unsigned and the signed view of the texture are separate images; each is sampled under its own branch, on "any component unsigned" and "any component signed" |
| Exponent bias and store | about 100 | `ldexp` and four multiplies, the composite, the register store |

So the phone's count (88 sample instructions for 44 fetches) is this
design, not a bug: every fetch has both samples in the module and runs one
of them. Its 60 switches over 44 fetches say the title's shaders use fewer
components per fetch than my test's four (the switch is emitted per used
component), so its fetches are smaller than 1,080 words each. Estimating
420 words of fixed part and 165 per switched component, that shader's
fetches are about 28,000 of its 70,500 words, 40%.

- **What a rewrite would emit:** the sign, bias and gamma step on the whole
  `vec4` at once: both candidate values computed as vectors, the gamma
  curve once on a vector instead of four scalar copies (`PWLGammaToLinear`
  takes vectors already, as the pixel shader's tail uses it, but asserts
  fewer than four components: lift that, or run it on three and one), then
  three vector `OpSelect`s on masks built from the mode word. About 150 words instead of 660 for four components, and no
  control flow. It computes the same value per lane as the switch does, by
  the same operations in the same order, so results are identical as long
  as each lane's arithmetic is kept as it is now.
- **How much:** on the test, about 510 of 1,080 words a fetch, 45% of a
  texture-heavy pixel shader. On the title's largest pixel shader, by the
  estimate above, about 60 x 115 = 7,000 words, 10%. More if several
  fetches read the same texture (the fixed 420 words are per fetch today
  and partly shareable).
- **What could go wrong:** the GPU now evaluates the gamma curve for every
  lane of every fetch instead of branching around it. That is more
  arithmetic per pixel and no branches; on a mobile GPU I would expect it
  to be neutral or better, but it is a run-time cost that must be measured
  (frame time, not compile time). A lane the old code never computed can
  now hold Inf or NaN before the select discards it, which is harmless
  unless a driver "optimises" on the assumption there are none; the float
  controls modes forbid that. The stacked (3D) path and
  `kGetTextureWeights` share this code and need the same care.
- **Not recommended:** specialising on the bound texture's signs through a
  shader modification bit. The signs are run-time state of the texture, so
  each combination would be another pipeline to compile, which is the
  problem being solved.

### (c) Swizzle and write-mask code, and the multiply rule

- **Swizzle and mask today:** an operand swizzle is one `OpVectorShuffle`
  (or one `OpCompositeExtract` for a scalar), already minimal. A partial
  write is a load of the old value, a shuffle with the new and a store,
  also minimal once (a) removes the load. The expensive forms are the dot
  products, which extract every component of both operands to apply the
  zero rule per component, and scalar results smeared across a mask with
  one `OpCompositeInsert` per component. Test: 20% of an arithmetic
  instruction; phone: `OpVectorShuffle` 6.7% plus extracts and inserts.
- **What a rewrite would emit:** for `dp3`/`dp4`, one vector multiply and
  one vector zero test, then the extracts and adds, instead of per
  component scalars (halves the extracts). For a smeared scalar, one
  `OpCompositeConstruct` and one shuffle. Saving: about 5% of an
  arithmetic instruction. Low priority.
- **The part the brief did not list, and the largest:** the Shader Model 3
  rule that zero times anything is zero (`ZeroIfAnyOperandIsZero`): for
  every multiply whose operands differ, `FAbs`, `FAbs`, `NMin`, a compare
  with zero and an `OpSelect`. 34% of an arithmetic instruction on the
  test; on the phone it is most of the `OpExtInst` 9.7% and `OpSelect`
  6.5% outside texture fetches. It cannot be dropped without changing
  results (0 x Inf). Level 1 shares the repeated `FAbs`. According to the
  `spirv_multiply_zero_test_on_bits` setting's own description Mesa folds
  the pattern into one operation, so it may cost the driver less than its
  size suggests. I would leave it alone.
- **`NoContraction`:** 6% on the test, 7.0% on the phone: one 3-word
  `OpDecorate` per arithmetic result. `SPV_KHR_float_controls2` can state
  it once per module as a default fast-math mode. Whether Turnip v39
  exposes `VK_KHR_shader_float_controls2` I could not check from here: one
  line of the device's extension list answers it.
- **What could go wrong:** dot product operation order. The adds must stay
  in today's order with `NoContraction`, or multipass rendering that
  relies on identical results between shaders breaks (z-fighting between
  passes).

### (d) The pixel shader's tail

Fixed at about 800 words (3.2 KB) per pixel shader, 900 with float24
depth conversion; a vertex shader's is 143.

| Part | Words | Why it is there |
|---|---|---|
| Alpha test | about 170 | The compare function is a run-time system constant, so all of less, equal, greater and the never/always cases are emitted |
| Alpha to coverage | about 330 | Run-time on the `alpha_to_mask` constant, with a branch per MSAA sample count (1, 2, 4) and the dither pattern for each |
| Colour written check, exponent bias, gamma on output, blend factor premultiply, the store | about 300 | Per render target; the gamma conversion is a run-time flag (already in vector form) |

- **How much:** nothing on the large shaders (0.4% of `ps_alu_600`). On a
  median module (run 37: 39 KB for a vertex and pixel pair, about 10,000
  words) the tail is 8% and the fixed declarations of the two modules
  another 20%. So this is about the many small pipelines, where the driver
  already takes 5 ms.
- **What a rewrite would emit:** nothing safe that I can see. Removing the
  alpha test or alpha to coverage from the module means knowing at
  translation time that they are off, which means a modification bit,
  which means more pipelines per shader. The system constants block cannot
  shrink either (its members' offsets are fixed and its binding must
  stay). The only free saving is the unused types and constants, which
  level 1 already drops (`ps_minimal`: 8,168 to 7,016 bytes).
- **What could go wrong:** more pipeline variants, and so more compiles,
  which is worse than a larger tail.

## What to build next, in order

1. **Measure this branch** (the steps above). If level 1 pays on
   arithmetic-heavy shaders only, a size or content threshold in
   `CompleteTranslation` (skip when the shader is mostly texture fetches)
   is a few lines.
2. **(b), the vectorised sign, bias and gamma step**, behind a setting.
   The largest saving on texture-heavy pixel shaders and the only one
   that touches them at all. Check with the harness (words) and on the
   phone (compile time and frame time).
3. **(a) step 1 and 2, registers as separate variables with a per-block
   cache**, behind a setting. Gives arithmetic most of what level 1 gives
   without the optimiser's time.
4. `SPV_KHR_float_controls2` for `NoContraction`, if the driver has it.
5. Leave the tail and the multiply rule alone.

Running the harness on a real `dump_shaders` folder (kept outside the
repository) would replace my estimates with the title's own numbers:
`spirv-opt` with level 1's passes on each module, remembering the float
controls trap above.

## Upstream files changed, and why

Paths under `x360/emulator-core/src/main/cpp/xenia/src/xenia/`. Each
change is marked `hakuX-Boosted:`. For `docs/ARCHITECTURE.md` at merge.

| File | Change |
|---|---|
| `gpu/gpu_flags.cc`, `gpu/gpu_flags.h` | The two settings, one block at the end of each file |
| `gpu/spirv_builder.h` | `ClearDebugNames()`, three lines: the builder's `names` list is protected |
| `gpu/spirv_shader_translator.cc` | Two includes; in `CompleteTranslation`, the names switch before `dump()` and the optimiser call with its `shader_profiling` line after it |
| `gpu/CMakeLists.txt` | Links `SPIRV-Tools-opt` into `xenia-gpu` when the target exists, its headers as SYSTEM, defines `XE_GPU_SPIRV_TOOLS_OPT=1` |

New files, the fork's own:

| File | What |
|---|---|
| `gpu/spirv_shader_translator_optimize.h`, `.cc` | The optimiser call, the two pass lists, the float controls workaround |
| `boosted/tools/spirv-harness/` (`harness.cc`, `stubs.cc`, `build.sh`, `README.md`) | The Linux harness. Not built by Actions |
| `boosted/notes/spirv-slimming.md` | This file |

Rows for `boosted/HANDOFF.md`, "Settings this fork added":

| Setting | Section | Default | What |
|---|---|---|---|
| `spirv_debug_names` | GPU | true | false leaves `OpName`/`OpMemberName` out of translated shaders (changes module bytes: the driver's cache refills once) |
| `spirv_optimize` | GPU | 0 | 1 a float-exact SPIRV-Tools pass list over each translated shader, 2 its full performance list; `shader_profiling` logs sizes and the optimiser's time (built, not run) |

Nothing outside my row of the ownership table was edited. One thing in
another session's file would help and is left for the merge: the
`ucode->SPIR-V` line in `vulkan_pipeline_cache.cc` could say that its time
includes the optimiser.

## What the build verified

(Updated last; see the top of the file for the state when you read this.)
