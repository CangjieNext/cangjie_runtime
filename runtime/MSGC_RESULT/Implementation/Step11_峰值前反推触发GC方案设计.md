# Step11 峰值前反推触发 GC 方案设计

> 基于 2026-05-10 对 `MaybeNotifyReclaim` / `MaybeNotifySweep` / `MaybeNotifyMark` 当前实现、`case1` 峰值行为、以及多组参数实验的静态与实测分析整理。

## 1. 问题背景

当前 MSGC 已经具备一条“Reclaim -> Sweep -> Mark”的弱反推链，但从 `case1` 的 RSS 曲线和 `gc.log` 看，峰值仍然经常表现为：

- mutator 先把 active-region / committed bytes 顶高
- 随后后台 reclaim / sweep 再把空 region 慢慢退掉
- 最终形成“峰值先出现，后台再追赶回落”的形态

这说明当前链路更偏向**峰值后的补救**，还不够像一套“峰值起来前就升级压力”的前置触发机制。

## 2. 当前实现梳理

### 2.1 第一层：Mark 触发

`MSRegionManager::MaybeNotifyMark()` 当前有两条入口：

- `TAKE_REGIONS`
  - 分配路径入口
  - 判据：`effectiveAlloc >= threshold`
- `BG_SWEEP_EXHAUSTED`
  - 后台 sweep 一轮 slice 结束后，若 `remainingUnswept < maxRegions`，则调用
  - 最终判据：`emptyCache + partialSwept + fullUnswept < CJ_MS_HEU_SUPPLY_ENOUGH_REGIONS`

结论：

- 主触发仍然是 `TAKE_REGIONS` 路径的 threshold 判定
- `BG_SWEEP_EXHAUSTED` 只是一个较弱的补充通道

### 2.2 第二层：Sweep 触发

`MSRegionManager::MaybeNotifySweep()` 当前职责是：

- 当某个 size class 的 `ready` 太低，且 `unswept` backlog 还在时，唤醒后台 sweep
- `ConcurrentSweep()` 完成后，也会无条件 kick 一次后台 sweep

`BackgroundSweep()` 当前行为：

- 每轮 slice 先按 `underPressure` 选择 normal / pressure 预算
- 扫 `partialUnswept` / `fullUnswept`
- slice 结束后，若 `remainingUnswept < maxRegions`，则调用 `MaybeNotifyMark(BG_SWEEP_EXHAUSTED)`

结论：

- sweep 当前已经能反向推动 mark
- 但判据偏晚，通常发生在 unswept 快扫空之后

### 2.3 第三层：Reclaim 触发

`MSRegionManager::MaybeNotifyReclaim()` 当前只看：

- `emptyCount >= highWatermark`
- 或 `committedBytes > targetBytes && emptyCount > 0`

`HandleBackgroundScavenge()` 当前行为：

- 如果 `emptyCount == 0 && unswept > 0 && committedBytes > targetBytes`
  - 选择一个 ready 最紧、且 unswept 还在的 `candidateCls`
  - 调 `MaybeNotifySweep(this, candidateCls)`，反向催 Layer 2

结论：

- reclaim 已经能反向触发 sweep
- 但还是偏向“空 region 不够用时再升级”，不是“峰值即将形成前的主动干预”

## 3. 峰值问题的当前判断

基于 `case1` 的 `rss_cpu.csv + gc.log` 对照，目前更像：

- **主因**：GC 触发偏晚，mutator 在两轮 GC 之间先把 active-region / committed 顶高
- **次因**：reclaim / sweep 虽然很积极，但更像在峰值形成后追赶回落

换句话说：

- 当前不是“reclaim 完全不工作”
- 也不是“sweep 完全不跟”
- 而是“它们介入时机还不够前”

## 4. 目标：建立正式的反推升级链

目标是把当前弱链路升级成更清晰的三级升级：

1. **先 Reclaim**
   - 当 committed 对 true-live target 的冗余明显偏大时，优先试图回收 empty region
2. **Reclaim 不够，再 Sweep**
   - reclaim 后如果 committed 没明显回落、但 unswept backlog 还在，则升级 sweep
3. **Sweep 还不够，再 Mark**
   - 如果 sweep 一轮或多轮之后，empty region 还是补不起来，就提前启动下一轮 GC

## 5. 方案设计

### 5.1 Level 1：Reclaim 预警前移

当前 `MaybeNotifyReclaim()` 主要是“empty cache 水位触发器”。建议升级成“committed 冗余触发器”：

- 保留现有：
  - `emptyCount >= highWatermark`
- 新增重点判断：
  - `committedBytes - targetBytes` 是否已经明显偏大
  - 即按 region 粒度看 slack 是否已经超过某条阈值

建议引入：

- `slackBytes = committedBytes > targetBytes ? committedBytes - targetBytes : 0`
- `slackRegions = slackBytes / MS_REGION_SIZE`

触发条件可以设计为：

