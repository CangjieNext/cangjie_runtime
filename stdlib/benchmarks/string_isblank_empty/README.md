# `String.isBlank()` empty-string benchmark

This benchmark reproduces issue #582 with 256 Ki empty strings. It measures
the previous implementation (the original `runes()` loop) and the current
`String.isBlank()` implementation in the same process and with the same input.

Build the standard library from this checkout first, then run the benchmark
with an SDK that uses that build:

```shell
cd stdlib/benchmarks/string_isblank_empty
cjpm bench
```

The report is written to `bench_report/`. Compare the `Median` values for
`checkBaselineIsBlank` and `checkOptimizedIsBlank`; the optimized benchmark
should be materially faster because empty strings no longer construct a rune
iterator.
