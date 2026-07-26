# MSGC Runway-Based Sweep Pressure Design

## 1. Goal

Make natural Sweep pressure enter `HIGH` early enough to drain a large unswept
backlog before the next HEU GC, especially under high-P allocation pressure.

The change replaces the current late `U > T` natural-HIGH boundary with a
remaining-runway model while preserving the existing bounded execution actions:

- `HIGH` uses all available Sweep Helpers;
- `HIGH` keeps the existing `10x` P-debt and slice-budget multiplier;
- the base background budget remains `cjGCBGSweepMaxTime`;
- one direct P Assist remains capped by `cjMSAssistSweepMaxRegionsPerCall`;
- forced `HIGH` remains latched until the Sweep epoch completes.

## 2. Non-Goals

This design does not:

- add an independent Sweep thread pool;
- change the maximum Helper formula of `gcThreads - 1`;
- increase the existing `HIGH` slice budget;
- add allocation-rate or Sweep-throughput EWMA sampling;
- defer an HEU GC to wait for Sweep;
- change Sweep admission, quiescence, or PreEnum closure rules;
- introduce a new environment variable.

## 3. Current Problem

The current pressure model uses:

- `U`: central unswept small-Region bytes;
- `S`: swept-retained bytes;
- `T`: current heap threshold.

Its natural classification is:

```text
HIGH:   U > T
MEDIUM: U <= T and S + U > T
LOW:    otherwise
```

On a large heap, `T` can be several GiB. A multi-GiB backlog can therefore
remain `MEDIUM`, which usually selects only one Helper because the MEDIUM target
is normalized by `T`. The next GC may start before Helper ramp-up or background
Sweep drains the backlog.

In the Step54 P128 run, cycle 11 observed approximately:

```text
U = 5.983 GiB
S = 4.679 GiB
T = 7.389 GiB
```

The current model classified this as `MEDIUM` because `U < T`. It requested two
Helpers but activated only one before GC interrupted the slice, leaving 182,342
unswept Regions for PreEnum force-finish.

## 4. Pressure Model

Define the remaining allocation runway:

```text
R = max(T - S, 0)
```

`R` represents how much swept-retained footprint can still accumulate before
reaching the heap threshold. Pressure is based on whether the outstanding Sweep
backlog can fit within that runway.

### 4.1 Upgrade boundaries

Natural pressure upgrades immediately:

```text
if U == 0:
    LOW
else if forcedHigh:
    HIGH
else if T == 0 or R == 0:
    HIGH
else if U >= R:
    HIGH
else if U >= ceil(R / 2):
    MEDIUM
else:
    LOW
```

The explicit `T == 0` case avoids treating a non-empty backlog as low pressure
when there is no usable threshold. The existing `U > T` HIGH condition is
logically covered because `R <= T`, but may remain as a defensive/commented
condition if that improves source readability.

All ratio comparisons must avoid multiplication and addition overflow. Use
saturating subtraction and division-based boundaries such as
`U >= R / 2 + R % 2`.

### 4.2 Downgrade hysteresis

Natural upgrades are immediate, while downgrades use lower boundaries:

```text
previous HIGH:
    remain HIGH while U >= ceil(R / 2)

previous MEDIUM:
    remain at least MEDIUM while U >= ceil(R / 4)
```

The resulting transition behavior is:

```text
LOW    -> MEDIUM: U >= R/2
LOW    -> HIGH:   U >= R
MEDIUM -> HIGH:   U >= R
HIGH   -> MEDIUM: U < R/2
MEDIUM -> LOW:    U < R/4
```

If a previous `HIGH` sample falls below the HIGH hold boundary but remains above
the MEDIUM hold boundary, it becomes `MEDIUM`. Otherwise it becomes `LOW`.

Forced `HIGH` takes precedence over natural hysteresis and retains its existing
epoch latch. `U == 0` or completed Sweep always resolves to `LOW` and clears the
forced latch through the existing completion path.

## 5. Execution Actions

The new model changes classification only. Existing actions remain authoritative:

| Mode | Slice budget | Helper target | P-debt multiplier |
| --- | ---: | ---: | ---: |
| LOW | `1x` | `0` | `1x` |
| MEDIUM | `4x` | existing MEDIUM formula | `4x` |
| HIGH | `10x` | all available Helpers | `10x` |

