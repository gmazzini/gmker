ENTRY(gm_main)
PHDRS
{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}
SECTIONS
{
  . = 0x0000000040000000;
  .text : ALIGN(16) {
    KEEP(*(.text.gm_main))
    *(.text*)
    *(.rodata*)
  } :text
  . = ALIGN(16);
  .data : ALIGN(16) {
    *(.data*)
    *(COMMON)
    *(.bss*)
  } :data
  /DISCARD/ : {
    *(.comment*)
    *(.note*)
    *(.eh_frame*)
  }
}
