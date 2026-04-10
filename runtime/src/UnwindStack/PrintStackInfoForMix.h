// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_PRINT_STACKINFO_FOR_MIX_H
#define MRT_PRINT_STACKINFO_FOR_MIX_H

#include "Base/LogFile.h"
#include "StackInfo.h"

namespace MapleRuntime {

enum class StepFrameType : uint8_t {
    NATIVE_FRAME = 0,
    JS_FRAME = 1,
    STATIC_JS_FRAME = 2,
};

struct ArkStepParam {
    uintptr_t* fp;
    uintptr_t* sp;
    uintptr_t* pc;
    bool* isJsFrame;
    StepFrameType* frameType;
    uint64_t frameIndex;
};

typedef bool (*ReadMemFunc)(void* ctx, uintptr_t addr, uintptr_t* val);
typedef int (*StepArkFunc)(void* ctx, ReadMemFunc readMem, ArkStepParam* arkStepParam);

class PrintStackInfoForMix : public StackInfo {
public:
    explicit PrintStackInfoForMix(const UnwindContext* context = nullptr) : StackInfo(context) {}
    ~PrintStackInfoForMix() override = default;
    void FillInStackTrace() override;

private:
    bool UnwindToCallerArkTSFrame(UnwindContext& uwContext);
    static StepArkFunc LoadArkTSStepArk();
    static bool ShouldStopUnwind(const MachineFrame& frame);
};

} // namespace MapleRuntime

#endif // MRT_PRINT_STACKINFO_FOR_MIX_H
