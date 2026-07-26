# MSGC Indexed Scavenge Hot-Word Release Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make background Scavenge discovery proportional to hot-free bitmap words actually visited instead of committed Regions, while preserving two-phase OS release safety and current pacing semantics.

**Architecture:** Each owned segment view maintains a mutex-protected 64-bit summary whose bit identifies a bitmap word containing at least one valid hot-free Region. Each shard maintains reverse circular view/word cursors; Scavenge claims directly from indexed words under the shard lock, releases maximal safe spans outside the lock, then completes or rolls back state under the lock. Optional per-slice stats flow through RegionManager and RegionSpace to one background REPORT record.

**Tech Stack:** C++14, Cangjie Runtime MSGC allocator, bitmap summaries, existing shard mutexes, standalone Clang white-box tests, CMake/build.py.

---

## File map

- Modify `src/Heap/MAllocator/MSFreeManager.h`: summary/cursor state, stats API, scan and batch-transition declarations.
- Modify `src/Heap/MAllocator/MSFreeManager.cpp`: summary maintenance, indexed circular scan, span coalescing, two-phase timing/counters.
- Modify `src/Heap/MAllocator/MSRegionManager.h` and `.cpp`: forward optional Scavenge slice stats.
- Modify `src/Heap/MAllocator/MSRegionSpace.h` and `.cpp`: forward optional Scavenge slice stats to background processing.
- Modify `src/Heap/MCollector/BackgroundProcessor.cpp`: emit one REPORT line per attempted Scavenge slice.
- Modify `test/MSFreeManagerShardStatsTest.cpp`: white-box invariant, cursor, sparse-scan, coalescing, failure and state-machine coverage.
- Modify `skills/cangjie-runtime-skill/references/GC/回收器与任务/collector.md`: record the indexed Scavenge discovery mechanism and invariants.

## Task 1: Lock the hot-word summary contract with failing tests

**Files:**
- Modify: `test/MSFreeManagerShardStatsTest.cpp`
- Test: `test/MSFreeManagerShardStatsTest.cpp`

- [ ] **Step 1: Add invariant and bitmap-mask test helpers**

Add helpers that compute the reference hot mask from `allocBits_`, `scavengedBits_`, and the committed/valid tail mask, then compare every word with `OwnedSegmentView::hotWordMask_`:

```cpp
uint64_t ValidMask(size_t wordIdx, size_t validRegions)
{
    size_t begin = wordIdx * FreeManager::kChunkBits;
    if (begin >= validRegions) {
        return 0;
    }
    size_t count = std::min(FreeManager::kChunkBits, validRegions - begin);
    return count == FreeManager::kChunkBits ? ~0ULL : ((1ULL << count) - 1);
}

void ExpectHotWordSummaryMatches(const FreeManager::OwnedSegmentView& view,
                                 size_t committedLocal,
                                 const char* message)
{
    uint64_t expected = 0;
    for (size_t wi = 0; wi < view.bitmapWordCount_; ++wi) {
        uint64_t hot = (~view.allocBits_[wi]) & (~view.scavengedBits_[wi]) &
                       ValidMask(wi, committedLocal);
        if (hot != 0) {
            expected |= 1ULL << wi;
        }
    }
    ExpectEq(view.hotWordMask_, expected, message);
}
```

- [ ] **Step 2: Add tests for initialization, tail validity, and transitions**

Create `TestHotWordSummaryInitializationAndTransitions()` with a 70-Region view. Assert that initialization sets word bits 0 and 1, never exposes invalid tail bits, allocation clears a word when its last hot Region is consumed, reclaim sets it again, successful Scavenge clears it, failed release restores it, and unavailable Regions do not appear hot.

- [ ] **Step 3: Run the focused test and verify RED**

Run:

```bash
test_dir=$(mktemp -d)
clang++ -std=c++14 -pthread -I. -Isrc \
  test/MSFreeManagerShardStatsTest.cpp \
  -o "$test_dir/MSFreeManagerShardStatsTest"
```

Expected: compile failure because `OwnedSegmentView::hotWordMask_` does not exist.

## Task 2: Implement summary initialization and transition maintenance

**Files:**
- Modify: `src/Heap/MAllocator/MSFreeManager.h`
- Modify: `src/Heap/MAllocator/MSFreeManager.cpp`
- Test: `test/MSFreeManagerShardStatsTest.cpp`

- [ ] **Step 1: Add the summary field and bounded-word invariant**

Add to `OwnedSegmentView`:

```cpp
uint64_t hotWordMask_ = 0;
```

After `bitmapWordCount_` is computed in `BuildOwnedSegmentView`, enforce:

```cpp
CHECK_DETAIL(view.bitmapWordCount_ <= kChunkBits,
             "[MS-GC] Scavenge hot-word summary supports at most 64 bitmap words per segment");
```

- [ ] **Step 2: Add exact word recomputation helpers**

Declare and implement:

```cpp
static uint64_t ValidRegionMaskForWord(size_t wordIdx, size_t committedLocal);
static uint64_t HotFreeMaskForWord(const OwnedSegmentView& view,
                                   size_t wordIdx,
                                   size_t committedLocal);
static void RefreshHotWordLocked(OwnedSegmentView& view,
                                 size_t wordIdx,
                                 size_t committedLocal);
static void RefreshHotWordsInRangeLocked(OwnedSegmentView& view,
                                         size_t localStart,
                                         RegionCount num,
                                         size_t committedLocal);
```

`RefreshHotWordLocked` must set the summary bit iff:

```cpp
((~view.allocBits_[wordIdx]) & (~view.scavengedBits_[wordIdx]) &
 ValidRegionMaskForWord(wordIdx, committedLocal)) != 0
```

- [ ] **Step 3: Initialize and maintain the summary at every state transition**

Refresh affected words in `BuildOwnedSegmentView`/attach initialization and after mutations in:

```text
MarkAllocatedOneLocked
MarkAllocatedLocked
MarkFreeLocked
MarkScavengingLocked
CompleteScavengingLocked
MarkUnavailableLocked
```

Use the view’s committed-local bound, not the raw vector capacity, so the final word cannot expose uncommitted Regions.

- [ ] **Step 4: Run the standalone test and verify GREEN**

Run the compile command from Task 1 and then:

```bash
"$test_dir/MSFreeManagerShardStatsTest"
```

Expected: exit 0; all pre-existing tests plus the new summary invariant test pass.

- [ ] **Step 5: Inspect only implementation-owned hunks**

Run:

```bash
git diff --check
git diff -- src/Heap/MAllocator/MSFreeManager.h \
  src/Heap/MAllocator/MSFreeManager.cpp \
  test/MSFreeManagerShardStatsTest.cpp
```

Expected: no whitespace errors; no unrelated existing allocator behavior is removed. Do not commit these overlapping dirty files automatically.

## Task 3: Lock cursor-guided sparse discovery and span coalescing with failing tests

**Files:**
- Modify: `test/MSFreeManagerShardStatsTest.cpp`
- Test: `test/MSFreeManagerShardStatsTest.cpp`

- [ ] **Step 1: Add sparse and cold-view discovery tests**

Add `TestScavengeSkipsColdViewsAndResumesFromCursor()`. Construct at least three views, make the newest two entirely scavenged/cold, leave one hot word in the oldest view, and call a one-Region slice twice around a reclaim. Assert:

```cpp
ExpectEq(stats.skippedColdViews, 2, "two cold views should be skipped by summary");
ExpectEq(stats.visitedHotWords, 1, "only one indexed word should be visited");
ExpectHotWordSummaryMatches(view, committedLocal, "summary after cursor resume");
```

- [ ] **Step 2: Add cross-word and fragmentation tests**

Add `TestScavengeCoalescesOnlyAdjacentRegions()`. Produce a hot run spanning bitmap bits 63 and 64, assert it becomes one `PendingScavengeSpan`; then insert an allocated or scavenged gap and assert two spans remain separate. Also cover address-contiguous adjacent owned views and assert they coalesce only when `previous.idx + previous.num == current.idx`.

- [ ] **Step 3: Add grow/detach cursor clamp tests**

Directly position `scavengeViewCursor_` and `scavengeWordCursor_` at the last view/word, detach a trailing completely free segment through the existing shrink/release path, and assert both cursors remain in range (or reset to zero for an empty view list).

- [ ] **Step 4: Run and verify RED**

Run the standalone Clang command from Task 1.

Expected: compile failure for missing `ScavengeSliceStats`, `scavengeViewCursor_`, or `scavengeWordCursor_`.

## Task 4: Implement indexed circular claim and batched completion

**Files:**
- Modify: `src/Heap/MAllocator/MSFreeManager.h`
- Modify: `src/Heap/MAllocator/MSFreeManager.cpp`
- Test: `test/MSFreeManagerShardStatsTest.cpp`

- [ ] **Step 1: Add per-shard cursor state**

Add:

```cpp
size_t scavengeViewCursor_ = 0;
size_t scavengeWordCursor_ = 0;
```

Initialize both to the reverse-scan start. On attach preserve a valid existing position; on detach clamp `scavengeViewCursor_` modulo the new view count and clamp/reset the word cursor against the selected view.

