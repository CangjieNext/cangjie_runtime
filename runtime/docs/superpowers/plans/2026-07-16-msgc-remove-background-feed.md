# MSGC Background Feed Removal Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the unused MSGC background Feed path and typed spare cache while preserving CopyGC Feed and all fixed ABI offsets.

**Architecture:** MSGC keeps only foreground Region refill and the classless local/global empty caches. Its background processor no longer schedules Feed work, while the common allocator and CopyGC finalizer Feed interfaces remain intact. Removed public configuration storage becomes a zeroed reserved byte, and removed pre-`activeLog_` cache storage becomes ABI padding.

**Tech Stack:** C++14, C99-compatible public runtime API, shell source-contract tests, CMake/build.py, Markdown architecture documentation.

---

### Task 1: Lock the removal boundary with a failing source contract

**Files:**
- Create: `test/MSGCBackgroundFeedRemovalContractTest.sh`

- [ ] **Step 1: Write the complete source contract**

Create an executable shell test with these checks:

```bash
#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MS_FILES=(
    "$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.h"
    "$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.cpp"
    "$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.h"
    "$ROOT_DIR/src/Heap/MAllocator/MSCentralRegionPool.cpp"
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.h"
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionSpace.cpp"
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionInfo.h"
    "$ROOT_DIR/src/Heap/MAllocator/MSRegionInfo.cpp"
    "$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.h"
    "$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.cpp"
)

for pattern in \
    'cachedRegion_' 'cachedRegionOrigin_' 'MSAllocCacheFillStats' \
    'BackgroundFillCache' 'RegionAcquireOrigin' 'CACHED_LOCAL' \
    'BackgroundFeedLoop' 'ShouldStartPeriodicFeed' 'HasPendingFeedWork' \
    'pendingFeedStats_' 'GetAllAllocBufferCachedRegionCount' 'HasHungryAllocBuffer'; do
    if rg -q "$pattern" "${MS_FILES[@]}"; then
        echo "stale MSGC Feed symbol remains: $pattern" >&2
        exit 1
    fi
done

if rg -q 'BackgroundFillAllocCaches' \
    "$ROOT_DIR/src/Heap/MAllocator" "$ROOT_DIR/src/Heap/MCollector"; then
    echo 'MSGC must not implement the common CopyGC background-fill hook' >&2
    exit 1
fi

if rg -q '\bmsBackgroundFeed\b|cjMSBackgroundFeed' \
    "$ROOT_DIR/src/Cangjie.h" "$ROOT_DIR/src/CangjieRuntimeApi.cpp" \
    "$ROOT_DIR/src/CjScheduler.cpp" "$ROOT_DIR/src/Heap/MCollector/BackgroundProcessor.cpp"; then
    echo 'live MSGC Feed configuration remains' >&2
    exit 1
fi

rg -q 'uint8_t reservedMSBackgroundFeed;' "$ROOT_DIR/src/Cangjie.h"
rg -q 'offsetof\(AllocBufferMS, activeLog_\) == 896' \
    "$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.cpp"
rg -q 'kNumSizeClasses \* sizeof\(std::atomic<MSRegionInfo\*>\)' \
    "$ROOT_DIR/src/Heap/MAllocator/MSAllocBuffer.h"

# CopyGC/common Feed is explicitly out of scope and must survive.
rg -q 'struct BackgroundFeedStats' "$ROOT_DIR/src/Heap/Allocator/Allocator.h"
rg -q 'virtual BackgroundFeedStats BackgroundFillAllocCaches' \
    "$ROOT_DIR/src/Heap/Allocator/Allocator.h"
rg -q 'NotifyToFeedAllocBuffers' "$ROOT_DIR/src/Heap/Collector/FinalizerProcessor.h"
rg -q 'NotifyToFeedAllocBuffers' "$ROOT_DIR/src/Heap/Allocator/AllocBuffer.cpp"

echo 'MSGC Background Feed removal contract holds'
```

- [ ] **Step 2: Run the contract and verify RED**

Run:

```bash
bash test/MSGCBackgroundFeedRemovalContractTest.sh
```

Expected: exit 1 with the first stale MSGC Feed symbol or configuration path. CopyGC preservation checks must already pass.

