// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_HEAP_MANAGER_H
#define MRT_HEAP_MANAGER_H

#include "Base/Macros.h"
#include "Common/TypeDef.h"
#include "Heap/Collector/GcRequest.h"
#include "RuntimeConfig.h"

namespace MapleRuntime {
struct ThreadLocalData;
#ifdef MRT_USE_MARK_SWEEP
enum class MSScanClass : uint8_t;
#endif

// replace this for Heap.
class HeapManager {
public:
    HeapManager();
    ~HeapManager() = default;

    // runtime required lifecycle interfaces
    void Init(const HeapParam& param);
    void Fini();

    // alloc returns a memory address, not an "object" pointer that has not been initialized
    static MAddress Allocate(size_t allocSize, AllocType allocType = AllocType::MOVEABLE_OBJECT);
#ifdef MRT_USE_MARK_SWEEP
    static MAddress AllocateMS(size_t allocSize, MSScanClass scanClass,
                               ThreadLocalData* local);
#endif
    static inline void RequestGC(GCReason reason, bool async);
};
} // namespace MapleRuntime
#endif // MRT_HEAP_MANAGER_H
