// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "ThreadLocal.h"

#include "Common/Runtime.h"
#include "Heap/Collector/Collector.h"
#include "Mutator.h"
#include "schedule.h"
#include "Base/Globals.h"
#include <type_traits>

#ifdef MRT_USE_MARK_SWEEP
#include "Heap/MAllocator/MSRegionInfo.h"
#endif

namespace MapleRuntime {
namespace {
inline uint8_t EncodeThreadLocalGCPhaseByte(GCPhase phase)
{
    // Fix: directly cast phase to uint8_t so GC_PHASE_IDLE(1) encodes to 1,
    // matching the LLVM fast-path constant MSGCGCPhaseIdle=1.
    // GC_PHASE_UNDEF=0 naturally encodes to 0, keeping fast-path disabled
    // for uninitialized mutators.
    return static_cast<uint8_t>(phase);
}
}

RwLock ThreadLocal::tlEnableLock;
MRT_EXPORT thread_local uint64_t threadLocalData[sizeof(ThreadLocalData) / sizeof(uint64_t)];
thread_local CleanThreadLocalData cleaner;

void ThreadLocal::SetMutator(Mutator* newMutator)
{
    ThreadLocalData* local = GetThreadLocalData();
    local->mutator = newMutator;
    GCPhase phase = newMutator != nullptr ? newMutator->GetMutatorPhase() : GCPhase::GC_PHASE_IDLE;
    local->encodedGCPhase = EncodeThreadLocalGCPhaseByte(phase);
}

CleanThreadLocalData::~CleanThreadLocalData()
{
    if (!ThreadLocal::TryGetRdLock()) {
        return;
    }

    ThreadLocalData* local = ThreadLocal::GetThreadLocalData();
    if (Runtime::CurrentRef() == nullptr ||
        local->isCJProcessor || local->foreignCJThread == nullptr) {
        ThreadLocal::UnlockRdLock();
        return;
    }

    CJForeignThreadExit(reinterpret_cast<CJThreadHandle>(local->foreignCJThread));
    ThreadLocal::UnlockRdLock();
}

extern "C" void MCC_CheckThreadLocalDataOffset()
{
    static_assert(std::is_standard_layout<ThreadLocalData>::value,
                  "ThreadLocalData must remain standard-layout for compiler ABI checks");
    static_assert(offsetof(ThreadLocalData, buffer) == 0,
                  "need to modify the offset of this value in llvm-project and cjthread at the same time");
    static_assert(offsetof(ThreadLocalData, mutator) == sizeof(void*),
                  "need to modify the offset of this value in llvm-project and cjthread at the same time");
    static_assert(offsetof(ThreadLocalData, cjthread) == sizeof(void*) * 2,
                  "need to modify the offset of this value in llvm-project and cjthread at the same time");
    static_assert(offsetof(ThreadLocalData, schedule) == sizeof(void*) * 3,
                  "need to modify the offset of this value in llvm-project and cjthread at the same time");
    static_assert(offsetof(ThreadLocalData, preemptFlag) == sizeof(void*) * 4,
                  "need to modify the offset of this value in llvm-project and cjthread at the same time");
    static_assert(offsetof(ThreadLocalData, protectAddr) == sizeof(void*) * 5,
                  "need to modify the offset of this value in llvm-project and cjthread at the same time");
    static_assert(offsetof(ThreadLocalData, safepointState) == sizeof(void*) * 6,
                  "need to modify the offset of this value in llvm-project and cjthread at the same time");
#if defined(__arm__)
    static_assert(offsetof(ThreadLocalData, tid) == sizeof(void*) * 6 + sizeof(uint64_t),
                  "need to modify the offset of this value in llvm-project and cjthread at the same time");
    static_assert(offsetof(ThreadLocalData, foreignCJThread) == sizeof(void*) * 6 + sizeof(uint64_t) * 2,
                  "need to modify the offset of this value in llvm-project and cjthread at the same time");
    static_assert(sizeof(ThreadLocalData) == sizeof(void*) * 10 + sizeof(uint64_t) * 2 + sizeof(void*),
                  "need to modify the offset of this value in llvm-project and cjthread at the same time");
#else    
    static_assert(offsetof(ThreadLocalData, tid) == sizeof(void*) * 7,
                  "need to modify the offset of this value in llvm-project and cjthread at the same time");
    static_assert(offsetof(ThreadLocalData, foreignCJThread) == sizeof(void*) * 8,
                  "need to modify the offset of this value in llvm-project and cjthread at the same time");
    static_assert(offsetof(ThreadLocalData, encodedGCPhase) == sizeof(void*) * 9 + 6,
                  "need to modify the offset of this value in llvm-project at the same time");
    static_assert(offsetof(ThreadLocalData, msAllocBuffer) == sizeof(void*) * 11,
                  "need to modify the offset of this value in llvm-project at the same time");
    static_assert(offsetof(ThreadLocalData, msAllocBuffer) == 88,
                  "compiler ABI: AArch64/X86 ThreadLocalData::msAllocBuffer");
    static_assert(sizeof(ThreadLocalData) == sizeof(void*) * 12,
                  "need to modify the offset of this value in llvm-project and cjthread at the same time");
#endif

#ifdef MRT_USE_MARK_SWEEP
    static_assert(offsetof(MSRegionInfo, allocPtr) == 0,
                  "need to modify the offset of this value in llvm-project at the same time");
    static_assert(offsetof(MSRegionInfo, regionEnd) == 8,
                  "need to modify the offset of this value in llvm-project at the same time");
    static_assert(offsetof(MSRegionInfo, slotOrLargeMeta) == 24,
                  "need to modify the offset of this value in llvm-project at the same time");
    static_assert(offsetof(MSRegionInfo, collectEpoch) == 32,
                  "need to modify the offset of this value in llvm-project at the same time");
    static_assert(offsetof(MSRegionInfo, control) == 36,
                  "need to modify the offset of this value in llvm-project at the same time");
    static_assert(offsetof(MSRegionInfo, allocSummary) == 40,
                  "need to modify the offset of this value in llvm-project at the same time");
    static_assert(offsetof(MSRegionInfo, allocMode) == 60,
                  "need to modify the offset of this value in llvm-project at the same time");
    static_assert(offsetof(MSRegionInfo, sizeClass) == 62,
                  "need to modify the offset of this value in llvm-project at the same time");
    static_assert(offsetof(MSRegionInfo, layout) == 63,
                  "need to modify the offset of this value in llvm-project at the same time");
#endif
}

#ifdef __APPLE__
extern "C" MRT_EXPORT void MRT_CheckThreadLocalDataOffset();
__asm__(
    ".global _MRT_CheckThreadLocalDataOffset\n\t.set _MRT_CheckThreadLocalDataOffset, _MCC_CheckThreadLocalDataOffset");
extern "C" MRT_EXPORT void CJ_MCC_CheckThreadLocalDataOffset();
__asm__(
    ".global _CJ_MCC_CheckThreadLocalDataOffset\n\t.set _CJ_MCC_CheckThreadLocalDataOffset, "
    "_MCC_CheckThreadLocalDataOffset");
#else
extern "C" MRT_EXPORT void MRT_CheckThreadLocalDataOffset() __attribute__((alias("MCC_CheckThreadLocalDataOffset")));
extern "C" MRT_EXPORT void CJ_MCC_CheckThreadLocalDataOffset() __attribute__((alias("MCC_CheckThreadLocalDataOffset")));
#endif
} // namespace MapleRuntime
