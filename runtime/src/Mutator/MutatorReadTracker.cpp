// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#include "MutatorReadTracker.h"

#include "Base/SysCall.h"
#include "Base/TimeUtils.h"

namespace MapleRuntime {

void MutatorReadTracker::OnReadLock(MutatorReadSite site)
{
    ReaderInfo info;
    info.tid = GetTid();
    info.startNs = TimeUtil::NanoSeconds();
    info.site = site;

    std::lock_guard<std::mutex> lk(mutex);
    readers[info.tid] = info;
}

void MutatorReadTracker::OnUnlockRead()
{
    pid_t tid = GetTid();
    std::lock_guard<std::mutex> lk(mutex);
    readers.erase(tid);
}

bool MutatorReadTracker::SelectLongest(uint64_t nowNs, ReaderInfo& out) const
{
    std::lock_guard<std::mutex> lk(mutex);
    bool found = false;
    uint64_t best = 0;
    for (const auto& kv : readers) {
        uint64_t hold = (nowNs > kv.second.startNs) ? (nowNs - kv.second.startNs) : 0;
        if (!found || hold > best) {
            found = true;
            best = hold;
            out = kv.second;
        }
    }
    return found;
}

size_t MutatorReadTracker::ReaderCount() const
{
    std::lock_guard<std::mutex> lk(mutex);
    return readers.size();
}

const char* MutatorReadTracker::SiteName(MutatorReadSite site)
{
    switch (site) {
        case MutatorReadSite::FINALIZER_BIND: return "FINALIZER_BIND";
        case MutatorReadSite::FINALIZER_INIT: return "FINALIZER_INIT";
        case MutatorReadSite::FINALIZER_FINI: return "FINALIZER_FINI";
        case MutatorReadSite::CREATE_MUTATOR: return "CREATE_MUTATOR";
        case MutatorReadSite::CREATE_RUNTIME_MUTATOR: return "CREATE_RUNTIME_MUTATOR";
        case MutatorReadSite::DESTROY_MUTATOR: return "DESTROY_MUTATOR";
        case MutatorReadSite::DESTROY_RUNTIME_MUTATOR: return "DESTROY_RUNTIME_MUTATOR";
        case MutatorReadSite::END_FINALIZER: return "END_FINALIZER";
        default: return "UNKNOWN";
    }
}

} // namespace MapleRuntime