- [ ] **Step 2: Replace full reverse scans with summary-guided circular scans**

Refactor `ClaimScavengeSpansLocked` to:

```text
start at the saved view/word cursor
visit each owned view at most once per slice
skip a view immediately when hotWordMask_ == 0
mask summary bits according to the current reverse word position
use clz to select the next set summary bit
derive hot Region bits only for that word
claim no more than the remaining Region budget
save the next reverse position, wrapping circularly
```

The scan must terminate after a bounded number of views/summary words even if state changes are impossible under the held shard mutex.

- [ ] **Step 3: Claim a selected word mask directly**

Add a helper with this contract:

```cpp
size_t ClaimScavengeWordMaskLocked(FreeManagerShard& shard,
                                   OwnedSegmentView& view,
                                   size_t wordIdx,
                                   uint64_t claimMask,
                                   std::vector<PendingScavengeSpan>& spans);
```

Set `allocBits_` and `scavengingBits_` for `claimMask` in word operations, update shard counts by `__builtin_popcountll(claimMask)`, refresh the word summary once, and append ascending physical Region runs. Merge only exact address adjacency:

```cpp
if (!spans.empty() &&
    spans.back().idx + spans.back().num == run.idx) {
    spans.back().num += run.num;
} else {
    spans.push_back(run);
}
```

- [ ] **Step 4: Batch completion by affected words**

Refactor completion to apply masks per word, preserving:

```text
success: alloc=0, scavenging=0, scavenged=1, summary remains cold
failure: alloc=0, scavenging=0, scavenged=0, summary becomes hot
```

Update chunk summaries and `maxFreeRun_` once per affected view/range batch, then refresh shard snapshots once per completion batch.

- [ ] **Step 5: Keep OS release outside the shard lock**

Retain the two-phase structure:

```cpp
{ try_lock; claim; unlock; }
for (PendingScavengeSpan& span : spans) {
    span.released = ReleaseUnits(...);
}
{ lock; complete all spans; unlock; }
```

Do not change Linux `MADV_DONTNEED`, macOS zero-plus-`MAP_FIXED`, threshold, reserve, or eight-round pacing semantics.

- [ ] **Step 6: Run focused tests and verify GREEN**

Run the standalone Clang compile and executable.

Expected: exit 0; lock-release, failure rollback, cursor, sparse-scan, coalescing, shrink and existing Scavenge tests all pass.

## Task 5: Add optional per-slice statistics and background REPORT plumbing

**Files:**
- Modify: `src/Heap/MAllocator/MSFreeManager.h`
- Modify: `src/Heap/MAllocator/MSFreeManager.cpp`
- Modify: `src/Heap/MAllocator/MSRegionManager.h`
- Modify: `src/Heap/MAllocator/MSRegionManager.cpp`
- Modify: `src/Heap/MAllocator/MSRegionSpace.h`
- Modify: `src/Heap/MAllocator/MSRegionSpace.cpp`
- Modify: `src/Heap/MCollector/BackgroundProcessor.cpp`
- Test: `test/MSFreeManagerShardStatsTest.cpp`

- [ ] **Step 1: Add a zero-initialized optional stats type**

Add to `FreeManager` public API:

```cpp
struct ScavengeSliceStats {
    size_t requestedBytes = 0;
    size_t claimedRegions = 0;
    size_t releasedRegions = 0;
    size_t visitedShards = 0;
    size_t lockMissShards = 0;
    size_t visitedViews = 0;
    size_t skippedColdViews = 0;
    size_t visitedHotWords = 0;
    size_t cursorWraps = 0;
    size_t candidateRuns = 0;
    size_t releaseCalls = 0;
    size_t releaseFailures = 0;
    uint64_t scanClaimUs = 0;
    uint64_t releaseUs = 0;
    uint64_t completeUs = 0;
};

size_t ScavengeFreeRegionsUpTo(size_t maxBytes,
                              ScavengeSliceStats* stats = nullptr);
```

- [ ] **Step 2: Populate stats without changing the null fast path**

Only take clocks and increment diagnostic counters inside `if (stats != nullptr)`. Count claimed Regions at claim time, released Regions only after successful OS release, one `releaseCalls` per final coalesced span, and one `releaseFailures` per failed span.

- [ ] **Step 3: Forward the optional pointer through RegionManager and RegionSpace**

Use the same type alias and default:

```cpp
using ScavengeSliceStats = FreeManager::ScavengeSliceStats;
size_t ScavengeFreeRegionsUpTo(size_t maxBytes,
                              ScavengeSliceStats* stats = nullptr);
size_t BackgroundScavenge(size_t quantumBytes,
                          FreeManager::ScavengeSliceStats* stats = nullptr);
```

