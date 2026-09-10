# Heap Verify Walker 方案设计（ADR-12）

- 日期：2026-09-10
- 状态：实现中
- 前置：`2026-09-02-heap-prepost-verify-design.md`（握手版，ADR-11）
- 分支：`feature/heap-prepost-verify`

## 1. 背景与动机

握手版（ADR-11）消除了 v1 pending 排干门控的 ~2s/轮串行化，但保留了 FP 互斥协议：verify 通过 `verifyRequested`/`metadataJobActive` 两个 seq_cst 原子 + Dekker 双检 + FP 侧 `Wait(1)` 退避实现互斥。

**双开关全量矩阵实测**（`logs/verify_matrix_report_20260910.md`）暴露了握手的残余成本：

| 配置 | 墙钟 | 增量 |
|---|---|---|
| post-only | 4h01m | ≈ 0 |
| pre-only | 4h15m | +14m |
| 双开 | 4h31m | +30m |

pre-verify 的 +14 分钟全部来自 in-flight FP 等待（2% 碰撞率 × 0.58s 中位 × 全量周期数）。

**根因**：verify 的遍历复用了 `ForEachObjUnsafe`（全堆地址空间扫描、读非 0 当前几何），被迫与 FP 的元数据写集产生交集。这个交集是**实现偷懒的结果，不是需求**——verify 的判定路径（活对象内容、位图、ghost 路由表）与 FP 写集本来零交集。

## 2. 核心洞察

FP 的写集 = garbage/free region 的**非 0 后缀元数据**（`InitRegionInfo`/`InitSubordinateUnit`/`ToFreeRegion` 逐 unit 改写）。verify 只需绕开这个写集：

- **ghost 位 + 0 后缀字段**：FP 从不写入（惰性路由要求其跨周期存活，`InitRegionInfo` 字段清单不含任何 0 后缀字段）——**天然的免竞争冻结快照**
- **FREE / SUBORDINATE 单元**：不需要解读结构，逐 unit 爬行即可安全通过
- **leader 角色**：`InitRegionInfo` 写序保证 regionEnd 先于 unitRole，读到 leader 角色时 extent 已完成

## 3. Walker 算法

```
cursor 从 regionHeapStart 推进到 inactiveZone（读一次，FP 可并发推进，读旧值安全）：

① GetGhostFromRegionAt(cursor) ≠ nullptr？
     → ghost 位=1：用 0 后缀冻结 extent 跳过该 region 全部内容
     → cursor = leader.start + GetGhostRegionSize()（regionEnd0，FP 永不写）
     → continue（跳过的内容 = 本轮尸体或复用后新对象，均为零价值）

② 读 unitRole（对非 ghost 单元的唯一一次非 0 读）：
     FREE_UNITS       → cursor += UNIT_SIZE    // free 单元恒 1-unit
     SUBORDINATE_UNIT → cursor += UNIT_SIZE    // ★永不跟随 ownerRegion
     leader 角色      → 进入③

③ leader（InitRegionInfo 写序：regionEnd → regionType → unitRole 最后写，
   读到 leader 角色时 extent 已完成）：
     regionEnd = region->GetRegionEnd()
     if (!IsValidRegion || IsFreeRegion || IsGarbageRegion) → cursor = regionEnd, continue
     region->VisitAllObjects(VerifyObject)   // 唯一真正访问对象的地方
     cursor = regionEnd
```

## 4. 五条承重不变量（正确性依赖，违反即静默竞态回归）

| # | 不变量 | 代码依据 | 违反后果 |
|---|---|---|---|
| 1 | `InitRegionInfo` 写序 regionEnd → regionType → unitRole（role 最后） | `RegionInfo.h:1182-1189` | 读到 leader 角色但 extent 未完成 → 游标跳飞 |
| 2 | free 单元恒 1-unit | `ToFreeRegion → InitFreeRegion(idx, 1)`，`RegionInfo.h:1147` | +UNIT_SIZE 步长错误 → 游标失步 |
| 3 | 0 后缀字段 + ghost 位 FP 永不写 | `InitRegionInfo` 字段清单不含 0 后缀；惰性路由要求跨周期存活 | ghost 分支读到 FP 正在写的快照 → 撕裂 |
| 4 | FP 作业不分配对象（STW 期间 FP-as-mutator 被停住） | FP 跑 CJ finalizer 时是正常 mutator | 新 region 非空 → 跳过有实际覆盖损失 |
| 5 | 撕裂读只影响导航、不影响判定 | 规则①②只做 skip/advance，verdict 永远来自 VisitAllObjects 的完整 region | 撕裂读产生错误 verdict → 误报/漏检 |

