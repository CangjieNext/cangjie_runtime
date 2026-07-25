// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_MARK_LOCAL_STATS_H
#define MRT_MS_MARK_LOCAL_STATS_H

#include <cstddef>

namespace MapleRuntime {

class MSMarkWorkerLocalStats {
public:
    void RecordScanned(size_t scanned)
    {
        totalProcessed_ += scanned;
    }

    size_t TotalProcessed() const { return totalProcessed_; }

private:
    size_t totalProcessed_ = 0;
};

struct MSMarkAssistStats {
    size_t processed = 0;

    void Merge(const MSMarkAssistStats& other)
    {
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