The active Helper count continues to jump directly to the target for natural or
forced `HIGH`. MEDIUM and LOW continue to move by at most one Helper per
coordinator advance.

Promoting more samples to `HIGH` therefore activates all available Helpers
immediately without changing the worker-pool ownership protocol. The existing
shared deadline continues to bound how long one coordinated Sweep round can hold
the pool before Mark can acquire it.

## 6. Component Changes

### `MSSweepDebtPolicy.h`

- Replace the stateless `U/S/T` classification helper with a runway-aware helper.
- Accept the previous natural pressure mode for downgrade hysteresis.
- Keep the computation pure and header-only for unit testing.
- Add overflow-safe helpers for `ceil(R / 2)` and `ceil(R / 4)`.

### `MSCentralRegionPool.cpp`

- In `RefreshSweepDebtSnapshot()`, load the published previous mode.
- Pass it to the runway-aware pressure evaluator.
- Preserve the existing `sweepDone`, empty-backlog, and forced-latch behavior.

### `BackgroundProcessor.cpp`

- No scheduling-policy change is required.
- Continue refreshing pressure at slice boundaries.
- Keep logging `mode`, `forced`, `U`, `S`, `T`, target/active Helpers, and budget.
- Add `R` to the REPORT record so pressure decisions can be replayed directly.

## 7. Edge Cases

- `U == 0`: always `LOW`, regardless of stale previous mode.
- `S >= T` and `U > 0`: `R == 0`, therefore immediate `HIGH`.
- `T == 0` and `U > 0`: immediate `HIGH`.
- Sweep completion: reset Helper control and pressure as today.
- New Sweep generation: start from `LOW`; the first refresh performs an immediate
  upgrade from the new epoch's `U/S/T`.
- Parallel Sweep disabled: pressure still scales coordinator budget and P debt,
  while Helper target remains zero.
- Background Sweep disabled: the classification remains valid for P Assist and
  diagnostic logging, without opening a new execution path.
- Forced HIGH racing with completion: retain the existing post-store completion
  check and latch clearing.

## 8. Step54 Replay

For cycle 11:

```text
R = T - S
  = 7.389 - 4.679
  = 2.710 GiB

U = 5.983 GiB >= R
```

The new model produces:

```text
mode=HIGH
targetHelpers=maxHelpers
activeHelpers=maxHelpers
budget=20ms with the current 2ms base
```

For the earlier representative sample:

```text
U = 1.695 GiB
S = 4.244 GiB
T = 4.511 GiB
R = 0.267 GiB
```

`U >= R` also produces immediate `HIGH`, instead of the previous one-Helper
MEDIUM response.

## 9. Verification Design

Pure-policy tests must cover:

- all LOW/MEDIUM/HIGH upgrade boundaries;
- exact equality at `R/2` and `R`;
- HIGH retention and downgrade at the hysteresis boundary;
- MEDIUM retention and downgrade at the hysteresis boundary;
- `U == 0`, `T == 0`, and `S >= T`;
- forced HIGH precedence;
- values near `SIZE_MAX` without overflow;
- Step54 cycle-11 and representative-slice replay values.

Source-contract or component tests must confirm:

- `RefreshSweepDebtSnapshot()` passes the previous mode;
- the first sample of a new generation can upgrade immediately;
- HIGH still activates `maxHelpers`;
- `R` is present in the parallel-Sweep REPORT line;
- no independent pool or new environment parameter is introduced.

Runtime performance verification, when separately authorized, should compare:

- HIGH/MEDIUM/LOW slice counts;
- target and active Helper distributions;
- unswept backlog at the next PreEnum;
- `StopAndWaitForAllSweep` duration;
- OOM emergency Sweep frequency and duration;
- application throughput, CPU, and RSS;
- Mark worker-pool acquisition delay.

## 10. Acceptance Criteria

The policy implementation is acceptable when:

1. Both cited Step54 snapshots classify as natural `HIGH`.
2. Natural `HIGH` immediately selects all available Sweep Helpers.
3. A natural HIGH sample does not downgrade until the hysteresis exit condition.
4. Existing forced-HIGH and Sweep-completion semantics remain unchanged.
5. One coordinated HIGH round remains bounded by the existing 20 ms default
   budget rather than introducing a longer pool hold.
6. The implementation adds no hot-path counter or global allocation atomic.
