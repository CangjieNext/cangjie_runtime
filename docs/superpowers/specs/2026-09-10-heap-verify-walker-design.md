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
| 6 | 阴性注入复测 | 检测能力不回退 | fatal + 正确报告（**已执行 2026-09-15，8/8 通过，见 §12**） |

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
| F4 | 目标非堆地址且 dladdr 不命中——栈/malloc/匿名 mmap/未映射 | `targets non-module %p (wild pointer?)` | **唯一不 deref 目标的检查**；**opt-in：cjVerifyNonHeap（默认关，2026-09-23 起，见 §10.5）**——关=全平台容忍非堆目标且跳过 dladdr；开=dladdr 命中（模块常量对象，如 RawArray<UInt8> 字面量）与注册原生根放行，门控平台另查栈区间 |
| F5 | 目标在堆内但对象头损坏（:244） | `targets %p with invalid header` | |
| F6 | 目标状态码非法且在 ghost region 外（:249） | `targets %p<%s> in state %d outside ghost from-region` | |
| F7 | 活对象/根的目标落在 GARBAGE/FREE region（:265） | `targets %p<%s> in %s region (dangling reference to reclaimed object?)` | 悬垂引用两种时态：GARBAGE=判死待 FP 回收，FREE=已回收进 dirtyTree；复活对象靠 ExemptFromRegions 前置豁免免于误报 |

F7 的 liveness 门是**保守 best-effort**：仅当目标 unit 角色为 leader（:263）时读 leader 的 regionType；目标 unit 为 FREE_UNITS/SUBORDINATE_UNIT 时跳过——后者因 walker 安全规则禁止跟随 `ownerRegion`，前者单元级已回收（madvise 过的页已被 F5 头检查拦下）。

### 10.2 合法放行场景（查但不报错）

| 场景 | 机制 | 依据 |
|---|---|---|
| null 引用 | :223 直接通过 | |
| 模块常量对象 | dladdr 命中已加载模块映射——仅在 cjVerifyNonHeap=1 时才查询（默认关连 dladdr 都跳过） | 字符串字面量等 |
| ghost region 整段跳过 | WalkHeap 分支①按冻结 `regionEnd0` 跳过——**真尸体从不进 VerifyObject（连头都不查）** | :126 注释：corpses 零价值（ART `heap.cc:3328` 先例） |
| 混合复用 region 的新对象 | 分支③遍历到带残留 ghost 位 unit 上的新对象 → :180 拦下：查头+状态、字段不查 | 可达路径：dirtyTree 相邻合并复用时 leader unit 无 ghost 位 + 成员 unit 带残留位（ghost 位/0 后缀跨回收存活，`InitRegionInfo` 不写）；新对象无位图，字段本就不可查 |
| 位图门 | 非存活（floating garbage）对象的字段不查（:189），头+状态保留 | ADR-10；生产可达性前置过滤的复制 |
| tagged ref 不可路由回退裸地址 | ResolveRef :218 回退，**非放行**——回退地址继续过 F4-F7 | tag 语义是"疏散前地址"而非"已搬迁"：exempted（`ExemptFromRegions` 先于 `PrepareForwardTable` 摘除、从未置 ghost 位）、pinned（`ClearGhostRegionBit`）、resurrect 对象打 tag 后从未移动，裸地址即对象本身；为 `IdleBarrier` 两级语义（`TryUpdateRefField` 失败 → `TryUntagRefField`）的只读复刻。**位布局（2026-09-15 实测确认，RefField.h:167-176）**：x86_64 下 `address:48 + isTagged:1(bit48) + tagID:1(bit49)`——tag 在高位而非低位；`GetTargetObject()` 返回的 address 位域天然不含 tag 位，回退值即干净裸地址。补充：活对象持有指向可路由 region 内死 offset 的 tag 时，`GetRoute` 的 `CHECK(toRegion2Idx != INVALID_VALUE)`（`LiveInfo.cpp:20`）fail-fast 中止——真 corruption 信号，接受 |

### 10.3 失败后的处理（Verify，:65-92）

