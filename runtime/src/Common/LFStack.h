// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_LFSTACK_H
#define MRT_LFSTACK_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <utility>

#include "Common/WorkEpoch.h"

namespace MapleRuntime {

class BaseObject;

// ============================================================================
// WorkBuf: fixed-size object pointer buffer, unit of global work exchange.
// Go equivalent: runtime.workbuf (256 pointers per buf).
// ============================================================================
struct alignas(64) WorkBuf {
    static constexpr size_t kCapacity = 1024;

    WorkBuf* next = nullptr;
    size_t len = 0;
    // Layout note: objs is kept at offset 16 (right after next+len) so the
    // AArch64 MSGC write-barrier fast path can address it with a fixed offset.
    BaseObject* objs[kCapacity];

    bool Full() const { return len >= kCapacity; }
    bool Empty() const { return len == 0; }

    void Push(BaseObject* obj) { objs[len++] = obj; }
    BaseObject* Pop() { return objs[--len]; }

    void Reset()
    {
        len = 0;
        next = nullptr;
    }
};

// ============================================================================
// LFStack: shared WorkBuf stack used by global GC work exchange and the pool.
// WorkBuf nodes are aggressively recycled, so a plain Treiber stack is not
// safe here without ABA protection. Use a small mutex to preserve correctness.
// ============================================================================
class LFStack {
public:
    struct Counts {
        size_t total;
        size_t full;
    };

    struct PopResult {
        WorkBuf* buf;
        Counts counts;
    };

    explicit LFStack(WorkEpoch* workEpoch = nullptr) : workEpoch_(workEpoch) {}

    void Push(WorkBuf* buf)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            WorkBuf*& top = buf->Full() ? fullTop_ : partialTop_;
            buf->next = top;
            top = buf;
            ++count_;
            if (buf->Full()) {
                ++fullCount_;
            }
            PublishApproxCountsLocked();
        }
        if (workEpoch_ != nullptr) {
            workEpoch_->Notify();
        }
    }

    WorkBuf* Pop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return PopLocked();
    }

    PopResult PopWithCounts()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        WorkBuf* buf = PopLocked();
        return PopResult { buf, Counts { count_, fullCount_ } };
    }

    WorkBuf* PopPartial()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        WorkBuf* oldTop = partialTop_;
        if (oldTop == nullptr) {
            return nullptr;
        }
        partialTop_ = oldTop->next;
        oldTop->next = nullptr;
        --count_;
        PublishApproxCountsLocked();
        return oldTop;
    }

    WorkBuf* PopFull()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        WorkBuf* full = fullTop_;
        if (full == nullptr) {
            return nullptr;
        }
        fullTop_ = full->next;
        full->next = nullptr;
        --count_;
        --fullCount_;
        PublishApproxCountsLocked();
        return full;
    }

    size_t PopN(WorkBuf** out, size_t n)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t got = 0;
        while (got < n && fullTop_ != nullptr) {
            WorkBuf* buf = fullTop_;
            fullTop_ = buf->next;
            buf->next = nullptr;
            out[got++] = buf;
            --count_;
            --fullCount_;
        }
        while (got < n && partialTop_ != nullptr) {
            WorkBuf* buf = partialTop_;
            partialTop_ = buf->next;
            buf->next = nullptr;
            out[got++] = buf;
            --count_;
        }
        if (got != 0) {
            PublishApproxCountsLocked();
        }
        return got;
    }

    void PushChain(WorkBuf* head)
    {
        if (head == nullptr) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            WorkBuf* fullHead = nullptr;
            WorkBuf* fullTail = nullptr;
            WorkBuf* partialHead = nullptr;
            WorkBuf* partialTail = nullptr;
            size_t added = 0;
            size_t addedFull = 0;
            while (head != nullptr) {
                WorkBuf* next = head->next;
                head->next = nullptr;
                WorkBuf*& laneHead = head->Full() ? fullHead : partialHead;
                WorkBuf*& laneTail = head->Full() ? fullTail : partialTail;
                if (laneHead == nullptr) {
                    laneHead = head;
                } else {
                    laneTail->next = head;
                }
                laneTail = head;
                ++added;
                if (head->Full()) {
                    ++addedFull;
                }
                head = next;
            }
            if (fullHead != nullptr) {
                fullTail->next = fullTop_;
                fullTop_ = fullHead;
            }
            if (partialHead != nullptr) {
                partialTail->next = partialTop_;
                partialTop_ = partialHead;
            }
            count_ += added;
            fullCount_ += addedFull;
            PublishApproxCountsLocked();
        }
        if (workEpoch_ != nullptr) {
            workEpoch_->Notify();
        }
    }

    bool Empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return fullTop_ == nullptr && partialTop_ == nullptr;
    }

    Counts GetCounts() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return Counts { count_, fullCount_ };
    }

    Counts GetApproxCounts() const
    {
        if (workEpoch_ == nullptr) {
            return GetCounts();
        }
        uint64_t packed = approxCounts_.load(std::memory_order_acquire);
        return Counts {
            static_cast<size_t>(packed & 0xffffffffULL),
            static_cast<size_t>(packed >> 32)
        };
    }

