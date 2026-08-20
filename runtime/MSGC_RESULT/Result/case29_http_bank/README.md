# case29_http_yinhang — in-memory HTTP comparison

The original Spring/MyBatis/MySQL project remains unchanged. This mirror uses a
deterministic in-memory account/card dataset while preserving the endpoint
behavior: active-account filtering, active-card aggregation, department/name/id
sorting, pagination, and the original seven JSON response fields.

The Cangjie sources are in `cangjie_benchmark/`; Java sources are in
`java_benchmark/`.

Run from this directory:

    CASE29_ACCOUNT_COUNT=1000 CASE29_CONCURRENCY=5 CASE29_DURATION_MS=10000 HEAP_SIZE=512mb ./run_compare.sh

    CASE29_ACCOUNT_COUNT=1000 CASE29_CONCURRENCY=50 CASE29_DURATION_MS=10000 HEAP_SIZE=512mb ./run_compare.sh

The configurable values are CASE29_SEED, CASE29_ACCOUNT_COUNT,
CASE29_SUBJECT_ID, CASE29_LIMIT, CASE29_OFFSET, CASE29_CONCURRENCY,
CASE29_DURATION_MS, PROCS, and HEAP_SIZE.

PROCS remains optional and maps to Cangjie cjProcessorNum and Java
ActiveProcessorCount when explicitly set. The default leaves CPU unrestricted;
the runner does not apply a CPU quota, core affinity, or CPU throttling.
HEAP_SIZE maps to cjHeapSize and Java Xmx and does not hard-cap RSS. CPU and
RSS are sampled every 200ms. The result directory contains source snapshots,
compile/run logs, GC logs, metadata, RSS/CPU samples, and case25-compatible
reports/charts. MSGC always uses gc-mode marksweep.

## Verified unrestricted-CPU samples

macOS arm64, 1,000 accounts, 10 seconds, HEAP_SIZE 512mb, and PROCS unset. All
runs completed with zero failed requests:

| Client concurrency | Side | Throughput | RSS peak | sampled CPU average |
| ---: | --- | ---: | ---: | ---: |
| 5 | Java 25 | 16,657 req/s | 306.91 MB | 409.18% |
| 5 | MSGC | 23,454 req/s | 84.06 MB | 462.38% |
| 5 | CopyGC | 16,002 req/s | 543.66 MB | 364.59% |
| 50 | Java 25 | 17,890 req/s | 331.73 MB | 486.63% |
| 50 | MSGC | 30,351 req/s | 97.84 MB | 591.95% |
| 50 | CopyGC | 18,142 req/s | 563.42 MB | 416.16% |

The matching reports are in result/20260819_173143_seed0_a1000_c5_ProcsAuto_compare
and result/20260819_173419_seed0_a1000_c50_ProcsAuto_compare.
