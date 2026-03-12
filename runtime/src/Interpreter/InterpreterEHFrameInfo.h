#ifndef MRT_INTERPRETER_EH_FRAMEINFO_H
#define MRT_INTERPRETER_EH_FRAMEINFO_H

#include "Exception/EhFrameInfo.h"
#include "Common/StackType.h"

namespace MapleRuntime {

class InterpreterEHFrameInfo : public IEHFrameInfo, public FrameInfo {
public:

    InterpreterEHFrameInfo(const FrameInfo& info, uintptr_t landingPad): FrameInfo(info), landingPad(landingPad)
    {
    }

    // forbid copying, objects are accessed through unique_ptr
    InterpreterEHFrameInfo(const InterpreterEHFrameInfo&) = delete;
    InterpreterEHFrameInfo& operator=(const InterpreterEHFrameInfo&) = delete;

    ~InterpreterEHFrameInfo() override = default;

    bool IsCatchException() const override {
        return true; // interpreter frame always catches exception and rethrow it in landing pad if no suitable catch block is found
    }

    uint64_t GetTTypeIndex() const override {
        return 0;
    }

    uintptr_t GetLandingPad() const override {
        return landingPad;
    }

    void RestoreToCallerContext(CalleeSavedRegisterContext& context, uint32_t adjustedSize = 0) const override {
        // this method should not be called because interpreted frame always catches exception
        LOG(RTLOG_FATAL, "IntepreterEHFrameInfo::RestoreToCallerContext should not be called\n");
        DLOG_INTERPRETER(DEBUG, "IntepreterEHFrameInfo::RestoreToCallerContext should not be called\n");
    }

    const uint32_t* GetIP() const override {
        return mFrame.GetIP();
    }

    FrameAddress* GetFA() const override {
        return mFrame.GetFA();
    }

    MachineFrame GetMachineFrame() const override {
        return mFrame;
    }

    const CString GetFunctionName() const override { return FrameInfo::GetFuncName(); }

private:

    uintptr_t landingPad;
};

} // namespace MapleRuntime
#endif // MRT_INTERPRETER_EH_FRAMEINFO_H