// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_ROOT_SNAPSHOT_H
#define MRT_MS_ROOT_SNAPSHOT_H

#ifdef MRT_USE_MARK_SWEEP

#include <utility>
#include <vector>

#include "Heap/MCollector/MSRootDedupTable.h"

namespace MapleRuntime {
class BaseObject;

// GC-owned, single-epoch root snapshot. Collection is intentionally separate
// from validation/deduplication so every root source shares one publication
// boundary and one reusable dedup table.
class MSRootSnapshot {
public:
    void Clear() { roots_.clear(); }
    void Append(BaseObject* root) { roots_.push_back(root); }

    // Root staging has one completion boundary: validate and deduplicate every
    // collected source, then hand the immutable range to its publisher and
    // clear the staging buffer for the next epoch.
    template<typename IsValidRoot, typename Publisher>
    size_t DeduplicateAndPublish(IsValidRoot&& isValidRoot,
                                 Publisher&& publisher,
                                 size_t* collectedCount = nullptr)
    {
        if (collectedCount != nullptr) {
            *collectedCount = roots_.size();
        }
        size_t uniqueCount = Deduplicate(std::forward<IsValidRoot>(isValidRoot));
        publisher(roots_);
        roots_.clear();
        return uniqueCount;
    }

private:
    template<typename IsValidRoot>
    size_t Deduplicate(IsValidRoot&& isValidRoot)
    {
        dedupTable_.Reset(roots_.size());
        size_t uniqueCount = 0;
        for (BaseObject* root : roots_) {
            if (!isValidRoot(root)) {
                continue;
            }
            uintptr_t address = reinterpret_cast<uintptr_t>(root);
            if (!dedupTable_.Insert(address)) {
                continue;
            }
            roots_[uniqueCount++] = root;
        }
        roots_.resize(uniqueCount);
        return uniqueCount;
    }

    std::vector<BaseObject*> roots_;
    MSRootDedupTable dedupTable_;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_ROOT_SNAPSHOT_H
