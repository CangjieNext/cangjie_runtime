// Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_EXCLUSIVE_SCOPE_EH_FRAMEINFO_H
#define MRT_EXCLUSIVE_SCOPE_EH_FRAMEINFO_H

#include "Base/CString.h"
#include "Base/Log.h"
#include "Base/Types.h"
#include "CalleeSavedRegisterContext.h"
#include "Common/StackType.h"
#include "EhFrameInfo.h"

#ifdef __aarch64__
#include "Common/RegisterAarch64.h"
#elif defined(__arm__)
#include "Common/RegisterArm.h"
#else
#include "Common/RegisterX86-64.h"
#endif

namespace MapleRuntime {
using namespace Register;

// EH frame for CJ_MCC_ExclusiveScope (ExclusiveScope.S).
//
// RestoreToCallerContext unwinds through both the OS exclusive bridge and the
// CJ exclusive stub in one step: the bridge has no callee-saved spills; the stub
// holds RecordStubCalleeSaved slots. After AutoEnv restore, context.FP is the
// bridge; this restore keys off the stub FA only and leaves context as if
// bridge+stub had returned into exclusiveScope.
class ExclusiveScopeEHFrameInfo : public IEHFrameInfo, public FrameInfo {
public:
    explicit ExclusiveScopeEHFrameInfo(const FrameInfo& info) : FrameInfo(info) {}

    ExclusiveScopeEHFrameInfo(const ExclusiveScopeEHFrameInfo&) = delete;
    ExclusiveScopeEHFrameInfo& operator=(const ExclusiveScopeEHFrameInfo&) = delete;

    ~ExclusiveScopeEHFrameInfo() override = default;

    bool IsCatchException() const override { return false; }

    uint64_t GetTTypeIndex() const override { return 0; }

    uintptr_t GetLandingPad() const override { return 0; }

    void RestoreToCallerContext(CalleeSavedRegisterContext& context, uint32_t adjustedSize) const override
    {
        if (UNLIKELY(adjustedSize != 0)) {
            LOG(RTLOG_FATAL, "ExclusiveScopeEHFrameInfo: unexpected adjustedSize %u", adjustedSize);
        }
        FrameAddress* stubFa = mFrame.GetFA();
        if (UNLIKELY(stubFa == nullptr)) {
            LOG(RTLOG_FATAL, "ExclusiveScopeEHFrameInfo: null stub FA");
        }

        FrameAddress* callerFa = stubFa->callerFrameAddress;
        if (UNLIKELY(callerFa == nullptr)) {
            LOG(RTLOG_FATAL, "ExclusiveScopeEHFrameInfo: null caller FA for stub FA %p", stubFa);
        }

        RestoreArch(context, stubFa, callerFa);
    }

    const uint32_t* GetIP() const override { return mFrame.GetIP(); }

    FrameAddress* GetFA() const override { return mFrame.GetFA(); }

    MachineFrame GetMachineFrame() const override { return mFrame; }

    const CString GetFunctionName() const override { return CString("exclusiveScopeImpl"); }

private:
    // Layout constants match ExclusiveScope.S / RegRoot::RecordStubCalleeSaved.
    // OFFSET/STRIDE are signed so x86 (spills below FA) and ARM (above FA) share code.
#if defined(__x86_64__)
#ifdef _WIN64
    static constexpr uint32_t CALLEE_SAVED_CONTEXT_COUNT = 17;
#else
    static constexpr uint32_t CALLEE_SAVED_CONTEXT_COUNT = 5;
#endif
    static constexpr Sptr CALLEE_SAVED_OFFSET = -static_cast<Sptr>(sizeof(Uptr));
    static constexpr Sptr CALLEE_SAVED_SLOT_STRIDE = -static_cast<Sptr>(sizeof(Uptr));
    // FA slot + return-address slot above the stub FA.
    static constexpr Uptr POST_RETURN_SP_DELTA = 2 * sizeof(void*);
#elif defined(__aarch64__)
    static constexpr uint32_t CALLEE_SAVED_CONTEXT_COUNT = 20;
    static constexpr Sptr CALLEE_SAVED_OFFSET = 0x20;
    static constexpr Sptr CALLEE_SAVED_SLOT_STRIDE = sizeof(Uptr);
    // ExclusiveScopeFrameSize in ExclusiveScope.S (post-deallocate SP = FA + frame).
    static constexpr Uptr POST_RETURN_SP_DELTA = 0x70;
#elif defined(__arm__)
    static constexpr uint32_t CALLEE_SAVED_CONTEXT_COUNT = 17;
    static constexpr Sptr CALLEE_SAVED_OFFSET = 0x10;
    static constexpr Sptr CALLEE_SAVED_SLOT_STRIDE = sizeof(ArchUInt);
    // ExclusiveScopeFrameSize in ExclusiveScope.S (post-deallocate SP = FA + frame).
    static constexpr Uptr POST_RETURN_SP_DELTA = 0x30;
#endif

