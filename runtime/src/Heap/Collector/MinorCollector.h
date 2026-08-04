// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
#ifndef MRT_MINOR_COLLECTOR_H
#define MRT_MINOR_COLLECTOR_H

#include "TracingCollector.h"
#include "GenGCConfig.h"

namespace MapleRuntime {

class MinorCollector : public TracingCollector {
public:
    explicit MinorCollector(Allocator& allocator, CollectorResources& resources, GenGCConfig& config)
        : TracingCollector(allocator, resources), genConfig(config)
    {
        collectorType = CollectorType::MINOR_COLLECTOR;
    }

    ~MinorCollector() override = default;

    void Init() override;
    void RunGarbageCollection(uint64_t gcIndex, GCReason reason) override;

private:
    void ScanRoots(WorkStack& workStack);
    void ForwardSurvivors(WorkStack& workStack);
    bool ShouldPromote(BaseObject* obj);
    void PromoteToOldGen(BaseObject* obj);
    void SwapSurvivorSpaces();

    GenGCConfig& genConfig;
    RegionList youngSurvivorRegions;
    size_t survivedSize = 0;
    uint64_t minorGCIndex = 0;
};

} // namespace MapleRuntime
#endif