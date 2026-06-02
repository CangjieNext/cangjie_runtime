    cpu x64
    bits 64
    default rel

section .text align=16

%define CALLEE_ADDR r8
%define THREAD_LOCAL_DATA r9

    global   _I2C_Stub
_I2C_Stub:
    %define SAVED_RSP_REG rbp

    push    SAVED_RSP_REG
    mov     SAVED_RSP_REG, rsp

    mov     r15, THREAD_LOCAL_DATA

    call    CALLEE_ADDR

    pop     SAVED_RSP_REG
    ret
