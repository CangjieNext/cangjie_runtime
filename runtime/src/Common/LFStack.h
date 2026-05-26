// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_LFSTACK_H
#define MRT_LFSTACK_H

#include <atomic>
#include <cstddef>
#include <mutex>
#include <new>
#include <utility>

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
    void Push(WorkBuf* buf)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        buf->next = top_;
        top_ = buf;
        ++count_;
    }

    WorkBuf* Pop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        WorkBuf* oldTop = top_;
        if (oldTop == nullptr) {
            return nullptr;
        }
        top_ = oldTop->next;
        oldTop->next = nullptr;
        --count_;
        return oldTop;
    }

    bool Empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return top_ == nullptr;
    }

    size_t Count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

private:
    mutable std::mutex mutex_;
    WorkBuf* top_ = nullptr;
    size_t count_ = 0;
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
        totalAllocated_.fetch_add(1, std::memory_order_relaxed);
        return new (std::nothrow) WorkBuf();
    }

    void Put(WorkBuf* buf)
    {
        buf->Reset();
        freeList_.Push(buf);
    }

    size_t TotalAllocated() const
    {
        return totalAllocated_.load(std::memory_order_relaxed);
    }

private:
    LFStack freeList_;
    std::atomic<size_t> totalAllocated_{0};
};

// ============================================================================
// PerPGCWork: per-Processor GC work buffer with double-buffering.
// Go equivalent: runtime.gcWork (per-P, wbuf1/wbuf2).
// Interfaces with a global LFStack for overflow/refill.
// ============================================================================
struct PerPGCWork {
    WorkBuf* wbuf1_ = nullptr;
    WorkBuf* wbuf2_ = nullptr;
    bool flushedWork_ = false;

    // Set at GC cycle start; null between cycles.
    LFStack* globalFull_ = nullptr;
    WorkBufPool* workBufPool_ = nullptr;

    bool IsInitialized() const { return globalFull_ != nullptr; }

    void Init(LFStack* full, WorkBufPool* pool)
    {
        globalFull_ = full;
        workBufPool_ = pool;
        flushedWork_ = false;
        // wbuf1_ (get buf) allocated lazily on first TryGetObj refill;
        // wbuf2_ (put buf) allocated lazily on first PutObj.
    }

    // Push a grey object into the put buffer (wbuf2_).
    // wbuf2_ is the dedicated sink for newly discovered children;
    // it is separate from wbuf1_ (the get buffer) so that TryGetObj
    // monotonically drains wbuf1_ without interference from PutObj.
    // Overflow → push full wbuf2_ to globalFull_, get new empty buf from pool.
    void PutObj(BaseObject* obj)
    {
        if (wbuf2_ == nullptr) {
            wbuf2_ = workBufPool_->Get();
        }
        if (!wbuf2_->Full()) {
            wbuf2_->Push(obj);
            return;
        }
        // wbuf2 full → push to global, get fresh empty put buf
        globalFull_->Push(wbuf2_);
        flushedWork_ = true;
        wbuf2_ = workBufPool_->Get();
        wbuf2_->Push(obj);
    }

    // Pop a grey object from the get buffer (wbuf1_).
    // wbuf1_ is the dedicated source; PutObj never writes here.
    // Drain order:
    //   1. wbuf1_ (no lock)
    //   2. wbuf2_ → promote to wbuf1_ (no lock; avoids globalFull_ mutex)
    //   3. globalFull_ (mutex; cross-thread refill)
    BaseObject* TryGetObj()
    {
        // 1. Drain get buffer
        if (wbuf1_ != nullptr && !wbuf1_->Empty()) {
            return wbuf1_->Pop();
        }
        // 2. wbuf1 empty: promote put buffer to get buffer
        if (wbuf2_ != nullptr && !wbuf2_->Empty()) {
            if (wbuf1_ != nullptr) {
                workBufPool_->Put(wbuf1_);
            }
            wbuf1_ = wbuf2_;
            wbuf2_ = workBufPool_->Get();  // fresh empty put buf
            return wbuf1_->Pop();
        }
        // 3. Both empty: refill from global
        if (globalFull_ == nullptr) {
            return nullptr;
        }
        WorkBuf* buf = globalFull_->Pop();
        if (buf == nullptr) {
            return nullptr;
        }
        if (wbuf1_ != nullptr) {
            workBufPool_->Put(wbuf1_);
        }
        wbuf1_ = buf;
        return wbuf1_->Pop();
    }