private:
    void PublishApproxCountsLocked()
    {
        if (workEpoch_ == nullptr) {
            return;
        }
        uint64_t packed = (static_cast<uint64_t>(fullCount_) << 32) |
                          (static_cast<uint64_t>(count_) & 0xffffffffULL);
        approxCounts_.store(packed, std::memory_order_release);
    }

    WorkBuf* PopLocked()
    {
        WorkBuf* oldTop = fullTop_;
        if (oldTop != nullptr) {
            fullTop_ = oldTop->next;
            --fullCount_;
        } else {
            oldTop = partialTop_;
            if (oldTop == nullptr) {
                return nullptr;
            }
            partialTop_ = oldTop->next;
        }
        oldTop->next = nullptr;
        --count_;
        PublishApproxCountsLocked();
        return oldTop;
    }

    mutable std::mutex mutex_;
    WorkBuf* fullTop_ = nullptr;
    WorkBuf* partialTop_ = nullptr;
    size_t count_ = 0;
    size_t fullCount_ = 0;
    std::atomic<uint64_t> approxCounts_ {0};
    WorkEpoch* workEpoch_ = nullptr;
};

// ============================================================================
// WorkBufPool: free-list pool of WorkBuf (also backed by LFStack).
// Avoids malloc/free on hot path.
// ============================================================================
class WorkBufPool {
public:
    ~WorkBufPool()
    {
        WorkBuf* buf;
        while ((buf = freeList_.Pop()) != nullptr) {
            delete buf;
        }
    }

    WorkBuf* Get()
    {
        WorkBuf* buf = freeList_.Pop();
        if (buf != nullptr) {
            buf->Reset();
            return buf;
        }
        return new (std::nothrow) WorkBuf();
    }

    void Put(WorkBuf* buf)
    {
        buf->Reset();
        freeList_.Push(buf);
    }

    // Batch acquire up to n bufs (falling back to new() when the free list runs
    // dry). Returns the number actually written to out. The per-P cache then
    // serves subsequent swaps without touching the global free list.
    size_t GetN(WorkBuf** out, size_t n)
    {
        size_t got = freeList_.PopN(out, n);
        for (size_t i = 0; i < got; ++i) {
            out[i]->Reset();
        }
        while (got < n) {
            WorkBuf* buf = new (std::nothrow) WorkBuf();
            if (buf == nullptr) {
                break;
            }
            out[got++] = buf;
        }
        return got;
    }

    void PutChain(WorkBuf* head)
    {
        for (WorkBuf* buf = head; buf != nullptr; buf = buf->next) {
            buf->len = 0;
        }
        freeList_.PushChain(head);
    }

private:
    LFStack freeList_;
};

// ============================================================================
// PerPGCWork: per-Processor GC work buffers.
//
//   getCur_  : in-use get buffer (Pop side). NOT counted in any list.
//   putCur_  : in-use put buffer (Push side). NOT counted in any list.
//
// Full write-barrier logs and full putCur_ buffers go to the Full lane.
// Published partial tails go to the Partial lane.
// ============================================================================
struct PerPGCWork {
    WorkBuf* getCur_ = nullptr;
    WorkBuf* putCur_ = nullptr;

    // Set at GC cycle start; null between cycles.
    LFStack* globalWork_ = nullptr;
    WorkBufPool* workBufPool_ = nullptr;
    // Local empty-buffer cache state remains public with the other data members
    // so PerPGCWork stays standard-layout inside compiler-visible AllocBufferMS.
    WorkBuf* emptyCache_ = nullptr;
    size_t emptyCacheCount_ = 0;

    bool IsInitialized() const { return globalWork_ != nullptr; }

    void Init(LFStack* global, WorkBufPool* pool)
    {
        globalWork_ = global;
        workBufPool_ = pool;
        // getCur_/putCur_ allocated lazily on first use.
    }

    // Publish complete mark-output buffers globally. Partial output remains in
    // putCur_ until it fills or MarkTermination disposes per-P work.
    void PutObj(BaseObject* obj)
    {
        if (putCur_ == nullptr) {
            putCur_ = AcquireEmptyBuffer();
        }
        putCur_->Push(obj);
        if (putCur_->Full()) {
            globalWork_->Push(putCur_);
            putCur_ = AcquireEmptyBuffer();
        }
    }

    // Acquire exactly one full global buffer for one assist/GC slice.
    bool AcquireOneFullBuffer()
    {
        if (getCur_ != nullptr) {
            if (!getCur_->Empty()) {
                return true;
            }
            RecycleCurrentInput();
        }
        getCur_ = globalWork_->PopFull();
        return getCur_ != nullptr;
    }

    bool AcquireOnePartialBuffer()
    {
        RecycleEmptyInput();
        if (getCur_ != nullptr) {
            return true;
        }
        getCur_ = globalWork_->PopPartial();
        return getCur_ != nullptr;
    }

