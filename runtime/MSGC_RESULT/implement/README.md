# MSGC 分配实现文档索引

## 目录结构

```
implement/
├── README.md                        ← 本文件（索引）
├── 01_MSGC_Runtime_分配实现.md      ← Runtime 侧：AllocBufferMS、SizeClass、FastAlloc/SlowAlloc
├── 02_LLVM_编译器优化实现.md        ← 编译器侧：CJBarrierLowering、AArch64AsmPrinter、fast path 生成
└── 03_有优化与无优化分配路径对比.md  ← 三条路径完整对比（汇编级别 + 性能分析）
```

## 快速导航

| 问题 | 查哪里 |
|------|--------|
| AllocBufferMS 内部结构、FastAlloc/SlowAlloc 算法 | `01_MSGC_Runtime_分配实现.md` |
| MSGC size-class 是怎么划分的？ | `01_MSGC_Runtime_分配实现.md §二` |
| 为什么 AllocBufferMS 第一个字段是 tlRegion_？ | `01_MSGC_Runtime_分配实现.md §五`，`03_...对比 §二` |
| cjc 编译时如何决定调哪个 wrapper 函数？ | `02_LLVM_编译器优化实现.md §二` |
| MSGC fast path 汇编是什么样的？ | `02_LLVM_编译器优化实现.md §三` |
| CopyGC 风格 fast path 的汇编是什么样的？ | `02_LLVM_编译器优化实现.md §三` |
| SDK2 的 fast path 在 MSGC 下是否真正执行？ | `03_...对比 §二`（永远 miss） |
| SDK1 vs SDK2 性能差异的根本原因 | `03_...对比 §五` |
| MSGC 完整分配路径（端到端）| `03_...对比 §六` |

## 关键结论速记

1. **SDK2 的 `CJ_MCC_NewObjectFast` 在 MSGC 下永远 miss**：MSGC runtime 刻意将 `AllocBufferMS::tlRegion_` 设为 `NullRegion()`（allocPtr=regionEnd=0），7 条 inline 指令完全无效，每次都 fallback 到 `_CJ_MCC_NewObject`。

2. **SDK1 的 MSGC size-class 特化 fast path 真正命中**：在 GC 空闲（IDLE/SWEEP）且有活跃 Region 时，约 12-25 条指令完成分配，不经过 CalleeSavedStub 的寄存器保存。

3. **SDK2 反而比 SDK1 快（case1）**：bitmap 60条指令 inline 带来 icache 压力，而 case1 批量顺序分配对 bump pointer 更友好。SDK1 bitmap 优势在 P5 碎片化场景才体现（31ms vs 36ms）。

4. **读屏障消除（SDK1/SDK2 均有）**：`EnableMSGCReadBarrierLowering` 消除不必要的 `cj_gcread_*` intrinsic，与分配路径无关。