1. 每条失败立即 `RTLOG_ERROR`（带 pre-GC/post-GC tag、holder 地址与 TypeInfo 名、字段偏移，`RecordFailure` :316-325）；
2. 单次校验完整跑完（不在首个失败处中断），统计 `failCount_`；
3. `failCount_ > 0` → 输出 FP 活动归因行（finalized/enqueued 周期增量、FP mid-run 实时态）；
4. pre-GC 额外输出 `NOTE: heap was already corrupted BEFORE this GC cycle started`（区分本轮损坏 vs 上轮遗留）；
5. `CHECK_DETAIL(false, ...)` → fatal 中止（:88）；通过则 VLOG 一行统计（对象数/根数/耗时）。

### 10.4 已知问题与优化方向（未实施，待授权）

- ~~**`DumpAddressMapping` 未定义**~~：**已于 2026-09-14 实现并验证**（`HeapVerifier.cpp:331-359`，读 `/proc/self/maps` 逐行匹配区间；best-effort，失败降级为单行 ERROR，不扰动 failCount）。F4 触发时输出 `HeapVerifier: %p in mapping: <maps行>` 或 `not in any mapping (unmapped hole)`；
- **dladdr 性能**（**已缓解 2026-09-23**：`cjVerifyNonHeap` 默认关，非堆目标连 dladdr 都跳过——全量实测 HLT per-case 总和 −14.3%；开启时的下列优化方向保留）：每次调用走 glibc link_map 遍历 + dl 锁（百 ns~µs 级），字符串密集程序中非堆目标高频。优化方向：Verify 入口（STW 下模块集稳定）用 `dl_iterate_phdr` / `/proc/self/maps` 快照文件映射区间 → 排序数组二分（语义等价、纯内存无锁），可选叠加页粒度 memoization（字面量高度聚集）。**TypeInfo/TypeKind 判型不能替代**：需先 deref 目标读头，wild pointer 场景（恰是 F4 要抓的）SEGV；且"指向模块"这一合法性判据 TypeInfo 结构内无等价信息。TypeInfo 校验仅可作为区间判定之后的可选精化（抓"指向模块但非对象边界"），前提是先证实常量对象只有 RawArray 一种。

### 10.5 F4 非堆检查开关（cjVerifyNonHeap，2026-09-23 已实现并全量验证）

F4 从"平台门控"改为 **env opt-in**（`cjVerifyNonHeap`，"1"/"true"/"on"，默认关，进程级缓存；`HeapVerifier::IsNonHeapVerifyEnabled`，HeapVerifier.h:86 / .cpp:88）。设计动机：① dladdr 消歧有实测成本（§10.4，全量 −14.3% HLT per-case）；② 平台门控后 Linux 本就整体放弃——"查不查"的决定权交给显式选项，与其他 cjVerify* 开关语义统一（ADR-4）。

| 状态 | 行为 |
|---|---|
| 关（默认） | 非堆目标一律容忍（TraceRefField give-up 同语义），**连 dladdr 分类也跳过**；全平台统一（OHOS/ANDROID 原"无条件查"同改 opt-in） |
| 开 | 全平台严查：模块常量（dladdr 命中）与注册原生根（futureSet）放行不变；`#if OHOS/ANDROID` 另有 ScheduleIsCJThreadStackAddr 栈区间放行；其余报 F4 |

已知代价（显式开启即接受）：Linux 等扩容平台无完备栈 oracle，闭包捕获槽持有栈地址的形态（escape-analyzed receiver）会误报。

验证：单元级 4/4（csv_case 默认关 PASS / 开=1 时 F4 abort rc=134、offset 24 即闭包 slot3；math_gate 开=1 时 PASS=原生根放行有效；helloworld 四变量 PASS）；全量 71,611 例（verify 三变量 + NonHeap 关）vs 9/22 同配置：正确性零漂移（flaky 池轮换、零代码归因回归），墙钟 4h28→4h05，**verify 三变量全开净开销 +9%→≈+0.5%**。明细：`logs/compare_report_20260923_vs0911.txt` / `_vs0922ref.txt`，per-case：`logs/cost_time_20260923_verifyall/`。

## 11. 诊断增强：root-path 追溯（cjVerifyRootPath，2026-09-17 已实现并业务场景验证）

