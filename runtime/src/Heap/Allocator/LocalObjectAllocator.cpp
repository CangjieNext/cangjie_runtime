// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "Allocator/LocalObjectAllocator.h"

#include "Allocator/Allocator.h"
#include "Allocator/RegionInfo.h"
#include "Allocator/RegionManager.h"
#include "Base/Globals.h"
#include "Base/Log.h"
#include "Common/NativeAllocator.h"
#include "ExceptionManager.inline.h"
#include "Heap/Heap.h"
#include "Mutator/MutatorManager.h"
#include "Mutator/Mutator.h"
#include "ObjectModel/MObject.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <vector>

namespace MapleRuntime {
class LocalObjectRootRegistry {
public:
    void AddFinalizer(BaseObject* obj);
    bool RunFinalizer(Mutator& mutator, BaseObject* obj);
    void AddLocalRoot(BaseObject* obj);
    void VisitRegisteredObjects(
        const std::function<void(BaseObject*, LocalObjectInfo::RootKind, uint8_t)>& visitor) const;
    void VisitLocalObjectRefFields(const RootVisitor& visitor) const;
    bool RunPendingFinalizers(Mutator& mutator);

private:
    enum class FinalizerState : uint8_t {
        PENDING,
        RUNNING,
        DONE,
    };

    struct FinalizerEntry {
        BaseObject* obj;
        FinalizerState state;
    };

    void VisitOneLocalObject(BaseObject* obj, const RootVisitor& visitor) const;
    bool RunFinalizerAt(Mutator& mutator, size_t idx);

    // DONE entries retain the object identity to reject accidental re-registration,
    // but are not visited as roots and can be compacted if churn becomes measurable.
    std::vector<FinalizerEntry> finalizers;
    std::vector<BaseObject*> localRoots;
};

// Heap-backed local allocator. It preserves the current local region
// implementation while hiding RegionInfo stack manipulation behind
// LocalObjectAllocator.
class HeapLocalObjectAllocator final : public LocalObjectAllocator {
public:
    explicit HeapLocalObjectAllocator(RegionManager& regionManager) : regionManager(regionManager) {}
    ~HeapLocalObjectAllocator() override = default;

    bool StartRegion(Mutator& mutator, FrameAddress* ownerFA) override;
    void EndRegion(Mutator& mutator, FrameAddress* ownerFA) override;
    void EndRegionsForFrame(Mutator& mutator, FrameAddress* ownerFA) override;
    MAddress Allocate(Mutator& mutator, size_t size) override;
    size_t GetHeapAllocatedBytes() const override
    {
        return heapAllocatedBytes.load(std::memory_order_relaxed);
    }
    size_t GetAllocatedObjectBytes() const override
    {
        return heapAllocatedBytes.load(std::memory_order_relaxed);
    }
    void AddFinalizer(Mutator& mutator, BaseObject* obj) override;
    void RemoveFinalizer(Mutator& mutator, BaseObject* obj) override;
    void AddLocalRoot(Mutator& mutator, BaseObject* obj) override;
    void VisitLocalObjectRefFields(Mutator& mutator, const RootVisitor& visitor) override;
    void OnMutatorExit(Mutator& mutator) override;

private:
    struct LocalHeapFirstBlock;

    struct LocalHeapBlock {
        LocalHeapBlock(RegionInfo* region, LocalHeapBlock* prev, LocalHeapFirstBlock* firstBlock)
            : region(region), prev(prev), firstBlock(firstBlock) {}

        RegionInfo* region;
        LocalHeapBlock* prev;
        size_t allocatedBytes = 0;
        LocalHeapFirstBlock* firstBlock;
    };

    struct LocalHeapFirstBlock final : LocalHeapBlock {
        LocalHeapFirstBlock(RegionInfo* region, LocalHeapBlock* prev, FrameAddress* ownerFA)
            : LocalHeapBlock(region, prev, this), ownerFA(ownerFA) {}

        FrameAddress* ownerFA;
        LocalObjectRootRegistry rootRegistry;
    };

    LocalHeapBlock* GetBlockStackTop(const Mutator& mutator) const;
    void SetBlockStackTop(Mutator& mutator, LocalHeapBlock* block);
    bool PushBlock(Mutator& mutator, RegionInfo* region, bool firstOfRegion, FrameAddress* ownerFA);
    LocalHeapBlock* PopRegionBlocks(Mutator& mutator, FrameAddress* ownerFA);
    LocalHeapBlock* FindBlockOfObject(Mutator& mutator, BaseObject* obj);
    void RunLocalFinalizers(Mutator& mutator, FrameAddress* ownerFA);
    void RecordAllocation(LocalHeapBlock& block, size_t size);
    void RecordReclamation(const LocalHeapBlock& block);

    RegionManager& regionManager;
    std::atomic<size_t> heapAllocatedBytes = { 0 };
};

// Native local allocator.  The active storage is a doubly-linked chunk list and
// region lifetime is represented by an explicit frame snapshot (top chunk + bump
// pointer). This keeps nested regions cheap: ending a region only rolls back the
// suffix allocated after its snapshot.
class NativeLocalObjectAllocator final : public LocalObjectAllocator {
public:
    ~NativeLocalObjectAllocator() override = default;

    bool StartRegion(Mutator& mutator, FrameAddress* ownerFA) override;
    void EndRegion(Mutator& mutator, FrameAddress* ownerFA) override;
    void EndRegionsForFrame(Mutator& mutator, FrameAddress* ownerFA) override;
    MAddress Allocate(Mutator& mutator, size_t size) override;
    size_t GetHeapAllocatedBytes() const override { return 0; }
    size_t GetAllocatedObjectBytes() const override;
    void AddFinalizer(Mutator& mutator, BaseObject* obj) override;
    void RemoveFinalizer(Mutator& mutator, BaseObject* obj) override;
    void AddLocalRoot(Mutator& mutator, BaseObject* obj) override;
    void VisitLocalObjectRefFields(Mutator& mutator, const RootVisitor& visitor) override;
    void OnMutatorExit(Mutator& mutator) override;
    void VisitLocalObjects(const LocalObjectVisitor& visitor) const override;
    void VisitLocalRoots(const LocalObjectVisitor& visitor) const override;
    bool HasLocalObjects() const override { return true; }

private:
    struct RegionFrame;
    struct RegionChunk {
        RegionChunk* prev = nullptr;
        RegionChunk* next = nullptr;
        MAddress base = 0;
        MAddress top = 0;
        MAddress end = 0;
        size_t capacity = 0;
        void* memory = nullptr;
        RegionFrame* ownerFrame = nullptr;
        bool largeObject = false;
    };

    struct RegionFrame {
        RegionFrame* prev = nullptr;
        uint8_t* top = nullptr;
        RegionChunk* chunk = nullptr;
        FrameAddress* ownerFA = nullptr;
        LocalObjectRootRegistry rootRegistry;
    };

    struct MutatorData {
        Mutator* ownerMutator = nullptr;
        RegionChunk* regionData = nullptr;
        RegionChunk* currentChunk = nullptr;
        uint8_t* regionTop = nullptr;
        RegionFrame* currentFrame = nullptr;
    };

    static constexpr size_t DEFAULT_CHUNK_SIZE = 4 * KB;

    ALWAYS_INLINE MutatorData* GetData(const Mutator& mutator) const
    {
        return static_cast<MutatorData*>(mutator.GetLocalObjectAllocatorData());
    }
    MutatorData* CreateData(Mutator& mutator);
    RegionChunk* AllocateChunk(size_t minSize, RegionFrame* ownerFrame);
    void ReleaseChunk(RegionChunk* chunk);
    void AppendActiveChunk(MutatorData& data, RegionChunk* chunk);
    void DetachActiveSuffix(MutatorData& data, RegionChunk* snapshotChunk);
    ALWAYS_INLINE bool IsAddressInChunk(const RegionChunk& chunk, MAddress addr) const
    {
        // `addr` is the object address returned to the object model, while allocation starts at the preceding
        // allocator header. The live interval therefore must use the chunk's bump pointer, not an exact large-object
        // allocation address or the whole reserved capacity.
        return addr >= chunk.base && addr < chunk.top;
    }
    RegionChunk* FindOwnerChunkForObject(MutatorData& data, MAddress addr) const;
    ALWAYS_INLINE MAddress TryAllocate(MutatorData& data, size_t size);
    void RunFinalizers(Mutator& mutator, RegionFrame& frame);
    void EndTopFrame(Mutator& mutator, MutatorData& data);

    mutable std::mutex activeDataMtx;
    std::vector<MutatorData*> activeData;
};

namespace {
void RunLocalObjectFinalizer(Mutator& mutator, BaseObject* obj)
{
    TypeInfo* classInfo = reinterpret_cast<MObject*>(obj)->GetTypeInfo();
    FuncRef finalizerMethod = classInfo->GetFinalizeMethod();
    CHECK_DETAIL(finalizerMethod != nullptr, "%p has no finalize method", obj);
    void (*finalizer)(BaseObject*, TypeInfo*) = reinterpret_cast<void (*)(BaseObject*, TypeInfo*)>(finalizerMethod);

    bool oldManagedContext = mutator.IsManagedContext();
    mutator.SetManagedContext(true);
    DLOG(FINALIZE, "tid %u finalize local object %p", mutator.GetTid(), obj);
    uintptr_t threadData = MapleRuntime::MRT_GetThreadLocalData();
    ExecuteCangjieStub(obj, obj->GetTypeInfo(), 0, reinterpret_cast<void*>(finalizer),
                       reinterpret_cast<void*>(threadData), 0);
    mutator.SetManagedContext(oldManagedContext);

    if (ExceptionManager::HasFatalException()) {
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        ExceptionManager::DumpException();
#endif
        LOG(RTLOG_FATAL, "FatalException happened in local finalizer");
    }
    ExceptionManager::ClearPendingException();
}
}

void LocalObjectRootRegistry::AddFinalizer(BaseObject* obj)
{
    if (obj == nullptr) {
        return;
    }
    for (const FinalizerEntry& entry : finalizers) {
        if (entry.obj == obj) {
            return;
        }
    }
    finalizers.push_back({ obj, FinalizerState::PENDING });
}

bool LocalObjectRootRegistry::RunFinalizer(Mutator& mutator, BaseObject* obj)
{
    if (obj == nullptr) {
        return false;
    }
    for (size_t idx = 0; idx < finalizers.size(); ++idx) {
        if (finalizers[idx].obj == obj) {
            return RunFinalizerAt(mutator, idx);
        }
    }
    return false;
}

void LocalObjectRootRegistry::AddLocalRoot(BaseObject* obj)
{
    if (obj == nullptr) {
        return;
    }
    for (BaseObject* localRoot : localRoots) {
        if (localRoot == obj) {
            return;
        }
    }
    localRoots.push_back(obj);
}

void LocalObjectRootRegistry::VisitRegisteredObjects(
    const std::function<void(BaseObject*, LocalObjectInfo::RootKind, uint8_t)>& visitor) const
{
    for (BaseObject* obj : localRoots) {
        if (obj != nullptr) {
            visitor(obj, LocalObjectInfo::RootKind::LOCAL_OBJECT, 0);
        }
    }
    for (const FinalizerEntry& entry : finalizers) {
        if (entry.obj != nullptr && entry.state != FinalizerState::DONE) {
            visitor(entry.obj, LocalObjectInfo::RootKind::FINALIZER,
                    static_cast<uint8_t>(entry.state));
        }
    }
}

void LocalObjectRootRegistry::VisitLocalObjectRefFields(const RootVisitor& visitor) const
{
    for (BaseObject* localRoot : localRoots) {
        VisitOneLocalObject(localRoot, visitor);
    }
    for (const FinalizerEntry& entry : finalizers) {
        if (entry.state != FinalizerState::DONE) {
            // RUNNING finalizers remain scan sources while user code executes,
            // so a re-entrant GC can still trace and update their heap fields.
            VisitOneLocalObject(entry.obj, visitor);
        }
    }
}

void LocalObjectRootRegistry::VisitOneLocalObject(BaseObject* obj, const RootVisitor& visitor) const
{
    if (obj == nullptr) {
        return;
    }
    obj->ForEachRefField([&visitor](RefField<>& field) {
        BaseObject* target = field.GetTargetObject();
        if (target == nullptr || !Heap::IsHeapAddress(target)) {
            return;
        }
        visitor(reinterpret_cast<ObjectRef&>(field));
    });
}

bool LocalObjectRootRegistry::RunPendingFinalizers(Mutator& mutator)
{
    bool ranFinalizer = false;
    for (size_t idx = 0; idx < finalizers.size(); ++idx) {
        if (finalizers[idx].state != FinalizerState::PENDING) {
            continue;
        }
        ranFinalizer = RunFinalizerAt(mutator, idx) || ranFinalizer;
    }
    return ranFinalizer;
}

bool LocalObjectRootRegistry::RunFinalizerAt(Mutator& mutator, size_t idx)
{
    CHECK_DETAIL(idx < finalizers.size(), "invalid local finalizer index %zu", idx);
    if (finalizers[idx].state != FinalizerState::PENDING) {
        return false;
    }

    BaseObject* finalizerObj = finalizers[idx].obj;
    finalizers[idx].state = FinalizerState::RUNNING;
    RunLocalObjectFinalizer(mutator, finalizerObj);

    // User code may register more finalizers and reallocate the vector, so use
    // the stable index instead of retaining a reference across the call.
    CHECK_DETAIL(idx < finalizers.size() && finalizers[idx].obj == finalizerObj,
                 "local finalizer registry changed running entry %p", finalizerObj);
    finalizers[idx].state = FinalizerState::DONE;
    return true;
}

LocalObjectAllocator* LocalObjectAllocator::InitLocalObjectAllocator(RegionManager& regionManager)
{
    (void)regionManager;
    LocalObjectAllocator* localObjectAllocator = new (std::nothrow) NativeLocalObjectAllocator();
    VLOG(LOCAL_REGION, "use native local object allocator");
    CHECK_DETAIL(localObjectAllocator != nullptr, "new local object allocator failed");
    return localObjectAllocator;
}

bool HeapLocalObjectAllocator::StartRegion(Mutator& mutator, FrameAddress* ownerFA)
{
    RegionInfo* region = regionManager.AllocateLocalModeRegion();
    if (region == nullptr) {
        // todo gc or oom
        VLOG(LOCAL_REGION, "Start local object region failed, region is null");
        return false;
    }
    if (!PushBlock(mutator, region, true, ownerFA)) {
        regionManager.ReclaimLocalModeRegion(region);
        return false;
    }
    VLOG(LOCAL_REGION, "StartLocalRegion: mutator %p, region %p, ownerFA %p, size %zu, stackTop %p", &mutator,
         region, ownerFA, region->GetRegionSize(), GetBlockStackTop(mutator));
    return true;
}

void HeapLocalObjectAllocator::EndRegion(Mutator& mutator, FrameAddress* ownerFA)
{
    RunLocalFinalizers(mutator, ownerFA);
    LocalHeapBlock* headBlock = PopRegionBlocks(mutator, ownerFA);
    while (headBlock != nullptr) {
        LocalHeapBlock* del = headBlock;
        headBlock = headBlock->prev;
        VLOG(LOCAL_REGION, "CollectLocalGarbage: region %p, ownerFA %p", del->region, del->firstBlock->ownerFA);
        RecordReclamation(*del);
        regionManager.ReclaimLocalModeRegion(del->region);
        if (del->firstBlock == del) {
            delete static_cast<LocalHeapFirstBlock*>(del);
        } else {
            delete del;
        }
    }
}

void HeapLocalObjectAllocator::EndRegionsForFrame(Mutator& mutator, FrameAddress* ownerFA)
{
    if (ownerFA == nullptr) {
        return;
    }
    LocalHeapBlock* block = GetBlockStackTop(mutator);
    while (block != nullptr) {
        if (block->firstBlock->ownerFA != ownerFA) {
            return;
        }
        EndRegion(mutator, ownerFA);
        block = GetBlockStackTop(mutator);
    }
}

void HeapLocalObjectAllocator::OnMutatorExit(Mutator& mutator)
{
    // Reclaim blocks still alive (abnormal exit with local regions active).
    // Finalizers are not run here: this is called during thread teardown and
    // running user code would risk re-entering the runtime.
    LocalHeapBlock* block = GetBlockStackTop(mutator);
    while (block != nullptr) {
        LocalHeapBlock* del = block;
        block = block->prev;
        RecordReclamation(*del);
        regionManager.ReclaimLocalModeRegion(del->region);
        if (del->firstBlock == del) {
            delete static_cast<LocalHeapFirstBlock*>(del);
        } else {
            delete del;
        }
    }
    SetBlockStackTop(mutator, nullptr);
}

MAddress HeapLocalObjectAllocator::Allocate(Mutator& mutator, size_t size)
{
    MAddress addr = 0;
    LocalHeapBlock* block = GetBlockStackTop(mutator);
    if (UNLIKELY(block == nullptr)) {
        LOG(RTLOG_ERROR, "alloc heap local object without local object region");
        return 0;
    }

    addr = block->region->Alloc(size);
    if (addr != 0) {
        RecordAllocation(*block, size);
        VLOG(LOCAL_REGION,
             "AllocLocalMode in old region: mutator %p, regoin %p(isFirst %d), "
             "ownerFA %p, size %zu, stackTop %p, obj %p, objSize %zu",
             &mutator, block->region, block->firstBlock == block, block->firstBlock->ownerFA,
             block->region->GetRegionSize(),
             GetBlockStackTop(mutator), addr, size);
        return addr;
    }

    RegionInfo* region = regionManager.AllocateLocalModeRegion(size);
    if (region == nullptr) {
        DLOG(REGION, "cannot take a local mode region from heap");
        return 0;
    }
    if (!PushBlock(mutator, region, false, block->firstBlock->ownerFA)) {
        regionManager.ReclaimLocalModeRegion(region);
        return 0;
    }
    addr = region->Alloc(size);
    CHECK_DETAIL(addr != 0, "alloc local object failed");
    LocalHeapBlock* newBlock = GetBlockStackTop(mutator);
    CHECK_DETAIL(newBlock != nullptr && newBlock->region == region, "invalid local heap block for region %p", region);
    RecordAllocation(*newBlock, size);
    VLOG(LOCAL_REGION,
         "AllocLocalMode in new region: mutator %p, regoin %p(isFirst %d), "
         "ownerFA %p, size %zu, stackTop %p, obj %p, objSize %zu",
         &mutator, region, false, block->firstBlock->ownerFA, region->GetRegionSize(),
         GetBlockStackTop(mutator), addr, size);
    return addr;
}

void HeapLocalObjectAllocator::AddFinalizer(Mutator& mutator, BaseObject* obj)
{
    if (obj == nullptr) {
        return;
    }
    LocalHeapBlock* block = GetBlockStackTop(mutator);
    CHECK_DETAIL(block != nullptr, "add local finalizer without current local region: %p", obj);
    block->firstBlock->rootRegistry.AddFinalizer(obj);
    VLOG(LOCAL_REGION, "AddLocalFinalizer: mutator %p, block %p, obj %p", &mutator, block, obj);
}

void HeapLocalObjectAllocator::RemoveFinalizer(Mutator& mutator, BaseObject* obj)
{
    if (obj == nullptr) {
        return;
    }
    LocalHeapBlock* block = FindBlockOfObject(mutator, obj);
    if (block != nullptr && block->firstBlock->rootRegistry.RunFinalizer(mutator, obj)) {
        VLOG(LOCAL_REGION, "RemoveLocalFinalizer: mutator %p, block %p, obj %p", &mutator, block, obj);
    }
}

void HeapLocalObjectAllocator::AddLocalRoot(Mutator& mutator, BaseObject* obj)
{
    if (obj == nullptr) {
        return;
    }
    LocalHeapBlock* block = FindBlockOfObject(mutator, obj);
    CHECK_DETAIL(block != nullptr, "add local root with non-local object %p", obj);
    block->firstBlock->rootRegistry.AddLocalRoot(obj);
    VLOG(LOCAL_REGION, "AddLocalRoot: mutator %p, block %p, obj %p", &mutator, block, obj);
}

void HeapLocalObjectAllocator::VisitLocalObjectRefFields(Mutator& mutator, const RootVisitor& visitor)
{
    for (LocalHeapBlock* block = GetBlockStackTop(mutator); block != nullptr; block = block->prev) {
        if (block->firstBlock != block) {
            continue;
        }
        block->firstBlock->rootRegistry.VisitLocalObjectRefFields(visitor);
    }
}

HeapLocalObjectAllocator::LocalHeapBlock* HeapLocalObjectAllocator::GetBlockStackTop(const Mutator& mutator) const
{
    return reinterpret_cast<LocalHeapBlock*>(mutator.GetLocalObjectAllocatorData());
}

void HeapLocalObjectAllocator::SetBlockStackTop(Mutator& mutator, LocalHeapBlock* block)
{
    mutator.SetLocalObjectAllocatorData(block);
}

bool HeapLocalObjectAllocator::PushBlock(Mutator& mutator, RegionInfo* region, bool firstOfRegion,
                                         FrameAddress* ownerFA)
{
    LocalHeapBlock* prev = GetBlockStackTop(mutator);
    CHECK_DETAIL(firstOfRegion || prev != nullptr, "push non-first local heap block without active region");
    LocalHeapBlock* block = firstOfRegion ? static_cast<LocalHeapBlock*>(
        new (std::nothrow) LocalHeapFirstBlock(region, prev, ownerFA))
        : new (std::nothrow) LocalHeapBlock(region, prev, prev->firstBlock);
    if (block == nullptr) {
        VLOG(LOCAL_REGION, "push local heap block failed, region %p", region);
        return false;
    }
    SetBlockStackTop(mutator, block);
    return true;
}

HeapLocalObjectAllocator::LocalHeapBlock* HeapLocalObjectAllocator::PopRegionBlocks(
    Mutator& mutator, FrameAddress* ownerFA)
{
    LocalHeapBlock* curr = GetBlockStackTop(mutator);
    if (curr != nullptr && ownerFA != nullptr && curr->firstBlock->ownerFA != ownerFA) {
        LOG(RTLOG_FATAL, "end local heap region with mismatched frame: top ownerFA %p, target ownerFA %p",
            curr->firstBlock->ownerFA, ownerFA);
    }
    LocalHeapBlock* ret = curr;
    while (curr != nullptr) {
        LocalHeapBlock* prev = curr->prev;
        if (ownerFA != nullptr && curr->firstBlock->ownerFA != ownerFA) {
            LOG(RTLOG_FATAL, "local heap region crosses frame boundary: block ownerFA %p, target ownerFA %p",
                curr->firstBlock->ownerFA, ownerFA);
        }
        if (curr->firstBlock == curr) {
            SetBlockStackTop(mutator, prev);
            curr->prev = nullptr;
            break;
        }
        curr = prev;
    }
    return ret;
}

HeapLocalObjectAllocator::LocalHeapBlock* HeapLocalObjectAllocator::FindBlockOfObject(Mutator& mutator,
                                                                                      BaseObject* obj)
{
    if (obj == nullptr || !Heap::IsHeapAddress(obj)) {
        return nullptr;
    }
    RegionInfo* region = RegionInfo::GetRegionInfoAt(reinterpret_cast<MAddress>(obj));
    if (!region->IsLocalModeRegion()) {
        return nullptr;
    }
    for (LocalHeapBlock* block = GetBlockStackTop(mutator); block != nullptr; block = block->prev) {
        if (block->region == region) {
            return block;
        }
    }
    return nullptr;
}

void HeapLocalObjectAllocator::RunLocalFinalizers(Mutator& mutator, FrameAddress* ownerFA)
{
    LocalHeapBlock* block = GetBlockStackTop(mutator);
    if (block == nullptr) {
        return;
    }
    if (ownerFA != nullptr && block->firstBlock->ownerFA != ownerFA) {
        LOG(RTLOG_FATAL, "run local heap finalizers with mismatched frame: block ownerFA %p, target ownerFA %p",
            block->firstBlock->ownerFA, ownerFA);
    }
    while (block->firstBlock->rootRegistry.RunPendingFinalizers(mutator)) {
    }
}

void HeapLocalObjectAllocator::RecordAllocation(LocalHeapBlock& block, size_t size)
{
    CHECK_DETAIL(block.allocatedBytes <= std::numeric_limits<size_t>::max() - size,
                 "local heap block allocated bytes overflow");
    block.allocatedBytes += size;
    (void)heapAllocatedBytes.fetch_add(size, std::memory_order_relaxed);
}

void HeapLocalObjectAllocator::RecordReclamation(const LocalHeapBlock& block)
{
    size_t oldSize = heapAllocatedBytes.fetch_sub(block.allocatedBytes, std::memory_order_relaxed);
    CHECK_DETAIL(oldSize >= block.allocatedBytes,
                 "local heap allocated bytes underflow: old %zu, reclaimed %zu", oldSize, block.allocatedBytes);
}

NativeLocalObjectAllocator::MutatorData* NativeLocalObjectAllocator::CreateData(Mutator& mutator)
{
    MutatorData* data = GetData(mutator);
    if (data != nullptr) {
        return data;
    }
    data = new (std::nothrow) MutatorData();
    CHECK_DETAIL(data != nullptr, "new native local mutator data failed");
    data->ownerMutator = &mutator;
    mutator.SetLocalObjectAllocatorData(data);
    std::lock_guard<std::mutex> lock(activeDataMtx);
    activeData.push_back(data);
    return data;
}

NativeLocalObjectAllocator::RegionChunk* NativeLocalObjectAllocator::AllocateChunk(size_t minSize,
                                                                                    RegionFrame* ownerFrame)
{
    size_t alignedSize = RoundUp<size_t>(minSize, Allocator::ALLOC_ALIGN);
    size_t capacity = std::max(DEFAULT_CHUNK_SIZE, alignedSize);
    void* memory = NativeAllocator::NativeAlloc(capacity);
    if (memory == nullptr) {
        return nullptr;
    }
    RegionChunk* chunk = new (std::nothrow) RegionChunk();
    if (chunk == nullptr) {
        NativeAllocator::NativeFree(memory, capacity);
        return nullptr;
    }
    chunk->memory = memory;
    chunk->base = reinterpret_cast<MAddress>(memory);
    chunk->capacity = capacity;
    chunk->end = chunk->base + capacity;
    chunk->top = chunk->base;
    chunk->ownerFrame = ownerFrame;
    chunk->largeObject = capacity != DEFAULT_CHUNK_SIZE;
    return chunk;
}

void NativeLocalObjectAllocator::AppendActiveChunk(MutatorData& data, RegionChunk* chunk)
{
    CHECK_DETAIL(chunk != nullptr, "append null native chunk");
    chunk->prev = data.currentChunk;
    chunk->next = nullptr;
    if (data.currentChunk != nullptr) {
        data.currentChunk->next = chunk;
    } else {
        data.regionData = chunk;
    }
    data.currentChunk = chunk;
    data.regionTop = reinterpret_cast<uint8_t*>(chunk->top);
}

void NativeLocalObjectAllocator::ReleaseChunk(RegionChunk* chunk)
{
    CHECK_DETAIL(chunk != nullptr, "release null native chunk");
    NativeAllocator::NativeFree(chunk->memory, chunk->capacity);
    delete chunk;
}

bool NativeLocalObjectAllocator::StartRegion(Mutator& mutator, FrameAddress* ownerFA)
{
    MutatorData* data = CreateData(mutator);
    RegionFrame* frame = new (std::nothrow) RegionFrame();
    if (frame == nullptr) {
        return false;
    }
    frame->prev = data->currentFrame;
    frame->top = data->regionTop;
    frame->chunk = data->currentChunk;
    frame->ownerFA = ownerFA;

    // Always start a nested region on a fresh chunk. This makes object,
    // finalizer and root ownership unambiguous.
    RegionChunk* chunk = AllocateChunk(DEFAULT_CHUNK_SIZE, frame);
    if (chunk == nullptr) {
        delete frame;
        return false;
    }
    AppendActiveChunk(*data, chunk);
    data->currentFrame = frame;
    VLOG(LOCAL_REGION, "StartNativeLocalRegion: mutator %p, frame %p, ownerFA %p, chunk %p, size %zu", &mutator,
         frame, ownerFA, chunk, chunk->capacity);
    return true;
}

ALWAYS_INLINE MAddress NativeLocalObjectAllocator::TryAllocate(MutatorData& data, size_t size)
{
    RegionChunk* chunk = data.currentChunk;
    if (chunk == nullptr) {
        return 0;
    }
    size_t allocSize = RoundUp<size_t>(size, Allocator::ALLOC_ALIGN);
    MAddress addr = RoundUp<MAddress>(chunk->top, Allocator::ALLOC_ALIGN);
    if (addr > chunk->end || allocSize > chunk->end - addr) {
        return 0;
    }
    chunk->top = addr + allocSize;
    data.regionTop = reinterpret_cast<uint8_t*>(chunk->top);
    return addr;
}

MAddress NativeLocalObjectAllocator::Allocate(Mutator& mutator, size_t size)
{
    MutatorData* data = GetData(mutator);
    if (data == nullptr || data->currentFrame == nullptr) {
        LOG(RTLOG_ERROR, "alloc native local object without local object region");
        return 0;
    }
    MAddress addr = TryAllocate(*data, size);
    if (addr != 0) {
        return addr;
    }
    RegionChunk* chunk = AllocateChunk(size, data->currentFrame);
    if (chunk == nullptr) {
        return 0;
    }
    AppendActiveChunk(*data, chunk);
    addr = TryAllocate(*data, size);
    CHECK_DETAIL(addr != 0, "native local allocation failed after adding chunk");
    VLOG(LOCAL_REGION, "AllocNativeLocalObject: mutator %p, chunk %p, obj %p, objSize %zu", &mutator, chunk,
         reinterpret_cast<void*>(addr), size);
    return addr;
}

void NativeLocalObjectAllocator::RunFinalizers(Mutator& mutator, RegionFrame& frame)
{
    while (frame.rootRegistry.RunPendingFinalizers(mutator)) {
    }
}

void NativeLocalObjectAllocator::DetachActiveSuffix(MutatorData& data, RegionChunk* snapshotChunk)
{
    RegionChunk* chunk = data.currentChunk;
    while (chunk != snapshotChunk) {
        CHECK_DETAIL(chunk != nullptr, "native region snapshot chunk not found");
        RegionChunk* prev = chunk->prev;
        ReleaseChunk(chunk);
        chunk = prev;
    }
    if (snapshotChunk == nullptr) {
        data.regionData = nullptr;
        data.currentChunk = nullptr;
        data.regionTop = nullptr;
        return;
    }
    data.currentChunk = snapshotChunk;
    data.regionTop = reinterpret_cast<uint8_t*>(snapshotChunk->top);
    snapshotChunk->next = nullptr;
}

void NativeLocalObjectAllocator::EndRegion(Mutator& mutator, FrameAddress* ownerFA)
{
    MutatorData* data = GetData(mutator);
    if (data == nullptr || data->currentFrame == nullptr) {
        LOG(RTLOG_WARNING, "end native local region without active region");
        return;
    }
    RegionFrame* frame = data->currentFrame;
    if (ownerFA != nullptr && frame->ownerFA != ownerFA) {
        RegionFrame* targetFrame = frame->prev;
        while (targetFrame != nullptr && targetFrame->ownerFA != ownerFA) {
            targetFrame = targetFrame->prev;
        }
        if (targetFrame == nullptr) {
            LOG(RTLOG_WARNING, "skip native local region end for mismatched frame: ownerFA %p target %p",
                frame->ownerFA, ownerFA);
            return;
        }
        VLOG(LOCAL_REGION, "EndNativeLocalRegion across frame boundary: top ownerFA %p, target ownerFA %p",
             frame->ownerFA, ownerFA);
        while (data->currentFrame != nullptr) {
            bool reachedTarget = data->currentFrame == targetFrame;
            EndTopFrame(mutator, *data);
            if (reachedTarget) {
                return;
            }
        }
        return;
    }
    EndTopFrame(mutator, *data);
}

void NativeLocalObjectAllocator::EndTopFrame(Mutator& mutator, MutatorData& data)
{
    RegionFrame* frame = data.currentFrame;
    CHECK_DETAIL(frame != nullptr, "end native top frame without active frame");
    RunFinalizers(mutator, *frame);
    DetachActiveSuffix(data, frame->chunk);
    data.regionTop = frame->top;
    data.currentChunk = frame->chunk;
    if (data.currentChunk != nullptr) {
        data.currentChunk->top = reinterpret_cast<MAddress>(frame->top);
    }
    data.currentFrame = frame->prev;
    delete frame;
}

void NativeLocalObjectAllocator::EndRegionsForFrame(Mutator& mutator, FrameAddress* ownerFA)
{
    if (ownerFA == nullptr) {
        return;
    }
    MutatorData* data = GetData(mutator);
    while (data != nullptr && data->currentFrame != nullptr && data->currentFrame->ownerFA == ownerFA) {
        EndTopFrame(mutator, *data);
    }
}

NativeLocalObjectAllocator::RegionChunk* NativeLocalObjectAllocator::FindOwnerChunkForObject(
    MutatorData& data, MAddress addr) const
{
    if (data.currentChunk != nullptr && IsAddressInChunk(*data.currentChunk, addr)) {
        return data.currentChunk;
    }
    for (RegionChunk* chunk = data.currentChunk == nullptr ? nullptr : data.currentChunk->prev; chunk != nullptr;
         chunk = chunk->prev) {
        if (IsAddressInChunk(*chunk, addr)) {
            return chunk;
        }
    }
    return nullptr;
}

void NativeLocalObjectAllocator::VisitLocalObjects(const LocalObjectVisitor& visitor) const
{
    if (!visitor) {
        return;
    }
    CHECK_DETAIL(MutatorManager::Instance().WorldStopped(), "native local object visit requires STW");
    std::lock_guard<std::mutex> lock(activeDataMtx);
    for (MutatorData* data : activeData) {
        if (data == nullptr || data->ownerMutator == nullptr) {
            continue;
        }
        for (RegionChunk* chunk = data->regionData; chunk != nullptr; chunk = chunk->next) {
            if (chunk->base == chunk->top || chunk->ownerFrame == nullptr) {
                continue;
            }
            LocalObjectInfo info { data->ownerMutator, chunk->ownerFrame->ownerFA, chunk->base, chunk->capacity,
                                   chunk->largeObject };
            MAddress cursor = chunk->base;
            while (cursor < chunk->top) {
                BaseObject* obj = reinterpret_cast<BaseObject*>(cursor);
                size_t objSize = obj->GetSize();
                if (objSize == 0 || objSize > chunk->top - cursor) {
                    LOG(RTLOG_ERROR, "invalid local object size %zu at %p in chunk %p", objSize,
                        reinterpret_cast<void*>(cursor), chunk);
                    break;
                }
                visitor(*data->ownerMutator, obj, info);
                cursor = RoundUp<MAddress>(cursor + objSize, Allocator::ALLOC_ALIGN);
            }
        }
    }
}

void NativeLocalObjectAllocator::VisitLocalRoots(const LocalObjectVisitor& visitor) const
{
    if (!visitor) {
        return;
    }
    CHECK_DETAIL(MutatorManager::Instance().WorldStopped(), "native local root visit requires STW");
    std::lock_guard<std::mutex> lock(activeDataMtx);
    for (MutatorData* data : activeData) {
        if (data == nullptr || data->ownerMutator == nullptr) {
            continue;
        }
        for (RegionFrame* frame = data->currentFrame; frame != nullptr; frame = frame->prev) {
            frame->rootRegistry.VisitRegisteredObjects(
                [this, data, frame, &visitor](BaseObject* obj, LocalObjectInfo::RootKind rootKind,
                                              uint8_t finalizerState) {
                RegionChunk* chunk = FindOwnerChunkForObject(*data, reinterpret_cast<MAddress>(obj));
                if (chunk == nullptr || chunk->ownerFrame != frame) {
                    LOG(RTLOG_ERROR, "registered local root %p has no active owning chunk", obj);
                    return;
                }
                LocalObjectInfo info { data->ownerMutator, frame->ownerFA, chunk->base, chunk->capacity,
                                       chunk->largeObject, rootKind, finalizerState };
                visitor(*data->ownerMutator, obj, info);
            });
        }
    }
}

void NativeLocalObjectAllocator::AddFinalizer(Mutator& mutator, BaseObject* obj)
{
    if (obj == nullptr) {
        return;
    }
    MutatorData* data = GetData(mutator);
    RegionChunk* chunk = data == nullptr ? nullptr : FindOwnerChunkForObject(*data, reinterpret_cast<MAddress>(obj));
    if (chunk == nullptr || chunk->ownerFrame == nullptr) {
        LOG(RTLOG_ERROR,
            "add native local finalizer with object not in active chunk: obj %p, currentChunk %p",
            obj, data == nullptr ? nullptr : data->currentChunk);
        return;
    }
    chunk->ownerFrame->rootRegistry.AddFinalizer(obj);
}

void NativeLocalObjectAllocator::RemoveFinalizer(Mutator& mutator, BaseObject* obj)
{
    if (obj == nullptr) {
        return;
    }
    MutatorData* data = GetData(mutator);
    RegionChunk* chunk = data == nullptr ? nullptr : FindOwnerChunkForObject(*data, reinterpret_cast<MAddress>(obj));
    if (chunk == nullptr || chunk->ownerFrame == nullptr) {
        LOG(RTLOG_ERROR,
            "remove native local finalizer with object not in active chunk: obj %p, currentChunk %p",
            obj, data == nullptr ? nullptr : data->currentChunk);
        return;
    }
    (void)chunk->ownerFrame->rootRegistry.RunFinalizer(mutator, obj);
}

void NativeLocalObjectAllocator::AddLocalRoot(Mutator& mutator, BaseObject* obj)
{
    if (obj == nullptr) {
        return;
    }
    MutatorData* data = GetData(mutator);
    RegionChunk* chunk = data == nullptr ? nullptr :
        FindOwnerChunkForObject(*data, reinterpret_cast<MAddress>(obj));
    CHECK_DETAIL(chunk != nullptr && chunk->ownerFrame != nullptr,
                 "add native local root with non-local object: %p", obj);
    chunk->ownerFrame->rootRegistry.AddLocalRoot(obj);
}

void NativeLocalObjectAllocator::VisitLocalObjectRefFields(Mutator& mutator, const RootVisitor& visitor)
{
    MutatorData* data = GetData(mutator);
    if (data == nullptr) {
        return;
    }
    for (RegionFrame* frame = data->currentFrame; frame != nullptr; frame = frame->prev) {
        frame->rootRegistry.VisitLocalObjectRefFields(visitor);
    }
}

size_t NativeLocalObjectAllocator::GetAllocatedObjectBytes() const
{
    CHECK_DETAIL(MutatorManager::Instance().WorldStopped(), "native local allocated bytes query requires STW");
    size_t total = 0;
    std::lock_guard<std::mutex> lock(activeDataMtx);
    for (MutatorData* data : activeData) {
        if (data == nullptr) {
            continue;
        }
        for (RegionChunk* chunk = data->regionData; chunk != nullptr; chunk = chunk->next) {
            CHECK_DETAIL(chunk->top >= chunk->base && chunk->top <= chunk->end,
                         "invalid native local chunk range: base %p top %p end %p",
                         reinterpret_cast<void*>(chunk->base), reinterpret_cast<void*>(chunk->top),
                         reinterpret_cast<void*>(chunk->end));
            size_t usedBytes = chunk->top - chunk->base;
            CHECK_DETAIL(total <= std::numeric_limits<size_t>::max() - usedBytes,
                         "native local allocated bytes overflow");
            total += usedBytes;
        }
    }
    return total;
}

void NativeLocalObjectAllocator::OnMutatorExit(Mutator& mutator)
{
    MutatorData* data = GetData(mutator);
    if (data == nullptr) {
        return;
    }
    RegionChunk* chunk = data->regionData;
    while (chunk != nullptr) {
        RegionChunk* next = chunk->next;
        NativeAllocator::NativeFree(chunk->memory, chunk->capacity);
        delete chunk;
        chunk = next;
    }
    while (data->currentFrame != nullptr) {
        RegionFrame* frame = data->currentFrame;
        data->currentFrame = frame->prev;
        delete frame;
    }
    {
        std::lock_guard<std::mutex> lock(activeDataMtx);
        auto it = std::find(activeData.begin(), activeData.end(), data);
        if (it != activeData.end()) {
            *it = activeData.back();
            activeData.pop_back();
        }
    }
    mutator.SetLocalObjectAllocatorData(nullptr);
    delete data;
}
} // namespace MapleRuntime
