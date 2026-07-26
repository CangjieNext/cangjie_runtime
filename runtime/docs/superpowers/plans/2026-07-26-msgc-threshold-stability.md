# MSGC Threshold Stability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stabilize MSGC GC threshold decisions across equivalent case27 runs by filtering accepted live samples and smoothing normal threshold transitions.

**Architecture:** Keep median/history and transition arithmetic in the header-only threshold policy. `MarkSweepCollector` owns the eight-sample state, excludes floating/OOM cycles as specified, and exposes each decision through the existing REPORT line. Trigger accounting, GC phases, Mark, Assist, Sweep, and Scavenge remain unchanged.

**Tech Stack:** C++14 runtime, standalone C++ policy test, shell source-contract test, existing MarkSweep collector and build scripts.

---

## File Map

- Modify `test/MSHeapThresholdPolicyTest.cpp`: history median, floating exclusion, bootstrap, deadband, symmetric limits, bounds, OOM, and case27-shape tests.
- Create `test/MSGCThresholdStabilityContractTest.sh`: verify collector-owned state, OOM bypass wiring, and diagnostic fields without depending on log timing.
- Modify `src/Heap/MCollector/MSHeapThresholdPolicy.h`: eight-sample accepted-live ring, median decision, and normal threshold transition policy.
- Modify `src/Heap/MCollector/MarkSweepCollector.h`: own the threshold history state for the collector lifetime.
- Modify `src/Heap/MCollector/MarkSweepCollector.cpp`: feed cycle samples, bypass exceptional cycles, update stats, and report policy decisions.
- Modify `/Users/lipeisheng/.codex/skills/cangjie-runtime-skill/references/GC/回收器与任务/collector.md`: document the implemented stable-live threshold mechanism.

## Task 1: Add Failing Stable-Live History Tests

- [x] **Step 1: Replace single-sample assumptions with history tests**

Add tests using `MSHeapThresholdState` and the stateful `EvaluateMSStableLive` overload. Cover:

```cpp
// Odd median: 100, 300, 200 -> 200.
// Even median: 100, 300 -> overflow-safe 200.
// Partial history: every accepted sample participates before size 8.
// Wrapped history: the ninth accepted sample evicts the first.
// SIZE_MAX middle pair does not overflow.
```

Require the decision to expose `historySize`, `medianStableLive`,
`acceptedSample`, `hadAcceptedHistory`, and `exceptionalBypass`.

- [x] **Step 2: Cover floating and exceptional exclusion**

Use the historical case27 shape:

```cpp
markStart = 1200 * MiB;
markEnd = 15500 * MiB;
bitmapLive = 13 * GiB;
```

Assert that it is classified as floating, does not change the ring, and uses
the existing median. With an empty ring, assert fallback to
`max(lastStableLive, markStart)`. For `exceptionalBypass=true`, assert current
bitmap live is used and the ring is neither read as the baseline nor updated.

- [x] **Step 3: Run RED verification**

```bash
mkdir -p .tmp/threshold-stability
clang++ -std=c++14 -Wall -Wextra -Werror -I. -Isrc \
  test/MSHeapThresholdPolicyTest.cpp \
  -o .tmp/threshold-stability/MSHeapThresholdPolicyTest
```

Expected: compilation fails because the history state and stateful decision
fields do not exist yet.

## Task 2: Implement Stable-Live History

- [x] **Step 1: Add the fixed-size state and overflow-safe median**

Add:

```cpp
constexpr size_t kMSHeapThresholdHistoryLength = 8;

struct MSHeapThresholdState {
    std::array<size_t, kMSHeapThresholdHistoryLength> liveHistory {};
    size_t historySize = 0;
    size_t nextHistoryIndex = 0;
};
```

Copy the available values to a local array, sort only the populated range, and
compute an even median as:

```cpp
lower + (upper - lower) / 2
```

- [x] **Step 2: Make stable-live evaluation stateful**

For a normal accepted cycle, append `markBitmapLiveBytes` and use the current
history median. For floating cycles, preserve the state and use the existing
median; if empty, use `max(lastStableLiveBytes, markStartInUseBytes)`. For an
exceptional cycle, use `markBitmapLiveBytes` directly and preserve state.

- [x] **Step 3: Run GREEN history verification**

```bash
clang++ -std=c++14 -Wall -Wextra -Werror -I. -Isrc \
  test/MSHeapThresholdPolicyTest.cpp \
  -o .tmp/threshold-stability/MSHeapThresholdPolicyTest
.tmp/threshold-stability/MSHeapThresholdPolicyTest
```

Expected: compile and execution both exit `0` for the history group and all
unchanged Scavenge policy tests.

## Task 3: Add Failing Threshold Transition Tests

- [x] **Step 1: Specify inclusive deadband behavior**

Use an old threshold of `400` and raw targets at `360`, `440`, `359`, and
`441`. Both boundary targets keep `400`; values immediately outside the
boundaries begin moving.

- [x] **Step 2: Specify symmetric rate limits and hard bounds**

Assert:

```cpp
// Large rise: 400 -> at most 450.
// Large fall: 400 -> at least 350.
// Bootstrap or exceptional bypass: raw target is used directly.
// Minimum and maximum heap bounds override transition smoothing.
```

Remove the obsolete expectation that a current-live 1.25 safety floor can
override the rise limit.

- [x] **Step 3: Run RED transition verification**

Compile and execute the focused test. Expected: at least the deadband and
decrease-limit assertions fail against the old asymmetric implementation.

## Task 4: Implement Symmetric Threshold Smoothing

