# `String.isBlank()` empty-string benchmark

This benchmark reproduces issue #582 with 256 Ki empty strings. It measures
the previous implementation (the original `runes()` loop) and the current
`String.isBlank()` implementation in the same process and with the same input.

Run the benchmark with the Cangjie SDK under test:

```shell
cd stdlib/benchmarks/string_isblank_empty
cjpm bench
```

The report is written to `bench_report/`. `checkReferenceEmptyOrBlank`
reproduces the issue's `str.isEmpty() || str.isBlank()` reference expression.
Compare its `Median` with `checkOptimizedIsBlank`; they should be comparable
because empty strings no longer construct a rune iterator. The
`checkBaselineIsBlank` case retains the original rune-loop implementation for
before/after comparisons in the same process.

For an exact source-level before/after comparison, build the `std.unicode`
library from each revision and run this benchmark with the corresponding
standard-library artifacts. Do not compare an unmodified SDK against a source
checkout unless the SDK has been built from that checkout.
