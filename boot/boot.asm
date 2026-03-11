; boot/boot.asm — Knail x86-64 entry
; GRUB (multiboot2) drops us in 32-bit protected mode.
; We set up page tables, enable long mode, then call kernel_main.

global _start
extern kernel_main

section .text
bits 32

; ── Multiboot2 header ────────────────────────────────────────────────────────
align 8
mb2_header:
    dd 0xE85250D6
    dd 0
    dd (mb2_header_end - mb2_header)
    dd -(0xE85250D6 + 0 + (mb2_header_end - mb2_header))
    align 8
    dw 0
    dw 0
    dd 8
mb2_header_end:

; ── _start ───────────────────────────────────────────────────────────────────
_start:
    mov edi, eax            ; arg1: multiboot2 magic
    mov esi, ebx            ; arg2: multiboot2 info pointer
    mov esp, stack_top
    cli

    ; 1. Page tables
    call setup_page_tables

    ; 2. PAE
    mov eax, cr4
    or  eax, 1 << 5
    mov cr4, eax

    ; 3. CR3 = PML4
    mov eax, p4_table
    mov cr3, eax

    ; 4. EFER.LME
    mov ecx, 0xC0000080
    rdmsr
    or  eax, 1 << 8
    wrmsr

    ; 5. Enable paging
    mov eax, cr0
    or  eax, (1 << 31) | 1
    mov cr0, eax

    ; 6. Load GDT (32-bit pointer arithmetic — no RIP-relative)
    lgdt [gdt64_ptr]

    ; 7. Far jump to flush pipeline and enter 64-bit mode
    jmp 0x08:long_mode_start

; ── Page tables ──────────────────────────────────────────────────────────────
setup_page_tables:
    ; PML4[0] -> PDPT
    mov eax, p3_table
    or  eax, 3
    mov dword [p4_table], eax
    mov dword [p4_table + 4], 0

    ; PDPT[0] -> PDT
    mov eax, p2_table
    or  eax, 3
    mov dword [p3_table], eax
    mov dword [p3_table + 4], 0

    ; PDT: 512 x 2MiB identity-mapped huge pages
    mov ecx, 0
.loop:
    mov eax, 0x200000
    mul ecx
    or  eax, (1 << 7) | 3   ; huge | writable | present
    mov dword [p2_table + ecx * 8],     eax
    mov dword [p2_table + ecx * 8 + 4], 0
    inc ecx
    cmp ecx, 512
    jne .loop
    ret

; ── GDT (32-bit, placed in .text so bits 32 context applies) ─────────────────
align 8
gdt64:
    dq 0                                                ; null
    dq (1<<43)|(1<<44)|(1<<47)|(1<<53)                  ; 64-bit code  (selector 0x08)
    dq (1<<41)|(1<<44)|(1<<47)                          ; 64-bit data  (selector 0x10)
gdt64_end:

gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dd gdt64                ; 32-bit base address — safe here

; ── 64-bit code ──────────────────────────────────────────────────────────────
bits 64
long_mode_start:
    mov ax, 0x10            ; data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; edi=magic, esi=mb2_info — intact from _start
    call kernel_main

.hang:
    cli
    hlt
    jmp .hang

; ── BSS ──────────────────────────────────────────────────────────────────────
section .bss
align 4096
p4_table:    resb 4096
p3_table:    resb 4096
p2_table:    resb 4096

stack_bottom:
    resb 16384
stack_top:
