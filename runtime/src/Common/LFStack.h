// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_LFSTACK_H
#define MRT_LFSTACK_H

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
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
    static constexpr bool kRevalidateTrustedGrey = true;
#else
    static constexpr bool kRevalidateTrustedGrey = false;
#endif

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

    void Push(WorkBuf* buf)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        buf->next = top_;
        top_ = buf;
        ++count_;
        if (buf->Full()) {
            ++fullCount_;
        }
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
        if (oldTop->Full()) {
            --fullCount_;
        }
        return oldTop;
    }

    // Pop the first full buffer, leaving partial STW/exit buffers in place.
    // A partial top must not hide full concurrent-mark work below it.
    WorkBuf* PopFull()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        WorkBuf** link = &top_;
        while (*link != nullptr && (*link)->len < WorkBuf::kCapacity) {
            link = &(*link)->next;
        }
        WorkBuf* full = *link;
        if (full == nullptr) {
            return nullptr;
        }
        *link = full->next;
        full->next = nullptr;
        --count_;
        --fullCount_;
        return full;
    }

    bool Empty() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return top_ == nullptr;
    }

    Counts GetCounts() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return Counts { count_, fullCount_ };
    }

private:
    mutable std::mutex mutex_;
    WorkBuf* top_ = nullptr;
    size_t count_ = 0;
    size_t fullCount_ = 0;
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
        size_t got = 0;
        for (; got < n; ++got) {
            WorkBuf* buf = freeList_.Pop();
            if (buf == nullptr) {
                break;
            }
            buf->Reset();
            out[got] = buf;
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

private:
    LFStack freeList_;
};

// ============================================================================
// PerPGCWork: per-Processor GC work buffers.
//
//   getCur_  : in-use get buffer (Pop side). NOT counted in any list.
//   putCur_  : in-use put buffer (Push side). NOT counted in any list.
//
// Full write-barrier logs and full putCur_ buffers go directly to globalFull_.
// ============================================================================
struct PerPGCWork {
    WorkBuf* getCur_ = nullptr;
    WorkBuf* putCur_ = nullptr;

    // Set at GC cycle start; null between cycles.
    LFStack* globalFull_ = nullptr;
    WorkBufPool* workBufPool_ = nullptr;

    bool IsInitialized() const { return globalFull_ != nullptr; }

    void Init(LFStack* full, WorkBufPool* pool)
    {
        globalFull_ = full;
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
            globalFull_->Push(putCur_);
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
            workBufPool_->Put(getCur_);
            getCur_ = nullptr;
        }
        getCur_ = globalFull_->PopFull();
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
            workBufPool_->Put(getCur_);
            getCur_ = nullptr;
        }
        getCur_ = globalFull_->Pop();
        return getCur_ != nullptr;
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
        if (getCur_ != nullptr && !getCur_->Empty()) {
            globalFull_->Push(getCur_);
            getCur_ = nullptr;
        }
        if (putCur_ != nullptr && !putCur_->Empty()) {
            globalFull_->Push(putCur_);
            putCur_ = nullptr;
        }
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
        globalFull_ = nullptr;
        workBufPool_ = nullptr;
    }
};


} // namespace MapleRuntime

#endif // MRT_LFSTACK_H
