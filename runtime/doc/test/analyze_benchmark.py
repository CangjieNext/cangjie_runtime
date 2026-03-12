#!/usr/bin/env python3
"""
GC Benchmark 基线分析工具
从 stdout 提取 [BENCH] 行的耗时/通过率，
从 GC 日志提取 GC 次数/总耗时/collection rate/碎片率。
输出 JSON 基线文件。
"""

import re, sys, json, argparse
from pathlib import Path
from datetime import datetime


def parse_stdout(filepath: str) -> dict:
    """从 stdout 提取 [BENCH] 行"""
    result = {
        "functional": {},
        "performance": {},
        "summary": {},
    }
    with open(filepath) as f:
        for line in f:
            line = line.strip()
            if not line.startswith("[BENCH]"):
                continue

            # Functional: [BENCH] F1_TreeIntegrity         PASS  123 ms
            m = re.match(r'\[BENCH\]\s+(F\d+_\w+)\s+(PASS|FAIL)\s+(\d+)\s+ms', line)
            if m:
                result["functional"][m.group(1)] = {
                    "status": m.group(2),
                    "time_ms": int(m.group(3)),
                }
                continue

            # Performance with nodes: [BENCH] P1_SmallAlloc  123 ms  456 nodes  789 nodes/ms
            m = re.match(r'\[BENCH\]\s+(P1_\w+)\s+(\d+)\s+ms\s+(\d+)\s+nodes\s+(\d+)\s+nodes/ms', line)
            if m:
                result["performance"][m.group(1)] = {
                    "time_ms": int(m.group(2)),
                    "total_nodes": int(m.group(3)),
                    "throughput_nodes_per_ms": int(m.group(4)),
                }
                continue

            # Performance with MB/s: [BENCH] P2_LargeAlloc  123 ms  45 MB  678 MB/s
            m = re.match(r'\[BENCH\]\s+(P2_\w+)\s+(\d+)\s+ms\s+(\d+)\s+MB\s+(\d+)\s+MB/s', line)
            if m:
                result["performance"][m.group(1)] = {
                    "time_ms": int(m.group(2)),
                    "total_mb": int(m.group(3)),
                    "throughput_mb_per_s": int(m.group(4)),
                }
                continue

            # Performance with rounds: [BENCH] P3_MixedWorkload  123 ms  30 rounds
            m = re.match(r'\[BENCH\]\s+(P\d+_\w+)\s+(\d+)\s+ms\s+(\d+)\s+rounds', line)
            if m:
                result["performance"][m.group(1)] = {
                    "time_ms": int(m.group(2)),
                    "rounds": int(m.group(3)),
                }
                continue

            # Performance P4 with persistent: [BENCH] P4_LongLivedChurn  123 ms  persistent_intact=20/20
            m = re.match(r'\[BENCH\]\s+(P4_\w+)\s+(\d+)\s+ms\s+persistent_intact=(\d+)/(\d+)', line)
            if m:
                result["performance"][m.group(1)] = {
                    "time_ms": int(m.group(2)),
                    "persistent_intact": f"{m.group(3)}/{m.group(4)}",
                }
                continue

            # Summary lines: [BENCH] P1_SmallAlloc:      123 ms
            m = re.match(r'\[BENCH\]\s+(P\d+_\w+):\s+(\d+)\s+ms', line)
            if m:
                result["summary"][m.group(1)] = int(m.group(2))
                continue

            # Total: [BENCH] Total wall time:    123 ms
            m = re.match(r'\[BENCH\]\s+Total wall time:\s+(\d+)\s+ms', line)
            if m:
                result["summary"]["total_wall_time_ms"] = int(m.group(1))
                continue

            # Functional summary: [BENCH] Functional: 5/5 passed
            m = re.match(r'\[BENCH\]\s+Functional:\s+(\d+)/(\d+)\s+passed', line)
            if m:
                result["summary"]["functional_passed"] = int(m.group(1))
                result["summary"]["functional_total"] = int(m.group(2))

    return result


