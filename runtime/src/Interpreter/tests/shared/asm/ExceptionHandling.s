    cpu x64
    bits 64
    default rel

section .text  align=16

    extern landingPad
    extern g_GetCurrentCatchFunctionPCPtr

    global LandingPadWrapper
    align 16
LandingPadWrapper:
    ; Jumped here from RestoreContextForEH.
    ; Manually construct function frame for landingPad.

    ; TODO should we save volatile registers?
    mov rax, [rel g_GetCurrentCatchFunctionPCPtr wrt ..got]
    mov rax, [rax]
    call rax

    push rax ; push the return address for tail call

    jmp landingPad wrt ..plt


extern g_ThrowExceptionPtr
    extern g_GetThreadLocalDataPtr
    extern C2I_Stub
    extern C2I_Stub_End
    extern I2I_Stub
    extern I2I_Stub_End
    
    global unwindInterpretedFrameAndRethrowException
    align 16
unwindInterpretedFrameAndRethrowException:
    ; Dynamically unwinds frames until C2I_Stub or I2I_Stub is found.
    ; Restores context if it's C2I_Stub, then rethrows the exception 
    ; like it was thrown from the caller of the found stub.
    ; Arguments:
    ;   rdi = exception_obj

    ; no prologue since we don`t need unwinder`s frame

    mov r8, [rel C2I_Stub wrt ..got]
    mov r9, [rel C2I_Stub_End wrt ..got]

    mov r10, [rel I2I_Stub wrt ..got]
    mov r11, [rel I2I_Stub_End wrt ..got]

.unwind_loop:
    ; if RBP hits 0, we reached the top of the thread stack without finding a stub.
    test rbp, rbp
    jz .crash
    
    ; read the return address of the current frame
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

    ; =========================================================================
    ; The current frame was called by I2I_Stub. 
    ; =========================================================================
.i2i_stub_found:
    ; Unwind the current frame to enter I2I_Stub's frame.
    mov rsp, rbp
    pop rbp
    
    ; I2I_Stub: no callee-saved registers need restoring.
    ; Unwind I2I_Stub's frame itself.
    mov rsp, rbp
    pop rbp
    ; do not pop the return address, it is needed for the tail call
    
    jmp .tail_call_throw

    ; =========================================================================
    ; The current frame was called by C2I_Stub. 
    ; =========================================================================
.c2i_stub_found:
    ; Unwind the current frame to enter C2I_Stub's frame.
    mov rsp, rbp
    pop rbp
    
    ; save exception_obj and align stack
    push rdi

    mov rax, [rel g_GetThreadLocalDataPtr wrt ..got]
    mov rax, [rax]
    call rax
    mov r15, rax
    
    ; restore exception_obj
    pop rdi

    ; restore the rest of C2I_Stub's saved registers
    mov r14, [rbp - 16]
    mov r13, [rbp - 24]
    mov r12, [rbp - 32]
    mov rbx, [rbp - 40]

    ; unwind C2I_Stub's frame itself
    mov rsp, rbp
    pop rbp
    ; do not pop the return address, it is needed for the tail call

.tail_call_throw:
    ; RSP now points to the return address of the stub's caller frame.
    ; ready to make a tail call
    mov rax, [rel g_ThrowExceptionPtr wrt ..got]
    mov rax, [rax]
    jmp rax

.not_stub:
    ; unwind 1 intermediate frame and continue searching
    mov rsp, rbp
    pop rbp
    jmp .unwind_loop

.crash:
    ud2