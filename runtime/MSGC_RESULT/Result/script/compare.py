#!/usr/bin/env python3
"""
compare.py — MSGC vs CopyGC 通用对比脚本 (case2 ~ case7)
================================================================
采集指标：
  • 执行时间 (wall time)
  • RSS 趋势图
  • CPU 趋势图
  • GC 每次执行时间柱状图
  • GC 执行次数 + 总时间柱状图
  • 子测试/阶段时间对比柱状图

产物目录：<CASE_DIR>/result/YYYYMMDD_HHMMSS_MSGC_<ver>_vs_CopyGC_<ver>_O1/

使用方式：
    python3 script/compare.py --case case3_half_alive [--heap 2gb] [--skip-build]
    python3 script/compare.py --case case2_50x_pressure --msgc-sdk /path/to/out --copygc-sdk /path/to/copygc/sdk
"""

import argparse
import os
import re
import subprocess
import sys
import time
import resource
from datetime import datetime
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

# ─────────────────────────────────────────────────────────────────────────────
# SDK 路径
# ─────────────────────────────────────────────────────────────────────────────
DEFAULT_MSGC_SDK   = Path('/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260509/cangjie')
DEFAULT_COPYGC_SDK = Path('/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie')

MSGC_SDK   = DEFAULT_MSGC_SDK
COPYGC_SDK = DEFAULT_COPYGC_SDK

def _ver(sdk: Path) -> str:
    return sdk.parent.name

MSGC_VER   = _ver(MSGC_SDK)
COPYGC_VER = _ver(COPYGC_SDK)

RESULT_ROOT = Path(__file__).resolve().parent.parent   # Result/

COLORS = {'MSGC': '#E05C5C', 'CopyGC': '#4C8EF5'}
SAMPLE_INTERVAL = 0.1