对齐 ART `Verification::FirstPathFromRootSet`（`art/runtime/gc/verification.cc:206`）：任一 F1-F7 失败后，BFS 追溯**从根集到坏对象的首条最短引用路径**并输出，例如：

```
post-GC: root-path to target 0x...f090: root = 0x...f180<default:StackNode> -> +16 0x...f168<default:StackNode> -> ... -> +16 0x...f090<header-untrusted>
```

（业务场景实测：栈持有的链表 10 跳完整路径，`+16` 为字段偏移，坏对象以 `<header-untrusted>` 标注）

- **开关**：`cjVerifyRootPath=1`，默认关；正常路径零成本零输出（RSS 对照实测 <0.3% 噪声）——仅在 `failCount_ > 0` 的崩溃路径上付费
- **触发位置**：各失败点只 `MaybeTraceRootPath` 记入 pending；`Verify()` 末尾（WalkHeap+VerifyRoots 均返回后）`FlushPathTraces` 统一执行——**延迟是有意的**，见坑 1
- **算法**：五源根集（static/export/concurrency/FP + **mutator 栈根**，见修复 1）入队 + BFS，节点池记录 `(obj, parentIdx, offset)`，命中 target 时**回溯构建路径串**——内存 O(N)，优于 ART 的逐跳字符串拼接（O(N×路径长)，其只在小型测试堆成立）
- **展开 gate（追溯语义，故意宽于 VerifyObject）**：可信头 + 非 ghost 尸体；**刻意不用 mark-bitmap gate**——转发到 to-space 的对象与 TL bump 新分配都是"活但未标记"，套用 verify 的保守检查 gate 会剪断一切穿过转发对象的路径（实测教训）。最坏走进浮动垃圾的陈旧引用：只读诊断无害，预算兜底
- **预算**：25 万节点封顶（`kMaxTraceNodes`）——时间（每节点 O(字段数)）与内存（~64B/节点含哈希集，~16MB）同时受限；耗尽输出 `<trace aborted: node budget of 250000 exceeded (target unreachable or path too deep)>`。
  **定位（量化）**：512MB 堆按最小对象（8B StateWord 头）计约 6700 万对象，25 万预算覆盖率 ~0.4%；全覆盖需 ~4.3GB 节点池（大于堆本身 8 倍）——预算不是"调大即可"的参数，是 O(节点) 内存追踪算法的**必然约束**。若需覆盖全堆深度的路径，方向是复用 GC 标记位图的"父亲位图"方案（每对象 ~2bit、约 8MB，但要求 verify 时跑一次类标记遍历，属另一次设计变更）。
- **实现中踩掉的两个坑（已固化在代码注释）**：
  1. **嵌套死锁**：失败可能发生在 `Visit*Roots` 回调内部，而 BFS 又要迭代同一批根——从回调内重入同一迭代直接死锁（实测挂起 10 分钟）。延迟 flush 根治；
  2. **堆外地址必须先过滤**：字段合法指向模块常量（.so 数据段），未过 `IsHeapAddress` 的地址流入 `IsSurvivedObject → GetRegionInfoAt` 会触发 runtime 的越界 `std::abort`。enqueue 前置堆地址 gate。
- **实现过程中的两个功能修复（2026-09-17，业务场景验证驱动）**：
  1. **栈根缺失（功能错误级）**：初版种子只含 VerifyRoots 的 4 类根，局部变量持有的链（业务代码最常见形态）整体不可见，`<no path>` 成为误导答案。修复：种子补 `MutatorManager::VisitAllMutators + VisitMutatorRoots`（GC remark 同源，TracingCollector.cpp:631，同 STW 语境）。修复前冒烟 case 恰好全部命中 static 直达的 runtime 内部对象（注入目标偏置），缺陷不可见；
  2. **bitmap gate 剪断转发链**：见上"展开 gate"——post-GC 时 to-space 转发对象 `IsSurvivedObject=false`，初版 gate 在第一跳就断链。
- **开销实测**（`logs/inject_probe/mt_*.log`、`biz_*.log`）：

