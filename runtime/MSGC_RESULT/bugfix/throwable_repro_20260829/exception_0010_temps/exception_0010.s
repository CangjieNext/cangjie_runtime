	.text
	.def	@feat.00;
	.scl	3;
	.type	0;
	.endef
	.globl	@feat.00
.set @feat.00, 0
	.file	"0-default"
	.def	_CGP7defaultfiHv;
	.scl	3;
	.type	32;
	.endef
	.section	.cjinit_function,"xr"
	.p2align	4, 0x90                         # -- Begin function _CGP7defaultfiHv
.Ltmp0:                                 # @_CGP7defaultfiHv
	.long	.Lmethod_desc._CGP7defaultfiHv-.Ltmp0
_CGP7defaultfiHv:
.Lfunc_begin0:
.seh_proc _CGP7defaultfiHv
	.seh_handler .L__cj_personality_v0$, @unwind, @except
# %bb.0:                                # %bb0
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$32, %rsp
	movq	40(%r15), %rax
	cmpq	%rax, %rsp
	jbe	.Lstack.overflow0
.Lstack.check.end0:
	.seh_stackalloc 32
	leaq	32(%rsp), %rbp
	.seh_setframe %rbp, 32
	.seh_endprologue
	callq	.LCJ_Safepoint_Stub
.Ltmp1:
	callq	_CGPaciiHv
.Ltmp2:
	callq	_CGPatiiHv
.Ltmp3:
.L_epilogue0:
	movq	%rbp, %rsp
	subq	$32, %rsp
	addq	$32, %rsp
	popq	%rbp
	retq
.Lstack.overflow0:
	addq	$32, %rsp
	movabsq	$16, %rax
	callq	CJ_MCC_StackGrowStub
.Ltmp4:
	subq	$32, %rsp
	jmp	.Lstack.check.end0
.Lfunc_end0:
	.seh_handlerdata
	.section	.cjinit_function,"xr"
	.seh_endproc
	.section	.xdata,"dw"
	.p2align	2
CJ_except_table0:
.Lexception0:
	.long	1431655765                      # No callsite function EH table header
	.section	.cjinit_function,"xr"
                                        # -- End function
	.def	.L__cj_personality_v0$;
	.scl	3;
	.type	32;
	.endef
	.text
	.p2align	4, 0x90                         # -- Begin function __cj_personality_v0$
.L__cj_personality_v0$:                 # @"__cj_personality_v0$"
# %bb.0:                                # %entry
	xorl	%eax, %eax
	retq
                                        # -- End function
	.def	_CGP7defaultflHv;
	.scl	3;
	.type	32;
	.endef
	.p2align	4, 0x90                         # -- Begin function _CGP7defaultflHv
.Ltmp5:                                 # @_CGP7defaultflHv
	.long	.Lmethod_desc._CGP7defaultflHv-.Ltmp5
_CGP7defaultflHv:
.Lfunc_begin1:
.seh_proc _CGP7defaultflHv
	.seh_handler .L__cj_personality_v0$, @unwind, @except
# %bb.0:                                # %bb0
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$32, %rsp
	movq	40(%r15), %rax
	cmpq	%rax, %rsp
	jbe	.Lstack.overflow1
.Lstack.check.end1:
	.seh_stackalloc 32
	leaq	32(%rsp), %rbp
	.seh_setframe %rbp, 32
	.seh_endprologue
	callq	.LCJ_Safepoint_Stub
.Ltmp6:
	callq	_CGPacilHv
.Ltmp7:
	callq	_CGPatilHv
.Ltmp8:
.L_epilogue1:
	movq	%rbp, %rsp
	subq	$32, %rsp
	addq	$32, %rsp
	popq	%rbp
	retq
.Lstack.overflow1:
	addq	$32, %rsp
	movabsq	$16, %rax
	callq	CJ_MCC_StackGrowStub
.Ltmp9:
	subq	$32, %rsp
	jmp	.Lstack.check.end1
.Lfunc_end1:
	.seh_handlerdata
	.text
	.seh_endproc
	.section	.xdata,"dw"
	.p2align	2
CJ_except_table2:
.Lexception1:
	.long	1431655765                      # No callsite function EH table header
	.text
                                        # -- End function
	.def	_CGP7defaultiiHv;
	.scl	2;
	.type	32;
	.endef
	.section	.cjinit_function,"xr"
	.globl	_CGP7defaultiiHv                # -- Begin function _CGP7defaultiiHv
	.p2align	4, 0x90
.Ltmp10:                                # @_CGP7defaultiiHv
	.long	.Lmethod_desc._CGP7defaultiiHv-.Ltmp10
_CGP7defaultiiHv:
.Lfunc_begin2:
.seh_proc _CGP7defaultiiHv
	.seh_handler .L__cj_personality_v0$, @unwind, @except
# %bb.0:                                # %bb0
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$48, %rsp
	movq	40(%r15), %rax
	cmpq	%rax, %rsp
	jbe	.Lstack.overflow2
.Lstack.check.end2:
	.seh_stackalloc 48
	leaq	48(%rsp), %rbp
	.seh_setframe %rbp, 48
	.seh_endprologue
	movb	($has_applied_pkg_init_func)(%rip), %al
	movb	%al, -1(%rbp)                   # 1-byte Spill
	callq	.LCJ_Safepoint_Stub
.Ltmp11:
	movb	-1(%rbp), %al                   # 1-byte Reload
	testb	$1, %al
	jne	.LBB3_1
	jmp	.LBB3_2
.LBB3_1:                                # %bb1
.L_epilogue2:
	movq	%rbp, %rsp
	subq	$48, %rsp
	addq	$48, %rsp
	popq	%rbp
	retq
.LBB3_2:                                # %bb2
	movb	$1, ($has_applied_pkg_init_func)(%rip)
	callq	_CGP7defaultfiHv
.Ltmp12:
	xorl	%eax, %eax
	movl	%eax, %ecx
	leaq	__CJMetadataStart(%rip), %rcx
	callq	CJ_MRT_PreInitializePackage
	nop
	addq	$48, %rsp
	popq	%rbp
	retq
.Lstack.overflow2:
	addq	$48, %rsp
	movabsq	$16, %rax
	callq	CJ_MCC_StackGrowStub
.Ltmp13:
	subq	$48, %rsp
	jmp	.Lstack.check.end2
.Lfunc_end2:
	.seh_handlerdata
	.section	.cjinit_function,"xr"
	.seh_endproc
	.section	.xdata,"dw"
	.p2align	2
CJ_except_table3:
.Lexception2:
	.long	1431655765                      # No callsite function EH table header
	.section	.cjinit_function,"xr"
                                        # -- End function
	.def	_CGP7defaultilHv;
	.scl	2;
	.type	32;
	.endef
	.text
	.globl	_CGP7defaultilHv                # -- Begin function _CGP7defaultilHv
	.p2align	4, 0x90
.Ltmp14:                                # @_CGP7defaultilHv
	.long	.Lmethod_desc._CGP7defaultilHv-.Ltmp14
_CGP7defaultilHv:
.Lfunc_begin3:
.seh_proc _CGP7defaultilHv
	.seh_handler .L__cj_personality_v0$, @unwind, @except
# %bb.0:                                # %bb0
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$48, %rsp
	movq	40(%r15), %rax
	cmpq	%rax, %rsp
	jbe	.Lstack.overflow3
.Lstack.check.end3:
	.seh_stackalloc 48
	leaq	48(%rsp), %rbp
	.seh_setframe %rbp, 48
	.seh_endprologue
	movb	has_invoked_pkg_init_literal(%rip), %al
	movb	%al, -1(%rbp)                   # 1-byte Spill
	callq	.LCJ_Safepoint_Stub
.Ltmp15:
	movb	-1(%rbp), %al                   # 1-byte Reload
	testb	$1, %al
	jne	.LBB4_1
	jmp	.LBB4_2
.LBB4_1:                                # %bb1
.L_epilogue3:
	movq	%rbp, %rsp
	subq	$48, %rsp
	addq	$48, %rsp
	popq	%rbp
	retq
.LBB4_2:                                # %bb2
	movb	$1, has_invoked_pkg_init_literal(%rip)
	callq	_CGP7defaultflHv
.Ltmp16:
	addq	$48, %rsp
	popq	%rbp
	retq
.Lstack.overflow3:
	addq	$48, %rsp
	movabsq	$16, %rax
	callq	CJ_MCC_StackGrowStub
.Ltmp17:
	subq	$48, %rsp
	jmp	.Lstack.check.end3
.Lfunc_end3:
	.seh_handlerdata
	.text
	.seh_endproc
	.section	.xdata,"dw"
	.p2align	2
CJ_except_table4:
.Lexception3:
	.long	1431655765                      # No callsite function EH table header
	.text
                                        # -- End function
	.def	"_CN7default1A6<init>Hv";
	.scl	2;
	.type	32;
	.endef
	.globl	"_CN7default1A6<init>Hv"        # -- Begin function _CN7default1A6<init>Hv
	.p2align	4, 0x90
"_CN7default1A6<init>Hv":               # @"_CN7default1A6<init>Hv"
.Lfunc_begin4:
.seh_proc "_CN7default1A6<init>Hv"
	.seh_handler .L__cj_personality_v0$, @unwind, @except
# %bb.0:                                # %bb0
	retq
.Lfunc_end4:
	.seh_handlerdata
	.text
	.seh_endproc
	.section	.xdata,"dw"
	.p2align	2
CJ_except_table5:
.Lexception4:
	.long	1431655765                      # No callsite function EH table header
	.text
                                        # -- End function
	.def	"_CN7default6<main>Hv";
	.scl	2;
	.type	32;
	.endef
	.section	.cjinit_function,"xr"
	.globl	"_CN7default6<main>Hv"          # -- Begin function _CN7default6<main>Hv
	.p2align	4, 0x90
.Ltmp27:                                # @"_CN7default6<main>Hv"
	.long	".Lmethod_desc._CN7default6<main>Hv"-.Ltmp27
"_CN7default6<main>Hv":
.Lfunc_begin5:
.seh_proc "_CN7default6<main>Hv"
	.seh_handler .L__cj_personality_v0$, @unwind, @except
# %bb.0:                                # %allocas
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$128, %rsp
	movq	40(%r15), %rax
	cmpq	%rax, %rsp
	jbe	.Lstack.overflow4
.Lstack.check.end4:
	.seh_stackalloc 128
	leaq	128(%rsp), %rbp
	.seh_setframe %rbp, 128
	.seh_endprologue
# %bb.1:                                # %bb0
	leaq	"std.collection:ArrayList<default:A>.ti"(%rip), %rcx
	movq	%rcx, -48(%rbp)                 # 8-byte Spill
	movl	"std.collection:ArrayList<default:A>.ti"+12(%rip), %edx
	addl	$15, %edx
	andl	$-8, %edx
	callq	.LCJ_Safepoint_Stub
.Ltmp28:
	movq	48(%r15), %r11
	testq	%r11, %r11
	jne	.LMSGCAllocSlow0
	testq	%rdx, %rdx
	je	.LMSGCAllocSlow0
	cmpq	$16384, %rdx                    # imm = 0x4000
	ja	.LMSGCAllocSlow0
	leaq	lCJMSGCSizeClassTable(%rip), %r10
	leaq	7(%rdx), %r9
	shrq	$3, %r9
	movzbl	(%r10,%r9), %r9d
	movzwl	2050(%r10,%r9,2), %r10d
	movq	88(%r15), %r11
	testq	%r11, %r11
	je	.LMSGCAllocSlow0
	shlq	$1, %r9
	addq	$1, %r9
	movq	(%r11,%r9,8), %r11
	testq	%r11, %r11
	je	.LMSGCAllocSlow0
	movq	%r10, %r8
	movzbl	60(%r11), %r10d
	cmpl	$1, %r10d
	je	.LMSGCAllocBump0
	cmpl	$2, %r10d
	jne	.LMSGCAllocSlow0
.LMSGCAllocBitmapRetry0:
	movq	40(%r11), %r10
	testq	%r10, %r10
	je	.LMSGCAllocSlow0
	bsfq	%r10, %r9
	movq	24(%r11), %rax
	movq	(%rax,%r9,8), %r10
	testq	%r10, %r10
	je	.LMSGCAllocBitmapRepair0
	bsfq	%r10, %rdx
	btrq	%rdx, %r10
	movq	%r10, (%rax,%r9,8)
	testq	%r10, %r10
	jne	.LMSGCAllocBitmapSummaryDone0
	movq	40(%r11), %rax
	btrq	%r9, %rax
	movq	%rax, 40(%r11)
.LMSGCAllocBitmapSummaryDone0:
	shlq	$6, %r9
	addq	%rdx, %r9
	imulq	%r8, %r9
	movq	8(%r11), %rax
	subq	$32768, %rax                    # imm = 0x8000
	addq	%r9, %rax
	movq	%rcx, (%rax)
	jmp	.LMSGCAllocFin0
.LMSGCAllocBitmapRepair0:
	movq	40(%r11), %r10
	btrq	%r9, %r10
	movq	%r10, 40(%r11)
	jmp	.LMSGCAllocBitmapRetry0
.LMSGCAllocBump0:
	movq	(%r11), %r10
	leaq	(%r10,%r8), %rax
	cmpq	8(%r11), %rax
	ja	.LMSGCAllocSlow0
	movq	%rcx, (%r10)
	movq	%rax, (%r11)
	movq	%r10, %rax
	jmp	.LMSGCAllocFin0
.LMSGCAllocSlow0:
	callq	CJ_MCC_NewObjectFast_MSGC_dynamic_sc1
.LMSGCAllocFin0:
.Ltmp29:
	movq	-48(%rbp), %rdx                 # 8-byte Reload
	movq	%rax, %rcx
	movq	%rcx, -32(%rbp)
	callq	"_CNac9ArrayListIG_E6<init>Hv"
.Ltmp30:
	movq	-32(%rbp), %rax
	movq	%rax, -24(%rbp)
.LBB6_2:                                # %bb7
                                        # =>This Inner Loop Header: Depth=1
	movq	-24(%rbp), %rax
	movq	%rax, -32(%rbp)
.Ltmp18:
	leaq	"default:A.ti"(%rip), %rcx
	movl	$8, %edx
	movq	48(%r15), %r11
	testq	%r11, %r11
	jne	.LMSGCAllocSlow1
	movq	88(%r15), %r11
	testq	%r11, %r11
	je	.LMSGCAllocSlow1
	movq	(%r11), %r11
	testq	%r11, %r11
	je	.LMSGCAllocSlow1
	movzbl	60(%r11), %r10d
	cmpl	$1, %r10d
	je	.LMSGCAllocBump1
	cmpl	$2, %r10d
	jne	.LMSGCAllocSlow1
