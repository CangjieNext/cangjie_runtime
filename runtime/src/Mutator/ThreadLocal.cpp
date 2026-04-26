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

namespace MapleRuntime {
namespace {
inline uint8_t EncodeThreadLocalGCFastState(GCPhase phase)
{
    switch (phase) {
        case GCPhase::GC_PHASE_MS_MARK_SETUP:
        case GCPhase::GC_PHASE_MS_MARK_TERMINATION:
            return ThreadLocal::GC_FAST_STATE_MS_MARKING;
        case GCPhase::GC_PHASE_MS_CONCURRENT_MARK:
            return ThreadLocal::GC_FAST_STATE_MS_MARKING | ThreadLocal::GC_FAST_STATE_MS_ASSIST;
        default:
            return ThreadLocal::GC_FAST_STATE_NONE;
    }
}

inline uint8_t EncodeThreadLocalGCPhaseByte(GCPhase phase)
{
    return phase > GCPhase::GC_PHASE_INIT ? static_cast<uint8_t>(phase) : 0;
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
    local->gcFastState = EncodeThreadLocalGCFastState(phase);
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
    static_assert(sizeof(ThreadLocalData) == sizeof(void*) * 12,
                  "need to modify the offset of this value in llvm-project and cjthread at the same time");
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