# ─────────────────────────────────────────────────────────────────────────────
# 各 case 配置
# ─────────────────────────────────────────────────────────────────────────────
CASE_CONFIGS = {
    'case1_baseline': {
        'heap':      '256mb',
        'title':     'case1_baseline',
        'subtests':  ['F1_TreeIntegrity', 'F2_Generational', 'F3_LargeObject',
                      'F4_LinkedList', 'F5_HashMap',
                      'P1_SmallAlloc', 'P2_LargeAlloc', 'P3_MixedWorkload',
                      'P4_LongLivedChurn', 'P5_Fragmentation'],
        'parse':     'bench',
    },
    'case2_50x_pressure': {
        'heap':      '4gb',
        'title':     'case2_50x_pressure',
        'subtests':  ['F1_TreeIntegrity', 'F2_Generational', 'F3_LargeObject',
                      'F4_LinkedList', 'F5_HashMap',
                      'P1_SmallAlloc', 'P2_LargeAlloc', 'P3_MixedWorkload',
                      'P4_LongLivedChurn', 'P5_Fragmentation'],
        'parse':     'bench',
    },
    'case3_half_alive': {
        'heap':      '2gb',
        'title':     'case3_half_alive',
        'subtests':  ['PHASE1', 'PHASE2', 'PHASE3'],
        'parse':     'phase',
    },
    'case4_read_heavy': {
        'heap':      '2gb',
        'title':     'case4_read_heavy',
        'subtests':  ['PHASE1', 'PHASE2', 'PHASE3'],
        'parse':     'phase',
        'max_wall_s': 600,
    },
    'case5_mixed_objects': {
        'heap':      '1gb',
        'title':     'case5_mixed_objects',
        'subtests':  ['PHASE1', 'PHASE2', 'PHASE3'],
        'parse':     'phase',
    },
    'case6_10g_scale': {
        'heap':      '12gb',
        'title':     'case6_10g_scale',
        'subtests':  ['PHASE1', 'PHASE2', 'PHASE3'],
        'parse':     'phase',
    },
    'case7_read_barrier_read_mostly': {
        'heap':      '512mb',
        'title':     'case7_read_barrier_read_mostly',
        'subtests':  ['PHASE1', 'PHASE2', 'PHASE3'],
        'parse':     'phase',
    },
    'case10_large_objects': {
        'heap':      '256mb',
        'title':     'case10_large_objects',
        'subtests':  ['F1_SingleSizeIntegrity', 'F2_MultiSizeIntegrity',
                      'F3_PersistentSet',
                      'P1_SequentialLargeAlloc', 'P2_MultiSizeLargeAlloc',
                      'P3_LongLivedLargeChurn', 'P4_LargeFragmentation',
                      'P5_BurstLargeAlloc'],
        'parse':     'bench',
    },
    'case11_small_objects': {
        'heap':      '256mb',
        'title':     'case11_small_objects',
        'subtests':  ['F1_SingleSizeIntegrity', 'F2_MultiSizeIntegrity',
                      'F3_PersistentSet',
                      'P1_SequentialSmallAlloc', 'P2_MultiSizeSmallAlloc',
                      'P3_LongLivedSmallChurn', 'P4_SmallFragmentation',
                      'P5_BurstSmallAlloc'],
        'parse':     'bench',
    },
    'case12_pure_small_objects': {
        'heap':      '256mb',
        'title':     'case12_pure_small_objects',
        'subtests':  ['F1_SingleSizeIntegrity', 'F2_MultiSizeIntegrity',
                      'F3_PersistentSet',
                      'P1_SequentialSmallAlloc', 'P2_MultiSizeSmallAlloc',
                      'P3_LongLivedSmallChurn', 'P4_SmallFragmentation',
                      'P5_BurstSmallAlloc'],
        'parse':     'bench',
    },
    'case13_pure_large_objects': {
        'heap':      '256mb',
        'title':     'case13_pure_large_objects',
        'subtests':  ['F1_SingleSizeIntegrity', 'F2_MultiSizeIntegrity',
                      'F3_PersistentSet',
                      'P1_SequentialLargeAlloc', 'P2_MultiSizeLargeAlloc',
                      'P3_LongLivedLargeChurn', 'P4_LargeFragmentation',
                      'P5_BurstLargeAlloc'],
        'parse':     'bench',
    },
    'case21_small_object_object_struct': {
        'heap':      '512mb',
        'title':     'case21_small_object_object_struct',
        'subtests':  ['F1_ObjectIntegrity', 'F2_StructWindow', 'F3_PersistentGraph',
                      'P1_ObjectCreateRead', 'P2_StructCreateRead', 'P3_ChainCreateRead',
                      'P4_LongLivedChurn', 'P5_SlotReuse'],
        'parse':     'bench',
        'max_wall_s': 300,
    },
    'case22_large_object_containers': {
        'heap':      '1gb',
        'title':     'case22_large_object_containers',
        'subtests':  ['F1_LargePayloadIntegrity', 'F2_ListWrappedIntegrity', 'F3_MapWrappedIntegrity',
                      'P1_SequentialLargeCreateRead', 'P2_ListWrappedLargeRead',
                      'P3_MapWrappedLargeRead', 'P4_LongLivedContainerRead', 'P5_FragmentedLargeReuse'],
        'parse':     'bench',
        'max_wall_s': 300,
    },
    'case23_concurrent_mixed_alloc_read': {
        'heap':      '2gb',
        'title':     'case23_concurrent_mixed_alloc_read',
        'subtests':  ['F1_ConcurrentSmallIntegrity', 'F2_ConcurrentLargeIntegrity',
                      'F3_ConcurrentMixedIntegrity', 'P1_ConcurrentSmallCreateRead',
                      'P2_ConcurrentLargeCreateRead', 'P3_ConcurrentMixedCreateRead',
                      'P4_ConcurrentPersistentRead', 'P5_ConcurrentContainerChurn'],
        'parse':     'bench',
        'case_extra_env': {'CASE23_CONCURRENCY': '1000'},
        'max_wall_s': 900,
    },
    'case24_single_coroutine_heavy_alloc_read': {
        'heap':      '16gb',
        'title':     'case24_single_coroutine_heavy_alloc_read',
        'subtests':  ['F1_Persistent100MIntegrity', 'F2_Temp100MChurnIntegrity',
                      'F3_PostChurnReadIntegrity', 'P1_ConcurrentPersistent100MCreate',
                      'P2_ConcurrentPersistent100MPlusTemp100M', 'P3_PostChurnPersistentReadHeavy',
                      'P4_AliasWrappedPersistentRead', 'P5_ReadHeavyAfterTempChurn'],
        'parse':     'bench',
        'case_extra_env': {
            'CASE24_CONCURRENCY': '20',
            'CASE24_PERSISTENT_MB': '100',
            'CASE24_TEMP_MB': '100',
            'CASE24_READ_LOOPS': '8',
        },
        'max_wall_s': 1800,
    },
    'case14_arraylist_small_no_expand': {
        'heap':      '256mb',
        'title':     'case14_arraylist_small_no_expand',
        'subtests':  ['F1_SingleSizeIntegrity', 'F2_MultiSizeIntegrity',
                      'F3_PersistentSet',
                      'P1_SequentialSmallAlloc', 'P2_MultiSizeSmallAlloc',
                      'P3_LongLivedSmallChurn', 'P4_SmallFragmentation',
                      'P5_BurstSmallAlloc'],
        'parse':     'bench',
    },
    'case16_20gb_concurrent': {
        'heap':      '16gb',
        'title':     'case16_20gb_concurrent',
        'subtests':  ['PHASE1', 'PHASE2'],
        'parse':     'phase',
    },
    'case_mid': {
        'heap':      '8gb',
        'title':     'case_mid',
        'subtests':  ['PHASE1', 'PHASE2'],
        'parse':     'phase',
    },
    'case8_http_json_serde': {
        'heap':           '256mb',
        'title':          'case8_http_json_serde',
        'subtests':       ['P1_1KB_HighFreq', 'P2_10KB_MedFreq', 'P3_500KB_LowFreq', 'P4_Mixed'],
        'parse':          'http_phase',
        'max_wall_s':     150,
        'case_extra_env': {'CASE9_TOTAL_MS': '60000'},
        'msgc_port':      '18130',
        'copygc_port':    '18140',
        'subtest_unit':   'req/s',
        'higher_is_better': True,
    },
    'case17_json_file_serde': {
        'heap':           '6gb',
        'title':          'case17_json_file_serde',
        # 1000 并发阶段：单阶段 fire-and-forget 压测
        'subtests':       ['P1_1000conn'],
        'parse':          'http_phase',
        'max_wall_s':     600,
        'zero_cpu_limit_s': 120,
        'case_extra_env': {
            'CASE17_PHASE_MS':  '20000',
            # JSON 文件使用绝对路径，避免进程工作目录不确定问题
            'CASE17_JSON_FILE': str(RESULT_ROOT / 'case17_json_file_serde' / 'json' / 'payload_10k.json'),
            'CASE17_CONCURRENCY': '1000',
            'CASE17_CLIENT_TIMEOUT_MS': '5000',
            'CASE17_DRAIN_TIMEOUT_MS': '7000',
        },
        'port_env_var':   'CASE17_SERVER_PORT',   # 覆盖默认的 CASE9_SERVER_PORT
        'msgc_port':      '18320',
        'copygc_port':    '18330',
        'subtest_unit':   'req/s',
        'higher_is_better': True,
    },
    'case18_fire_forget': {
        'heap':           '8gb',
        'title':          'case18_fire_forget',
        # 5 个 fire-and-forget 阶段：maxInflight=2/4/8/12/16（共享client，避免高并发bug）
        'subtests':       ['P1_2inf', 'P2_4inf', 'P3_8inf',
                           'P4_12inf', 'P5_16inf'],
        'parse':          'http_phase',
        'max_wall_s':     600,
        'case_extra_env': {
            'CASE18_PHASE_MS':  '20000',
            'CASE18_JSON_FILE': str(RESULT_ROOT / 'case17_json_file_serde' / 'json' / 'payload_100k.json'),
        },
        'port_env_var':   'CASE18_SERVER_PORT',
        'msgc_port':      '18240',
        'copygc_port':    '18250',
        'subtest_unit':   'req/s',
        'higher_is_better': True,
    },
}