.LMSGCAllocBitmapRetry1:
	movq	40(%r11), %r10
	testq	%r10, %r10
	je	.LMSGCAllocSlow1
	bsfq	%r10, %r9
	movq	24(%r11), %rax
	movq	(%rax,%r9,8), %r10
	testq	%r10, %r10
	je	.LMSGCAllocBitmapRepair1
	bsfq	%r10, %rdx
	btrq	%rdx, %r10
	movq	%r10, (%rax,%r9,8)
	testq	%r10, %r10
	jne	.LMSGCAllocBitmapSummaryDone1
	movq	40(%r11), %rax
	btrq	%r9, %rax
	movq	%rax, 40(%r11)
.LMSGCAllocBitmapSummaryDone1:
	shlq	$6, %r9
	addq	%rdx, %r9
	shlq	$3, %r9
	movq	8(%r11), %rax
	subq	$32768, %rax                    # imm = 0x8000
	addq	%r9, %rax
	movq	%rcx, (%rax)
	jmp	.LMSGCAllocFin1
.LMSGCAllocBitmapRepair1:
	movq	40(%r11), %r10
	btrq	%r9, %r10
	movq	%r10, 40(%r11)
	jmp	.LMSGCAllocBitmapRetry1
.LMSGCAllocBump1:
	movq	(%r11), %r10
	leaq	8(%r10), %rax
	cmpq	8(%r11), %rax
	ja	.LMSGCAllocSlow1
	movq	%rcx, (%r10)
	movq	%rax, (%r11)
	movq	%r10, %rax
	jmp	.LMSGCAllocFin1
.LMSGCAllocSlow1:
	callq	CJ_MCC_NewObjectFast_MSGC_cls0_sz8_sc0
.LMSGCAllocFin1:
.Ltmp31:
.Ltmp19:
	movq	%rax, -56(%rbp)                 # 8-byte Spill
	jmp	.LBB6_3
.LBB6_3:                                # %normalDest
                                        #   in Loop: Header=BB6_2 Depth=1
	movq	-56(%rbp), %rax                 # 8-byte Reload
	movq	%rax, -64(%rbp)                 # 8-byte Spill
	movq	-32(%rbp), %rax
# %bb.4:                                # %bb8
                                        #   in Loop: Header=BB6_2 Depth=1
	movq	-64(%rbp), %rcx                 # 8-byte Reload
	movq	%rcx, -40(%rbp)
.Ltmp21:
	leaq	"default:A.ti"(%rip), %rdx
	callq	"_CN7default1A6<init>Hv"
.Ltmp32:
.Ltmp22:
	jmp	.LBB6_5
.LBB6_5:                                # %normalDest1
                                        #   in Loop: Header=BB6_2 Depth=1
	movq	-32(%rbp), %rax
	movq	%rax, -80(%rbp)                 # 8-byte Spill
	movq	-40(%rbp), %rax
	movq	%rax, -72(%rbp)                 # 8-byte Spill
# %bb.6:                                # %bb9
                                        #   in Loop: Header=BB6_2 Depth=1
.Ltmp24:
	movq	-72(%rbp), %r8                  # 8-byte Reload
	movq	-80(%rbp), %rdx                 # 8-byte Reload
	leaq	"std.collection:ArrayList<default:A>.ti"(%rip), %r9
	leaq	-8(%rbp), %rcx
	callq	_CNac9ArrayListIG_E3addHG_
.Ltmp33:
.Ltmp25:
	jmp	.LBB6_7
.LBB6_7:                                # %normalDest2
                                        #   in Loop: Header=BB6_2 Depth=1
	callq	.LCJ_Safepoint_Stub
.Ltmp34:
	jmp	.LBB6_2
.LBB6_8:                                # %bb31
.Ltmp26:
	jmp	.LBB6_12
.LBB6_9:                                # %bb324
.Ltmp23:
	jmp	.LBB6_11
.LBB6_10:                               # %bb325
.Ltmp20:
.LBB6_11:                               # %bb32
	jmp	.LBB6_12
.LBB6_12:                               # %bb3
	callq	CJ_MCC_GetExceptionWrapper
	movq	%rax, %rcx
	callq	CJ_MCC_PostThrowException
	movq	%rax, -88(%rbp)                 # 8-byte Spill
	movq	(%rax), %rcx
	movq	".refptr.std.core:Exception.ti"(%rip), %rdx
	callq	CJ_MCC_IsSubType
	testb	$1, %al
	jne	.LBB6_13
	jmp	.LBB6_14
.LBB6_13:                               # %bb11
	movq	-88(%rbp), %rdx                 # 8-byte Reload
	movq	%rdx, -32(%rbp)
	movq	".refptr.std.core:Exception.ti"(%rip), %r8
	leaq	-8(%rbp), %rcx
	callq	_CNat9Exception15printStackTraceHv
.Ltmp35:
	movq	$0, -16(%rbp)
	movq	-16(%rbp), %rax
.L_epilogue4:
	movq	%rbp, %rsp
	subq	$128, %rsp
	addq	$128, %rsp
	popq	%rbp
	retq
.LBB6_14:                               # %bb12
	movq	-88(%rbp), %rcx                 # 8-byte Reload
	movq	%rcx, -32(%rbp)
	callq	CJ_MCC_ThrowException
	int3
.Lstack.overflow4:
	addq	$128, %rsp
	movabsq	$16, %rax
	callq	CJ_MCC_StackGrowStub
.Ltmp36:
	subq	$128, %rsp
	jmp	.Lstack.check.end4
.Lfunc_end5:
	.seh_handlerdata
	.section	.cjinit_function,"xr"
	.seh_endproc
	.section	.xdata,"dw"
	.p2align	2
CJ_except_table6:
.Lexception5:
	.byte	255                             # @LPStart Encoding = omit
	.byte	0                               # @TType Encoding = absptr
	.uleb128 .Lttbase0-.Lttbaseref0
.Lttbaseref0:
	.byte	1                               # Call site Encoding = uleb128
	.uleb128 .Lcst_end5-.Lcst_begin5
.Lcst_begin5:
	.uleb128 .Ltmp18-.Lfunc_begin5          # >> Call Site 1 <<
	.uleb128 .Ltmp19-.Ltmp18                #   Call between .Ltmp18 and .Ltmp19
	.uleb128 .Ltmp20-.Lfunc_begin5          #     jumps to .Ltmp20
	.byte	1                               #   On action: 1
	.uleb128 .Ltmp21-.Lfunc_begin5          # >> Call Site 2 <<
	.uleb128 .Ltmp22-.Ltmp21                #   Call between .Ltmp21 and .Ltmp22
	.uleb128 .Ltmp23-.Lfunc_begin5          #     jumps to .Ltmp23
	.byte	1                               #   On action: 1
	.uleb128 .Ltmp24-.Lfunc_begin5          # >> Call Site 3 <<
	.uleb128 .Ltmp25-.Ltmp24                #   Call between .Ltmp24 and .Ltmp25
	.uleb128 .Ltmp26-.Lfunc_begin5          #     jumps to .Ltmp26
	.byte	1                               #   On action: 1
.Lcst_end5:
	.byte	1                               # >> Action Record 1 <<
                                        #   Catch TypeInfo 1
	.byte	0                               #   No further actions
	.p2align	2
                                        # >> Catch TypeInfos <<
	.quad	"std.core:Exception.ti"         # TypeInfo 1
.Lttbase0:
	.p2align	2
	.section	.cjinit_function,"xr"
                                        # -- End function
	.def	"_CNat5ArrayICN7default1AEE6<init>Hv";
	.scl	3;
	.type	32;
	.endef
	.text
	.p2align	4, 0x90                         # -- Begin function _CNat5ArrayICN7default1AEE6<init>Hv
.Ltmp37:                                # @"_CNat5ArrayICN7default1AEE6<init>Hv"
	.long	".Lmethod_desc._CNat5ArrayICN7default1AEE6<init>Hv"-.Ltmp37
"_CNat5ArrayICN7default1AEE6<init>Hv":
.Lfunc_begin6:
.seh_proc "_CNat5ArrayICN7default1AEE6<init>Hv"
	.seh_handler .L__cj_personality_v0$, @unwind, @except
# %bb.0:                                # %bb0
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$48, %rsp
	.seh_stackalloc 48
	leaq	48(%rsp), %rbp
	.seh_setframe %rbp, 48
	.seh_endprologue
	movq	$0, 8(%rcx)
	movq	%rdx, -8(%rbp)
	movq	%rcx, -16(%rbp)
	leaq	"RawArray<default:A>.ti"(%rip), %rcx
	xorl	%eax, %eax
	movl	%eax, %edx
	movl	$16, %r8d
	movq	48(%r15), %r11
	testq	%r11, %r11
	jne	.LMSGCAllocSlow2
	movq	88(%r15), %r11
	testq	%r11, %r11
	je	.LMSGCAllocSlow2
	movq	24(%r11), %r11
	testq	%r11, %r11
	je	.LMSGCAllocSlow2
	movzbl	60(%r11), %r10d
	cmpl	$1, %r10d
	je	.LMSGCAllocBump2
	cmpl	$2, %r10d
	jne	.LMSGCAllocSlow2
.LMSGCAllocBitmapRetry2:
	movq	40(%r11), %r10
	testq	%r10, %r10
	je	.LMSGCAllocSlow2
	bsfq	%r10, %r9
	movq	24(%r11), %rax
	movq	(%rax,%r9,8), %r10
	testq	%r10, %r10
	je	.LMSGCAllocBitmapRepair2
	bsfq	%r10, %r8
	btrq	%r8, %r10
	movq	%r10, (%rax,%r9,8)
	testq	%r10, %r10
	jne	.LMSGCAllocBitmapSummaryDone2
	movq	40(%r11), %rax
	btrq	%r9, %rax
	movq	%rax, 40(%r11)
.LMSGCAllocBitmapSummaryDone2:
	shlq	$6, %r9
	addq	%r8, %r9
	shlq	$4, %r9
	movq	8(%r11), %rax
	subq	$32768, %rax                    # imm = 0x8000
	addq	%r9, %rax
	movq	%rcx, (%rax)
	movq	%rdx, 8(%rax)
	jmp	.LMSGCAllocFin2
.LMSGCAllocBitmapRepair2:
	movq	40(%r11), %r10
	btrq	%r9, %r10
	movq	%r10, 40(%r11)
	jmp	.LMSGCAllocBitmapRetry2
.LMSGCAllocBump2:
	movq	(%r11), %r10
	leaq	16(%r10), %rax
	cmpq	8(%r11), %rax
	ja	.LMSGCAllocSlow2
	movq	%rcx, (%r10)
	movq	%rdx, 8(%r10)
	movq	%rax, (%r11)
	movq	%r10, %rax
	jmp	.LMSGCAllocFin2
.LMSGCAllocSlow2:
	callq	CJ_MCC_NewObjArray_MSGC_cls1_sz16_sc1
.LMSGCAllocFin2:
.Ltmp38:
	movq	%rax, %rcx
	movq	-16(%rbp), %rax
	movq	%rcx, (%rax)
	movq	$0, 16(%rax)
.L_epilogue5:
	movq	%rbp, %rsp
	subq	$48, %rsp
	addq	$48, %rsp
	popq	%rbp
	retq
.Lfunc_end6:
	.seh_handlerdata
	.text
	.seh_endproc
	.section	.xdata,"dw"
	.p2align	2
CJ_except_table7:
.Lexception6:
	.long	1431655765                      # No callsite function EH table header
	.text
                                        # -- End function
	.def	_CNat5ArrayICN7default1AEE7isLegalHll;
	.scl	3;
	.type	32;
	.endef
	.p2align	4, 0x90                         # -- Begin function _CNat5ArrayICN7default1AEE7isLegalHll
_CNat5ArrayICN7default1AEE7isLegalHll:  # @_CNat5ArrayICN7default1AEE7isLegalHll
.Lfunc_begin7:
.seh_proc _CNat5ArrayICN7default1AEE7isLegalHll
	.seh_handler .L__cj_personality_v0$, @unwind, @except
# %bb.0:                                # %allocas
	subq	$48, %rsp
	.seh_stackalloc 48
	.seh_endprologue
	movq	%r8, (%rsp)                     # 8-byte Spill
	movq	%rdx, 8(%rsp)                   # 8-byte Spill
	movq	%rcx, 16(%rsp)                  # 8-byte Spill
# %bb.1:                                # %bb0
	movq	8(%rsp), %rcx                   # 8-byte Reload
	xorl	%eax, %eax
                                        # kill: def $rax killed $eax
	cmpq	%rcx, %rax
	jg	.LBB8_7
# %bb.2:                                # %bb2
	movq	8(%rsp), %rax                   # 8-byte Reload
	movq	(%rsp), %rcx                    # 8-byte Reload
	cmpq	%rcx, %rax
	setle	%al
	andb	$1, %al
	movb	%al, 32(%rsp)
.LBB8_3:                                # %bb4
	testb	$1, 32(%rsp)
	jne	.LBB8_4
	jmp	.LBB8_6
.LBB8_4:                                # %bb5
	movq	(%rsp), %rax                    # 8-byte Reload
	movq	16(%rsp), %rcx                  # 8-byte Reload
	cmpq	16(%rcx), %rax
	setle	%al
	andb	$1, %al
	movb	%al, 24(%rsp)
.LBB8_5:                                # %bb7
	movb	24(%rsp), %al
	movb	%al, 40(%rsp)
	movb	40(%rsp), %al
.L_epilogue6:
	addq	$48, %rsp
	retq
.LBB8_6:                                # %bb6
	movb	$0, 24(%rsp)
	jmp	.LBB8_5
.LBB8_7:                                # %bb3
	movb	$0, 32(%rsp)
	jmp	.LBB8_3
.Lfunc_end7:
	.seh_handlerdata
	.text
	.seh_endproc
	.section	.xdata,"dw"
	.p2align	2
CJ_except_table8:
.Lexception7:
	.long	1431655765                      # No callsite function EH table header
	.text
                                        # -- End function
	.def	_CNat5ArrayICN7default1AEE9normalizeHRNat5RangeIlE;
	.scl	3;
	.type	32;
	.endef
	.p2align	4, 0x90                         # -- Begin function _CNat5ArrayICN7default1AEE9normalizeHRNat5RangeIlE
.Ltmp39:                                # @_CNat5ArrayICN7default1AEE9normalizeHRNat5RangeIlE
	.long	.Lmethod_desc._CNat5ArrayICN7default1AEE9normalizeHRNat5RangeIlE-.Ltmp39
