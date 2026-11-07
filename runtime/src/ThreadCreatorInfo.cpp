// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "ThreadCreatorInfo.h"

#include "Common/StackType.h"
#include "Mutator/Mutator.h"
#include "ObjectModel/MFuncDesc.h"
#include "UnwindStack/PrintStackInfo.h"

namespace MapleRuntime {

namespace {

// Bounded unwind: walk from the current (MCC entry) frame and stop at the first MANAGED
// frame. The loop body mirrors PrintStackInfo::FillInStackTrace (including the _WIN64
// UnwindToCallerContext variant and the classification-state updates that
// AnalyseAndSetFrameType depends on) but stops at the first managed frame and does not
// collect into the base vector. NOTE: the inherited StackInfo base constructor still
// performs its usual 32-entry vector reserve (~1.8KB) per capture - accepted per the
// minimalist-diff principle of not touching shared unwind headers for this feature.
class BoundedStackInfo : public PrintStackInfo {
public:
    BoundedStackInfo() : PrintStackInfo() {}

    void FillInStackTrace() override
    {
        constexpr uint32_t kMaxUnwindSteps = 16; // defensive bound; MCC entry is 1-2 frames away
        uint32_t steps = 0;
        UnwindContext uwContext;
        // Top unwind context can only be runtime or Cangjie context.
        CheckTopUnwindContextAndInit(uwContext);
        while (!uwContext.frameInfo.mFrame.IsAnchorFrame(anchorFA)) {
            if (steps++ >= kMaxUnwindSteps) {
                return; // give up
            }
            AnalyseAndSetFrameType(uwContext);
            FrameType currentFrameType = uwContext.frameInfo.GetFrameType();
            if (currentFrameType == FrameType::MANAGED) {
                firstManagedFrame = uwContext.frameInfo; // member copy, never a loop-local
                hasFirstManaged = true;
                return; // stop at the first managed frame
            }
#ifdef INTERPRETER_ENABLED
            if (currentFrameType == FrameType::INTERPRETER) {
                return; // the immediate caller is interpreted: give up, do not fall through
            }
#endif
            UnwindContext caller;
            lastFrameType = uwContext.frameInfo.GetFrameType();
#ifndef _WIN64
            if (uwContext.UnwindToCallerContext(caller) == false) {
#else
            if (uwContext.UnwindToCallerContext(caller, uwCtxStatus) == false) {
#endif
                return; // give up
            }
            uwContext = caller;
        }
    }

    FrameInfo* FirstManagedFrame()
    {
        return hasFirstManaged ? &firstManagedFrame : nullptr;
    }

private:
    FrameInfo firstManagedFrame{};
    bool hasFirstManaged = false;
};

// FuncDesc of the capture frame, platform-split the same way as
// StackMetadataHelper::ResolveFuncDesc / StackInfo::ExtractLiteFrameInfoFromStack.
uint64_t ResolveFrameFuncDesc(const FrameInfo& frameInfo)
{
#ifdef __APPLE__
    return reinterpret_cast<uint64_t>(MFuncDesc::GetFuncDesc(frameInfo.mFrame.GetFA()));
#else
    return reinterpret_cast<uint64_t>(MFuncDesc::GetFuncDesc(reinterpret_cast<Uptr>(frameInfo.GetFuncStartPC())));
#endif
}

} // namespace

CreatorFrameInfo CaptureCreator()
{
    // The parent thread's unwind context must be RELIABLE, otherwise give up (thread
    // creation itself is unchanged).
    Mutator* mutator = Mutator::GetMutator();
    if (mutator == nullptr ||
        mutator->GetUnwindContext().GetUnwindContextStatus() != UnwindContextStatus::RELIABLE) {
        return CreatorFrameInfo{};
    }

    BoundedStackInfo stackInfo;
    stackInfo.FillInStackTrace();
    FrameInfo* frame = stackInfo.FirstManagedFrame();
    if (frame == nullptr) {
        return CreatorFrameInfo{}; // stop rules: interpreter caller / anchor / unwind failure / bound
    }

    // Verbatim copy of the frame's identity triple - the same three values the ordinary
    // fill path stores for every frame (StackInfo.cpp ExtractLiteFrameInfoFromStack).
    // No decoding, no allocation, no residency lookup: the values are decoded later,
    // at print time, exactly like an ordinary frame's values.
    CreatorFrameInfo info;
    info.ip = reinterpret_cast<uintptr_t>(frame->mFrame.GetIP());
    info.funcStart = reinterpret_cast<uintptr_t>(frame->GetFuncStartPC());
    info.funcDesc = ResolveFrameFuncDesc(*frame);
    if (info.funcDesc == 0) {
        return CreatorFrameInfo{}; // cannot resolve the descriptor: give up, retry next spawn
    }
    return info;
}

} // namespace MapleRuntime
