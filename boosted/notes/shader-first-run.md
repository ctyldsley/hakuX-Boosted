# Shader first run (branch `claude/shader-first-run`)

Base: `claude/integration-1113` at `8e03766`. **Nothing here has been run on
a device.** Work in progress: this file is rewritten as each step lands.

## Status

Done:
- The guest shader interpreter, version 2, as shaders and a program builder
  (`gpu/shaders/ucode_program*.glsl*`, `gpu/ucode_program.{h,cc}`), not yet
  used by any draw.
- A rig that checks it against the translator on a software Vulkan device
  (`gpu/first_run_rig/`, README there): 367 vertex test shaders, none
  differs.
- A translator bug the rig found, fixed: `cube` written to `.x` or `.w`
  without `.y` produced a module with a missing id
  (`spirv_shader_translator_alu.cc`).

In progress: putting the interpreter into the draw path behind a setting.

Next: the frame-time split log, the pixel side of the interpreter, less
compile work, the kept record. See the task list in
`boosted/notes/OVERNIGHT-2.md`.
