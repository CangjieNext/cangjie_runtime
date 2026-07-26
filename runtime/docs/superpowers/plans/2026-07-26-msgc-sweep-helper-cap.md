# MSGC Sweep Helper Cap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove the default fixed limit of eight MSGC Sweep pool helpers while preserving the explicit helper cap.

**Architecture:** Keep `gcThreads` as the single source of available GC pool width. `ComputeMSMaxSweepHelpers()` derives the default maximum as `gcThreads - 1`; a nonzero `cjMSParallelSweepMaxHelpers` remains an optional lower cap.

**Tech Stack:** C++14, header-only policy functions, C++ policy contract tests.

---

### Task 1: Update the helper-cap contract

**Files:**
- Modify: `test/MSParallelSweepPolicyTest.cpp`

- [ ] **Step 1: Change the default-cap expectations**

Replace the fixed-eight expectation and add a larger-width case:

```cpp
ExpectEq(ComputeMSMaxSweepHelpers(16, 0), 15, "automatic sweep follows GC pool width");
ExpectEq(ComputeMSMaxSweepHelpers(128, 0), 127, "automatic sweep has no fixed helper cap");
```

Keep the existing one-thread, low-width, explicit-cap, and GC-thread-cap cases unchanged.

- [ ] **Step 2: Do not execute the test**

Per user direction, update the contract source only. Do not compile or run the test.

### Task 2: Remove the fixed default cap

**Files:**
- Modify: `src/Heap/MAllocator/MSParallelSweepPolicy.h`

- [ ] **Step 1: Replace the default policy**

Remove `kMSDefaultMaxSweepHelpers` and implement:

```cpp
inline size_t ComputeMSMaxSweepHelpers(size_t gcThreads, size_t configuredMaxHelpers)
{
    size_t logicalMaxHelpers = gcThreads > 1 ? gcThreads - 1 : 0;
    return configuredMaxHelpers == 0
        ? logicalMaxHelpers
        : std::min(logicalMaxHelpers, configuredMaxHelpers);
}
```

- [ ] **Step 2: Do not run build or Runtime validation**

Per user direction, do not build Runtime, replace the SDK dylib, or run a benchmark.

### Task 3: Synchronize the Runtime GC knowledge

**Files:**
- Modify: `/Users/lipeisheng/.codex/skills/cangjie-runtime-skill/references/GC/回收器与任务/collector.md`

- [ ] **Step 1: Correct the helper-limit description**

Replace the statement that the automatic maximum is
`min(cjGCThreads - 1, 8)` with:

```text
未显式配置时最大 Helper 数为 cjGCThreads - 1；显式非零
cjMSParallelSweepMaxHelpers 继续作为人工上限，且不能超过 cjGCThreads - 1。
```

### Task 4: Review the scoped diff

**Files:**
- Review only the three files listed above.

- [ ] **Step 1: Inspect the diff without running validation**

Confirm that the diff changes only the default Sweep Helper cap, its contract expectations,
and the matching knowledge text. Do not claim build or test success.

- [ ] **Step 2: Commit only repository-owned implementation files**

```bash
git add src/Heap/MAllocator/MSParallelSweepPolicy.h \
        test/MSParallelSweepPolicyTest.cpp
git commit -m "perf: remove MSGC sweep helper default cap"
```

The personal Runtime skill knowledge file is outside this repository and is not included in
the repository commit.
