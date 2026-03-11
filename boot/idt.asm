; boot/idt.asm - IDT flush + exception stubs

global idt_flush
global isr_stub_table

extern exception_handler

section .text
bits 64

idt_flush:
    lidt [rdi]
    ret

isr_common:
    ; Stack on entry: [rsp+0]=vector [rsp+8]=err [rsp+16]=rip [rsp+24]=cs
    test qword [rsp+24], 3
    jz   .isr_no_swapgs_entry
    swapgs
.isr_no_swapgs_entry:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    call exception_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ; [rsp+0]=vector [rsp+8]=err [rsp+16]=rip [rsp+24]=cs
    test qword [rsp+24], 3
    jz   .isr_no_swapgs_exit
    swapgs
.isr_no_swapgs_exit:
    add rsp, 16
    iretq

%macro ISR_NOERR 1
isr_%1:
    push 0
    mov  rax, %1
    push rax
    jmp  isr_common
%endmacro

%macro ISR_ERR 1
isr_%1:
    mov  rax, %1
    push rax
    jmp  isr_common
%endmacro

ISR_NOERR  0
ISR_NOERR  1
ISR_NOERR  2
ISR_NOERR  3
ISR_NOERR  4
ISR_NOERR  5
ISR_NOERR  6
ISR_NOERR  7
ISR_ERR    8
ISR_NOERR  9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

isr_stub_table:
%assign i 0
%rep 32
    dq isr_%+i
%assign i i+1
%endrep

; ── IRQ stubs (vectors 32–47) ─────────────────────────────────────────────
; IRQs don't push error codes, and we dispatch via a C handler table
; rather than individual C++ functions, so we just push the vector and jump.

global irq_stub_table
extern irq_dispatch

%macro IRQ 1
irq_%1:
    push 0
    mov  rax, (32 + %1)
    push rax
    jmp  irq_common
%endmacro

irq_common:
    ; Check if we interrupted user mode (CPL=3) by inspecting saved CS.
    ; At irq_common entry the stack has: [rsp]=vector [rsp+8]=err [rsp+16]=rip [rsp+24]=cs
    test qword [rsp+24], 3   ; CS & 3 == 3 means user mode
    jz   .no_swapgs_entry
    swapgs
.no_swapgs_entry:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    call irq_dispatch

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    ; Stack now: [rsp+0]=vector [rsp+8]=err [rsp+16]=rip [rsp+24]=cs
    ; Check CS to see if returning to user mode
    test qword [rsp+24], 3
    jz   .no_swapgs_exit
    swapgs
.no_swapgs_exit:
    add rsp, 16              ; skip vector + err
    iretq

IRQ  0   ; timer
IRQ  1   ; keyboard
IRQ  2   ; cascade
IRQ  3
IRQ  4
IRQ  5
IRQ  6
IRQ  7
IRQ  8   ; RTC
IRQ  9
IRQ 10
IRQ 11
IRQ 12
IRQ 13
IRQ 14
IRQ 15

irq_stub_table:
%assign i 0
%rep 16
    dq irq_%+i
%assign i i+1
%endrep
