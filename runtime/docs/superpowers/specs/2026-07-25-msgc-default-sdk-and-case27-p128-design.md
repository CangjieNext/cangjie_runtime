# MSGC Default SDK and case27 P=128 Design

## Goal

Use `/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/msgc`
as the default MSGC SDK for future local tests, then run the case27 high-P
benchmark once with `cjProcessorNum=128`.

## Scope

- Update active defaults and current validation instructions that still point
  at an older self-built MSGC SDK.
- Preserve historical result and bug-analysis documents because their SDK paths
  describe the environments used for those recorded runs.
- Keep environment-variable overrides available for callers that need another
  SDK.
- Do not replace or rebuild the runtime dylib for this benchmark.

## Files

- `AGENTS.md`: update the current MSGC SDK convention, runtime path, commands,
  and examples.
- `MSGC_RESULT/Result/script/compare.py`: update `DEFAULT_MSGC_SDK`.
- `test/SignalRuntimeNoiseRegression.sh`: update the `SDK_ROOT` fallback.
- `MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/run_msgc_high_p.sh`:
  no SDK-path change is required because it already uses the requested path.

## Benchmark Execution

Run:

```bash
PROCS=128 \
  MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/run_compare.sh
```

The script will use an isolated environment, compile `benchmark.cj` with the
MSGC SDK, and run the generated binary with `cjProcessorNum=128`. Its existing
defaults remain 100K payload, concurrency 1000, duration 60000 ms, and 16 GB
heap.

Before launching, inspect old `benchmark`, `rss_monitor.py`, and related
processes and verify that the selected server port is not occupied by a stale
case27 process.

## Verification

- Validate that the SDK contains `envsetup.sh`, the macOS arm64 runtime dylib,
  and required stdx libraries.
- Run focused checks covering the changed Python and shell defaults.
- Confirm the result directory, compile and run exit codes, `[RESULT]` line,
  and loaded runtime path.
- Report compiler, runtime, and stdlib sources explicitly.
- Summarize throughput, request success/failure counts, latency, CPU/RSS, and
  relevant GC statistics from the generated artifacts.

