bits 64
section .text
global gm_isr_de
global gm_isr_db
global gm_isr_bp
global gm_isr_of
global gm_isr_br
global gm_isr_ud
global gm_isr_nm
global gm_isr_df
global gm_isr_ts
global gm_isr_np
global gm_isr_ss
global gm_isr_gp
global gm_isr_pf
global gm_isr_mf
global gm_isr_ac
global gm_isr_xm
global gm_isr_timer
global gm_isr_service
global gm_gdt_load
global gm_program_enter
extern gm_fault_dispatch
extern gm_timer_irq
extern gm_program_tick
extern gm_program_service
extern gm_program_resume_rsp
extern gm_program_result

gm_gdt_load:
  lgdt [rdi]
  mov ax,0x10
  mov ds,ax
  mov es,ax
  mov ss,ax
  xor eax,eax
  mov fs,ax
  mov gs,ax
  push qword 0x08
  lea rax,[rel .reload]
  push rax
  retfq
.reload:
  mov ax,0x28
  ltr ax
  ret

gm_program_enter:
  pushfq
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  mov [rel gm_program_resume_rsp],rsp
  push qword 0x1b
  push rsi
  pushfq
  pop rax
  or rax,0x200
  push rax
  push qword 0x23
  push rdi
  mov rdi,rdx
  mov rsi,rcx
  iretq

gm_program_resume:
  cli
  mov rsp,[rel gm_program_resume_rsp]
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  popfq
  mov eax,[rel gm_program_result]
  ret

%macro FAULT_NOERR 2
%1:
  cld
  mov rdi,%2
  xor esi,esi
  mov rdx,[rsp+8]
  call gm_fault_dispatch
  jmp gm_program_resume
%endmacro

%macro FAULT_ERR 2
%1:
  cld
  mov rdi,%2
  mov rsi,[rsp]
  mov rdx,[rsp+16]
  call gm_fault_dispatch
  jmp gm_program_resume
%endmacro

FAULT_NOERR gm_isr_de,0
FAULT_NOERR gm_isr_db,1
FAULT_NOERR gm_isr_bp,3
FAULT_NOERR gm_isr_of,4
FAULT_NOERR gm_isr_br,5
FAULT_NOERR gm_isr_ud,6
FAULT_NOERR gm_isr_nm,7
FAULT_ERR gm_isr_df,8
FAULT_ERR gm_isr_ts,10
FAULT_ERR gm_isr_np,11
FAULT_ERR gm_isr_ss,12
FAULT_ERR gm_isr_gp,13
FAULT_ERR gm_isr_pf,14
FAULT_NOERR gm_isr_mf,16
FAULT_ERR gm_isr_ac,17
FAULT_NOERR gm_isr_xm,19

%macro PUSH_REGS 0
  push r15
  push r14
  push r13
  push r12
  push r11
  push r10
  push r9
  push r8
  push rbp
  push rdi
  push rsi
  push rdx
  push rcx
  push rbx
  push rax
%endmacro

%macro POP_REGS 0
  pop rax
  pop rbx
  pop rcx
  pop rdx
  pop rsi
  pop rdi
  pop rbp
  pop r8
  pop r9
  pop r10
  pop r11
  pop r12
  pop r13
  pop r14
  pop r15
%endmacro

gm_isr_service:
  cld
  PUSH_REGS
  mov rdi,rsp
  call gm_program_service
  test eax,eax
  jnz gm_program_resume
  POP_REGS
  iretq

gm_isr_timer:
  cld
  PUSH_REGS
  call gm_timer_irq
  mov rdi,[rsp+128]
  call gm_program_tick
  test eax,eax
  jnz gm_program_resume
  POP_REGS
  iretq
