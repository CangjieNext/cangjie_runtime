// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MSGC_WRITE_BARRIER_FLUSH_STATS_H
#define MRT_MSGC_WRITE_BARRIER_FLUSH_STATS_H

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace MapleRuntime {

enum class MSGCWriteBarrierFlushSource : uint8_t {
    OVERFLOW_FLUSH = 0,
    EXITED_OWNER = 1,
    FINAL_STW = 2,
    COUNT = 3,
};

struct MSGCWriteBarrierFlushSnapshot {
    static constexpr size_t kSourceCount =
        static_cast<size_t>(MSGCWriteBarrierFlushSource::COUNT);

    size_t rawEntries[kSourceCount] = {};
    size_t forwardedObjects[kSourceCount] = {};

    size_t TotalRawEntries() const
    {
        size_t total = 0;
        for (size_t i = 0; i < kSourceCount; ++i) {
            total += rawEntries[i];
        }
        return total;
    }

    size_t TotalForwardedObjects() const
    {
        size_t total = 0;
        for (size_t i = 0; i < kSourceCount; ++i) {
            total += forwardedObjects[i];
        }
        return total;
    }
};

class MSGCWriteBarrierFlushStats {
public:
    static constexpr size_t kSourceCount =
        static_cast<size_t>(MSGCWriteBarrierFlushSource::COUNT);

    MSGCWriteBarrierFlushStats()
    {
        Reset();
    }

    void Reset()
    {
        for (size_t i = 0; i < kSourceCount; ++i) {
            rawEntries_[i].store(0, std::memory_order_relaxed);
            forwardedObjects_[i].store(0, std::memory_order_relaxed);
        }
    }

    void Record(MSGCWriteBarrierFlushSource source, size_t rawEntries, size_t forwardedObjects)
    {
        size_t idx = static_cast<size_t>(source);
        rawEntries_[idx].fetch_add(rawEntries, std::memory_order_relaxed);
        forwardedObjects_[idx].fetch_add(forwardedObjects, std::memory_order_relaxed);
    }

    MSGCWriteBarrierFlushSnapshot Snapshot() const
    {
        MSGCWriteBarrierFlushSnapshot snapshot;
        for (size_t i = 0; i < kSourceCount; ++i) {
            snapshot.rawEntries[i] = rawEntries_[i].load(std::memory_order_relaxed);
            snapshot.forwardedObjects[i] = forwardedObjects_[i].load(std::memory_order_relaxed);
        }
        return snapshot;
    }

private:
    std::atomic<size_t> rawEntries_[kSourceCount];
    std::atomic<size_t> forwardedObjects_[kSourceCount];
};

} // namespace MapleRuntime

#endif // MRT_MSGC_WRITE_BARRIER_FLUSH_STATS_H
