# Step41: MSGC bitmap 统计替代 markedObjectCount 热原子

## 问题

MSGC 普通 tracing 每次赢得 mark bitmap CAS 后都会更新继承自 `TracingCollector` 的
`markedObjectCount`。多 GC worker 与 mutator assist 并发写同一个原子 cache line，导致高频
cache-line bouncing。case27 `c1000/p100K/60s` 基线中累计标记约 10.89 亿个对象，ConcurrentMark
平均 343.01ms、总计 48.36s。

`markedObjectCount` 不参与 MSGC 的存活、收敛或 sweep 正确性；它只用于下一轮 assist ratio 和日志。
真正的存活依据是 region mark bitmap。

## 实现

- 新增 `MSMarkedStats`，同时保存 `objectCount` 与 `liveBytes`。
- 将 `ComputeMarkedBytes()` 改为 `ComputeMarkedStats()`：复用原有 bitmap popcount 遍历，small/medium
  的 popcount 同时累计对象数和 `count * slotSize`；每个 marked large primary 计一个对象并累计 span。
- `ConcurrentSweep()` 在 mark bitmap 稳定窗口一次得到两个统计，并把 bitmap objectCount 保存到
  `prevCycleMarkedObjects_`，继续驱动下一轮 assist ratio。
- 删除 MSGC 中所有 `markedObjectCount` 读写，包括普通 discover、write-barrier log publish、raw log
  consume、ConcurrentMark 日志以及 MarkTermination delta 统计。
- `ConcurrentMark` 日志保留各执行方 processed 数；精确 marked 数改由周期末
  `Mark bitmap: objects=N liveBytes=B` 输出。
- CopyGC/TracingCollector 自身的 `markedObjectCount` 保持不变，本次只移除 MSGC 路径。

## LLVM 核查

AArch64 MSGC inline 写屏障不存在 `markedObjectCount` 或等价全局计数更新。LLVM 只执行：

- mark bitmap 原子 OR/CAS；
- 仅 mark winner 写入 `activeLog_->objs[len++]`；
- buffer 满时回 runtime slow/publish 路径。

其中 `activeLog_->len` 是 WorkBuf 内容长度和发布正确性字段，不是 GC 统计，必须保留。因此 LLVM
不需要代码修改；原计数全部位于 runtime，现已从 MSGC 路径删除。

## 验证

- `MSMarkedStatsTest` 完成 RED/GREEN，覆盖 small/medium slot、零位图和 large object 聚合。
- WorkBuf、worker policy、worker control 三组既有测试通过。
- Release MarkSweep 增量构建通过，安装并 codesign 到 compiler `output`。
- case27 替换 runtime 后使用 output compiler/runtime/stdlib 重新编译；dyld 路径确认正确。
- 5 秒短测 76 个相邻周期中，上一轮 bitmap objectCount 与下一轮 `prevMarked` 全部一致，0 mismatch。
- 60 秒 `c1000/p100K`：`46265/46265` 成功，吞吐 `751 req/s`，无 runtime fatal/OOM。

## 性能结果

与 `20260711_224830` 的 MSGC 基线相比：

| 指标 | 基线 | bitmap 统计 | 变化 |
| --- | ---: | ---: | ---: |
| 吞吐 | 388 req/s | 751 req/s | +93.6% |
| ConcurrentMark 平均 | 343.01ms | 52.19ms | -84.8% |
| ConcurrentMark 总时间 | 48.36s | 30.01s | -38.0% |
| GC 平均 | 367.65ms | 73.97ms | -79.9% |
| GC 总时间 | 51.84s | 42.53s | -18.0% |
| 单请求 GC 时间 | 2.162ms | 0.919ms | -57.5% |
| GC 次数 | 141 | 575 | +307.8% |

GC 次数增加主要来自吞吐接近翻倍后分配速率上升，以及现有 HEU threshold/retained-footprint 口径
不一致。该问题会放大每轮固定 STW/Enum 成本，后续应通过 retained baseline + allocation headroom
修正 pacer，而不是恢复逐对象计数。
