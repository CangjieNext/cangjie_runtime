    cpu x64
    bits 64
    default rel

section .text  align=16

    extern g_SaveContextForNativeCallPtr
    extern g_DeleteContextForNativeCallPtr
    extern g_EnterSaferegionPtr
    extern g_LeaveSaferegionPtr
    extern g_ThrowPendingExceptionPtr

    %define CALLEE_ADDR r8
    %define THREAD_LOCAL_DATA r9

; Frame layout (offsets from rbp):
;   rbp - 8:    rdi (arg1)
;   rbp - 16:   rsi (arg2)
;   rbp - 24:   rdx (arg3)
;   rbp - 32:   rcx (arg4)
;   rbp - 40:   CALLEE_ADDR (r8, arg5)
;   rbp - 48:   THREAD_LOCAL_DATA (r9, arg6)
;   rbp - 56:   rax (varargs vector register count)
;   (8 bytes padding for xmm alignment)
;   rbp - 80:   xmm0 (16 bytes)
;   rbp - 96:   xmm1 (16 bytes)
;   rbp - 112:  xmm2 (16 bytes)
;   rbp - 128:  xmm3 (16 bytes)
;   rbp - 144:  xmm4 (16 bytes)
;   rbp - 160:  xmm5 (16 bytes)
;   rbp - 176:  xmm6 (16 bytes)
;   rbp - 192:  xmm7 (16 bytes)
;   rbp - 200:  rax (return value)
;   rbp - 208:  rdx (return value)
;   rbp - 224:  xmm0 (return value, 16 bytes)
;   rbp - 240:  xmm1 (return value, 16 bytes)

%define FRAME_SIZE 240

    global I2N_Stub
    align 16
I2N_Stub:
    push    rbp
    mov     rbp, rsp
    sub     rsp, FRAME_SIZE

    ; Save integer argument registers
    mov     [rbp - 8],  rdi
    mov     [rbp - 16], rsi
    mov     [rbp - 24], rdx
    mov     [rbp - 32], rcx
    mov     [rbp - 40], CALLEE_ADDR
    mov     [rbp - 48], THREAD_LOCAL_DATA
    mov     [rbp - 56], rax

    ; Save floating-point argument registers
    movapd  [rbp - 80],  xmm0
    movapd  [rbp - 96],  xmm1
    movapd  [rbp - 112], xmm2
    movapd  [rbp - 128], xmm3
    movapd  [rbp - 144], xmm4
    movapd  [rbp - 160], xmm5
    movapd  [rbp - 176], xmm6
    movapd  [rbp - 192], xmm7

    ; SaveC2NContext(pc, fa, threadLocalData)
    lea     rdi, [rel .i2n_unwind_pc]
.i2n_unwind_pc:
    mov     rsi, rbp
    mov     rdx, [rbp - 48]
    mov     r10, [rel g_SaveContextForNativeCallPtr wrt ..got]
    mov     r10, [r10]
    call    r10

    ; EnterSaferegion(false)
    xor     edi, edi
    mov     r10, [rel g_EnterSaferegionPtr wrt ..got]
    mov     r10, [r10]
    call    r10

    ; Restore integer argument registers
    mov     rdi, [rbp - 8]
    mov     rsi, [rbp - 16]
    mov     rdx, [rbp - 24]
    mov     rcx, [rbp - 32]
    mov     rax, [rbp - 56]

    ; Restore floating-point argument registers
    movapd  xmm0, [rbp - 80]
    movapd  xmm1, [rbp - 96]
    movapd  xmm2, [rbp - 112]
    movapd  xmm3, [rbp - 128]
    movapd  xmm4, [rbp - 144]
    movapd  xmm5, [rbp - 160]
    movapd  xmm6, [rbp - 176]
    movapd  xmm7, [rbp - 192]

    ; Call callee
    mov     r10, [rbp - 40]
    call    r10

    ; Save return values
    mov     [rbp - 200], rax
    mov     [rbp - 208], rdx
    movapd  [rbp - 224], xmm0
    movapd  [rbp - 240], xmm1

    ; LeaveSaferegion()
    mov     r10, [rel g_LeaveSaferegionPtr wrt ..got]
    mov     r10, [r10]
    call    r10

    ; DeleteC2NContext(threadLocalData)
    mov     rdi, [rbp - 48]
    mov     r10, [rel g_DeleteContextForNativeCallPtr wrt ..got]
    mov     r10, [r10]
    call    r10

    ; MRT_ThrowPendingException(nullptr)
    ; If a pending exception exists, this will unwind and not return.
    xor     rdi, rdi
    mov     r10, [rel g_ThrowPendingExceptionPtr wrt ..got]
    mov     r10, [r10]
    call    r10

    ; Restore return values
    mov     rax, [rbp - 200]
    mov     rdx, [rbp - 208]
    movapd  xmm0, [rbp - 224]
    movapd  xmm1, [rbp - 240]

    mov     rsp, rbp
    pop     rbp
    ret

    global I2N_Stub_End
I2N_Stub_End:
    nop
