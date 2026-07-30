# MSGC SDK 构建、安装与用例验证

> 当前代码基线：Region-scan Mark，runtime 提交 `24ca097ace`。本文统一使用 `output/msgc` SDK；旧结果不代表当前版本。

## 1. 统一路径

macOS aarch64 当前合同：

```bash
export CJ_WORK=/Users/lipeisheng/code/cangjie/cangjie
export COMPILER_ROOT=$CJ_WORK/cangjie_compiler_region_mark
export RUNTIME_ROOT=$CJ_WORK/cangjie_runtime_region_mark/runtime
export STDLIB_ROOT=$CJ_WORK/cangjie_runtime_region_mark/stdlib
export STDX_ROOT=$CJ_WORK/cangjie_stdx

export MSGC_SDK_ROOT=$COMPILER_ROOT/output/msgc
export MSGC_STDX_ROOT=$MSGC_SDK_ROOT/darwin_aarch64_cjnative
export MSGC_STDX_DIR=$MSGC_STDX_ROOT/dynamic/stdx
```

必须统一：

- SDK：`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler_region_mark/output/msgc`
- stdx platform root：`.../output/msgc/darwin_aarch64_cjnative`
- stdx dylib dir：`.../output/msgc/darwin_aarch64_cjnative/dynamic/stdx`

用例脚本中 `MSGC_STDX_ROOT` 通常表示 platform root；`STDX_DIR` 才表示 `dynamic/stdx`。

## 2. 完整构建

推荐从 runtime 根目录使用统一 builder：

```bash
cd "$RUNTIME_ROOT"

python3 build/scripts/rebuild_msgc_sdk.py \
  --compiler-root "$COMPILER_ROOT" \
  --runtime-root "$RUNTIME_ROOT" \
  --stdlib-root "$STDLIB_ROOT" \
  --stdx-root "$STDX_ROOT"
```

builder 将 MSGC 输出约束到 `$COMPILER_ROOT/output/msgc`。实际可用参数以：

```bash
python3 build/scripts/rebuild_msgc_sdk.py --help
```

为准。正式构建前可先运行相关脚本单测，不应靠旧文档猜参数。

## 3. 增量构建

### 3.1 Runtime、stdlib、stdx

保留 compiler output，只重建其余组件：

```bash
python3 build/scripts/incremental_rebuild_msgc_sdk.py
```

该脚本的目标 SDK同样是 `output/msgc`。

### 3.2 只重建 Runtime并安装进 SDK

```bash
python3 build/scripts/rebuild_msgc_runtime_sdk.py
```

底层实现由 `rebuild_msgc_runtime_output.py` 完成，包含：

- marksweep mode构建；
- install；
- overlay 到 SDK；
- macOS dylib codesign；
- 关键 runtime/static/start object检查。

如需看参数：

```bash
python3 build/scripts/rebuild_msgc_runtime_output.py --help
```

## 4. 产物检查

```bash
test -f "$MSGC_SDK_ROOT/envsetup.sh"
test -x "$MSGC_SDK_ROOT/bin/cjc"
test -d "$MSGC_STDX_DIR"
test -f "$MSGC_SDK_ROOT/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib"
```

干净 shell：

```bash
env -i HOME="$HOME" TERM="$TERM" \
  PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin \
  MSGC_SDK_ROOT="$MSGC_SDK_ROOT" \
  bash -c 'source "$MSGC_SDK_ROOT/envsetup.sh" && cjc -v'
```

macOS 手工替换 dylib后必须 ad-hoc codesign。更推荐使用 builder，避免漏同步 static runtime、
start objects 或 stdlib/stdx。

## 5. 路径合同测试

当前仓库提供：

```bash
python3 -m unittest build/scripts/test_msgc_sdk_path_defaults.py
```

它检查 macOS case脚本是否默认指向：

```text
cangjie_compiler_region_mark/output/msgc
output/msgc/darwin_aarch64_cjnative
```

构建脚本本身还有 `test_full_sdk_builder.py`、
`test_rebuild_msgc_runtime_output.py` 等单测。

## 6. 用例清单

| 目录 | 推荐入口 |
|---|---|
| `case25_http_complex_json` | `run.sh` |
| `case26_http_complex_json` | `run_compare.sh` / `run_compare_portsafe.sh` |
| `case27_http_complex_json` | `run_compare.sh` / `run_compare_portsafe.sh` |
| `case27_http_complex_json_msgc_high_p` | `run_msgc_high_p.sh` / `run_compare_all_modes.sh` |
| `case28_exception_ffi_boundary` | 按目录现有构建/运行说明 |
| `case28_http_complex_json` | `run_compare.sh` |
| `case29_http_complex_json` | `run_case29.sh` |
| `case30_31_http_external_client` | `run_external_case.sh` |
| `case30_http_complex_json` | `run_case30.sh` |
| `case31_http_complex_json` | `run_case31.sh` |

相对路径均在 [`MSGC_RESULT/Result`](../Result/) 下。

## 7. 推荐验证顺序

### 7.1 冒烟

1. path contract unit test；
2. `cjc -v`；
3. 编译一个最小 MSGC程序；
4. `DYLD_PRINT_LIBRARIES=1` 确认加载 `$MSGC_SDK_ROOT` 的 runtime；
5. case25/case27 小 payload、低并发、短 duration。

### 7.2 Region-scan Mark正确性

启用当前代码支持的 verifier/DFX，重点观察：

- task ledger 是否归零；
- REGION/OBJECT ready lane 是否清空；
- stranded pending summary；
- non-IDLE Region；
- MARK & ~SCAN residual；
- reachable-unmarked edge；
- process exit code、client fail、server异常日志。

### 7.3 压力

- case27 payload/concurrency矩阵；
- high-P；
- case28 bulk/exception/FFI；
- case29–31 外部 client；
- 长时间循环与 GC频率/RSS采样。

## 8. 环境隔离

不要在同一交互 shell连续 source MSGC 与 CopyGC SDK。每一侧使用独立 `env -i`，并记录：

- SDK root；
- stdx root/dir；
- `cjc -v`；
- runtime dylib path与 hash；
- git revision；
- payload/concurrency/duration；
- heap/GC参数；
- result directory。

每次 SDK、runtime、stdlib 或 stdx变化后都必须重新执行 `cjc`，不要复用历史 binary。

## 9. 结果口径

仓库中的 2026-07-12/24 等结果来自旧 revision。它们可以用于：

- 理解用例；
- 检查报告格式；
- 观察历史趋势。

不能用于宣称 `24ca097ace` Region-scan 的吞吐、RSS、GC时间或成功率。

新结果目录至少保存：

```text
run_metadata.env
stdout/stderr
exit status
rss/cpu samples
runtime hash
source revision
benchmark/source snapshot
compare_report.md
```

## 10. 判定标准

先判有效性，再判性能：

1. 编译成功且使用目标 SDK。
2. 动态加载来源正确。
3. server/client exit status可解释。
4. 输出存在最终 RESULT。
5. fail/timeout为0或有明确原因。
6. verifier没有 Mark完整性错误。
7. RSS/CPU采样完整。
8. 同参数重复运行结果稳定后再比较。

## 11. 常见错误

- 把 `$COMPILER_ROOT/output` 当成当前 MSGC SDK；当前是 `output/msgc`。
- 把 `dynamic/stdx` 传给需要 platform root 的 `MSGC_STDX_ROOT`。
- 只复制一份 dylib，未同步 compiler/runtime/stdlib/stdx。
- dylib替换后未 codesign。
- 在污染 shell 中对比两套 SDK。
- 使用旧 binary或旧 result宣称新 Mark效果。
- 只看吞吐，不看 exit/fail/verifier。

## 12. 本文验证范围

本次文档更新只核对了源码、构建脚本默认值与用例入口，没有执行完整 SDK构建、安装或压力
用例。因此本文不新增任何“当前 Region-scan 已通过”结论。
