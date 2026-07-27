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
// WorkBuf: runtime-sized object pointer buffer, unit of global work exchange.
// The header layout is compiler ABI. The trailing object array starts at
// offset 16 and is allocated in one of four supported storage sizes.
// ============================================================================
struct WorkBuf {
    static constexpr uint32_t kDefaultCapacity = 1024;

    WorkBuf* next = nullptr;
    uint32_t len = 0;
    uint32_t capacity = 0;

    explicit WorkBuf(uint32_t bufferCapacity = kDefaultCapacity) : capacity(bufferCapacity) {}

    bool Full() const { return len >= capacity; }
    bool Empty() const { return len == 0; }

    BaseObject** Objects()
    {
        return reinterpret_cast<BaseObject**>(
            reinterpret_cast<uint8_t*>(this) + sizeof(WorkBuf));
    }

    BaseObject* const* Objects() const
    {
        return reinterpret_cast<BaseObject* const*>(
            reinterpret_cast<const uint8_t*>(this) + sizeof(WorkBuf));
    }

    void Push(BaseObject* obj) { Objects()[len++] = obj; }
    BaseObject* Pop() { return Objects()[--len]; }

    void Reset()
    {
        len = 0;
        next = nullptr;
    }
};

static_assert(sizeof(WorkBuf) == 16, "WorkBuf header size is compiler ABI");
static_assert(offsetof(WorkBuf, len) == 8, "WorkBuf::len offset is compiler ABI");
static_assert(offsetof(WorkBuf, capacity) == 12, "WorkBuf::capacity offset is compiler ABI");

template<size_t Capacity>
struct alignas(64) WorkBufStorage {
    WorkBufStorage() : header(static_cast<uint32_t>(Capacity)) {}

    WorkBuf header;
    BaseObject* objs[Capacity];
};

static_assert(offsetof(WorkBufStorage<128>, header) == 0,
              "WorkBuf storage header must be first");
static_assert(offsetof(WorkBufStorage<128>, objs) == 16,
              "WorkBuf object array offset is compiler ABI");
static_assert(offsetof(WorkBufStorage<1024>, objs) == 16,
              "WorkBuf object array offset is compiler ABI");

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
        size_t objects;
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
            objectCount_ += buf->len;
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
        return PopResult { buf, Counts { count_, fullCount_, objectCount_ } };
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
        objectCount_ -= oldTop->len;
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
        objectCount_ -= full->len;
        PublishApproxCountsLocked();
        return full;
    }

    bool Empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return fullTop_ == nullptr && partialTop_ == nullptr;
    }

    Counts GetCounts() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return Counts { count_, fullCount_, objectCount_ };
    }

    Counts GetApproxCounts() const
    {
        if (workEpoch_ == nullptr) {
            return GetCounts();
        }
        uint64_t packed = approxCounts_.load(std::memory_order_acquire);
        return Counts {
            static_cast<size_t>(packed & 0xffffffffULL),
            static_cast<size_t>(packed >> 32),
            approxObjectCount_.load(std::memory_order_acquire)
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
        approxObjectCount_.store(objectCount_, std::memory_order_release);
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
        objectCount_ -= oldTop->len;
        PublishApproxCountsLocked();
        return oldTop;
    }

    mutable std::mutex mutex_;
    WorkBuf* fullTop_ = nullptr;
    WorkBuf* partialTop_ = nullptr;
    size_t count_ = 0;
    size_t fullCount_ = 0;
    size_t objectCount_ = 0;
    std::atomic<uint64_t> approxCounts_ {0};
    std::atomic<size_t> approxObjectCount_ {0};
    WorkEpoch* workEpoch_ = nullptr;
};

// ============================================================================
// WorkBufPool: free-list pool of WorkBuf (also backed by LFStack).
// Avoids malloc/free on hot path.
// ============================================================================
class WorkBufPool {
public:
    explicit WorkBufPool(size_t capacity = 1024) : capacity_(capacity) {}

    ~WorkBufPool()
    {
        WorkBuf* buf;
        while ((buf = freeList_.Pop()) != nullptr) {
            Destroy(buf);
        }
    }

    bool Configure(size_t capacity)
    {
        if (capacity != 128 && capacity != 256 &&
            capacity != 512 && capacity != 1024) {
            return false;
        }
        if (capacity_ == capacity) {
            return true;
        }
        // Runtime configuration is published before the first GC can allocate
        // a WorkBuf, so changing capacity with cached nodes is not supported.
        if (!freeList_.Empty()) {
            return false;
        }
        capacity_ = capacity;
        return true;
    }

    size_t Capacity() const { return capacity_; }

    WorkBuf* Get()
    {
        WorkBuf* buf = freeList_.Pop();
        if (buf != nullptr) {
            buf->Reset();
            return buf;
        }
        return Allocate();
    }

    void Put(WorkBuf* buf)
    {
        if (buf == nullptr) {
            return;
        }
        buf->Reset();
        freeList_.Push(buf);
    }

private:
    template<size_t Capacity>
    static WorkBuf* AllocateStorage()
    {
        WorkBufStorage<Capacity>* storage =
            new (std::nothrow) WorkBufStorage<Capacity>();
        return storage == nullptr ? nullptr : &storage->header;
    }

    WorkBuf* Allocate()
    {
        switch (capacity_) {
            case 128:
                return AllocateStorage<128>();
            case 256:
                return AllocateStorage<256>();
            case 512:
                return AllocateStorage<512>();
            case 1024:
                return AllocateStorage<1024>();
            default:
                return nullptr;
        }
    }

    template<size_t Capacity>
    static void DestroyStorage(WorkBuf* buf)
    {
        delete reinterpret_cast<WorkBufStorage<Capacity>*>(buf);
    }

    void Destroy(WorkBuf* buf)
    {
        switch (capacity_) {
            case 128:
                DestroyStorage<128>(buf);
                return;
            case 256:
                DestroyStorage<256>(buf);
                return;
            case 512:
                DestroyStorage<512>(buf);
                return;
            case 1024:
                DestroyStorage<1024>(buf);
                return;
            default:
                return;
        }
    }

    size_t capacity_;
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
            putCur_ = workBufPool_->Get();
        }
        putCur_->Push(obj);
        if (putCur_->Full()) {
            globalWork_->Push(putCur_);
            putCur_ = workBufPool_->Get();
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
        RecycleCurrentInput();
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
                // This snapshot only feeds pressure/convergence prechecks.
                // Avoid taking the global queue lock while reusing local work.
                countsAfterPop = globalWork_->GetApproxCounts();
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
        RecycleCurrentInput();
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
            putCur_ = workBufPool_->Get();
        }
        return putCur_;
    }

    void PublishPartialOutput()
    {
        if (putCur_ == nullptr) {
            return;
        }
        if (putCur_->Empty()) {
            workBufPool_->Put(putCur_);
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
                workBufPool_->Put(getCur_);
            } else {
                globalWork_->Push(getCur_);
            }
            getCur_ = nullptr;
        }
        if (putCur_ != nullptr) {
            if (putCur_->Empty()) {
                workBufPool_->Put(putCur_);
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
        workBufPool_->Put(input);
    }

    // Reset: return all bufs to pool, clear state. Called at end of GC cycle.
    void Reset()
    {
        if (getCur_ != nullptr) {
            workBufPool_->Put(getCur_);
            getCur_ = nullptr;
        }
        if (putCur_ != nullptr) {
            workBufPool_->Put(putCur_);
            putCur_ = nullptr;
        }
        globalWork_ = nullptr;
        workBufPool_ = nullptr;
    }
};


} // namespace MapleRuntime

#endif // MRT_LFSTACK_H