# ─────────────────────────────────────────────────────────────────────────────
# 参数解析
# ─────────────────────────────────────────────────────────────────────────────
def parse_args():
    p = argparse.ArgumentParser(description='MSGC vs CopyGC 通用对比脚本')
    p.add_argument('--case',       required=True,
                   choices=list(CASE_CONFIGS.keys()),
                   help='要对比的 case 名称')
    p.add_argument('--runs',       type=int, default=1,
                   help='每个 SDK 的运行次数（默认 1）')
    p.add_argument('--heap',       default=None,
                   help='堆大小（默认使用 case 配置的推荐值）')
    p.add_argument('--skip-build', action='store_true',
                   help='跳过编译，从历史 result/ 中找最新 binary')
    p.add_argument('--extra-env', metavar='KEY=VAL', action='append', default=[],
                   help='追加/覆盖 MSGC 运行时的环境变量（可多次指定）')
    p.add_argument('--json-file', metavar='PATH', default=None,
                   help='覆盖 CASE17_JSON_FILE（对 MSGC 和 CopyGC 均生效）')
    p.add_argument('--proc', type=int, default=6, metavar='N',
                   help='cjProcessorNum（默认 6，对 MSGC 和 CopyGC 均生效）')
    p.add_argument('--opt', choices=['O0', 'O1'], default='O1',
                   help='编译优化级别（默认 O1）')
    p.add_argument('--msgc-sdk', default=str(DEFAULT_MSGC_SDK),
                   help='MSGC SDK 根目录；可传 compiler/output 这类新组装包')
    p.add_argument('--copygc-sdk', default=str(DEFAULT_COPYGC_SDK),
                   help='CopyGC SDK 根目录')
    return p.parse_args()


def resolve_sdk(path_str: str, label: str) -> Path:
    sdk = Path(path_str).expanduser().resolve()
    envsetup = sdk / 'envsetup.sh'
    if not envsetup.is_file():
        print(f'[ERROR] {label} SDK invalid: missing {envsetup}', file=sys.stderr)
        sys.exit(1)
    return sdk

# ─────────────────────────────────────────────────────────────────────────────
# SDK 环境
# ─────────────────────────────────────────────────────────────────────────────
def load_sdk_env(sdk: Path) -> dict:
    cmd = f'source "{sdk}/envsetup.sh" >/dev/null 2>&1 && env -0'
    clean_env_cmd = [
        'env', '-i',
        f'HOME={os.environ.get("HOME", "")}',
        f'TERM={os.environ.get("TERM", "xterm-256color")}',
        'PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin',
        'bash', '-c', cmd,
    ]
    raw = subprocess.check_output(clean_env_cmd, text=True)
    env = {}
    for item in raw.split('\0'):
        if '=' in item:
            k, _, v = item.partition('=')
            env[k] = v
    if 'DYLD_LIBRARY_PATH' not in env and 'CANGJIE_HOME' in env:
        hw_arch = subprocess.check_output(['uname', '-m'], text=True).strip()
        if hw_arch == 'arm64':
            hw_arch = 'aarch64'
        cj_home = env['CANGJIE_HOME']
        env['DYLD_LIBRARY_PATH'] = (
            f'{cj_home}/runtime/lib/darwin_{hw_arch}_cjnative'
            f':{cj_home}/tools/lib'
        )
    # Auto-detect sibling stdx directory (needed for stdx.net.http / stdx.encoding.json)
    hw_arch = subprocess.check_output(['uname', '-m'], text=True).strip()
    if hw_arch == 'arm64':
        hw_arch = 'aarch64'
    stdx_dir = sdk.parent / f'darwin_{hw_arch}_cjnative/dynamic/stdx'
    if stdx_dir.is_dir():
        cur = env.get('DYLD_LIBRARY_PATH', '')
        env['DYLD_LIBRARY_PATH'] = str(stdx_dir) + (':' + cur if cur else '')
    return env

# ─────────────────────────────────────────────────────────────────────────────
# 编译
# ─────────────────────────────────────────────────────────────────────────────
def compile_binary(sdk: Path, env: dict, src: Path, out: Path, opt: str = '-O1'):
    print(f'  [build] cjc {opt} {src.name} → {out.name} ...', flush=True)
    hw_arch = subprocess.check_output(['uname', '-m'], text=True).strip()
    if hw_arch == 'arm64':
        hw_arch = 'aarch64'
    cmd = ['cjc', opt, str(src), '-o', str(out)]
    # Auto-add stdx flags if sibling stdx dir exists (needed for stdx.net.http, etc.)
    stdx_cjo_dir = sdk.parent / f'darwin_{hw_arch}_cjnative/dynamic/stdx'
    if stdx_cjo_dir.is_dir():
        cmd += ['--import-path', str(stdx_cjo_dir)]
        cmd += ['-L', str(stdx_cjo_dir)]
        # Link all stdx dylibs found in the directory
        for dylib in sorted(stdx_cjo_dir.glob('libstdx.*.dylib')):
            cmd += ['--link-option', str(dylib)]
    result = subprocess.run(cmd, env=env, capture_output=True, text=True,
                            cwd=str(out.parent))
    if result.returncode != 0:
        print(f'  [ERROR] compile failed:\n{result.stderr}', file=sys.stderr)
        sys.exit(1)
    print(f'  [build] done.', flush=True)

# ─────────────────────────────────────────────────────────────────────────────
# GC log 解析
# ─────────────────────────────────────────────────────────────────────────────
_TS_RE = re.compile(r'^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)')

def _parse_ts(line: str):
    m = _TS_RE.match(line)
    if not m:
        return None
    ts_str = re.sub(r'(\.\d{6})\d*', r'\1', m.group(1))
    try:
        return datetime.strptime(ts_str, '%Y-%m-%d %H:%M:%S.%f').timestamp()
    except ValueError:
        return None

def parse_gc_log_msgc(log_path: Path) -> dict:
    if not log_path.exists():
        return {'gc_count': 0, 'cycle_us': [], 'stw_us': [], 'total_gc_us': 0.0}
    gc_count, cycle_us, stw_us = 0, [], []
    with open(log_path, errors='replace') as f:
        for line in f:
            if '=== GC START cycle=' in line:
                gc_count += 1
            elif 'total gc time:' in line:
                # 使用日志记录的实际 GC 工作耗时，而非相邻 GC START 时间戳差值
                m = re.search(r'total gc time:\s*([\d,]+)\s*us', line)
                if m:
                    cycle_us.append(float(m.group(1).replace(',', '')))
            elif 'MarkTermination total:' in line:
                m = re.search(r'MarkTermination total:\s*(\d+)\s*us', line)
                if m:
                    stw_us.append(float(m.group(1)))
    return {
        'gc_count':    gc_count,
        'cycle_us':    cycle_us,
        'stw_us':      stw_us,
        'total_gc_us': sum(cycle_us),
    }

