# MSGC Active Region Mark Bitmap Clear Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make MSGC Mark setup clear mark state only for central, thread-local, and large Regions that are actually in use.

**Architecture:** Reuse the existing central size-class lists, registered `AllocBufferMS` instances, and dense large-base list while Sweep is quiescent and mutators are stopped. Keep the current sweep-generation normalization for active-local Regions and remove the committed metadata-array scan.

**Tech Stack:** C++14, Bash/Python contract tests, Cangjie Runtime MSGC, CMake/build.py, macOS aarch64 MSGC SDK.

---

## File Map

- Create `test/MSGCActiveRegionBitmapClearContractTest.sh`: structural regression test for the active-only traversal.
- Modify `src/Heap/MAllocator/MSCentralRegionPool.cpp`: replace the committed scan with central/TLS/large traversal and add REPORT counts.
- Modify `src/Heap/MAllocator/MSCentralRegionPool.h`: correct the traversal comments to describe central small Regions.
- Modify `/Users/lipeisheng/.codex/skills/cangjie-runtime-skill/references/GC/回收器与任务/collector.md`: record the new Mark setup boundary.

The target source files already contain user changes. Do not commit source-file modifications because a normal source commit would also capture those pre-existing changes.

### Task 1: Add the failing traversal contract

**Files:**
- Create: `test/MSGCActiveRegionBitmapClearContractTest.sh`
- Inspect: `src/Heap/MAllocator/MSCentralRegionPool.cpp`

- [ ] **Step 1: Write the failing contract test**

Create a script that extracts only `ClearAllMarkBitmaps()` and enforces:

```python
clear = section(
    source,
    'void CentralRegionPool::ClearAllMarkBitmaps()',
    'void CentralRegionPool::ReclassifyUnsweptToSwept()',
)

for forbidden in (
    'GetMaxRegionCount()',
    'GetRegionInfoByIdx',
    'maxRegions',
):
    if forbidden in clear:
        raise SystemExit(
            f'ClearAllMarkBitmaps must not scan committed metadata: {forbidden}')

for required in (
    'VisitAllRegions',
    'VisitAllocBufferMS',
    'VisitLargeRegionsLocked',
    'ClearMarkBitmap',
    'ClearMarkedFlag',
):
    if required not in clear:
        raise SystemExit(
            f'ClearAllMarkBitmaps is missing active traversal step: {required}')
```

Also require the function to retain `SetSweepGen`, to log central/TLS/large
counts, and to avoid a temporary `unordered_set`.

- [ ] **Step 2: Run the contract and verify RED**

Run:

```bash
bash test/MSGCActiveRegionBitmapClearContractTest.sh
```

Expected: non-zero exit with
`ClearAllMarkBitmaps must not scan committed metadata: GetMaxRegionCount()`.

### Task 2: Implement active-only bitmap clearing

**Files:**
- Modify: `src/Heap/MAllocator/MSCentralRegionPool.cpp:1530`
- Modify: `src/Heap/MAllocator/MSCentralRegionPool.h:378`

- [ ] **Step 1: Clarify the existing visitor boundary**

Update the `VisitAllRegions` comment so it explicitly says it visits every
small Region currently owned by the central size-class lists. Do not change the
method signature or list implementation.

- [ ] **Step 2: Replace the committed scan**

Implement this structure:

```cpp
void CentralRegionPool::ClearAllMarkBitmaps()
{
    uint64_t clearStartUs = TimeUtil::MicroSeconds();
    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
    size_t centralSmallCleared = 0;
    size_t tlSmallCleared = 0;
    size_t largeBaseCleared = 0;

    VisitAllRegions([sg, &centralSmallCleared](MSRegionInfo* region) {
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
        CHECK_DETAIL(region != nullptr,
                     "[MS-GC] null central Region during mark bitmap clear");
        CHECK_DETAIL(
            region->regionType == MSRegionType::MS_REGION_PARTIAL ||
            region->regionType == MSRegionType::MS_REGION_FULL,
            "[MS-GC] unexpected central Region type during mark bitmap clear: "
            "idx=%zu type=%u",
            MSRegionInfoArray::GetRegionIdx(region),
            static_cast<unsigned>(region->regionType));
        CHECK_DETAIL(
            region->GetSweepGen() == sg,
            "[MS-GC] stale central Region generation during mark bitmap clear: "
            "idx=%zu sweepGen=%u expected=%u",
            MSRegionInfoArray::GetRegionIdx(region),
            static_cast<unsigned>(region->GetSweepGen()),
            static_cast<unsigned>(sg));
#endif
        region->ClearMarkBitmap();
        ++centralSmallCleared;
    });

    VisitAllocBufferMS([sg, &tlSmallCleared](AllocBufferMS& buffer) {
        for (uint8_t cls = 0; cls < kNumSizeClasses; ++cls) {
            MSRegionInfo* region = buffer.GetActiveRegion(cls);
            if (region == nullptr) {
                continue;
            }
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
            CHECK_DETAIL(
                region->regionType == MSRegionType::MS_REGION_BUMP ||
                region->regionType == MSRegionType::MS_REGION_BITMAP,
                "[MS-GC] unexpected TLS Region type during mark bitmap clear: "
                "idx=%zu cls=%u type=%u",
                MSRegionInfoArray::GetRegionIdx(region),
                static_cast<unsigned>(cls),
                static_cast<unsigned>(region->regionType));
#endif
            if (region->GetSweepGen() == sg - 2) {
                region->SetSweepGen(sg);
            }
#if defined(MRT_DEBUG) && (MRT_DEBUG == 1)
            CHECK_DETAIL(
                region->GetSweepGen() == sg,
                "[MS-GC] stale TLS Region generation during mark bitmap clear: "
                "idx=%zu cls=%u sweepGen=%u expected=%u",
                MSRegionInfoArray::GetRegionIdx(region),
                static_cast<unsigned>(cls),
                static_cast<unsigned>(region->GetSweepGen()),
                static_cast<unsigned>(sg));
#endif
            region->ClearMarkBitmap();
            ++tlSmallCleared;
        }
    });

    VisitLargeRegionsLocked([&largeBaseCleared](MSRegionInfo* region) {
        region->ClearMarkedFlag();
        ++largeBaseCleared;
    });

    VLOG(REPORT,
         "[MS-GC] ClearAllMarkBitmaps: centralSmall=%zu tlSmall=%zu "
         "largeBase=%zu elapsed=%sus",
         centralSmallCleared, tlSmallCleared, largeBaseCleared,
         Pretty(TimeUtil::MicroSeconds() - clearStartUs).Str());
}
```

