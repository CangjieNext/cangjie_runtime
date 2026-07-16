# MSGC Background Feed Removal Design

## Goal

Remove the MSGC background allocation-cache Feed mechanism end to end, including its configuration, scheduling, typed spare cache, counters, reports, and documentation. Preserve CopyGC Feed behavior and preserve published runtime/compiler ABI layouts.

## Scope

The removal covers only the MarkSweep-specific path:

- `cjMSBackgroundFeed` environment parsing and MSGC configuration semantics.
- `BackgroundProcessor` Feed wake state, periodic decision, loop, pending statistics, and REPORT output.
- `RegionSpaceMS` and `CentralRegionPool` background-fill entry points.
- `AllocBufferMS` per-size-class typed spare Regions and every producer, drain, ownership, and count helper used only by that cache.
- MSGC diagnostics and documentation that mention cached typed spare Regions or Feed.

The following CopyGC facilities remain unchanged:

- `BackgroundFeedStats` and `Allocator::BackgroundFillAllocCaches()` in the common allocator API.
- `FinalizerProcessor` Feed state and `NotifyToFeedAllocBuffers()`.
- CopyGC `AllocBuffer` notification and `FeedHungryBuffers()` behavior.

## Current Behavior and Reason for Removal

`BackgroundProcessor` can periodically call `RegionSpaceMS::BackgroundFillAllocCaches()`, which walks registered `AllocBufferMS` objects and fills `cachedRegion_[36]`. The normal MSGC refill path does not consume this typed spare array: it consumes ready partial Regions, classless local/global empty caches, lazy-swept Regions, and fresh Regions. The Feed path therefore reserves Regions, maintains ownership transitions and statistics, but does not accelerate the foreground allocation path.

Removing the unused path reduces retained metadata and eliminates misleading configuration and diagnostics.

## Design

### Configuration and API ABI

Remove all runtime reads and writes of `msBackgroundFeed`, including the `cjMSBackgroundFeed` environment variable. In public `Cangjie.h`, rename the byte to `reservedMSBackgroundFeed` instead of deleting it physically. The byte is always initialized to zero and has no runtime meaning.

This preserves the offsets of following `HeapParam` fields for existing binaries while removing source-level Feed configuration from the current API.

### Background Processor

Delete the MSGC-only Feed control plane:

- `IsBackgroundFeedEnabled()` and the periodic Feed target constant.
- `ShouldStartPeriodicFeed()`, `HasPendingFeedWork()`, and `BackgroundFeedLoop()`.
- `shouldFeedHungryBuffers` and `pendingFeedStats_`.
- Feed branches in initialization, wake predicates, periodic scheduling, and the main background loop.

Sweep, parallel Sweep, Scavenge, Shrink, and finalizer processing remain unchanged.

### AllocBufferMS Typed Spare

Delete the typed spare data plane:

- `MSAllocCacheFillStats`.
- `cachedRegion_[36]` and `cachedRegionOrigin_[36]`.
- `RegionAcquireOrigin`, `kCachedRegionSlots`, and origin output parameters.
- `BackgroundFillCache()`, `TryCacheRegion()`, `TrimExtraCachedRegions()`, cached count helpers, hungry checks, and cached drain helpers.
- The `CACHED_LOCAL` Region ownership location, which has no remaining user after typed spare removal.

`ReturnAllRegionsToCentral()` will return active Regions directly. The classless `localEmptyRegionCache_` and central `emptySweptCache_` remain because they are consumed by foreground allocation.

`RefillFromCentral()` becomes a foreground-only operation with this unchanged effective order:

1. ready partial Region;
2. local/global classless empty Region;
3. lazy-swept Region;
4. fresh Region from `FreeManager`.

### AllocBufferMS ABI

Removing `cachedRegion_[36]` would move `activeLog_`, whose offset is fixed at 896 bytes by the AArch64 MSGC write-barrier lowering. Replace the removed array footprint with ABI padding by growing `wbBufAbiReserved_` by:

```text
kNumSizeClasses * sizeof(std::atomic<MSRegionInfo*>)
```

Keep the existing `static_assert(offsetof(AllocBufferMS, activeLog_) == 896)`. The removed tail-side origin array is not part of that fixed compiler window and needs no replacement.

### Central Pool and Diagnostics

Delete `CentralRegionPool::BackgroundFillAllocCaches()`, cached-Region aggregation, and hungry-buffer queries. Keep active-Region counting because it is used by GC diagnostics and retained-footprint accounting.

Remove the redundant total/cached split from `DumpGCStateSnapshot()`. The report will describe only active per-P Regions and central caches that still exist.

The `msAllocBuffers_` registry remains for active Region enumeration, local empty cache draining, per-P GC state, and assist statistics.

### RegionSpaceMS

Remove the MarkSweep override of `BackgroundFillAllocCaches()`. The common virtual method remains because CopyGC owns and uses the generic Feed contract.

## Verification

Add a source contract test that requires:

- no MSGC Feed environment parsing, scheduler state, loops, statistics, or typed spare fields;
- no `CACHED_LOCAL` ownership state or Feed-only origin metadata;
- `reservedMSBackgroundFeed` remains as the ABI byte;
- the 896-byte `activeLog_` assertion remains;
- CopyGC `FinalizerProcessor`, `NotifyToFeedAllocBuffers()`, common `BackgroundFeedStats`, and CopyGC allocation notification remain.

Run the relevant standalone MSGC tests, source contracts, `git diff --check`, and a full MarkSweep runtime build. No SDK dylib replacement or `.cj` execution is required unless the build or focused tests reveal a runtime-only uncertainty.

## Documentation

Update `MSGC_RESULT/Docs/MSGC_总体架构与实现详解.md` and the Cangjie Runtime allocator knowledge document. Remove configuration-table entries, typed spare diagrams, counters, lifecycle descriptions, and “unfinished optimization” notes. Explicitly retain and distinguish the classless local/global empty caches.

## Success Criteria

- Searching MSGC sources finds no live background Feed implementation or configuration semantics.
- MSGC no longer reserves or transitions typed spare Regions.
- CopyGC Feed symbols and behavior remain present.
- `AllocBufferMS::activeLog_` remains at offset 896.
- Focused tests and the MarkSweep build pass.
