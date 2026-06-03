# 2026-05-28 LLVM MSGC NewObject FastPath 移除 alloc-mark 记录

## 目标

- 将 MSGC AArch64 new object fast path 的“分配时 mark”去掉。
- 统一为“不在分配点做 mark，完全依赖写屏障闭包新边”。
- 重新组装新的 compiler `output/` 包，并用这套包验证 `case2_50x_pressure`。

## 本次改动范围

### 1. LLVM 侧改动

改动文件位于编译器仓的 LLVM fork：

- `cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp`

本次删除了 AArch64 MSGC fast path 中用于 alloc-black 的逻辑，包括：

- static fast path 的 `EmitMarkNewObjectIfNeeded` helper
- dynamic fast path 的 `EmitMarkNewObjectIfNeeded` helper
- static PARTIAL / BUMP 两处调用点
- dynamic PARTIAL / BUMP 两处调用点
- 清理因此变成未使用的 MSGC phase / mark bitmap 常量

结果是：AArch64 MSGC fast allocation 不再在分配点内联 `MarkNewObject`，而是仅完成分配，后续对象图闭包完全交给写屏障路径。

### 2. compare 脚本改动

为了让 `MSGC_RESULT/Result/script/compare.py` 可以直接对比新组装的 `compiler/output` 包，本次给脚本增加了两个可选参数：

- `--msgc-sdk`
- `--copygc-sdk`

这样可以不改脚本常量，直接指定：

- MSGC = `cangjie_compiler/output`
- CopyGC = 官方 SDK

## 构建与组包过程

### 1. 增量重编 LLVM

没有走整条 compiler 全量链路，而是直接在 LLVM build tree 做定点重编：

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/build/build/third_party/cjnative-prefix/src/cjnative-build
ninja lib/libLLVM.dylib bin/llc
```

这一步用于快速验证 `AArch64AsmPrinter.cpp` 改动是否可编译。

### 2. 同步到 compiler output

`build.py install` 没有自动覆盖 `output/third_party/llvm/lib/libLLVM.dylib`，因此本次采用手工同步：

```bash
cp <build-tree>/lib/libLLVM.dylib \
  /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/third_party/llvm/lib/libLLVM.dylib

cp <build-tree>/bin/llc \
  /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/third_party/llvm/bin/llc
```

同步后在 macOS arm64 上必须重新签名：

```bash
codesign -f -s - /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/third_party/llvm/lib/libLLVM.dylib
codesign -f -s - /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/third_party/llvm/bin/llc
```

### 3. runtime install 与 stdlib 重建

本轮真正的非 LLVM 阻塞点不是编译失败，而是 stdlib 链接 `libstdFFI.dylib` 时找不到：

- `runtime/output/common/darwin_release_aarch64/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib`

因此先执行 runtime install，补齐 stdlib 期望的目录结构，再重跑 stdlib：

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime
./.venv/bin/python build.py install
./.venv/bin/python build.py install --prefix /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output
```

然后重建 stdlib：

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/stdlib
./.venv/bin/python build.py clean
./.venv/bin/python build.py build -t release \
  --target-lib=/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/output \
  --build-args=-Woff --build-args=deprecated \
  --build-args=-Woff --build-args=unused
./.venv/bin/python build.py install
```

### 4. 组装新 output 包

将 stdlib 产物并入 compiler output：

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler
rsync -a /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/stdlib/output/ output/
find output -path '*/darwin_aarch64_cjnative/*.dylib' -print0 | xargs -0 -I{} codesign -f -s - '{}'
```

组包后确认至少具备以下关键产物：

- `output/bin/cjc`
- `output/third_party/llvm/lib/libLLVM.dylib`
- `output/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib`
- `output/runtime/lib/darwin_aarch64_cjnative/libcangjie-std-core.dylib`

## 编译器冒烟验证

在新的 `output/` 包下重新编译最小 `hello.cj`，运行输出 `Hello, Cangjie`，说明：

- 新 LLVM 已被 `output/bin/cjc` 消费
- 运行时与标准库装配可用

## case2 手工验证

先用新包单独编译并运行 `case2_50x_pressure/benchmark.cj`，结果：

- 进程正常退出
- `Functional: 5/5 passed`
- `F5_HashMap` 通过

说明“去掉 fastpath alloc-mark”后，当前新包至少能稳定通过 case2 的正确性验证。

## case2 SDK 对比

### 对比命令

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result

python3 script/compare.py \
  --case case2_50x_pressure \
  --msgc-sdk /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output \
  --copygc-sdk /Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie \
  --runs 1 \
  --proc 6 \
  --opt O1
```

对比产物目录：

```text
/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case2_50x_pressure/result/20260528_002954_MSGC_cangjie_compiler_vs_CopyGC_20260507_O1
```

### 对比结果

| 指标 | MSGC(out SDK) | CopyGC(20260507) |
|------|---------------|------------------|
| Total wall time | 115543 ms | 131639 ms |
| RSS peak | 2741 MB | 429 MB |
| GC count | 1503 | 2810 |
| GC total time | 5664 ms | 67315 ms |

子测试结果：

| 子测试 | MSGC | CopyGC | 结论 |
|-------|------|--------|------|
| F1_TreeIntegrity | 2479 ms | 3178 ms | MSGC 快 22.0% |
| F2_Generational | 8668 ms | 7313 ms | MSGC 慢 18.5% |
| F3_LargeObject | 33784 ms | 38733 ms | MSGC 快 12.8% |
| F4_LinkedList | 6269 ms | 5873 ms | MSGC 慢 6.7% |
| F5_HashMap | 9279 ms | 10845 ms | MSGC 快 14.4% |
| P1_SmallAlloc | 3877 ms | 5954 ms | MSGC 快 34.9% |
| P2_LargeAlloc | 17643 ms | 21939 ms | MSGC 快 19.6% |
| P3_MixedWorkload | 11416 ms | 14333 ms | MSGC 快 20.4% |
| P4_LongLivedChurn | 8906 ms | 10920 ms | MSGC 快 18.4% |
| P5_Fragmentation | 13218 ms | 12547 ms | MSGC 慢 5.3% |

### 结论

1. 新的 out SDK 在 case2 上功能正确，`5/5 passed`。
2. 在本轮 O1、`cjProcessorNum=6`、`cjHeapSize=4gb` 条件下，MSGC(out SDK) 总耗时比 CopyGC 快 12.2%。
3. MSGC 的 GC 次数和 GC 总耗时都显著低于 CopyGC，但 RSS 峰值明显更高，这是当前结果里最需要继续观察的代价项。
4. 去掉 fastpath alloc-mark 之后，至少在 case2 上没有引入正确性回退，且性能结果仍然具备竞争力。

## 当前建议

如果后续继续推进“完全依赖写屏障、分配点不做 mark”的方向，建议优先做两件事：

1. 用同一套 out SDK 把 case1 到 case6 全量回归一遍，确认不是仅 case2 成立。
2. 专门分析 MSGC 在 case2 上 RSS 峰值显著高于 CopyGC 的原因，区分是堆策略、回收时机还是统计口径差异。