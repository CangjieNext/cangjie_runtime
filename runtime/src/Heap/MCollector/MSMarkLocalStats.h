// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_MARK_LOCAL_STATS_H
#define MRT_MS_MARK_LOCAL_STATS_H

#include <atomic>
#include <cstddef>

namespace MapleRuntime {

class MSMarkWorkerLocalStats {
public:
    explicit MSMarkWorkerLocalStats(size_t buffersPerPublish)
        : buffersPerPublish_(buffersPerPublish)
    {
    }

    void RecordScanned(size_t scanned)
    {
        totalProcessed_ += scanned;
        pendingCredit_ += scanned;
        pendingBuffers_++;
        if (pendingBuffers_ >= buffersPerPublish_) {
            PublishPendingCredit();
        }
    }

    void PublishPendingCredit()
    {
        if (pendingCredit_ == 0) {
            return;
        }
        publishedCredit_.fetch_add(pendingCredit_, std::memory_order_release);
        pendingCredit_ = 0;
        pendingBuffers_ = 0;
    }

    size_t TakePublishedCredit()
    {
        return publishedCredit_.exchange(0, std::memory_order_acq_rel);
    }

    size_t TotalProcessed() const { return totalProcessed_; }

private:
    const size_t buffersPerPublish_;
    size_t totalProcessed_ = 0;
    size_t pendingCredit_ = 0;
    size_t pendingBuffers_ = 0;
    std::atomic<size_t> publishedCredit_{0};
};

struct MSMarkAssistStats {
    size_t calls = 0;
    size_t bgStolen = 0;
    size_t globalPaid = 0;
    size_t processed = 0;

    void Merge(const MSMarkAssistStats& other)
    {
        calls += other.calls;
        bgStolen += other.bgStolen;
        globalPaid += other.globalPaid;
        processed += other.processed;
    }

    MSMarkAssistStats TakeAndReset()
    {
        MSMarkAssistStats snapshot = *this;
        *this = MSMarkAssistStats{};
        return snapshot;
    }
};

} // namespace MapleRuntime

#endif // MRT_MS_MARK_LOCAL_STATS_H
