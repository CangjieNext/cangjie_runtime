#!/usr/bin/env python3
"""
GC Stress Test — 资源监控脚本
启动被测程序，每 200ms 采样 RSS 和 CPU%，输出 CSV。

用法：
  python3 monitor_stress.py <executable> [args...] --csv <output.csv>

示例：
  python3 monitor_stress.py ./gc_stress_pinned --csv copygc_metrics.csv
  MRT_REPORT=./gc.log cjHeapSize=256mb python3 monitor_stress.py ./gc_stress_pinned --csv ms_metrics.csv
"""

import subprocess
import sys
import time
import csv
import os
import signal


def get_rss_mb(pid):
    """通过 ps 获取进程 RSS (MB)"""
    try:
        out = subprocess.check_output(
            ["ps", "-o", "rss=", "-p", str(pid)],
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
        if out:
            return int(out) / 1024.0  # KB → MB
    except (subprocess.CalledProcessError, ValueError):
        pass
    return None


def get_cpu_percent(pid, interval=0.1):
    """通过 /proc 或 ps 获取 CPU%（macOS 用 ps）"""
    try:
        out = subprocess.check_output(
            ["ps", "-o", "%cpu=", "-p", str(pid)],
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()
        if out:
            return float(out)
    except (subprocess.CalledProcessError, ValueError):
        pass
    return None


def main():
    args = sys.argv[1:]
    csv_path = "metrics.csv"

    # 解析 --csv 参数
    if "--csv" in args:
        idx = args.index("--csv")
        csv_path = args[idx + 1]
        args = args[:idx] + args[idx + 2:]

    if not args:
        print("Usage: python3 monitor_stress.py <executable> [args...] --csv <output.csv>")
        sys.exit(1)

    # 继承环境变量（MRT_REPORT, cjHeapSize 等）
    env = os.environ.copy()

    print(f"[monitor] Starting: {' '.join(args)}")
    print(f"[monitor] CSV output: {csv_path}")
    print(f"[monitor] Sampling every 200ms")

    stderr_path = csv_path.replace(".csv", "_stderr.txt")
    stderr_file = open(stderr_path, "w")
    proc = subprocess.Popen(
        args,
        env=env,
        stdout=subprocess.PIPE,
        stderr=stderr_file,
    )

    samples = []
    t0 = time.time()
    sample_interval = 0.2  # 200ms

    try:
        while proc.poll() is None:
            elapsed = time.time() - t0
            rss = get_rss_mb(proc.pid)
            cpu = get_cpu_percent(proc.pid)

            if rss is not None:
                samples.append({
                    "time_s": round(elapsed, 2),
                    "rss_mb": round(rss, 1),
                    "cpu_pct": round(cpu, 1) if cpu is not None else 0.0,
                })

            time.sleep(sample_interval)
    except KeyboardInterrupt:
        print("\n[monitor] Interrupted, killing process ...")
        proc.send_signal(signal.SIGTERM)
        proc.wait(timeout=5)

    # 读取程序输出
    stdout_data, _ = proc.communicate()
    stderr_file.close()
    output_text = stdout_data.decode("utf-8", errors="replace")

    # 打印程序输出
    print("\n--- Program Output ---")
    print(output_text)
    print(f"--- Exit Code: {proc.returncode} ---")

    # 保存程序输出到 txt 文件
    txt_path = csv_path.replace(".csv", "_output.txt")
    with open(txt_path, "w") as f:
        f.write(output_text)

    # 写 CSV
    if samples:
        with open(csv_path, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=["time_s", "rss_mb", "cpu_pct"])
            writer.writeheader()
            writer.writerows(samples)
        print(f"[monitor] Saved {len(samples)} samples to {csv_path}")

        # 简要统计
        rss_values = [s["rss_mb"] for s in samples]
        cpu_values = [s["cpu_pct"] for s in samples]
        print(f"[monitor] RSS: min={min(rss_values):.1f} MB, max={max(rss_values):.1f} MB, "
              f"avg={sum(rss_values)/len(rss_values):.1f} MB")
        print(f"[monitor] CPU: min={min(cpu_values):.1f}%, max={max(cpu_values):.1f}%, "
              f"avg={sum(cpu_values)/len(cpu_values):.1f}%")
    else:
        print("[monitor] No samples collected!")

    sys.exit(proc.returncode or 0)


if __name__ == "__main__":
    main()
