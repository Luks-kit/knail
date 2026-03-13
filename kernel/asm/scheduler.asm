; boot/scheduler.asm - Context switch for Knail scheduler
;
; switch_context(Context** old_ctx, Context** new_ctx)
;   rdi = &old_task->ctx   (save here)
;   rsi = &new_task->ctx   (restore from here)
;
; We only save callee-saved registers (System V ABI):
;   rbx, rbp, r12-r15, and the return address (rip via call stack)
;
; The caller's rip is whatever address 'call switch_context' will return to.
; We push it implicitly via 'call', so it's already on the stack.

global switch_context

section .text
bits 64

switch_context:
    ; ── Save current task's callee-saved registers ────────────────────────
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rbx
    ; rsp now points to the bottom of the saved Context

    ; Store rsp into old_task->ctx
    mov [rdi], rsp

    ; ── Load next task's context ──────────────────────────────────────────
    mov rsp, [rsi]

    ; Restore callee-saved registers
    pop rbx
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    ; 'ret' pops the saved rip and jumps there —
    ; for a new task this is task_entry_trampoline,
    ; for a resuming task it's wherever it called switch_context from.
    ret

; ── user_task_trampoline ──────────────────────────────────────────────────
; switch_context 'ret's here for a new user task.
; The iretq frame is already on the stack above us.
global user_task_trampoline
user_task_trampoline:
    iretq