### Task 2: Remove MSGC configuration and background scheduling

**Files:**
- Modify: `src/Cangjie.h`
- Modify: `src/CangjieRuntimeApi.cpp`
- Modify: `src/CjScheduler.cpp`
- Modify: `src/Heap/MCollector/BackgroundProcessor.h`
- Modify: `src/Heap/MCollector/BackgroundProcessor.cpp`

- [ ] **Step 1: Preserve the public byte but remove Feed semantics**

Replace the public field and both runtime aggregate initializers with:

```cpp
/*
 * Reserved MSGC configuration byte. Must remain in place for HeapParam ABI.
 */
uint8_t reservedMSBackgroundFeed;
```

```cpp
.reservedMSBackgroundFeed = 0,
```

Delete the `ReadBoolParameter("cjMSBackgroundFeed", false)` call and every exact `msBackgroundFeed` read. Do not modify adjacent Sweep, Scavenge, Shrink, or parallel-Sweep fields.

- [ ] **Step 2: Delete the MSGC Feed scheduler state**

Remove these declarations and definitions only from `BackgroundProcessor`:

```text
DEFAULT_MS_FEED_TARGET_CACHED_PER_BUFFER
IsBackgroundFeedEnabled
ShouldStartPeriodicFeed
HasPendingFeedWork
BackgroundFeedLoop
shouldFeedHungryBuffers
pendingFeedStats_
```

Remove their branches from constructor initialization, `HasPendingWakeWork()`, `MaybeSchedulePeriodicWork()`, and the main loop. The main-loop ordering after removal must remain:

```cpp
if (shouldParallelSweep_.load(std::memory_order_relaxed)) {
    shouldContinueImmediate = ParallelSweepLoop() || shouldContinueImmediate;
    handledParallelSweep = true;
}
if (!handledParallelSweep && shouldBackgroundSweep_.load(std::memory_order_relaxed)) {
    shouldContinueImmediate = BackgroundSweepLoop() || shouldContinueImmediate;
}
if (shouldScavengeHeap_.load(std::memory_order_relaxed)) {
    shouldContinueImmediate = BackgroundScavengeLoop() || shouldContinueImmediate;
}
if (shouldShrinkHeap_.load(std::memory_order_relaxed)) {
    shouldContinueImmediate = BackgroundShrinkLoop() || shouldContinueImmediate;
}
```

- [ ] **Step 3: Re-run the contract**

Run `bash test/MSGCBackgroundFeedRemovalContractTest.sh`.

Expected: still RED because the typed spare and MSGC allocator hooks remain, but no failure may mention live configuration or `BackgroundProcessor` Feed state.

### Task 3: Remove the AllocBufferMS typed spare cache

**Files:**
- Modify: `src/Heap/MAllocator/MSAllocBuffer.h`
- Modify: `src/Heap/MAllocator/MSAllocBuffer.cpp`
- Modify: `src/Heap/MAllocator/MSRegionInfo.h`
- Modify: `src/Heap/MAllocator/MSRegionInfo.cpp`

- [ ] **Step 1: Remove Feed-only types, fields, and helpers**

Delete `MSAllocCacheFillStats`, `RegionAcquireOrigin`, `kCachedRegionSlots`, both cached arrays, and these methods:

```text
BackgroundFillCache
CountCachedRegions (both overloads)
DrainCachedRegionsToCentral
NeedsBackgroundFill
CountTotalRegions
TrimExtraCachedRegions
TryCacheRegion
```

Keep `cachedHomeShardId_`: it is the active P's `FreeManager` shard route and is unrelated to Feed.

- [ ] **Step 2: Preserve the write-barrier ABI window**

Replace the old reserved field with the platform-sized removed-array footprint:

```cpp
uint8_t wbBufAbiReserved_[184 +
    kNumSizeClasses * sizeof(std::atomic<MSRegionInfo*>)] = {};
```

Do not move `activeLog_`, `gcWork_`, or the P-level Mark/Sweep debt fields. The constructor's 896-byte `static_assert` remains the authoritative check.

- [ ] **Step 3: Simplify foreground refill**

Change declarations and call sites to:

```cpp
MSRegionInfo* RefillFromCentral(uint8_t cls);
MSRegionInfo* GetCacheRegion(uint8_t cls);
```

The complete `RefillFromCentral()` body becomes:

```cpp
MSRegionInfo* AllocBufferMS::RefillFromCentral(uint8_t cls)
{
    if (UNLIKELY(central_ == nullptr)) {
        return nullptr;
    }
    MSRegionInfo* region = central_->TakePartialSweptRegion(cls);
    if (region != nullptr) {
        return region;
    }
    region = GetCacheRegion(cls);
    if (region != nullptr) {
        return region;
    }
    region = central_->TakeUnsweptRegion(cls);
    if (region != nullptr) {
        return region;
    }
    return central_->GetFreshRegion(cls, cachedHomeShardId_);
}
```

`SlowAlloc()` calls `RefillFromCentral(cls)`. `GetCacheRegion()` retains only classless local/global empty-cache acquisition and size-class binding.

- [ ] **Step 4: Simplify owner cleanup and Region location state**

Initialize `ReturnAllRegionsToCentral()` with:

```cpp
size_t returned = 0;
```

Delete `CACHED_LOCAL` from `MSRegionLocation` and from `LocationHasOwner()`. Keep `LOCAL_EMPTY_CACHE`, which represents the live classless foreground cache.

- [ ] **Step 5: Compile the focused ownership tests**

Run:

```bash
clang++ -std=c++14 -pthread -I. -Isrc \
  test/MSRegionOwnershipStateTest.cpp -o .tmp/MSRegionOwnershipStateTest
.tmp/MSRegionOwnershipStateTest
bash test/MSGCBackgroundFeedRemovalContractTest.sh
```

Expected: the ownership test exits 0. The contract may remain RED only for central/RegionSpace cached aggregation or diagnostics handled in Task 4.

### Task 4: Remove central/RegionSpace aggregation and stale diagnostics

**Files:**
- Modify: `src/Heap/MAllocator/MSCentralRegionPool.h`
- Modify: `src/Heap/MAllocator/MSCentralRegionPool.cpp`
- Modify: `src/Heap/MAllocator/MSRegionSpace.h`
- Modify: `src/Heap/MAllocator/MSRegionSpace.cpp`
- Modify: `src/Heap/MCollector/MarkSweepCollector.cpp`

- [ ] **Step 1: Remove MarkSweep background-fill hooks**

Delete the `RegionSpaceMS::BackgroundFillAllocCaches()` override and `CentralRegionPool::BackgroundFillAllocCaches()` declaration/definition. Do not change `Allocator::BackgroundFillAllocCaches()` or any `FinalizerProcessor` file.

- [ ] **Step 2: Remove cached/hungry aggregation**

Delete these central queries:

```text
GetAllAllocBufferCachedRegionCount
HasHungryAllocBuffer
GetAllAllocBufferRegionCount
```

Keep `GetAllocBufferCount()` and `GetAllAllocBufferActiveRegionCount()`. The latter remains:

```cpp
size_t CentralRegionPool::GetAllAllocBufferActiveRegionCount() const
{
    size_t total = 0;
    std::lock_guard<std::mutex> lg(const_cast<std::mutex&>(msAllocBufferLock_));
    for (AllocBufferMS* buffer : msAllocBuffers_) {
        if (buffer != nullptr) {
            total += buffer->CountActiveRegions();
        }
    }
    return total;
}
```

- [ ] **Step 3: Make the GC snapshot active-only**

Remove `bufferRegionCount` and `bufferCachedRegionCount` from `DumpGCStateSnapshot()`. Replace the split report with:

```cpp
size_t bufferActiveRegionCount = centralPool->GetAllAllocBufferActiveRegionCount();
VLOG(REPORT, "[MS-GC]   alloc-buffer active regions: %zu", bufferActiveRegionCount);
```

Keep buffer count, central empty cache, large Region, and per-class bucket reports.

- [ ] **Step 4: Verify the removal contract is GREEN**

Run:

```bash
bash test/MSGCBackgroundFeedRemovalContractTest.sh
rg -n '\bmsBackgroundFeed\b|cjMSBackgroundFeed|cachedRegion_|cachedRegionOrigin_|BackgroundFeedLoop' \
  src/Cangjie.h src/CangjieRuntimeApi.cpp src/CjScheduler.cpp \
  src/Heap/MAllocator src/Heap/MCollector
```

Expected: the contract exits 0; the symbol scan exits 1 with no output. A separate scan for `reservedMSBackgroundFeed` must find the ABI byte and its zero initializers.

### Task 5: Update documentation and verify the runtime

**Files:**
- Modify: `MSGC_RESULT/Docs/MSGC_总体架构与实现详解.md`
- Modify: `/Users/lipeisheng/.codex/skills/cangjie-runtime-skill/references/GC/Allocator内存分配/Allocator.md`
- Test: `test/MSGCBackgroundFeedRemovalContractTest.sh`
- Test: `test/MSSweepAssistPacingTest.cpp`
- Test: `test/MSGCPLevelSweepDebtContractTest.sh`

- [ ] **Step 1: Update the main architecture document**

Remove all MSGC typed-spare and Feed descriptions:

```text
cachedRegion_[36]
cachedRegionOrigin_[36]
BackgroundFillCache
BackgroundFillAllocCaches
cjMSBackgroundFeed
typed spare
后台 Feed
```

Update the `AllocBufferMS` size formula to describe the removed array footprint as ABI padding, not a live cache. In allocation diagrams and cache tables, retain only `activeRegions_[36]`, `localEmptyRegionCache_`, and central `emptySweptCache_`. Remove the Feed configuration-table row and the “unfinished optimization” risk item.

- [ ] **Step 2: Update the runtime allocator knowledge document**

In the MSGC section of `Allocator.md`, remove typed spare and Feed references. Record these current facts:

```text
MSGC has no background allocation-cache Feed.
Foreground refill uses ready partial, classless local/global empty, lazy Sweep, then fresh Region.
The common CopyGC BackgroundFeedStats/FinalizerProcessor path is intentionally retained.
The removed typed-spare footprint remains ABI padding so activeLog_ stays at offset 896.
```

- [ ] **Step 3: Run focused regression tests and document scans**

Run:

```bash
set -e
clang++ -std=c++14 -pthread -I. -Isrc \
  test/MSSweepAssistPacingTest.cpp -o .tmp/MSSweepAssistPacingTest
.tmp/MSSweepAssistPacingTest
bash test/MSGCPLevelSweepDebtContractTest.sh
bash test/MSGCBackgroundFeedRemovalContractTest.sh
bash test/MSGCDynamicSweepHelperContractTest.sh
git diff --check
```

Expected: every command exits 0. Verify the main document has an even number of lines beginning with three backticks and that its MSGC sections contain none of the removed symbols.

- [ ] **Step 4: Check for stale processes before building**

Run:

```bash
ps -axo pid,ppid,state,etime,command | \
  rg '(^|/)(llc|benchmark|rss_monitor\.py)( |$)|[ /](llc|benchmark|rss_monitor\.py)( |$)' || true
```

Expected: no old `U`/`UE` `llc`, benchmark, or monitor process is mistaken for this build.

- [ ] **Step 5: Build the MarkSweep runtime**

Run:

```bash
python3 build.py build --gc-mode marksweep
```

Expected: exit 0 and a current `output/temp/lib/aarch64_Release/libcangjie-runtime.dylib`. Do not replace the SDK dylib: this task does not require a runtime execution test, so no `.cj` binary is compiled or reused.

- [ ] **Step 6: Final scoped review**

Run:

```bash
rg -n 'NotifyToFeedAllocBuffers|BackgroundFeedStats' \
  src/Heap/Allocator src/Heap/Collector
rg -n 'reservedMSBackgroundFeed' \
  src/Cangjie.h src/CangjieRuntimeApi.cpp src/CjScheduler.cpp
git status --short
```

Expected: CopyGC/common Feed symbols are present, the reserved ABI byte has exactly the intended declarations/zero initializers, and unrelated dirty/untracked files remain untouched.

Implementation commits are intentionally omitted from these tasks because the same production files already contain uncommitted P-level Sweep debt work. Committing whole files would mix independently scoped changes.
