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
| ghost region（本轮尸体） | 跳过（0 后缀导航）——**真尸体从不进 VerifyObject，零检查** | 握手版也跳过（garbage 检查）——一致 |
| FP 复用单元上的新 region（新分配对象） | 整块复用（leader 带 ghost 位）→ 跳过；**混合复用（dirtyTree 相邻合并出"leader 无位 + 成员带位"的新 extent）→ 分支③ `VisitAllObjects` 按当前 extent 遍历，带位 unit 上的新对象进 VerifyObject，被 `:180 InGhostFromRegion` 拦下：查头+状态、字段不查** | **实测零损失**：新对象无位图（引用本就不查）、header 必然合法 |
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
- 位图门控 + idle 读语义 tag 解析 + ghost 状态豁免（ADR-10，对象级检查不变）——**细化修正**：相对 ADR-10 的 §6.2，引用目标检查新增 dladdr 非堆消歧（F4）与 region 状态 liveness 门（F7），完整核对清单见 §10

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
| FP 行为改变 | 退避重试 + notify 挪位 | **零**（全部删除） |
| 协议复杂度 | 2 原子 + Dekker + 3s 阀 | **零**（全部删除） |

## 10. 校验内容（实现核对版，2026-09-14）

对照 `HeapVerifier.cpp` 逐行核对的完整清单。行号为核对时点锚点。

### 10.1 失败场景（共 7 类）

**对象本体（VerifyObject，:164-195，仅来自 WalkHeap 分支③ 的 VisitAllObjects）**

| # | 条件 | 报错信息 | 后续处理 |
|---|---|---|---|
| F1 | walker 产出对象不在堆范围（:167） | `object %p from ForEachObj is outside heap range` | return |
| F2 | 对象头损坏（:171） | `object %p has invalid header (null or corrupted TypeInfo)` | return，**跳过字段遍历**（头不可信） |
| F3 | FORWARDING/FORWARDED/未知状态码出现在 ghost from-region 之外（:175，规则见 :277-290） | `object %p<%s> has unexpected state %d outside ghost from-region` | **继续字段遍历**（StateWord 与字段区独立） |

**引用目标（CheckTarget，:221-275，覆盖活对象 ref 字段 + 四类根 :292-314）**

| # | 条件 | 报错信息（均带 `kind/holder<type>/offset` 前缀） | 备注 |
|---|---|---|---|
| F4 | 目标非堆地址且 dladdr 不命中（:233）——栈/malloc/匿名 mmap/未映射 | `targets non-module %p (wild pointer?)` | **唯一不 deref 目标的检查**；dladdr 命中（模块常量对象，如 RawArray<UInt8> 字面量）→ 合法放行（:242） |
| F5 | 目标在堆内但对象头损坏（:244） | `targets %p with invalid header` | |
| F6 | 目标状态码非法且在 ghost region 外（:249） | `targets %p<%s> in state %d outside ghost from-region` | |
| F7 | 活对象/根的目标落在 GARBAGE/FREE region（:265） | `targets %p<%s> in %s region (dangling reference to reclaimed object?)` | 悬垂引用两种时态：GARBAGE=判死待 FP 回收，FREE=已回收进 dirtyTree；复活对象靠 ExemptFromRegions 前置豁免免于误报 |

F7 的 liveness 门是**保守 best-effort**：仅当目标 unit 角色为 leader（:263）时读 leader 的 regionType；目标 unit 为 FREE_UNITS/SUBORDINATE_UNIT 时跳过——后者因 walker 安全规则禁止跟随 `ownerRegion`，前者单元级已回收（madvise 过的页已被 F5 头检查拦下）。

### 10.2 合法放行场景（查但不报错）

| 场景 | 机制 | 依据 |
|---|---|---|
| null 引用 | :223 直接通过 | |
| 模块常量对象 | dladdr 命中已加载模块映射（:242） | 字符串字面量等 |
| ghost region 整段跳过 | WalkHeap 分支①按冻结 `regionEnd0` 跳过——**真尸体从不进 VerifyObject（连头都不查）** | :126 注释：corpses 零价值（ART `heap.cc:3328` 先例） |
| 混合复用 region 的新对象 | 分支③遍历到带残留 ghost 位 unit 上的新对象 → :180 拦下：查头+状态、字段不查 | 可达路径：dirtyTree 相邻合并复用时 leader unit 无 ghost 位 + 成员 unit 带残留位（ghost 位/0 后缀跨回收存活，`InitRegionInfo` 不写）；新对象无位图，字段本就不可查 |
| 位图门 | 非存活（floating garbage）对象的字段不查（:189），头+状态保留 | ADR-10；生产可达性前置过滤的复制 |
| tagged ref 不可路由回退裸地址 | ResolveRef :218 回退，**非放行**——回退地址继续过 F4-F7 | tag 语义是"疏散前地址"而非"已搬迁"：exempted（`ExemptFromRegions` 先于 `PrepareForwardTable` 摘除、从未置 ghost 位）、pinned（`ClearGhostRegionBit`）、resurrect 对象打 tag 后从未移动，裸地址即对象本身；为 `IdleBarrier` 两级语义（`TryUpdateRefField` 失败 → `TryUntagRefField`）的只读复刻。补充：活对象持有指向可路由 region 内死 offset 的 tag 时，`GetRoute` 的 `CHECK(toRegion2Idx != INVALID_VALUE)`（`LiveInfo.cpp:20`）fail-fast 中止——真 corruption 信号，接受 |

### 10.3 失败后的处理（Verify，:65-92）

1. 每条失败立即 `RTLOG_ERROR`（带 pre-GC/post-GC tag、holder 地址与 TypeInfo 名、字段偏移，`RecordFailure` :316-325）；
2. 单次校验完整跑完（不在首个失败处中断），统计 `failCount_`；
3. `failCount_ > 0` → 输出 FP 活动归因行（finalized/enqueued 周期增量、FP mid-run 实时态）；
4. pre-GC 额外输出 `NOTE: heap was already corrupted BEFORE this GC cycle started`（区分本轮损坏 vs 上轮遗留）；
5. `CHECK_DETAIL(false, ...)` → fatal 中止（:88）；通过则 VLOG 一行统计（对象数/根数/耗时）。

### 10.4 已知问题与优化方向（未实施，待授权）

- **`DumpAddressMapping` 未定义**：`HeapVerifier.h:69` 声明、`:239` 调用，全仓库无定义——F4 触发路径存在链接错误风险；
- **dladdr 性能**：每次调用走 glibc link_map 遍历 + dl 锁（百 ns~µs 级），字符串密集程序中非堆目标高频。优化方向：Verify 入口（STW 下模块集稳定）用 `dl_iterate_phdr` / `/proc/self/maps` 快照文件映射区间 → 排序数组二分（语义等价、纯内存无锁），可选叠加页粒度 memoization（字面量高度聚集）。**TypeInfo/TypeKind 判型不能替代**：需先 deref 目标读头，wild pointer 场景（恰是 F4 要抓的）SEGV；且"指向模块"这一合法性判据 TypeInfo 结构内无等价信息。TypeInfo 校验仅可作为区间判定之后的可选精化（抓"指向模块但非对象边界"），前提是先证实常量对象只有 RawArray 一种。
