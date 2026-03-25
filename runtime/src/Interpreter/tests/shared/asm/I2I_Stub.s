cpu x64
    bits 64
    default rel

section .text align=16

    %define SAVED_RSP_REG rbp
    %define CALLEE_ADDR r8

    global I2I_Stub
    align 16
I2I_Stub:
    push    SAVED_RSP_REG
    mov     SAVED_RSP_REG, rsp

    call    CALLEE_ADDR

    pop     SAVED_RSP_REG
    ret

    global I2I_Stub_End
I2I_Stub_End:
    nop