_CNat5ArrayICN7default1AEE9normalizeHRNat5RangeIlE:
.Lfunc_begin8:
.seh_proc _CNat5ArrayICN7default1AEE9normalizeHRNat5RangeIlE
	.seh_handler .L__cj_personality_v0$, @unwind, @except
# %bb.0:                                # %allocas
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$112, %rsp
	.seh_stackalloc 112
	leaq	112(%rsp), %rbp
	.seh_setframe %rbp, 112
	.seh_endprologue
	movq	%r8, -96(%rbp)                  # 8-byte Spill
	movq	%rdx, -88(%rbp)                 # 8-byte Spill
	movq	%rcx, -80(%rbp)                 # 8-byte Spill
	movq	%rcx, -72(%rbp)                 # 8-byte Spill
# %bb.1:                                # %bb0
	movq	-96(%rbp), %rax                 # 8-byte Reload
	movb	24(%rax), %al
	movb	%al, -97(%rbp)                  # 1-byte Spill
	callq	.LCJ_Safepoint_Stub
.Ltmp40:
	movb	-97(%rbp), %al                  # 1-byte Reload
	testb	$1, %al
	jne	.LBB9_2
	jmp	.LBB9_15
.LBB9_2:                                # %bb3
	movq	-96(%rbp), %rax                 # 8-byte Reload
	movq	(%rax), %rax
	movq	%rax, -16(%rbp)
.LBB9_3:                                # %bb2
	movq	-96(%rbp), %rax                 # 8-byte Reload
	movq	-16(%rbp), %rcx
	movq	%rcx, -8(%rbp)
	testb	$1, 25(%rax)
	jne	.LBB9_4
	jmp	.LBB9_14
.LBB9_4:                                # %bb6
	movq	-96(%rbp), %rax                 # 8-byte Reload
	movq	8(%rax), %rax
	movq	%rax, -32(%rbp)
.LBB9_5:                                # %bb5
	movq	-96(%rbp), %rax                 # 8-byte Reload
	movq	-32(%rbp), %rcx
	movq	%rcx, -24(%rbp)
	testb	$1, 26(%rax)
	jne	.LBB9_6
	jmp	.LBB9_13
.LBB9_6:                                # %bb8
	movq	-96(%rbp), %rax                 # 8-byte Reload
	movb	25(%rax), %al
	andb	$1, %al
	movb	%al, -40(%rbp)
.LBB9_7:                                # %bb10
	testb	$1, -40(%rbp)
	jne	.LBB9_8
	jmp	.LBB9_11
.LBB9_8:                                # %bb12
	movq	-96(%rbp), %rax                 # 8-byte Reload
	movq	-24(%rbp), %rcx
	movq	%rcx, -112(%rbp)                # 8-byte Spill
	cmpq	$0, 16(%rax)
	jle	.LBB9_12
# %bb.9:                                # %bb14
	movq	$1, -48(%rbp)
.LBB9_10:                               # %bb13
	movq	-112(%rbp), %rax                # 8-byte Reload
	addq	-48(%rbp), %rax
	movq	%rax, -24(%rbp)
.LBB9_11:                               # %bb11
	movq	-72(%rbp), %rax                 # 8-byte Reload
	movq	-80(%rbp), %rcx                 # 8-byte Reload
	movq	-8(%rbp), %r8
	movq	-24(%rbp), %rdx
	movq	%r8, -64(%rbp)
	movq	%rdx, -56(%rbp)
	movq	-64(%rbp), %rdx
	movq	%rdx, (%rcx)
	movq	-56(%rbp), %rdx
	movq	%rdx, 8(%rcx)
.L_epilogue7:
	addq	$112, %rsp
	popq	%rbp
	retq
.LBB9_12:                               # %bb15
	movq	$-1, -48(%rbp)
	jmp	.LBB9_10
.LBB9_13:                               # %bb9
	movb	$0, -40(%rbp)
	jmp	.LBB9_7
.LBB9_14:                               # %bb7
	movq	-88(%rbp), %rax                 # 8-byte Reload
	movq	16(%rax), %rax
	movq	%rax, -32(%rbp)
	jmp	.LBB9_5
.LBB9_15:                               # %bb4
	movq	$0, -16(%rbp)
	jmp	.LBB9_3
.Lfunc_end8:
	.seh_handlerdata
	.text
	.seh_endproc
	.section	.xdata,"dw"
	.p2align	2
CJ_except_table9:
.Lexception8:
	.long	1431655765                      # No callsite function EH table header
	.text
                                        # -- End function
	.def	_CNat5RangeIlE4lastHv;
	.scl	3;
	.type	32;
	.endef
	.p2align	4, 0x90                         # -- Begin function _CNat5RangeIlE4lastHv
.Ltmp41:                                # @_CNat5RangeIlE4lastHv
	.long	.Lmethod_desc._CNat5RangeIlE4lastHv-.Ltmp41
_CNat5RangeIlE4lastHv:
.Lfunc_begin9:
.seh_proc _CNat5RangeIlE4lastHv
	.seh_handler .L__cj_personality_v0$, @unwind, @except
# %bb.0:                                # %allocas
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$240, %rsp
	movq	40(%r15), %rax
	cmpq	%rax, %rsp
	jbe	.Lstack.overflow5
.Lstack.check.end5:
	.seh_stackalloc 240
	leaq	128(%rsp), %rbp
	.seh_setframe %rbp, 128
	.seh_endprologue
	movq	%rcx, -32(%rbp)                 # 8-byte Spill
# %bb.1:                                # %bb0
	movq	-32(%rbp), %rax                 # 8-byte Reload
	movq	16(%rax), %rax
	movq	%rax, -40(%rbp)                 # 8-byte Spill
	callq	.LCJ_Safepoint_Stub
.Ltmp42:
	movq	-40(%rbp), %rax                 # 8-byte Reload
	cmpq	$1, %rax
	je	.LBB10_17
# %bb.2:                                # %bb2
	movq	-32(%rbp), %rax                 # 8-byte Reload
	cmpq	$-1, 16(%rax)
	setne	%al
	andb	$1, %al
	movb	%al, 96(%rbp)
.LBB10_3:                               # %bb4
	testb	$1, 96(%rbp)
	jne	.LBB10_4
	jmp	.LBB10_12
.LBB10_4:                               # %bb6
	movq	-32(%rbp), %rax                 # 8-byte Reload
	cmpq	$0, 16(%rax)
	jle	.LBB10_16
# %bb.5:                                # %bb8
	movq	-32(%rbp), %rax                 # 8-byte Reload
	movq	(%rax), %rdx
	movq	8(%rax), %rcx
	movq	16(%rax), %rax
	movq	%rdx, 48(%rbp)
	movq	%rcx, 56(%rbp)
	movq	%rax, 64(%rbp)
	movq	48(%rbp), %rax
	movq	%rax, 72(%rbp)
	movq	56(%rbp), %rax
	movq	%rax, 80(%rbp)
	movq	64(%rbp), %rax
	movq	%rax, 88(%rbp)
.LBB10_6:                               # %bb7
	movq	88(%rbp), %rax
	movq	%rax, 32(%rbp)
	movups	72(%rbp), %xmm0
	movaps	%xmm0, 16(%rbp)
	movq	16(%rbp), %rax
	movq	%rax, -80(%rbp)                 # 8-byte Spill
	movq	24(%rbp), %rcx
	movq	32(%rbp), %rax
	movq	%rax, -56(%rbp)                 # 8-byte Spill
	movq	.refptr.Int64.ti(%rip), %rdx
	movq	%rdx, -72(%rbp)                 # 8-byte Spill
	callq	_CNatXl8positionHv
.Ltmp43:
	movq	-80(%rbp), %rcx                 # 8-byte Reload
	movq	-72(%rbp), %rdx                 # 8-byte Reload
	movq	%rax, -64(%rbp)                 # 8-byte Spill
	callq	_CNatXl8positionHv
.Ltmp44:
	movq	-64(%rbp), %rcx                 # 8-byte Reload
	movq	%rax, %rdx
	movq	-56(%rbp), %rax                 # 8-byte Reload
	subq	%rdx, %rcx
	movq	%rcx, -48(%rbp)                 # 8-byte Spill
	cmpq	$0, %rax
	jne	.LBB10_8
# %bb.7:                                # %divisorIs0
	leaq	".L$const_cjstring.c-J6RJj+YQj"(%rip), %rcx
	callq	rt$CreateArithmeticException_msg
.Ltmp45:
	movq	%rax, %rcx
	movq	%rcx, -24(%rbp)
	callq	CJ_MCC_ThrowException
.LBB10_8:                               # %divisorIsNot0
	movq	-56(%rbp), %rcx                 # 8-byte Reload
	movq	-48(%rbp), %rax                 # 8-byte Reload
	xorl	%edx, %edx
                                        # kill: def $rdx killed $edx
	divq	%rcx
	movq	%rdx, -88(%rbp)                 # 8-byte Spill
	cmpq	$0, %rdx
	je	.LBB10_12
# %bb.9:                                # %bb11
	movq	-32(%rbp), %rax                 # 8-byte Reload
	cmpq	$0, 16(%rax)
	jle	.LBB10_11
# %bb.10:                               # %bb13
	movq	-88(%rbp), %rdx                 # 8-byte Reload
	movq	-32(%rbp), %rax                 # 8-byte Reload
	movq	8(%rax), %rcx
	negq	%rdx
	movq	.refptr.Int64.ti(%rip), %r8
	callq	_CNatXl4nextHl
.Ltmp46:
	movq	%rax, 104(%rbp)
	movq	104(%rbp), %rax
.L_epilogue8:
	movq	%rbp, %rsp
	subq	$128, %rsp
	addq	$240, %rsp
	popq	%rbp
	retq
.LBB10_11:                              # %bb15
	movq	-88(%rbp), %rdx                 # 8-byte Reload
	movq	-32(%rbp), %rax                 # 8-byte Reload
	movq	8(%rax), %rcx
	movq	.refptr.Int64.ti(%rip), %r8
	callq	_CNatXl4nextHl
.Ltmp47:
	movq	%rax, 104(%rbp)
	movq	104(%rbp), %rax
	addq	$240, %rsp
	popq	%rbp
	retq
.LBB10_12:                              # %bb5
	movq	-32(%rbp), %rax                 # 8-byte Reload
	testb	$1, 26(%rax)
	jne	.LBB10_13
	jmp	.LBB10_15
.LBB10_13:                              # %bb18
	movq	-32(%rbp), %rax                 # 8-byte Reload
	movq	8(%rax), %rax
	movq	%rax, 8(%rbp)
.LBB10_14:                              # %bb17
	movq	8(%rbp), %rax
	movq	%rax, 104(%rbp)
	movq	104(%rbp), %rax
	addq	$240, %rsp
	popq	%rbp
	retq
.LBB10_15:                              # %bb19
	movq	-32(%rbp), %rax                 # 8-byte Reload
	movq	8(%rax), %rcx
	movq	16(%rax), %rdx
	negq	%rdx
	movq	.refptr.Int64.ti(%rip), %r8
	callq	_CNatXl4nextHl
.Ltmp48:
	movq	%rax, 8(%rbp)
	jmp	.LBB10_14
.LBB10_16:                              # %bb9
	movq	-32(%rbp), %r8                  # 8-byte Reload
	movq	8(%r8), %rdx
	movq	(%r8), %rcx
	xorl	%eax, %eax
                                        # kill: def $rax killed $eax
	subq	16(%r8), %rax
	movq	%rdx, -16(%rbp)
	movq	%rcx, -8(%rbp)
	movq	%rax, (%rbp)
	movq	-16(%rbp), %rax
	movq	%rax, 72(%rbp)
	movq	-8(%rbp), %rax
	movq	%rax, 80(%rbp)
	movq	(%rbp), %rax
	movq	%rax, 88(%rbp)
	jmp	.LBB10_6
.LBB10_17:                              # %bb3
	movb	$0, 96(%rbp)
	jmp	.LBB10_3
.Lstack.overflow5:
	addq	$240, %rsp
	movabsq	$16, %rax
	callq	CJ_MCC_StackGrowStub
.Ltmp49:
	subq	$240, %rsp
	jmp	.Lstack.check.end5
.Lfunc_end9:
	.seh_handlerdata
	.text
	.seh_endproc
	.section	.xdata,"dw"
	.p2align	2
CJ_except_table10:
.Lexception9:
	.long	1431655765                      # No callsite function EH table header
	.text
                                        # -- End function
	.def	"_CNat5RangeIlE6<init>Hlllbbb";
	.scl	3;
	.type	32;
	.endef
	.p2align	4, 0x90                         # -- Begin function _CNat5RangeIlE6<init>Hlllbbb
.Ltmp50:                                # @"_CNat5RangeIlE6<init>Hlllbbb"
	.long	".Lmethod_desc._CNat5RangeIlE6<init>Hlllbbb"-.Ltmp50
"_CNat5RangeIlE6<init>Hlllbbb":
.Lfunc_begin10:
.seh_proc "_CNat5RangeIlE6<init>Hlllbbb"
	.seh_handler .L__cj_personality_v0$, @unwind, @except
# %bb.0:                                # %bb0
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$96, %rsp
	movq	40(%r15), %rax
	cmpq	%rax, %rsp
	jbe	.Lstack.overflow6
.Lstack.check.end6:
	.seh_stackalloc 96
	leaq	96(%rsp), %rbp
	.seh_setframe %rbp, 96
	.seh_endprologue
	movq	%r9, -56(%rbp)                  # 8-byte Spill
	movq	%r8, -48(%rbp)                  # 8-byte Spill
	movb	72(%rbp), %al
	movb	%al, -35(%rbp)                  # 1-byte Spill
	movb	64(%rbp), %al
	movb	%al, -34(%rbp)                  # 1-byte Spill
	movb	56(%rbp), %al
	movb	%al, -33(%rbp)                  # 1-byte Spill
	movq	48(%rbp), %rax
	movq	%rax, -32(%rbp)                 # 8-byte Spill
	movq	%rdx, -8(%rbp)
	movq	%rcx, -16(%rbp)
	callq	.LCJ_Safepoint_Stub
.Ltmp51:
	movq	-32(%rbp), %rax                 # 8-byte Reload
	movq	-16(%rbp), %rcx
	movq	%rcx, -24(%rbp)                 # 8-byte Spill
	cmpq	$0, %rax
	jne	.LBB11_2
# %bb.1:                                # %bb4
	movq	".refptr.std.core:IllegalArgumentException.ti"(%rip), %rcx
	movq	%rcx, -64(%rbp)                 # 8-byte Spill
	movl	$64, %edx
	movq	48(%r15), %r11
	testq	%r11, %r11
	jne	.LMSGCAllocSlow3
	movq	88(%r15), %r11
	testq	%r11, %r11
	je	.LMSGCAllocSlow3
	movq	88(%r11), %r11
	testq	%r11, %r11
	je	.LMSGCAllocSlow3
	movzbl	60(%r11), %r10d
	cmpl	$1, %r10d
	je	.LMSGCAllocBump3
	cmpl	$2, %r10d
	jne	.LMSGCAllocSlow3
