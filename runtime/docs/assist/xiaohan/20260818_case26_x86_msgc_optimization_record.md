# Case26 x86 MSGC Optimization Record

## Scope

This record describes the Cangjie-side configuration and source changes used by the current x86 case26 measurement. It separates runtime parameters, SDK changes, benchmark changes, and API compatibility work. It does not claim that the Cangjie and Java implementations have identical allocation paths.

## Measured Configuration

The recorded paired run is:

`MSGC_RESULT/Result/case26_http_complex_json/result/20260815_64M_c100_100k_HeapGrow1_Scavenge1.10_Procs12_compare_3`

| Item | Cangjie MSGC | Java 8 |
| --- | --- | --- |
| Payload / concurrency / duration | 100K / 100 / 60 s | 100K / 100 / 60 s |
| Compile / collector | `-O2 --gc-mode=marksweep` | Parallel GC |
| Heap limit | `cjHeapSize=8gb` | `-Xmx8g` |
| Processor setting | `cjProcessorNum=12` | `-XX:ActiveProcessorCount=12` |
| GC workers | Runtime default for this MSGC build | `-XX:ParallelGCThreads=6` |
| Persistent baseline | 64 MiB, 8 workers | 64 MiB, 8 workers |
| Growth settings | `cjHeapGrowth=1`, `cjScavengeGrowth=1.10`, `cjMSMinGCThreshold=20mb`, `cjMSAdaptiveGrowth=1` | Default ParallelGC adaptive policy |

The run completed with zero failed requests. The generated comparison report records Cangjie at 1,219 req/s and Java at 1,337 req/s. Cangjie therefore reached 91.2% of the Java throughput in this one run. Average RSS was 766.66 MB for Cangjie and 2,946.50 MB for Java.

The result is a single paired run, not a multi-round alternating A/B experiment. It is a point-in-time comparison rather than a stable regression threshold.

## Cangjie Optimizations

### Runtime Parameter: Adaptive MSGC Heap Growth

`cjMSAdaptiveGrowth=1` is enabled for the Cangjie run. This is a runtime tuning parameter, not a source-code optimization. It changes post-GC threshold selection and must be recorded whenever data is compared with runs that did not enable it.

### Benchmark: Response Serialization Capacity

`benchmark.cj` serializes the mutated DOM with:

```cangjie
root.toStringWithCapacity(requestBytes + requestBytes / 8 + 1024)
```

The response differs from the request only by a small number of values and fields. The capacity hint avoids the generic `toString()` capacity-estimation traversal before writing JSON. It preserves JSON values and field mutation semantics. `WriteBuffer` still grows safely when the estimate is insufficient.

This is a case26 benchmark change. It makes the Cangjie DOM serialization allocation path more efficient than an implementation that uses generic `toString()`.

### Benchmark: HTTP Body Read Allocation Reduction

`benchmark.cj` reads HTTP bodies in 64 KiB chunks, reserves `ArrayList` capacity when the HTTP body size is known, and returns a slice of the backing array rather than creating an additional `ArrayList -> Array` copy.

This is a case26 benchmark change. It preserves the body bytes passed to JSON parsing but reduces chunk iterations, ArrayList growth, and one intermediate copy relative to the earlier generic reader.

### SDK API Adaptation (Not a Performance Optimization)

Commit `54dfab5b` changes the benchmark from `JsonValue.fromUtf8Bytes` to `String.fromUtf8` plus `JsonValue.fromStr`, and replaces unavailable `setValue` calls with `put` plus newly constructed `JsonInt` or `JsonFloat` values.

This change was required to compile with the current SDK. It must not be attributed as a performance optimization. In particular, creating replacement numeric nodes can add allocation compared with in-place scalar mutation.

### SDK DOM Parsed-Root Capacity Hint Exclusion

The current uncommitted `cangjie_stdx` worktree also records input length on parsed roots so ordinary `toString()` can skip recursive capacity estimation. Case26 already calls `toStringWithCapacity` explicitly, so this separate, case27-oriented change is not part of the case26 patch inventory below.

## Comparability Boundary

The case26 Cangjie and Java programs apply the same business JSON mutation for valid benchmark payloads. Their internal parsing, HTTP buffering, serialization, allocation, and exception-handling paths are not identical. The Cangjie-side capacity and body-read changes above are therefore valid implementation optimizations, but the result should be described as an end-to-end implementation comparison, not a same-allocation-path microbenchmark.

## Patch Inventory

| Patch | Repository to apply from | Contents | Classification |
| --- | --- | --- | --- |
| `patches/20260818_case26_benchmark_optimization_series.patch` | `cangjie_runtime` | Four case26 `benchmark.cj` commits: JSON/serialization, byte-body path, and final API adaptation | Includes both performance work and the final required API adaptation |
| `patches/20260818_case26_stdx_json_optimization.patch` | `cangjie_stdx` | Commit `0de8f8d`; `toStringWithCapacity`, Float raw-token preservation, and WriteBuffer copy reductions | SDK JSON support required by the case26 optimization series |

The benchmark patch is an mbox series, not a one-file diff: apply it with `git am` in `cangjie_runtime`. The stdx patch is a separate mbox patch and must be applied with `git am` in `cangjie_stdx`.

## Applying the Patches

Apply the stdx patch before the benchmark series, because the benchmark calls `JsonValue.toStringWithCapacity` supplied by the stdx patch:

```bash
cd /root/workspace/code/msgc/cangjie_stdx
git am --3way /root/workspace/code/msgc/cangjie_runtime/runtime/docs/assist/patches/20260818_case26_stdx_json_optimization.patch

cd /root/workspace/code/msgc/cangjie_runtime
git am --3way /root/workspace/code/msgc/cangjie_runtime/runtime/docs/assist/patches/20260818_case26_benchmark_optimization_series.patch
```

The patches were verified in isolated worktrees with exactly those `git am --3way` commands. They should be applied only to branches that need to reproduce this historical case26 optimization series; the current local branches already contain the committed changes.

After applying the stdx patch, rebuild and install the MSGC SDK before recompiling case26:

```bash
python3 /root/workspace/code/msgc/cangjie_runtime/runtime/build/scripts/rebuild_msgc_sdk.py
```

Then compile the case26 benchmark with the installed SDK using `-O2 --gc-mode=marksweep` and rerun the body-read self-test before a performance run:

```bash
CASE26_BODY_READ_SELF_TEST=1 ./benchmark
```
