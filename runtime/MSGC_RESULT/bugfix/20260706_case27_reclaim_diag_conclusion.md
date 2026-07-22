# 2026-07-06 case27 reclaim 诊断与根因再收敛

## 结论

针对

- `MSGC_RESULT/Result/case27_http_complex_json/result/20260706_123120_case27_validate_c1000_p100K_60s_heap4gb_5runs/run3`

里这次

- `RegisterLargeObjectMS: span[1/4] ... regionType=3(FULL)`

的 abort，当前可以进一步收敛为：

1. `RegisterLargeObjectMS` 报错本身是**后续症状**，不是第一现场。
2. `freeManager reclaim` 目前**没有证据**表明是第一触发点。
3. 当前更高置信的根因仍然是 **LLVM/AArch64 MSGC inline fastpath**，尤其是
   `WriteRefFieldFast_MSGC` / `WriteStaticRefFast_MSGC` 这类 ref 写路径。

## 本轮做了什么

在 runtime 里新增了一个 env-gated 诊断：

- `CJ_MS_DIAG_VERIFY_RECLAIM_FREE=1`

作用：

- 在 span / region 退回 `FreeManager` 之前，检查对应 `MSRegionInfo` 是否已经是 `FREE`。
- 如果不是，就直接 FATAL，打印 reclaim 来源和 span 信息。

相关代码：

- `src/Heap/MAllocator/MSRegionManager.cpp`
- `src/Heap/MAllocator/MSRegionManager.h`
- `src/Heap/MAllocator/MSCentralRegionPool.cpp`

## 关键复现

目录：

- `MSGC_RESULT/Result/case27_http_complex_json/result/20260706_130514_reclaim_diag_c1000_p100K_60s_heap4gb_1run`

### run1

- `rc=0`
- `[RESULT] total=57652 ok=57651 fail=1 throughput=946req/s`
- 未命中 reclaim 诊断

### run2

- `rc=139`
- `run2.stderr.txt` 为 managed `SIGSEGV`
- 崩溃栈：
  - `benchmark.cj::__CN7default14processRequest...`
  - `stdx.net.http` 请求处理路径
- `gc_run2.log` 只有 **62 行**
- 进程在 **第 1 个 GC 周期** 中就崩了：
  - 已进入 `ConcurrentMark`
  - 还**没有进入 sweep/reclaim**

这点非常关键：

- reclaim 诊断完全**没有**命中
- crash 发生在 **sweep/reclaim 之前**

因此：

- `freeManager` 把 non-FREE region 直接 reclaim 回去，不是当前最先发生的错误
- `RegisterLargeObjectMS` 的 overlap / abort 更像是**后续状态损坏后的二次表现**

## 与 benchmark / 编译产物的对应关系

同一份 fresh benchmark 二进制里，仍然能看到：

- `CJ_MCC_WriteRefFieldFast_MSGC`
- `CJ_MCC_WriteStaticRefFast_MSGC`

命令：

```bash
nm -m benchmark | rg 'CJ_MCC_(WriteRefField|WriteStaticRef).*MSGC'
```

而 `benchmark.cj` 的 `processRequest` 热路径里正好大量执行：

- `root.put(...)`
- `co.put(...)`
- `item.put(...)`
- `ao.put(...)`

这正是高频 ref 写场景。

## 与既有结论的关系

这次结果与以下历史结论一致，而不是冲突：

- `MSGC_RESULT/bugfix/20260630_case27_ref_fastpath_rootcause.md`
  - 已把 case27 高置信主嫌收敛到
    `WriteRefFieldFast_MSGC` / `WriteStaticRefFast_MSGC`
- `MSGC_RESULT/bugfix/20260703_case27_arraycopy_tryemit_removed_sigv_status.md`
  - 移除部分 MSGC inline fastpath 后，SIGV 症状显著下降

## 当前最合理的解释

更符合现有证据的解释是：

1. 先发生的是 **compiler inline fastpath 导致的对象图/屏障语义问题**，或由此引出的对象状态损坏。
2. 之后 runtime 才出现：
   - managed `SIGSEGV`
   - 或更晚一些的 allocator / region metadata 异常
   - 最终在某些 run 上表现成 `RegisterLargeObjectMS` overlap abort

换句话说：

- `RegisterLargeObjectMS span[1/4] non-free(FULL)` 是**重要现场**
- 但它更像“损坏已经扩散到 allocator 元数据之后的暴露点”
- 不是当前最该优先修的第一根因

## 下一步建议

1. 优先回到 LLVM/AArch64 MSGC inline fastpath：
   - `WriteRefFieldFast_MSGC`
   - `WriteStaticRefFast_MSGC`
2. 用当前 `compiler/output` 继续做一次最小对照：
   - 把 ref/static-ref fastpath 再次切 slow-probe
   - 观察 case27 `c1000 + p100K + 60s + heap=4gb` 是否再次压住 `SIGSEGV`
3. reclaim 诊断代码建议暂时保留：
   - 它能帮助判断以后再出现 allocator overlap 时，是否真的是 reclaim 先做错

