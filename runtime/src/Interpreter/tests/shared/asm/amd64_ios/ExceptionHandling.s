    cpu x64
    bits 64
    default rel

section .text align=16

    extern _landingPad
    extern _g_GetCurrentCatchFunctionPCPtr

    global _LandingPadWrapper
    align 16
_LandingPadWrapper:
    mov rax, [rel _g_GetCurrentCatchFunctionPCPtr wrt ..gotpcrel]
    mov rax, [rax]
    call rax

    push rax

    jmp _landingPad


    extern _g_ThrowExceptionPtr
    extern _g_GetThreadLocalDataPtr
    extern _C2I_Stub
    extern _C2I_Stub_End
    extern _I2I_Stub
    extern _I2I_Stub_End

    global _unwindInterpretedFrameAndRethrowException
    align 16
_unwindInterpretedFrameAndRethrowException:
    mov r8,  _C2I_Stub
    mov r9,  _C2I_Stub_End

    mov r10, _I2I_Stub
    mov r11, _I2I_Stub_End

.unwind_loop:
    test rbp, rbp
    jz .crash

    mov rax, [rbp + 8]

    cmp rax, r8
    jb .check_i2i
    cmp rax, r9
    jb .c2i_stub_found

.check_i2i:
    cmp rax, r10
    jb .not_stub
    cmp rax, r11
    jae .not_stub

.i2i_stub_found:
    mov rsp, rbp
    pop rbp

    mov rsp, rbp
    pop rbp

    jmp .tail_call_throw

.c2i_stub_found:
    mov rsp, rbp
    pop rbp

    push rdi

    mov rax, [rel _g_GetThreadLocalDataPtr wrt ..gotpcrel]
    mov rax, [rax]
    call rax
    mov r15, rax

    pop rdi

    mov r14, [rbp - 16]
    mov r13, [rbp - 24]
    mov r12, [rbp - 32]
    mov rbx, [rbp - 40]

    mov rsp, rbp
    pop rbp

.tail_call_throw:
    mov rax, [rel _g_ThrowExceptionPtr wrt ..gotpcrel]
    mov rax, [rax]
    jmp rax

.not_stub:
    mov rsp, rbp
    pop rbp
    jmp .unwind_loop

.crash:
    ud2