    template <size_t N>
    static void RestoreStubCalleeSaved(CalleeSavedRegisterContext& context, Uptr stubFaAddr,
                                       const RegisterId (&regs)[N])
    {
        Uptr slotAddr = stubFaAddr + CALLEE_SAVED_OFFSET;
        for (auto reg : regs) {
            context.SetValueByIdx(CalleeSavedContextIdx(reg), *reinterpret_cast<ArchUInt*>(slotAddr));
            slotAddr += CALLEE_SAVED_SLOT_STRIDE;
        }
    }

#if defined(__x86_64__)
    static void RestoreArch(CalleeSavedRegisterContext& context, FrameAddress* stubFa, FrameAddress* callerFa)
    {
        Uptr stubFaAddr = reinterpret_cast<Uptr>(stubFa);
        // Spill order / layout matches RegRoot::RecordStubCalleeSaved in StackMapX86.h.
#ifdef _WIN64
        static constexpr RegisterId calleeSavedRegisters[] = { R15, R14, R13, R12, RDI, RSI, RBX };
#else
        static constexpr RegisterId calleeSavedRegisters[] = { R15, R14, R13, R12, RBX };
#endif
        RestoreStubCalleeSaved(context, stubFaAddr, calleeSavedRegisters);
        // Match stub epilogue after `pop %rbp; ret`: FP = exclusiveScope FA, SP =
        // just above the stub's return-address slot (caller SP after the call).
        context.rbp = reinterpret_cast<uint64_t>(callerFa);
        context.rsp = stubFaAddr + POST_RETURN_SP_DELTA;
    }
#elif defined(__aarch64__)
    static void RestoreArch(CalleeSavedRegisterContext& context, FrameAddress* stubFa, FrameAddress* callerFa)
    {
        Uptr stubFaAddr = reinterpret_cast<Uptr>(stubFa);
        // Spill order / layout matches RegRoot::RecordStubCalleeSaved in StackMapAarch64.h.
        static constexpr RegisterId calleeSavedRegisters[] = { X19, X20, X21, X22, X23, X24, X25, X26, X27, X28 };
        RestoreStubCalleeSaved(context, stubFaAddr, calleeSavedRegisters);
        // Match stub epilogue after `ldp x29,x30,[sp],#frame`: FP/LR = caller's
        // FA and return address; SP = stub FA + frame size (post-deallocate).
        context.x29 = reinterpret_cast<uint64_t>(callerFa);
        context.x30 = reinterpret_cast<uint64_t>(stubFa->returnAddress);
        context.sp = stubFaAddr + POST_RETURN_SP_DELTA;
    }
#elif defined(__arm__)
    static void RestoreArch(CalleeSavedRegisterContext& context, FrameAddress* stubFa, FrameAddress* callerFa)
    {
        Uptr stubFaAddr = reinterpret_cast<Uptr>(stubFa);
        // Spill order / layout matches RegRoot::RecordStubCalleeSaved in StackMapArm.h.
        static constexpr RegisterId calleeSavedRegisters[] = { R4, R5, R6, R7, R8, R9, R10 };
        RestoreStubCalleeSaved(context, stubFaAddr, calleeSavedRegisters);
        // Match stub epilogue after restoring fp/lr and `add sp,#ExclusiveScopeFrameSize`:
        // FP/LR = caller's FA and return address; SP = stub FA + frame size.
        context.r11 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(callerFa));
        context.lr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(stubFa->returnAddress));
        context.sp = static_cast<uint32_t>(stubFaAddr + POST_RETURN_SP_DELTA);
    }
#else
    static void RestoreArch(CalleeSavedRegisterContext&, FrameAddress*, FrameAddress*)
    {
        LOG(RTLOG_FATAL, "ExclusiveScopeEHFrameInfo: unsupported architecture");
    }
#endif

    // Map RegisterId → CalleeSavedRegisterContext index (Register::ResolveCalleeSaved order).
    static uint32_t CalleeSavedContextIdx(RegisterId reg)
    {
        for (uint32_t idx = 0; idx < CALLEE_SAVED_CONTEXT_COUNT; ++idx) {
            if (ResolveCalleeSaved(idx) == reg) {
                return idx;
            }
        }
        LOG(RTLOG_FATAL, "ExclusiveScopeEHFrameInfo: register %u is not callee-saved",
            static_cast<uint32_t>(reg));
        return 0;
    }
};

} // namespace MapleRuntime
#endif // MRT_EXCLUSIVE_SCOPE_EH_FRAMEINFO_H
