bits 64
section .text
global _start
extern kernel_main
_start:
  call kernel_main
.halt:
  cli
  hlt
  jmp .halt
