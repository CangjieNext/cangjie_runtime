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
    // Layout note: objs is kept at offset 16 (right after next+len) so the
    // AArch64 MSGC write-barrier fast path can address it with a fixed offset.
    // needsDiscover is placed AFTER objs on purpose, to avoid shifting objs.
    BaseObject* objs[kCapacity];
    // Route-B deferred discover flag:
    //   true  = raw write-barrier log (entries not yet DiscoverObject'd).
    //           Consumers must DiscoverObject each entry before scanning.
    //   false = grey objects already discovered (mark-traversal output / roots).
    bool needsDiscover = false;

    bool Full() const { return len >= kCapacity; }
    bool Empty() const { return len == 0; }

    void Push(BaseObject* obj) { objs[len++] = obj; }
    BaseObject* Pop() { return objs[--len]; }

    void Reset()
    {
        len = 0;
        next = nullptr;
        needsDiscover = false;
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

    // PopFull: pop the top buf only when it is a FULL buf (len == kCapacity).
    // Used by debt-driven assist global pull so each pulled buf accounts for
    // exactly kCapacity objects of debt. Returns nullptr if the stack is empty
    // or the top buf is partial.
    WorkBuf* PopFull()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        WorkBuf* oldTop = top_;
        if (oldTop == nullptr || oldTop->len < WorkBuf::kCapacity) {
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

    // Batch acquire up to n bufs under a single lock pass (falls back to new()
    // when the free list runs dry). Returns the number actually written to out.
    // Used by the per-P WorkBuf cache to amortize the global freeList_ mutex.
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
            totalAllocated_.fetch_add(1, std::memory_order_relaxed);
            out[got++] = buf;
        }
        return got;
    }

    // Batch return n bufs to the free list.
    void PutN(WorkBuf** bufs, size_t n)
    {
        for (size_t i = 0; i < n; ++i) {
            bufs[i]->Reset();
            freeList_.Push(bufs[i]);
        }
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
// WorkBufList: bounded singly-linked list of WorkBuf nodes (LIFO).
// Reuses WorkBuf::next as the link. Holds at most kMaxNodes nodes.
// Used by PerPGCWork for the two local lists (list1_ = consume-ready,
// list2_ = mark-output spill target). Not thread-safe; per-P only.
// ============================================================================
struct WorkBufList {
    static constexpr size_t kMaxNodes = 4;

    WorkBuf* head_ = nullptr;
    size_t nodeCount_ = 0;

    bool ListEmpty() const { return head_ == nullptr; }
    bool ListFull() const { return nodeCount_ >= kMaxNodes; }
    size_t NodeCount() const { return nodeCount_; }

    void PushNode(WorkBuf* buf)
    {
        buf->next = head_;
        head_ = buf;
        ++nodeCount_;
    }

    WorkBuf* PopNode()
    {
        if (head_ == nullptr) {
            return nullptr;
        }
        WorkBuf* buf = head_;
        head_ = buf->next;
        buf->next = nullptr;
        --nodeCount_;
        return buf;
    }

    size_t TotalObjs() const
    {
        size_t n = 0;
        for (WorkBuf* b = head_; b != nullptr; b = b->next) {
            n += b->len;
        }
        return n;
    }
};

// ============================================================================
// PerPGCWork: per-Processor GC work buffers with bounded local lists.
//
//   getCur_  : in-use get buffer (Pop side). NOT counted in any list.
//   putCur_  : in-use put buffer (Push side). NOT counted in any list.
//   list1_   : consume-ready full bufs (write-barrier output + spill target).
//   list2_   : mark-output spill target (putCur_ overflow lands here first).
//
// Write-barrier overflow → PushWBBuf → list1_ (local-first), else global.
// Mark output (PutObj) → putCur_ → (full) list2_ → (full) SpillToList1OrGlobal.
// Get path: getCur_ → list1_ → list2_ → putCur_ → globalFull_ (TryGetObj),
//           getCur_ → list1_ → list2_ → putCur_           (TryGetObjLocal).
// ============================================================================
struct PerPGCWork {
    WorkBufList list1_;
    WorkBufList list2_;
    WorkBuf* getCur_ = nullptr;
    WorkBuf* putCur_ = nullptr;
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
        // getCur_/putCur_ allocated lazily on first use.
    }

    // Spill a full buf preferentially into list1_ (so it gets consumed locally),
    // falling back to globalFull_ when list1_ is full.
    void SpillToList1OrGlobal(WorkBuf* buf)
    {
        if (!list1_.ListFull()) {
            list1_.PushNode(buf);
        } else {
            globalFull_->Push(buf);
            flushedWork_ = true;
        }
    }

    // Push a write-barrier-overflow buf into the local list1_ (local-first),
    // falling back to globalFull_ when list1_ is full.
    void PushWBBuf(WorkBuf* buf)
    {
        if (!list1_.ListFull()) {
            list1_.PushNode(buf);
        } else {
            globalFull_->Push(buf);
            flushedWork_ = true;
        }
    }

    // Push a grey object into the put buffer (putCur_).
    // putCur_ full → list2_ (if room) → SpillToList1OrGlobal (otherwise).
    void PutObj(BaseObject* obj)
    {
        if (putCur_ == nullptr) {
            putCur_ = workBufPool_->Get();
        }
        if (!putCur_->Full()) {
            putCur_->Push(obj);
            return;
        }
        // putCur_ full → spill to list2_ first, else to list1_/global.
        if (!list2_.ListFull()) {
            list2_.PushNode(putCur_);
        } else {
            SpillToList1OrGlobal(putCur_);
        }
        putCur_ = workBufPool_->Get();
        putCur_->Push(obj);
    }

    // Internal: promote one source buffer to getCur_ and Pop from it.
    BaseObject* PromoteAndPop(WorkBuf* src)
    {
        if (getCur_ != nullptr) {
            workBufPool_->Put(getCur_);
        }
        getCur_ = src;
        return getCur_->Pop();
    }

    // TryGetObjLocal: local-only get (no global refill). Used by assist.
    BaseObject* TryGetObjLocal()
    {
        if (getCur_ != nullptr && !getCur_->Empty()) {
            return getCur_->Pop();
        }
        if (!list1_.ListEmpty()) {
            return PromoteAndPop(list1_.PopNode());
        }
        if (!list2_.ListEmpty()) {
            return PromoteAndPop(list2_.PopNode());
        }
        if (putCur_ != nullptr && !putCur_->Empty()) {
            WorkBuf* src = putCur_;
            putCur_ = workBufPool_->Get();
            return PromoteAndPop(src);
        }
        return nullptr;
    }

    // TryGetObj: local-first then global refill. Used by the GC thread loop.
    BaseObject* TryGetObj()
    {
        BaseObject* obj = TryGetObjLocal();
        if (obj != nullptr) {
            return obj;
        }
        if (globalFull_ == nullptr) {
            return nullptr;
        }
        WorkBuf* buf = globalFull_->Pop();
        if (buf == nullptr) {
            return nullptr;
        }
        return PromoteAndPop(buf);
    }

    // Dispose: flush all local work to global (convergence / MarkTermination).
    void Dispose()
    {
        if (getCur_ != nullptr && !getCur_->Empty()) {
            globalFull_->Push(getCur_);
            getCur_ = nullptr;
            flushedWork_ = true;
        }
        if (putCur_ != nullptr && !putCur_->Empty()) {
            globalFull_->Push(putCur_);
            putCur_ = nullptr;
            flushedWork_ = true;
        }
        WorkBuf* b;
        while ((b = list1_.PopNode()) != nullptr) {
            globalFull_->Push(b);
            flushedWork_ = true;
        }
        while ((b = list2_.PopNode()) != nullptr) {
            globalFull_->Push(b);
            flushedWork_ = true;
        }
    }

    bool Empty() const
    {
        return (getCur_ == nullptr || getCur_->Empty()) &&
               (putCur_ == nullptr || putCur_->Empty()) &&
               list1_.ListEmpty() && list2_.ListEmpty();
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
        WorkBuf* b;
        while ((b = list1_.PopNode()) != nullptr) {
            workBufPool_->Put(b);
        }
        while ((b = list2_.PopNode()) != nullptr) {
            workBufPool_->Put(b);
        }
        flushedWork_ = false;
        globalFull_ = nullptr;
        workBufPool_ = nullptr;
    }
};


} // namespace MapleRuntime

#endif // MRT_LFSTACK_H