.LMSGCAllocBitmapRetry3:
	movq	40(%r11), %r10
	testq	%r10, %r10
	je	.LMSGCAllocSlow3
	bsfq	%r10, %r9
	movq	24(%r11), %rax
	movq	(%rax,%r9,8), %r10
	testq	%r10, %r10
	je	.LMSGCAllocBitmapRepair3
	bsfq	%r10, %rdx
	btrq	%rdx, %r10
	movq	%r10, (%rax,%r9,8)
	testq	%r10, %r10
	jne	.LMSGCAllocBitmapSummaryDone3
	movq	40(%r11), %rax
	btrq	%r9, %rax
	movq	%rax, 40(%r11)
.LMSGCAllocBitmapSummaryDone3:
	shlq	$6, %r9
	addq	%rdx, %r9
	shlq	$6, %r9
	movq	8(%r11), %rax
	subq	$32768, %rax                    # imm = 0x8000
	addq	%r9, %rax
	movq	%rcx, (%rax)
	jmp	.LMSGCAllocFin3
.LMSGCAllocBitmapRepair3:
	movq	40(%r11), %r10
	btrq	%r9, %r10
	movq	%r10, 40(%r11)
	jmp	.LMSGCAllocBitmapRetry3
.LMSGCAllocBump3:
	movq	(%r11), %r10
	leaq	64(%r10), %rax
	cmpq	8(%r11), %rax
	ja	.LMSGCAllocSlow3
	movq	%rcx, (%r10)
	movq	%rax, (%r11)
	movq	%r10, %rax
	jmp	.LMSGCAllocFin3
.LMSGCAllocSlow3:
	callq	CJ_MCC_NewObjectFast_MSGC_cls5_sz64_sc1
.LMSGCAllocFin3:
.Ltmp52:
	movq	-64(%rbp), %r8                  # 8-byte Reload
	movq	%rax, %rcx
	movq	%rcx, -8(%rbp)
	leaq	.L$const_cjstring.daAk2A5RJ4U(%rip), %rdx
	callq	"_CNat24IllegalArgumentException6<init>HRNat6StringE"
.Ltmp53:
	movq	-8(%rbp), %rcx
	callq	CJ_MCC_ThrowException
.LBB11_2:                               # %bb3
	movq	-24(%rbp), %rax                 # 8-byte Reload
	movb	-35(%rbp), %cl                  # 1-byte Reload
	movb	-34(%rbp), %dl                  # 1-byte Reload
	movb	-33(%rbp), %r8b                 # 1-byte Reload
	movq	-32(%rbp), %r9                  # 8-byte Reload
	movq	-56(%rbp), %r10                 # 8-byte Reload
	movq	-48(%rbp), %r11                 # 8-byte Reload
	movq	%r11, (%rax)
	movq	%r10, 8(%rax)
	movq	%r9, 16(%rax)
	andb	$1, %r8b
	movb	%r8b, 24(%rax)
	andb	$1, %dl
	movb	%dl, 25(%rax)
	andb	$1, %cl
	movb	%cl, 26(%rax)
.L_epilogue9:
	movq	%rbp, %rsp
	subq	$96, %rsp
	addq	$96, %rsp
	popq	%rbp
	retq
.Lstack.overflow6:
	addq	$96, %rsp
	movabsq	$16, %rax
	callq	CJ_MCC_StackGrowStub
.Ltmp54:
	subq	$96, %rsp
	jmp	.Lstack.check.end6
.Lfunc_end10:
	.seh_handlerdata
	.text
	.seh_endproc
	.section	.xdata,"dw"
	.p2align	2
CJ_except_table11:
.Lexception10:
	.long	1431655765                      # No callsite function EH table header
	.text
                                        # -- End function
	.def	_CNat5RangeIlE7isEmptyHv;
	.scl	3;
	.type	32;
	.endef
	.p2align	4, 0x90                         # -- Begin function _CNat5RangeIlE7isEmptyHv
_CNat5RangeIlE7isEmptyHv:               # @_CNat5RangeIlE7isEmptyHv
.Lfunc_begin11:
.seh_proc _CNat5RangeIlE7isEmptyHv
	.seh_handler .L__cj_personality_v0$, @unwind, @except
# %bb.0:                                # %allocas
	subq	$16, %rsp
	.seh_stackalloc 16
	.seh_endprologue
	movq	%rcx, (%rsp)                    # 8-byte Spill
# %bb.1:                                # %bb0
	movq	(%rsp), %rax                    # 8-byte Reload
	testb	$1, 26(%rax)
	jne	.LBB12_2
	jmp	.LBB12_5
.LBB12_2:                               # %bb3
	movq	(%rsp), %rax                    # 8-byte Reload
	cmpq	$0, 16(%rax)
	jle	.LBB12_4
# %bb.3:                                # %bb5
	movq	(%rsp), %rcx                    # 8-byte Reload
	movq	(%rcx), %rax
	movq	8(%rcx), %rcx
	subq	%rcx, %rax
	setg	8(%rsp)
	movb	8(%rsp), %al
.L_epilogue10:
	addq	$16, %rsp
	retq
.LBB12_4:                               # %bb4
	movq	(%rsp), %rcx                    # 8-byte Reload
	movq	(%rcx), %rax
	movq	8(%rcx), %rcx
	subq	%rcx, %rax
	setl	8(%rsp)
	movb	8(%rsp), %al
	addq	$16, %rsp
	retq
.LBB12_5:                               # %bb8
	movq	(%rsp), %rax                    # 8-byte Reload
	cmpq	$0, 16(%rax)
	jle	.LBB12_7
# %bb.6:                                # %bb10
	movq	(%rsp), %rcx                    # 8-byte Reload
	movq	(%rcx), %rax
	movq	8(%rcx), %rcx
	subq	%rcx, %rax
	setge	8(%rsp)
	movb	8(%rsp), %al
	addq	$16, %rsp
	retq
.LBB12_7:                               # %bb9
	movq	(%rsp), %rcx                    # 8-byte Reload
	movq	(%rcx), %rax
	movq	8(%rcx), %rcx
	subq	%rcx, %rax
	setle	8(%rsp)
	movb	8(%rsp), %al
	addq	$16, %rsp
	retq
.Lfunc_end11:
	.seh_handlerdata
	.text
	.seh_endproc
	.section	.xdata,"dw"
	.p2align	2
CJ_except_table12:
.Lexception11:
	.long	1431655765                      # No callsite function EH table header
	.text
                                        # -- End function
	.def	user.main;
	.scl	2;
	.type	32;
	.endef
	.section	.cjinit_function,"xr"
	.globl	user.main                       # -- Begin function user.main
	.p2align	4, 0x90
.Ltmp55:                                # @user.main
	.long	.Lmethod_desc.user.main-.Ltmp55
user.main:
.Lfunc_begin12:
.seh_proc user.main
	.seh_handler .L__cj_personality_v0$, @unwind, @except
# %bb.0:                                # %allocas
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$48, %rsp
	movq	40(%r15), %rax
	cmpq	%rax, %rsp
	jbe	.Lstack.overflow7
.Lstack.check.end7:
	.seh_stackalloc 48
	leaq	48(%rsp), %rbp
	.seh_setframe %rbp, 48
	.seh_endprologue
# %bb.1:                                # %bb0
	callq	.LCJ_Safepoint_Stub
.Ltmp56:
	callq	"_CN7default6<main>Hv"
.Ltmp57:
	movq	%rax, -8(%rbp)
	movq	-8(%rbp), %rax
.L_epilogue11:
	movq	%rbp, %rsp
	subq	$48, %rsp
	addq	$48, %rsp
	popq	%rbp
	retq
.Lstack.overflow7:
	addq	$48, %rsp
	movabsq	$16, %rax
	callq	CJ_MCC_StackGrowStub
.Ltmp58:
	subq	$48, %rsp
	jmp	.Lstack.check.end7
.Lfunc_end12:
	.seh_handlerdata
	.section	.cjinit_function,"xr"
	.seh_endproc
	.section	.xdata,"dw"
	.p2align	2
CJ_except_table13:
.Lexception12:
	.long	1431655765                      # No callsite function EH table header
	.section	.cjinit_function,"xr"
                                        # -- End function
	.def	main;
	.scl	2;
	.type	32;
	.endef
	.globl	main                            # -- Begin function main
	.p2align	4, 0x90
main:                                   # @main
.seh_proc main
# %bb.0:                                # %entry
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$48, %rsp
	.seh_stackalloc 48
	leaq	48(%rsp), %rbp
	.seh_setframe %rbp, 48
	.seh_endprologue
	movq	%rdx, -8(%rbp)                  # 8-byte Spill
	movl	%ecx, -12(%rbp)                 # 4-byte Spill
	callq	__main
	callq	CJ_MRT_CjRuntimeInit
	movl	-12(%rbp), %ecx                 # 4-byte Reload
	movq	-8(%rbp), %rdx                  # 8-byte Reload
	callq	CJ_MRT_SetCommandLineArgs
	leaq	cj_entry$(%rip), %rcx
	callq	CJ_MRT_CjRuntimeStart
	nop
	addq	$48, %rsp
	popq	%rbp
	retq
	.seh_endproc
                                        # -- End function
	.def	cj_entry$;
	.scl	2;
	.type	32;
	.endef
	.globl	cj_entry$                       # -- Begin function cj_entry$
	.p2align	4, 0x90
.Ltmp80:                                # @"cj_entry$"
	.long	.Lmethod_desc.cj_entry$-.Ltmp80
cj_entry$:
.Lfunc_begin13:
.seh_proc cj_entry$
	.seh_handler .L__cj_personality_v0$, @unwind, @except
# %bb.0:                                # %allocas
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$144, %rsp
	movq	40(%r15), %rax
	cmpq	%rax, %rsp
	jbe	.Lstack.overflow8
.Lstack.check.end8:
	.seh_stackalloc 144
	leaq	128(%rsp), %rbp
	.seh_setframe %rbp, 128
	.seh_endprologue
# %bb.1:                                # %entry
	callq	.LCJ_Safepoint_Stub
.Ltmp81:
	callq	CJ_MCC_CheckThreadLocalDataOffset
.Ltmp82:
.Ltmp59:
	callq	_CGP7defaultiiHv
.Ltmp83:
.Ltmp60:
	jmp	.LBB15_2
.LBB15_2:                               # %normalDest
.Ltmp62:
	callq	user.main
.Ltmp84:
.Ltmp63:
	movq	%rax, -56(%rbp)                 # 8-byte Spill
	jmp	.LBB15_3
.LBB15_3:                               # %normalDest1
	movq	-56(%rbp), %rax                 # 8-byte Reload
	movq	%rax, -64(%rbp)                 # 8-byte Spill
.Ltmp65:
	leaq	8(%rbp), %rcx
	callq	_CNat27CJ_CORE_ExecAtexitCallbacksHv
.Ltmp85:
.Ltmp66:
	jmp	.LBB15_4
.LBB15_4:                               # %normalDest2
	movq	-64(%rbp), %rax                 # 8-byte Reload
	cmpq	$-2147483648, %rax              # imm = 0x80000000
	setl	%cl
	movb	%cl, -73(%rbp)                  # 1-byte Spill
	cmpq	$2147483647, %rax               # imm = 0x7FFFFFFF
	setg	%dl
	orb	%dl, %cl
	testb	$1, %cl
	movq	%rax, -72(%rbp)                 # 8-byte Spill
	jne	.LBB15_5
	jmp	.LBB15_7
.LBB15_5:                               # %min.bb
	movb	-73(%rbp), %cl                  # 1-byte Reload
	movq	$-2147483648, %rax              # imm = 0x80000000
	testb	$1, %cl
	movq	%rax, -72(%rbp)                 # 8-byte Spill
	jne	.LBB15_7
	jmp	.LBB15_6
.LBB15_6:                               # %max.bb
	movl	$2147483647, %eax               # imm = 0x7FFFFFFF
	movq	%rax, -72(%rbp)                 # 8-byte Spill
	jmp	.LBB15_7
.LBB15_7:                               # %check.over
	movq	-72(%rbp), %rax                 # 8-byte Reload
                                        # kill: def $eax killed $eax killed $rax
.L_epilogue12:
	movq	%rbp, %rsp
	subq	$128, %rsp
	addq	$144, %rsp
	popq	%rbp
	retq
.LBB15_8:                               # %unwindDest12
.Ltmp67:
	jmp	.LBB15_12
.LBB15_9:                               # %unwindDest1316
.Ltmp64:
	jmp	.LBB15_11
.LBB15_10:                              # %unwindDest1317
.Ltmp61:
.LBB15_11:                              # %unwindDest13
	jmp	.LBB15_12
.LBB15_12:                              # %unwindDest
	callq	CJ_MCC_GetExceptionWrapper
	movq	%rax, %rcx
	callq	CJ_MCC_PostThrowException
	movq	%rax, -88(%rbp)                 # 8-byte Spill
	movq	(%rax), %rcx
	movq	".refptr.std.core:OutOfMemoryError.ti"(%rip), %rdx
	callq	CJ_MCC_IsSubType
	testb	$1, %al
	jne	.LBB15_13
	jmp	.LBB15_16
.LBB15_13:                              # %is.OOM
.Ltmp74:
	leaq	.L$const_cjstring.eOeYIhisqu(%rip), %rdx
	movq	%rbp, %rcx
	callq	_CNat8eprintlnHRNat6StringE
.Ltmp86:
.Ltmp75:
	jmp	.LBB15_14
.LBB15_14:                              # %normalDest3
.Ltmp77:
	leaq	.L$const_cjstring.5CeEPuGykeB(%rip), %rdx
	leaq	-8(%rbp), %rcx
	callq	_CNat8eprintlnHRNat6StringE
.Ltmp87:
.Ltmp78:
	jmp	.LBB15_15
.LBB15_15:                              # %normalDest4
	jmp	.LBB15_22
.LBB15_16:                              # %judge.Error
	movq	-88(%rbp), %rax                 # 8-byte Reload
	movq	(%rax), %rcx
	movq	".refptr.std.core:Error.ti"(%rip), %rdx
	callq	CJ_MCC_IsSubType
	testb	$1, %al
	jne	.LBB15_17
	jmp	.LBB15_19