| 场景 | 耗时 | 内存 |
|---|---|---|
| 快速命中（10 跳业务链，post） | 1.3ms | ~0 |
| 快速命中（static 链 11 跳） | ~284ms（含 300 万对象 verify 收尾） | ~0 |
| 全图深路径（30 万跳链） | BFS ~364ms（25 万节点耗尽） | +16MB（预算封顶，RSS 77MB 含程序） |
| 正常路径（无失败） | 0 | 0（三组 RSS 对照 <0.3% 噪声） |

- **已知局限**：
  - 多根并列时只输出首条最短路径（BFS+visited 先到先得，对齐 ART）；
  - 预算耗尽的输出 `target unreachable or path too deep` 中，两种成因**无法区分**：目标真不可达（无引用链）与路径存在但超过 25 万步（B3 实测形态：30 万跳链尾，路径存在、预算内走不完）——读到 unreachable 不要直接推断"无引用链"；
  - pre-GC 首轮的深路径：gate 放宽后 BFS 已不依赖 bitmap，**按设计**首轮深路径同样可追，但 B1/B2/B3 均以 post-GC 验证，pre 首轮未单独实测。

## 12. 故障注入验证（2026-09-15/17 执行）

### 12.1 注入机制

语言层拿不到托管对象地址（无 `std.unsafe` 包、`std.core` 无 `Unsafe` 类——安全设计使然），注入下沉到 runtime：`HeapVerifier::WalkHeap` 分支③的 `VisitAllObjects` lambda 内、`VerifyObject(obj)` 之前插入 env 门控钩子（`MRT_HEAP_INJECT=<mode>`，未设置零开销），对 walker 首个到达对象做位级破坏——破坏与检查在同一次遍历内相邻，STW 下原子安全。**v2 扩展**（2026-09-17）：`MRT_HEAP_INJECT_TARGET=<TypeName子串>:<序号>` 指定「walker 第 N 个类型匹配对象」为破坏目标，用于业务对象验证（注入 patch 备份于 `logs/inject_patch_backup/`，非仓内代码）。

| 模式 | 破坏动作 | 目标检查 |
|---|---|---|
| f2 | StateWord 低 48 位（TypeInfo 指针）清零、保留高 16 位状态 | F2（顺带 F5：坏对象同时被 root 引用） |
| f3 | offset 6 状态码写 FORWARDED(3) | F3 |
| f4 | 经 ForEachRefField 全部引用字段写 `0xdeadbeef000` | F4 + DumpAddressMapping |
| f7 | 扫描定位 GARBAGE region 首对象，存活 holder 字段指向它 | F7 |
| tagged | 同 f7 但置 isTagged 位（bit48） | ResolveRef tagged 路径 |
| TARGET 模式 | 按「类型名子串:序号」选中业务对象执行 f2 破坏 | F2 + root-path 业务形态 |

### 12.2 验证矩阵

**冒烟级（2026-09-15，8/8）——目标均为 walker 首对象（runtime 内部对象、static 直达），证据力有限，详见 12.4 教训**：

| # | 场景 | 结果 |
|---|---|---|
| 0 | 对照（verify 全开无注入） | ✅ 10 轮 passed 零误报 |
| 1 | F2 头损坏 | ✅ abort + invalid header |
| 2 | F3 状态非法 | ✅ abort + unexpected state |
| 3 | F4 野指针 | ✅ abort + non-module + unmapped hole 诊断 |
| 4 | F5 坏头目标 | ✅（f2 顺带） |
| 5 | F7 悬挂引用 | ✅ abort + dangling reference |
| 6 | 豁免不误报 | ✅ 常量引用零误报 |
| 7 | tagged 双面 | ✅ 天然残留 1,284~22,944 个/轮全部正确解析；bit48 注入识别 + FindToVersion/fallback 双分支实测 |

**业务场景级（2026-09-17，root-path 功能，TARGET 模式，`logs/inject_probe/biz_*.log`）**：

