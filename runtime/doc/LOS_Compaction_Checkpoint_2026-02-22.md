# LOS Compaction 调试检查点（2026-02-22）

## 当前状态
- 并发搬移主线已接入：
  - barrier assist 前驱推进（访问中间对象时可触发依赖安装）
  - compaction plan 并发访问加锁（快照读取）
  - finalize 与 pending plan 生命周期已形成闭环
- 之前频发的 `failed to add dirty units` 已做止血处理（重复 free 区间不再直接 fatal）。
- 单轮高压验证结果：**通过**（`EXIT:0`，`=== NO CRASH ===`）。

## 最近一次有效验证
- 用例：`gc/main_crash_repro`
- 运行参数：`cjEnableLOSCompact=1 cjHeapSize=64mb`
- 诊断日志：`gc/diag_one_round_try.log`
- 结果：
  - 程序输出：`=== NO CRASH ===`
  - 退出码：`0`

## 2026-02-22 最新增量
- 追加了 2 轮短回归尝试：
  - `diag_next_2.log` 对应一轮完整通过（stdout 结尾 `=== NO CRASH ===`）。
  - `diag_next_1.log` 对应会话中断场景（日志有 GC 进展，但 stdout 为空，未形成完整结束标记）。
- 当前未再次观察到 `failed to add dirty units` fatal。
- 现阶段判断：稳定性较昨日提升，但仍需在“无中断前提”下做 2~5 轮连续回归确认。

## 本轮关键变更（高层）
- LOS 回收/compaction 路径的并发一致性修正。
- free tree 重复插入导致的 fatal 改为可恢复处理（避免同一 free 区间重复回收直接中止进程）。
- 设计文档已同步到“并发搬移 + barrier assist”方案。

## 明天建议第一步
1. 先跑 2 轮短回归确认非偶发：
   - `source /Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260220/cangjie/envsetup.sh`
   - `cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime/runtime/gc`
   - `for i in 1 2; do env MRT_REPORT=./diag_next_${i}.log cjEnableLOSCompact=1 cjHeapSize=64mb ./main_crash_repro; echo RUN-$i:$?; done`
2. 若稳定，再扩到 5 轮；若失败，优先看是否再次出现 `failed to add dirty units`。

## 当前建议（今天继续时）
1. 用前台串行命令重新做 2 轮（避免后台会话中断噪声）：
  - `for i in 1 2; do env MRT_REPORT=./diag_serial_${i}.log cjEnableLOSCompact=1 cjHeapSize=64mb ./main_crash_repro; echo RUN-$i:$?; done`
2. 若 2/2 均为 0，再做 5 轮；若有失败，优先抓失败轮的 `diag_serial_i.log` 与 stdout 尾部。

## 关注点
- 目前 LOS 压力仍高（`alloc fail/fallback` 数值较大），后续可继续优化 compaction 触发与回收效率。
- 当前目标先稳住 correctness，再做性能和碎片优化。
