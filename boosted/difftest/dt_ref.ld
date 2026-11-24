/* hakuX-Boosted: layout of the reference harness (dt_ref.c). The image is
 * loaded at address 0 as the machine's firmware; 0x2000 to 0x3FFF is the
 * absolutely addressed scratch of dt_ref_start.S. */
ENTRY(_start)
SECTIONS {
  . = 0;
  .vectors : { *(.text.vectors) }
  . = 0x4000;
  .text : { *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) *(.got .got.*) *(.toc .toc.*) *(.branch_lt) }
  .bss : { *(.bss .bss.*) *(COMMON) }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
