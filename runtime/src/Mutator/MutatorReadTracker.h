// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_MUTATOR_READ_TRACKER_H
#define MRT_MUTATOR_READ_TRACKER_H

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <sys/types.h>
#include <unordered_map>

namespace MapleRuntime {

// Call site that acquired the mutator management read lock. Diagnostics only.
enum class MutatorReadSite : uint8_t {
    UNKNOWN = 0,
    FINALIZER_BIND,
    FINALIZER_INIT,
    FINALIZER_FINI,
    CREATE_MUTATOR,
    CREATE_RUNTIME_MUTATOR,
    DESTROY_MUTATOR,
    DESTROY_RUNTIME_MUTATOR,
    END_FINALIZER
};

struct ReaderInfo {
    pid_t tid = 0;
    uint64_t startNs = 0;
    MutatorReadSite site = MutatorReadSite::UNKNOWN;
};

// Tracks the readers of the mutator management lock with a map:
//   read lock acquired : readers[tid] = now
//   read lock released : readers.erase(tid)
//   timeout            : pick the entry with the largest now - startNs
//
// std::unordered_map is not thread safe, so a mutex protects it. Note that an
// insert allocates and every operation takes the mutex; the key is the OS tid,
// so nested acquisitions and releases on another OS thread are not represented.
class MutatorReadTracker {
public:
    // Called after the read lock is acquired.
    void OnReadLock(MutatorReadSite site);

    // Called after the read lock is released.
    void OnUnlockRead();

    // The reader that has held the lock the longest. Returns false when empty.
    bool SelectLongest(uint64_t nowNs, ReaderInfo& out) const;

    size_t ReaderCount() const;

    static const char* SiteName(MutatorReadSite site);

private:
    mutable std::mutex mutex;
    std::unordered_map<pid_t, ReaderInfo> readers;
};

} // namespace MapleRuntime

#endif // MRT_MUTATOR_READ_TRACKER_H
