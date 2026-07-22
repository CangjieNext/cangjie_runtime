# MSGC Scavenge 两阶段 OS Release 修复记录

## 基本信息

- 标题：Scavenge 锁内 OS 操作与失败误发布修复
- 日期：2026-07-20
- 提交/分支：当前 MSGC 工作树
- 平台：macOS aarch64；实现同时覆盖 Windows/Linux 返回值

## 现象与复现

- 现象：普通 Scavenge 和完整 Segment release 在 owner shard 锁内执行 `MemorySet/mmap`、`madvise` 或 `VirtualFree`，同 shard 分配可能长时间等待。
- 最小复现步骤：在 `ReleaseUnits()` 的 macOS `MemorySet` 中尝试获取当前 shard mutex；修改前获取失败。
- 实际结果：OS release 期间 shard 锁被占用；`mmap` 失败后调用方仍设置 scavenged bit并累计 released bytes。
- 期望结果：OS release 不持有 shard 锁；只有成功后发布 scavenged，失败恢复 hot-free。

## 定位过程

- 已排除项：瓶颈不在 shard `try_lock` 本身，而在获取成功后的临界区长度。
- 关键函数/调用链：`ScavengeFreeRegionsByRegionBudget()`、`ReleaseCompletelyFreeSegment()`、`ReleaseUnits()`、`ShrinkHeap()`。
- 关键状态：修改前只有 alloc/scavenged 两个位图，没有表示“已 claim、尚未完成 OS release”的状态。

## 根因与修复

- 根因：页释放和 metadata 发布绑定在同一 shard 临界区；`ReleaseUnits()` 返回 `void`，调用方无法区分成功与失败。
- 修复方式：新增 `scavengingBits_` 和 `scavengingRegionCount_`。锁内把 hot-free 转为 `alloc=1, scavenging=1`，更新 free count 与 summary；锁外执行返回 bool 的 `ReleaseUnits()`；再加锁，成功转为 scavenged-free，失败回滚 hot-free。
- 复用范围：普通 Scavenge 与 `ReleaseCompletelyFreeSegment()` 共用 claim/complete helper。`ShrinkHeap()` 在 release 失败时保持 view 和 committed window 不变。
- 未包含：macOS `MemorySet` 删除、零页来源优化。

## 验证与回归

- 验证命令：`clang++ -std=c++14 -pthread -I. -Isrc test/MSFreeManagerShardStatsTest.cpp ...`，并同样运行 SegmentMetadata 与 ShrinkLockOrder 定向测试。
- 验证结果：锁外执行、失败回滚、SCAVENGING 期间不可分配、整 Segment 两阶段发布、Shrink 失败保留拓扑均通过。
- 回归风险：需要继续用压力测试观察高并发 Scavenge 与分配交错、RSS 回落和 shard lock p99；macOS 仍保留 release 前整段清零成本。
