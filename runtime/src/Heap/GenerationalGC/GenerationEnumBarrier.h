// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GENERATION_ENUM_BARRIER_H
#define MRT_GENERATION_ENUM_BARRIER_H

#include "RSetUpdateBarrier.h"
#include "WCollector/EnumBarrier.h"

namespace MapleRuntime {

class GenerationEnumBarrier : public EnumBarrier {
public:
    explicit GenerationEnumBarrier(Collector& collector) : EnumBarrier(collector) {}

    void WriteReference(BaseObject* obj, RefField<false>& field, BaseObject* ref) const override
    {
        EnumBarrier::WriteReference(obj, field, ref);
        rsetUpdateBarrier_.WriteReference(obj, reinterpret_cast<BaseObject*>(&field), ref);
    }

private:
    mutable RSetUpdateBarrier rsetUpdateBarrier_;
};

} // namespace MapleRuntime

#endif // MRT_GENERATION_ENUM_BARRIER_H
