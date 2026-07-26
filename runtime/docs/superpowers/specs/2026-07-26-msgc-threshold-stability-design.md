# MSGC Threshold Stability Design

## Goal

Reduce cross-cycle and cross-run MSGC GC wall-time variance by preventing one
transient bitmap-live sample from immediately reshaping the heuristic GC
threshold.

The first stage changes only threshold estimation. It does not change the HEU
trigger axis, Mark helper policy, Mark Assist, Sweep, Scavenge, or GC phases.

## Stable-live history

`MarkSweepCollector` owns an `MSHeapThresholdState` containing the most recent
eight accepted bitmap-live samples.

- A normal completed cycle appends its `trueLiveBytes`.
- A cycle classified as `floatingLiveInflated` does not update the history.
- Before the history has eight entries, all available accepted samples are
  used.
- The stable live value is the median. For an even number of samples, it is the
  overflow-safe average of the two middle values.
- Before any accepted history exists, threshold calculation retains the current
  single-sample behavior.

The existing floating-live predicate remains unchanged. Its decision is made
before updating the history.

## Threshold calculation

For normal cycles with accepted history:

```text
rawTarget = clamp(stableLive * (1 + cjHeapGrowth), minThreshold, maxThreshold)
```

The transition from the old threshold to `rawTarget` uses two controls:

1. A 10% deadband. If `rawTarget` is within the inclusive range
   `[oldThreshold * 0.90, oldThreshold * 1.10]`, keep `oldThreshold`.
2. Outside the deadband, limit both increases and decreases to 12.5% per
   completed GC cycle.

The minimum threshold and allocator maximum capacity remain hard bounds.
The old raw-live safety floor no longer overrides the normal-cycle rate limit;
the trusted `stableLive` is the safety baseline for this stage.

## Exceptional cycles

- The first cycle without accepted history uses the existing raw-sample
  bootstrap behavior.
- OOM cycles neither read nor update the normal-cycle history. They bypass
  median/deadband/rate limiting and use the current live sample, still clamped
  to the minimum and maximum threshold bounds.
- A floating-live cycle preserves the existing history and calculates from the
  existing stable history. If no accepted history exists, it falls back to
  `max(lastStableLive, markStartInUse)`.

## Diagnostics

The existing `heapThreshold` REPORT line is extended with:

- history size;
- median stable live;
- deadband bounds and whether the deadband held the old value;
- rise and fall limits;
- whether either transition limit applied;
- whether exceptional-cycle bypass was used.

Existing fields remain available where their meaning is unchanged.

## Tests

Focused policy tests cover:

1. median calculation for odd, even, partial, and wrapped histories;
2. floating-live samples being excluded;
3. 10% deadband boundaries;
4. symmetric 12.5% increase and decrease limits;
5. minimum and maximum bounds;
6. first-cycle bootstrap;
7. OOM bypass;
8. the historical case27 floating-live shape.

After focused tests pass, build the MarkSweep runtime and run case27 at least
five times in randomized order using the same compiler/runtime/stdlib.

Initial acceptance criteria:

- mean adjacent threshold change at or below 10%;
- GC cycle P95 cross-run coefficient of variation at or below 10%;
- GC cycle P99 cross-run coefficient of variation at or below 15%;
- throughput regression no worse than 3%;
- peak RSS increase no worse than 15%;
- exact STW P99 does not regress.

If threshold variance improves but GC wall-time variance does not, do not add
more threshold heuristics. Proceed to the separate heap-goal/trigger runway
pacer design.
