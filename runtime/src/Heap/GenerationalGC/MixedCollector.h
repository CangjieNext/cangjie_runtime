// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information


#ifndef MRT_MIXEDCOLLECTOR_H
#define MRT_MIXEDCOLLECTOR_H

#include "YoungCollector.h"

namespace MapleRuntime {

class MixedCollector : public YoungCollector {
public:
    explicit MixedCollector(Allocator& allocator, CollectorResources& resources)
        : YoungCollector(allocator, resources) {
        collectorType = CollectorType::MIXED_COLLECTOR;
    }

    ~MixedCollector() override = default;

    void DoGarbageCollection() override;
    void EnumRefFieldRoot(RefField<>& ref, RootSet& rootSet) const override;
    void EnumAndTagRawRoot(ObjectRef& ref, RootSet& rootSet) const override;
    RefField<> GetAndTryTagRefField(BaseObject* target) const override;
    BaseObject* ForwardObject(BaseObject* fromVersion) override;
    bool MarkObject(BaseObject* obj) const override;
    bool IsTargetAddress(BaseObject* obj) const override;
    void AssembleCandidates() override;
    void ConcurrentSweep() override;
protected:
    void ResetForwardingData() override;
};

} // namespace MapleRuntime

#endif // MRT_MIXEDCOLLECTOR_H
