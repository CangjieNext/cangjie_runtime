# MSGC Floating-Live Relative Detection Design

## Goal

Remove the absolute `inUseBurst >= maxHeap / 4` gate from MSGC floating-live
inflation detection. A cycle is classified from its relative growth shape,
independent of configured maximum heap size.

## Behavior

`IsMSFloatingLiveInflated()` returns true only when both conditions hold:

1. `markEndInUse >= 4 * markStartInUse`
2. `markBitmapLive > 2 * markStartInUse`

The existing zero-start and overflow guards remain unchanged. When the
classification is true, `EvaluateMSStableLive()` continues to:

- use `max(lastStableLive, markStartInUse)` as `thresholdBase`;
- preserve `lastStableLive` as `nextStableLive`;
- report `floatingLiveInflated = true`.

Normal cycles continue to use the current bitmap live value for both fields.

## Interface

Remove `maxHeapBytes` from the internal signatures of:

- `IsMSFloatingLiveInflated()`
- `EvaluateMSStableLive()`

Update the collector call site and all unit-test calls. These helpers are
header-local Runtime implementation details, not exported ABI.

## Testing

Use test-driven development:

1. Add a case27-shaped regression where in-use grows by at least four times and
   bitmap live exceeds twice the start, but the burst is less than one quarter
   of a large configured heap. Verify the current implementation fails it.
2. After implementation, verify that the regression is classified as inflated.
3. Preserve a negative test where growth is below four times.
4. Run the focused threshold-policy test and a compile check covering the
   collector call site.

## Documentation

Update the existing collector reference and Step55 analysis so neither document
describes the removed `maxHeap / 4` condition.

## Scope and Rollback

This change affects only heap-threshold baseline selection after a GC cycle. It
does not change marking correctness, sweep behavior, object reachability, or
the current cycle's pause time. Rollback is the inverse signature and predicate
change plus removal of the new regression case.