def parse_gc_log_copygc(log_path: Path) -> dict:
    if not log_path.exists():
        return {'gc_count': 0, 'cycle_us': [], 'total_gc_us': 0.0}
    cycle_us = []
    with open(log_path, errors='replace') as f:
        for line in f:
            m = re.search(r'total gc time:\s*([\d,]+)\s*us', line)
            if m:
                cycle_us.append(float(m.group(1).replace(',', '')))
    return {
        'gc_count':    len(cycle_us),
        'cycle_us':    cycle_us,
        'total_gc_us': sum(cycle_us),
    }

# ─────────────────────────────────────────────────────────────────────────────
# 端口清理：释放被残留进程占用的 TCP 端口（UE 进程 kill -9 无效时忽略）
# ─────────────────────────────────────────────────────────────────────────────
def _try_free_port(port: int) -> None:
    try:
        out = subprocess.check_output(
            ['lsof', '-t', '-i', f':{port}'], text=True
        ).strip()
        if not out:
            return
        for pid_str in out.splitlines():
            pid = int(pid_str.strip())
            try:
                os.kill(pid, 9)
                print(f'  [port-cleanup] killed pid={pid} on port {port}', flush=True)
            except Exception:
                pass
        time.sleep(0.5)   # 等待 OS 回收端口
    except Exception:
        pass

# ─────────────────────────────────────────────────────────────────────────────
# 单次运行
# ─────────────────────────────────────────────────────────────────────────────
def run_once(label: str, run_idx: int, binary: Path, env: dict, heap: str,
             gc_log: Path, parse_style: str, subtests_list: list,
             extra_env=None, max_wall_s: int = 180,
             zero_cpu_limit_s: float = 20.0) -> dict:
    try:
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        target_soft = min(max(soft, 8192), hard)
        if target_soft > soft:
            resource.setrlimit(resource.RLIMIT_NOFILE, (target_soft, hard))
    except Exception:
        pass

    run_env = {
        **env,
        'cjProcessorNum': '6',
        'cjHeapSize': heap,
        'cjGCInterval': '5ms',
        'MRT_REPORT': str(gc_log),
    }
    if extra_env:
        run_env.update(extra_env)
    MAX_WALL_SECONDS = max_wall_s   # 每轮最长等待时间；超时则强杀
    ZERO_CPU_LIMIT   = zero_cpu_limit_s  # CPU 持续为 0% 超过此秒数则判定为死锁，立即强杀

    proc = subprocess.Popen(
        [str(binary)], env=run_env,
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
    )
    pid = proc.pid
    t0  = time.monotonic()
    rss_ts, cpu_ts = [], []
    timed_out = False
    zero_cpu_since: float | None = None   # 开始连续 0% CPU 的时刻

    while proc.poll() is None:
        elapsed = round(time.monotonic() - t0, 2)
        if elapsed > MAX_WALL_SECONDS:
            print(f'  [WARN] run {run_idx} exceeded {MAX_WALL_SECONDS}s, killing pid={pid}',
                  flush=True)
            try:
                proc.kill()
            except Exception:
                pass
            timed_out = True
            break
        try:
            ps_out = subprocess.check_output(
                ['ps', '-o', 'rss=,pcpu=', '-p', str(pid)], text=True
            ).strip()
            if ps_out:
                parts = ps_out.split()
                cur_cpu = float(parts[1])
                rss_ts.append((elapsed, int(parts[0])))
                cpu_ts.append((elapsed, cur_cpu))
                # 检测 CPU 持续为 0 → 进程死锁
                if cur_cpu == 0.0 and elapsed > 5.0:
                    if zero_cpu_since is None:
                        zero_cpu_since = time.monotonic()
                    elif time.monotonic() - zero_cpu_since >= ZERO_CPU_LIMIT:
                        print(
                            f'  [WARN] run {run_idx} stuck (0% CPU for '
                            f'{ZERO_CPU_LIMIT:.0f}s), killing pid={pid}',
                            flush=True
                        )
                        try:
                            proc.kill()
                        except Exception:
                            pass
                        timed_out = True
                        break
                else:
                    zero_cpu_since = None
        except Exception:
            pass
        time.sleep(SAMPLE_INTERVAL)

    try:
        stdout, _ = proc.communicate(timeout=5)
    except Exception:
        stdout = b''
    wall_s = round(time.monotonic() - t0, 3)
    if timed_out:
        print(f'  [WARN] run {run_idx} killed after timeout; partial data may be recorded.',
              flush=True)

    # Save raw stdout next to GC log for post-mortem inspection
    stdout_log = gc_log.parent / f'stdout_{label.lower()}_run{run_idx}.txt'
    try:
        stdout_log.write_bytes(stdout)
    except Exception:
        pass

    subtests: dict[str, int] = {}
    total_ms = None
    phase2_parts = []

    for line in stdout.decode(errors='replace').splitlines():
        # Total wall time (all cases)
        m = re.search(r'Total wall time:\s*(\d+)\s*ms', line)
        if m:
            total_ms = int(m.group(1))
            continue

        if parse_style == 'bench':
            # [BENCH] P1_SmallAlloc 123 ms ...
            # [BENCH] F1_TreeIntegrity tag 123 ms ...
            # [BENCH] P1_SmallAlloc:  123 ms  (summary section)
            m = re.search(r'\[BENCH\]\s+([A-Z]\d_\w+?):?\s+(?:\w+\s+)?(\d+)\s+ms', line)
            if m:
                key = m.group(1).rstrip(':')
                val = int(m.group(2))
                # prefer SUMMARY section (last occurrence wins)
                subtests[key] = val

        elif parse_style == 'phase':
            # [PHASE1] ... NNN ms
            # [PHASE2] Round X/Y: ... NNN ms  (take last number = ms value)
            # [PHASE3] Verify time: NNN ms
            m = re.match(r'\[(PHASE\d+)\]', line)
            if m:
                tag = m.group(1)
                nums = re.findall(r'(\d+)\s*ms', line)
                if nums:
                    if tag == 'PHASE2':
                        phase2_parts.append(int(nums[-1]))
                    else:
                        subtests[tag] = int(nums[-1])

        elif parse_style == 'http_phase':
            # [PHASE] P1_1KB_HighFreq: count=NNN ... elapsed=NNNms throughput=NNNreq/s
            # [PHASE] P4_Mixed: count=NNN ... elapsed=NNNms throughput=NNNreq/s
            m = re.search(r'\[PHASE\]\s+(\S+?):\s+.*?throughput=(\d+)req/s', line)
            if m:
                tag = m.group(1)
                throughput = int(m.group(2))
                if tag in subtests_list:
                    subtests[tag] = throughput
            # [DONE] Benchmark total: NNNms
            m2 = re.search(r'\[DONE\].*total:\s*(\d+)\s*ms', line)
            if m2:
                total_ms = int(m2.group(1))

    if parse_style == 'phase' and phase2_parts:
        subtests['PHASE2'] = sum(phase2_parts)

    peak_rss = max((r for _, r in rss_ts), default=0)
    avg_rss  = int(sum(r for _, r in rss_ts) / len(rss_ts)) if rss_ts else 0
    peak_cpu = max((c for _, c in cpu_ts), default=0.0)

    return {
        'total_ms': total_ms,
        'wall_s':   wall_s,
        'subtests': subtests,
        'rss_ts':   rss_ts,
        'cpu_ts':   cpu_ts,
        'peak_rss': peak_rss,
        'avg_rss':  avg_rss,
        'peak_cpu': peak_cpu,
    }