.LBB15_17:                              # %is.Error
	movq	-88(%rbp), %rax                 # 8-byte Reload
	movq	(%rax), %rcx
	movq	72(%rcx), %rax
	movq	8(%rax), %r8
	movq	%r8, -96(%rbp)                  # 8-byte Spill
	movq	".refptr.std.core:Error.ti"(%rip), %rdx
	callq	CJ_MCC_UpdateVMT
	movq	-96(%rbp), %rax                 # 8-byte Reload
	movq	-88(%rbp), %rdx                 # 8-byte Reload
	movq	32(%rax), %rax
	movq	16(%rax), %rax
	movq	%rdx, -48(%rbp)
.Ltmp71:
	leaq	-16(%rbp), %rcx
	callq	*%rax
.Ltmp88:
.Ltmp72:
	jmp	.LBB15_18
.LBB15_18:                              # %normalDest5
	jmp	.LBB15_22
.LBB15_19:                              # %judge.Exception
	movq	-88(%rbp), %rax                 # 8-byte Reload
	movq	(%rax), %rcx
	movq	".refptr.std.core:Exception.ti"(%rip), %rdx
	callq	CJ_MCC_IsSubType
	testb	$1, %al
	jne	.LBB15_20
	jmp	.LBB15_22
.LBB15_20:                              # %is.Exception
	movq	-88(%rbp), %rdx                 # 8-byte Reload
	movq	%rdx, -48(%rbp)
.Ltmp68:
	leaq	-24(%rbp), %rcx
	callq	_CNat15handleExceptionHCNat9ExceptionE
.Ltmp89:
.Ltmp69:
	jmp	.LBB15_21
.LBB15_21:                              # %normalDest6
.LBB15_22:                              # %judge.end
	leaq	-32(%rbp), %rcx
	callq	_CNat27CJ_CORE_ExecAtexitCallbacksHv
.Ltmp90:
	movl	$1, %eax
	addq	$144, %rsp
	popq	%rbp
	retq
.LBB15_23:                              # %rethrow1
.Ltmp70:
	jmp	.LBB15_29
.LBB15_24:                              # %rethrow24
.Ltmp73:
	jmp	.LBB15_28
.LBB15_25:                              # %rethrow258
.Ltmp79:
	jmp	.LBB15_27
.LBB15_26:                              # %rethrow259
.Ltmp76:
.LBB15_27:                              # %rethrow25
	jmp	.LBB15_28
.LBB15_28:                              # %rethrow2
	jmp	.LBB15_29
.LBB15_29:                              # %rethrow
	leaq	-40(%rbp), %rcx
	callq	_CNat27CJ_CORE_ExecAtexitCallbacksHv
.Ltmp91:
	movl	$1, %eax
	addq	$144, %rsp
	popq	%rbp
	retq
.Lstack.overflow8:
	addq	$144, %rsp
	movabsq	$16, %rax
	callq	CJ_MCC_StackGrowStub
.Ltmp92:
	subq	$144, %rsp
	jmp	.Lstack.check.end8
.Lfunc_end13:
	.seh_handlerdata
	.section	.cjinit_function,"xr"
	.seh_endproc
	.section	.xdata,"dw"
	.p2align	2
CJ_except_table15:
.Lexception13:
	.byte	255                             # @LPStart Encoding = omit
	.byte	0                               # @TType Encoding = absptr
	.uleb128 .Lttbase1-.Lttbaseref1
.Lttbaseref1:
	.byte	1                               # Call site Encoding = uleb128
	.uleb128 .Lcst_end13-.Lcst_begin13
.Lcst_begin13:
	.uleb128 .Ltmp59-.Lfunc_begin13         # >> Call Site 1 <<
	.uleb128 .Ltmp60-.Ltmp59                #   Call between .Ltmp59 and .Ltmp60
	.uleb128 .Ltmp61-.Lfunc_begin13         #     jumps to .Ltmp61
	.byte	1                               #   On action: 1
	.uleb128 .Ltmp62-.Lfunc_begin13         # >> Call Site 2 <<
	.uleb128 .Ltmp63-.Ltmp62                #   Call between .Ltmp62 and .Ltmp63
	.uleb128 .Ltmp64-.Lfunc_begin13         #     jumps to .Ltmp64
	.byte	1                               #   On action: 1
	.uleb128 .Ltmp65-.Lfunc_begin13         # >> Call Site 3 <<
	.uleb128 .Ltmp66-.Ltmp65                #   Call between .Ltmp65 and .Ltmp66
	.uleb128 .Ltmp67-.Lfunc_begin13         #     jumps to .Ltmp67
	.byte	1                               #   On action: 1
	.uleb128 .Ltmp74-.Lfunc_begin13         # >> Call Site 4 <<
	.uleb128 .Ltmp75-.Ltmp74                #   Call between .Ltmp74 and .Ltmp75
	.uleb128 .Ltmp76-.Lfunc_begin13         #     jumps to .Ltmp76
	.byte	1                               #   On action: 1
	.uleb128 .Ltmp77-.Lfunc_begin13         # >> Call Site 5 <<
	.uleb128 .Ltmp78-.Ltmp77                #   Call between .Ltmp77 and .Ltmp78
	.uleb128 .Ltmp79-.Lfunc_begin13         #     jumps to .Ltmp79
	.byte	1                               #   On action: 1
	.uleb128 .Ltmp71-.Lfunc_begin13         # >> Call Site 6 <<
	.uleb128 .Ltmp72-.Ltmp71                #   Call between .Ltmp71 and .Ltmp72
	.uleb128 .Ltmp73-.Lfunc_begin13         #     jumps to .Ltmp73
	.byte	1                               #   On action: 1
	.uleb128 .Ltmp68-.Lfunc_begin13         # >> Call Site 7 <<
	.uleb128 .Ltmp69-.Ltmp68                #   Call between .Ltmp68 and .Ltmp69
	.uleb128 .Ltmp70-.Lfunc_begin13         #     jumps to .Ltmp70
	.byte	1                               #   On action: 1
.Lcst_end13:
	.byte	1                               # >> Action Record 1 <<
                                        #   Catch TypeInfo 1
	.byte	0                               #   No further actions
	.p2align	2
                                        # >> Catch TypeInfos <<
	.quad	0                               # TypeInfo 1
.Lttbase1:
	.p2align	2
	.section	.cjinit_function,"xr"
                                        # -- End function
	.def	_CGP7defaultirHv;
	.scl	2;
	.type	32;
	.endef
	.text
	.globl	_CGP7defaultirHv                # -- Begin function _CGP7defaultirHv
	.p2align	4, 0x90
.Ltmp93:                                # @_CGP7defaultirHv
	.long	.Lmethod_desc._CGP7defaultirHv-.Ltmp93
_CGP7defaultirHv:
.Lfunc_begin14:
.seh_proc _CGP7defaultirHv
	.seh_handler .L__cj_personality_v0$, @unwind, @except
# %bb.0:                                # %entry
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$48, %rsp
	movq	40(%r15), %rax
	cmpq	%rax, %rsp
	jbe	.Lstack.overflow9
.Lstack.check.end9:
	.seh_stackalloc 48
	leaq	48(%rsp), %rbp
	.seh_setframe %rbp, 48
	.seh_endprologue
	movq	%rcx, -8(%rbp)                  # 8-byte Spill
	callq	.LCJ_Safepoint_Stub
.Ltmp94:
	callq	_CGP7defaultifHv
.Ltmp95:
	callq	_CGP7defaultilHv
.Ltmp96:
	callq	_CGP7defaultiiHv
.Ltmp97:
	movq	-8(%rbp), %rax                  # 8-byte Reload
	movq	(%rax), %rcx
	movq	8(%rax), %rdx
	movq	wrapper.F0uPuE.CJStubGV(%rip), %r11
	pushq	%r11
	pushq	$32
	callq	CJ_MCC_C2NStub
.Ltmp98:
.L_epilogue13:
	movq	%rbp, %rsp
	subq	$48, %rsp
	addq	$48, %rsp
	popq	%rbp
	retq
.Lstack.overflow9:
	addq	$48, %rsp
	movabsq	$16, %rax
	callq	CJ_MCC_StackGrowStub
.Ltmp99:
	subq	$48, %rsp
	jmp	.Lstack.check.end9
.Lfunc_end14:
	.seh_handlerdata
	.text
	.seh_endproc
	.section	.xdata,"dw"
	.p2align	2
CJ_except_table16:
.Lexception14:
	.long	1431655765                      # No callsite function EH table header
	.text
                                        # -- End function
	.def	_CGP7defaultifHv;
	.scl	2;
	.type	32;
	.endef
	.globl	_CGP7defaultifHv                # -- Begin function _CGP7defaultifHv
	.p2align	4, 0x90
.Ltmp100:                               # @_CGP7defaultifHv
	.long	.Lmethod_desc._CGP7defaultifHv-.Ltmp100
_CGP7defaultifHv:
.Lfunc_begin15:
.seh_proc _CGP7defaultifHv
	.seh_handler .L__cj_personality_v0$, @unwind, @except
# %bb.0:                                # %entry
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$32, %rsp
	movq	40(%r15), %rax
	cmpq	%rax, %rsp
	jbe	.Lstack.overflow10
.Lstack.check.end10:
	.seh_stackalloc 32
	leaq	32(%rsp), %rbp
	.seh_setframe %rbp, 32
	.seh_endprologue
	callq	.LCJ_Safepoint_Stub
.Ltmp101:
	callq	_CGPacifHv
.Ltmp102:
	callq	_CGPatifHv
.Ltmp103:
	movb	$0, has_invoked_pkg_init_literal(%rip)
	movb	$0, ($has_applied_pkg_init_func)(%rip)
.L_epilogue14:
	movq	%rbp, %rsp
	subq	$32, %rsp
	addq	$32, %rsp
	popq	%rbp
	retq
.Lstack.overflow10:
	addq	$32, %rsp
	movabsq	$16, %rax
	callq	CJ_MCC_StackGrowStub
.Ltmp104:
	subq	$32, %rsp
	jmp	.Lstack.check.end10
.Lfunc_end15:
	.seh_handlerdata
	.text
	.seh_endproc
	.section	.xdata,"dw"
	.p2align	2
CJ_except_table17:
.Lexception15:
	.long	1431655765                      # No callsite function EH table header
	.text
                                        # -- End function
	.def	.Lwrapper.F0uPuE;
	.scl	3;
	.type	32;
	.endef
	.p2align	4, 0x90                         # -- Begin function wrapper.F0uPuE
.Lwrapper.F0uPuE:                       # @wrapper.F0uPuE
.seh_proc .Lwrapper.F0uPuE
# %bb.0:                                # %entry
	pushq	%rbp
	.seh_pushreg %rbp
	subq	$48, %rsp
	.seh_stackalloc 48
	leaq	48(%rsp), %rbp
	.seh_setframe %rbp, 48
	.seh_endprologue
	movq	%rdx, -8(%rbp)                  # 8-byte Spill
	movq	%rcx, %rax
	movq	-8(%rbp), %rcx                  # 8-byte Reload
	callq	*%rax
	nop
	addq	$48, %rsp
	popq	%rbp
	retq
	.seh_endproc
                                        # -- End function
	.def	.L0_for_keeping_some_types;
	.scl	3;
	.type	32;
	.endef
	.p2align	4, 0x90                         # -- Begin function 0_for_keeping_some_types
.L0_for_keeping_some_types:             # @"0_for_keeping_some_types"
.seh_proc .L0_for_keeping_some_types
# %bb.0:                                # %entry.unreachable
	subq	$560, %rsp                      # imm = 0x230
	.seh_stackalloc 560
	.seh_endprologue
	addq	$560, %rsp                      # imm = 0x230
	retq
	.seh_endproc
                                        # -- End function
	.def	CJ_MCC_NewObjArray_MSGC_cls1_sz16_sc1;
	.scl	3;
	.type	32;
	.endef
	.p2align	4, 0x90                         # -- Begin function CJ_MCC_NewObjArray_MSGC_cls1_sz16_sc1
CJ_MCC_NewObjArray_MSGC_cls1_sz16_sc1:  # @CJ_MCC_NewObjArray_MSGC_cls1_sz16_sc1
# %bb.0:                                # %entry
	jmp	CJ_MCC_NewArrayGenericFast3_MSGC # TAILCALL
                                        # -- End function
	.def	.LCJ_Safepoint_Stub;
	.scl	3;
	.type	32;
	.endef
	.p2align	4, 0x90                         # -- Begin function CJ_Safepoint_Stub
.LCJ_Safepoint_Stub:                    # @CJ_Safepoint_Stub
.seh_proc .LCJ_Safepoint_Stub
# %bb.0:
	cmpq	$0, 48(%r15)
	jne	.Ltmp105
	retq
.Ltmp105:
	jmpq	*CJ_MCC_HandleSafepoint.CJStubGV(%rip)
	retq
	.seh_endproc
                                        # -- End function
	.def	CJ_MCC_NewObjectFast_MSGC_dynamic_sc1;
	.scl	3;
	.type	32;
	.endef
	.p2align	4, 0x90                         # -- Begin function CJ_MCC_NewObjectFast_MSGC_dynamic_sc1
CJ_MCC_NewObjectFast_MSGC_dynamic_sc1:  # @CJ_MCC_NewObjectFast_MSGC_dynamic_sc1
# %bb.0:                                # %entry
	jmp	CJ_MCC_NewObject                # TAILCALL
                                        # -- End function
	.def	CJ_MCC_NewObjectFast_MSGC_cls0_sz8_sc0;
	.scl	3;
	.type	32;
	.endef
	.p2align	4, 0x90                         # -- Begin function CJ_MCC_NewObjectFast_MSGC_cls0_sz8_sc0
CJ_MCC_NewObjectFast_MSGC_cls0_sz8_sc0: # @CJ_MCC_NewObjectFast_MSGC_cls0_sz8_sc0
# %bb.0:                                # %entry
	jmp	CJ_MCC_NewObject                # TAILCALL
                                        # -- End function
	.def	CJ_MCC_NewObjectFast_MSGC_cls5_sz64_sc1;
	.scl	3;
	.type	32;
	.endef
	.p2align	4, 0x90                         # -- Begin function CJ_MCC_NewObjectFast_MSGC_cls5_sz64_sc1
CJ_MCC_NewObjectFast_MSGC_cls5_sz64_sc1: # @CJ_MCC_NewObjectFast_MSGC_cls5_sz64_sc1
# %bb.0:                                # %entry
	jmp	CJ_MCC_NewObject                # TAILCALL
                                        # -- End function
	.bss
	.p2align	3                               # @"$has_applied_pkg_init_func"
.LRef.$has_applied_pkg_init_func:
$has_applied_pkg_init_func:
	.byte	0                               # 0x0

	.p2align	3                               # @has_invoked_pkg_init_literal
