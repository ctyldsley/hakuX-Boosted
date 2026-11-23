/* hakuX-Boosted jit bench: the workloads as one flat image at the address a
 * title's code loads at. */
ENTRY(wl_integer)
SECTIONS
{
  . = 0x82000000 + 0x1000;
  .text : { *(.text .text.*) }
  . = ALIGN(0x1000);
  __data_start = .;
  .rodata : { *(.rodata .rodata.* .rodata.cst*) }
  .data : { *(.data .data.* .sdata .sdata.* .sdata2 .sdata2.* .got .got.* .got2 .fixup) }
  __file_end = .;
  .bss : { *(.sbss .sbss.* .bss .bss.* COMMON) }
  . = ALIGN(0x1000);
  __image_end = .;
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.gnu.attributes) }
}
