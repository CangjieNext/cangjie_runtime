    cpu x64
    bits 64
    default rel

section .text align=16

    %define SAVED_RSP_REG rbp
    %define CALLEE_ADDR r8

    global _I2I_Stub
    align 16
_I2I_Stub:
    push    SAVED_RSP_REG
    mov     SAVED_RSP_REG, rsp

    call    CALLEE_ADDR

    pop     SAVED_RSP_REG
    ret

    global _I2I_Stub_End
_I2I_Stub_End:
    nop