.LRef.has_invoked_pkg_init_literal:
has_invoked_pkg_init_literal:
	.byte	0                               # 0x0

	.p2align	3                               # @Unit.Val
.LRef..LUnit.Val:
.LUnit.Val:

	.section	.rdata,"dr"
	.p2align	3                               # @"$const_cjstring.c-J6RJj+YQj"
".L$const_cjstring.c-J6RJj+YQj":
	.quad	.L$const_cjstring_data.7QXoUAUdrCk
	.long	41                              # 0x29
	.long	16                              # 0x10

	.p2align	3                               # @"$const_cjstring.daAk2A5RJ4U"
.L$const_cjstring.daAk2A5RJ4U:
	.quad	.L$const_cjstring_data.7QXoUAUdrCk
	.long	0                               # 0x0
	.long	41                              # 0x29

	.data
.LRef..Lcj.sdk.version:                 # @cj.sdk.version
.Lcj.sdk.version:
	.asciz	"0.0.1"

	.section	.rdata,"dr"
	.p2align	3                               # @"$const_cjstring.eOeYIhisqu"
.L$const_cjstring.eOeYIhisqu:
	.quad	.L$const_cjstring_data.7QXoUAUdrCk
	.long	57                              # 0x39
	.long	26                              # 0x1a

	.p2align	3                               # @"$const_cjstring.5CeEPuGykeB"
.L$const_cjstring.5CeEPuGykeB:
	.quad	.L$const_cjstring_data.7QXoUAUdrCk
	.long	83                              # 0x53
	.long	17                              # 0x11

	.p2align	4                               # @"$const_cjstring_data.7QXoUAUdrCk"
.L$const_cjstring_data.7QXoUAUdrCk:
	.quad	"RawArray<UInt8>.ti"
	.quad	100                             # 0x64
	.ascii	"The value of the step should not be zero.Divided by zero!An exception has occurred:    Out of memory"
	.zero	4

	.data
	.p2align	3                               # @wrapper.F0uPuE.CJStubGV
wrapper.F0uPuE.CJStubGV:
	.quad	.Lwrapper.F0uPuE

	.p2align	3                               # @CJ_MCC_HandleSafepoint.CJStubGV
CJ_MCC_HandleSafepoint.CJStubGV:
	.quad	CJ_MCC_HandleSafepoint

	.section	.rdata$.refptr.Int64.ti,"dr",discard,.refptr.Int64.ti
	.p2align	3
	.globl	.refptr.Int64.ti
.refptr.Int64.ti:
	.quad	Int64.ti
	.section	.rdata$.refptr.std.core:Error.ti,"dr",discard,".refptr.std.core:Error.ti"
	.p2align	3
	.globl	".refptr.std.core:Error.ti"
".refptr.std.core:Error.ti":
	.quad	"std.core:Error.ti"
	.section	.rdata$.refptr.std.core:Exception.ti,"dr",discard,".refptr.std.core:Exception.ti"
	.p2align	3
	.globl	".refptr.std.core:Exception.ti"
".refptr.std.core:Exception.ti":
	.quad	"std.core:Exception.ti"
	.section	.rdata$.refptr.std.core:IllegalArgumentException.ti,"dr",discard,".refptr.std.core:IllegalArgumentException.ti"
	.p2align	3
	.globl	".refptr.std.core:IllegalArgumentException.ti"
".refptr.std.core:IllegalArgumentException.ti":
	.quad	"std.core:IllegalArgumentException.ti"
	.section	.rdata$.refptr.std.core:OutOfMemoryError.ti,"dr",discard,".refptr.std.core:OutOfMemoryError.ti"
	.p2align	3
	.globl	".refptr.std.core:OutOfMemoryError.ti"
".refptr.std.core:OutOfMemoryError.ti":
	.quad	"std.core:OutOfMemoryError.ti"
	.section	.rdata,"dr"
lCJMSGCSizeClassTable:
	.byte	255
	.byte	0
	.byte	1
	.byte	2
	.byte	3
	.byte	4
	.byte	4
	.byte	5
	.byte	5
	.byte	6
	.byte	6
	.byte	7
	.byte	7
	.byte	8
	.byte	8
	.byte	9
	.byte	9
	.byte	10
	.byte	10
	.byte	10
	.byte	10
	.byte	11
	.byte	11
	.byte	11
	.byte	11
	.byte	12
	.byte	12
	.byte	12
	.byte	12
	.byte	13
	.byte	13
	.byte	13
	.byte	13
	.byte	14
	.byte	14
	.byte	14
	.byte	14
	.byte	14
	.byte	14
	.byte	14
	.byte	14
	.byte	15
	.byte	15
	.byte	15
	.byte	15
	.byte	15
	.byte	15
	.byte	15
	.byte	15
	.byte	16
	.byte	16
	.byte	16
	.byte	16
	.byte	16
	.byte	16
	.byte	16
	.byte	16
	.byte	17
	.byte	17
	.byte	17
	.byte	17
	.byte	17
	.byte	17
	.byte	17
	.byte	17
	.byte	18
	.byte	18
	.byte	18
	.byte	18
	.byte	18
	.byte	18
	.byte	18
	.byte	18
	.byte	18
	.byte	18
	.byte	18
	.byte	18
	.byte	18
	.byte	18
	.byte	18
	.byte	18
	.byte	19
	.byte	19
	.byte	19
	.byte	19
	.byte	19
	.byte	19
	.byte	19
	.byte	19
	.byte	19
	.byte	19
	.byte	19
	.byte	19
	.byte	19
	.byte	19
	.byte	19
	.byte	19
	.byte	20
	.byte	20
	.byte	20
	.byte	20
	.byte	20
	.byte	20
	.byte	20
	.byte	20
	.byte	20
	.byte	20
	.byte	20
	.byte	20
	.byte	20
	.byte	20
	.byte	20
	.byte	20
	.byte	21
	.byte	21
	.byte	21
	.byte	21
	.byte	21
	.byte	21
	.byte	21
	.byte	21
	.byte	21
	.byte	21
	.byte	21
	.byte	21
	.byte	21
	.byte	21
	.byte	21
	.byte	21
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	22
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	23
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	24
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	25
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	26
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	27
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	28
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	29
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	30
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	31
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	32
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	33
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	34
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	35
	.byte	0
lCJMSGCSlotSizeTable:
	.short	8
	.short	16
	.short	24
	.short	32
	.short	48
	.short	64
	.short	80
	.short	96
	.short	112
	.short	128
	.short	160
	.short	192
	.short	224
	.short	256
	.short	320
	.short	384
	.short	448
	.short	512
	.short	640
	.short	768
	.short	896
	.short	1024
	.short	1280
	.short	1536
	.short	1792
	.short	2048
	.short	2560
	.short	3072
	.short	3584
	.short	4096
	.short	5120
	.short	6144
	.short	8192
	.short	10240
	.short	12288
	.short	16384
	.section	.cjsdkv,"dw"
	.quad	.LRef..Lcj.sdk.version
	.section	.cjsm,"dw"
.Lstack_map._CGP7defaultfiHv:
	#StackSize: 0
	#StackmapFormatType: bit map
	#CalleeSaveReg: (0x0=0), offsets(without sign): []
	#StackMapItem nums:0
	.byte	0
	.byte	0
	.byte	4
.Lstack_map._CGP7defaultflHv:
	#StackSize: 0
	#StackmapFormatType: bit map
	#CalleeSaveReg: (0x0=0), offsets(without sign): []
	#StackMapItem nums:0
	.byte	0
	.byte	0
	.byte	4
.Lstack_map._CGP7defaultiiHv:
	#StackSize: 0
	#StackmapFormatType: bit map
	#CalleeSaveReg: (0x0=0), offsets(without sign): []
	#StackMapItem nums:0
	.byte	0
	.byte	0
	.byte	4
.Lstack_map._CGP7defaultilHv:
	#StackSize: 0
	#StackmapFormatType: bit map
	#CalleeSaveReg: (0x0=0), offsets(without sign): []
	#StackMapItem nums:0
	.byte	0
	.byte	0
	.byte	4
".Lstack_map._CN7default6<main>Hv":
	#StackSize: 0
	#StackmapFormatType: bit map
	#CalleeSaveReg: (0x0=0), offsets(without sign): []
	#StackMapItem nums:9
	.byte	0
	.byte	144
	.byte	49
	.byte	19
	.byte	113
	.byte	4
	.long	.Ltmp28-"_CN7default6<main>Hv"
	#[RegIdx: -1, SlotIdx: -1, LNIdx: 0, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: -1]
	.byte	16
	.byte	0
	.long	.Ltmp29-"_CN7default6<main>Hv"
	#[RegIdx: -1, SlotIdx: -1, LNIdx: 0, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: -1]
	.byte	16
	.byte	0
	.long	.Ltmp30-"_CN7default6<main>Hv"
	#[RegIdx: -1, SlotIdx: 0, LNIdx: 0, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: -1]
	.byte	18
	.byte	0
	.long	.Ltmp31-"_CN7default6<main>Hv"
	#[RegIdx: -1, SlotIdx: 1, LNIdx: 1, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: -1]
	.byte	36
	.byte	0
	.long	.Ltmp32-"_CN7default6<main>Hv"
	#[RegIdx: -1, SlotIdx: 2, LNIdx: 1, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: -1]
	.byte	38
	.byte	0
	.long	.Ltmp33-"_CN7default6<main>Hv"
	#[RegIdx: -1, SlotIdx: 2, LNIdx: 1, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: -1]
	.byte	38
	.byte	0
	.long	.Ltmp34-"_CN7default6<main>Hv"
	#[RegIdx: -1, SlotIdx: 3, LNIdx: 1, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: -1]
	.byte	40
	.byte	0
	.long	.Ltmp35-"_CN7default6<main>Hv"
	#[RegIdx: -1, SlotIdx: 0, LNIdx: 2, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: -1]
	.byte	50
	.byte	0
	.long	.Ltmp36-"_CN7default6<main>Hv"
	#[RegIdx: -1, SlotIdx: -1, LNIdx: 3, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: -1]
	.byte	64
	.byte	0
	#RegNums: 0
	#SlotsNums: 4
		#Idx[0]: BaseOffset: -32, SlotBits: 0x1[ -32 ]
		#Idx[1]: BaseOffset: -24, SlotBits: 0x3[ -24 -32 ]
		#Idx[2]: BaseOffset: -24, SlotBits: 0x7[ -24 -32 -40 ]
		#Idx[3]: BaseOffset: -24, SlotBits: 0x1[ -24 ]
	#LineNumbersNums: 4
		#Idx[0]: 20
		#Idx[1]: 23
		#Idx[2]: 26
		#Idx[3]: 19
	#DerivedInfoNums: 0
	.quad	4404417030439273488
	.byte	84
	.byte	244
	.byte	234
	.byte	9
".Lstack_map._CNat5ArrayICN7default1AEE6<init>Hv":
	#StackSize: 0
	#StackmapFormatType: bit map
	#CalleeSaveReg: (0x0=0), offsets(without sign): []
	#StackMapItem nums:1
	.byte	0
	.byte	16
	.byte	33
	.byte	34
	.byte	129
	.byte	4
	.long	.Ltmp38-"_CNat5ArrayICN7default1AEE6<init>Hv"
	#[RegIdx: -1, SlotIdx: 0, LNIdx: 0, DerivedStartIdx: 0, SPRegIdx: -1, SPSlotIdx: -1]
	.byte	42
	.byte	0
	#RegNums: 0
	#SlotsNums: 2
		#Idx[0]: BaseOffset: -8, SlotBits: 0x1[ -8 ]
		#Idx[1]: BaseOffset: -16, SlotBits: 0x1[ -16 ]
	#LineNumbersNums: 1
		#Idx[0]: 30
	#DerivedInfoNums: 1
		#Idx[0]: RegIdx: -1, SlotIdx: 1
	.byte	16
	.byte	130
	.byte	129
	.byte	31
	.byte	126
	.byte	148
	.byte	15
	.byte	2
.Lstack_map._CNat5ArrayICN7default1AEE9normalizeHRNat5RangeIlE:
	#StackSize: 0
	#StackmapFormatType: bit map
	#CalleeSaveReg: (0x0=0), offsets(without sign): []
	#StackMapItem nums:1
	.byte	0
	.byte	16
	.byte	33
	.byte	18
	.byte	145
	.byte	4
	.long	.Ltmp40-_CNat5ArrayICN7default1AEE9normalizeHRNat5RangeIlE
	#[RegIdx: -1, SlotIdx: -1, LNIdx: 0, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: 0]
	.byte	136
	.byte	0
	#RegNums: 0
	#SlotsNums: 1
		#Idx[0]: BaseOffset: -72, SlotBits: 0xf[ -72 -80 -88 -96 ]
	#LineNumbersNums: 1
		#Idx[0]: 231
	#DerivedInfoNums: 0
	.byte	16
	.byte	129
	.byte	132
	.byte	251
	.byte	129
	.byte	231
	.byte	0
.Lstack_map._CNat5RangeIlE4lastHv:
	#StackSize: 112
	#StackmapFormatType: bit map
	#CalleeSaveReg: (0x0=0), offsets(without sign): []
	#StackMapItem nums:8
	.byte	12
	.byte	7
	.byte	128
	.byte	34
	.byte	20
	.byte	82
	.byte	4
	.long	.Ltmp42-_CNat5RangeIlE4lastHv
	#[RegIdx: -1, SlotIdx: -1, LNIdx: 0, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: 0]
	.byte	16
	.byte	8
	.long	.Ltmp43-_CNat5RangeIlE4lastHv
	#[RegIdx: -1, SlotIdx: -1, LNIdx: 1, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: 1]
	.byte	32
	.byte	16
	.long	.Ltmp44-_CNat5RangeIlE4lastHv
	#[RegIdx: -1, SlotIdx: -1, LNIdx: 1, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: 1]
	.byte	32
	.byte	16
	.long	.Ltmp45-_CNat5RangeIlE4lastHv
	#[RegIdx: -1, SlotIdx: -1, LNIdx: 2, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: -1]
	.byte	48
	.byte	0
	.long	.Ltmp46-_CNat5RangeIlE4lastHv
	#[RegIdx: -1, SlotIdx: -1, LNIdx: 3, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: -1]
	.byte	64
	.byte	0
	.long	.Ltmp47-_CNat5RangeIlE4lastHv
	#[RegIdx: -1, SlotIdx: -1, LNIdx: 4, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: -1]
	.byte	80
	.byte	0
	.long	.Ltmp48-_CNat5RangeIlE4lastHv
	#[RegIdx: -1, SlotIdx: -1, LNIdx: 5, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: -1]
	.byte	96
	.byte	0
	.long	.Ltmp49-_CNat5RangeIlE4lastHv
	#[RegIdx: -1, SlotIdx: -1, LNIdx: 6, DerivedStartIdx: -1, SPRegIdx: 0, SPSlotIdx: -1]
	.byte	112
	.byte	2
	#RegNums: 1
		#Idx[0]: (0x6=6), rdx, rcx
	#SlotsNums: 2
		#Idx[0]: BaseOffset: -144, SlotBits: 0x3[ -144 -152 ]
		#Idx[1]: BaseOffset: -144, SlotBits: 0x1[ -144 ]
	#LineNumbersNums: 7
		#Idx[0]: 62
		#Idx[1]: 68
		#Idx[2]: 69
		#Idx[3]: 72
		#Idx[4]: 74
		#Idx[5]: 81
		#Idx[6]: 61
	#DerivedInfoNums: 0
	.quad	-80502152299669967
	.byte	187
	.byte	243
	.byte	17
	.byte	139
	.byte	72
	.byte	101
	.byte	180
	.byte	7
