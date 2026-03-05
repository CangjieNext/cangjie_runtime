// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "CollectorProxy.h"

namespace MapleRuntime {
void CollectorProxy::Init()
{
#ifdef MRT_USE_MARK_SWEEP
    msCollector.Init();
    if (currentCollector == nullptr) {
        currentCollector = &msCollector;
    }
#else
    wCollector.Init();
    if (currentCollector == nullptr) {
        currentCollector = &wCollector;
    }
#endif
}

void CollectorProxy::Fini()
{
#ifdef MRT_USE_MARK_SWEEP
    msCollector.Fini();
#else
    wCollector.Fini();
#endif
}

void CollectorProxy::RunGarbageCollection(uint64_t gcIndex, GCReason reason)
{
#ifdef MRT_USE_MARK_SWEEP
    currentCollector = &msCollector;
#else
    switch (reason) {
        case GC_REASON_HEU:
        case GC_REASON_BACKUP:
            currentCollector = &wCollector;
            break;
        case GC_REASON_OOM:
        case GC_REASON_FORCE:
            currentCollector = &wCollector;
            break;
        default:
            currentCollector = &wCollector;
            break;
    }
#endif
    currentCollector->SetGcStarted(true);
    currentCollector->RunGarbageCollection(gcIndex, reason);
}
} // namespace MapleRuntime