# ─────────────────────────────────────────────────────────────────────────────
# 统计辅助
# ─────────────────────────────────────────────────────────────────────────────
def _stats_str(vals):
    vals = [v for v in vals if v is not None]
    if not vals:
        return 'N/A'
    if len(vals) == 1:
        return f'{vals[0]:.0f}'
    return f'min={min(vals):.0f}  avg={sum(vals)/len(vals):.0f}  max={max(vals):.0f}'

# ─────────────────────────────────────────────────────────────────────────────
# 绘图
# ─────────────────────────────────────────────────────────────────────────────
def plot_wall_time(results: dict, nruns: int, title: str, out: Path, opt_label: str):
    fig, ax = plt.subplots(figsize=(max(7, nruns * 2), 5))
    if nruns == 1:
        raw    = [results['MSGC'][0]['total_ms'], results['CopyGC'][0]['total_ms']]
        vals   = [v if v is not None else 0 for v in raw]
        colors = [COLORS['MSGC'], COLORS['CopyGC']]
        bars   = ax.bar(['MSGC', 'CopyGC'], vals, color=colors, width=0.4)
        for b, rv in zip(bars, raw):
            label_txt = f'{rv}ms' if rv is not None else 'OOM/N/A'
            ypos = b.get_height() + 10 if b.get_height() > 0 else 10
            ax.text(b.get_x() + b.get_width()/2, ypos, label_txt,
                    ha='center', va='bottom', fontsize=10)
        ax.set_ylabel('Wall Time (ms)')
    else:
        runs_x = list(range(1, nruns + 1))
        for label in ['MSGC', 'CopyGC']:
            ys = [r['total_ms'] if r['total_ms'] is not None else 0 for r in results[label]]
            ax.plot(runs_x, ys, marker='o', color=COLORS[label], label=label, linewidth=2)
            for x, y, r in zip(runs_x, ys, results[label]):
                lbl = f"{r['total_ms']}ms" if r['total_ms'] is not None else 'OOM/N/A'
                ax.annotate(lbl, (x, y), textcoords='offset points',
                            xytext=(0, 7), ha='center', fontsize=8, color=COLORS[label])
        ax.set_xlabel('Run #')
        ax.set_ylabel('Wall Time (ms)')
        ax.set_xticks(runs_x)
        ax.legend()
    ax.set_title(f'{title} — Total Wall Time ({opt_label}, {nruns} run{"s" if nruns > 1 else ""})')
    ax.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(out, dpi=150)
    plt.close(fig)
    print(f'[saved] {out.name}', flush=True)


def plot_subtest_compare(results: dict, subtests_list: list, title: str, out: Path, opt_label: str):
    """第一次 run 的子测试/阶段时间对比柱状图。"""
    labels = []
    msgc_vals, copygc_vals = [], []
    for st in subtests_list:
        m_val = results['MSGC'][0]['subtests'].get(st)
        c_val = results['CopyGC'][0]['subtests'].get(st)
        if m_val is not None or c_val is not None:
            labels.append(st.replace('_', '\n'))
            msgc_vals.append(m_val or 0)
            copygc_vals.append(c_val or 0)

    if not labels:
        return

    x = np.arange(len(labels))
    w = 0.35
    fig, ax = plt.subplots(figsize=(max(8, len(labels) * 1.5), 5))
    b1 = ax.bar(x - w/2, msgc_vals,   w, color=COLORS['MSGC'],   label='MSGC',   alpha=0.85)
    b2 = ax.bar(x + w/2, copygc_vals, w, color=COLORS['CopyGC'], label='CopyGC', alpha=0.85)
    for bars in [b1, b2]:
        for bar in bars:
            h = bar.get_height()
            if h > 0:
                ax.text(bar.get_x() + bar.get_width()/2, h + 5, f'{h:.0f}',
                        ha='center', va='bottom', fontsize=7)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=8)
    ax.set_ylabel('Time (ms)')
    ax.set_title(f'{title} — Subtest/Phase Time Comparison ({opt_label}, Run 1)')
    ax.legend()
    ax.grid(True, alpha=0.3, axis='y')
    plt.tight_layout()
    plt.savefig(out, dpi=150)
    plt.close(fig)
    print(f'[saved] {out.name}', flush=True)