".Lstack_map._CNat5RangeIlE6<init>Hlllbbb":
	#StackSize: 0
	#StackmapFormatType: bit map
	#CalleeSaveReg: (0x0=0), offsets(without sign): []
	#StackMapItem nums:4
	.byte	0
	.byte	64
	.byte	50
	.byte	35
	.byte	66
	.byte	4
	.long	.Ltmp51-"_CNat5RangeIlE6<init>Hlllbbb"
	#[RegIdx: -1, SlotIdx: 0, LNIdx: 0, DerivedStartIdx: 0, SPRegIdx: -1, SPSlotIdx: -1]
	.byte	36
	.byte	1
	.long	.Ltmp52-"_CNat5RangeIlE6<init>Hlllbbb"
	#[RegIdx: -1, SlotIdx: -1, LNIdx: 1, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: -1]
	.byte	64
	.byte	0
	.long	.Ltmp53-"_CNat5RangeIlE6<init>Hlllbbb"
	#[RegIdx: -1, SlotIdx: 0, LNIdx: 1, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: -1]
	.byte	68
	.byte	0
	.long	.Ltmp54-"_CNat5RangeIlE6<init>Hlllbbb"
	#[RegIdx: -1, SlotIdx: -1, LNIdx: 2, DerivedStartIdx: -1, SPRegIdx: 0, SPSlotIdx: 2]
	.byte	96
	.byte	52
	#RegNums: 1
		#Idx[0]: (0x6=6), rdx, rcx
	#SlotsNums: 3
		#Idx[0]: BaseOffset: -8, SlotBits: 0x1[ -8 ]
		#Idx[1]: BaseOffset: -16, SlotBits: 0x1[ -16 ]
		#Idx[2]: BaseOffset: 80, SlotBits: 0x1[ 80 ]
	#LineNumbersNums: 3
		#Idx[0]: 25
		#Idx[1]: 26
		#Idx[2]: 24
	#DerivedInfoNums: 1
		#Idx[0]: RegIdx: -1, SlotIdx: 1
	.quad	7299950102407028273
	.byte	141
	.byte	3
	.byte	1
.Lstack_map.user.main:
	#StackSize: 0
	#StackmapFormatType: bit map
	#CalleeSaveReg: (0x0=0), offsets(without sign): []
	#StackMapItem nums:0
	.byte	0
	.byte	0
	.byte	4
.Lstack_map.cj_entry$:
	#StackSize: 16
	#StackmapFormatType: bit map
	#CalleeSaveReg: (0x0=0), offsets(without sign): []
	#StackMapItem nums:2
	.byte	12
	.byte	1
	.byte	32
	.byte	33
	.byte	17
	.byte	33
	.byte	4
	.long	.Ltmp88-cj_entry$
	#[RegIdx: -1, SlotIdx: 0, LNIdx: -1, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: -1]
	.byte	2
	.long	.Ltmp89-cj_entry$
	#[RegIdx: -1, SlotIdx: 0, LNIdx: -1, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: -1]
	.byte	2
	#RegNums: 0
	#SlotsNums: 1
		#Idx[0]: BaseOffset: -64, SlotBits: 0x1[ -64 ]
	#LineNumbersNums: 0
	#DerivedInfoNums: 0
	.byte	16
	.byte	129
	.byte	1
	.byte	28
	.byte	2
	.byte	0
.Lstack_map._CGP7defaultirHv:
	#StackSize: 0
	#StackmapFormatType: bit map
	#CalleeSaveReg: (0x0=0), offsets(without sign): []
	#StackMapItem nums:5
	.byte	0
	.byte	80
	.byte	34
	.byte	17
	.byte	130
	.byte	4
	.long	.Ltmp94-_CGP7defaultirHv
	#[RegIdx: -1, SlotIdx: -1, LNIdx: -1, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: 0]
	.byte	0
	.byte	1
	.long	.Ltmp95-_CGP7defaultirHv
	#[RegIdx: -1, SlotIdx: -1, LNIdx: -1, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: 0]
	.byte	0
	.byte	1
	.long	.Ltmp96-_CGP7defaultirHv
	#[RegIdx: -1, SlotIdx: -1, LNIdx: -1, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: 0]
	.byte	0
	.byte	1
	.long	.Ltmp97-_CGP7defaultirHv
	#[RegIdx: -1, SlotIdx: -1, LNIdx: -1, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: 0]
	.byte	0
	.byte	1
	.long	.Ltmp99-_CGP7defaultirHv
	#[RegIdx: -1, SlotIdx: -1, LNIdx: -1, DerivedStartIdx: -1, SPRegIdx: 0, SPSlotIdx: -1]
	.byte	64
	.byte	0
	#RegNums: 1
		#Idx[0]: (0x4=4), rcx
	#SlotsNums: 1
		#Idx[0]: BaseOffset: -8, SlotBits: 0x1[ -8 ]
	#LineNumbersNums: 0
	#DerivedInfoNums: 0
	.byte	49
	.byte	12
	.byte	12
	.byte	252
	.byte	16
	.byte	0
.Lstack_map._CGP7defaultifHv:
	#StackSize: 0
	#StackmapFormatType: bit map
	#CalleeSaveReg: (0x0=0), offsets(without sign): []
	#StackMapItem nums:0
	.byte	0
	.byte	0
	.byte	4

	.section	.cjmthd,"dw"
.Lmethod_desc._CGP7defaultflHv:
	.long	.Lstack_map._CGP7defaultflHv-.Lmethod_desc._CGP7defaultflHv
	.long	.Lfunc_end1-.Lfunc_begin1
	.long	.Lstr_pool.0-.Lmethod_desc._CGP7defaultflHv
	.long	.Lstr_pool.1-.Lmethod_desc._CGP7defaultflHv
	.long	.Lstr_pool.1-.Lmethod_desc._CGP7defaultflHv
	.long	.Lstr_pool_dict_offsets-.Lmethod_desc._CGP7defaultflHv
.Ltmp106:
	.long	CJ_except_table2-.Ltmp106
.Lmethod_desc._CGP7defaultilHv:
	.long	.Lstack_map._CGP7defaultilHv-.Lmethod_desc._CGP7defaultilHv
	.long	.Lfunc_end3-.Lfunc_begin3
	.long	.Lstr_pool.2-.Lmethod_desc._CGP7defaultilHv
	.long	.Lstr_pool.1-.Lmethod_desc._CGP7defaultilHv
	.long	.Lstr_pool.1-.Lmethod_desc._CGP7defaultilHv
	.long	.Lstr_pool_dict_offsets-.Lmethod_desc._CGP7defaultilHv
.Ltmp107:
	.long	CJ_except_table4-.Ltmp107
".Lmethod_desc._CNat5ArrayICN7default1AEE6<init>Hv":
	.long	".Lstack_map._CNat5ArrayICN7default1AEE6<init>Hv"-".Lmethod_desc._CNat5ArrayICN7default1AEE6<init>Hv"
	.long	.Lfunc_end6-.Lfunc_begin6
	.long	.Lstr_pool.3-".Lmethod_desc._CNat5ArrayICN7default1AEE6<init>Hv"
	.long	.Lstr_pool.4-".Lmethod_desc._CNat5ArrayICN7default1AEE6<init>Hv"
	.long	.Lstr_pool.5-".Lmethod_desc._CNat5ArrayICN7default1AEE6<init>Hv"
	.long	.Lstr_pool_dict_offsets-".Lmethod_desc._CNat5ArrayICN7default1AEE6<init>Hv"
.Ltmp108:
	.long	CJ_except_table7-.Ltmp108
.Lmethod_desc._CNat5ArrayICN7default1AEE9normalizeHRNat5RangeIlE:
	.long	.Lstack_map._CNat5ArrayICN7default1AEE9normalizeHRNat5RangeIlE-.Lmethod_desc._CNat5ArrayICN7default1AEE9normalizeHRNat5RangeIlE
	.long	.Lfunc_end8-.Lfunc_begin8
	.long	.Lstr_pool.6-.Lmethod_desc._CNat5ArrayICN7default1AEE9normalizeHRNat5RangeIlE
	.long	.Lstr_pool.4-.Lmethod_desc._CNat5ArrayICN7default1AEE9normalizeHRNat5RangeIlE
	.long	.Lstr_pool.5-.Lmethod_desc._CNat5ArrayICN7default1AEE9normalizeHRNat5RangeIlE
	.long	.Lstr_pool_dict_offsets-.Lmethod_desc._CNat5ArrayICN7default1AEE9normalizeHRNat5RangeIlE
.Ltmp109:
	.long	CJ_except_table9-.Ltmp109
.Lmethod_desc._CNat5RangeIlE4lastHv:
	.long	.Lstack_map._CNat5RangeIlE4lastHv-.Lmethod_desc._CNat5RangeIlE4lastHv
	.long	.Lfunc_end9-.Lfunc_begin9
	.long	.Lstr_pool.7-.Lmethod_desc._CNat5RangeIlE4lastHv
	.long	.Lstr_pool.4-.Lmethod_desc._CNat5RangeIlE4lastHv
	.long	.Lstr_pool.8-.Lmethod_desc._CNat5RangeIlE4lastHv
	.long	.Lstr_pool_dict_offsets-.Lmethod_desc._CNat5RangeIlE4lastHv
.Ltmp110:
	.long	CJ_except_table10-.Ltmp110
".Lmethod_desc._CNat5RangeIlE6<init>Hlllbbb":
	.long	".Lstack_map._CNat5RangeIlE6<init>Hlllbbb"-".Lmethod_desc._CNat5RangeIlE6<init>Hlllbbb"
	.long	.Lfunc_end10-.Lfunc_begin10
	.long	.Lstr_pool.9-".Lmethod_desc._CNat5RangeIlE6<init>Hlllbbb"
	.long	.Lstr_pool.4-".Lmethod_desc._CNat5RangeIlE6<init>Hlllbbb"
	.long	.Lstr_pool.8-".Lmethod_desc._CNat5RangeIlE6<init>Hlllbbb"
	.long	.Lstr_pool_dict_offsets-".Lmethod_desc._CNat5RangeIlE6<init>Hlllbbb"
.Ltmp111:
	.long	CJ_except_table11-.Ltmp111
.Lmethod_desc._CGP7defaultirHv:
	.long	.Lstack_map._CGP7defaultirHv-.Lmethod_desc._CGP7defaultirHv
	.long	.Lfunc_end14-.Lfunc_begin14
	.long	.Lstr_pool.10-.Lmethod_desc._CGP7defaultirHv
	.long	.Lstr_pool.1-.Lmethod_desc._CGP7defaultirHv
	.long	.Lstr_pool.1-.Lmethod_desc._CGP7defaultirHv
	.long	.Lstr_pool_dict_offsets-.Lmethod_desc._CGP7defaultirHv
.Ltmp112:
	.long	CJ_except_table16-.Ltmp112
.Lmethod_desc._CGP7defaultifHv:
	.long	.Lstack_map._CGP7defaultifHv-.Lmethod_desc._CGP7defaultifHv
	.long	.Lfunc_end15-.Lfunc_begin15
	.long	.Lstr_pool.11-.Lmethod_desc._CGP7defaultifHv
	.long	.Lstr_pool.1-.Lmethod_desc._CGP7defaultifHv
	.long	.Lstr_pool.1-.Lmethod_desc._CGP7defaultifHv
	.long	.Lstr_pool_dict_offsets-.Lmethod_desc._CGP7defaultifHv
.Ltmp113:
	.long	CJ_except_table17-.Ltmp113
.Lmethod_desc._CGP7defaultfiHv:
	.long	.Lstack_map._CGP7defaultfiHv-.Lmethod_desc._CGP7defaultfiHv
	.long	.Lfunc_end0-.Lfunc_begin0
	.long	.Lstr_pool.12-.Lmethod_desc._CGP7defaultfiHv
	.long	.Lstr_pool.1-.Lmethod_desc._CGP7defaultfiHv
	.long	.Lstr_pool.1-.Lmethod_desc._CGP7defaultfiHv
	.long	.Lstr_pool_dict_offsets-.Lmethod_desc._CGP7defaultfiHv
.Ltmp114:
	.long	CJ_except_table0-.Ltmp114
.Lmethod_desc._CGP7defaultiiHv:
	.long	.Lstack_map._CGP7defaultiiHv-.Lmethod_desc._CGP7defaultiiHv
	.long	.Lfunc_end2-.Lfunc_begin2
	.long	.Lstr_pool.13-.Lmethod_desc._CGP7defaultiiHv
	.long	.Lstr_pool.1-.Lmethod_desc._CGP7defaultiiHv
	.long	.Lstr_pool.1-.Lmethod_desc._CGP7defaultiiHv
	.long	.Lstr_pool_dict_offsets-.Lmethod_desc._CGP7defaultiiHv
.Ltmp115:
	.long	CJ_except_table3-.Ltmp115
".Lmethod_desc._CN7default6<main>Hv":
	.long	".Lstack_map._CN7default6<main>Hv"-".Lmethod_desc._CN7default6<main>Hv"
	.long	.Lfunc_end5-.Lfunc_begin5
	.long	.Lstr_pool.14-".Lmethod_desc._CN7default6<main>Hv"
	.long	.Lstr_pool.15-".Lmethod_desc._CN7default6<main>Hv"
	.long	.Lstr_pool.16-".Lmethod_desc._CN7default6<main>Hv"
	.long	.Lstr_pool_dict_offsets-".Lmethod_desc._CN7default6<main>Hv"
.Ltmp116:
	.long	CJ_except_table6-.Ltmp116
