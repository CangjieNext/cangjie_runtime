cpu x64
    bits 64
    default rel

section .text  align=16

    extern g_C2NStubPtr
    global   I2N_Stub

I2N_Stub:
    %define arg1 rdi
    %define arg2 rsi
    %define arg3 rdx
    %define arg4 rcx
    %define CALLEE_ADDR r8
    %define THREAD_LOCAL_DATA r9
    %define SAVED_RSP_REG rbp
    
    push    SAVED_RSP_REG
    mov     SAVED_RSP_REG, rsp

    mov     r15, THREAD_LOCAL_DATA

    ; C2NStub expects callee_addr and cpStackSize pushed to the stack
    ; [rsp + 8] = CALLEE_ADDR
    ; [rsp]     = cpStackSize
    push    CALLEE_ADDR
    push    qword 0         ; TODO support passing more arguments

    mov     r10, [rel g_C2NStubPtr wrt ..got]
    mov     r10, [r10]
    call    r10

    pop     SAVED_RSP_REG
    ret