// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_MS_ENUM_ROOTS_WORKER_SESSION_H
#define MRT_MS_ENUM_ROOTS_WORKER_SESSION_H

#ifdef MRT_USE_MARK_SWEEP

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>

namespace MapleRuntime {

class CollectorResources;
class GCThreadPool;

// Keeps one GCThreadPool task epoch alive across the two parallel EnumRoots
// stages. Helpers finish transition, park here while GC main enumerates and
// deduplicates roots, then wake to publish their root ranges.
class MSEnumRootsWorkerSession {
public:
    MSEnumRootsWorkerSession() = default;
    ~MSEnumRootsWorkerSession();

    MSEnumRootsWorkerSession(const MSEnumRootsWorkerSession&) = delete;
    MSEnumRootsWorkerSession& operator=(const MSEnumRootsWorkerSession&) = delete;

    bool Start(CollectorResources& resources, size_t helperCount);
    void RunStage(const std::function<void(size_t)>& work);
    uint64_t WaitStage();
    void Finish();

    size_t GetHelperCount() const { return helperCount_; }
    bool IsStarted() const { return started_; }

private:
    void WorkerLoop(size_t laneIndex);

    CollectorResources* resources_ = nullptr;
    GCThreadPool* threadPool_ = nullptr;
    size_t helperCount_ = 0;
    int32_t oldActiveThreadCount_ = 0;
    bool started_ = false;

    std::mutex lock_;
    std::condition_variable stageReady_;
    std::condition_variable stageDone_;
    std::function<void(size_t)> stageWork_;
    uint64_t stageGeneration_ = 0;
    uint64_t stageStartUs_ = 0;
    size_t stageRemaining_ = 0;
    bool stopping_ = false;
};

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_ENUM_ROOTS_WORKER_SESSION_H