    // GC main/helpers consume every published buffer as one complete task,
    // regardless of whether it reached kCapacity before publication.
    bool AcquireOneBuffer()
    {
        if (getCur_ != nullptr) {
            if (!getCur_->Empty()) {
                return true;
            }
            RecycleCurrentInput();
        }
        getCur_ = globalWork_->Pop();
        return getCur_ != nullptr;
    }

    bool AcquireOneBuffer(LFStack::Counts& countsAfterPop)
    {
        if (getCur_ != nullptr) {
            if (!getCur_->Empty()) {
                countsAfterPop = globalWork_->GetCounts();
                return true;
            }
            RecycleCurrentInput();
        }
        LFStack::PopResult result = globalWork_->PopWithCounts();
        getCur_ = result.buf;
        countsAfterPop = result.counts;
        return getCur_ != nullptr;
    }

    bool PromotePutToGet(size_t minLen)
    {
        if (putCur_ == nullptr || putCur_->len < minLen) {
            return false;
        }
        RecycleEmptyInput();
        if (getCur_ != nullptr) {
            return false;
        }
        getCur_ = putCur_;
        putCur_ = nullptr;
        return true;
    }

    WorkBuf* AcquirePartialOutputBuffer()
    {
        if (putCur_ != nullptr) {
            return putCur_;
        }
        putCur_ = globalWork_->PopPartial();
        if (putCur_ == nullptr) {
            putCur_ = AcquireEmptyBuffer();
        }
        return putCur_;
    }

    void PublishPartialOutput()
    {
        if (putCur_ == nullptr) {
            return;
        }
        if (putCur_->Empty()) {
            RecycleEmptyBuffer(putCur_);
        } else {
            globalWork_->Push(putCur_);
        }
        putCur_ = nullptr;
    }

    // Private putCur_ is deliberately left for MarkTermination. Re-publishing
    // every partial tail can keep concurrent mark alive under sustained WB load.
    bool ConcurrentInputEmpty() const
    {
        return getCur_ == nullptr || getCur_->Empty();
    }

    // Dispose: flush all private work to global (convergence / MarkTermination).
    void Dispose()
    {
        if (getCur_ != nullptr) {
            if (getCur_->Empty()) {
                RecycleEmptyBuffer(getCur_);
            } else {
                globalWork_->Push(getCur_);
            }
            getCur_ = nullptr;
        }
        if (putCur_ != nullptr) {
            if (putCur_->Empty()) {
                RecycleEmptyBuffer(putCur_);
            } else {
                globalWork_->Push(putCur_);
            }
            putCur_ = nullptr;
        }
    }

    void RecycleCurrentInput()
    {
        if (getCur_ == nullptr || !getCur_->Empty()) {
            return;
        }
        WorkBuf* input = getCur_;
        getCur_ = nullptr;
        RecycleEmptyBuffer(input);
    }

    // Reset: return all bufs to pool, clear state. Called at end of GC cycle.
    void Reset()
    {
        if (getCur_ != nullptr) {
            RecycleEmptyBuffer(getCur_);
            getCur_ = nullptr;
        }
        if (putCur_ != nullptr) {
            RecycleEmptyBuffer(putCur_);
            putCur_ = nullptr;
        }
        FlushEmptyCache();
        globalWork_ = nullptr;
        workBufPool_ = nullptr;
    }

private:
    static constexpr size_t kLocalEmptyCacheCapacity = 2;

    WorkBuf* AcquireEmptyBuffer()
    {
        if (emptyCache_ != nullptr) {
            WorkBuf* buf = emptyCache_;
            emptyCache_ = buf->next;
            buf->next = nullptr;
            --emptyCacheCount_;
            buf->Reset();
            return buf;
        }

        WorkBuf* bufs[kLocalEmptyCacheCapacity];
        size_t got = workBufPool_->GetN(bufs, kLocalEmptyCacheCapacity);
        if (got == 0) {
            return nullptr;
        }
        for (size_t i = 1; i < got; ++i) {
            bufs[i]->next = emptyCache_;
            emptyCache_ = bufs[i];
            ++emptyCacheCount_;
        }
        bufs[0]->Reset();
        return bufs[0];
    }

    void RecycleEmptyBuffer(WorkBuf* buf)
    {
        if (buf == nullptr) {
            return;
        }
        buf->Reset();
        if (emptyCacheCount_ < kLocalEmptyCacheCapacity) {
            buf->next = emptyCache_;
            emptyCache_ = buf;
            ++emptyCacheCount_;
            return;
        }

        WorkBuf* chain = emptyCache_;
        emptyCache_ = buf;
        emptyCacheCount_ = 1;
        workBufPool_->PutChain(chain);
    }

    void FlushEmptyCache()
    {
        WorkBuf* chain = emptyCache_;
        emptyCache_ = nullptr;
        emptyCacheCount_ = 0;
        if (chain != nullptr) {
            workBufPool_->PutChain(chain);
        }
    }

    void RecycleEmptyInput()
    {
        RecycleCurrentInput();
    }

};


} // namespace MapleRuntime

#endif // MRT_LFSTACK_H