.Lmethod_desc.user.main:
	.long	.Lstack_map.user.main-.Lmethod_desc.user.main
	.long	.Lfunc_end12-.Lfunc_begin12
	.long	.Lstr_pool.17-.Lmethod_desc.user.main
	.long	.Lstr_pool.1-.Lmethod_desc.user.main
	.long	.Lstr_pool.1-.Lmethod_desc.user.main
	.long	.Lstr_pool_dict_offsets-.Lmethod_desc.user.main
.Ltmp117:
	.long	CJ_except_table13-.Ltmp117
.Lmethod_desc.cj_entry$:
	.long	.Lstack_map.cj_entry$-.Lmethod_desc.cj_entry$
	.long	.Lfunc_end13-.Lfunc_begin13
	.long	.Lstr_pool.18-.Lmethod_desc.cj_entry$
	.long	.Lstr_pool.1-.Lmethod_desc.cj_entry$
	.long	.Lstr_pool.1-.Lmethod_desc.cj_entry$
	.long	.Lstr_pool_dict_offsets-.Lmethod_desc.cj_entry$
.Ltmp118:
	.long	CJ_except_table15-.Ltmp118
	.section	.cjinitF,"dw"
	.quad	.Lfunc_begin2

	.section	.cjspdct,"dw"
	.p2align	3
	.byte	1
.Lstr_pool_dict_offsets:
	.long	35
	.long	.Lstr_pool_dict.1-.Lstr_pool_dict_offsets
.Lstr_pool_dict.1:
	.ascii	"_CGP,"
	.ascii	"7default,"
	.ascii	"flHv,"
	.ascii	"ilHv,"
	.ascii	"_CNat,"
	.ascii	"5Array,"
	.ascii	"ICN,"
	.ascii	"1A,"
	.ascii	"EE,"
	.ascii	"6<init>,"
	.ascii	"Hv,"
	.ascii	"std.core,"
	.ascii	"array.cj,"
	.ascii	"9normalize,"
	.ascii	"HRNat,"
	.ascii	"5Range,"
	.ascii	"IlE,"
	.ascii	"4last,"
	.ascii	"range.cj,"
	.ascii	"Hlllbbb,"
	.ascii	"irHv,"
	.ascii	"ifHv,"
	.ascii	"fiHv,"
	.ascii	"iiHv,"
	.ascii	"_CN,"
	.ascii	"6<main>,"
	.ascii	"C:\\cangjie\\cangjie_runtime_CMSGC\\runtime\\MSGC_RESULT\\bugfix\\throwable_repro_,"
	.ascii	"20260829,"
	.ascii	"\\exception_,"
	.ascii	"0010,"
	.ascii	"_temps,"
	.ascii	"exception_,"
	.ascii	".cj,"
	.ascii	"user.main,"
	.ascii	"cj_entry$,"
	.byte	0

	.section	.cjsp,"dw"
	.p2align	3
.Lstr_pool.17:
	.asciz	"\""
.Lstr_pool.15:
	.asciz	"\033\034\035\036\037"
.Lstr_pool.14:
	.asciz	"\031\002\032\013"
.Lstr_pool.18:
	.asciz	"#"
.Lstr_pool.13:
	.asciz	"\001\002\030"
.Lstr_pool.0:
	.asciz	"\001\002\003"
.Lstr_pool.10:
	.asciz	"\001\002\025"
.Lstr_pool.1:
	.byte	0
.Lstr_pool.4:
	.asciz	"\f"
.Lstr_pool.11:
	.asciz	"\001\002\026"
.Lstr_pool.16:
	.asciz	" \036!"
.Lstr_pool.3:
	.asciz	"\005\006\007\002\b\t\n\013"
.Lstr_pool.5:
	.asciz	"\r"
.Lstr_pool.7:
	.asciz	"\005\020\021\022\013"
.Lstr_pool.9:
	.asciz	"\005\020\021\n\024"
.Lstr_pool.6:
	.asciz	"\005\006\007\002\b\t\016\017\020\021"
.Lstr_pool.8:
	.asciz	"\023"
.Lstr_pool.2:
	.asciz	"\001\002\004"
.Lstr_pool.12:
	.asciz	"\001\002\027"

	.section	.cjti,"dw"
	.globl	"default:A.ti"                  # @"default:A.ti"
	.p2align	4
".LRef.default:A.ti":
"default:A.ti":
	.quad	".Ldefault:A.name"
	.byte	128                             # 0x80
	.byte	192                             # 0xc0
	.short	0                               # 0x0
	.long	0                               # 0x0
	.quad	-9223372036854775808
	.long	0                               # 0x0
	.byte	1                               # 0x1
	.byte	0                               # 0x0
	.short	32770                           # 0x8002
	.quad	0
	.quad	0
	.quad	0
	.quad	0
	.quad	"std.core:Object.ti"
	.quad	.LNonExternalExtensionDefs
	.quad	-9223372036854775808
	.quad	"default:A.ti.reflect"

	.p2align	4                               # @"std.collection:ArrayList<default:A>.ti"
".LRef.std.collection:ArrayList<default:A>.ti":
"std.collection:ArrayList<default:A>.ti":
	.quad	".Lstd.collection:ArrayList<default:A>.name"
	.byte	128                             # 0x80
	.byte	65                              # 0x41
	.short	3                               # 0x3
	.long	40                              # 0x28
	.quad	-9223372036854775807
	.long	0                               # 0x0
	.byte	8                               # 0x8
	.byte	1                               # 0x1
	.short	32770                           # 0x8002
	.quad	".Lstd.collection:ArrayList<default:A>.ti.offsets"
	.quad	"std.collection:ArrayList.tt"
	.quad	".Lstd.collection:ArrayList<default:A>.ti.typeArgs"
	.quad	".Lstd.collection:ArrayList<default:A>.ti.fields"
	.quad	"std.core:Object.ti"
	.quad	0
	.quad	0
	.quad	0

	.p2align	4                               # @"RawArray<default:A>.ti"
".LRef.RawArray<default:A>.ti":
"RawArray<default:A>.ti":
	.quad	".LRawArray<default:A>.name"
	.byte	130                             # 0x82
	.byte	65                              # 0x41
	.short	0                               # 0x0
	.long	8                               # 0x8
	.quad	0
	.long	0                               # 0x0
	.byte	1                               # 0x1
	.byte	0                               # 0x0
	.short	32768                           # 0x8000
	.quad	0
	.quad	RawArray.tt
	.quad	0
	.quad	0
	.quad	"default:A.ti"
	.quad	0
	.quad	0
	.quad	0

	.p2align	4                               # @"std.core:Array<default:A>.ti"
".LRef.std.core:Array<default:A>.ti":
"std.core:Array<default:A>.ti":
	.quad	".Lstd.core:Array<default:A>.name"
	.byte	22                              # 0x16
	.byte	65                              # 0x41
	.short	3                               # 0x3
	.long	24                              # 0x18
	.quad	-9223372036854775807
	.long	0                               # 0x0
	.byte	8                               # 0x8
	.byte	1                               # 0x1
	.short	32768                           # 0x8000
	.quad	".Lstd.core:Array<default:A>.ti.offsets"
	.quad	"std.core:Array.tt"
	.quad	".Lstd.core:Array<default:A>.ti.typeArgs"
	.quad	".Lstd.core:Array<default:A>.ti.fields"
	.quad	0
	.quad	0
	.quad	0
	.quad	0

	.p2align	4                               # @"RawArray<UInt8>.ti"
".LRef.RawArray<UInt8>.ti":
"RawArray<UInt8>.ti":
	.quad	".LRawArray<UInt8>.name"
	.byte	130                             # 0x82
	.byte	64                              # 0x40
	.short	0                               # 0x0
	.long	0                               # 0x0
	.quad	0
	.long	0                               # 0x0
	.byte	0                               # 0x0
	.byte	0                               # 0x0
	.short	32768                           # 0x8000
	.quad	0
	.quad	RawArray.tt
	.quad	0
	.quad	0
	.quad	UInt8.ti
	.quad	0
	.quad	0
	.quad	0

	.section	.cjfield,"dw"
".LRef..LRawArray<default:A>.name":     # @"RawArray<default:A>.name"
".LRawArray<default:A>.name":
	.asciz	"RawArray<default:A>"

".LRef..Lstd.collection:ArrayList<default:A>.name": # @"std.collection:ArrayList<default:A>.name"
".Lstd.collection:ArrayList<default:A>.name":
	.asciz	"std.collection:ArrayList<default:A>"

	.p2align	4                               # @"std.collection:ArrayList<default:A>.ti.fields"
".LRef..Lstd.collection:ArrayList<default:A>.ti.fields":
".Lstd.collection:ArrayList<default:A>.ti.fields":
	.quad	"std.core:Array<default:A>.ti"
	.quad	Int64.ti
	.quad	Int64.ti

	.p2align	2                               # @"std.collection:ArrayList<default:A>.ti.offsets"
".LRef..Lstd.collection:ArrayList<default:A>.ti.offsets":
".Lstd.collection:ArrayList<default:A>.ti.offsets":
	.long	0                               # 0x0
	.long	24                              # 0x18
	.long	32                              # 0x20

	.p2align	3                               # @"std.collection:ArrayList<default:A>.ti.typeArgs"
".LRef..Lstd.collection:ArrayList<default:A>.ti.typeArgs":
".Lstd.collection:ArrayList<default:A>.ti.typeArgs":
	.quad	"default:A.ti"

".LRef..Lstd.core:Array<default:A>.name": # @"std.core:Array<default:A>.name"
".Lstd.core:Array<default:A>.name":
	.asciz	"std.core:Array<default:A>"

	.p2align	4                               # @"std.core:Array<default:A>.ti.fields"
".LRef..Lstd.core:Array<default:A>.ti.fields":
".Lstd.core:Array<default:A>.ti.fields":
	.quad	"RawArray<default:A>.ti"
	.quad	Int64.ti
	.quad	Int64.ti

	.p2align	2                               # @"std.core:Array<default:A>.ti.offsets"
".LRef..Lstd.core:Array<default:A>.ti.offsets":
".Lstd.core:Array<default:A>.ti.offsets":
	.long	0                               # 0x0
	.long	8                               # 0x8
	.long	16                              # 0x10

	.p2align	3                               # @"std.core:Array<default:A>.ti.typeArgs"
".LRef..Lstd.core:Array<default:A>.ti.typeArgs":
".Lstd.core:Array<default:A>.ti.typeArgs":
	.quad	"default:A.ti"

".LRef..Ldefault:A.name":               # @"default:A.name"
".Ldefault:A.name":
	.asciz	"default:A"

".LRef..LRawArray<UInt8>.name":         # @"RawArray<UInt8>.name"
".LRawArray<UInt8>.name":
	.asciz	"RawArray<UInt8>"

	.section	.cjmtbl,"dw"
	.p2align	4                               # @"default:A_ed_std.core:Object"
".LRef..Ldefault:A_ed_std.core:Object":
".Ldefault:A_ed_std.core:Object":
	.long	0                               # 0x0
	.byte	1                               # 0x1
	.byte	129                             # 0x81
	.short	0                               # 0x0
	.quad	"default:A.ti"
	.quad	"std.core:Object.ti"
	.quad	0
	.quad	0

	.p2align	4                               # @"default:A_ed_default:A"
".LRef..Ldefault:A_ed_default:A":
".Ldefault:A_ed_default:A":
	.long	0                               # 0x0
	.byte	1                               # 0x1
	.byte	7                               # 0x7
	.short	0                               # 0x0
	.quad	"default:A.ti"
	.quad	"default:A.ti"
	.quad	0
	.quad	0

	.section	.cjinty,"dw"
	.p2align	4                               # @NonExternalExtensionDefs
.LRef..LNonExternalExtensionDefs:
.LNonExternalExtensionDefs:
	.quad	".Ldefault:A_ed_std.core:Object"
	.quad	".Ldefault:A_ed_default:A"
	.quad	0

	.section	.cjsgt,"dw"
.Lgeneric_ti0:
	.long	".LRef.RawArray<default:A>.ti"-.Lgeneric_ti0
.Lgeneric_ti1:
	.long	".LRef.std.collection:ArrayList<default:A>.ti"-.Lgeneric_ti1
.Lgeneric_ti2:
	.long	".LRef.std.core:Array<default:A>.ti"-.Lgeneric_ti2
	.section	.cjgcflg,"dw"
	.byte	1
	.byte	1
	.byte	1

	.section	.cjrflp,"dw"
	.globl	default_default.packageInfo     # @default_default.packageInfo
	.p2align	4
.LRef.default_default.packageInfo:
default_default.packageInfo:
	.quad	0
	.long	1                               # 0x1
	.long	0                               # 0x0
	.long	0                               # 0x0
	.long	0                               # 0x0
	.quad	1                               # 0x1
.Ltmp119:
	.quad	.LRef.default.reflectStr-.Ltmp119
.Ltmp120:
	.quad	.LRef.default.reflectStr-.Ltmp120
.Ltmp121:
	.quad	.LRef.NullString.reflectStr-.Ltmp121
	.quad	0
	.quad	0
	.quad	"default:A.ti"

	.section	.cjrflv,"dw"
	.p2align	4                               # @"default:A.ti.method0"
".LRef.default:A.ti.method0":
"default:A.ti.method0":
.Ltmp122:
	.quad	.LRef.init.reflectStr-.Ltmp122
	.long	8                               # 0x8
	.short	0                               # 0x0
	.short	0                               # 0x0
	.quad	"_CN7default1A6<init>Hv"
	.quad	Unit.ti
	.quad	0
	.quad	"default:A.ti"
	.quad	0
	.quad	0

	.p2align	4                               # @"default:A.ti.reflect"
".LRef.default:A.ti.reflect":
"default:A.ti.reflect":
	.quad	0
	.long	1                               # 0x1
	.short	0                               # 0x0
	.short	0                               # 0x0
	.long	1                               # 0x1
	.long	0                               # 0x0
	.quad	0
	.quad	0
.Ltmp123:
	.quad	".LRef.default:A.ti.method0"-.Ltmp123

.LRef.init.reflectStr:                  # @init.reflectStr
init.reflectStr:
	.asciz	"init"

.LRef.default.reflectStr:               # @default.reflectStr
default.reflectStr:
	.asciz	"default"

.LRef.NullString.reflectStr:            # @NullString.reflectStr
NullString.reflectStr:
	.zero	1

	.section	.cjmetadata.type.ext,"dw"
	.section	.cjrflg,"dw"
	.p2align	4                               # @"ext_default:A.ti"
".LRef..Lext_default:A.ti":
".Lext_default:A.ti":
	.quad	"default:A.ti"
	.short	1                               # 0x1
	.zero	2
	.long	24                              # 0x18
	.byte	1                               # 0x1
	.zero	7


