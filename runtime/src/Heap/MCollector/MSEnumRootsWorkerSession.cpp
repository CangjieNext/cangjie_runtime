// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#include "Heap/MCollector/MSEnumRootsWorkerSession.h"

#ifdef MRT_USE_MARK_SWEEP

#include <memory>
#include <vector>

#include "Base/TimeUtils.h"
#include "Heap/Collector/CollectorResources.h"
#include "Heap/Collector/MSGCWorkerPoolController.h"
#include "Heap/GcThreadPool.h"
#include "Heap/HeapWork.h"

namespace MapleRuntime {

MSEnumRootsWorkerSession::~MSEnumRootsWorkerSession()
{
    CHECK_DETAIL(!started_, "destroying active EnumRoots worker session");
}

bool MSEnumRootsWorkerSession::Start(
    CollectorResources& resources, size_t helperCount)
{
    CHECK_DETAIL(!started_, "starting an already active EnumRoots worker session");
    if (helperCount == 0 || resources.GetThreadPool() == nullptr) {
        return false;
    }

    std::vector<std::unique_ptr<HeapWork>> stagedTasks;
    stagedTasks.reserve(helperCount);
    for (size_t laneIndex = 0; laneIndex < helperCount; ++laneIndex) {
        std::unique_ptr<HeapWork> task(new (std::nothrow) LambdaWork(
            [this, laneIndex](size_t) { WorkerLoop(laneIndex); }));
        if (task == nullptr) {
            return false;
        }
        stagedTasks.push_back(std::move(task));
    }

    if (!resources.AcquireMSGCWorkerPoolForEnumRoots()) {
        return false;
    }

    resources_ = &resources;
    threadPool_ = resources.GetThreadPool();
    helperCount_ = helperCount;
    oldActiveThreadCount_ = threadPool_->GetMaxActiveThreadNum();
    stopping_ = false;
    stageGeneration_ = 0;
    stageRemaining_ = 0;
    stageWork_ = nullptr;
    started_ = true;

    threadPool_->SetMaxActiveThreadNum(static_cast<int32_t>(helperCount_));
    for (std::unique_ptr<HeapWork>& task : stagedTasks) {
        threadPool_->AddWork(task.release());
    }
    threadPool_->Start();
    return true;
}

void MSEnumRootsWorkerSession::RunStage(
    const std::function<void(size_t)>& work)
{
    if (!started_) {
        return;
    }
    std::lock_guard<std::mutex> guard(lock_);
    CHECK_DETAIL(stageRemaining_ == 0 && !stopping_,
                 "overlapping EnumRoots worker stages");
    stageWork_ = work;
    stageRemaining_ = helperCount_;
    stageStartUs_ = TimeUtil::MicroSeconds();
    ++stageGeneration_;
    stageReady_.notify_all();
}

uint64_t MSEnumRootsWorkerSession::WaitStage()
{
    if (!started_) {
        return 0;
    }
    std::unique_lock<std::mutex> lock(lock_);
    stageDone_.wait(lock, [this] { return stageRemaining_ == 0; });
    uint64_t elapsedUs = TimeUtil::MicroSeconds() - stageStartUs_;
    stageWork_ = nullptr;
    return elapsedUs;
}

void MSEnumRootsWorkerSession::Finish()
{
    if (!started_) {
        return;
    }
    {
        std::lock_guard<std::mutex> guard(lock_);
        CHECK_DETAIL(stageRemaining_ == 0,
                     "finishing EnumRoots session with active stage");
        stopping_ = true;
        stageReady_.notify_all();
    }
    threadPool_->WaitFinish();
    threadPool_->SetMaxActiveThreadNum(oldActiveThreadCount_);
    resources_->ReleaseMSGCWorkerPool(MSGCWorkerPoolOwner::ENUM_ROOTS);

    resources_ = nullptr;
    threadPool_ = nullptr;
    helperCount_ = 0;
    oldActiveThreadCount_ = 0;
    started_ = false;
}

void MSEnumRootsWorkerSession::WorkerLoop(size_t laneIndex)
{
    uint64_t observedGeneration = 0;
    for (;;) {
        std::function<void(size_t)> work;
        {
            std::unique_lock<std::mutex> lock(lock_);
            stageReady_.wait(lock, [this, observedGeneration] {
                return stopping_ || stageGeneration_ != observedGeneration;
            });
            if (stopping_) {
                return;
            }
            observedGeneration = stageGeneration_;
            work = stageWork_;
        }

        work(laneIndex);

        {
            std::lock_guard<std::mutex> guard(lock_);
            CHECK_DETAIL(stageRemaining_ != 0,
                         "EnumRoots helper completed an inactive stage");
            if (--stageRemaining_ == 0) {
                stageDone_.notify_all();
            }
        }
    }
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
