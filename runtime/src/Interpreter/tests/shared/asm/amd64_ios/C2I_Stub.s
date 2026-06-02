    cpu x64
    bits 64
    default rel

section .data align=16

    extern _virtualFuncs

section .text align=16

%define VIRTUAL_FUNCS_N 10
%define CALLEE_ADDR rdi

    global  _trampolinesStart
    align 16
_trampolinesStart:
    %assign i 1
    %rep VIRTUAL_FUNCS_N
        mov rax, [rel _virtualFuncs wrt ..gotpcrel]
        mov CALLEE_ADDR, [rax + i*8]
        jmp near _C2I_Stub
        %assign i i + 1
    %endrep


global _C2I_Stub
    extern _g_CJ_MCC_StackGrowStubPtr
    extern _g_GetThreadLocalDataPtr
    extern _g_InterpreterStackReserveSize
    extern _g_getPendingExceptionPtr

_C2I_Stub:
    %define SAVED_RSP_REG rbp
    %define CalleeSavedRegistersStubFrameSize 8 * 8

    push    SAVED_RSP_REG
    mov     SAVED_RSP_REG, rsp

    sub     rsp, CalleeSavedRegistersStubFrameSize

    mov     rax, [r15 + 40]

    mov     r11, [rel _g_InterpreterStackReserveSize wrt ..gotpcrel]
    mov     r11, [r11]

    add     rax, r11

    cmp     rsp, rax
    jbe     .Lstack_overflow
.Lstack_check_end:
    mov     [SAVED_RSP_REG - 8], r15
    mov     [SAVED_RSP_REG - 16], r14
    mov     [SAVED_RSP_REG - 24], r13
    mov     [SAVED_RSP_REG - 32], r12
    mov     [SAVED_RSP_REG - 40], rbx

.Lcall_interpreter:
    mov     rax, CALLEE_ADDR
    mov     rdi, r15
    call    rax

    mov     [SAVED_RSP_REG - 8], rax

    mov     r10, [rel _g_GetThreadLocalDataPtr wrt ..gotpcrel]
    mov     r10, [r10]
    call    r10
    mov     r15, rax

    mov     r14, [SAVED_RSP_REG - 16]
    mov     r13, [SAVED_RSP_REG - 24]
    mov     r12, [SAVED_RSP_REG - 32]
    mov     rbx, [SAVED_RSP_REG - 40]

    mov     rax, [SAVED_RSP_REG - 8]
    add     rsp, CalleeSavedRegistersStubFrameSize

    pop     SAVED_RSP_REG
    ret

.Lstack_overflow:
    mov     [SAVED_RSP_REG - 8], r15
    mov     [SAVED_RSP_REG - 16], r14
    mov     [SAVED_RSP_REG - 24], r13
    mov     [SAVED_RSP_REG - 32], r12
    mov     [SAVED_RSP_REG - 40], rbx

    mov     rax, CalleeSavedRegistersStubFrameSize
    mov     r11, [rel _g_InterpreterStackReserveSize wrt ..gotpcrel]
    mov     r11, [r11]
    add     rax, r11

    mov     r10, [rel _g_CJ_MCC_StackGrowStubPtr wrt ..gotpcrel]
    mov     r10, [r10]
    call    r10

    push    rdi
    mov     r10, [rel _g_getPendingExceptionPtr wrt ..gotpcrel]
    mov     r10, [r10]
    call    r10
    pop     rdi
    test    rax, rax
    jnz     .Lsoe_return

    jmp     .Lcall_interpreter

.Lsoe_return:
    mov     rsp, rbp
    pop     SAVED_RSP_REG
    ret

    global _C2I_Stub_End
_C2I_Stub_End:
    nop
