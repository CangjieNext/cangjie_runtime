    cpu x64
    bits 64
    default rel

section .data  align=16

    extern virtualFuncs ; defined in libinterpreter.cpp

section .text  align=16

%define VIRTUAL_FUNCS_N 10
%define CALLEE_ADDR rdi

    global  trampolinesStart
    align 16
trampolinesStart:
    %assign i 1
    %rep VIRTUAL_FUNCS_N
        mov rax, [rel virtualFuncs wrt ..got]
        mov CALLEE_ADDR, [rax + i*8]
        jmp near C2I_Stub
        %assign i i + 1
    %endrep


global C2I_Stub
    extern g_GetThreadLocalDataPtr
C2I_Stub:
    %define SAVED_RSP_REG rbp
    %define CalleeSavedRegistersStubFrameSize 8 * 8
    
    push    SAVED_RSP_REG
    mov     SAVED_RSP_REG, rsp

    sub     rsp, CalleeSavedRegistersStubFrameSize

    ; save non-volatile registers allowing GC-tracing of contained references
    mov     [SAVED_RSP_REG - 8], r15 ; save old EE, expected by cjnative-GC
    mov     [SAVED_RSP_REG - 16], r14
    mov     [SAVED_RSP_REG - 24], r13
    mov     [SAVED_RSP_REG - 32], r12
    mov     [SAVED_RSP_REG - 40], rbx

    mov     rax, CALLEE_ADDR
    mov     rdi, r15 ; pass EE as first arg
    call    rax

    ; TODO rethrow possible exception

    ; Refresh r15 (EE) after the interpreter returns: the TLD pointer may have
    ; changed (e.g. after a GC safepoint).  Temporarily save the interpreter's
    ; return value (rax) in the saved-r15 slot so it survives the call.
    mov     [SAVED_RSP_REG - 8], rax

    mov     r10, [rel g_GetThreadLocalDataPtr wrt ..got]
    mov     r10, [r10]
    call    r10
    mov     r15, rax            ; r15 = refreshed EE

    ; restore other saved registers
    mov     r14, [SAVED_RSP_REG - 16]
    mov     r13, [SAVED_RSP_REG - 24]
    mov     r12, [SAVED_RSP_REG - 32]
    mov     rbx, [SAVED_RSP_REG - 40]

    ; restore interpreter's return value
    mov     rax, [SAVED_RSP_REG - 8]
    add     rsp, CalleeSavedRegistersStubFrameSize

    pop     SAVED_RSP_REG
    ret

    global C2I_Stub_End
C2I_Stub_End:
    nop