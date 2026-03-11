section .data
    msg db "Hello, knail!", 0x0a   ; the string to print, with a newline character (0x0a)
    len equ $ - msg                ; length of the string

section .text
    global _start                  ; makes the _start symbol visible to the linker

_start:
    ; syscall for write (sys_write)
    mov rax, 1                     ; syscall number for 'write' is 1
    mov rdi, 1                     ; file descriptor 1 is stdout (standard output)
    mov rsi, msg                   ; address of the string to write
    mov rdx, len                   ; number of bytes to write
    syscall                        ; call the kernel

    ; syscall for exit (sys_exit)
    mov rax, 60                    ; syscall number for 'exit' is 60
    xor rdi, rdi                   ; exit code 0 (successful termination)
    syscall                        ; call the kernel