Implement both wrappers as direct forwarding calls.

- [ ] **Step 4: Emit one REPORT record per attempted background slice**

In `BackgroundScavengeLoop`, create stats, call `space.BackgroundScavenge(quantum, &stats)`, and emit a single `VLOG(REPORT, ...)` record including requested/released bytes, discovery counters, release counters, and the three durations. Do not emit per-view or per-span logs.

- [ ] **Step 5: Add exact counter assertions**

Extend sparse, try-lock, failure, and cross-word tests to assert `visitedShards`, `lockMissShards`, `visitedViews`, `skippedColdViews`, `visitedHotWords`, `claimedRegions`, `releasedRegions`, `candidateRuns`, `releaseCalls`, and `releaseFailures`.

- [ ] **Step 6: Compile focused test and affected runtime translation units**

Run the standalone test, then:

```bash
python3 build.py build --gc-mode marksweep
```

Expected: focused test exits 0 and the MarkSweep runtime build exits 0 with `-Werror`.

## Task 6: Add deterministic state-machine regression coverage

**Files:**
- Modify: `test/MSFreeManagerShardStatsTest.cpp`

- [ ] **Step 1: Add a fixed-seed reference model**

Use a local xorshift generator with a fixed seed and a vector of reference states:

```cpp
enum class RefState : uint8_t { HOT_FREE, COLD_FREE, ALLOCATED, UNAVAILABLE };
uint64_t seed = 0x9e3779b97f4a7c15ULL;
auto next = [&seed]() {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
};
```

Drive valid public or existing white-box transitions: allocate, reclaim, successful Scavenge, forced failed release, grow, and detach/shrink.

- [ ] **Step 2: Check invariants after every operation**

After each of at least 500 deterministic steps, compare:

```text
per-Region alloc/scavenged/scavenging state
per-view hotWordMask against the reference formula
free/scavenged/scavenging counts
hot-free aggregate snapshot
cursor bounds
no in-flight Region is allocatable
```

- [ ] **Step 3: Run the test repeatedly**

Run:

```bash
for run in 1 2 3; do
  "$test_dir/MSFreeManagerShardStatsTest"
done
```

Expected: all three runs exit 0 with identical behavior.

## Task 7: Build, run focused verification, and update runtime knowledge

**Files:**
- Modify: `skills/cangjie-runtime-skill/references/GC/回收器与任务/collector.md`
- Verify: all implementation and test files above

- [ ] **Step 1: Update the GC knowledge document**

Document:

```text
hotWordMask invariant and valid-tail masking
per-shard reverse circular cursors
summary-guided bounded scan complexity
claim/release/complete state transitions
failure rollback
stats meanings and REPORT granularity
Linux/macOS release mechanisms remain unchanged
```

- [ ] **Step 2: Run static and focused validation**

Run:

```bash
git diff --check
rg -n "hotWordMask_|scavengeViewCursor_|ScavengeSliceStats" \
  src/Heap test/MSFreeManagerShardStatsTest.cpp
```

Expected: no diff errors and every declaration has a matching implementation/use.

- [ ] **Step 3: Build the MarkSweep runtime**

Before starting, identify old residual build/benchmark processes and do not count them as this run. Then run:

```bash
python3 build.py build --gc-mode marksweep
```

Expected: exit 0 and `output/temp/lib/aarch64_Release/libcangjie-runtime.dylib` is refreshed.

- [ ] **Step 4: Perform macOS functional SDK verification**

Use compiler, runtime, and stdlib from `/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/msgc`; replace only the SDK runtime with the freshly built dylib, create a timestamped backup first, and re-sign it. In a clean `env -i` shell, verify the loaded dylib path, recompile `gc/gc.cj` after replacement, then run it. Never reuse an old binary.

- [ ] **Step 5: Record performance acceptance commands for Linux AArch64**

Run the approved high-P sparse and case27 workloads with P128, collecting the per-slice REPORT fields. Accept only if:

```text
visited bitmap words: at least 90% lower in sparse microbenchmark
scanClaimUs/released GiB: at least 50% lower
releaseUs/GiB: no more than 5% regression
throughput: no more than 3% regression
p99: no more than 5% regression
RSS: no more than 3% regression
```

- [ ] **Step 6: Final review without unsafe source commits**

Run `git status --short` and inspect exact diffs. Because allocator and background files were already dirty before this work, do not create a source commit that would absorb unrelated user changes. Report the independently committed design/plan hashes and leave implementation hunks visible for user review unless a safe ownership boundary is established.