def plot_rss_trend(results: dict, nruns: int, title: str, out: Path, opt_label: str):
    from matplotlib.lines import Line2D
    alphas = [1.0, 0.85, 0.70, 0.55, 0.40][:nruns]
    fig, ax = plt.subplots(figsize=(11, 5))
    for label in ['MSGC', 'CopyGC']:
        for i, r in enumerate(results[label]):
            ts = r['rss_ts']
            if not ts:
                continue
            xs = [t for t, _ in ts]
            ys = [v / 1024 for _, v in ts]
            ax.plot(xs, ys, color=COLORS[label], alpha=alphas[i],
                    linewidth=1.5 if i == 0 else 1.0)
    legend_els = [
        Line2D([0], [0], color=COLORS['MSGC'],   linewidth=2, label='MSGC'),
        Line2D([0], [0], color=COLORS['CopyGC'], linewidth=2, label='CopyGC'),
    ]
    ax.legend(handles=legend_els)
    ax.set_title(f'{title} — RSS Over Time ({opt_label}, {nruns} run{"s" if nruns > 1 else ""})')
    ax.set_xlabel('Elapsed (s)')
    ax.set_ylabel('RSS (MB)')
    ax.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(out, dpi=150)
    plt.close(fig)
    print(f'[saved] {out.name}', flush=True)


def plot_cpu_trend(results: dict, nruns: int, title: str, out: Path, opt_label: str):
    from matplotlib.lines import Line2D
    alphas = [1.0, 0.85, 0.70, 0.55, 0.40][:nruns]
    fig, ax = plt.subplots(figsize=(11, 5))
    for label in ['MSGC', 'CopyGC']:
        for i, r in enumerate(results[label]):
            ts = r['cpu_ts']
            if not ts:
                continue
            ax.plot([t for t, _ in ts], [c for _, c in ts],
                    color=COLORS[label], alpha=alphas[i],
                    linewidth=1.5 if i == 0 else 1.0)
    legend_els = [
        Line2D([0], [0], color=COLORS['MSGC'],   linewidth=2, label='MSGC'),
        Line2D([0], [0], color=COLORS['CopyGC'], linewidth=2, label='CopyGC'),
    ]
    ax.legend(handles=legend_els)
    ax.set_title(f'{title} — CPU Usage Over Time ({opt_label}, {nruns} run{"s" if nruns > 1 else ""})')
    ax.set_xlabel('Elapsed (s)')
    ax.set_ylabel('CPU (%)')
    ax.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(out, dpi=150)
    plt.close(fig)
    print(f'[saved] {out.name}', flush=True)


def plot_gc_cycle_time_bar(gc_stats: dict, nruns: int, title: str, out: Path, opt_label: str):
    fig, axes = plt.subplots(2, nruns, figsize=(4 * nruns, 8), sharey='row', squeeze=False)
    fig.suptitle(f'{title} — GC Per-Cycle Time ({opt_label}, each run)', fontsize=12, y=1.01)
    for row, label in enumerate(['MSGC', 'CopyGC']):
        for col in range(nruns):
            ax = axes[row][col]
            cycles = gc_stats[label][col]['cycle_us']
            if cycles:
                xs = list(range(1, len(cycles) + 1))
                ys = [c / 1000 for c in cycles]
                ax.bar(xs, ys, color=COLORS[label], alpha=0.75, width=0.8)
                ax.set_title(f'{label} Run {col+1}\n(n={len(cycles)})', fontsize=8)
            else:
                ax.text(0.5, 0.5, 'no data', ha='center', va='center',
                        transform=ax.transAxes, fontsize=8)
                ax.set_title(f'{label} Run {col+1}', fontsize=8)
            if col == 0:
                ax.set_ylabel('GC cycle time (ms)')
            ax.set_xlabel('GC #')
            ax.grid(True, alpha=0.2, axis='y')
    plt.tight_layout()
    plt.savefig(out, dpi=150, bbox_inches='tight')
    plt.close(fig)
    print(f'[saved] {out.name}', flush=True)


def plot_gc_count_total(gc_stats: dict, nruns: int, title: str, out: Path, opt_label: str):
    runs_x = np.arange(1, nruns + 1)
    bar_w  = 0.35
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))
    fig.suptitle(f'{title} — GC Count & Total Time ({opt_label}, each run)', fontsize=12)
    for i, label in enumerate(['MSGC', 'CopyGC']):
        counts  = [gc_stats[label][r]['gc_count'] for r in range(nruns)]
        totals  = [gc_stats[label][r]['total_gc_us'] / 1000 for r in range(nruns)]
        offset  = (i - 0.5) * bar_w
        bars1 = ax1.bar(runs_x + offset, counts, bar_w, color=COLORS[label], label=label, alpha=0.85)
        bars2 = ax2.bar(runs_x + offset, totals, bar_w, color=COLORS[label], label=label, alpha=0.85)
        for bar, v in zip(bars1, counts):
            ax1.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.3,
                     str(v), ha='center', va='bottom', fontsize=8)
        for bar, v in zip(bars2, totals):
            ax2.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.3,
                     f'{v:.0f}', ha='center', va='bottom', fontsize=7)
    for ax, ylabel, title_ in [
        (ax1, 'GC Count',        'GC Execution Count'),
        (ax2, 'Total GC (ms)',   'Total GC Time'),
    ]:
        ax.set_title(title_)
        ax.set_xlabel('Run #')
        ax.set_ylabel(ylabel)
        ax.set_xticks(runs_x)
        ax.legend()
        ax.grid(True, alpha=0.3, axis='y')
    plt.tight_layout()
    plt.savefig(out, dpi=150)
    plt.close(fig)
    print(f'[saved] {out.name}', flush=True)