不变量 5 是 walker 与已否决的"读侧容错"的本质区别：**撕裂永远到不了判定路径**。

## 5. 覆盖分析

| 对象类别 | walker 行为 | 价值判定 |
|---|---|---|
| 本轮 GC 有效 region 的对象（to/exempted/pinned/large） | 完整检查（header + 位图门控引用） | 与握手版完全一致 |
| ghost region（本轮尸体） | 跳过（0 后缀导航） | 握手版也跳过（garbage 检查）——一致 |
| FP 复用单元上的新 region（新分配对象） | 跳过（ghost 位残留 → 0 后缀导航） | **实测零损失**：新对象无位图（引用本就不查）、header 必然合法 |
| 死循环/SEGV 风险单元（SUBORDINATE 中间态） | +UNIT_SIZE 爬行 | 消除（永不读 ownerRegion） |

## 6. 拆除清单

| 组件 | 操作 |
|---|---|
| `FinalizerProcessor.h` | 删 `metadataJobActive`/`verifyRequested` 原子、`IsMetadataJobActive`/`SetVerifyRequested`/`TryEnterMetadataJob`/`ExitMetadataJob`/`HasPendingMetadataWork`；保留 `finalizedCount`/`enqueuedCount`/`fpState`（证据用） |
| `FinalizerProcessor.cpp` | Run() 恢复原始作业调用（去掉 TryEnter/Exit 包裹与 Wait(1) 退避） |
| `WCollector.cpp` | `NotifyToReclaimGarbage` 归位到 `CollectSmallSpace` 末尾 |
| `TracingCollector.cpp` | hook 简化为直接 STW + Verify（无重试/等待/skip） |
| `HeapVerifier.h/.cpp` | 删 `RunVerifyWhenQuiet`；新增 `WalkHeap()` 替换 `Heap::ForEachObj` 调用 |

FP 侧行为**完全归位生产语义**（零 verify 感知）；GC 流程 `NotifyToReclaimGarbage` 回到原始位置（WCollector.cpp 的 diff 归零）。

## 7. 保留的设计决策（不变）

- STW 仍必须持有（对象内容/allocPtr/finalizer 需冻结）
- `UnbindPreviousLiveInfo` 延后到 pre-verify 之后（位图门控需要上一轮位图）
- verify 只读、失败统计后 fatal（ADR-5）
- FP 活动证据报告（计数器保留）
- 位图门控 + idle 读语义 tag 解析 + ghost 状态豁免（ADR-10，对象级检查不变）

## 8. 验证矩阵

| # | 用例 | 判定 | 通过标准 |
|---|---|---|---|
| 1 | gc_conc01 × 3 | 安全底线（原 10/10 SEGV 用例） | FP 全速无退避下零 SEGV / 零误报 |
| 2 | 压力程序 + helloworld | 冒烟 | exit 0 |
| 3 | gccount_01 | 性能 | 墙钟 ≈ 109s 或更低（等待消除）；仪表化统计 verify 次数/耗时 |
| 4 | bench_finalizers | 性能 | 墙钟 ≈ 9s 或更低 |
| 5 | 全量 HLT+LLT（600s 标准） | 综合 | 0 verify fatal；与基线对齐 |
| 6 | 阴性注入复测 | 检测能力不回退 | fatal + 正确报告 |

## 9. 预期收益

| 指标 | 握手版 | walker 预期 |
|---|---|---|
| pre-only 墙钟 | 4h15m | ≈ 4h01m（与 post-only 持平） |
| 双开墙钟 | 4h31m | ≈ 4h05m（仅剩扫描成本） |
| gccount in-flight 等待 | 41 × 0.58s | **0** |
| FP 行为改变 | 退避重试 + notify 挪位 | **零** |
| 协议复杂度 | 2 原子 + Dekker + 3s 阀 | **零**（全部删除） |
