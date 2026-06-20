// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_WRITE_BARRIER_BUFFER_H
#define MRT_WRITE_BARRIER_BUFFER_H

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <vector>

#include "Common/BaseObject.h"
#include "Base/Macros.h"

namespace MapleRuntime {

// Forward declarations
class WorkStack;
struct WriteBarrierBufferLayoutAccess;

// WriteBarrierBuffer — per-owner log buffer for the mark barrier
//
// During concurrent marking, the write barrier records overwritten reference
// targets here. Flushing the buffer performs the actual discover/mark step and
// forwards newly-grey objects into collector work queues.
//
// Layout: fixed-size inline array (256 entries = 2 KB on 64-bit).
//   - Push is O(1) lock-free (single writer per owner).
//   - When full, the collector flush path drains and shades the logged refs.
//   - At MarkTermination STW, the collector flushes every live owner buffer.
//
// Thread-safety:
//   - Push: called only from the owning thread-local execution context (single writer).
//   - Flush/Clear: called under STW or by the owning thread.
//   - No lock needed.

class WriteBarrierBuffer {
public:
    static constexpr size_t kCapacity = 1024;

    WriteBarrierBuffer() : count_(0)
    {
        for (size_t i = 0; i < kCapacity; ++i) {
            buf_[i] = nullptr;
        }
    }

    ~WriteBarrierBuffer() = default;

    // Push a logged heap reference into the buffer.
    // Caller MUST check IsFull() before calling if overflow handling is needed
    // (see MSMarkBarrier::EnqueueNew for the overflow-to-global-queue path).
    inline void Push(BaseObject* obj)
    {
        size_t idx = count_;
        if (LIKELY(idx < kCapacity)) {
            buf_[idx] = obj;
            count_ = idx + 1;
        }
    }

    // Flush all buffered objects into the given container, then reset.
    // Container must support push_back(BaseObject*).
    // Called under STW (MarkTermination) or by the owning Mutator.
    template<typename Container>
    inline void Flush(Container& workStack)
    {
        size_t n = count_;
        for (size_t i = 0; i < n; ++i) {
            if (buf_[i] != nullptr) {
                workStack.push_back(buf_[i]);
            }
        }
        count_ = 0;
    }

    // Clear the buffer without flushing (e.g., at GC cycle end).
    inline void Clear()
    {
        count_ = 0;
    }

    // Query
    size_t Count() const { return count_; }
    bool IsEmpty() const { return count_ == 0; }
    bool IsFull() const { return count_ >= kCapacity; }
    BaseObject** Data() { return buf_; }

private:
    friend struct WriteBarrierBufferLayoutAccess;

    BaseObject* buf_[kCapacity];
    size_t count_;
};

// Expose layout facts for backend/runtime ABI checks without making the buffer
// payload fields public.
struct WriteBarrierBufferLayoutAccess {
    static constexpr size_t BufOffset()
    {
        return offsetof(WriteBarrierBuffer, buf_);
    }

    static constexpr size_t CountOffset()
    {
        return offsetof(WriteBarrierBuffer, count_);
    }

    static constexpr size_t LayoutSize()
    {
        return sizeof(WriteBarrierBuffer);
    }
};

} // namespace MapleRuntime

#endif // MRT_WRITE_BARRIER_BUFFER_H