Use complete diagnostic messages in the implementation, including Region
index, type, actual generation, and expected generation. Do not allocate a
deduplication container in the STW path.

- [ ] **Step 3: Run the contract and verify GREEN**

Run:

```bash
bash test/MSGCActiveRegionBitmapClearContractTest.sh
```

Expected: exit 0 and
`MSGC active Region bitmap clear contract holds`.

### Task 3: Run focused regressions

**Files:**
- Test: `test/MSGCActiveRegionBitmapClearContractTest.sh`
- Test: `test/MSGCSweepRegionBitmapContractTest.sh`
- Test: `test/MSGCIncrementalRegionSnapshotContractTest.sh`
- Test: `test/MSGCLargeRegionSemanticsContractTest.sh`
- Test: `test/MSGCSweepAdmissionContractTest.sh`

- [ ] **Step 1: Run independent focused tests**

Run each script and record every exit code:

```bash
bash test/MSGCActiveRegionBitmapClearContractTest.sh
bash test/MSGCSweepRegionBitmapContractTest.sh
bash test/MSGCIncrementalRegionSnapshotContractTest.sh
bash test/MSGCLargeRegionSemanticsContractTest.sh
bash test/MSGCSweepAdmissionContractTest.sh
```

Expected: all exit 0. If an unrelated dirty-worktree failure occurs, report it
separately instead of modifying unrelated code.

- [ ] **Step 2: Check the focused diff**

Run:

```bash
git diff --check -- \
  src/Heap/MAllocator/MSCentralRegionPool.cpp \
  src/Heap/MAllocator/MSCentralRegionPool.h \
  test/MSGCActiveRegionBitmapClearContractTest.sh
```

Expected: exit 0 with no whitespace errors.

### Task 4: Build and run end-to-end verification

**Files:**
- Build: Runtime source tree
- Verify: `gc/gc.cj`

- [ ] **Step 1: Check for stale processes**

Inspect `llc`, benchmark, `rss_monitor.py`, and prior GC test processes. Mark
old `U`/`UE` processes as historical; do not treat them as this build.

- [ ] **Step 2: Build the Runtime**

Run the repository build command:

```bash
python3 build.py build
```

Expected: exit 0 and
`output/temp/lib/aarch64_Release/libcangjie-runtime.dylib`.

- [ ] **Step 3: Install the Runtime safely**

Use:

```text
runtime root = /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime
SDK root     = /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/msgc
platform     = macos-aarch64
```

Back up the SDK dylib, copy the built dylib, and run `codesign -f -s -`.
Compiler, runtime, and stdlib/stdx all come from the current `output/msgc` SDK;
only the newly built runtime dylib replaces the SDK runtime.

- [ ] **Step 4: Recompile and run the GC test**

In an isolated `env -i` shell, source the SDK, compile `gc/gc.cj` again, verify
the loaded runtime with `DYLD_PRINT_LIBRARIES=1`, then run the test with a
bounded heap and `MRT_REPORT`.

Expected: compilation and run exit 0, the runtime path points into
`cangjie_compiler/output/msgc`, and the report contains active-only
`ClearAllMarkBitmaps` counters.

### Task 5: Update GC knowledge and report evidence

**Files:**
- Modify: `/Users/lipeisheng/.codex/skills/cangjie-runtime-skill/references/GC/回收器与任务/collector.md`

- [ ] **Step 1: Correct the Mark bitmap clearing description**

Replace the statement that Mark setup scans all committed metadata with the
new central/TLS/large traversal, its correctness preconditions, and its
complexity.

- [ ] **Step 2: Verify documentation and summarize**

Run placeholder and consistency scans, then report:

- old and new traversal complexity;
- files changed;
- RED/GREEN evidence;
- focused test results;
- build and end-to-end results;
- any remaining performance measurement not covered by correctness testing.