- `slackRegions >= reclaimSlackTrigger`
- 且 `emptyCount > 0`

这样 reclaim 的作用从：

- “empty cache 太多了，该退页了”

变成：

- “committed 已经明显高于目标 live 水位，该先回收一批空 region，别让峰值继续涨”

### 5.2 Level 2：Reclaim 不够则升级 Sweep

这一步建议放在 `HandleBackgroundScavenge()` 结束后，或者 `BackgroundProcessor::ReclaimHeapGarbage()` 的外层。

需要关注的不是“reclaim 做了没有”，而是“reclaim 效果够不够”：

- 本轮 `releasedBytes`
- reclaim 前后的 `emptyCount`
- reclaim 前后的 `committedBytes`

若出现：

- reclaim 已执行
- 但 `releasedBytes` 很少，或 `committedBytes` 几乎没回落
- 且 `unsweptCount > 0`

则说明：

- 当前不是单纯 empty cache 足够多的问题
- 需要先从 unswept backlog 里制造更多 empty / reusable region

此时升级为：

- `MaybeNotifySweep(this, candidateCls)` 或直接 `NotifyToSweep()`

### 5.3 Level 3：Sweep 不够则升级 Mark

这一步应当利用 `BackgroundSweep()` 当前 slice 结束点。

当前只在：

- `remainingUnswept < maxRegions`

时调用 `MaybeNotifyMark(BG_SWEEP_EXHAUSTED)`。

建议增强为两层：

#### 5.3.1 Sweep 侧时机判定

在一轮 slice 结束后，如果满足任一：

- `remainingUnswept < maxRegions`
- 本轮 `emptiedRegions` 很少
- 本轮虽然 sweep 了不少 region，但 `emptyCache` 仍然没有明显回升

则通知 `MaybeNotifyMark(BG_SWEEP_EXHAUSTED, ...)`

#### 5.3.2 Mark 侧最终升级判定

`MaybeNotifyMark(BG_SWEEP_EXHAUSTED)` 收到这类通知后，再做最终判断。

建议判据：

- `emptyCacheCount < 5`
- 且 `partialSwept + fullUnswept` 也偏低
- 且 `committedBytes > targetBytes`

也就是说，不是仅靠 `emptyCache < 5` 单独决定，而是：

- reclaim 已经不够
- sweep 也没补出足够 supply
- committed 还高

这时才升级成：

- `collector.RequestGC(GC_REASON_HEU, true)`

## 6. 推荐的职责边界

建议保持职责清晰：

- `MaybeNotifyReclaim()`
  - 负责判断是否先开 reclaim
- `HandleBackgroundScavenge()` / `ReclaimHeapGarbage()`
  - 负责判断 reclaim 后是否该升级到 sweep
- `BackgroundSweep()`
  - 负责判断 sweep slice 结束后是否该升级到 mark
- `MaybeNotifyMark(BG_SWEEP_EXHAUSTED)`
  - 负责最终决定是否真的要开新一轮 GC

这样能得到一条清晰的升级链：

```text
Region pressure
  -> Reclaim
  -> Reclaim not enough
  -> Sweep
  -> Sweep not enough
  -> Mark
```

## 7. 与当前实现相比的关键变化

1. `MaybeNotifyReclaim()` 不再只是水位触发，而是前移为 committed-vs-target 的冗余触发
2. `Reclaim -> Sweep` 从隐式 pressure kick 升级成“基于 reclaim 效果不佳”的正式升级
3. `Sweep -> Mark` 不再只靠 `remainingUnswept < maxRegions`，还要看本轮 sweep 是否真正补出了 empty / reusable supply
4. `MaybeNotifyMark(BG_SWEEP_EXHAUSTED)` 的判断语义将更偏“峰值前升级”，而不是单纯 sweep 收口时的尾部补判

## 8. 风险与建议

这套方案最大的风险是：

- 如果 `emptyCache < 5` 或 reclaim-sweep 升级判据过于激进
- 会把 GC 提前得过头
- 结果是 RSS 下降，但 GC 次数、总 GC 时间、mutator 抖动明显上升

因此建议：

- 所有新增判据都做成 env 参数
- 先只在 `case1` / `case9` 上做小范围回归
- 观察：
  - 峰值是否下降
  - GC 次数是否过度飙升
  - sweep / reclaim 总工作量是否只是“提前做了同样的事”

## 9. 下一步落地建议

如果要实现，建议顺序是：

1. 先增强 `MaybeNotifyReclaim()` 的 committed-slack 判据
2. 再给 `HandleBackgroundScavenge()` 增加“reclaim 不够 → 升级 sweep”的结果反馈
3. 最后增强 `BackgroundSweep()` 和 `MaybeNotifyMark(BG_SWEEP_EXHAUSTED)` 的“empty supply 未补足 → 提前开 GC”逻辑

这样可以分阶段验证每一级升级链是否真的对峰值有帮助，而不会一次把所有行为搅在一起。
