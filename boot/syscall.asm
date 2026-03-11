extern syscall_handler
extern preempt_disable
extern preempt_enable
extern syscall_consume_yield_pending
extern syscall_do_yield
extern syscall_consume_exec_pending
extern syscall_last_user_rip
extern syscall_last_user_rsp
extern syscall_last_user_rflags
extern syscall_exec_rip
extern syscall_exec_rsp
extern syscall_exec_rflags

global syscall_entry

syscall_entry:
    swapgs
    mov  [gs:8], rsp          ; save user RSP
    mov  rsp,  [gs:0]         ; switch to kernel syscall stack

    push rcx                  ; user RIP
    push r11                  ; user RFLAGS
    push rbp

    ; Save current user return frame for fork().
    push rax
    mov  [rel syscall_last_user_rip], rcx
    mov  [rel syscall_last_user_rflags], r11
    mov  rax, [gs:8]
    mov  [rel syscall_last_user_rsp], rax
    pop  rax

    ; ── Shift args into SysV calling convention ───────────────────────────
    ; User:    rax=nr, rdi=a0, rsi=a1, rdx=a2, r10=a3, r8=a4
    ; Handler: rdi=nr, rsi=a0, rdx=a1, rcx=a2, r8=a3,  r9=a4

    mov r9,  r8   ; user a4 (r8)  -> handler 6th (r9)
    mov r8,  r10  ; user a3 (r10) -> handler 5th (r8)
    mov rcx, rdx  ; user a2 (rdx) -> handler 4th (rcx)
    mov rdx, rsi  ; user a1 (rsi) -> handler 3rd (rdx)
    mov rsi, rdi  ; user a0 (rdi) -> handler 2nd (rsi)
    mov rdi, rax  ; user nr (rax) -> handler 1st (rdi)    

    ; Save args across preempt_disable (it's a C function, clobbers regs)
    push rdi
    push rsi
    push rdx
    push rcx
    push r8
    push r9
    call preempt_disable
    pop  r9
    pop  r8
    pop  rcx
    pop  rdx
    pop  rsi
    pop  rdi

    call syscall_handler      ; returns int64_t in rax

    push rax
    call preempt_enable
    pop  rax                  ; rax = syscall return value, preserved across sysret

    ; ── Unwind user state ─────────────────────────────────────────────────
    pop  rbp
    pop  r11                  ; user RFLAGS
    pop  rcx                  ; user RIP
    
    
    ; If execve replaced the process image, override the return target.
    push rax                  ; save rax first (don't forget this)

    call syscall_consume_exec_pending
    test rax, rax
    pop rax                   ; now you pop
    jz .no_exec
    mov rcx, [rel syscall_exec_rip]
    mov r11, [rel syscall_exec_rflags]
    mov rdx, [rel syscall_exec_rsp]
    mov [gs:8], rdx
.no_exec:

    ; ── Check yield-pending AFTER full unwind, BEFORE sysret ─────────────
    ; We are still on the kernel syscall stack with interrupts masked (FMASK).
    ; Safe to check and act on the flag here without any stack corruption risk.
    push rax                  ; preserve syscall return value
    push rcx                  ; preserve user RIP
    push r11                  ; preserve user RFLAGS
    call syscall_consume_yield_pending
    test rax, rax
    pop  r11
    pop  rcx
    pop  rax
    jz   .no_yield

    ; Yield requested: switch back to kernel stack (we restored rsp to gs:8
    ; conceptually, but haven't yet — we're still on kernel stack here).
    ; Just call do_yield directly; we never touched user RSP yet.
    push rax
    push rcx
    push r11
    call syscall_do_yield     ; parks this task, returns when rescheduled
    pop  r11
    pop  rcx
    pop  rax

.no_yield:
    mov  rsp, [gs:8]          ; restore user RSP
    swapgs
    o64 sysret



