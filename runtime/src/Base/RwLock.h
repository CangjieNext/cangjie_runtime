// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_RWLOCK_H
#define MRT_RWLOCK_H
#include <atomic>
#include <pthread.h>
#include <sys/types.h>
#include "Log.h"

namespace MapleRuntime {
pid_t GetTid(); // forward declaration; defined in Base/SysCall.cpp

class RwLock {
public:
    void LockRead()
    {
        int count = lockCount.load(std::memory_order_acquire);
        do {
            while (count == WRITE_LOCKED) {
                sched_yield();
                count = lockCount.load(std::memory_order_acquire);
            }
        } while (!lockCount.compare_exchange_weak(count, count + 1, std::memory_order_release));
        ownerTid.store(GetTid(), std::memory_order_release);
    }

    void LockWrite()
    {
        for (int count = 0; !lockCount.compare_exchange_weak(count, WRITE_LOCKED, std::memory_order_release);
             count = 0) {
            sched_yield();
        }
        ownerTid.store(GetTid(), std::memory_order_release);
    }

    bool TryLockWrite()
    {
        int count = 0;
        if (lockCount.compare_exchange_weak(count, WRITE_LOCKED, std::memory_order_release)) {
            ownerTid.store(GetTid(), std::memory_order_release);
            return true;
        }
        return false;
    }

    bool TryLockRead()
    {
        int count = lockCount.load(std::memory_order_acquire);
        do {
            if (count == WRITE_LOCKED) {
                return false;
            }
        } while (!lockCount.compare_exchange_weak(count, count + 1, std::memory_order_release));
        ownerTid.store(GetTid(), std::memory_order_release);
        return true;
    }

    void UnlockRead()
    {
        // Only clear if this thread is still the recorded owner; otherwise another
        // reader has overwritten ownerTid since we acquired and may still hold the lock.
        pid_t expected = GetTid();
        ownerTid.compare_exchange_strong(expected, 0, std::memory_order_release);
        int count = lockCount.fetch_sub(1);
        if (count < 0) {
            std::abort();
        }
    }

    void UnlockWrite()
    {
        CHECK(lockCount.load() == WRITE_LOCKED);
        ownerTid.store(0, std::memory_order_release);
        lockCount.store(0, std::memory_order_release);
    }

    // Returns the tid of the most recent lock acquirer (write or read), 0 when unlocked.
    // Note: with multiple concurrent readers, ownerTid follows last-writer-wins semantics
    // and may not represent every reader currently holding the lock; treat as a hint.
    pid_t GetOwnerTid() const { return ownerTid.load(std::memory_order_acquire); }

    // Returns the current lock state: 0 unlocked, WRITE_LOCKED (-1) write locked,
    // positive value is the number of concurrent readers.
    int GetLockCount() const { return lockCount.load(std::memory_order_acquire); }

private:
    // 0: unlocked
    // -1: write locked
    // inc 1 for each read lock
    static constexpr int WRITE_LOCKED = -1;
    std::atomic<int> lockCount{ 0 };
    // tid of the most recent lock acquirer (write or read). For write lock it is the
    // exclusive holder; for read lock it is the last reader that acquired (multi-reader
    // caveat: see GetOwnerTid comment above).
    std::atomic<pid_t> ownerTid{ 0 };
};
} // namespace MapleRuntime
#endif // MRT_SPINLOCK_H
