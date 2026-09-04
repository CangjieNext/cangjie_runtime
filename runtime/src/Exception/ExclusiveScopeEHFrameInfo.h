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

namespace MapleRuntime {

// EH frame for CJ_MCC_ExclusiveScope (ExclusiveScope.S).
// The walker only visits the OS bridge; unwrap bridgeFA → stub FA, then restore
// FP/SP as if the stub epilogue ran (heap SP, managed caller FP).
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
        FrameAddress* bridgeFa = mFrame.GetFA();
        if (UNLIKELY(bridgeFa == nullptr)) {
            LOG(RTLOG_FATAL, "ExclusiveScopeEHFrameInfo: null bridge FA");
        }
        FrameAddress* stubFa = bridgeFa->callerFrameAddress;
        if (UNLIKELY(stubFa == nullptr)) {
            LOG(RTLOG_FATAL, "ExclusiveScopeEHFrameInfo: null stub FA for bridge FA %p", bridgeFa);
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
#if defined(__x86_64__)
    static void RestoreArch(CalleeSavedRegisterContext& context, FrameAddress* stubFa, FrameAddress* callerFa)
    {
        Uptr stubFaAddr = reinterpret_cast<Uptr>(stubFa);
        // Match stub epilogue after `pop %rbp; ret`: FP = exclusiveScope FA, SP =
        // just above the stub's return-address slot (caller SP after the call).
        context.rbp = reinterpret_cast<uint64_t>(callerFa);
        // FA slot + return-address slot above the stub FA.
        context.rsp = stubFaAddr + 2 * sizeof(void*);
    }
#elif defined(__aarch64__)
    static void RestoreArch(CalleeSavedRegisterContext& context, FrameAddress* stubFa, FrameAddress* callerFa)
    {
        Uptr stubFaAddr = reinterpret_cast<Uptr>(stubFa);
        // Match stub epilogue after `ldp x29,x30,[sp],#frame`: FP/LR = caller's
        // FA and return address; SP = stub FA + frame size (post-deallocate).
        context.x29 = reinterpret_cast<uint64_t>(callerFa);
        context.x30 = reinterpret_cast<uint64_t>(stubFa->returnAddress);
        // ExclusiveScopeFrameSize in ExclusiveScope.S (post-deallocate SP = FA + frame).
        context.sp = stubFaAddr + 0x10;
    }
#elif defined(__arm__)
    static void RestoreArch(CalleeSavedRegisterContext& context, FrameAddress* stubFa, FrameAddress* callerFa)
    {
        Uptr stubFaAddr = reinterpret_cast<Uptr>(stubFa);
        // Match stub epilogue after restoring fp/lr and `add sp,#ExclusiveScopeFrameSize`:
        // FP/LR = caller's FA and return address; SP = stub FA + frame size.
        context.r11 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(callerFa));
        context.lr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(stubFa->returnAddress));
        // ExclusiveScopeFrameSize in ExclusiveScope.S (post-deallocate SP = FA + frame).
        context.sp = static_cast<uint32_t>(stubFaAddr + 8);
    }
#else
    static void RestoreArch(CalleeSavedRegisterContext&, FrameAddress*, FrameAddress*)
    {
        LOG(RTLOG_FATAL, "ExclusiveScopeEHFrameInfo: unsupported architecture");
    }
#endif
};

} // namespace MapleRuntime
#endif // MRT_EXCLUSIVE_SCOPE_EH_FRAMEINFO_H
