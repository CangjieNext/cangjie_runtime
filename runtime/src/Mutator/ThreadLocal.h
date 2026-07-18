// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_THREAD_LOCAL_H
#define MRT_THREAD_LOCAL_H

#include <cstdint>
#include "Base/RwLock.h"

namespace MapleRuntime {
class AllocBuffer;
class AllocBufferMS;
class Mutator;

enum class ThreadType { CJ_PROCESSOR = 0, GC_THREAD, FP_THREAD, MSGC_BACKGROUND_THREAD, HOT_UPDATE_THREAD };

// Backend and CJThread will use external tls var through offset calculation, so external tls
// must in the first place, followed by the internal tls.
struct ThreadLocalData {
    // External thread local var.
    AllocBuffer* buffer;
    Mutator* mutator;
    uint8_t* cjthread;
    uint8_t* schedule;
    uint8_t* preemptFlag;
    uint8_t* protectAddr;
    uint64_t safepointState;
    uint64_t tid;
    void* foreignCJThread;
    // Internal thread local var.
    ThreadType threadType;
    bool isCJProcessor;
    // Preserve the historical ABI layout around encodedGCPhase for generated code.
    uint8_t reserved0;
    uint8_t encodedGCPhase;
    uint8_t reserved1;
    void* threadCache;
    AllocBufferMS* msAllocBuffer;
};

struct CleanThreadLocalData {
    ~CleanThreadLocalData();
};

extern thread_local uint64_t threadLocalData[sizeof(ThreadLocalData) / sizeof(uint64_t)];

class ThreadLocal { // merge this to ThreadLocalData.
public:
    static inline ThreadLocalData* GetThreadLocalData()
    {
        return reinterpret_cast<ThreadLocalData*>(threadLocalData);
    }

    static void SetMutator(Mutator* newMutator);

    static Mutator* GetMutator() { return GetThreadLocalData()->mutator; }

    static AllocBuffer* GetAllocBuffer() { return GetThreadLocalData()->buffer; }

    static void SetAllocBuffer(AllocBuffer* buffer) { GetThreadLocalData()->buffer = buffer; }

    static AllocBufferMS* GetAllocBufferMS() { return GetThreadLocalData()->msAllocBuffer; }

    static void SetAllocBufferMS(AllocBufferMS* buffer) { GetThreadLocalData()->msAllocBuffer = buffer; }

    static uint8_t* GetPreemptFlag() { return GetThreadLocalData()->preemptFlag; }

    static void SetProtectAddr(uint8_t* addr) { GetThreadLocalData()->protectAddr = addr; }

    static void SetThreadType(ThreadType type) { GetThreadLocalData()->threadType = type; }

    static ThreadType GetThreadType() { return GetThreadLocalData()->threadType; }

    static void SetCJProcessorFlag(bool flag) { GetThreadLocalData()->isCJProcessor = flag; }

    static bool IsCJProcessor() { return GetThreadLocalData()->isCJProcessor; }

    static void SetForeignCJThread(void* cjthread)
    {
        GetThreadLocalData()->foreignCJThread = cjthread;
    }
    
    static void* GetForeignCJThread()
    {
        return GetThreadLocalData()->foreignCJThread;
    }

    static void SetCJThread(void* cjthread)
    {
        GetThreadLocalData()->cjthread = reinterpret_cast<uint8_t*>(cjthread);
    }

    static void SetSchedule(void* schedule)
    {
        GetThreadLocalData()->schedule = reinterpret_cast<uint8_t*>(schedule);
    }

    // When runtime is stop, we need to lock any operation which may access runtime.
    static void ThreadLocalFini()
    {
        tlEnableLock.LockWrite();
    }

    static bool TryGetRdLock()
    {
        return tlEnableLock.TryLockRead();
    }

    static void UnlockRdLock()
    {
        tlEnableLock.UnlockRead();
    }
private:
    static RwLock tlEnableLock;
};
} // namespace MapleRuntime
#endif // MRT_THREAD_LOCAL_H
