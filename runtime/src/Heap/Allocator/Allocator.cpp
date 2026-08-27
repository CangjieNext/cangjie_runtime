// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "Allocator.h"

#include <cinttypes>
#include <cstdint>
#ifdef _WIN64
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "Base/ImmortalWrapper.h"
#include "Common/BaseObject.h"
#include "Common/PageAllocator.h"
#include "Heap/Allocator/RegionInfo.h"
#ifdef MRT_USE_MARK_SWEEP
#include "Heap/MAllocator/MSRegionSpace.h"
#else
#include "Heap/Allocator/RegionSpace.h"
#endif
#include "Mutator/ThreadLocal.h"
#include "ObjectModel/MObject.h"

namespace MapleRuntime {
using namespace std;

// ============================================================================
// Global constants shared by both CopyGC and MarkSweep modes.
// Moved here from RegionManager.cpp so that RegionManager.cpp can be excluded
// from the MarkSweep build.
// ============================================================================

uintptr_t RegionInfo::UnitInfo::totalUnitCount = 0;
uintptr_t RegionInfo::UnitInfo::heapStartAddress = 0;

static size_t GetPageSize() noexcept
{
#if defined(_WIN64)
    SYSTEM_INFO systeminfo;
    GetSystemInfo(&systeminfo);
    if (systeminfo.dwPageSize != 0) {
        return systeminfo.dwPageSize;
    } else {
        // default page size is 4KB if get system page size failed.
        return 4 * KB;
    }
#elif defined(__APPLE__)
    // default page size is 16KB in MacOS
    return 16 * KB;
#else
    return getpagesize();
#endif
}

// System default page size
const size_t MRT_PAGE_SIZE = GetPageSize();
const size_t AllocatorUtils::ALLOC_PAGE_SIZE = MapleRuntime::MRT_PAGE_SIZE;
// region unit size: same as system page size
const size_t RegionInfo::UNIT_SIZE = MapleRuntime::MRT_PAGE_SIZE;
// regarding a object as a large object when the size is greater than 32KB or one page size,
// depending on the system page size.
const size_t RegionInfo::LARGE_OBJECT_DEFAULT_THRESHOLD = MapleRuntime::MRT_PAGE_SIZE > (32 * KB) ?
                                                            MapleRuntime::MRT_PAGE_SIZE : 32 * KB;
Allocator::Allocator() = default;

// PageAlllocator
// the input parameter cat should be guaranteed in the range of value of enum type AllocationTag by
// external invoker, in order to avoid exceed the border of matrix
AggregateAllocator& AggregateAllocator::Instance(AllocationTag tag)
{
    static ImmortalWrapper<AggregateAllocator> instance[MAX_ALLOCATION_TAG];
    return *(instance[tag]);
}

// PagePool
PagePool& PagePool::Instance() noexcept
{
    static ImmortalWrapper<PagePool> instance("PagePool");
    return *instance;
}

Allocator* Allocator::NewAllocator()
{
#ifdef MRT_USE_MARK_SWEEP
    RegionSpaceMS* regionSpaceMS = new (std::nothrow) RegionSpaceMS();
    CHECK_DETAIL(regionSpaceMS != nullptr, "New RegionSpaceMS failed");
    return regionSpaceMS;
#else
    RegionSpace* regionSpace = new (std::nothrow) RegionSpace();
    CHECK_DETAIL(regionSpace != nullptr, "New RegionSpace failed");
    return regionSpace;
#endif
}
} // namespace MapleRuntime
