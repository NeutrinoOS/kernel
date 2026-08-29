bits 64
section .text
global isr_stub_table

%macro ISR_NOERR 1
isr_stub_%1:
    push 0
    push %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
isr_stub_%1:
    push %1
    jmp isr_common_stub
%endmacro

%macro ISR_GEN 1
%if %1 = 8 || %1 = 10 || %1 = 11 || %1 = 12 || %1 = 13 || %1 = 14 || %1 = 30
    ISR_ERR %1
%else
    ISR_NOERR %1
%endif
%endmacro

%assign __vec 0
%rep 256
    ISR_GEN __vec
%assign __vec __vec+1
%endrep

isr_common_stub:
    cli
    cld
    ; Materialize the normalized NeuExceptionContext.  The CPU's interrupt
    ; frame has a different size for same-ring and privilege-changing events,
    ; so rsp is captured explicitly rather than treating its optional slot as
    ; always present.
    sub rsp, 184
    mov [rsp + 0], rax
    mov [rsp + 8], rbx
    mov [rsp + 16], rcx
    mov [rsp + 24], rdx
    mov [rsp + 32], rsi
    mov [rsp + 40], rdi
    mov [rsp + 48], rbp
    mov [rsp + 64], r8
    mov [rsp + 72], r9
    mov [rsp + 80], r10
    mov [rsp + 88], r11
    mov [rsp + 96], r12
    mov [rsp + 104], r13
    mov [rsp + 112], r14
    mov [rsp + 120], r15

    mov rax, [rsp + 200]
    mov [rsp + 128], rax
    mov rax, [rsp + 216]
    mov [rsp + 136], rax
    mov qword [rsp + 144], 0
    mov rax, [rsp + 192]
    mov [rsp + 152], rax
    mov rax, [rsp + 184]
    mov [rsp + 160], rax
    mov rax, [rsp + 208]
    mov [rsp + 168], rax
    test al, 3
    jnz .from_userspace
    lea rax, [rsp + 224]
    mov [rsp + 56], rax
    mov qword [rsp + 168], 0
    jmp .context_ready
.from_userspace:
    mov rax, [rsp + 224]
    mov [rsp + 56], rax
    mov rax, [rsp + 232]
    mov [rsp + 168], rax
.context_ready:

    mov rdi, rsp
    extern isr_handler
    call isr_handler
    mov rdi, rsp
    extern isr_validate_return
    call isr_validate_return

    ; Handlers and the scheduler edit the normalized context.  Reflect the
    ; return state back into the hardware frame consumed by iretq.
    mov rax, [rsp + 128]
    mov [rsp + 200], rax
    mov rax, [rsp + 136]
    mov [rsp + 216], rax
    mov rax, [rsp + 168]
    mov [rsp + 208], rax
    test al, 3
    jz .return_to_kernel
    mov rax, [rsp + 56]
    mov [rsp + 224], rax
    mov rax, [rsp + 176]
    mov [rsp + 232], rax
.return_to_kernel:

    mov r15, [rsp + 120]
    mov r14, [rsp + 112]
    mov r13, [rsp + 104]
    mov r12, [rsp + 96]
    mov r11, [rsp + 88]
    mov r10, [rsp + 80]
    mov r9, [rsp + 72]
    mov r8, [rsp + 64]
    mov rbp, [rsp + 48]
    mov rdi, [rsp + 40]
    mov rsi, [rsp + 32]
    mov rdx, [rsp + 24]
    mov rcx, [rsp + 16]
    mov rbx, [rsp + 8]
    mov rax, [rsp + 0]

    add rsp, 200
    iretq

section .rodata
isr_stub_table:
%assign i 0
%rep 256
    dq isr_stub_%+i
%assign i i+1
%endrep