| # | 场景 | 结果 |
|---|---|---|
| B1 | **栈根持有**：业务类 StackNode 链（main 局部变量），破坏链中部，期望多跳栈根路径 | ✅ `root = 链头<StackNode> -> +16 ×9 -> 坏对象<header-untrusted>`，1.3ms |
| B2 | **static 根持有**：业务类 StaticNode 30 万链，破坏中部 | ✅ `root = StaticHolder -> +8 链头 -> +16 ×9 -> 坏对象` |
| B3 | **深路径预算**：30 万跳链尾目标——路径存在但超出预算 | ✅ 走满 25 万节点后明确收手（报告 too deep），BFS ~364ms / +16MB 封顶 |
| 对照 | verify+pathtrace 全开无注入 | ✅ 零输出（biz 与 f2 两程序） |

**已知未覆盖缺口**（低优先）：F6 目标侧无独立注入；tagged 悬挂的确定性 F7 未复现（ghost region 死对象经 FindToVersion 返回算术 toObj，解析后合法放行——生产不存在此形态）。

### 12.3 过程中的事实澄清（修正早前认知）

1. **region 回收三档路径**：未满 TL region 的死对象原地躺到 region 满（copying 无原地清扫）；满 region 存活率 ≥0.80（Exemption threshold）豁免转发、死对象成浮动垃圾；<0.80 转发后整 region 标 GARBAGE → FP 拆解进 dirtyUnitTree。LARGE region 由 `CollectLargeGarbage` 在 PostTrace 直接标 GARBAGE（早于 forwarding 路径）。依据：`AssembleSmallGarbageCandidates`（RegionManager.cpp:443）、`ExemptFromRegions`（:480）、`CollectFromSpaceGarbage`（RegionManager.h:258）。
2. **ghost+GARBAGE 叠加态**：转发产生的 GARBAGE region 同时带 ghost bit（`PrepareForwardableRegion` 先置，`DispelGhostFromRegion` 下轮才清）——**每轮 GC 都大量产生**，此前"F7 生产很少触发"的说法不成立。F7 检查（CheckTarget 直读 regionType）不受 walker Branch① ghost 跳过影响，看得见这些 region；但复用 walker 导航的扫描器若先查 ghost 会全部漏掉（实测教训：注入扫描盲区，修正后普通链表程序即可命中小对象 GARBAGE region）。
3. **元数据区与对象数据区物理分离**（`RegionManager::Initialize`，RegionManager.cpp:376-385：`regionHeapStart = regionInfoAddr + metadataSize`）：FP 拆解只写元数据区，**物理上碰不到对象头**。注入期一次 SEGV 曾误归因于"FP 竞态覆盖对象头"，真因是注入字节序错误（tag 位误写低位）造成未对齐地址错位读。
4. **FindToVersion 是区间算术路由**（`GetRoute = toRegion1Start + preLiveBytes`，LiveInfo.cpp:15-24）：对 ghost region 内死对象返回**非 null 的算术地址**而非 null——"活 holder 的 tagged 指向死对象"在生产不存在（死对象字段不被读），故无风险，但注入实验需知此语义。
5. **转发产物不在 mark bitmap**（2026-09-17 新）：forwarding 后 to-space 对象 `IsSurvivedObject=false`——verify 主流程 :189 的 bitmap gate 因此**跳过转发对象的字段检查**（保守语义，可能是覆盖缺口，值得另行评估）；root-path 已通过放宽 gate 规避（见 §11）。
6. **`_ = x` 丢弃表达式不构成活跃性使用**（2026-09-17 新，用例工程教训）：仅靠尾部 `_ = head` 保温的引用链会被编译器判死回收（栈根消失）；用例必须真实遍历链（如定期求和）以保持活跃。

### 12.4 方法论教训（记录以防复发）

- **验证目标偏置掩盖功能缺陷**：冒烟 8/8 的注入目标全是「walker 首对象」= runtime 内部对象（static 直达、零跳），业务对象/栈持有/多跳形态零覆盖——root-path 的栈根缺陷在此矩阵中不可见。功能验证的用例设计必须覆盖功能的**目标使用形态**，而非仅便利形态。
- **归因先查布局事实再套已知主题**：SEGV 曾拿设计文档中最显眼的"FP 竞态"主题硬套（参见 12.3-3 的误归因）。
- **实验脚手架（注入钩子/诊断日志）与功能代码严格分账**：注入 patch 始终以备份形式存在（`logs/inject_patch_backup/`），验证完毕即剥离，仓内只留功能本体。