def parse_gclog(filepath: str) -> dict:
    """从 GC 日志提取关键指标"""
    result = {
        "gc_count": 0,
        "gc_reasons": {},
        "gc_times_us": [],
        "collection_rates_mbs": [],
        "last_heap_util_pct": None,
        "last_frag_ext_index": None,
        "last_frag_int_waste_bytes": None,
        "last_active_units": None,
        "last_large_regions": None,
    }

    with open(filepath) as f:
        lines = f.readlines()

    for line in lines:
        # GC START
        if "Begin GC log" in line:
            result["gc_count"] += 1
            m = re.search(r'GCReason:\s*(\w+)', line)
            if m:
                r = m.group(1)
                result["gc_reasons"][r] = result["gc_reasons"].get(r, 0) + 1

        # total gc time
        m = re.search(r'total gc time:\s*([0-9,]+)\s*us.*?collection rate\s*([0-9.]+)\s*MB/s', line)
        if m:
            t = int(m.group(1).replace(',', ''))
            rate = float(m.group(2))
            result["gc_times_us"].append(t)
            result["collection_rates_mbs"].append(rate)

        # heap utilization
        m = re.search(r'heap utilization:\s*([0-9.]+)%', line)
        if m:
            result["last_heap_util_pct"] = float(m.group(1))

        # FRAG-EXT
        m = re.search(r'FRAG-EXT:\s*index\s*([0-9.]+)', line)
        if m:
            result["last_frag_ext_index"] = float(m.group(1))

        # FRAG-INT
        m = re.search(r'internalWaste\s*([0-9]+)\s*B', line)
        if m:
            result["last_frag_int_waste_bytes"] = int(m.group(1))

        # active units
        m = re.search(r'active units:\s*(\d+)', line)
        if m:
            result["last_active_units"] = int(m.group(1))

        # large-object regions
        m = re.search(r'large-object regions\s*(\d+)', line)
        if m:
            result["last_large_regions"] = int(m.group(1))

    # 汇总
    times = result["gc_times_us"]
    if times:
        result["gc_total_time_us"] = sum(times)
        result["gc_avg_time_us"] = sum(times) // len(times)
        result["gc_max_time_us"] = max(times)
        result["gc_min_time_us"] = min(times)
        result["gc_p50_time_us"] = sorted(times)[len(times) // 2]
        result["gc_p99_time_us"] = sorted(times)[int(len(times) * 0.99)]
    rates = result["collection_rates_mbs"]
    if rates:
        result["avg_collection_rate_mbs"] = round(sum(rates) / len(rates), 1)

    # 清理列表（太长不存 JSON）
    del result["gc_times_us"]
    del result["collection_rates_mbs"]

    return result


def main():
    parser = argparse.ArgumentParser(description="GC Benchmark Analyzer")
    parser.add_argument("--stdout", required=True, help="benchmark stdout file")
    parser.add_argument("--gclog", default=None, help="GC log file (MRT_REPORT)")
    parser.add_argument("--output", required=True, help="output JSON baseline file")
    args = parser.parse_args()

    data = {
        "timestamp": datetime.now().isoformat(),
        "runtime": "CopyGC (current)",
        "bench": parse_stdout(args.stdout),
    }

    if args.gclog and Path(args.gclog).exists():
        data["gc"] = parse_gclog(args.gclog)

    with open(args.output, 'w') as f:
        json.dump(data, f, indent=2, ensure_ascii=False)

    # 打印摘要
    print("=== Baseline Summary ===")
    bench = data["bench"]
    for name, info in bench.get("functional", {}).items():
        print(f"  {name}: {info['status']}  {info['time_ms']} ms")
    for name, info in bench.get("performance", {}).items():
        print(f"  {name}: {info.get('time_ms', '?')} ms")
    if "total_wall_time_ms" in bench.get("summary", {}):
        print(f"  Total wall time: {bench['summary']['total_wall_time_ms']} ms")

    if "gc" in data:
        gc = data["gc"]
        print(f"\n=== GC Summary ===")
        print(f"  GC count: {gc['gc_count']}")
        print(f"  GC reasons: {gc['gc_reasons']}")
        if "gc_total_time_us" in gc:
            print(f"  GC total: {gc['gc_total_time_us']} us"
                  f" ({gc['gc_total_time_us']/1000:.1f} ms)")
            print(f"  GC avg: {gc['gc_avg_time_us']} us,"
                  f" p50: {gc['gc_p50_time_us']} us,"
                  f" p99: {gc['gc_p99_time_us']} us,"
                  f" max: {gc['gc_max_time_us']} us")
        if gc.get("avg_collection_rate_mbs"):
            print(f"  Avg collection rate: {gc['avg_collection_rate_mbs']} MB/s")
        if gc.get("last_heap_util_pct") is not None:
            print(f"  Last heap util: {gc['last_heap_util_pct']}%")
        if gc.get("last_frag_ext_index") is not None:
            print(f"  Last FRAG-EXT index: {gc['last_frag_ext_index']}")

    print(f"\n  Saved to: {args.output}")


if __name__ == "__main__":
    main()