- [x] **Step 1: Extend the threshold decision**

Expose `deadbandLower`, `deadbandUpper`, `riseLimit`, `fallLimit`,
`deadbandHeld`, `riseLimited`, `fallLimited`, `transitionLimited`, and
`transitionBypassed`. Keep `exceptionalBypass` on the stable-live decision and
remove the obsolete `liveSafetyFloor` behavior.

- [x] **Step 2: Apply normal-cycle transition controls**

Compute the clamped raw target first. For normal established history:

```text
deadband = inclusive [old * 0.90, old * 1.10]
riseLimit = old * 1.125
fallLimit = old * 0.875
```

Hold the old value inside the deadband. Outside it, clamp rises and falls to
their respective limits. If the old threshold is zero or outside current hard
bounds, establish the clamped raw target directly.

- [x] **Step 3: Preserve bootstrap and OOM behavior**

Add a boolean bypass parameter to `EvaluateMSHeapThreshold`. A first accepted
sample and an OOM cycle pass `true`, use the current clamped raw target, and do
not apply deadband or transition limits.

- [x] **Step 4: Run GREEN transition verification**

```bash
clang++ -std=c++14 -Wall -Wextra -Werror -I. -Isrc \
  test/MSHeapThresholdPolicyTest.cpp \
  -o .tmp/threshold-stability/MSHeapThresholdPolicyTest
.tmp/threshold-stability/MSHeapThresholdPolicyTest
```

Expected: all threshold-history, transition, case27, and existing Scavenge
tests exit `0`.

## Task 5: Integrate Collector State, OOM Bypass, and Diagnostics

- [x] **Step 1: Add a source-contract test**

Create `test/MSGCThresholdStabilityContractTest.sh` requiring:

```text
MarkSweepCollector owns MSHeapThresholdState
UpdateGCStats passes that state to EvaluateMSStableLive
GC_REASON_OOM selects exceptional bypass
the threshold evaluator receives bootstrap/OOM bypass
REPORT contains history, median, deadband, rise/fall limit, transition, bypass
```

Run it before integration and confirm RED.

- [x] **Step 2: Wire the collector-owned state**

Add `MSHeapThresholdState heapThresholdState_` beside the existing Scavenge
state. In `UpdateGCStats()`, compute:

```cpp
bool exceptionalBypass = gcStats.reason == GC_REASON_OOM;
```

Pass it into stable-live evaluation. A normal first accepted sample bypasses
transition smoothing based on `hadAcceptedHistory`; subsequent normal cycles
use the median/deadband/rate limit. OOM cycles do not mutate normal history.

- [x] **Step 3: Extend the existing REPORT record**

Keep meaningful existing fields and add:

```text
historySize medianStableLive
deadbandLower deadbandUpper deadbandHeld
riseLimit fallLimit transitionLimited
exceptionalBypass
```

Update comments that currently describe immediate decreases and the 1.25
current-live safety floor.

- [x] **Step 4: Run integration verification**

```bash
bash test/MSGCThresholdStabilityContractTest.sh
.tmp/threshold-stability/MSHeapThresholdPolicyTest
```

Expected: both commands exit `0`.

## Task 6: Update Runtime Knowledge and Verify

- [x] **Step 1: Update the GC collector reference**

Document the eight accepted-sample median, floating/OOM exclusion, bootstrap,
inclusive 10% deadband, symmetric 12.5% transition limit, and diagnostic
fields. Keep trigger, Mark, Assist, Sweep, and Scavenge descriptions unchanged.

- [x] **Step 2: Check for stale build processes**

Before starting the build, inspect `llc`, `ninja`, `make`, `cjc`, benchmark,
and `rss_monitor.py` processes. Treat old `U`/`UE` processes as historical
residue rather than current progress.

- [ ] **Step 3: Build the MarkSweep runtime**

```bash
python3 build.py build --gc-mode marksweep
```

Expected artifact:

```text
output/temp/lib/aarch64_Release/libcangjie-runtime.dylib
```

Execution result: blocked by the existing AppleClang 17 assembler rejection
of non-private labels inside `.cfi_startproc/.cfi_endproc` in four unchanged
AArch64 macOS stub files. The generated MarkSweep compile command separately
compiled `MarkSweepCollector.cpp.o` successfully.

- [ ] **Step 4: Run focused runtime verification before benchmark**

The verification tuple is:

```text
compiler: /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/msgc
runtime:  this worktree's newly built MarkSweep dylib
stdlib:   /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/msgc
platform: macos-aarch64
```

Use the cangjie-runtime build/copy/verification workflow. Back up the current
SDK dylib, copy and ad-hoc codesign the new dylib, then recompile the `.cj`
test with that SDK before running it. Verify the loaded dylib source in an
isolated `env -i` shell.

- [ ] **Step 5: Run case27 repeated stability validation**

Run at least five MSGC repetitions with isolated SDK environments and fresh
case compilation after runtime replacement. Compare adjacent threshold
changes, GC P95/P99 cross-run CV, throughput, RSS peak, and exact STW P99
against the recorded baseline criteria in the design.

- [x] **Step 6: Review the final diff**

```bash
git diff --check
git diff -- \
  src/Heap/MCollector/MSHeapThresholdPolicy.h \
  src/Heap/MCollector/MarkSweepCollector.h \
  src/Heap/MCollector/MarkSweepCollector.cpp \
  test/MSHeapThresholdPolicyTest.cpp \
  test/MSGCThresholdStabilityContractTest.sh
```

Confirm no unrelated dirty-worktree file is staged or modified by this task.