# ─────────────────────────────────────────────────────────────────────────────
# 文字汇总
# ─────────────────────────────────────────────────────────────────────────────
def write_summary(results: dict, gc_stats: dict, nruns: int, heap: str,
                  subtests_list: list, title: str, out: Path, opt_label: str):
    lines = []
    lines.append('=' * 68)
    lines.append(f'  {title}: MSGC vs CopyGC  {opt_label}  对比汇总')
    subtest_unit = results.get('_subtest_unit', 'ms')
    proc_num = results.get('_proc_num', 6)
    lines.append(f'  heap={heap}  runs={nruns}  cjProcessorNum={proc_num}')
    lines.append(f'  MSGC SDK  : {MSGC_SDK}')
    lines.append(f'  CopyGC SDK: {COPYGC_SDK}')
    lines.append(f'  生成时间  : {datetime.now().strftime("%Y-%m-%d %H:%M:%S")}')
    lines.append('=' * 68)

    for label in ['MSGC', 'CopyGC']:
        runs = results[label]
        lines.append(f'\n{"─"*30}  {label}  {"─"*30}')
        totals    = [r['total_ms']         for r in runs]
        peak_rsss = [r['peak_rss'] // 1024 for r in runs]
        avg_rsss  = [r['avg_rss']  // 1024 for r in runs]
        peak_cpus = [r['peak_cpu']         for r in runs]
        gc_counts = [gc_stats[label][i]['gc_count']           for i in range(nruns)]
        gc_totals = [gc_stats[label][i]['total_gc_us'] / 1000 for i in range(nruns)]
        lines.append(f'  Total wall (ms)  : {_stats_str(totals)}')
        lines.append(f'  RSS peak  (MB)   : {_stats_str(peak_rsss)}')
        lines.append(f'  RSS avg   (MB)   : {_stats_str(avg_rsss)}')
        lines.append(f'  CPU peak  (%)    : min={min(peak_cpus):.1f}  '
                     f'avg={sum(peak_cpus)/len(peak_cpus):.1f}  max={max(peak_cpus):.1f}')
        lines.append(f'  GC count         : {_stats_str(gc_counts)}')
        lines.append(f'  GC total  (ms)   : {_stats_str(gc_totals)}')

        # per-run table
        lines.append('')
        hdr = f'  {"Run":>3}  {"Wall(ms)":>9}  {"RSS_pk(MB)":>11}  {"CPU_pk(%)":>10}'
        hdr += f'  {"GC_cnt":>7}  {"GC_tot(ms)":>11}'
        for st in subtests_list:
            hdr += f'  {st.split("_")[0]:>7}'
        lines.append(hdr)
        for i, r in enumerate(runs):
            gs  = gc_stats[label][i]
            row = (f'  {i+1:>3}  {str(r["total_ms"])+"ms":>9}  '
                   f'{str(r["peak_rss"]//1024)+"MB":>11}  '
                   f'{r["peak_cpu"]:>9.1f}%  '
                   f'{gs["gc_count"]:>7}  '
                   f'{gs["total_gc_us"]/1000:>10.0f}ms')
            for st in subtests_list:
                v = r['subtests'].get(st, '?')
                row += f'  {str(v):>7}'
            lines.append(row)

    # diff summary
    lines.append('\n' + '─' * 68)
    if nruns > 1:
        lines.append(f'  Run 2-{nruns} 均值对比（排除 run1 热身）')
    else:
        lines.append('  Run 1 对比汇总')
    lines.append('─' * 68)
    summary_row = {}
    for label in ['MSGC', 'CopyGC']:
        warm        = results[label][1:] if nruns > 1 else results[label]
        warm_offset = 1 if nruns > 1 else 0
        t    = [r['total_ms'] for r in warm if r['total_ms']]
        rss  = [r['peak_rss'] // 1024 for r in warm]
        gc_t = [gc_stats[label][warm_offset + i]['total_gc_us'] / 1000 for i in range(len(warm))]
        gc_c = [gc_stats[label][warm_offset + i]['gc_count']           for i in range(len(warm))]
        summary_row[label] = {
            'wall': sum(t) // len(t) if t else 0,
            'rss':  sum(rss) // len(rss) if rss else 0,
            'gc_t': sum(gc_t) / len(gc_t) if gc_t else 0.0,
            'gc_c': sum(gc_c) / len(gc_c) if gc_c else 0.0,
        }
        lines.append(f'  {label:<8}: wall={summary_row[label]["wall"]}ms  '
                     f'RSS_peak={summary_row[label]["rss"]}MB  '
                     f'GC_count={summary_row[label]["gc_c"]:.1f}  '
                     f'GC_total={summary_row[label]["gc_t"]:.0f}ms')

    m = summary_row['MSGC']['wall']
    c = summary_row['CopyGC']['wall']
    if c and m:
        diff = (m - c) / c * 100
        sign = '+' if diff >= 0 else ''
        lines.append(f'\n  Wall time diff: MSGC {sign}{diff:.1f}% vs CopyGC')

    # subtest comparison (run 1)
    lines.append('\n' + '─' * 68)
    lines.append('  子测试/阶段时间对比（Run 1）')
    lines.append('─' * 68)
    unit_lbl = subtest_unit
    hdr2 = f'  {"Subtest":<28}  {"MSGC("+unit_lbl+")": >12}  {"CopyGC("+unit_lbl+")": >14}  {"diff%":>8}'
    lines.append(hdr2)
    for st in subtests_list:
        m_val = results['MSGC'][0]['subtests'].get(st)
        c_val = results['CopyGC'][0]['subtests'].get(st)
        if m_val is not None and c_val is not None and c_val > 0:
            diff_pct = (m_val - c_val) / c_val * 100
            sign = '+' if diff_pct >= 0 else ''
            lines.append(f'  {st:<28}  {m_val:>12}  {c_val:>14}  {sign}{diff_pct:>7.1f}%')
        else:
            lines.append(f'  {st:<28}  {str(m_val):>12}  {str(c_val):>14}  {"N/A":>8}')

    text = '\n'.join(lines) + '\n'
    print('\n' + text, flush=True)
    out.write_text(text)
    print(f'[saved] {out.name}', flush=True)

# ─────────────────────────────────────────────────────────────────────────────
# 主流程
# ─────────────────────────────────────────────────────────────────────────────
def main():
    global MSGC_SDK, COPYGC_SDK, MSGC_VER, COPYGC_VER

    args    = parse_args()
    MSGC_SDK = resolve_sdk(args.msgc_sdk, 'MSGC')
    COPYGC_SDK = resolve_sdk(args.copygc_sdk, 'CopyGC')
    MSGC_VER = _ver(MSGC_SDK)
    COPYGC_VER = _ver(COPYGC_SDK)
    cfg     = CASE_CONFIGS[args.case]
    opt_tag = args.opt
    opt_flag = f'-{opt_tag}'
    opt_suffix = opt_tag.lower()
    heap    = args.heap or cfg['heap']
    nruns   = args.runs
    title   = cfg['title']
    sub_lst = cfg['subtests']
    pstyle  = cfg['parse']
    max_wall_s   = cfg.get('max_wall_s', 180)
    zero_cpu_limit_s = cfg.get('zero_cpu_limit_s', 20.0)
    case_extra   = dict(cfg.get('case_extra_env', {}))
    # --proc 覆盖 cjProcessorNum（对 MSGC 和 CopyGC 均生效）
    case_extra['cjProcessorNum'] = str(args.proc)
    # --json-file 覆盖 CASE17_JSON_FILE（对 MSGC 和 CopyGC 均生效）
    if args.json_file:
        case_extra['CASE17_JSON_FILE'] = str(Path(args.json_file).expanduser().resolve())
    msgc_port    = cfg.get('msgc_port')
    copygc_port  = cfg.get('copygc_port')
    subtest_unit = cfg.get('subtest_unit', 'ms')

    # --extra-env KEY=VAL  → override MSGC run env
    args_extra = {}  # type: dict
    for kv in args.extra_env:
        if '=' in kv:
            k, _, v = kv.partition('=')
            args_extra[k.strip()] = v.strip()
        else:
            print(f'[WARN] --extra-env ignored (no =): {kv}', file=sys.stderr)

    case_dir = RESULT_ROOT / args.case
    src      = case_dir / 'benchmark.cj'
    if not src.exists():
        print(f'[ERROR] {src} not found', file=sys.stderr)
        sys.exit(1)

    # 准备输出目录
    ts_tag     = datetime.now().strftime('%Y%m%d_%H%M%S')
    result_name = f'{ts_tag}_MSGC_{MSGC_VER}_vs_CopyGC_{COPYGC_VER}_{opt_tag}'
    result_dir  = case_dir / 'result' / result_name
    result_dir.mkdir(parents=True, exist_ok=True)
    print(f'\nResult dir: {result_dir}', flush=True)

    # 加载 SDK 环境
    print('\nLoading SDK environments...', flush=True)
    envs = {
        'MSGC':   load_sdk_env(MSGC_SDK),
        'CopyGC': load_sdk_env(COPYGC_SDK),
    }
    bins = {
        'MSGC':   result_dir / f'bench_msgc_{opt_suffix}',
        'CopyGC': result_dir / f'bench_copygc_{opt_suffix}',
    }
    print('  done.', flush=True)

    # 编译
    if not args.skip_build:
        print('\n=== Compile ===', flush=True)
        compile_binary(MSGC_SDK,   envs['MSGC'],   src, bins['MSGC'], opt_flag)
        compile_binary(COPYGC_SDK, envs['CopyGC'], src, bins['CopyGC'], opt_flag)
    else:
        result_root = case_dir / 'result'
        prev_bins: dict = {}
        if result_root.exists():
            for d in sorted(result_root.iterdir(), reverse=True):
                if not d.is_dir() or d == result_dir:
                    continue
                m = d / f'bench_msgc_{opt_suffix}'
                c = d / f'bench_copygc_{opt_suffix}'
                if m.exists() and c.exists():
                    prev_bins = {'MSGC': m, 'CopyGC': c}
                    break
        if not prev_bins:
            print('[ERROR] No compiled binaries found. Run without --skip-build first.', file=sys.stderr)
            sys.exit(1)
        bins = prev_bins
        print(f'  [skip-build] using binaries from: {list(prev_bins.values())[0].parent}', flush=True)

    # 运行
    all_results: dict = {'MSGC': [], 'CopyGC': [], '_proc_num': args.proc}
    gc_stats:    dict = {'MSGC': [], 'CopyGC': []}
    gc_parsers = {'MSGC': parse_gc_log_msgc, 'CopyGC': parse_gc_log_copygc}
    port_env_key = cfg.get('port_env_var', 'CASE9_SERVER_PORT')

    for label in ['MSGC', 'CopyGC']:
        print(f'\n=== {label} {opt_tag} ({nruns} run{"s" if nruns > 1 else ""}) ===', flush=True)
        for i in range(1, nruns + 1):
            gc_log = result_dir / f'gc_{label.lower()}_run{i}.log'
            # Build per-run extra env: case-level → label-specific port → MSGC CLI overrides
            extra = dict(case_extra)
            if label == 'MSGC' and msgc_port:
                extra[port_env_key] = str(int(msgc_port) + i - 1)
            elif label == 'CopyGC' and copygc_port:
                extra[port_env_key] = str(int(copygc_port) + i - 1)
            if label == 'MSGC':
                extra.update(args_extra)
            # 清理端口上的残留进程（防止 UE 进程占用端口）
            _port = extra.get(port_env_key)
            if _port:
                _try_free_port(int(_port))
            r = run_once(label, i, bins[label], envs[label], heap, gc_log, pstyle, sub_lst,
                         extra, max_wall_s, zero_cpu_limit_s)
            all_results[label].append(r)
            gs = gc_parsers[label](gc_log)
            gc_stats[label].append(gs)

            sub_str = '  '.join(
                f'{k.split("_")[0]}={r["subtests"].get(k, "?")}{subtest_unit}'
                for k in sub_lst
            )
            print(
                f'  Run {i}: wall={r["total_ms"]}ms  '
                f'RSS_pk={r["peak_rss"]//1024}MB  CPU_pk={r["peak_cpu"]:.1f}%  '
                f'GC_cnt={gs["gc_count"]}  GC_tot={gs["total_gc_us"]/1000:.0f}ms',
                flush=True
            )
            print(f'         {sub_str}', flush=True)

    # 生成图表
    print('\n=== Generating charts ===', flush=True)
    plot_wall_time(all_results, nruns, title, result_dir / 'wall_time.png', opt_tag)
    plot_subtest_compare(all_results, sub_lst, title, result_dir / 'subtest_compare.png', opt_tag)
    plot_rss_trend(all_results, nruns, title, result_dir / 'rss_trend.png', opt_tag)
    plot_cpu_trend(all_results, nruns, title, result_dir / 'cpu_trend.png', opt_tag)
    plot_gc_cycle_time_bar(gc_stats, nruns, title, result_dir / 'gc_cycle_time_bar.png', opt_tag)
    plot_gc_count_total(gc_stats, nruns, title, result_dir / 'gc_count_total.png', opt_tag)

    # 文字汇总
    all_results['_subtest_unit'] = subtest_unit
    write_summary(all_results, gc_stats, nruns, heap, sub_lst, title,
                  result_dir / 'summary.txt', opt_tag)

    print(f'\n=== Done. Results in:\n  {result_dir}\n', flush=True)


if __name__ == '__main__':
    main()
