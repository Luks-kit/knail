; boot/gdt.asm - GDT + TSS flush routines (64-bit)
; Called from C++ after building the GDT table in memory.

global gdt_flush
global tss_flush

section .text
bits 64

; void gdt_flush(Pointer* gdtr)   [rdi = pointer to GDTR struct]
gdt_flush:
    lgdt [rdi]              ; load new GDT

    ; Reload code segment via a far return
    ; Push new CS and the return address, then retfq
    push 0x08               ; KERNEL_CODE selector
    lea  rax, [rel .reload]
    push rax
    retfq                   ; far return: pops RIP then CS

.reload:
    ; Reload all data segments with kernel data selector
    mov ax, 0x10            ; KERNEL_DATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    ret

; void tss_flush(uint16_t selector)   [di = TSS selector]
tss_flush:
    ltr di                  ; load task register
    ret