    // TryGetObjLocal: pop from local wbuf1/wbuf2 ONLY.
    // Does NOT refill from globalFull_.
    // Used by GC thread when assist is enabled: leaves globalFull_
    // work for assist threads to consume, creating true parallelism.
    BaseObject* TryGetObjLocal()
    {
        if (wbuf1_ != nullptr && !wbuf1_->Empty()) {
            return wbuf1_->Pop();
        }
        // Promote put buffer to get buffer (no lock)
        if (wbuf2_ != nullptr && !wbuf2_->Empty()) {
            if (wbuf1_ != nullptr) {
                workBufPool_->Put(wbuf1_);
            }
            wbuf1_ = wbuf2_;
            wbuf2_ = workBufPool_->Get();
            return wbuf1_->Pop();
        }
        return nullptr;
    }

    // Dispose: flush all local work to global (convergence / MarkTermination).
    // Go equivalent: gcWork.dispose().
    void Dispose()
    {
        if (wbuf1_ != nullptr && !wbuf1_->Empty()) {
            globalFull_->Push(wbuf1_);
            wbuf1_ = workBufPool_->Get();
            flushedWork_ = true;
        }
        if (wbuf2_ != nullptr && !wbuf2_->Empty()) {
            globalFull_->Push(wbuf2_);
            wbuf2_ = workBufPool_->Get();
            flushedWork_ = true;
        }
    }

    // FlushAndSplit: share work with globalFull_ for assist threads.
    // 1. Push wbuf2 to global if non-empty
    // 2. Split wbuf1 in half -> push half to global, keep half locally
    // Unlike Dispose(), the GC thread keeps half of wbuf1 so it can
    // continue draining without an expensive global refill.
    // Returns number of bufs pushed to global.
    size_t FlushAndSplit()
    {
        size_t pushed = 0;
        // Share wbuf2 first
        if (wbuf2_ != nullptr && !wbuf2_->Empty()) {
            globalFull_->Push(wbuf2_);
            wbuf2_ = workBufPool_->Get();
            flushedWork_ = true;
            pushed++;
        }
        // Split wbuf1 if it has enough objects.
        // DFS frontier is typically narrow (2-8 items with avg 1.0 children),
        // so use a low threshold to actually trigger sharing.
        constexpr size_t kMinSplitLen = 4;
        if (wbuf1_ != nullptr && wbuf1_->len >= kMinSplitLen) {
            size_t shareCount = wbuf1_->len / 2;
            size_t keepCount = wbuf1_->len - shareCount;
            WorkBuf* shareBuf = workBufPool_->Get();
            // Bulk copy the second half to shareBuf
            memcpy(shareBuf->objs, &wbuf1_->objs[keepCount],
                   shareCount * sizeof(BaseObject*));
            shareBuf->len = shareCount;
            wbuf1_->len = keepCount;
            globalFull_->Push(shareBuf);
            flushedWork_ = true;
            pushed++;
        }
        return pushed;
    }

    // Balance: share wbuf2 to global when global is empty.
    // Lets other threads (GC / assist) find work.
    void Balance()
    {
        if (wbuf2_ != nullptr && !wbuf2_->Empty()) {
            globalFull_->Push(wbuf2_);
            wbuf2_ = workBufPool_->Get();
            flushedWork_ = true;
        }
    }

    // LocalLen: return total objects in local wbuf1 + wbuf2 (no lock needed).
    size_t LocalLen() const
    {
        size_t n = 0;
        if (wbuf1_ != nullptr) { n += wbuf1_->len; }
        if (wbuf2_ != nullptr) { n += wbuf2_->len; }
        return n;
    }

    bool Empty() const
    {
        return (wbuf1_ == nullptr || wbuf1_->Empty()) &&
               (wbuf2_ == nullptr || wbuf2_->Empty());
    }

    // Reset: return all bufs to pool, clear state.
    // Called at end of GC cycle.
    void Reset()
    {
        if (wbuf1_ != nullptr) {
            workBufPool_->Put(wbuf1_);
            wbuf1_ = nullptr;
        }
        if (wbuf2_ != nullptr) {
            workBufPool_->Put(wbuf2_);
            wbuf2_ = nullptr;
        }
        flushedWork_ = false;
        globalFull_ = nullptr;
        workBufPool_ = nullptr;
    }
};

} // namespace MapleRuntime

#endif // MRT_LFSTACK_H
