// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GENERATION_FORWARD_BARRIER_H
#define MRT_GENERATION_FORWARD_BARRIER_H

#include "RSetUpdateBarrier.h"
#include "WCollector/ForwardBarrier.h"

namespace MapleRuntime {

class GenerationForwardBarrier : public ForwardBarrier {
public:
    explicit GenerationForwardBarrier(Collector& collector) : ForwardBarrier(collector) {}

    void WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const override
    {
        ForwardBarrier::WriteReference(obj, field, ref);
        rsetUpdateBarrier_.WriteReference(obj, reinterpret_cast<BaseObject*>(&field), ref);
    }

private:
    mutable RSetUpdateBarrier rsetUpdateBarrier_;
};

} // namespace MapleRuntime

#endif // MRT_GENERATION_FORWARD_BARRIER_H
