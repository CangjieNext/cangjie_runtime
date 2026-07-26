# MSGC Active Region Mark Bitmap Clear Design

## Goal

Replace `CentralRegionPool::ClearAllMarkBitmaps()`'s committed-Region scan with
an active-Region traversal whose cost is proportional to Regions that are
currently used by the allocator or tracked as live large objects.

The change must preserve the current Mark/Sweep generation protocol and must
not modify CopyGC behavior.

## Current Problem

`ClearAllMarkBitmaps()` currently loops from zero to
`MSRegionManager::GetMaxRegionCount()`. That getter returns the committed Region
count, so Mark setup grows from about 262,144 metadata entries at 8 GiB to
524,288 entries at 16 GiB even when only a small fraction of those Regions
contain active objects.

The function runs after `StopAndWaitForAllSweep()` and inside the
`PreEnumRoots()` stop-the-world window. Its full committed scan therefore
directly increases Mark setup pause time.

## Selected Approach

Reuse the allocator's existing ownership containers:

1. Central small-Region lists:
   `partialSwept`, `fullSwept`, `partialUnswept`, and `fullUnswept`.
2. Every registered `AllocBufferMS` and its per-size-class active Region.
3. The tracked large-object base list.

Do not add a second active registry, side bitmap, epoch array, or Region
location table. The existing containers already define Region ownership and
are stable while Sweep is quiescent and mutators are stopped.

## Correctness Preconditions

The caller must preserve this order:

1. `StopAndWaitForAllSweep()` drains the previous sweep generation and closes
   all Sweep admission.
2. `ScopedStopTheWorld` stops mutators and freezes `AllocBufferMS` active-Region
   transitions.
3. `ClearAllMarkBitmaps()` visits and clears all markable active Regions.
4. `ReclassifyUnsweptToSwept()` and `PrepareForSweep()` establish the next
   generation snapshot.

No bitmap may be cleared while `SweepRegion()` can still read it.

## Traversal Rules

### Central small Regions

Visit every Region in the four central lists for all size classes.

- Expected types: `MS_REGION_PARTIAL` or `MS_REGION_FULL`.
- Expected sweep generation: current global sweep generation.
- Action: clear the mark bitmap.
- Debug builds verify type and generation invariants.

### Thread-local active Regions

Visit every registered `AllocBufferMS`, then every size-class active Region.

- Expected types: `MS_REGION_BUMP` or `MS_REGION_BITMAP`.
- A Region still stamped with `currentGeneration - 2` is normalized to the
  current generation. This preserves the existing handling for active-local
  Regions that did not enter the previous central Sweep epoch.
- Action: clear the mark bitmap.
- Debug builds verify type and accepted generation values.

Central and thread-local Region ownership is disjoint. The implementation must
not allocate a temporary deduplication set in the STW path.

### Large Regions

Visit only entries in `largeRegionList_`.

- The list contains live large-object base entries.
- Action: clear the base entry's marked flag.
- Secondary `MS_REGION_SPAN` entries are not visited because they carry no
  independent mark state.

### Excluded Regions

Do not visit:

- free/uninitialized Regions;
- global or local empty-cache Regions;
- hot-free or scavenged Regions;
- large secondary span entries.

Their mark state is either absent or initialized when the Region is rebound for
allocation.

## Complexity

The old complexity is:

```text
O(committedRegions)
```

The new complexity is:

```text
O(centralActiveSmallRegions
  + registeredAllocBuffers * sizeClasses
  + liveLargeObjects)
```

The fixed `registeredAllocBuffers * 36` probe cost is intentional and bounded
by active allocator owners rather than committed heap capacity.

## Observability

Record one REPORT log per call containing:

- cleared central small Regions;
- cleared thread-local small Regions;
- cleared large bases;
- total elapsed microseconds.

The existing `PreEnumRoots` `clearBitmaps` and `stw` timings remain the primary
pause metrics.

## Testing

Follow TDD:

1. Add a contract test that initially fails because the current implementation
   calls `GetMaxRegionCount()`.
2. Require `ClearAllMarkBitmaps()` to cover the central-list, `AllocBufferMS`,
   and large-list traversal paths.
3. Require the committed-array loop and direct `GetRegionInfoByIdx` scan to be
   absent from this function.
4. Run the new contract test and existing MSGC Region/Sweep contract tests.
5. Build the Runtime.
6. Replace and sign the current macOS aarch64 MSGC Runtime, recompile the `.cj`
   GC test with that SDK, verify the loaded dylib path, and run the test.

## Files in Scope

- `src/Heap/MAllocator/MSCentralRegionPool.cpp`
- `src/Heap/MAllocator/MSCentralRegionPool.h`
- `test/MSGCActiveRegionBitmapClearContractTest.sh`
- relevant GC reference documentation

No public Runtime ABI or compiler-visible `MSRegionInfo` layout changes are
required.
