# Step62：MSGC 六项 Mark 优化最终架构与全流程实施方案

> 本文是后续代码落地的唯一目标设计。它只描述最终结构，不保留 Step60/Step61 的中间过渡形态。

## 0. 文档目标与结论

本文把 Step60 的四项核心优化与 Step61 中仍需保留的两项调度优化，收敛为一套可直接
落地的最终 MSGC Mark 架构：

1. `NO_REF/HAS_REF Region` 分离；
2. `alloc/mark/scan` 三平面 Bitmap；
3. Region 级扫描；
4. Mark Bitmap 批量更新；
5. 统一任务调度与终止（原优化六）；
6. WorkUnit Pacer/Assist（原优化七）。

最终 Mark 不再以“每个灰对象进入一个 WorkBuf”为主线，而是：

```text
GC Root / SATB old edge / 对象扫描发现 child
  -> 解析目标 Region 和 slot
  -> MarkWordBatch 或单次原子 OR mark bit
  -> NO_REF：winner 到此结束
  -> HAS_REF：设置 pendingWordSummary
  -> 必要时发布一个 REGION task
  -> Region owner 扫描 mark & ~scan 对应的对象
  -> 发现更多 child，直到统一任务账本归零
  -> STW 最终封口
```

这套结构针对大堆下 Mark 速率下降的主要成本：逐对象入队、重复对象合法性检查、
分散的 TypeInfo/bitmap 元数据访问、mark bit 原子 RMW 过多、WorkBuf 搬运以及按
“对象个数”计量导致的负载失真。

本文不是兼容性设计。Runtime、LLVM compiler、stdlib 和最终 `.cj` 二进制必须使用同一版
ABI 一起构建和发布；不支持新旧 MSGC compiler、Runtime、stdlib 或应用二进制的任意混用。
因此本步骤直接保留并升级现有 `CJ_MRT_MSGCFastPathState` 符号，不新增带`V2`后缀的state
或helper符号，也不实现加载期ABI版本协商。写屏障继续调用现有 Runtime wrapper，不新增
compiler-visible 的 mark/publish helper。每次替换Runtime后必须使用同一套SDK重新编译
全部`.cj`产物，布局一致性只由联合构建和跨仓contract test保证。

本步骤只支持 64 位 AArch64 与 X86-64。`MSGCFastPathState`、`MSRegionInfo`、64 位 tagged
head 和原始引用 ABI 都按 64 位固定；不设计 32 位布局或32位原子 fallback。其他目标不得
生成 Step62 inline fast path。

### 0.1 代码落地状态（更新至 2026-07-29）

本节只记录最终结构的落地边界，不引入中间架构。第8章的清单仍作为逐项验收定义。

| 任务 | 当前状态 | 已落地内容 | 尚未完成的验收 |
|---|---|---|---|
| Task 1：64位ABI | Runtime/LLVM已落地 | 64B `MSRegionInfo`、64B原符号FastPathState、统一LLVM常量、二维AllocBuffer ABI、独立`msAllocBuffer` | 完整compiler/stdlib联合SDK |
| Task 2：ScanClass | 已落地 | 唯一分类入口、36×2 TLS/Central lane、object/array传递、AArch64/X86分配fast path | 联合SDK应用级分类压力测试 |
| Task 3：三平面/collect-set | 已落地 | 独立alloc/mark/scan plane、512B stride、SEALED collect-set、ACTIVE不flush、epoch边界 | 联合SDK长时间ACTIVE延迟回收测试 |
| Task 4：Scheduler/Ledger/WorkUnit | 已落地并完成case27调优 | REGION/OBJECT tagged head、move-only lease、统一ledger、能力切片、Q16 Pacer/Assist；删除ready诊断计数、减少重复ledger原子与唤醒 | case26大持久集Mark速率验收 |
| Task 5：batch/Region owner | 已落地并完成case27调优 | outside-set解析、8组×2路word batch、双层mark预检查、winner/pending、单owner scan、word级scan写回、budget requeue、LARGE OBJECT | case26大持久集Mark速率验收 |
| Task 6：写屏障 | 已落地 | 删除activeLog/WorkBuf发布；Runtime scalar/bulk/atomic shade；LLVM phase gate | 联合SDK atomic/bulk并发压力 |
| Task 7：Collector流程 | 已落地 | bounded root staging、统一ConcurrentMark/Termination、Finalizer锁内发布、WeakRef汇聚 | 应用级Finalizer/WeakRef压力 |
| Task 8：Sweep/LARGE/统计 | 已落地 | 单unswept、collect-set移交、`largeNextSupply/largeCollectSet`、collect-only统计、诊断Sweep gate | 默认并发Sweep长稳验证 |
| Task 9：删除与联合验证 | case27已完成 | MSGC普通WorkBuf/activeLog/双unswept/单一large list已删除；Runtime完整构建、安装、codesign和应用重新编译通过；case27 60秒0失败 | case26大持久集性能回归 |

2026-07-29 的最新验证使用
`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/msgc` 中的 compiler 与
stdlib；Runtime由当前工作区以marksweep模式重新构建并安装到同一SDK。替换Runtime后，
case27的`.cj`文件由该SDK重新编译，没有复用旧binary。

已完成的验证证据：

| 验证 | 结果 |
|---|---|
| MarkBatch/WorkUnit/WorkerPolicy单元测试 | 通过 |
| Scheduler/MarkControl/DiscoverObject/dual-ready/TailDrain契约 | 通过 |
| ThresholdPolicy单元测试与ThresholdStability契约 | 通过 |
| `python3 build/scripts/rebuild_msgc_runtime_output.py --skip-clean` | 通过；完成Release构建、SDK安装、codesign与符号核验 |
| case27 20秒筛选 | 952.68 req/s，GC平均17.61ms，0失败 |
| case27 60秒稳定性 | 903.46 req/s，GC平均18.16ms，1019轮GC，54382/54382成功 |
| Runtime目标文件`git diff --check` | 通过 |

case27验证已经完成；case26的128MiB/2GiB持久对象Mark速率回归仍是后续独立验收项。

## 1. 范围、保留项与明确排除项

### 1.1 保留的 GC 语义

- 每轮仍是完整堆 Mark-Sweep，不增加 minor/major 分代；
- 继续使用 deletion-style SATB 写屏障：覆盖旧引用前先 shade old edge；
- small object 使用三平面和 Region owner；
- LARGE/SPAN 保持独立对象标记和 OBJECT task；
- WeakRef、Finalizer、resurrection 保持 MarkTermination 中的既有顺序；
- Sweep 仍可 background/lazy/assist 并发执行；
- Scavenge 仍由 FreeManager 对已经为空且可回收的 Region/span 归还物理页；
- `CJ_MS_STW_SWEEP` 继续作为并发 Sweep 根因诊断开关，不在本步骤删除。

### 1.1.1 固定的平台与ABI边界

- 支持目标只有 AArch64 和 X86-64；
- `uintptr_t`、对象引用和 tagged head 都固定为 64 位；
- `CJ_MRT_MSGCFastPathState` 原符号原地升级为 64B；
- 现有或本步骤新增的Runtime helper都使用5.4正式名称，不增加V2版本后缀；
- 不支持 new compiler + old Runtime、old compiler + new Runtime、旧stdlib或旧应用二进制
  与新Runtime混用；
- contract test 负责验证两个仓的字段offset、bit定义、size-class descriptor和helper符号
  完全一致，但不承担运行时兼容检测。

### 1.2 明确排除

本文明确不实现以下两项：

- **大数组 RangeQueue**：没有 `RangeTask`、`ArrayScanTicket`、RANGE lane 或数组内部
  并行切片。LARGE HAS_REF 数组仍是一个 OBJECT task，由一个 GC worker 完整扫描。
- **Old/Immutable Region**：没有 YOUNG/OLD、minor/major、age table、CardTable、
  remembered set、card epoch 或跨代引用过滤。

因此统一 Scheduler 最终只有 `REGION/OBJECT` 两个 typed lane，WorkUnit 也只维护一套
full-GC 历史。WeakRef 登记使用 worker-local discovery buffer，Finalizer 分类只在
MarkTermination 执行，不再为二者建立并发 `MAIN_ONLY` task。

### 1.3 不改变的对象图正确性

优化只改变“如何记录已发现对象”和“谁负责扫描”，不改变可达性定义：

```text
Root closure
  + 并发期间被 SATB deletion barrier 保存的旧边
  + finalizer resurrection closure
  = 本轮最终 live set
```

ACTIVE Region 不是额外 GC Root，也不建立 `ActiveRootSnapshot`。只有从普通 GC Root、
全局 Root、已扫描对象或 SATB old edge 实际到达的 ACTIVE 对象，才进入临时 visit/scan
闭包。不可达 ACTIVE Region 不会因为“仍在 TLS 中”而被整区扫描；它只是本轮不参与
Sweep。

## 2. 当前实现与最终实现的总体差别

### 2.1 当前基于 WorkBuf 的 Mark

当前 `MarkSweepCollector` 的普通路径是：

```text
ResolveMarkTarget(ref)
  -> 测 mark bit
  -> 读取白对象 stateWord/TypeInfo
  -> atomic TryMark
  -> TypeInfo::HasRefField()
  -> 把对象指针放入 WorkBuf
  -> gcThread/helper/assist 从 globalFull_ 取 WorkBuf
  -> 再次触碰对象，按 TypeInfo 枚举引用字段
```

写屏障同样以对象日志为中心：先 shade old ref，winner 若有引用字段则写入
`AllocBufferMS::activeLog_`；满 WorkBuf 发布到 `globalFull_`。MarkTermination 再回收
所有 per-P `gcWork_`、partial `activeLog_`、global LFStack 和 main-thread deferred queue。

这套方式的优点是对象级语义直接、任务可自然分散；缺点是大堆中每个灰对象都付出对象
地址搬运、WorkBuf 管理、队列竞争和分散的元数据访问。数百万 `[]byte` 叶子虽然不扫描
payload，仍会逐个经历目标解析、对象头/TypeInfo 检查和 mark RMW。

### 2.2 最终基于 Region 的 Mark

最终路径把“对象已发现”与“对象待扫描”分开：

```text
mark plane：任意 producer 并发提交“这个 slot 已发现”
scan plane：Region owner 提交“这个 HAS_REF slot 已完整扫描”
pendingWordSummary：指出哪些 bitmap word 可能存在 mark & ~scan
REGION task：只表达“该 Region 有一段增量扫描工作”
```

普通 small HAS_REF 对象不再携带对象指针进入 WorkBuf。owner 从 bitmap word 直接计算
slot 地址，在同一个 Region 内连续读取 TypeInfo 和字段布局；NO_REF winner 不读取对象
本体，也不创建扫描任务。

### 2.3 最终必须删除或合并的结构

| 当前/中间结构 | 最终处理 |
|---|---|
| `globalFull_`、普通对象 `WorkBuf`、`PerPGCWork` | 从普通 small Mark 主路径删除 |
| `AllocBufferMS::activeLog_` | 最终 ABI 删除；SATB small winner 直接提交 mark/pending |
| 独立 `MSRegionScanQueue` | 删除；REGION 是统一 Scheduler 的一个 typed lane |
| `DIRTY` bit | 删除；`pendingWordSummary != 0` 本身就是工作内容 |
| QUEUED/OWNED 两个独立 bit | 改为单一枚举 `IDLE/QUEUED/OWNED` |
| `regionOutstanding` 与总 outstanding 两套账本 | 删除前者，只保留统一 TaskLedger |
| token 内 role/epoch/generation | Release token 只存 `regionIdx`；role 从稳定 cycle metadata 推导 |
| `activeWorkers` 作为终止正确性条件 | 删除；queued + in-flight 均由 task outstanding 表达 |
| `MAIN_ONLY` lane/chunk pool | 删除；WeakRef 本地登记，Finalizer 在 STW 直接分类 |
| `EXTERNAL_VISITED`/Release visited list | 删除；OUTSIDE_SET 与 collect-set 共用 state/pending/ledger；完整扫描只保留 Debug |
| Release `regionGeneration` | 删除；不重用由阶段协议保证，Debug generation 放 side table |
| Region 内 `sweepState`、`sweepGen` 与 swept/unswept list 三套状态 | 删除 `MSRegionInfo::sweepState/sweepGen`；list membership + sweep in-flight admission 是唯一 Region Sweep 真值；保留 Central 级 `sweepCycleId` 只服务跨轮 pacing/worker/统计 |
| object-count Assist debt | 改为 WorkUnit debt；对象数只保留统计 |
| `DEBT_STALLED` 作为独立正确性分支 | 删除；Pacer 只决定并发度/何时请求 STW，STW drain 才是终止权威 |
| Range/age/card/remembered 相关字段和判断 | 全部不进入 Step62 |

### 2.4 两种方式的优缺点

| 维度 | 当前 WorkBuf | 最终 Region scan |
|---|---|---|
| 任务粒度 | 灰对象 | dirty Region bitmap word |
| 标记去重 | 每对象 mark CAS | 每 word 批量 OR，返回 `newBits` |
| 扫描去重 | 对象只入队一次 | `mark & ~scan` |
| 元数据局部性 | 对象、TypeInfo、WorkBuf、RegionInfo 分散 | 同 Region bitmap/对象连续 |
| NO_REF | winner 前仍常读对象头/TypeInfo | ScanClass + stable alloc，winner 后立即结束 |
| 并行度 | 灰对象多时高 | dirty Region 数少时可能较低 |
| 热点 Region | 多 worker 可扫描不同对象，但队列/RMW 多 | 单 owner，局部性强且避免重复扫描 |
| 超大引用数组 | 一个对象任务，可被某 worker 长时间占用 | 本步骤仍然如此，不做 RangeQueue |
| 实现复杂度 | 简单对象队列 | 需要三平面、owner 状态机和统一终止协议 |

最终选择 Region scan，是为了 case26 这类数百万叶子与大持久集场景的总吞吐；它不是在
所有对象图上都绝对优于 WorkBuf。因此验收必须同时观察 Mark 速率、worker 利用率和
dirty Region 分布，而不能只看任务数量。

## 3. 最终数据结构与全局不变量

### 3.1 ScanClass 与 Region 纯度

```cpp
enum class MSScanClass : uint8_t {
    NO_REF = 0,
    HAS_REF = 1,
};
```

每个非空 small Region 在首次绑定时固定 `(sizeClass, scanClass)`。只要 Region 内还有
对象，ScanClass 不可改变；只有完全空并回到 classless empty cache 后才能重新绑定。

`AllocBufferMS` 按二维索引；Central 保持 36 个 size-class bucket，每个 bucket 内放两条
ScanClass sub-lane，共用该 size class 的 pressure 和统计：

```text
activeRegions[sizeClass][scanClass]
centralBuckets[sizeClass].lanes[scanClass]
```

每条 Central lane 最终只保留三个 ABA-safe tagged intrusive stack：

```text
partialSwept / fullSwept / unswept
```

不再区分 `partialUnswept/fullUnswept`：进入本轮 collect-set 后是否曾经有空槽，对本轮
Sweep 没有意义，Sweep 结果再根据新的 `freeSlotCount` 分到 partial/full。三个 stack
都只做 head push/pop/steal，不需要双向链表和 bucket mutex。

Runtime定义唯一分类函数：

```cpp
MSScanClass ClassifyMSScanClass(const TypeInfo* typeInfo);
```

它是Runtime slow allocation、反射/RAW_POINTER_OBJECT allocation和contract test的唯一
分类真值；不允许MObject、MArray、RegionSpace和barrier各自复制判断。分类矩阵固定为：

| allocation TypeInfo | ScanClass |
|---|---|
| 普通class/struct且`HasRefField()==false` | `NO_REF` |
| 普通class/struct且`HasRefField()==true` | `HAS_REF` |
| primitive/无引用数组 | `NO_REF` |
| ref array | `HAS_REF` |
| struct array | 由component TypeInfo的`HasRefField()`决定 |
| 动态泛型数组 | component为ref或component `HasRefField()==true`时为`HAS_REF`，否则`NO_REF` |
| WeakRef、Finalizer及需要特殊GC处理的对象 | 强制`HAS_REF`并走special handler |
| pinned、反射、FFI或`RAW_POINTER_OBJECT` | 不按AllocType猜测；仍按实际TypeInfo调用唯一分类函数 |

静态已知TypeInfo由LLVM wrapper携带同一结果；动态数组由Runtime读取component TypeInfo。
LLVM侧的分类实现不是第二套语义，必须用与上表相同的lit/runtime矩阵逐项对拍。错误地把
HAS_REF分到NO_REF是fatal ABI错误；不得用Mark期回退判断掩盖。

当前调用链在 `MObject/MArray -> HeapManager -> RegionSpaceMS` 之间会丢掉TypeInfo，因此
最终内部接口固定改为：

```text
MObject/MArray持有TypeInfo
  -> ClassifyMSScanClass(TypeInfo)
  -> HeapManager::AllocateMS(size, allocType, scanClass, local)
  -> RegionSpaceMS::Allocate(..., scanClass)
  -> AllocBufferMS::SlowAlloc(sizeClass, scanClass)
```

public `MCC_NewObject/MCC_NewArray*` 和LLVM wrapper到slow callee的签名不增加ScanClass，
保持现有musttail ABI；只在Runtime内部进入HeapManager前显式计算并传递。

### 3.2 最终数据结构、容器与链表生命周期

#### 3.2.1 阅读口径与当前/最终边界

本节只描述 Step62 六项优化全部落地后的最终结构，但为了避免把当前源码中的容器误认为
最终方案，下表先给出一一对应关系。后续表格中的“新增”是指相对当前 MSGC 源码新增，
不是要求在最终版本中同时保留旧结构。

| 对象 | 当前源码 | Step62 最终结构 | 处理 |
|---|---|---|---|
| small Central bucket | 36 个 bucket；`partialSwept_` 为 lock-free stack，`fullSwept_ / partialUnswept_ / fullUnswept_` 为加锁 `MSRegionList` | `centralBuckets[36].lanes[2]`；每 lane 只有 `partialSwept / fullSwept / unswept` 三个 ABA-safe tagged head | 二维化并合并两条 unswept 链 |
| 本轮 small collect-set | 没有独立的、Mark 期冻结的 Region 链 | GC 私有 `collectSet[36][2]` | 新增；PreEnum 从 Central 批量窃取 |
| 普通 small 灰工作 | `WorkBuf`、`globalFull_`、`PerPGCWork`、`activeLog_` | `RegionReadyHead` + Region 三平面 | 删除普通对象 WorkBuf 主路径 |
| LARGE 管理 | `largeRegionList_` + `largeHotCache_` | `largeNextSupply`、GC 私有 `largeCollectSet`、`ObjectReadyHead`、`largeHotCache` | 分开“下一轮供给”“本轮边界”“Mark任务”“回收缓存” |
| EmptyRegion | `emptySweptCache_` 加锁双向链 | classless、支持按地址区间摘除的加锁容器 | 保留能力；不强改成只能 pop-head 的 tagged stack |
| TLS active Region | `activeRegions_[36]` | `activeRegions[36][2]` | 二维化；它是数组，不是链 |
| AllocBuffer 全局登记 | `msAllocBuffers_` vector + lock | 保留 registry，PreEnum 一次快照 | 它是注册表，不是 Region 链 |
| 空闲地址管理 | FreeManager shard bitmap/summary | 保留 shard bitmap/summary | 它不是 EmptyRegion 链 |
| `sweepState/sweepGen` | Region 内字段与 swept/unswept 容器同时表达状态 | 从 RegionInfo 删除；容器 membership + sweep in-flight admission 表达状态，Central 仅保留 `sweepCycleId` | 消除三套真值 |

本节统一使用以下术语：

| 术语 | 精确定义 |
|---|---|
| 链/stack | 至少包含一个外部 head，并由 Region 内的 next 字段连接节点 |
| registry/array/bitmap | 是容器或索引，但不是 intrusive Region 链；不能为了叙述方便称为“链” |
| head | 决定 next 字段当前代表哪一条链；只看到 Region 内 next 值不能判断其 membership |
| owner | 唯一允许推进该容器或任务状态的线程/阶段 |
| `SEALED` | “Region 已离开 TLS、alloc plane 已物化且本轮不再分配”的派生状态，不新增 `regionKind` 或 `allocMode` 枚举值 |
| bitmap authoritative | 该 plane 的位在当前状态下可作为正确性依据；例如 BUMP active 时 alloc plane 不是逐对象实时真值 |

因此，后文提到“Region 在 collect-set 中”时，判断依据是 collect-set 的外部 head、链路和
`collectEpoch`，不是 `centralNextRegionIdx != INVALID`；提到“Region ready”时，判断依据是
ready head、`scanState` 和任务账本，不是只检查 `markTaskNextRegionIdx`。

#### 3.2.2 数据结构总表

| 最终结构/容器 | 维度或宿主 | node/link | 主要作用 | 并发所有权/同步 | 一轮结束时 |
|---|---|---|---|---|---|
| `partialSwept` | `Central[36][2]` | small Region / `centralNextRegionIdx` | 向 BITMAP allocator 提供仍有空槽的已扫 Region | tagged head；MPMC push/pop | 可留作下一轮 allocation supply |
| `fullSwept` | `Central[36][2]` | small Region / `centralNextRegionIdx` | 保存当前无空槽、但已完成 Sweep 的 Region | tagged head；GC/归还路径 push，PreEnum steal | 下一轮 PreEnum 可进入 collect-set |
| `unswept` | `Central[36][2]` | small Region / `centralNextRegionIdx` | 保存 PostMark 后尚未完成 Sweep 的本轮 Region | tagged head；Sweep worker pop | 应被并发 Sweep 清空；下一轮 PreEnum 可 force-finish backlog |
| `collectSet` | GC 私有 `[36][2]` | small Region / `centralNextRegionIdx` | 冻结本轮 small Region 回收边界 | 仅 GC main 在 STW 构建/移交；Mark 期只读 membership | PostMark 后移交 `unswept` 并清空 |
| `RegionReadyHead` | MarkScheduler 全局单 head | small Region / `markTaskNextRegionIdx` | 发布 HAS_REF Region 的 `mark & ~scan` 工作 | tagged head；producer push、任意 worker pop | head 为空，所有 Region `scanState=IDLE` |
| `ObjectReadyHead` | MarkScheduler 全局单 head | LARGE primary / `markTaskNextRegionIdx` | 发布已赢得标记权的 HAS_REF 大对象扫描任务 | tagged head；producer push、任意 worker pop | head 为空，所有 object task 已完成 |
| `TaskLedger` | MarkScheduler | `atomic<uint64_t> outstandingTotal`，不是链 | 精确覆盖queued、in-flight和provisional task；worker本地batch由其当前TaskLease覆盖 | release/acquire原子协议 | STW下为0且ready/batch均空才允许终止 |
| `largeNextSupply` | LargeObjectSpace | `vector<uint32_t>`，元素为 LARGE primary index | 保存边界之后的新 LARGE 和上一轮存活 LARGE | 唯一 `largeLock_` 下 append/swap | 成为下一轮 PreEnum 的输入 |
| `largeCollectSet` | GC 私有 | `vector<uint32_t>`，元素为 LARGE primary index | 冻结本轮大对象回收边界 | PreEnum 与 `largeNextSupply` swap；Mark 期只读 | live 回 `largeNextSupply`，dead 进 hot/free 路径 |
| `largeHotCache` | LargeObjectSpace，按 span count 精确分桶 | `map<spanCount, vector<LargeHotEntry>>` | 暂存已清理、可快速复用的大连续 Region | 独立 `largeHotCacheLock_` | 可跨轮保留或转 FreeManager |
| classless Empty cache | RegionManager | `head/count + centralNextRegionIdx` intrusive singly list | 保存完全空、已清元数据的单 Region | 专用 mutex；不进入 size/scan lane | 可供新绑定或 Scavenge |
| `activeRegions[36][2]` | 每个 `AllocBufferMS` | 无 link；每格一个 Region 指针/index | TLS BUMP/BITMAP allocation owner | TLS owner；PreEnum 只做 registry 快照，不 flush | 可继续 active 到下一轮 |
| `msAllocBuffers_` | Allocator 全局 registry | vector element，不是 Region link | 枚举当前全部 TLS buffer | 注册/注销加锁；PreEnum 一次快照后遍历 | 长生命周期保留 |
| FreeManager shard bitmap | FreeManager | `allocBits/scavengedBits/scavengingBits` + summaries | 管理可提交/已反提交/正在反提交的地址区间 | shard 级原子或锁协议 | 跨轮保留 |
| root staging | 每个`AllocBufferMS::stackRoots_` + GC预分配`RootOverflowChunkPool` | `vector<BaseObject*>`和固定chunk都只保存root target对象地址，不是slot或WorkBuf node | mutator safepoint EnumRoots短期汇聚；vector容量不足时overflow | mutator枚举时单owner；GC main在持有mutator registry快照时drain | EnumRoots返回前vector/chunk必须全部转成Mark工作并清空 |
| WeakRef discovery buffer | worker-local vector | WeakRef 地址 | 记录 owner 扫描时发现的 WeakRef，供 STW 处理 | worker-local；终止前汇总 | MarkTermination 中 drain/分类 |
| ordinary Mark `WorkBuf` 链 | —— | —— | 最终无此结构 | `globalFull_`、pool、per-P get/put、`activeLog_` 全删 | 不存在 |

这里的 `largeNextSupply/largeCollectSet` 不设计成 intrusive 链，而是两个只保存 primary
Region index 的 side vector；PreEnum 用 swap 建边界。这样不为 LARGE 再增加 RegionInfo
link，也不会与 small `centralNextRegionIdx` 发生角色复用。不能只保留一个可变
`largeRegionList_` 再靠遍历时猜测对象属于哪一轮。

#### 3.2.3 两个Region link、head与允许同时存在的membership

`MSRegionInfo` 只提供两个可复用 node link，链的身份由外部 head 决定。最终不能增加
`centralRole`、`readyRole` 或 “isInXXXList” 等重复状态字段。

| RegionInfo 字段 | 可被哪些 head 使用 | 同一时刻的约束 | 摘除后的要求 |
|---|---|---|---|
| `centralNextRegionIdx` | 某一条 `partialSwept/fullSwept/unswept`；或某一条 GC 私有 `collectSet`；或classless Empty单链 | 一个 Region 至多属于这些容器中的一个 | 在重新入链前写 `INVALID_REGION_IDX`；Debug 检查旧链已失去所有权 |
| `markTaskNextRegionIdx` | `RegionReadyHead` 或 `ObjectReadyHead` | small 只能进 REGION ready；LARGE primary 只能进 OBJECT ready；同一任务不得重复入链 | owner pop 后即可置 INVALID；是否仍 owned 由 `scanState`/TaskLedger 表达 |

两个 link 分开的直接原因是同一 Region 允许同时具有“两种正交 membership”：

```text
GC collectSet[sc][scan] head ──centralNext──> R ──centralNext──> ...
                                                │
Mark RegionReadyHead        ──markTaskNext──────┘
```

上图中的 `R` 同时是“本轮回收边界成员”和“当前有扫描工作”，但它没有同时进入两条
Central/collect-set 链，也没有同时发布两次 ready task。

| 组合 | 是否允许 | 原因 |
|---|---|---|
| small `collectSet` + `RegionReadyHead/OWNED` | 允许 | 一个表达回收边界，一个表达动态 Mark 工作 |
| OUTSIDE_SET `partialSwept` + `RegionReadyHead/OWNED` | 允许 | 本轮不回收的新 Region 仍可能被旧对象引用，必须完成可达图扫描 |
| LARGE `largeCollectSet` + `ObjectReadyHead/OWNED` | 允许 | large registry 使用独立 membership；Mark ready 使用 `markTaskNextRegionIdx` |
| 同一 small Region 同时在 `partialSwept` 与 `collectSet` | 禁止 | 二者共用 `centralNextRegionIdx`，且 allocator 与 GC 会争夺 Region |
| 同一 small Region同时在两条 Central lane/状态链 | 禁止 | `sizeClass/scanClass` 稳定且一个 link 只能有一个 owner |
| 同一 Region 同时出现两次 `RegionReadyHead` | 禁止 | `IDLE -> QUEUED` 的唯一状态转换负责去重 |
| 同一 LARGE primary 同时出现两次 `ObjectReadyHead` | 禁止 | `LARGE_MARKED` winner 负责去重 |

Central 和 ready 的全局 head 使用带 version/tag 的 CAS，push 以 release 发布之前对 RegionInfo、
bitmap 和 node link 的写入，pop 以 acquire 接收。两个 node link 本身使用 relaxed atomic，
目的不是给链提供发布语义，而是避免快速 pop、清 link、复用和 re-push 之间出现 C++ data
race。GC 私有 collect-set 的 head/tail/count 只由 GC main 修改；其中 node link 仍保持
atomic 类型，以免同一字段在不同阶段切回 lock-free Central 时改变内存对象类型。

任何 `next != INVALID` 都不能单独作为“正在链中”的判断：尾节点合法地为 INVALID，pop 后
尚未清 link 的节点也可能保留旧值。Release 路径从外部 head 与状态协议推导 membership；
只有 Debug 构建可以在 safepoint 遍历链生成 side membership set 做重链、环和重复节点检查。

#### 3.2.4 Central bucket与三条small Region链

最终每个 `(sizeClass, scanClass)` lane 只保存三个 head：

```cpp
struct MSCentralLane {
    TaggedRegionHead partialSwept;
    TaggedRegionHead fullSwept;
    TaggedRegionHead unswept;
};

MSCentralLane centralBuckets[36][2];
```

`partial/full` 只描述**最近一次 Sweep 或归还后的空槽结果**；`swept/unswept` 只描述
**当前能否被 allocator 使用**。它们不再与 Region 内 `sweepState/sweepGen` 组成重复判断。

| 源状态/触发点 | 操作 | 目标容器 | 添加节点时写什么 | 移除节点后谁拥有 |
|---|---|---|---|---|
| TLS active Region 被归还且仍有空槽 | BUMP 则先 materialize；固定 `allocMode=BITMAP`；release push | 对应 lane `partialSwept` | `freeSlotCount>0`、`allocSummary`、`centralNext` | 后续 refill 或下一轮 PreEnum |
| TLS active Region 被归还且已满 | 同上，但无 allocation capacity | 对应 lane `fullSwept` | `freeSlotCount=0`、`centralNext` | 下一轮 PreEnum |
| allocator refill | acquire pop；校验 lane 与 `freeSlotCount>0` | 从 `partialSwept` 摘除 | 清 `centralNext`；不改 `collectEpoch` | 唯一 TLS owner |
| PreEnum STW | 对每个 lane 原子 steal `partialSwept`、`fullSwept` 两个 head | GC 私有 `collectSet[sc][scan]` | 由 GC main 重接 `centralNext`、写 current epoch | 本轮 GC |
| Mark/PostMark 完成 | 按 lane 整链移交，不按原 free count 再分类 | `unswept` | release 发布 Mark 结果和 `centralNext` | Sweep worker |
| Sweep worker claim | acquire pop 一个 Region | 从 `unswept` 摘除 | 清 `centralNext`；获得唯一 sweep admission | 该 Sweep worker |
| Sweep 后仍有空槽 | 重建 alloc plane/summary 后 release push | `partialSwept` | `freeSlotCount>0`、`allocMode=BITMAP` | allocator/下轮 GC |
| Sweep 后无空槽但仍有存活对象 | release push | `fullSwept` | `freeSlotCount=0`、`allocMode=BITMAP` | 下轮 GC |
| Sweep 后无存活对象 | Reset class/GC metadata | classless Empty cache | `regionKind=FREE`、`allocMode=NONE` | RegionManager |

PreEnum 时 mutator 已停止，因此 steal 与 TLS refill 不并发。边界前已被 TLS pop 的 Region
留在 active 数组，不进入 collect-set；边界时仍在两个 swept head 中的 Region 被整批
窃取；STW 退出后归还或新绑定的 Region 进入已经重新开放的 swept supply，天然属于
OUTSIDE_SET。这个容器切换本身就是分配边界，不需要 `isCollecting` 分支或逐 Region 锁。

`unswept` 不再拆成 partial/full，因为旧空槽信息在 Sweep 前不是下一次分配的可靠依据；
只有重建 `alloc = ~mark & validMask` 后才能重新分类。如果下一轮 PreEnum 到达时
`unswept` 仍有backlog，PreEnum一律关闭admission、等待active=0并由GC线程串行
force-finish；`CJ_MS_STW_SWEEP=1`只是让整个本轮backlog都主动延后到该路径。任何模式都
绝不能把unswept直接拼进新collect-set。

#### 3.2.5 GC私有collect-set链

固定使用简单的GC私有链描述边界，不再造第二套并发list：

```cpp
struct RegionChain {
    uint32_t head = INVALID_REGION_IDX;
    uint32_t tail = INVALID_REGION_IDX;
    size_t count = 0;
};

RegionChain collectSet[36][2];
```

它复用 `centralNextRegionIdx`，但 head/tail/count 只由 GC main 在 safepoint 或已经
quiesce 的阶段修改，所以不需要 tagged head、CAS、mutex 或每 Region membership bit。

| 生命周期阶段 | collect-set 动作 | Region/plane 动作 | 禁止发生 |
|---|---|---|---|
| 上一轮 Sweep quiescence | 确认旧 `collectSet` 已空、`unswept` 不再被旧 worker持有 | 确认无旧 Mark task/lease | 新旧轮共用一个 Region |
| PreEnum STW 建边界 | steal 每个 lane 的 `partialSwept/fullSwept`，按 lane 拼为一条链并记录 count | 写 `collectEpoch=currentMarkEpoch`；clear mark；HAS_REF 再 clear scan/pending，置 `scanState=IDLE` | flush TLS active；遍历对象；复制全堆 alloc bitmap |
| ConcurrentMark | head/tail/link/membership 全部只读 | alloc plane 只读；mark 原子 OR；scan 由唯一 owner 写 | allocator pop collect-set Region；Sweep 读取/修改它 |
| MarkTermination STW | 仍保持链和 lane 分类不变 | drain ready/lease，得到 Mark fixed point | 根据旧 `freeSlotCount` 提前分流 |
| PostMarkTermination | 以 lane 为单位将链 release-publish 到对应 `unswept` | Mark/alloc 发布给 Sweep；collect-set head/tail/count 清零 | 把 Region直接放回 swept allocation supply |
| Sweep | collect-set 已不再拥有节点 | unswept pop 成为唯一 claim；按 Mark 重建 alloc | 重新检查 `collectEpoch` 决定是否 Sweep |

`collectEpoch` 的用途是让 barrier/DiscoverRef 对一个候选地址做 O(1) 的角色分流：

```text
collectEpoch == currentMarkEpoch  -> COLLECT_SET，可读取 stable alloc plane
collectEpoch != currentMarkEpoch  -> OUTSIDE_SET，禁止读取可能仍在变化的 alloc plane
```

它只替代“本轮 Mark membership”的 generation 判断，不替代 Sweep claim/completion：
unswept head 是待扫真值，成功 pop/admission 是 claim 真值，push 到 swept/Empty 是完成真值；
跨轮 pacing、worker 和统计使用 Central 级 `sweepCycleId`。因此最终删除 Region 内
`sweepGen/sweepState` 后，Sweep 仍有完整的代际转换协议。

在 collect-set 中，以下元数据整个 Mark 周期保持稳定：`regionKind=SMALL`、
`sizeClass/scanClass`、`allocMode=BITMAP`、alloc plane 和 `collectEpoch`。mark/scan/control/
pending 是本轮动态工作状态，不能误称为 collect-set 快照的一部分。

#### 3.2.6 RegionReady与ObjectReady链

两个 ready lane 都是 ABA-safe tagged intrusive stack：

```cpp
TaggedRegionHead regionReadyHead;
TaggedRegionHead objectReadyHead;
```

head 保存 `(regionIdx, abaTag)`；node 都使用 `markTaskNextRegionIdx`。两条链共享
TaskLedger，但任务去重方式不同。

| 任务 | node | 唯一发布条件 | pop 后的 owner | 完成条件 |
|---|---|---|---|---|
| REGION | SMALL HAS_REF Region | `pendingWordSummary!=0` 且唯一赢得 `IDLE -> QUEUED` | CAS `QUEUED -> OWNED` 的 worker | summary 已消费、本地 MarkBatch 已 flush、release/recheck 没有遗漏 |
| OBJECT | LARGE HAS_REF primary | 唯一赢得 `LARGE_MARKED`，并确认对象确有引用 | 持有该 Object TaskLease 的 GC main/helper | 整个大对象引用枚举完成、本地 MarkBatch 已 flush |

REGION task 的完整生命周期如下：

| 时刻 | ready head/link | `scanState` | pending/plane | TaskLedger |
|---|---|---|---|---|
| 本轮工作区初始化 | 不在链；link=INVALID | IDLE | mark/scan/pending 清零 | 该Region无task；全局ledger在cycle初始化时为0 |
| MarkBatch flush 产生 HAS_REF `newBits` | 尚未必入链 | IDLE/QUEUED/OWNED | release OR 对应 pending word bit | worker由当前TaskLease覆盖；root由EnumRoots阶段覆盖；bulk由无safepoint覆盖写协议覆盖 |
| producer 赢得发布权 | release push 一次 | `IDLE -> QUEUED` | pending 非零或由发布窗口保护 | 新 task admission 已计账 |
| worker acquire pop | 从 head 摘除，link 清 INVALID | `QUEUED -> OWNED` | acquire 接收 mark/pending | queued 变 in-flight，不减账 |
| owner 扫描 | 不在 head | OWNED | exchange pending；扫描 `mark & ~scan`；完成对象后置 scan | 旧 task lease 持续存在 |
| slice 到期且仍有工作 | 为replacement建provisional admission后release push | `OWNED -> QUEUED` | 未完成 word 重新 OR pending | replacement先+1，发布后旧lease才Complete，计数不瞬时归零 |
| 当前看似完成 | 不在 head | 尝试 `OWNED -> IDLE` | acquire 重读 pending，处理 lost-wakeup 窗口 | 尚未安全时不能 complete |
| release 后发现新 pending | release push 一次 | 唯一赢得 `IDLE -> QUEUED` | pending 保留 | 新 task 计账 |
| 真正完成 | link=INVALID | IDLE | `mark & ~scan == 0`，或新工作已有 task/lease 覆盖 | flush child 后 complete，减一 |

producer 在 `QUEUED` 或 `OWNED` 时只提交 mark/pending，不再 push 第二个 Region token。
因此多个 worker可同时命中同一 Region、同一 word，最终仍只有一个 scan owner；不同
Region 各自发布一个 token，可以并行扫描。

OBJECT task 不使用 small scan plane，也没有 `IDLE/QUEUED/OWNED` 状态机。LARGE primary
的原子 `LARGE_MARKED` winner 同时完成“本轮首次发现”和“唯一 task admission”。NO_REF
LARGE winner 只置 marked，不进 `ObjectReadyHead`；HAS_REF LARGE 先增加 outstanding，
再 release push。pop 后一次扫描完整对象，产生的 child 进入 worker-local MarkWordBatch，
flush 后 complete。由于本步骤排除 RangeQueue，mutator assist 不领取 OBJECT。

周期结束前必须同时满足：

```text
regionReadyHead empty
objectReadyHead empty
outstandingTotal == 0
all worker-local MarkBatch empty
no worker/assist still owns a TaskLease
```

只检查两个 head 为空会漏掉已 pop 但仍在扫描的 owner，也会漏掉尚未 flush 的 batch；
因此 ready 链负责“可领取任务”，TaskLedger 才负责“任务图是否真正归零”。

#### 3.2.7 LARGE primary、SPAN与大对象容器

LARGE 不进入 36×2 small Central，也不使用三平面。最终用两个 side vector 建立周期边界：

```cpp
std::vector<uint32_t> largeNextSupply;  // mutator/上一轮live写入
std::vector<uint32_t> largeCollectSet;  // 本轮Mark期间只读
```

vector 中只登记 LARGE primary 的 `regionIdx`。SPAN follower 通过自身
`pendingWordSummary` 中的 `baseRegionIdx` 回溯 primary，不独立登记、不独立标记、
不独立 Sweep。PreEnum STW 在 `largeLock_` 下 swap 两个 vector，然后只对
`largeCollectSet` 写 current epoch 和清 `LARGE_MARKED`；后续新 LARGE 继续 append 到已经
清空的 `largeNextSupply`。

锁协议固定为：

- LARGE分配先完成primary/SPAN metadata初始化，再持`largeLock_` append
  `largeNextSupply`，释放锁后才向mutator发布对象；
- PreEnum已STW，但仍持`largeLock_`执行两个vector的O(1) swap，随后
  `largeCollectSet`只归GC main所有；
- PostMark遍历GC私有`largeCollectSet`，把live primary先收集到本地vector，然后只获取
  一次`largeLock_`批量append到当前`largeNextSupply`；mutator在此期间可以正常逐个append；
- dead primary先在无registry锁时完成整span Reset，再持`largeHotCacheLock_`批量加入精确
  span-count bucket；
- `largeLock_`与`largeHotCacheLock_`不嵌套；需要跨容器迁移时先形成本地batch，释放一个
  锁后再取得另一个。

| 容器/角色 | 添加节点的时机 | 移除节点的时机 | 节点状态/用途 |
|---|---|---|---|
| `largeNextSupply` | 新大对象完成 primary+SPAN 初始化后；PostMark 判断本轮 primary live 后 | PreEnum 整 vector swap 到 `largeCollectSet` | 下一轮边界输入；当前 GC 不回收 |
| `largeCollectSet` | PreEnum 与 next-supply swap | PostMark 逐 primary 分类后 clear | 本轮不可变化的 LARGE 回收边界 |
| `ObjectReadyHead` | HAS_REF LARGE 唯一赢得 `LARGE_MARKED` 后 | GC main/helper acquire pop | 大对象引用枚举任务；不含 SPAN |
| `largeHotCache[spanCount]` | dead LARGE完成整span清理后；或显式释放的可复用连续区间 | 只由完全相同spanCount的大对象allocation精确命中；或由Scavenge drain | 地址+spanCount的热连续内存，不做best-fit/split/coalesce，不是live LARGE |
| FreeManager | hot cache 冷却/超限或 Scavenge 移交后 | 重新申请连续 Region | 地址可用性与 scavenged 状态；不保留 LARGE membership |

大对象的完整状态迁移如下：

| 阶段/触发 | primary | follower SPAN | membership | Mark状态 |
|---|---|---|---|---|
| 从hot/free连续区间分配 | `regionKind=LARGE`；`allocPtr`写span/internal起址，写完整span end、logical bytes、ScanClass；`collectEpoch!=current` | `regionKind=SPAN`；写`baseRegionIdx` | primary append `largeNextSupply` | `LARGE_MARKED=0`；公开对象地址为`allocPtr+HEADER_SIZE`；无small plane |
| PreEnum 边界 | metadata/layout 保持稳定；写 `collectEpoch=current` | 保持 base 指向 primary | next-supply swap 为 `largeCollectSet` | 清本轮 primary 的 marked flag |
| 引用首次命中 primary 或任一 SPAN 地址 | 统一解析到 primary并原子设置 marked | 不独立变更 | membership 不变 | NO_REF 到此结束；HAS_REF 发布一个 OBJECT |
| OBJECT owner 扫描 | 从 primary 推导对象地址/TypeInfo和精确 logical bytes | 只用于地址范围解析 | 仍在 collect-set 或 next-supply | 子引用批量提交；整对象完成后 task complete |
| PostMark：marked | metadata 不变 | base 不变 | 从 `largeCollectSet` append 回 `largeNextSupply` | 下轮 PreEnum 再清 |
| PostMark：unmarked | 清对象内容和 primary metadata，Reset 为 FREE | 全部 follower Reset 为 FREE | 从 collect-set 移除，按 spanCount 进 `largeHotCache` | flag/epoch/link 清理 |
| hot cache 被复用 | 重新初始化为新的 primary | 重新建立 SPAN base | append next-supply | 新对象重新从 unmarked 开始 |
| hot cache 被 Scavenge | 保持 FREE | 保持 FREE | 移交 FreeManager shard bitmap | 不读任何 Mark 元数据 |

`largeCollectSet` 与 small collect-set 概念相同：都是 PreEnum 边界前的可回收集合；差别只在
容器和扫描粒度。small 用 `centralNextRegionIdx` 链 + REGION owner，LARGE 用 primary
side vector + OBJECT task。LARGE 地址命中 SPAN 时必须先回溯 primary，再判断
`collectEpoch`；不能把 SPAN 当成一个独立对象发布，也不能把它塞入 small REGION ready。

dead large 的整 span 必须在放入 hot cache **之前**完成 metadata Reset；否则新的 large
allocation 可能看到旧 `baseRegionIdx`、epoch 或 marked flag。反过来，live primary 回
`largeNextSupply` 时不得清 SPAN base，因为下一轮前地址解析仍依赖它。

#### 3.2.8 EmptyRegion、TLS与FreeManager容器

这三类结构处在不同所有权层次，不能合成一条“空闲 Region 链”：

| 结构 | 实际形态 | 添加/建立 | 摘除/销毁 | 状态与同步 |
|---|---|---|---|---|
| classless Empty cache | 专用锁保护的 intrusive singly list；head/count + `centralNextRegionIdx` | small Sweep 得到 `liveCount=0`，或 TLS 归还完全空 Region；先 Reset metadata | 新 size/scan lane 绑定时 take-head；Scavenge/idle-segment reclaim 按批或地址区间摘除 | 节点 `regionKind=FREE, allocMode=NONE, sizeClass=INVALID`；锁内改链和 count |
| TLS active slots | `AllocBufferMS::activeRegions[36][2]` 固定数组 | refill 从 `partialSwept` 取 BITMAP Region，或从 Empty/FreeManager 绑定新的 BUMP Region | 满/切换/线程退出时归还 Central；完全空时可归 Empty | 只有 TLS owner 改 slot 与 allocation metadata；没有 next link |
| AllocBuffer registry | `msAllocBuffers_` vector | buffer 初始化/线程 attach 时登记 | buffer fini/线程 detach 时注销 | registry lock 只保护登记；PreEnum 加锁复制一次后释放 |
| FreeManager | 每 shard 的 `allocBits/scavengedBits/scavengingBits`、summary、cursor | Empty/large-hot 被提升为通用空闲地址时 reclaim bits | small/large 申请连续 Region 时 claim bits | shard lock/既有原子快照协议；不读 small 三平面 |

Empty cache 保留锁是有意的。当前 Scavenge/idle-segment shrink 需要把某个地址区间中的
EmptyRegion 摘出；仅有 lock-free pop-head 无法高效、可靠地完成这个操作。最终固定在
锁内单次遍历并重链：命中节点移交 FreeManager，未命中节点保持顺序。这里不再保留
RegionInfo `prevRegionIdx`，所以不是双向链；范围摘除是冷路径，O(n) 可接受。

Empty 节点的生命周期是：

```text
SMALL swept live=0
  -> clear object/size/scan/epoch/control/三个plane有效范围
  -> FREE(classless) + Empty cache
  -> take and bind(sizeClass, scanClass)
  -> SMALL ACTIVE(BUMP)
```

若进入 Scavenge，则路径为：

```text
Empty cache
  -> Reset/移交 FreeManager hot-free
  -> claim scavenging：allocBit=1, scavengingBit=1
  -> OS release完成：allocBit=0, scavengingBit=0, scavengedBit=1
  -> 后续allocation claim：allocBit=1, scavengedBit=0，并按需重新commit
```

FreeManager 的 `allocBit=1` 表示该地址已被 allocation 或 Scavenge claim，`0` 表示可取；
`scavengedBit=1` 表示物理页已归还 OS；`scavengingBit=1` 表示 release 正在进行。它们是
地址资源状态，不是对象级 alloc/mark/scan plane，命名相同也不能混用。

PreEnum 对 TLS 的操作只有：

1. 在 STW 下 snapshot `msAllocBuffers_`；
2. 枚举各 buffer 的 `activeRegions[36][2]`，清新一轮的临时 mark/scan/pending/control；
3. 不读取或物化其 alloc plane，不 detach，不归还 Central，也不构造
   `ActiveRootSnapshot`；
4. `collectEpoch!=current`，所以后续可达访问走 OUTSIDE_SET 验证和扫描；
5. 如果下一轮 PreEnum 时它仍 active，仍然继续等待，不强制 flush。

ACTIVE Region 不是额外 GC Root。GC Root、SATB deletion barrier 和对象图闭包决定
可达性；TLS registry 只用于重置本轮 GC 工作区并保证 Region 生命周期不被误回收。

#### 3.2.9 WorkBuf及其他保留的临时容器

最终 MSGC 的普通 Mark 路径没有“还存在的 WorkBuf 链”。需要保留的是若干用途明确、
生命周期受 TaskLease/STW 约束的短期缓冲，它们不能重新演化为第二套灰对象队列。

| 当前或候选结构 | 最终处理 | 替代/保留结构 | 生命周期 |
|---|---|---|---|
| `globalFull_` lock-free WorkBuf stack | MSGC 删除 | `RegionReadyHead/ObjectReadyHead` | 两个 ready head 仅在 ConcurrentMark 到 MarkTermination 存活 |
| `WorkBufPool` 的普通 Mark buffer pool | MSGC 删除 | RegionInfo node link，无 task node allocation | 不再申请/归还 WorkBuf |
| `PerPGCWork` 的 get/put/full/empty 链 | MSGC 删除 | worker 调用统一 `RunMarkSlice` | worker 不持有跨 slice 的对象队列 |
| GC main/helper 的 `gcWork_`/local WorkBuf | MSGC 删除 | 固定容量 `MarkWordBatch` | entry eviction、slice/lease/phase边界前 flush |
| `AllocBufferMS::activeLog_` | 删除字段和 flush 路径 | deletion barrier 直接 shade，bulk barrier 覆盖写前强制 flush | 不跨 safepoint 保存旧引用 |
| `AllocBufferMS::stackRoots_` | 保留现有专用root staging vector | root target对象地址经 `DiscoverRef` 进入MarkWordBatch/OBJECT | EnumRoots 当前批次内drain并clear，不跨ConcurrentMark |
| worker-local `MarkWordBatch` | 新增但不是链 | 固定16个 `(regionIdx, wordIdx, mask)` entry，8组×2路 | 受 producer/TaskLease 覆盖；所有退出边界前为空 |
| worker-local WeakRef discovery vector | 保留 | owner 只登记 WeakRef，不沿 referent 强扫描 | worker-local append；MarkTermination STW merge/deduplicate |
| Finalizer candidate list | 保留现有注册表语义 | GC main 在 STW 分类/发布 resurrection root | 不进入 ready head，不创建 MAIN_ONLY lane |
| CopyGC 自身 WorkBuf/packet | 不属于 Step62 MSGC 改造范围 | CopyGC 编译分支保持 | 不得因删除 MSGC WorkBuf 误删 CopyGC 实现 |

root staging 只用于每个mutator在safepoint枚举得到的栈/寄存器root target；沿用当前
`AllocBufferMS::stackRoots_`，元素固定为 `BaseObject*`，不保存root slot。全局、导出表和
Runtime特殊root由GC main直接调用 `DiscoverRef`，不绕入mutator staging；SATB old edge也
直接走barrier的MarkWordBatch/标记发布协议，不进入root staging。

GC main在EnumRoots中遍历已冻结的alloc-buffer registry，逐个drain `stackRoots_`，转成
mark plane、pending/ready 或OBJECT task，然后clear但保留vector capacity。为防止
`emplace_back`失败而丢root，每个buffer在非GC阶段按上轮high-water预留容量；仍溢出时走
预分配的GC root overflow chunk。chunk pool耗尽时允许在MARK_SETUP safepoint慢路径同步
申请native chunk；申请失败直接以root-staging OOM fatal终止进程，禁止忽略该root后继续
GC，也不临时恢复WorkBuf。overflow chunk通过专用root链交给GC main，不进入Mark ready
head。EnumRoots返回前必须证明所有staging和overflow chunk均为空。WeakRef discovery的
添加点是REGION/OBJECT owner识别特殊对象，消费点是全部Mark task quiesce后的STW弱引用
处理。

最终禁止保留以下兼容路径：

```text
Mark winner -> 有时进 WorkBuf，有时置 pending
Region owner -> 扫描一半后把普通 child object 塞回 WorkBuf
queue empty -> 再去 WorkBuf/activeLog 猜是否仍有工作
```

这些分支会重新引入双任务模型和双终止条件。迁移期可以用编译开关做 A/B，但最终
Release 二进制只能有 Region/Object ready + TaskLedger 一套普通 Mark 真值。

#### 3.2.10 SMALL Region容器、状态与Bitmap迁移

SMALL 没有新增 `SEALED/PARTIAL/FULL/SWEPT/UNSWEPT` 状态枚举；这些状态全部由
`regionKind + allocMode + 当前容器/owner` 推导。

| 触发点 | 源 → 目标 | `regionKind/allocMode` | membership/link | allocation owner |
|---|---|---|---|---|
| Empty/Free 绑定 lane | Empty/Free → TLS active | `FREE/NONE -> SMALL/BUMP`；固定 sizeClass/scanClass | 从 Empty/FreeManager 摘除；两个 link=INVALID | 唯一 TLS |
| BUMP 分配 | TLS active → TLS active | `SMALL/BUMP` 不变 | 无链 | TLS 只推进 `allocPtr` |
| BUMP active 归还 | TLS active → Central | 先 materialize，再 `SMALL/BITMAP` | partial/full swept release push `centralNext` | 从 TLS 交给 Central |
| BITMAP refill | Central partial swept → TLS active | `SMALL/BITMAP` 不变 | acquire pop，清 `centralNext` | 唯一 TLS |
| BITMAP active 归还 | TLS active → Central | `SMALL/BITMAP` 不变 | 按 free count push partial/full swept | 从 TLS 交给 Central |
| PreEnum 建边界 | Central partial/full swept → GC collect-set | 必须已经 `SMALL/BITMAP` | steal 后用 `centralNext` 重接 collect-set | allocator 不再拥有 |
| PostMark 移交 | collect-set → Central unswept | `SMALL/BITMAP` 不变 | collect-set link 整批变为 unswept link | Sweep 尚未 claim |
| Sweep claim/完成 | unswept → in-flight → partial/full swept | `SMALL/BITMAP` 不变 | pop 是 claim；结果 release push | 唯一 Sweep worker后交回Central |
| Sweep `liveCount=0` | unswept → in-flight → Empty | `SMALL/BITMAP -> FREE/NONE`；class fields失效 | 不再进 swept lane，改进 Empty | RegionManager |
| Scavenge/shrink | Empty/large-hot → FreeManager | 保持 `FREE/NONE` | 离开 intrusive cache，进入 shard bitmap | FreeManager |

allocation plane/summary 的变化：

| 时机 | alloc plane | `allocSummary` / `freeSlotCount` | 是否 authoritative |
|---|---|---|---|
| 绑定 fresh BUMP | 有效范围初始化为干净值，但不逐次跟随 BUMP allocation | 可保存容量/派生值，不允许 bitmap allocator读取 | 否；`allocPtr` 是唯一分配真值 |
| BUMP allocation | 不写 | 只更新 TLS 所需计数或由指针差推导 | 否 |
| BUMP materialize | 已分配前缀写0，未分配合法slot写1，padding写0 | 每个含 free bit 的 word 置 summary；精确统计free bits | 是 |
| BITMAP allocation | owner把选中 slot 从1清为0 | word无free bit时清summary；free count递减 | 是 |
| collect-set Mark | 只读 `allocated=(~alloc)&valid` | 只读 | 是且整个 Mark 周期稳定 |
| Sweep | `alloc=(~mark)&validMask` | 重算每word summary和总free count | 是 |
| Reset为Empty/FREE | 清有效范围，防止下次绑定读到旧slot角色 | 清0 | 当前不参与对象判断 |

GC mark/scan 工作区的变化：

| 时机 | `collectEpoch` | mark plane | scan plane | pending/control |
|---|---|---|---|---|
| PreEnum 从 Central steal | 写 current | 清本 Region 有效 mark words | HAS_REF清有效scan words；NO_REF不触页 | pending=0，state=IDLE，task link=INVALID |
| PreEnum 枚举 TLS active | 保证不等于 current | 清临时 visit words | HAS_REF清临时scan words | pending=0，state=IDLE，task link=INVALID；不碰alloc |
| PreEnum 后新绑定 Region | 写 `NO_COLLECT_EPOCH` 或其他非current值 | 发布对象前清有效范围 | HAS_REF清有效范围 | 清0/IDLE |
| DiscoverRef 命中 collect-set | 保持 current | batch flush先mask allocated，再原子OR | 不写 | HAS_REF winner置pending并可能发布REGION |
| DiscoverRef 命中 OUTSIDE_SET | 保持非current | 精确验证对象后原子OR visit bit | 不写 | HAS_REF winner同样置pending/发布 |
| REGION owner扫描 | 不写 | acquire读取 | 对每个完整枚举引用字段的对象置bit | exchange/recheck pending；OWNED/QUEUED/IDLE转换 |
| MarkTermination | 不写 | fixed point，不再有未发布winner | HAS_REF满足 `mark & ~scan==0` 或特殊语义已处理 | heads空、state IDLE、ledger=0 |
| collect-set Sweep | 不用于claim | 作为 live 真值读取并暂时保留 | 不读 | Mark任务生命周期已经结束 |
| OUTSIDE_SET 留在TLS/Central | 保持非current | 可保留到下轮PreEnum再清 | 同左 | 必须已回IDLE且pending=0 |
| Reset为Empty/FREE | 清0 | 清有效范围 | 清有效范围 | 清0、IDLE、两个link失效 |

NO_REF SMALL 仍使用 mark plane，因为 Sweep 需要 live bit；它不使用 scan plane、
`pendingWordSummary` 或 ready state。collect-set HAS_REF 的 scan bit 只在对象的全部强引用
字段已枚举后设置；child 尚在 local batch 时由当前 TaskLease 覆盖，lease 结束前必须 flush。

`NO_COLLECT_EPOCH` 固定为0，正常`currentMarkEpoch`从1开始且普通轮次不得发布0。
`collectEpoch` 是32位时，回绕不能让OUTSIDE_SET的旧值意外等于新current epoch。推进即将
回到0时，PreEnum STW对全部Region side metadata做一次全量归零，然后把新一轮epoch设为1；
这个极低频协议不进入barrier热路径。

#### 3.2.11 LARGE/SPAN容器与状态迁移

3.2.7 描述容器；本表固定每次迁移时 RegionInfo 字段的变化。LARGE/SPAN 不提交 small
alloc/mark/scan plane，它的 Mark 真值只有 primary `control.LARGE_MARKED`。

| 迁移 | primary字段 | SPAN字段 | side容器/ready | 发布顺序 |
|---|---|---|---|---|
| FREE连续区间 → 新LARGE | `layout=LARGE+scanClass`；写 `allocPtr/regionEnd/logicalObjectBytes`；`collectEpoch!=current`；marked=0；taskLink=INVALID | 每个 follower `layout=SPAN`；写 `baseRegionIdx`；其他对象字段无效 | primary append `largeNextSupply` | 先初始化全部 primary/follower metadata，再把对象发布给mutator |
| `largeNextSupply` → 本轮边界 | layout/span字段不变；写 current epoch；清 marked | base保持不变 | PreEnum vector swap 到 `largeCollectSet` | STW + large lock，退出前全部可见 |
| 首次 DiscoverRef | CAS marked 0→1；从稳定layout得ScanClass | 若命中SPAN先只读base并转primary | NO_REF不入ready；HAS_REF先记ledger再push `ObjectReadyHead` | marked winner在前，task release-push在后 |
| 重复 DiscoverRef | marked已为1，不读TypeInfo、不再发布 | 同样转primary | 容器/ledger不变 | relaxed/acq_rel按control协议 |
| OBJECT acquire pop | metadata只读；taskLink清INVALID | 不变 | ready → in-flight TaskLease | acquire接收winner前的metadata |
| OBJECT扫描完成 | marked保持1 | 不变 | flush child batch后complete lease | child admission/push先于parent complete |
| PostMark live | metadata和SPAN关系保持 | base保持 | `largeCollectSet` → `largeNextSupply` | append完成后再清collect-set vector |
| PostMark dead | 清对象、epoch、control、taskLink；primary Reset FREE | follower清base并Reset FREE | `largeCollectSet` → `largeHotCache[spanCount]` | 整span Reset先于hot-cache发布 |
| hot复用 | 重新执行“新LARGE”初始化 | 重建全部base | hot-cache → next-supply | 不继承旧marked/epoch |
| hot Scavenge | 保持FREE | 保持FREE | hot-cache → FreeManager bits | 离开cache后才能claim/release页面 |

`collectEpoch==current` 只决定该 primary 在 PostMark 是否可因 unmarked 被回收；非current 的
OUTSIDE_SET LARGE 即使本轮 `LARGE_MARKED=0` 也绝不能回收。反之，OUTSIDE_SET LARGE
沿可达图首次命中后仍需要 OBJECT 扫描其强引用，确保它指向的 collect-set 对象被发现。

SPAN 的 `baseRegionIdx` 在 live primary 的整个生命周期保持稳定。只有 dead primary 已经
完成 Mark task quiescence且从 `largeCollectSet` 摘除后才能清；否则 barrier/owner 可能把
旧 SPAN 解析成已复用的新对象。

#### 3.2.12 Mark任务状态、summary与Bitmap迁移

`pendingWordSummary` 是 64 位 word-level hint：bit N 表示第 N 个 64-bit bitmap word
可能存在 `mark & ~scan`。它允许 false positive，但不能发生 false negative。真正的对象
扫描工作始终由 plane 交集重新计算。

| 流程点 | mark bitmap | scan bitmap | pending summary | state/ready | ledger/本地状态 |
|---|---|---|---|---|---|
| `DiscoverRef` 收集 small candidate | 尚不写；同 `(Region,word)` mask 在本地OR | 不写 | 不写 | 不变 | candidate受当前TaskLease、EnumRoots阶段或bulk覆盖写前强制flush之一保护 |
| MarkWordBatch flush | collect-set先mask allocated；一次 `fetch_or(mask)` 得 `newBits`；OUTSIDE_SET candidate已做精确验证 | 不写 | 尚不写 | 不变 | `newBits=0`立即结束，不扫描对象 |
| NO_REF `newBits!=0` | winner已可见 | 不使用 | 不使用 | 不发布 | 只计WU/统计 |
| HAS_REF `newBits!=0` | winner已可见 | 仍未扫描 | release OR `wordBit` | 若IDLE，唯一winner转QUEUED并push；若QUEUED/OWNED不重复push | 新task admission必须先计账；若已有task则由其lease覆盖 |
| REGION acquire pop | acquire读mark | 读旧scan | acquire接收pending | `QUEUED -> OWNED`；taskLink清INVALID | queued转in-flight，总数不变 |
| owner领取word | 读当前markWord | 读当前scanWord | `exchange(0, acquire)`领取word集合 | 保持OWNED | lease持续 |
| owner算候选 | collect-set：`(~alloc & valid) & mark & ~scan`；OUTSIDE：`mark & ~scan` | 只读 | 可被producer并发再次OR | OWNED | 无candidate的summary bit是合法false positive |
| owner枚举对象 | 不改当前对象mark | 对象全部强引用枚举后才置对应scan bit | 新child只影响目标Region pending | OWNED | child进入local MarkWordBatch；WeakRef进local discovery |
| owner继续本Region | flush batch后重读/exchange pending | 已扫对象保持1 | 非零则只处理命中的word | OWNED | budget足够时复用同一lease |
| slice到期 | 不改 | 未完成对象不得提前置scan；未完成word重新置pending | release OR未完成word | `OWNED -> QUEUED`并pushreplacement token | replacement先Add，发布后旧lease Complete；净计数不变且中途不为0 |
| owner尝试结束 | child batch已flush | 当前候选已完成 | 先转IDLE再acquire recheck | 无新work则IDLE；有work的一方重新QUEUED/push | 新task先Add再发布，旧task最后Complete |
| OBJECT winner/扫描 | 不使用small mark plane；primary marked CAS是winner | 不使用 | 不使用 | HAS_REF进ObjectReady；pop后一次扫完整对象 | child flush后Complete；不支持assist/requeue |
| MarkTermination fixed point | 不再存在只在batch中的winner | 所有普通HAS_REF mark均已scan | 所有Region为0 | 两个head空，所有state IDLE | outstanding=0、无lease、所有batch空 |

批量更新不会让多个 worker 在 winner 判定前重复遍历同一大引用对象：

1. small candidate 在 batch 中只合并地址位，不读取目标对象 TypeInfo，更不枚举目标引用；
2. flush 后只有 `newBits` winner 才提交 pending；
3. 同一 small Region 只有一个 owner，owner再按 `mark & ~scan` 枚举对象；
4. LARGE 不进入 batch，先 CAS `LARGE_MARKED`，只有 winner 发布 OBJECT；
5. scan bit 是“引用枚举已完成”的记录，不是“mark candidate 已缓存”的记录。

producer 与 owner 的 lost-wakeup 由以下顺序关闭：

```text
producer: mark OR -> pending release-OR -> observe/transition state -> release-push
owner:    pending acquire-exchange -> scan/flush -> OWNED->IDLE -> pending acquire-recheck
```

producer 看到 OWNED 时不 push；owner 释放后必须 recheck。producer 看到 IDLE 时负责
`IDLE->QUEUED`；若双方竞争，只有一个状态 CAS winner 发布 token。`pending` 可重复置位，
所以即使 owner 已经 exchange 过该 bit，新的 `newBits` 仍会重新触发该 word。

#### 3.2.13 全局容器不变量

各阶段只有下表所列容器可以拥有 Region：

| 阶段 | small Region | LARGE primary | Mark ready | Empty/Free |
|---|---|---|---|---|
| 正常分配/上轮Sweep后 | TLS active；Central partial/full swept；可能仍有诊断中的unswept backlog | `largeNextSupply` | 必须空 | Empty cache、large hot、FreeManager |
| PreEnum STW | active保持；Central swept head被steal为`collectSet`；unswept先quiesce | next-supply swap为`largeCollectSet` | 重置为空 | 不变 |
| EnumRoots/ConcurrentMark | collect-set冻结；OUTSIDE_SET可继续active或在Central供给 | large collect-set冻结；新对象进next-supply | Region/Object动态push/pop | 不参与Mark |
| MarkTermination STW | membership仍冻结 | membership仍冻结 | drain到head空、ledger=0 | 不变 |
| PostMarkTermination | collect-set整批移交unswept | live回next-supply，dead进hot | 必须空 | 接收dead LARGE |
| Sweep | unswept逐个变partial/full swept或Empty | 本轮large已分类完 | 不存在 | 接收liveCount=0 small |
| Scavenge/Shrink | 不触碰active/collect-set/unswept | 只处理已经FREE的hot entry | 不存在 | Empty/hot移交FreeManager并release页面 |

membership 与 link 不变量：

| 不变量 | Release依据 | Debug验证 |
|---|---|---|
| 一个 small Region最多属于一条 Central/collect-set/Empty intrusive容器 | 容器所有权交接协议 | safepoint遍历所有head，检查重节点、环、lane匹配 |
| 一个 Region最多属于一条 ready stack且最多出现一次 | SMALL 的 scanState；LARGE 的 marked winner | ready链去重，检查node kind |
| small允许“一个资源容器 + 一个Mark ready”双membership | 两个独立next字段 | 检查两条link没有交叉写 |
| collect-set member在Mark期必有current epoch | PreEnum写epoch后才退出STW | 链遍历验证epoch/layout/mode |
| Mark期current epoch的SMALL/LARGE必来自本轮collect-set | Region发布/epoch回绕协议 | 全Region side scan反向验证 |
| TLS active不在任何Central/collect-set/Empty链 | refill pop与return push是唯一交接点 | registry snapshot与链节点集合求交为空 |
| SPAN不进入large vector或ready | 地址解析总是回primary | 检查所有vector元素kind=LARGE |
| node不在链时next必须在重新发布前清INVALID | pop/reset路径 | poison旧值、重复push检测 |
| Region重绑前没有ready owner/lease | cycle quiescence + Reset admission | generation side table/poison检查 |

plane 与 Mark 正确性不变量：

| 范围 | 必须成立 |
|---|---|
| collect-set small | `mark & ~allocatedMask == 0` |
| collect-set HAS_REF | `scan & ~mark == 0` |
| collect-set/OUTSIDE_SET HAS_REF | 若存在 `mark & ~scan`，则工作必被 pending、QUEUED/OWNED、producer batch 或 TaskLease之一覆盖 |
| NO_REF | scan plane、pending和ready都不是正确性输入 |
| OUTSIDE_SET | GC不读 live alloc plane；每个首次visit bit在提交前完成精确对象合法性检查 |
| Sweep | 只消费本轮 collect-set 移交的 unswept；`alloc=(~mark)&valid` |
| termination | 两个ready head空、ledger=0、无lease、所有batch空；不能以queue empty单独终止 |

最终哪些数据不需要加锁、哪些必须原子化：

| 数据 | 最终访问方式 | 原因 |
|---|---|---|
| collect-set `head/tail/count` | GC main普通读写 | 只在STW/已quiesce阶段构建和移交，Mark期不改 |
| collect-set `sizeClass/scanClass/regionKind/collectEpoch` | 发布后普通只读；Debug可校验 | Region重用被cycle/ledger禁止，布局稳定 |
| collect-set alloc plane | owner materialize后普通只读 | 本轮allocator无法获得该Region |
| scan plane word | Region owner普通读改写 | 同一Region只有一个scan owner；producer从不写scan |
| TLS `activeRegions`与allocation metadata | TLS owner普通读写 | PreEnum在STW读取；归还用Central release发布 |
| Sweep重建的alloc plane/summary/free count | 唯一Sweep worker普通写 | unswept acquire pop是唯一claim；完成后release push |
| `centralNextRegionIdx/markTaskNextRegionIdx` | relaxed atomic | head提供发布语义；atomic只避免快速复用data race |
| Central/ready head | tagged CAS，release push/acquire pop | MPMC并防ABA |
| mark plane | atomic fetch_or | 多producer批量winner去重 |
| pending summary、scanState、LARGE marked、TaskLedger | 对应release/acquire CAS/RMW | producer/owner和终止并发交接 |
| Empty cache | 专用mutex | 需要take-head、batch drain和地址区间摘除 |
| large two-vector | 唯一 `largeLock_` | allocation append与PreEnum swap可能交接 |
| large hot cache | 独立 `largeHotCacheLock_` | large复用与Scavenge可能并发访问；继续按精确span count分桶 |
| FreeManager bits/summary | 保留现有shard锁/原子快照协议 | allocation、steal、Scavenge跨线程并发 |

基于这些不变量，Release 热路径必须移除下列冗余判断：

| 删除的判断/字段 | 唯一替代真值 |
|---|---|
| `sweepState + sweepGen + list kind` 三重判断 | unswept pop/admission与swept/Empty发布 |
| `partialUnswept` 与 `fullUnswept` 分支 | 单一unswept；Sweep后才按新free count分类 |
| 每对象 `TypeInfo::HasRefField()` | 稳定Region ScanClass；owner只读字段布局 |
| `DIRTY` bit | `pendingWordSummary!=0` |
| 独立 QUEUED/OWNED bit组合合法性判断 | 单一三态枚举 |
| `next!=INVALID` 推断membership | 外部head+owner/state协议 |
| task token中的role/epoch/generation | stable RegionInfo + current epoch；Debug generation在side table |
| ACTIVE Region root登记/`ActiveRootSnapshot` | 真实GC roots + SATB + 对象图闭包 |
| queue empty后扫描全collect-set“补漏” | TaskLedger fixed point；全量扫描只留Debug |
| OUTSIDE_SET Release visited list | 与collect-set共用mark/scan/pending/state/ledger |

这些删除不是少做正确性检查，而是把每个事实压缩为一个可发布、可验证的真值；Release
不再在热路径重复推导，Debug 仍可在 safepoint 用 side set 做全局一致性验证。

### 3.3 64B RegionInfo 与三平面 arena

保留 `MSRegionInfo` 为 64B，把现有`allocBitmap* / markBitmap*`两个pointer移出；
新增scan plane时也不再加第三个pointer，三个plane统一按Region index定址。RegionInfo
只保留“地址/容器链接/本Region热状态”，固定size-class派生值和bitmap地址都不再逐
Region保存。

#### 3.3.1 最终字节布局

| 偏移/大小 | 字段 | SMALL 语义 | LARGE/SPAN/FREE 语义 | 主要使用点 |
|---:|---|---|---|---|
| +0 / 8B | `allocPtr` | BUMP的下一internal allocation地址；BITMAP/SEALED不依赖它分配 | LARGE primary的32KiB对齐span/internal allocation起址；公开`BaseObject* = allocPtr + HEADER_SIZE`；SPAN/FREE无效 | LLVM/Runtime allocation；LARGE对象解析与回收 |
| +8 / 8B | `regionEnd` | 32KB Region 的 exclusive end | LARGE primary 的完整 span end；SPAN/FREE无独立语义 | 分配边界、对象地址解析、LARGE spanCount推导 |
| +16 / 4B | `centralNextRegionIdx` | Central partial/full/unswept或GC私有collect-set单链 | LARGE不复用；classless Empty可在专用锁下复用；FreeManager使用side bitmap | Central/Empty push-pop、PreEnum head steal |
| +20 / 4B | `markTaskNextRegionIdx` | REGION ready单链 | LARGE primary的OBJECT ready单链；SPAN/FREE不用 | Mark task push/pop |
| +24 / 8B | `slotOrLargeMeta` | 低16位为`freeSlotCount`，其余位保留 | LARGE primary为不含allocator `HEADER_SIZE`的精确`logicalObjectBytes`；SPAN/FREE不用 | allocator refill/return、Sweep分类、LARGE统计 |
| +32 / 4B | `collectEpoch` | 等于`currentMarkEpoch`才是本轮collect-set | LARGE primary同样用epoch限定本轮回收；SPAN由primary决定；FREE为0 | PreEnum写；barrier/DiscoverRef读；Sweep仅Debug断言 |
| +36 / 4B | `control` | HAS_REF使用`scanState`；NO_REF保持IDLE | LARGE primary使用`LARGE_MARKED`；SPAN/FREE不用 | producer、Region owner、LARGE mark winner |
| +40 / 8B | `allocSummary` | bit表示对应alloc word仍有free bit | LARGE/SPAN/FREE不用 | BITMAP allocation；BUMP materialize；Sweep重建 |
| +48 / 8B | `pendingWordSummary` | HAS_REF中bit表示对应word可能有`mark & ~scan`；NO_REF不用 | SPAN follower保存`baseRegionIdx`；LARGE primary/FREE不用 | mark到Region owner交接；SPAN回溯primary |
| +56 / 4B | `reserved32` | 无Release语义 | 无Release语义 | 仅ABI预留 |
| +60 / 1B | `allocMode` | 唯一allocation owner读取：NONE/BUMP/BITMAP | LARGE/SPAN/FREE为NONE | LLVM/Runtime allocation |
| +61 / 1B | `reserved8` | 无Release语义 | 无Release语义 | 仅ABI预留 |
| +62 / 1B | `sizeClass` | 0..35，Region非空期间不变 | LARGE/SPAN/FREE为INVALID | descriptor索引、Central lane、Mark/Sweep |
| +63 / 1B | `layout` | `regionKind=SMALL`和稳定`scanClass` | FREE/LARGE/SPAN以及LARGE的scanClass | barrier分类、Mark/Sweep分流 |

物理C++类型固定为 `alignas(64)` standard-layout struct：

```text
uintptr_t allocPtr/regionEnd                 // static_assert sizeof==8
uint32_t centralNextRegionIdx/markTaskNextRegionIdx
uint64_t slotOrLargeMeta
uint32_t collectEpoch/control
uint64_t allocSummary/pendingWordSummary
uint32_t reserved32
uint8_t  allocMode/reserved8/sizeClass/layout
```

不使用C++ bitfield、union或`std::atomic`成员改变ABI。并发字段保持上述原始整数类型，通过
集中accessor中的`__atomic_*` builtins访问；Runtime启动时static_assert 4B/8B原子
always-lock-free。`slotOrLargeMeta`的SMALL low16读写用mask/shift，LARGE直接读写完整64位；
`pendingWordSummary`的SPAN base也用显式mask/shift，不依赖主机union布局或大小端。
`ResetToFree()`只能在已经取得唯一容器所有权且所有Mark/Sweep lease结束后逐字段清理，
不能对仍可能被并发读取的RegionInfo执行整块`memset`。

`centralNextRegionIdx` 与 `markTaskNextRegionIdx` 必须分开。一个 PreEnum 后归还的
OUTSIDE_SET partial Region可以仍在Central swept supply中，同时因对象图遍历进入REGION
ready stack；复用同一个link会破坏其中一条链。两个link都按relaxed atomic访问，head的
release/acquire负责发布，atomic link本身只消除快速pop/reuse/re-push时的C++ data race。

#### 3.3.2 bit编码

bit 编码固定为：

```text
layout:
  bits [1:0] regionKind = FREE/SMALL/LARGE/SPAN
  bit  [2]   reserved
  bit  [3]   scanClass
  bits [7:4] reserved

control:
  bits [1:0] scanState = IDLE/QUEUED/OWNED
  bit  [2]   LARGE_MARKED
  bits [31:3] reserved
```

`totalSlotCount/bitmapWords/validTailMask/slotSize/divMagic` 全部由只读
`MSSizeClassDesc[36]` 推导，owner 在 task 入口按 `sizeClass` 取一次并缓存，不在
RegionInfo 重复保存。small 的 `slotOrLargeMeta` 只需要低 16 位
`freeSlotCount`；LARGE 使用完整 64 位保存逻辑对象大小。

#### 3.3.3 不同Region角色实际使用哪些字段

| Region角色 | 必须有效的RegionInfo字段 | 明确不读取的字段 |
|---|---|---|
| SMALL TLS ACTIVE | `allocPtr/regionEnd`、`freeSlotCount`、`allocSummary`、`allocMode`、`sizeClass/layout`；若被发现为灰对象，还使用`collectEpoch/control/pending` | barrier/owner不读取ACTIVE alloc plane、`allocMode` |
| SMALL current collect-set | `regionEnd`、两个link、`freeSlotCount`、`collectEpoch/control`、`pending/sizeClass/layout`；alloc/mark/scan从plane arena解析 | `allocPtr/allocMode`不参与Mark和Sweep正确性 |
| SMALL OUTSIDE_SET Central supply | allocation字段保持可复用；Mark只走OUTSIDE_SET验证和`control/pending` | 当前Sweep不读它的alloc/mark |
| LARGE primary | `allocPtr/regionEnd/logicalObjectBytes/collectEpoch/control/layout`，需要OBJECT task时使用`markTaskNextRegionIdx` | small的alloc/mark/scan plane、`allocSummary/pending` |
| SPAN follower | `layout=SPAN`和`baseRegionIdx` | 其余字段；解析后统一读取primary |
| FREE/classless empty | `layout=FREE`，其余清零或INVALID | Mark/Sweep不读取任何small/LARGE union字段 |

classless empty cache和FreeManager中的 Region都使用 `regionKind=FREE`；两者的所有权
差别由所在容器表达，不再占一个 `EMPTY_CACHE` kind。只有从容器唯一 pop后，才以普通
store完成plane/metadata/kind初始化，再通过TLS安装或对象引用的既有发布链对外可见。

不再保留 `MSSweepState`、每Region `sweepGen` 和可变 `regionType`。
`regionKind/scanClass` 在Region非空期间不变，因而barrier/owner可以普通读取；
BUMP/BITMAP只由当前allocation owner或所有权交接后的新owner读取`allocMode`。
PARTIAL/FULL/SWEPT/UNSWEPT/CLAIMED分别由
`freeSlotCount`、stack membership 和 Sweep admission in-flight 表达，不再有重复状态和
claim CAS。原 `ResetToBump()` 的 fast reuse 分支改成由取得 empty Region 的调用者显式
初始化，不在每个 Region 保存第二套 Sweep 真值。

这里删除的是 `MSRegionInfo::sweepGen`。Central仍保留一个全局
`sweepCycleId`，用于P级sweep debt lazy reset、延迟helper任务的轮次校验和统计归属；
它不判断某个Region是否待扫，也不授予Sweep ownership，因此不占64B RegionInfo。

#### 3.3.4 LARGE/SPAN union

上述 +24/+40/+48 是 small Region 语义。LARGE/SPAN 固定复用为：

```text
+24..+31 logicalObjectBytes
+40..+47 unused
+48..+55 SPAN follower 的 baseRegionIdx；LARGE base 不重复保存自身 index
+36 control.LARGE_MARKED
```

LARGE的`allocPtr`固定保存32KiB对齐span base/internal allocation address，不保存返回给
对象模型的`BaseObject*`。因此：

```text
largeObjectAddress = allocPtr + HEADER_SIZE
spanBytes          = regionEnd - allocPtr
spanCount          = spanBytes / MS_REGION_SIZE
logicalObjectBytes = slotOrLargeMeta
```

绑定时检查`spanBytes % MS_REGION_SIZE == 0`且
`HEADER_SIZE + logicalObjectBytes <= spanBytes`。SPAN entry在+48保存baseRegionIdx，
`layout.regionKind=SPAN`；LARGE/SPAN不访问small三平面。这样不再为LARGE重复保存spanCount
和自身base index，同时不会因对象模型的`HEADER_SIZE`导致spanCount少算。

#### 3.3.5 三平面定址与LLVM可见范围

每个 plane 为每 Region 固定保留 512B，即 64 个 `uint64_t` word，覆盖最小 8B slot 下的
4096 个 slot：

```text
planeWord(base, regionIdx, wordIdx)
  = base + regionIdx * 512 + wordIdx * 8
```

物理实现固定为三个独立 `MemMap` 虚拟保留区，不再使用当前每Region
`[alloc 512B | mark 512B]` 的交错slot：

```text
allocPlaneMap : maxRegionCount * 512B
markPlaneMap  : maxRegionCount * 512B
scanPlaneMap  : maxRegionCount * 512B
```

三个map使用相同regionIdx坐标，但分别维护commit high-water/chunk状态：

- SMALL首次绑定时确保对应alloc和mark范围已commit；
- 只有HAS_REF SMALL绑定时主动commit scan范围；
- NO_REF绑定、PreEnum和Mark都不得为了清scan而主动commit scan页；
- 页粒度可能让相邻HAS_REF Region已使同一物理页驻留，这不改变“NO_REF路径不主动触页”
  的要求；
- LARGE/SPAN/FREE不访问三个small plane；
- commit失败必须在Region发布到TLS/Central前回滚，不能留下部分初始化的Region；
- Scavenge只在整个commit chunk都不再被small Region使用时decommit，不做512B粒度伪
  decommit。

接口收口为 `EnsurePlaneRangeCommitted(plane, startRegionIdx, count)`；PreEnum只能清已经
在Region绑定阶段保证可访问的plane，不能在STW Mark setup中首次申请native memory。

现有 `CJ_MRT_MSGCFastPathState` 原符号直接扩展为64B，不增加V2符号。字段类型和padding
固定为：

```text
+0   / 1B uint8_t  barrierPhase
+1   / 3B explicitPad0
+4   / 4B uint32_t currentMarkEpoch
+8   / 8B uint64_t heapReservedStart
+16  / 8B uint64_t heapReservedEnd
+24  / 8B uint64_t regionInfoArray
+32  / 8B uint64_t allocPlaneBase
+40  / 8B uint64_t markPlaneBase
+48  / 8B uint64_t scanPlaneBase
+56  / 8B uint64_t reservedAbi
```

`reservedAbi` 不进入热路径。布局一致性由 Runtime `static_assert`、LLVM
`CJMSGCABI.h` 和跨仓 contract test 保证，不让每次 barrier 额外读取
`layoutSignature`。`currentMarkEpoch` 让 barrier emitter 区分 stable collect-set 与
OUTSIDE_SET。因为已明确禁止任意新旧组件混用，contract test只要求布局不一致时让联合
构建失败，不设计动态加载时的版本探测或旧Runtime拒绝路径。

`AllocBufferMS` 删除兼容旧CopyGC/旧SDK fast allocation的 `tlRegion_` 占位字段，也删除
`activeLog_` 和为其保留的896B padding。compiler-visible前缀从offset 0开始就是：

```text
+0    activeRegions[0][NO_REF]
+8    activeRegions[0][HAS_REF]
...
+568  activeRegions[35][HAS_REF]
+576  Runtime-only fields
```

因此 `activeRegionBase=0`、lane stride为8B，最终索引仍是
`(sizeClass * 2 + scanClass) * 8`。MSGC不再把 `ThreadLocalData::buffer` alias到
`AllocBufferMS`；LLVM只通过 `ThreadLocalData::msAllocBuffer` 取得该对象。相关
ThreadLocal offset仍集中在 `CJMSGCABI.h`，但不再要求 `AllocBufferMS+0`兼容旧
`AllocBuffer::tlRegion`。

LLVM只允许直接读取以下字段：

- allocation fast path：`allocPtr/regionEnd/allocSummary/freeSlotCount/allocMode`；
- mark barrier：`collectEpoch/control/layout`，以及全局mark/scan plane base；
- size-class派生值：统一读取`CJMSGCABI.h`镜像的`MSSizeClassDesc`。

`centralNextRegionIdx/markTaskNextRegionIdx/reserved`只由Runtime使用。AArch64/X86不得
各自再硬编码RegionInfo offset；所有offset、layout/control bit和plane stride统一收口到
`CJMSGCABI.h`。

#### 3.3.6 各阶段的读写点

| 阶段 | 写RegionInfo | 读RegionInfo | 并发依据 |
|---|---|---|---|
| Region绑定/复用 | 初始化全部角色字段，清有效plane | 无 | 尚未发布，普通写 |
| BUMP/BITMAP分配 | `allocPtr`或alloc plane、`freeSlotCount/allocSummary/allocMode` | 同组字段 | 单AllocBuffer owner，普通读写 |
| 归还Central | 必要时materialize BUMP，固定`allocMode=BITMAP`，写`centralNext` | sizeClass/scanClass/free count | release push交接所有权 |
| PreEnum STW | 为steal出的collect-set写`collectEpoch`；清collect-set与ACTIVE的mark工作区及对应control | Central head、TLS active snapshot、layout/sizeClass | Sweep已quiesce且STW；ACTIVE alloc plane不读写 |
| DiscoverRef/barrier | 原子更新mark word；HAS_REF再更新pending/control | layout、collectEpoch、descriptor | mark relaxed RMW；pending release/owner acquire |
| Region owner | 更新scan plane、pending、scanState和task link | stable alloc、mark、TypeInfo | 单Region scan owner |
| Sweep | 重建alloc plane、`freeSlotCount/allocSummary`，结果push Central | mark plane、sizeClass/layout | unswept acquire pop唯一claim；swept release push |
| Scavenge/FreeManager | 最终Reset为FREE | 只验证kind/容器归属 | FreeManager shard/container ownership；不读mark/scan |

### 3.4 三个 plane 的准确语义

当前 Runtime 的 alloc bitmap 真实语义必须保持：

```text
alloc bit = 1：slot 空闲
alloc bit = 0：slot 已分配
mark  bit = 1：本轮已发现
scan  bit = 1：本轮已完整扫描引用字段
```

因此 collect-set 某 word 的掩码是：

```text
validMask     = lowBits(validSlotCount)
allocatedMask = (~allocWord) & validMask
newScanWork   = allocatedMask & markWord & ~scanWord
deadBits      = allocatedMask & ~markWord
nextFreeWord  = (~markWord) & validMask
```

不能写成 `alloc & mark`。ACTIVE Region 的 alloc plane 正在被其 owner 修改，GC 不并发
读取它；ACTIVE 的 mark/scan 仅是临时 visit/scan plane，精确对象合法性在提交 visit bit
前完成。

### 3.5 collect-set 与 ACTIVE 的并发协议

PreEnumRoots 不 flush TLS active Region，也不复制全堆 alloc bitmap。它只把当时已在
Central、分配状态已经物化的 SEALED Region 编入 collect-set：

```text
SEALED at PreEnum -> collectEpoch=current -> 本轮 Mark + Sweep
TLS ACTIVE        -> collectEpoch!=current -> 本轮只允许按可达路径 visit，不 Sweep
PreEnum 后新建/归还 Central -> 不进入本轮 collect-set
```

如果某 TLS Region 下一轮 PreEnum 时仍 active，则继续等下一轮，不强制 detach 或
materialize。这样避免 PreEnum 为建立全局 alloc snapshot 而遍历、flush 所有 P。

新分配对象也不再调用普通 `MarkNewObject`：它所在 Region 不属于本轮回收集合。唯一例外
是 Finalizer/resurrection 等显式特殊语义，它们通过专用闭包入口处理，不能借用
allocation-time mark 的旧含义。

collect-set 外的可达对象统一称为 `OUTSIDE_SET`，它可能仍在 TLS ACTIVE，也可能是
PreEnum 后才归还 Central、随后重新参与分配的 Region。它和 collect-set 共用
`pendingWordSummary + IDLE/QUEUED/OWNED + TaskLedger`，不维护 Release 版
`EXTERNAL_VISITED` 或 `externalVisitedRegions`。如需验证 visit/scan 闭包，Debug 构建
使用独立 side bitset/list 记录 touched Region，不占用 64B RegionInfo 热字段。

OUTSIDE_SET不能只写一句“精确验证”，最终使用唯一入口：

```cpp
bool ResolveOutsideSetObject(
    BaseObject* candidate,
    MSResolvedObject& out); // regionIdx/primaryIdx/slotIdx/wordIdx/bit/scanClass/typeInfo
```

Release检查顺序固定为：

1. candidate非空且位于当前64位heap reserved/committed范围；
2. 解析RegionInfo；若为SPAN先转LARGE primary，拒绝FREE或未知kind；
3. LARGE要求candidate等于primary的`allocPtr + HEADER_SIZE`，并验证
   `HEADER_SIZE + logicalObjectBytes <= regionEnd - allocPtr`及span对齐；
4. SMALL从稳定size-class descriptor推导slot base，要求candidate是精确slot对象地址、
   对齐正确且对象不跨Region有效slot范围；
5. OUTSIDE_SET绝不读取正在变化的alloc plane；读取对象stateWord/TypeInfo，要求对象已经
   完成发布、state合法、TypeInfo非空且对象大小/布局与Region sizeClass兼容；
6. 从Region稳定`scanClass`取得调度分类；Debug再断言TypeInfo分类与Region纯度一致；
7. 验证完成后才能对visit(mark) bit执行原子OR。首次winner负责pending/ready publication；
   已经marked的后续candidate可以跳过TypeInfo重复读取，但仍必须完成地址、kind和slot坐标
   解析。

allocator必须遵守“先写对象头/TypeInfo和数组长度，后发布引用”的既有对象发布规则；
validator遇到未完成发布的slot时返回false，Debug记录fatal上下文。OUTSIDE_SET Region即使
在Mark期间归还Central或继续被allocator填充，也不改变sizeClass/scanClass，不进入本轮
unswept；只要其pending、owner、TaskLease或MarkBatch仍存活，就禁止Reset为FREE或重新绑定
其他lane。

### 3.6 Region scan state 与任务

```cpp
enum class MSRegionScanState : uint8_t {
    IDLE = 0,
    QUEUED = 1,
    OWNED = 2,
};

struct MSRegionTask {
    uint32_t regionIdx;
};
```

Release task 不保存 role、epoch 或 generation：

- `collectEpoch == currentEpoch` 推导 `COLLECT_SET`；
- 否则只能按 `OUTSIDE_SET` 处理，绝不读取 live alloc plane；
- Region 在 Mark/Sweep 账本清空前不得重用；Debug side generation只用于捕获生命周期 bug。

统一 Scheduler 还保留：

```text
OBJECT = LARGE HAS_REF primary
```

small 对象全部由 REGION 表达；WeakRef 由 owner-local discovery 表达；Finalizer 在 STW
分类。因此不存在需要携带 `BaseObject* + objectKind/flags` 的第三种普通对象。OBJECT
token 也只保存 LARGE primary 的 `regionIdx`，类型、对象地址和 span 均从稳定 RegionInfo
推导。

WeakRef 对象由 Region/OBJECT handler 识别后追加到 worker-local discovery buffer，不沿
referent 形成强边；MarkTermination 汇总并去重。Finalizer 分类、resurrection root 发布
和 WeakRef 清理由 GC main 在 STW 直接执行。没有 MAIN_ONLY 或 RANGE lane。

#### 3.6.1 Finalizer专用root发布

普通分配路径不再调用 `MarkNewObject`，但保留现有
`Collector::MarkNewObject/MarkSweepCollector::MarkNewObject` 虚入口，且把它的 MSGC
语义严格收窄为“Finalizer 生命周期 root 发布”。不再增加两个只转发一次的专用虚接口，
避免扩大 Collector 公共接口和维护重复协议。

创建路径固定为：

```text
对象内存分配
  -> 写TypeInfo/对象头
  -> BaseObject::OnFinalizerCreated
  -> FinalizerWorkList::RegisterFinalizer
       持有listLock登记对象
       若phase属于MARK_SETUP/CONCURRENT_MARK/MARK_TERMINATION：
         调用MarkNewObject(obj)
         以特殊root producer身份DiscoverRef并同步flush
       完成后才释放listLock
  -> 对象才允许向mutator代码发布
```

registry插入与MarkTermination取得Finalizer表快照使用同一个现有Finalizer lock/gc gate作为
线性化边界。这样GC要么在快照中看见该对象，要么等待注册方完成显式root publication；
不能先把对象放入表、释放锁，再无保护地检查phase。取得gate后到publication完成并释放
gate之间禁止safepoint；该入口只处理一个对象，使用栈上固定容量的单对象临时MarkBatch并
当场flush，不向堆/全局池申请MarkBatch、WorkBuf或其他native内存。

Finalizer worker准备从`workingFinalizables`移除对象时固定执行：

```text
持有listLock
  -> MarkNewObject(obj)
       若处于Mark阶段，DiscoverRef并同步flush其当前强闭包
  -> publication完成后删除workingFinalizables中的该root
```

这保留“初始root快照前已开始执行、快照后才完成”的finalizer及其self-resurrection闭包。
普通新对象、普通数组和PreEnum后分配对象都不得调用 `MarkNewObject`。源码契约测试固定
检查“注册后发布”和“删除前发布”都发生在 `listLock` 临界区内。

### 3.7 TaskLedger、TaskLease 与 worker 能力

物理表示固定为：

```text
RegionReadyHead/ObjectReadyHead:
  std::atomic<uint64_t>，高32位ABA tag，低32位regionIdx
  INVALID_REGION_IDX = 0xffffffff

MSRegionInfo links:
  uint32_t centralNextRegionIdx
  uint32_t markTaskNextRegionIdx
  字段本体保持4B ABI，用__atomic builtins作relaxed load/store

TaskLedger:
  std::atomic<uint64_t> outstandingTotal
```

Runtime在支持的AArch64/X86-64构建中static_assert 32/64位原子always-lock-free；不提供
mutex fallback。`outstandingTotal`溢出或对0执行Complete是fatal不变量错误。

`outstandingTotal`只精确统计queued、in-flight和正在建立publication的provisional task。
成功pop只把task从queued变成in-flight，不减少outstanding；只有其扫描完成、MarkBatch已
flush、所有child task已经发布后，`TaskLease::Complete()`才减一。worker-local
MarkWordBatch由当前TaskLease覆盖；EnumRoots batch由STW/阶段边界覆盖；bulk barrier
batch由“无safepoint且覆盖写前flush”及最终STW handshake覆盖，不为它再增加第二套
producer计数。

`TaskLease`固定为move-only RAII对象，保存scheduler、task kind、regionIdx和active flag：

- 禁止copy；
- move转移唯一active flag；
- `Complete()`只能调用一次，先assert本地batch为空，再release `fetch_sub(1)`；
- requeue先发布replacement task，再Complete当前lease；
- 析构时active仍为true必须`CHECK_DETAIL`失败，不能静默Complete或泄漏账本；
- assist返回、worker park和phase切换前不得持有active lease。

父任务派生子任务的顺序必须是：

```text
flush local MarkBatch:
  each child task -> Add(child outstanding) -> release push(child)
publish any direct child task
Complete(parent)
```

这样任务图存在边时 outstanding 不会瞬时归零。三个调用者共用 `RunMarkSlice`：

| 调用者 | REGION | OBJECT |
|---|---:|---:|
| GC main | 是 | 是 |
| helper | 是 | 是 |
| mutator assist | 是 | 否 |

LARGE HAS_REF 尤其是大引用数组不会由 assist 领取，避免一次不可切分对象扫描把 mutator
阻塞很久。

### 3.8 WorkUnit

WorkUnit 使用 Q16 作为 Assist debt 比率，但热循环只累加整数事件：

```text
resolvedRefs
markWordRMW
scanWords
```

首版整数权重固定为：

```text
kResolvedRefCost = 1
kRmwCost         = 8
kScanWordCost    = 1
```

在slice边界换算：

```text
completedWU =
    resolvedRefs * kResolvedRefCost
  + markWordRMW * kRmwCost
  + scanWords   * kScanWordCost
```

三项计数和乘加都使用saturating `uint64_t`；溢出时钳位`UINT64_MAX`并在Debug fatal，不允许
回绕为小debt。上述权重只是首版控制常量，可以在profile后整体调优，但Runtime、测试和
REPORT必须引用同一 `MSWorkUnit.h` 定义，不能在Pacer/assist/helper中复制不同值。

`scanBytes` 仅作为性能统计，不进入首版 pacing，因为实际扫描的是 ref slot/GCTib，不是
对象 payload；它与 `resolvedRefs` 高度重复。对象数、task 数、WorkBuf 数也不作为
Assist收费单位。Pacer只保留`WU/allocatedByte`与`WU/us`两个full-GC EWMA，不实现
expected-cycle模型。二者使用Q16和固定`alpha=1/8`：

```text
nextEWMA = oldEWMA + (sample - oldEWMA) / 8
initialWorkDensity = 1 WU / 64 allocated bytes
initialThroughput  = 32 WU / us
```

只有分母非0的完整GC样本才更新；所有除法放在cycle/slice边界，不进入ref或bitmap热循环。

### 3.9 全局不变量

```text
collect-set small:
  mark & ~allocatedMask == 0
  scan & ~mark == 0

collect-set HAS_REF:
  allocatedMask & mark & ~scan != 0
    => pendingWordSummary != 0
       或 state in {QUEUED, OWNED}
       或某个 in-flight TaskLease/MarkBatch 正持有该工作

collect-set NO_REF:
  不读取、不提交 scan plane

OUTSIDE_SET HAS_REF:
  visit(mark) & ~scan != 0
    => 同样存在 pending/state/lease/batch 之一

OUTSIDE_SET:
  不要求 visit 是 alloc 的子集
  设置 visit 前必须验证精确对象
  Sweep 永不消费本轮 visit/scan

all tasks:
  queued or in-flight => outstandingTotal > 0
  TaskLease 完成前 local MarkBatch 为空

cycle:
  ACTIVE Region 不是 Root
  PreEnum 后新对象不执行普通 MarkNewObject
  Sweep 只消费从 current collect-set 发布的 unswept；Release不逐Region重读epoch
  Region 重用发生在所有 task/owner/batch 生命周期结束之后
```

Release 热路径只保留地址范围、Region 类型、slot 对齐/边界、OUTSIDE_SET 对象合法性、
LARGE/SPAN base 解析、mark winner 和特殊对象判断。role/generation/重复 queue-empty、
per-object `HasRefField()` 等可由稳定结构推导的判断改为 Debug assert 或在 task 入口
一次性完成。

## 4. 六项优化的最终做法与效果

### 4.1 优化一：NO_REF/HAS_REF Region 分离

#### 要解决的问题

当前发现一个白对象后必须读取 TypeInfo 才知道它是否含引用。case26 的约 413.7 万个
`[]byte` payload 都是叶子对象，虽然不扫描 2 GiB payload，仍逐对象读取对象头和
TypeInfo。大堆使这些随机元数据访问更难留在 cache 中。

#### 最终做法

1. LLVM 对静态已知的 object/array allocation wrapper 写入
   `cj-msgc-scanclass={NO_REF|HAS_REF}` 属性；
2. 动态泛型数组不能猜测，进入动态 wrapper 后由 component TypeInfo 分类；
3. Runtime slow allocation 接收最终 ScanClass，并使用
   `activeRegions_[sizeClass][scanClass]`；
4. refill 只能从同一 `(sizeClass, scanClass)` bucket 取非空 Region；
5. classless empty Region 可重新绑定；非空 Region 不允许改变 ScanClass；
6. Region `layout.scanClass` 成为 Mark 热路径的唯一普通 small-object分类来源；
7. owner 已经处于 HAS_REF Region，扫描每个对象时不再调用
   `TypeInfo::HasRefField()`，只读取 TypeInfo 的字段布局。

动态 TypeInfo 分类发生在分配 slow path/Region refill 边界，不允许在每个 Mark winner 上
重新分类。LLVM 与 Runtime 的分类函数必须由同一契约测试覆盖：普通 class、无引用
primitive array、引用数组、含引用 struct array、WeakRef 和 Finalizer。

#### 效果

- NO_REF collect-set winner 不读取 stateWord/TypeInfo，不发布扫描任务；
- NO_REF Region 不 commit、清零或统计 scan plane；
- HAS_REF owner 扫描时不再逐对象判断“是否含引用”；
- 相同布局对象集中，TypeInfo 和字段 offset cache 命中率提高；
- TLS active slot 从 36 增至 72；Central 仍是 36 个 bucket，只把三个 tagged stack
  head按ScanClass分成两组，共用pressure统计，不保留 bucket list lock。

#### 正确性边界

错误地把 HAS_REF 分为 NO_REF 会丢边，是致命错误；反向误分只损失性能。Debug/测试构建
必须抽样或全量验证：

```text
Region ScanClass == NO_REF => 每个已分配对象的 TypeInfo::HasRefField() == false
Region ScanClass == HAS_REF => 普通对象 TypeInfo::HasRefField() == true
```

WeakRef 等特殊对象不依赖上述普通断言；它们仍位于 HAS_REF Region，但 owner 必须走
special handler。LARGE HAS_REF 进入 ObjectReady，Finalizer 不由对象扫描创建 task。

### 4.2 优化二：alloc/mark/scan 三平面 Bitmap

#### 要解决的问题

只有 mark bitmap 时，Region 再次被新引用命中后，无法区分哪些 marked slot 已扫描、
哪些仍是灰色。要么重新遍历整个 Region，要么继续保留逐对象 WorkBuf。alloc plane
则负责在不读取对象头的情况下判断 collect-set slot 是否真实存在。

#### 最终做法

三个 plane 使用相同的 `(regionIdx, wordIdx, bitIdx)` 坐标：

- alloc：allocator 的 occupancy 反码，`1=free`；
- mark：多 producer 原子 OR，`1=discovered`；
- scan：Region owner 普通 load/store，`1=scanned`。

生命周期：

| 阶段 | alloc | mark | scan |
|---|---|---|---|
| ACTIVE allocation | owner 可写 | 本轮 visit 可原子写 | owner 可写临时 scan |
| PreEnum collect-set | SEALED 后只读 | 清零 | HAS_REF 清零 |
| ConcurrentMark | collect-set 只读 | producer 原子 OR | 单 owner 写 |
| MarkTermination | 只读 | 最终稳定 | 最终稳定 |
| Sweep | 用 mark 重建 free bits | 保留到下一轮 PreEnum | 不参与回收 |
| 下一轮 PreEnum | 新快照保持 | 清零 | HAS_REF 清零 |

PreEnum 不构建一份复制的“全局 allocBitmap 快照”。collect-set alloc plane 之所以稳定，
是因为 SEALED Region 生命周期禁止它在本轮重新分配，而不是因为复制。

#### scan plane 存在的意义

第一次发布某 Region 时，owner 扫描 `mark & ~scan`；之后其他 worker 或 SATB barrier
又标记同一 Region 的新对象，owner 仍只扫描新增 bit。没有 scan plane，就只能重复扫描
所有 marked 对象，或退回逐对象work-item模式。

scan bit 在一个对象的所有强引用字段已经枚举后才设置。枚举产生的 child 可以暂存在
当前 owner 的 MarkWordBatch，但 owner 在交出 TaskLease、重新排队或返回 assist 前必须
flush；因此 scan 提前于 child 的全局可见最多只发生在仍有 outstanding lease 的局部
区间内。

#### 效果

- collect-set 合法性从“逐对象头检查”降为“每 bitmap word 一次 stable alloc load”；
- Region 重复变脏时只扫描增量；
- scan plane 为 Region owner 提供持久去重状态；
- plane 连续布局避免每个 Region 跟随三个指针访问分散 bitmap。

额外虚拟地址开销是每个可管理 Region 三个 512B plane；NO_REF 的 scan plane 可只保留
地址空间而不物理触页。

### 4.3 优化三：Region 级扫描

#### producer：只发布内容，不发布对象列表

当 mark OR 返回 HAS_REF `newBits` 时：

```text
pendingWordSummary.fetch_or(1 << wordIdx, release)
state = load(relaxed)
if state == IDLE:
    TryPublishRegion(regionIdx)  // intrusive tagged-stack publish，详见 7.2
```

多个 winner 命中同一 word，只合并成一个 summary bit；命中同一 Region 的不同 word，
summary 中可同时有多个 bit；同一时刻该 Region 最多有一个 QUEUED 或 OWNED task。
“必要时只发布一个 Region token”是指**每个 Region 一份扫描所有权**，不是全局只发一个
token。若同时发现五个不同 Region，最多分别发布五个 REGION task。

#### owner：扫描增量直到 fixed point 或 slice 到期

```text
pop regionIdx
CAS(QUEUED -> OWNED)
derive role once

loop:
    summary = pendingWordSummary.exchange(0, acquire)
    for each wordIdx in summary:
        collect-set:
            candidates = (~allocWord & validMask) & markWord & ~scanWord
        external-active:
            candidates = markWord & ~scanWord

        for each candidate bit:
            obj = regionStart + slotIdx * slotSize
            load TypeInfo/field layout
            enumerate strong refs -> local MarkWordBatch
            WeakRef -> worker-local discovery buffer，不扫描 referent
            LARGE/small 无法表达对象 -> route OBJECT
            set scan bit after object enumeration

    flush local MarkWordBatch
    if pendingWordSummary != 0 and budget remains:
        continue
    if budget exhausted and work remains:
        requeue current Region before completing old lease
        return
    try release OWNED -> IDLE
    recheck pendingWordSummary
    if nonzero and win IDLE -> QUEUED:
        publish one new REGION task
    return
```

owner 不会不停遍历整个 Region，也不会“等 dirty 后再扫一整遍”。它只消费 summary 指向
的 word，并在每个 word 内计算 `mark & ~scan`。

#### producer/owner 竞态

producer 在 state 为 OWNED 时只 OR summary，不抢 owner。owner 释放为 IDLE 后必须再次
acquire 读取 summary；如果 producer 恰好在释放窗口提交工作，owner 或 producer 至少
一方会重新排队，从而避免 lost wakeup。

`pendingWordSummary` 已经表达 dirty 内容，所以没有第三个 DIRTY 状态。Region state
使用单一枚举，避免 QUEUED/OWNED/DIRTY 多 bit 组合产生非法状态判断。

#### 对象图遍历与 Region owner 的关系

二者是同一闭包的 producer/consumer 两面，并行执行：

- owner 扫描源对象字段；
- 每个 child 只提交到 child 所在 Region 的 mark plane；
- child 若在同一 Region，最终仍由当前或后续本 Region owner 扫描；
- child 若在其他 Region，由其他 Region owner 并行扫描；
- winner 不需要取得目标 Region 的 owner，也不等待目标 Region 扫完。

一个 worker 可以依次拥有多个 Region，但同一时刻只执行一个 TaskLease；它不能在持有
Region A owner 时阻塞等待 Region B。发现 B 只发布 B 的 task。

#### 效果与代价

收益是对象、bitmap 和 TypeInfo 的局部访问，且一个 Region 的 scan plane 只有一个写者。
代价是当 live graph 集中在极少数 Region 时并行度受 Region 数限制。32 KiB Region 和
预算式 requeue 限制单 owner 占用时间；本步骤不再用对象 WorkBuf 补充普通 small-object
并行度。

### 4.4 优化四：Mark Bitmap 批量更新

#### 要解决的问题

逐引用 `fetch_or` 会让同一 mark bitmap cache line 在多个 worker 间反复转移。一个对象
包含多个指向同一目标 Region 的引用时，还会重复执行 Region 解析和队列判断。

#### 最终 MarkWordBatch

每个 worker/slice 保存固定16个entry，不动态分配。物理结构是8组×2路的小型
set-associative cache；`HashSet(regionIdx, wordIdx)`只探测两路，冲突时按每组round-robin
选择victim并先flush，避免原先线性搜索8～16个entry：

```cpp
struct MSMarkWordBatchEntry {
    uint32_t regionIdx;
    uint8_t wordIdx;
    uint64_t mask;
};
```

key 只需要 `(regionIdx, wordIdx)`。ScanClass和role从稳定metadata推导，不放入entry。
batch第一次使用时缓存本轮`markEpoch`及alloc/mark plane base；后续通过
`planeBase + regionIdx * stride`直接定址，不重复读取全局arena对象。相同slot在flush前
被多次发现，只会OR到同一mask，不会多次发布灰工作。

加入 batch 前只做地址级必要检查：

1. heap range；
2. 直接定位 Region；
3. LARGE/SPAN 转 OBJECT 路径；
4. small slot base、对齐、边界和 word/bit 计算；
5. OUTSIDE_SET 白对象额外验证 stateWord/TypeInfo。

实际落地版本增加两层relaxed只读预检查，但仍由最终`fetch_or`决定并发winner：

1. candidate完成Region/slot解析后，先读取目标mark word；该bit已经为1时不进入batch；
2. entry flush完成alloc/valid mask后，再用一次mark word load剔除这一批等待期间已经
   被其他worker标记的bit；mask归零时不执行RMW；
3. 剩余mask仍必须执行原子`fetch_or`，不能用预检查结果代替并发唯一winner。

mark bit在一个GC epoch内单调从0变1，所以“读到1后返回”不会漏工作；HAS_REF对象的首次
winner已经发布pending/REGION所有权。预检查的目标是让共享/环形对象图的常见重复引用只
承担读取，不让cache line再次进入原子RMW写路径。flush时按word处理：

```text
collect-set mask &= (~allocWord) & validMask
old = markWord.fetch_or(mask, relaxed)
newBits = mask & ~old
```

`newBits`仍是唯一winner结果。预检查只是性能过滤器：读到0后即使发生竞态，也必须由
上述`fetch_or`重新裁决。

#### winner 后续处理

- `newBits == 0`：结束；
- NO_REF small：累计统计后结束；
- HAS_REF small：release OR 对应 `pendingWordSummary`，必要时 queue Region；
- LARGE/SPAN：不进入 word batch，使用 LARGE 原子 marked flag；
- WeakRef：由 owner 记录到 worker-local discovery buffer，不扫描 referent；
- Finalizer：不在普通对象扫描时分类，MarkTermination 直接处理注册表；
- LARGE/special：只有确实不能由 small Region owner 表达时才进入 OBJECT。

普通 collect-set HAS_REF 的 winner 不保存对象地址。owner 根据 Region/slot 重新计算
地址，因此 batch 不需要 `refs[64]` 或 `typeInfos[64]`。

#### 写屏障边界

deletion barrier 必须在覆盖旧引用前完成可恢复的 shade：

- scalar ref store：直接原子 mark + pending/queue，然后执行 store，不跨多次 store 缓存；
- struct/ref-array bulk overwrite：先收集全部旧边到局部 batch，**flush 完成后**才 memcpy；
- GC owner 扫描：可在 TaskLease 保护下跨多个字段累计 batch。

这使最终可以删除 `activeLog_`，同时不会出现“旧边已覆盖、mark 还只存在寄存器/栈上且
无法在 STW 找回”的窗口。

#### 必须 flush 的边界

以下边界前 batch 必须为空：

- entry eviction；
- Region slice 到达 WU/wall budget；
- TaskLease complete 或 requeue；
- helper/assist 返回；
- bulk barrier 执行覆盖写；
- worker quiesce；
- MarkTermination 固定点检查。

#### 效果

- 同一 bitmap word 的多个 candidate 合并为一次 RMW；
- `newBits` 同时完成并发去重；
- NO_REF 不保留对象/TypeInfo；
- HAS_REF 一批 winner 最多触发一次该 Region 的 queue 状态转换；
- 代价是 winner 全局可见会延迟到 flush，因此 batch 生命周期必须被 TaskLease 或
  overwrite 前强制 flush 覆盖。

### 4.5 优化五（原优化六）：统一任务调度与终止

#### 两个 typed ready stack，不使用巨型 union或ring

最终 Scheduler 逻辑上仍有两个 lane，但物理实现是两个 ABA-safe tagged intrusive
stack：

```text
RegionReadyHead : uint32_t regionIdx + uint32_t ABA tag
ObjectReadyHead : uint32_t LARGE primary regionIdx + uint32_t ABA tag
```

每个 RegionInfo 已有独立的 `markTaskNextRegionIdx`，所以不需要按
`maxRegionCount` 再分配 ring slot、sequence array或 task node pool。Region 的
scan state保证同一时刻最多进入一个 ready head；64 位 tag解决 Region node反复
pop/requeue形成的 ABA。二者共享任务生命周期和账本，但不强行做成
`MSMarkTask` 大 union。

任务选择使用简单轮转，不保留为 RANGE/OLD 设计的多 lane DRR。Finalizer 分类和 WeakRef
最终处理不是并发强闭包 task：前者由 GC main 在 STW 直接完成，后者由 worker-local
discovery buffer 汇总。OBJECT 中的大引用数组是本步骤已知例外：它不可切分，只由 GC
worker 执行。

WeakRef discovery buffer 复用上轮 high-water 预留容量；普通路径只做 owner-local
append。极少数容量溢出走现有 `WeakRefBuffer::Insert()` 锁保护 fallback，不能丢条目，
也不因此恢复 MAIN_ONLY queue。MarkTermination 在所有 worker/assist quiesce 后统一
merge/deduplicate。精确规则为：

1. 每个WeakRef对象只在其scan bit首次从未扫描变成已扫描时登记一次；
2. local vector有容量时只写local；
3. local已满时该对象只调用一次`WeakRefBuffer::Insert()`，不得同时留在local；
4. MarkTermination为现有`WeakRefBuffer`增加`InsertBatch(begin,end)`，每个worker只获取
   一次global lock，把local元素批量插入当前`unordered_set`后clear local；
5. GC main调用现有`DrainWeakRefs()`一次，由该`unordered_set`统一去重并处理；
6. 处理结束后所有local和global fallback都必须为空；各local high-water供下一轮预留。

#### publication 与 TaskLease

```text
Publish(task):
  outstandingTotal.fetch_add(1, relaxed)  // provisional lease
  acquire unique state ownership
  TaggedPush(regionIdx, release)           // 无容量失败
  state CAS失败则fetch_sub(1, relaxed)回滚

Claim(task):
  TaggedPop(acquire)
  return TaskLease               // 不减 outstanding

Complete(lease):
  assert local batch empty
  assert child tasks already published
  outstandingTotal.fetch_sub(1, release)
```

intrusive push 没有容量概念，也不会分配，因此不实现 `Reserve/Cancel`、ring
sequence、chunk pool或容量不足 fallback。CAS loop是 lock-free 而不是 wait-free：
高竞争时可能重试，但 barrier leaf路径没有 mutex、分配和 safepoint。publication
短窗口可能出现 `outstanding>0` 但 ready head尚不可见，只会延迟收敛，不会提前终止。

预算到期的 Region 不是把同一个 lease 放回队列；它先发布一个新的 REGION task，使
outstanding 加一，再 complete 旧 lease。父任务派生 child 同理。

#### 自然终止与 STW 权威终止

ConcurrentMark 中：

```text
outstandingTotal == 0
  -> 只表示当前没有已发布或执行中的 task
  -> 请求进入 MarkTermination STW
```

它不是“并发 mutator 永远不会再产生 SATB 工作”的证明。无需为此长期维护
`activeProducers/activeWorkers/publishSeq` 多套正确性计数：STW handshake 会等待已经
进入 barrier/helper 的代码完成，随后 MarkTermination 重新检查：

1. 所有 worker 已 quiesce；
2. 所有局部 batch 已 flush；
3. 两个 typed ready stack 与 outstanding 一致；
4. Finalizer/WeakRef 的 STW source按语义顺序处理。

如果发现工作，GC main 在 STW 使用同一 worker kernel drain 到 fixed point。只有该
fixed point 才是回收正确性的最终权威。Release 不再扫描所有 collect-set Region 来
“修复” `IDLE + pending`；publication 协议必须保证这类状态不可形成。全量
`mark & ~scan`、state/pending 核验只在 Debug 或 `cjMSVerifyMark` 下执行，避免给
MarkTermination 增加 O(live Regions) 的 STW 尖峰。

#### 效果

- Root、owner、barrier、helper、assist 使用同一发布协议；
- 不再分别检查 WorkBuf、ready stack、range ticket、active worker 等多个终止条件；
- queued 与 in-flight 都由一个 outstanding 表达，避免 pop 瞬时归零；
- child-before-parent-complete 保证任务图闭包；
- MarkTermination 代码从“收集多种历史缓存”收敛为“quiesce + flush + typed-ready drain”。

### 4.6 优化六（原优化七）：WorkUnit Pacer/Assist

#### 为什么不能继续按对象数

一个 NO_REF 叶子、一个含两个引用的小对象和一个百万元素引用数组都算“一个对象”，实际
成本相差巨大。object-count debt 会低估引用密集对象和扫描字节，也会让 case26 这类
数百万叶子把 Assist 比例推得过高或过低。

#### 最终计量

每个 slice 局部累计三类原始事件：

| 事件 | 含义 |
|---|---|
| `resolvedRefs` | 完成目标地址/slot 解析的引用数 |
| `markWordRMW` | 实际执行的 mark word 原子更新数 |
| `scanWords` | owner 实际消费的 bitmap word 数 |

`resolvedRefs` 基础成本记为 1，`markWordRMW/scanWords` 使用固定整数成本；热循环不做
浮点、除法或共享原子。Q16 只用于 `WU/allocatedByte` Assist ratio。每轮结束发布
`totalCompletedWU` 和各来源 WU，更新单一 full-GC EWMA。`scanBytes`、对象数和 task 数
可以继续作为诊断指标，但不参与首版控制环。

#### Pacer

首版Pacer不维护全局`pendingWordCount`。每个HAS_REF pending必然由QUEUED/OWNED/
TaskLease之一覆盖，因此在线调度只读取：

```text
outstanding       = TaskLedger relaxed hint load；只有终止观察使用acquire
allocationRate    = 最近10ms窗口的allocated bytes/us
workDensity       = 上一轮WU/allocatedByte Q16 EWMA
backgroundThroughput = GC main/helper的WU/us Q16 EWMA
heapRunway        = max(0, min(currentHeapLimit, maxCapacity) - allocatedBytes)
```

最终实现不维护`RegionReady/ObjectReady`的共享计数。queued和in-flight工作已经全部计入
`outstanding`，再叠加ready hint会把同一任务计算两次，并为每次push/pop增加一条全局
原子更新。ready head只在自然终止和STW终止断言中检查是否为空，不参与helper扩容计算。
最近10ms窗口分配量为0时固定`runwayUs=UINT64_MAX`；否则
`runwayUs = heapRunway / allocationRate`。压力分三档：

| runway | pressure |
|---|---|
| `> 100000us`或当前无分配 | NORMAL |
| `20000us .. 100000us` | HIGH |
| `< 20000us` | CRITICAL |

`maxHelpers = min(configuredMaxMarkHelpers, cpuCount > 1 ? cpuCount - 1 : 0)`。每1ms由GC main
重新计算：

```text
if outstanding == 0 or maxHelpers == 0:
  helperTarget = 0
  if outstanding == 0:
    request MarkTermination STW
else:
  base = ceil(outstanding / 8)
  NORMAL:   target = base
  HIGH:     target = max(base, ceil(maxHelpers / 2))
  CRITICAL: target = maxHelpers
  helperTarget = clamp(target, 1, min(maxHelpers, outstanding))
```

helper target是park/wake hint；减小target只让多余helper在当前TaskLease完成后park，不能中断
对象或丢task。

首版slice预算固定为：

| 调用者 | wall budget | WU budget | capability |
|---|---:|---:|---|
| GC main | 1000us | `max(64, throughputEWMA * 1000)` | REGION + OBJECT |
| helper | 500us | `max(64, throughputEWMA * 500)` | REGION + OBJECT |
| mutator assist | 50us | `min(debt, max(64, throughputEWMA * 50))` | REGION only |

Q16乘法使用128位中间值或显式饱和helper。预算只在bitmap word/完整对象边界检查；small
最多允许超出一个16KiB对象，LARGE OBJECT一旦由GC main/helper领取则允许超出整个对象。

Pacer 只影响速度，不允许丢 task、跳过 pending Region 或绕开最终 STW fixed point。
不实现`expectedCycleWU/remainingWU/targetMarkEnd`多变量预测，也不维护第三个task-cost
EWMA。

#### Assist

small allocation 在 Region refill 时按约 32 KiB 一次性计费，兼容 LLVM inline fast
allocation 不逐对象回 Runtime 的事实；LARGE 按实际 bytes 计费：

```text
assistDebtWU += allocatedBytes * assistRatioWUPerByte
if debt >= assistThreshold:
    paidWU = RunMarkSlice(ASSIST_CAPS, debtBudget, wallBudget)
    assistDebtWU -= min(assistDebtWU, paidWU)
```

固定：

```text
assistThreshold = 512 WU
NORMAL   assistRatio = workDensity / 4
HIGH     assistRatio = workDensity / 2
CRITICAL assistRatio = workDensity
outstanding == 0时assistRatio = 0
```

ratio和debt使用Q16+saturating `uint64_t`。small allocation只在refill取得Region时按实际
Region可分配bytes记一次债；LARGE按logical allocated bytes记债。assist只领取REGION
task，并在bitmap word/对象边界检查预算；返回前必须flush batch、Complete/Requeue lease，
且不持有任何Region owner。small object最大为16KiB，因此一个对象内不抢占是有界的。

#### 大对象边界

因为明确排除 RangeQueue：

- LARGE HAS_REF 是一个不可切分 OBJECT task；
- helper/GC main 可以领取，mutator assist 不领取；
- 扫描开始后必须完成整个对象才设置完成状态；
- 超大引用数组可能形成单 worker 尾延迟，这是 Step62 接受的非目标，不能用隐藏的
  `begin/end` cursor 或 RangeTask 重新引入被排除设计。

#### 效果

- Assist 收费反映真正引用解析、RMW 和 Region word 扫描；
- helper 数可随剩余工作量收缩，减少大堆下固定线程策略的过度竞争；
- GC main/helper/assist 共享同一 slice 核心，统计口径一致；
- 删除 object debt、buffer debt、minor/major history 等重复状态。

## 5. LLVM 到 Runtime 的最终 ABI 与编译流程

### 5.1 Module GCMode 是第一道隔离

MSGC module 必须携带：

```text
!"Cangjie_GCMode" = i32 1
```

`llvm/include/llvm/IR/CJGCMode.h` 已定义 `CopyGC=0、MSGC=1`。所有 allocation/barrier
lowering 先读取此 flag；MSGC wrapper 出现在 CopyGC module、CopyGC fast allocation
出现在 MSGC module 都应在编译期报错。LTO 合并也必须拒绝冲突 GCMode，不能静默选一套
ABI。

### 5.2 allocation lowering

前端 allocation intrinsic 不需要复制一套 MSGC intrinsic。`CJRuntimeLowering` 在
`Cangjie_GCMode=1` 时完成：

1. 计算静态 object size/sizeClass/slotSize；
2. 从静态 TypeInfo 或 array component kind 计算 ScanClass；
3. 创建 MSGC static wrapper，写入 sizeClass、slotSize、scanClass 属性；
4. 动态泛型数组进入 MSGC dynamic dispatch wrapper；
5. wrapper 保持 `noinline`、`cj-frame-transparent`；
6. wrapper 到 slow callee 的调用保持 `musttail`；
7. finalizer/weak/special allocation 不伪装为普通 NO_REF fast allocation。

ScanClass 不作为新增 public ABI 参数传给 slow callee，否则会破坏原 wrapper/callee 的
`musttail` 签名一致性。fast path 从 wrapper attribute 取 ScanClass；fast miss 保持原
函数签名。slow callee进入 `MObject::NewObject` 或 `MArray::NewKnownWidthArray` 后，在仍
持有TypeInfo/component TypeInfo的位置调用唯一 `ClassifyMSScanClass()`，再经新增的
`HeapManager::AllocateMS(..., scanClass)` 和 `RegionSpaceMS::Allocate(..., scanClass)`
传到 `AllocBufferMS::SlowAlloc(sizeClass, scanClass)`。禁止到RegionSpace后根据AllocType
猜测分类。LLVM lit 与Runtime unit test按3.1的同一类型矩阵验证两端结果一致。

最终 fast allocation 索引：

```text
activeRegionOffset =
    activeRegionBase
  + (sizeClass * 2 + scanClass) * sizeof(MSRegionInfo*)
```

目标 emitter 对 BUMP 继续使用 `allocPtr/regionEnd`；BITMAP allocation 通过
`regionInfoArray` 算出 regionIdx，再由 `allocPlaneBase + regionIdx * 512` 定位 free
bitmap。slot 分配仍由 TLS owner 普通 load/store，不使用原子 RMW。

Runtime 内部进入 `AllocBufferMS::SlowAlloc(sizeClass, scanClass)` 后才显式携带
ScanClass；public wrapper/slow call ABI 不增加参数。

### 5.3 barrier lowering

`CJBarrierLowering` 保持目标无关职责：

- MSGC read ref/weak/static ref 是原始 64-bit load；
- MSGC struct read 是 memcpy；
- write intrinsic 改写到 `_Fast_MSGC` wrapper；
- generic write 按 struct barrier 语义处理；
- AArch64/X86-64只生成`barrierPhase`门控 fast path；其他target/ABI/object format直接
  保留可执行Runtime wrapper，不读取64B fast state或64B RegionInfo的compiler-visible
  offset。

MSGC 不移动对象，因此 read barrier 不做 tag repair、forward 或 load barrier。

写屏障保持：

```text
old = load(slot)
shade(old)             // 必须在覆盖前可恢复地发布
store(slot, new)
```

struct/static struct/ref array/struct array copy必须先批量 shade 目标范围内将被覆盖的旧引用，
flush MarkWordBatch 后才执行 memcpy/memmove。

Atomic操作不能沿用当前“relaxed load旧值、shade、独立atomic store”的实现：多个writer
交错时，load到的值可能不是本次store实际覆盖的值。最终协议固定为：

```text
AtomicWrite(slot, new, storeOrder):
  candidate = atomic_load(slot, relaxed)
  loop:
    ShadeAndPublish(candidate)            // 完整mark/pending/ready，无safepoint
    if compare_exchange_weak(
         slot, candidate, new,
         StoreSuccessOrder(storeOrder),
         StoreFailureOrder(storeOrder)):
      return
    // 失败时candidate已更新为当前实际值，重新shade后再尝试

AtomicSwap(slot, new, order):
  candidate = atomic_load(slot, relaxed)
  loop:
    ShadeAndPublish(candidate)
    if compare_exchange_weak(slot, candidate, new, order, FailureOrder(order)):
      return candidate

CompareAndSwap(slot, expected, new, successOrder, failureOrder):
  ShadeAndPublish(expected)               // 失败时允许保守多shade
  return compare_exchange(slot, expected, new, successOrder, failureOrder)
```

`ShadeAndPublish`完成前不能执行覆盖CAS，也不能进入safepoint。AtomicWrite的order映射固定为：

| store order | CAS success | CAS failure |
|---|---|---|
| relaxed | relaxed | relaxed |
| release | release | relaxed |
| seq_cst | seq_cst | seq_cst |

如果Runtime的`MemoryOrder`还暴露C++不允许用于atomic store的acquire/acq_rel，入口先按既有
API规则规范化或Debug fatal，不能把非法failure order传给CAS。CAS失败只允许多shade，
任何成功覆盖都必须已经发布它实际删除的旧边。

### 5.4 AArch64/X86 目标发码

最终把所有 magic offset 收口到 `llvm/include/llvm/CodeGen/CJMSGCABI.h`：

- `MSGCFastPathState` 64B layout；
- `MSRegionInfo` 64B layout；
- Region shift 15、plane stride 512；
- 36项 `slotSize/totalSlots/bitmapWords/validTailMask/divMagic` descriptor；
- `activeRegions[36][2]` base/stride；
- ScanClass/layout/control bits；
- 现有 Runtime write-barrier wrapper 名称。

`AArch64MSGCEmitter.cpp` 与 `X86MSGCEmitter.cpp` 使用同一常量，不再各自保留
`MSRegionInfoAllocBitmapOffset/MarkBitmapOffset/activeLogOffset`。

scalar write barrier 的目标 fast path：

```text
phase = acquire_load(CJ_MRT_MSGCFastPathState.barrierPhase)
if phase == IDLE:
    raw store
else:
    call existing Runtime write-barrier wrapper
```

Runtime wrapper统一完成 deletion shade、对象解析、MarkWordBatch flush、
pending/REGION或OBJECT publication，再执行覆盖写。scalar wrapper使用栈上固定容量batch
并在返回前同步flush；bulk wrapper先把目标范围内旧边汇入局部batch，flush完成后才执行
memcpy/memmove。由此 compiler 不再重复实现 collect-set/outside-set/LARGE 分流，也不需要
新增 `CJ_MRT_MSGCPublishRegionWord` 或 `CJ_MRT_MSGCShadeOldRefSlow` 符号。

这里刻意不把mark/plane发布继续展开到目标代码：Runtime wrapper调用期间mutator仍处于
受运行时管理的调用边界，完整publication返回前不会执行覆盖写；MarkTermination STW
handshake会等待该调用退出，再检查统一Scheduler。这样既保留IDLE零shade快路径，又避免
compiler内联序列与Runtime发布协议漂移。

### 5.5 safepoint/statepoint 契约

fast allocation/slow fallback 必须维持现有 Cangjie statepoint 语义：

- 成功目标展开只记录一次对应 CJ stack map；
- fallback wrapper 自身可执行且保持 frame-transparent；
- 不因新增 ScanClass属性或64B state load重复记录 stack map；
- AArch64 `AArch64CangjieCallSize.cpp` 与实际发码长度同步；
- X86/AArch64 unsupported combination 回到 slow call，不生成半套 fast path。

barrier 的IDLE inline序列不得引入未声明safepoint。MARK分支调用原有Runtime wrapper，
仍沿用已审核的调用和stackmap契约，不额外产生publish-helper statepoint。

### 5.6 Runtime ABI 发布

Runtime 启动时发布：

```text
CJ_MRT_MSGCFastPathState.barrierPhase
CJ_MRT_MSGCFastPathState.currentMarkEpoch
CJ_MRT_MSGCFastPathState.heapReservedStart/end
CJ_MRT_MSGCFastPathState.regionInfoArray
CJ_MRT_MSGCFastPathState.alloc/mark/scanPlaneBase
CJ_MRT_MSGCFastPathState.reservedAbi
```

进入 `MS_MARK_SETUP/MS_CONCURRENT_MARK/MS_MARK_TERMINATION` 发布 mark barrier phase；回到
IDLE 必须在 MarkTermination STW 内先安装 idle barrier、release 发布 phase=0，再恢复
mutator。

ABI contract test 同时编译 Runtime `static_assert` 和 LLVM 常量测试。替换 Runtime dylib
后必须使用同一 SDK 重新编译 `.cj` 用例；禁止用旧 binary 验证新布局。

## 6. Runtime 从分配到 Scavenge 的完整流程

| 阶段 | 当前主结构 | Step62 最终结构 |
|---|---|---|
| LLVM | sizeClass wrapper、activeLog barrier ABI | 增加ScanClass/三plane分配ABI；写屏障只做phase gate，MARK统一进Runtime wrapper发布任务 |
| 分配 | `activeRegions[36]` | `activeRegions[36][2]` |
| PreEnumRoots | stop Sweep、清 mark、每Region generation reclassify | O(1) steal Central supply建立collect-set；用mark epoch限定成员，ACTIVE不flush |
| EnumRoots | Root 进入 WorkStack/grey object | Root 进入 MarkBatch/typed task |
| ConcurrentMark | `gcThreadWork_ + globalFull_ + PerPGCWork` | Region owner + Scheduler/Ledger + WorkUnit |
| MarkTermination | 汇总 activeLog/WorkBuf/deferred queues | quiesce、两个ready stack fixed point；全量plane校验仅Debug |
| PostMark | 统计并移交 Sweep | 按collect-set/ScanClass移交，Mark ready task已不存在 |
| Sweep | mark 重建 alloc，per-Region generation CAS claim | mark重建alloc；unswept tagged-pop唯一claim；全局`sweepCycleId`只服务pacing/worker/统计 |
| Scavenge | FreeManager释放空 Region物理页 | 语义不变，不接入 Mark ledger |

### 6.1 分配

完整 fast/slow 分配流程：

```text
LLVM wrapper(sizeClass, slotSize, scanClass)
  -> TLS activeRegions[sizeClass][scanClass]
  -> BUMP:
       allocPtr < regionEnd -> bump allocate
  -> BITMAP:
       allocSummary -> ctz word -> allocWord free bit 1->0
  -> miss:
       Runtime SlowAlloc(sizeClass, scanClass)
       -> 归还旧 ACTIVE
       -> 同 size-class bucket 的对应ScanClass lane取 partial swept / empty / fresh
       -> 安装为新 ACTIVE
       -> 计入 allocation bytes、Sweep debt、Mark WorkUnit debt
```

ACTIVE Region 由单个 AllocBufferMS owner 分配：

- BUMP hot path不更新 alloc plane；
- BUMP 只有自然耗尽/归还 Central 时才 materialize 已分配 prefix；
- BITMAP hot path普通 load/store alloc free bit 和 allocSummary；
- ACTIVE 不因 PreEnumRoots 被 flush；
- classless empty或cold-free Region在PreEnum后重新绑定时，必须先清本Region有效范围的
  mark plane；HAS_REF还要清scan/pending/state，再发布为OUTSIDE_SET可分配Region；
- 归还 Central 后成为 SEALED；其 stack publication本身就是所有权交接；
- PreEnum 后归还或新创建的 Region 不加入当前 collect-set。

对象 stateWord/TypeInfo 初始化完成后才允许其引用对其他线程可见。普通分配本来就不应
调用 `MarkNewObject`；`BaseObject::OnFinalizerCreated()`只进入
`FinalizerWorkList::RegisterFinalizer()`。Finalizer list在注册后和删除working-list
root前，持有同一`listLock`调用Finalizer-only `MarkNewObject`完成同步publication。
该入口不能扩展回普通新对象mark。

### 6.2 PreEnumRoots

最终 `MarkSweepCollector::PreEnumRoots()`：

1. 发布本轮 active collector/MarkContext，但尚不启动 worker；
2. `CentralRegionPool::StopAndWaitForAllSweep()`关闭上一轮Sweep admission，等待
   background/lazy/assist/helper owner退出，并把残留unswept全部扫完后push回swept
   supply；
3. 进入短 STW；
4. 推进 `markEpoch`；
5. 原子 steal Central 各 lane 当时的 partial/full swept chain，把其中 SEALED small
   Region 固定为 `collectEpoch=currentMarkEpoch`；
6. 同时snapshot LARGE primary registry：边界前的LARGE进入本轮large collect-set并写
   `collectEpoch=currentMarkEpoch`；边界后的LARGE注册到下一轮supply；
7. collect-set BUMP 不应存在；若存在先 materialize 并视为生命周期 bug；
8. 从 allocation-eligible list 隔离本轮 collect-set，Mark/Sweep 完成前不能重新发给
   mutator；
9. 清本轮涉及 Region 的 mark plane；HAS_REF 同时清 scan plane 和
   pending/state；
10. 只清本轮large collect-set的LARGE marked flag；
11. 对 TLS ACTIVE 清本轮作为Mark工作区使用的mark plane；HAS_REF再清scan、
    pending/state，但不复制、不读取也不修改其live alloc plane，更不改变所有权；
12. Debug 构建清 touched-OUTSIDE_SET side bitset/list；Release 不维护该结构；
13. 初始化 Region/Object 两个 typed ready head、TaskLedger、worker-local MarkBatch、
    WeakRef discovery buffer 和 WorkUnit cycle counters；
14. 按本轮collect-set Region数初始化Sweep ratio/credit/统计，设置`sweepDone=false`，
    再以release推进全局`sweepCycleId`；此时Sweep admission仍关闭；
15. 写入 plane base和 epoch，保持 barrier phase为IDLE，退出 STW；随后
    `EnumRoots/TransitionToGCPhase` 才以 release 发布 mark barrier phase。

这里的“快照”是 **collect-set membership + SEALED alloc plane 生命周期**，不是把全部
alloc bitmap 复制成另一份内存。PreEnum 后新分配/新归还 Region 通过
`collectEpoch != current` 自动排除。

边界分类固定如下：

| PreEnum边界时的来源 | 进入本轮collect-set | 后续去向 |
|---|---|---|
| StopAndWait后位于Central partial/full swept supply | 是 | Mark结束后发布到本轮unswept |
| PreEnum时仍在TLS ACTIVE | 否 | 继续ACTIVE；归还后进入下一轮Central supply |
| PreEnum后新建或从empty/cold-free绑定的Region | 否 | 下一轮Central supply |
| PreEnum后才归还Central的Region | 否 | 下一轮Central supply |
| 上一轮残留unswept | 先由StopAndWait完成Sweep，再进入swept supply，因此是 | 与其他边界前Central Region一致 |
| PreEnum边界前已注册的LARGE primary | 是 | 本轮按LARGE mark结果保留或回收 |
| PreEnum后注册的LARGE primary | 否 | 下一轮large supply |

ACTIVE不会因连续多轮停留而被强制detach；只有它在某一轮PreEnum之前已经归还Central，
才在该轮被steal进collect-set。这与前面确定的“不flush ACTIVE、再等下一轮”一致。

三个 plane 使用独立 demand-zero mapping；NO_REF Region 不触碰 scan plane，其 scan 页
不会因该Region而主动commit。若相邻HAS_REF Region与其共享物理页，该页可以已经驻留，
但NO_REF路径仍不读取、清零或统计scan内容。

### 6.3 EnumRoots

最终 `EnumRoots()`：

1. `TransitionToGCPhase(MS_MARK_SETUP, true)`；
2. Runtime/LLVM fast state 切换到 mark barrier；
3. 每个 mutator 在 safepoint 枚举自己的 stack roots，写入其 root staging buffer；
4. GC main 合并 staging roots，枚举 global/export/runtime roots；
5. 每个 root 进入统一 `DiscoverRef()`，不直接放普通对象 WorkBuf；
6. GC main 使用局部 MarkWordBatch 合并 root targets；
7. EnumRoots 返回前强制 flush batch，产生首批 REGION/OBJECT task。

Root 指向：

| 目标 | 处理 |
|---|---|
| collect-set NO_REF small | mark winner 后结束 |
| collect-set HAS_REF small | mark + pending + REGION |
| ACTIVE NO_REF small | 验证精确对象，visit 后结束 |
| ACTIVE HAS_REF small | 验证精确对象，visit + pending + REGION |
| LARGE NO_REF | LARGE marked 后结束 |
| LARGE HAS_REF | LARGE marked + OBJECT |
| WeakRef | HAS_REF owner记录到 local discovery buffer，不追踪 referent |
| Finalizer | 普通闭包阶段不分类；MarkTermination 直接处理注册表 |
| LARGE/special | 无法由 small Region表达时进入 OBJECT |

不会把整个 ACTIVE Region 作为 root，也不会创建 ActiveRootSnapshot。Root 闭包自然穿过
实际可达的 ACTIVE HAS_REF 对象。

### 6.4 ConcurrentMark

进入 `MS_CONCURRENT_MARK` 后：

```text
GC main:
  while not requestTermination:
      update Pacer window
      resize helper target
      RunMarkSlice(MAIN_CAPS, mainBudget)

helper:
  RunMarkSlice(HELPER_CAPS, helperBudget)

mutator allocation:
  accumulate assist debt
  debt >= threshold -> RunMarkSlice(ASSIST_CAPS, debt/wall budget)

write barrier:
  shade overwritten old edge before store
  small HAS_REF winner -> pending/REGION
```

三个调用者共用：

- typed ready claim；
- Region owner 状态机；
- `DiscoverRef`；
- MarkWordBatch；
- TaskLease；
- WorkUnit counter。

GC main/helper 可处理 REGION/OBJECT；assist 只处理 REGION。
同一 Region 只有一个 owner，但不同 Region 并行。owner 达到 slice budget时，在对象或
bitmap word 边界安全 requeue。

当前 ordinary `gcThreadWork_ + globalFull_ LFStack`、per-P `gcWork_`、WorkBufPool 和
`activeLog_` 不再参与最终 Mark。

当 `outstandingTotal==0`，GC main请求 MarkTermination；此观察允许保守提前，不能直接
发布 Sweep。

### 6.5 MarkTermination

最终顺序保持语义严格：

1. 进入 `MS_MARK_TERMINATION` STW，等待所有 mutator 停止；
2. 停止并 join helper；所有正在执行的 barrier、bulk batch 和 TaskLease 必须已完成或
   转交；
3. 清 active collector 的 mutator 入口，禁止再产生并发 assist；
4. flush GC main/helper 的局部 MarkWordBatch；
5. acquire 读取 TaskLedger 与两个 queue；publication 协议不允许形成
   `IDLE + pending!=0`，Release 不做 collect-set 全量 repair；
6. 使用同一个 `RunMarkSlice(MAIN_CAPS, unlimited)` drain REGION/OBJECT 到 fixed point；
7. 在Finalizer registry/gc gate下取得稳定注册表视图；并发注册要么已经被该视图看到，
   要么已经在同一`listLock`临界区内通过Finalizer-only `MarkNewObject()`完成root发布；
8. 基于普通闭包稳定mark plane分类不可达Finalizer，把新finalizable对象通过GC-main
   `DiscoverRef()`作为resurrection roots发布并drain到第二个fixed point；
9. mark plane最终稳定后处理 WeakRef：未标记 referent 清空，已标记保留；
10. Debug 或 `cjMSVerifyMark` 下扫描 touched/collect-set Region，验证
    `mark & ~scan==0`、state/pending/queue 闭合；Release 不做全量 Region 扫描；
11. Release 确认 `outstandingTotal==0`、两个 queue为空、所有 worker batch和
    WeakRef discovery buffer已交接；全 Region state/pending 由 Debug 校验；
12. 在 STW 内切到 IDLE barrier、release 发布 `barrierPhase=0`，再恢复 mutator。

Finalizer/WeakRef不依赖普通allocation-time `MarkNewObject`。Finalizer创建和working-root
删除只使用3.6.1的两个专用API；Finalizer resurrection是明确的第二次root closure，
WeakRef必须在resurrection closure后处理。

STW fixed point 的关键关系：

```text
for every collect-set HAS_REF Region:
    ((~alloc & valid) & mark & ~scan) == 0

for every Debug-touched OUTSIDE_SET HAS_REF Region:
    (mark & ~scan) == 0

Scheduler:
    outstandingTotal == 0
    all lanes empty
    no owner, no local batch
```

Release 的正确性权威是 TaskLedger publication、STW 下 producer 冻结和两个ready stack fixed
point；plane 差集是 Debug/验证模式的重建校验，不进入默认 STW 线性路径。

### 6.6 PostMarkTermination

mutator 已恢复、barrier 已是 IDLE 后，`PostMarkTermination()`：

1. 通知已经发布的 finalizable work；
2. 保持Sweep admission关闭，确认本轮所有unswept head尚未对consumer可见；
3. 仅遍历 current small/large collect-set计算marked object/live bytes/WU统计；
4. 只处理本轮large collect-set：未标记LARGE/SPAN进入large hot cache，marked LARGE
   转入下一轮large supply；PreEnum后新分配的LARGE不在该集合中；
5. 更新full-GC Pacer EWMA、heap utilization和下轮trigger；
6. 把small collect-set按`(sizeClass, scanClass)`一次性发布到本轮unswept heads；
7. 确认本轮`sweepDone=false`；默认模式在所有head发布完成后以release打开Sweep
   admission，`CJ_MS_STW_SWEEP=1`则保持CLOSED；
8. 默认模式的OOM路径可按既有plan做一次emergency parallel Sweep/flush cache；诊断模式
   禁止该parallel例外，内存仍不足时进入下一次GC，由其PreEnum串行force-drain；
9. 默认模式通知BackgroundProcessor执行Sweep；诊断模式不通知任何并发Sweep consumer；
10. 通知Scavenge检查已有空闲页。

PostMark 不再切换 barrier，也不再收集 per-P WorkBuf/activeLog；这些都已在 MarkTermination
前由最终协议消失或收口。

### 6.7 Sweep

#### 6.7.1 当前实现与最终实现的区别

当前实现把同一个Sweep事实编码了三次：

| Sweep事实 | 当前表达 |
|---|---|
| 某Region属于哪一轮 | `globalSweepGen_`与Region内`sweepGen`比较 |
| 待Sweep/claim中/已Sweep | Region内`sg-2 / sg-1 / sg` |
| Region实际位于哪里 | partial/full swept/unswept list |

当前`ClaimUnsweptHeadForSweep()`已经先从unswept list唯一摘取Region，随后再做
`sweepGen: sg-2 -> sg-1` CAS；这两个动作都在争夺同一份ownership。最终只保留容器和
admission lease作为Region正确性状态：

流程位置也发生变化：当前`PreEnumRoots()`在STW内执行
`ReclassifyUnsweptToSwept() + PrepareForSweep()`，把Central swept容器翻成unswept并推进
generation；最终PreEnum只把边界时的swept supply steal到GC私有collect-set，Mark期间
不公开为unswept，直到PostMark闭包稳定后才一次性发布给Sweep。

| Sweep事实 | 最终唯一表达 |
|---|---|
| 本轮需要Sweep | Region位于本轮GC私有collect-set，PostMark后位于本轮unswept head |
| 已被某worker claim | `PopUnswept(acquire)`成功，且该worker持有Sweep admission lease |
| 已完成Sweep | 结果已经`PushSwept(release)`到partial/full swept supply或empty cache |
| Sweep全局轮次 | Central/SweepContext的`sweepCycleId`，只服务debt、延迟worker和统计 |

因此删除的是 `MSRegionInfo::sweepGen` 和 `CasSweepGen()`，不是把所有跨轮上下文都删除。
`collectEpoch`也不是新的Sweep generation：它只回答“该Region是否属于本轮Mark
collect-set”，Sweep在Release中靠unswept membership工作，只在Debug检查epoch。

#### 6.7.2 不用每Region sweepGen时怎样做代际转换

这里的“代际”是Sweep周期代次，不是young/old分代GC。Region沿容器所有权完成转换：

```text
Central partial/full swept supply
        |
        | PreEnum：关闭上一轮Sweep并O(1) steal各head
        v
GC-private collectSet[N]
        |
        | 本轮Mark闭包稳定；Region始终不对allocator可见
        v
PostMark unswept[N]
        |
        | EnterSweep + PopUnswept
        v
Sweep owner
        |
        | 重建alloc plane，先发布结果再释放lease
        v
Central partial/full swept supply      -> 供分配，也作为collectSet[N+1]候选
或 classless empty / FreeManager
```

PreEnum steal后，原Central partial/full head立即变空；同一组head从此自然成为“下一轮
supply”，无需再分配两套数组：

```text
TLS ACTIVE at PreEnum
PreEnum后新建Region
PreEnum后才归还Central的Region
Sweep[N]完成后产生的partial/full结果
        |
        +----> 当前Central swept supply ----> 下次PreEnum才进入collectSet[N+1]
```

物理Region不需要写`generation=N+1`。它在哪一代由“当前被哪条私有链/公开head拥有”
决定。PreEnum仍会遍历steal出的chain写`collectEpoch`并清plane，因此head转移是O(1)，
但本轮Mark初始化本身仍按collect-set Region数线性；方案不虚构“整个PreEnum O(1)”。

链API固定为：

```text
StealAllSwept(lane):
  在STW且上一轮Sweep admission已关闭、active=0后
  exchange partialSwept/fullSwept tagged head为INVALID
  清各head近似count
  返回两个plain head

BuildCollectSet(lane):
  遍历两个stolen chain完成collectEpoch/plane初始化
  同一遍历得到准确tail/count
  令partial.tail.next = full.head并形成一个RegionChain{head, tail, count}

AdoptCollectSetAsUnswept(lane):
  仅在PostMark、admission仍关闭且该lane unswept head为空时执行
  collectSet.tail.next = INVALID
  先写relaxed近似count
  再用release store/CAS发布{collectSet.head, oldTag+1}
  清GC私有RegionChain
```

Central tagged stack不并发维护tail；准确tail只在PreEnum已经必须执行的线性初始化遍历中
构建。这样head steal是O(1)，PostMark移交也是O(1)，且不为并发Treiber stack增加一套难以
维护的tail协议。

#### 6.7.3 为什么不会扫到ACTIVE或PreEnum后的新对象

Sweep owner只能从PostMark发布的current-collect-set `unswept` tagged stack中pop。
ACTIVE、PreEnum后新建、PreEnum后归还的Region从未进入该stack。即使这些OUTSIDE_SET
Region在本轮对象图遍历中产生mark/scan工作，它们也只进入Mark ready stack，不会被加入
Sweep unswept。

一个Region若连续多轮保持TLS ACTIVE，就连续多轮延后Sweep；不强制detach。它在某次
PreEnum前归还Central后，才会被下一次head steal捕获。这个规则完全由边界时的容器
membership决定，不需要per-Region generation。

#### 6.7.4 small Sweep公式

small Region 每 word：

```text
validMask       = lowBits(validSlots)
oldAllocated    = (~allocWord) & validMask
liveBits        = markWord & validMask
deadObjectBits  = oldAllocated & ~markWord
nextFreeWord    = (~markWord) & validMask

zero object memory for deadObjectBits
allocWord = nextFreeWord
freeSlotCount = sizeClassDesc.totalSlots - popcount(all liveBits)
allocSummary bit = (nextFreeWord != 0)
```

mark bit若出现在旧 free slot 是 GC 不变量错误，Debug 检查
`mark & ~oldAllocated == 0`。Release 可直接用 `~mark` 重建 free bitmap，因为最终 mark
只允许指向本轮已分配 slot。

scan plane 不决定存活，也不参与 `SweepRegion()`；它在 owner 全部退出后保持只读，下一轮
PreEnum 再清。NO_REF/HAS_REF 使用相同回收公式，区别只在 Mark 是否产生扫描工作。

结果分类：

```text
live == 0       -> empty swept cache
0 < free < all -> partial swept[sizeClass][scanClass]
free == 0      -> full swept[sizeClass][scanClass]
```

完全空 Region 可清除 sizeClass/ScanClass 后进入 classless empty cache。非空 Region 保持
二维分类。现有 background Sweep、lazy Sweep、allocation Sweep assist 和
`CJ_MS_STW_SWEEP` 诊断路径可继续使用，但都必须复用同一个
`PopUnswept(acquire) -> SweepRegion -> PushSwept(release)` 协议。pop 后 Region 已从
unswept stack消失。默认并发consumer的Sweep admission active count在结果发布后才递减；
诊断/PreEnum独占consumer在admission已关闭且active=0时运行，不增加active count。两者都
不需要`sg-1=CLAIMED`状态或`CasSweepGen()`。

#### 6.7.5 publication、claim与完成协议

删除per-Region generation claim的前提是固定publication顺序：

```text
PreEnum:
  close previous admission
  wait previous active == 0
  force-drain residual unswept
  steal swept supply -> collectSet[N]
  initialize SweepContext[N], sweepDone = false

PostMark[N]:
  admission remains closed
  publish all collect-set chains to unswept heads
  if CJ_MS_STW_SWEEP is disabled:
    release open admission
    notify sweepers
  else:
    keep admission closed

Sweeper:
  EnterSweep(cycleId)          // active +1，closed或cycle不匹配则失败
  region = PopUnswept(acquire)
  if region == null:
      LeaveSweep()
      TryPublishSweepDone()
      return
  SweepRegion(region)
  PushPartial/FullSwept(release)
  LeaveSweep()                 // active -1，必须晚于结果发布
  TryPublishSweepDone()

TryPublishSweepDone:
  observe all unswept heads empty
  CAS admission OPEN+active0 -> CLOSED+active0
  recheck all unswept heads empty
  release sweepDone = true
```

完成发布必须先用packed admission word从`OPEN+0`原子关闭入口，不能分别读取“open、
active==0、head为空”后直接写`sweepDone`；否则新的worker可能在检查间隙进入。PostMark
在open前已经发布完全部head，之后没有任何unswept producer，所以成功关闭且复查head
为空就构成该轮Sweep fixed point。若CAS失败，当前执行者不发布done，由最后退出的
worker重试。

不能边发布collect-set chain边允许完成判定，否则sweeper可能在后续chain发布前观察
“head空且active为0”。默认并发路径通过admission lease进入；`CJ_MS_STW_SWEEP=1`路径让
admission保持关闭，并由下一轮PreEnum中的GC线程取得STW独占权。两者不改变head
publication和`SweepRegion -> PushSwept`结果发布顺序，精确定义见6.7.8。

#### 6.7.6 全局 sweepCycleId 保留什么、不保留什么

全局最小SweepContext为：

```text
sweepCycleId                 // 单调递增的全局周期号
sweepAdmission               // CLOSED bit + active owner count
sweepDone
sweepRegionsPerAllocScaled   // 本轮Pacing ratio
sweepCredit / cycleStats
partial/full/unswept heads
```

PreEnum在得到collect-set规模后先初始化ratio/credit/stats，最后以release发布新的
`sweepCycleId`。P在下一次成功分配记债时acquire读取该ID；若
`AllocBufferMS::sweepDebtCycleId`不匹配，只重置本P的旧debt和统计。后台/线程池任务捕获
cycleId，延迟执行时若ID不匹配就在`EnterSweep`前退出，避免把旧任务的credit或统计记入
新一轮。Mark期间admission关闭，P可以累计本轮debt但不能进入Sweep；debt保留到PostMark
打开admission后按per-call cap逐步偿还，不能因为“head尚未发布”把本轮debt清零。

`sweepCycleId`不出现在以下判断中：

- 不通过`region.sweepCycleId == current`判断某Region是否待扫；
- 不通过`cycleId-1`编码CLAIMED；
- 不通过CAS Region字段取得ownership；
- 不要求Sweep结束后逐Region写回新ID。

也就是说，全局ID保留“跨轮控制面”，Region的“数据面代际”完全由head ownership转换。

#### 6.7.7 下一轮PreEnum如何封闭上一轮

下一轮PreEnum先处理上一轮SweepContext：

1. 若`sweepDone=true`且admission已经CLOSED，直接确认active为0；
2. 若尚未完成，关闭admission并等待active owner全部退出；
3. GC线程使用同一个consumer kernel串行drain所有残留unswept；
4. 每个结果都先push到swept supply；此时GC线程持STW独占权，不创建普通admission lease；
5. 确认所有unswept为空、active为0；
6. 再steal此刻完整的swept supply建立下一轮collect-set。

因此不存在“N轮unswept残留与N+1轮collect-set混在同一head”的窗口。旧per-Region
`sweepGen`原来用于防止的跨轮混淆，改由“关闭、quiesce、drain、再steal”的全局边界协议
直接消除。

#### 6.7.8 `CJ_MS_STW_SWEEP`诊断路径的唯一语义

Runtime初始化时只读取一次环境变量；只有精确值`CJ_MS_STW_SWEEP=1`启用诊断模式，并把
结果保存在Runtime-only只读配置中。它不是普通调优开关。

默认模式：

```text
PostMark发布全部unswept
  -> release打开admission
  -> 允许background/lazy/allocation-assist/parallel Sweep
  -> 下一轮PreEnum只force-drain未完成残余
```

诊断模式：

```text
PostMark发布全部unswept
  -> admission保持CLOSED
  -> 不通知background worker
  -> lazy/allocation-assist/parallel Sweep全部拒绝进入
  -> 下一轮PreEnum确认active=0
  -> GC线程在STW下使用同一PopUnswept/SweepRegion/PushSwept核心串行force-drain
  -> drain完成后才能steal swept supply建立下一轮collect-set
```

诊断模式的GC线程持有“admission已关闭且world stopped”的独占权，不调用普通
`EnterSweep()`增加active count；但对象清理和结果发布必须复用完全相同的
`SweepRegion -> PushSwept` kernel。该开关不改变collect-set、unswept head、alloc重建公式、
large回收或Scavenge语义。启动和每次force-drain必须打印明确REPORT，测试同时验证启用时
没有background/lazy/assist Sweep owner。

### 6.8 Scavenge

Scavenge 不参与对象图，也不读取 mark/scan plane：

```text
Sweep 得到 empty swept Region / dead LARGE span
  -> Central empty cache / large hot cache
  -> 达到 flush/水位条件
  -> FreeManager hot-free view
  -> shard/cursor claim candidate runs
  -> madvise/decommit 归还物理页
  -> publish cold-free / scavenged 状态
  -> 后续分配可重新 commit/reuse
```

BackgroundProcessor 继续按 `scavengeThreshold`、target bytes、quantum 和 rounds 执行，
统计 requested/released bytes、claimed/released Region、release failures。六项 Mark
优化只可能通过“更快得到准确 empty set”间接影响 Scavenge，不把 Scheduler/TaskLedger
与 Sweep/Scavenge 的正确性账本合并。

执行时仍保持：

- 只有 Sweep 完成并发布为空的 Region 才可 Scavenge；
- collect-set Mark/owner 生命周期结束前不得释放物理页；
- cold-free Region 重用时重新初始化 RegionInfo/plane，并递增 Debug generation；
- Scavenge 失败只影响 RSS，不改变对象存活。

## 7. 并发状态机、内存序与终止证明

### 7.1 mark 到 pending 的 happens-before

producer 对一个 HAS_REF word：

```text
markWord.fetch_or(mask, relaxed)
pendingWordSummary.fetch_or(1 << wordIdx, release)
```

owner：

```text
summary = pendingWordSummary.exchange(0, acquire)
markWord.load(relaxed)
```

pending 的 release/acquire 使 owner 在看到 summary bit 后必能看到此前 mark OR。mark RMW
自身只需 relaxed；winner 由该原子 word 的 modification order 决定。

NO_REF 没有 pending。其 mark bit 在 mutator barrier 返回后通过 STW handshake/quiesce
进入最终观察窗口；删除旧边前仍必须在同一线程 sequenced-before 完成 mark RMW。

### 7.2 安全发布 REGION task

RegionReady 是复用 `markTaskNextRegionIdx` 的 ABA-safe intrusive stack；同一 Region
最多一个 QUEUED node。`TryPublishRegion` 先建立 provisional outstanding，再竞争唯一
state：

```text
pending already published
if state.load(relaxed) != IDLE:
    return

outstandingTotal.fetch_add(1, relaxed)

if CAS(state, IDLE -> QUEUED, acquire, relaxed):
    RegionReady.TaggedPush(regionIdx, release)
else:
    outstandingTotal.fetch_sub(1, relaxed)
```

成功的 state CAS使用 acquire，是为了在 Region 曾由旧 owner
`OWNED -> IDLE(release)` 后把旧 scan-plane写入传递给下一次 ready push；CAS失败路径
不消费任何 payload，只需 relaxed。ObjectReady 对 LARGE primary使用同一协议。两条
ready stack都没有 `Reserve/Cancel`、ring sequence、chunk pool和动态分配。consumer
acquire pop 后用 atomic relaxed store做 `QUEUED -> OWNED`；Release 依赖唯一 node
协议，Debug才用 CAS/assert 验证旧状态。QUEUED 到 ready-head publish之间即使暂时无可 pop task，
`outstandingTotal>0` 也只会延迟终止。

### 7.3 owner 扫描与 scan plane

同一 Region 只有一个 OWNED worker，所以 scan word 可普通 load/store。完成对象字段
枚举后：

```text
scanWord |= objectBit
```

在 TaskLease complete 前执行：

```text
flush MarkWordBatch
release complete outstanding
```

MarkTermination 对 outstanding 使用 acquire 观察，因此 owner 的 scan 写、child
publication 和 batch flush 均先于其确认 task 全部完成。Debug 如需并发读取 scan plane，
只能在 owner quiesce 后读取；不为诊断把 owner-only store改成原子 RMW。

### 7.4 owner 释放的 lost-wakeup 封闭

owner 消费完当前 summary 后：

```text
if pending.load(acquire) != 0:
    continue/requeue

state.store(IDLE, release)

if pending.load(acquire) != 0:
    TryPublishRegion()
```

竞态分三种：

1. producer 在 store IDLE 前提交：看到 OWNED，只写 pending；owner 第一次或第二次检查
   看见；
2. producer 在 store IDLE 后提交：看到 IDLE，producer 自己 queue；
3. producer 与 owner 第二次检查交错：二者都可尝试 CAS，但只有一个从 IDLE 赢得
   QUEUED。

因此不需要 DIRTY bit，也不会丢掉最后一个 word。

### 7.5 requeue 与 parent/child

owner budget 到期：

```text
flush local batch
pending.fetch_or(unprocessedWordSummary, release)
outstanding.fetch_add(1, relaxed)    // replacement provisional lease
state.store(QUEUED, relaxed)         // owner 独占地执行 OWNED -> QUEUED
RegionReady.TaggedPush(regionIdx, release)
complete current lease               // outstanding -1
```

replacement publish 前不能先把 state 置为 IDLE，否则 producer 可能另发一个同 Region
task；也不能先 complete 旧 lease，否则 outstanding 可瞬时归零。

`state.store(QUEUED)` 不需要 release：同一线程随后对 ready head的 release push已经发布
scan写入和node payload。相反，最终 `OWNED -> IDLE` 必须 release，因为下一次 producer
通过 acquire CAS接力发布。

对象扫描发现 child同样先 publish child，再 complete parent。即使两者指向同一 Region，
state/summary 会合并，不要求每个 child 都有一个 task。

### 7.6 barrier 与 STW

scalar barrier 的 mark/pending/queue 序列不可包含 safepoint；如果调用 Runtime helper，
helper 必须完成 publication 后才能返回。bulk barrier 的局部 batch 必须在覆盖写前
flush，不能跨 safepoint保存未发布 old edge。

自然终止和 barrier publication 短暂交错，最多使 GC 提前请求 STW：

- STW handshake 等 barrier 退出；
- helper/owner quiesce；
- GC main acquire 读取两条 typed ready head和TaskLedger；
- fixed-point drain吸收全部已发布工作。

所以最终不使用 `activeProducer` 参与正确性或终止判断。

### 7.7 cycle metadata 与 Region 重用

PreEnum STW 以 release 发布 `currentMarkEpoch/collectEpoch/plane base`，EnumRoots phase
切换后 worker acquire 读取。整个 Mark 周期：

- collect-set membership 不变；
- sizeClass/ScanClass 不变；
- Region 不重用；
- alloc plane 不被 allocator修改。

任务全部归零、owner 全为 IDLE、MarkTermination 完成后，才允许发布本轮unswept；
Sweep完成并把结果push到swept/empty容器后才允许相应Region重用。Release不保留或校验
per-Region mark/sweep generation；Debug task claim可通过独立side table和epoch捕获
生命周期bug。跨轮Sweep debt、延迟worker和统计只读取Central级`sweepCycleId`，不占
64B RegionInfo，也不参与Region claim。

### 7.8 终止证明

设待完成工作集合为：

```text
W =
  queued tasks
  + in-flight TaskLease
  + local MarkWordBatch candidate
  + HAS_REF pending summary
  + mark & ~scan delta
  + worker-local WeakRef discovery
  + MarkTermination finalizer sources
```

并发阶段中，task 派生工作受父 lease 保护，bulk barrier 未 flush 工作受“覆盖前必须
flush”保护，scalar barrier同步发布。`outstanding==0` 只触发冻结。

STW 后外部 producer 为零，所有 batch 已 flush；重复执行“pending→task→scan→child”
会单调增加 mark/scan bit，有限 slot 集合保证收敛。再依次执行 resurrection closure 与
WeakRef 清理，最终检查 `W=∅` 后发布 Sweep。因此 Sweep 不可能早于仍可产生强闭包的
producer。

## 8. 最终实施顺序

以下任务在同一开发分支连续完成。每个任务可以有单元测试，但只有 Task 1～9 全部完成、
旧结构删除、Runtime/compiler/stdlib 联合重建后，才形成可运行的 Step62 SDK。不得把
WorkBuf/Region 双轨中间状态作为发布版本。

### Task 1：冻结最终64位ABI与联合构建契约

- [ ] Runtime 定义最终 `MSRegionInfo` 64B layout；
- [ ] 原符号`CJ_MRT_MSGCFastPathState`直接扩为64B，增加currentMarkEpoch和三plane base，
      不新增V2符号；
- [ ] 删除 Release `MSRegionInfo::sweepState/sweepGen`、
      `regionGeneration/layoutSignature`语义；定义Central级`sweepCycleId`，明确禁止
      用它判断Region membership或claim；
- [ ] `CJMSGCABI.h` 定义唯一 offset/stride/bit和36项size-class descriptor；
- [ ] 删除 AArch64/X86 emitter私有的RegionInfo offset表，lowering/emitter统一读取
      `CJMSGCABI.h`；
- [ ] Runtime `static_assert` 与 LLVM unit/lit contract 同时失败/同时通过；
- [ ] 写负向测试：缺ScanClass wrapper、混合GCMode、非64位target生成inline MSGC fast
      path必须在编译期被拒绝；
- [ ] 删除`AllocBufferMS::tlRegion_`、`ThreadLocalData::buffer`到MSGC buffer的alias和
      896B padding，固定`activeRegions[sizeClass][scanClass]` base=0/stride=8；
- [ ] 联合SDK流程强制Runtime/compiler/stdlib/应用二进制同版，不实现运行时版本协商。

先锁 ABI 是因为后续 allocator、barrier emitter、plane 定址和 target call size 都依赖它。

### Task 2：实现 ScanClass 全链路

- [ ] 新增唯一`MSScanClass`定义和`ClassifyMSScanClass(TypeInfo*)`；
- [ ] `AllocBufferMS` 改为 `activeRegions[36][2]`；
- [ ] Central 保持36个 bucket，每个 bucket内部三个 tagged stack按 ScanClass分lane；
- [ ] empty Region 保持 classless，可重新绑定；
- [ ] `MObject/MArray`在持有TypeInfo处分类，经
      `HeapManager::AllocateMS -> RegionSpaceMS::Allocate -> SlowAlloc`显式传递；
- [ ] object/array/generic/pinned/finalizer/WeakRef/反射/`RAW_POINTER_OBJECT`全部覆盖3.1
      分类矩阵；
- [ ] `CJRuntimeLowering` 给 static/dynamic wrapper生成同一 ScanClass；
- [ ] AArch64/X86 allocation emitter 使用二维 offset；
- [ ] Debug 纯度扫描与 LLVM lit tests 通过。

### Task 3：实现三平面 arena 与 collect-set

- [ ] `MSBitmapArena`改成alloc/mark/scan三个独立MemMap和独立commit状态；
- [ ] plane 固定 stride 512B，按 Region index 直接定址；
- [ ] SMALL绑定时确保alloc/mark已commit，只有HAS_REF主动commit scan；PreEnum不首次申请
      plane native memory；
- [ ] RegionInfo 删除 bitmap pointer，使用 `collectEpoch/summary/control` 最终布局；
- [ ] 删除 `totalSlotCount/divMagic/MSRegionInfo::sweepState/sweepGen/prevRegionIdx`，
      从size-class descriptor、stack membership和Sweep admission推导；
- [ ] BUMP materialize 保持 `alloc bit 1=free`；
- [ ] 新增GC私有`RegionChain collectSet[36][2]`，只保存head/tail/count并复用
      `centralNextRegionIdx`；
- [ ] PreEnum 建立 SEALED collect-set，不 flush TLS active；
- [ ] 清collect-set mark、HAS_REF scan；ACTIVE只清mark/scan/pending工作区，不读写其
      live alloc plane；
- [ ] empty/cold-free重新绑定时先清有效mark/scan/pending/state再发布；
- [ ] allocation list与 current collect-set彻底隔离；
- [ ] Sweep contract 先按新 alloc 语义通过。

### Task 4：实现统一 Scheduler、Ledger 与 WorkUnit

- [ ] 新增 Region/Object 两个 tagged intrusive ready stack；
- [ ] head固定`atomic<uint64_t>{tag32,idx32}`，RegionInfo两个4B link用atomic builtins；
- [ ] 支持平台static_assert 32/64位原子always-lock-free，Pop CAS失败序为acquire；
- [ ] RegionInfo增加独立 `markTaskNextRegionIdx`，tagged head contract test覆盖ABA；
- [ ] 实现无容量失败的 tagged push + provisional outstanding publication；
- [ ] `outstandingTotal`固定`atomic<uint64_t>`；实现move-only `TaskLease`和
      child-before-parent-complete，active lease析构为fatal；
- [ ] 实现 `RunMarkSlice(caps, wuBudget, wallBudget)` 空 kernel；
- [ ] 新增`resolvedRefs/markWordRMW/scanWords`与`1/8/1`固定整数WorkUnit；
- [ ] Pacer实现3.8/4.6固定Q16 EWMA、runway档位、helper/slice/assist公式；
- [ ] Assist capability 限制为 REGION；
- [ ] ready-stack/ledger/requeue/并发ABA测试通过。

此任务不创建独立 `MSRegionScanQueue`，也不预留 RANGE lane。

### Task 5：实现 DiscoverRef、MarkWordBatch 与 Region owner

- [ ] collect-set 地址/slot 解析按 word 读取 stable alloc；
- [ ] 实现唯一`ResolveOutsideSetObject()`及3.5规定的地址、slot、stateWord、TypeInfo检查；
- [ ] `MarkWordBatch` key 固定为 `(regionIdx, wordIdx)`；
- [ ] flush 使用 fetch-or old word计算 `newBits`；
- [ ] NO_REF winner 直接结束；
- [ ] HAS_REF winner 更新 pending 并 TryPublishRegion；
- [ ] owner 实现 IDLE/QUEUED/OWNED 和 release recheck；
- [ ] owner 只扫描 summary 指向 word 的 `mark & ~scan`；
- [ ] budget requeue 在新 task 发布后 complete 旧 lease；
- [ ] LARGE 路由到 OBJECT；WeakRef进入 worker-local discovery；Finalizer留在STW分类。

### Task 6：改造 Runtime/LLVM 写屏障

- [ ] scalar deletion barrier在Runtime wrapper内完成mark/pending/queue后store；
- [ ] bulk barrier 使用局部 batch，覆盖写前强制 flush；
- [ ] atomic write/swap按5.3的shade-before-CAS循环实现；CAS成功前shade expected，
      MemoryOrder映射和并发writer测试通过；
- [ ] 删除 `AllocBufferMS::activeLog_` 及 fast ABI offset；
- [ ] 删除 barrier 到普通 WorkBuf 的 append/publish；
- [ ] `CJBarrierLowering`继续生成现有Runtime wrapper，并携带MSGC barrier kind；
- [ ] AArch64/X86 emitter只acquire读取phase；IDLE raw store/copy，MARK调用现有wrapper；
- [ ] compiler不新增publish/shade helper，不重复实现RegionInfo/plane分流；
- [ ] slow fallback、unsupported CC/object format、statepoint tests 通过。

### Task 7：替换 Collector 主流程

- [ ] 保留`AllocBufferMS::stackRoots_`作为唯一mutator root staging，增加预分配overflow
      chunk；EnumRoots返回前drain/clear后再由DiscoverRef + batch发布；
- [ ] ConcurrentMark 启动统一 worker kernel；
- [ ] GC main/helper/assist 使用 capability 表；
- [ ] MarkTermination 实现 quiesce、两个ready stack fixed-point drain；全量plane核验仅Debug；
- [ ] Finalizer resurrection直接发布强闭包task；WeakRef local通过`InsertBatch`汇聚到现有
      `WeakRefBuffer`去重后处理；
- [ ] `BaseObject::OnFinalizerCreated`只委托`FinalizerWorkList::RegisterFinalizer`；
      注册后发布和working-root删除前发布都在`listLock`内调用Finalizer-only
      `MarkNewObject`；
- [ ] 普通allocation-time `MarkNewObject`调用全部删除，override只保留Finalizer root语义；
- [ ] IDLE barrier仍在 STW 内恢复。

### Task 8：接入 Sweep、Scavenge 与统计

- [ ] Central 每个 `(sizeClass, scanClass)` lane收敛为
      `partialSwept/fullSwept/unswept` 三个 tagged stack；
- [ ] PreEnum先关闭、quiesce、drain上一轮，再steal partial/full swept head建立GC私有
      collect-set；steal后的Central head直接承接下一轮supply；
- [ ] Sweep 只从 current collect-set发布的unswept stack claim；
- [ ] 删除partial/full unswept拆分、bucket list mutex和
      `MSRegionInfo::sweepGen` claim CAS；
- [ ] 保留并重命名全局`sweepCycleId`，用于P debt lazy reset、stale worker gate和统计；
      发布新ID前先初始化ratio/credit；
- [ ] Sweep done使用`OPEN+active0 -> CLOSED+active0` admission CAS后复查head的协议，
      不用三个独立load直接判完成；
- [ ] 按 `nextFree=~mark&valid` 重建 alloc plane；
- [ ] Debug 检查 `mark` 只落在 old allocated slot；
- [ ] partial/full lists 保持 ScanClass；
- [ ] empty Region 清除 ScanClass后进入 classless cache；
- [ ] classless Empty cache固定为专用锁下复用`centralNextRegionIdx`的单链；删除
      `prevRegionIdx`后仍支持
      take-head、batch drain和idle-segment地址区间摘除；
- [ ] 用`largeNextSupply/largeCollectSet`两个primary-index vector替换单一可变
      `largeRegionList_`；allocation append、PreEnum swap、PostMark live batch append都按
      3.2.7使用唯一`largeLock_`，且不与hot-cache锁嵌套；
- [ ] Large marked flag使用最终 control；SPAN只保存primary base，不进入vector或ready；
- [ ] LARGE `allocPtr`固定为span/internal起址，对象地址固定`allocPtr+HEADER_SIZE`；
      spanCount/logical bytes边界测试覆盖HEADER_SIZE；
- [ ] `largeHotCache`保留按span-count分桶和独立cache锁，dead span Reset后才能发布；
- [ ] ComputeMarkedStats 只统计 collect-set；
- [ ] 恢复`CJ_MS_STW_SWEEP=1`启动读取；启用时保持admission关闭、禁用
      background/lazy/assist并在下一轮PreEnum单线程force-drain；
- [ ] Background/lazy/assist/STW diagnostic Sweep共享同一Pop/Sweep/Push公式；
- [ ] Scavenge 不访问 mark/scan plane。

### Task 9：删除旧结构并联合验证

- [ ] 删除 `globalFull_`、普通 Mark `WorkBufPool`、per-P `gcWork_`；
- [ ] 删除 `activeLog_`、WorkBuf barrier stats/policy；
- [ ] 删除`tlRegion_`兼容占位和MSGC `ThreadLocalData::buffer` alias；
- [ ] 删除 `workersInFlight/activeWorkers` 终止判断；
- [ ] 删除 object-count Assist debt和双口径日志；
- [ ] 删除独立/中间 Region queue、DIRTY、role token；
- [ ] 删除单一可变`largeRegionList_` membership，迁移为next-supply/collect-set两个side vector；
- [ ] 确认源码不存在 Range/Old/Card/Remembered Step62 状态；
- [ ] 构建 Runtime、compiler、stdlib 到同一 MSGC SDK；
- [ ] 替换 dylib 后重新编译所有 `.cj` 测试；
- [ ] 完成正确性、压力、case26 和吞吐回归。

## 9. 源码级改造清单

### 9.1 Runtime

| 文件 | 最终改造 |
|---|---|
| `src/Heap/MAllocator/MSScanClass.h` | 新建唯一 ScanClass/分类契约 |
| `src/Heap/MAllocator/MSRegionInfo.h/.cpp` | 最终 64B layout、epoch/control、plane accessor、正确 Sweep 公式 |
| `src/Heap/MAllocator/MSBitmapArena.h/.cpp` | 三个独立MemMap、独立commit状态、固定512B stride |
| `src/Heap/MAllocator/MSAllocBuffer.h/.cpp` | `activeRegions[36][2]`从offset 0开始；保留`stackRoots_`并增加root overflow；删除tlRegion/activeLog/per-P GCWork/object debt |
| `src/Heap/MAllocator/MSCentralRegionPool.h/.cpp` | 36 bucket内双ScanClass lane、每lane三个tagged stack、GC私有collect-set、classless Empty单链、LARGE two-vector边界与hot cache |
| `src/Heap/MAllocator/MSRegionManager.h/.cpp` | plane commit/shrink、Debug side generation和重用初始化 |
| `src/Heap/MAllocator/MSRegionSpace.h/.cpp` | slow allocation传 ScanClass，large exact-byte pacing |
| `src/ObjectModel/MObject.cpp`、`src/ObjectModel/MArray.inline.h` | 在仍持有TypeInfo/component TypeInfo的位置执行唯一ScanClass分类 |
| `src/HeapManager.h/.cpp` | 新增只在Runtime内部使用的`AllocateMS(..., scanClass)`传递链 |
| `src/Mutator/ThreadLocal.*` | 删除MSGC buffer到旧`buffer`字段的alias，只保留`msAllocBuffer`入口 |
| `src/Heap/MSGCFastPath.h`、`src/Heap/Heap.cpp` | 64B FastPathState、plane base/epoch、phase发布 |
| `src/Heap/MCollector/MSMarkScheduler.h/.cpp` | 新建两个tagged ready head、TaskLedger、TaskLease |
| `src/Heap/MCollector/MSMarkBatch.h` | 新建固定容量 word batch与flush协议 |
| `src/Heap/MCollector/MSWorkUnit.h` | 新建事件计数、1/8/1固定整数成本、饱和计数、Q16 debt换算和full-GC Pacer；不再拆独立Pacer文件 |
| `src/Heap/MCollector/MarkSweepCollector.h/.cpp` | 替换 Discover/WorkBuf/Concurrent/Termination/PostMark 主流程 |
| `src/Heap/MBarrier/MSMarkBarrier.h/.cpp` | scalar direct shade、bulk batch、atomic shade-before-CAS循环、special slow path，删除日志 |
| `src/Mutator/SatbBuffer.h/.cpp` | `WeakRefBuffer`增加单锁`InsertBatch`，供MarkTermination统一去重 |
| `src/Common/BaseObject.cpp` | MSGC Finalizer创建只委托`FinalizerWorkList`，不在锁外发布 |
| `src/Heap/Collector/FinalizerWorkList.cpp` | 注册后和working-root删除前在`listLock`内调用Finalizer-only `MarkNewObject` |
| `src/Heap/MCollector/BackgroundProcessor.*` | 适配collect-set/Sweep发布和`CJ_MS_STW_SWEEP`禁用路径，不合并Mark ledger |
| 三个模块 `CMakeLists.txt` | 加入新编译单元，删除废弃 WorkBuf policy依赖 |

现有文件中的直接替换点：

```text
MarkSweepCollector::DiscoverObject/TryEnqueueGreyObject
  -> DiscoverRef/MarkWordBatch

MarkSweepCollector::RunConcurrentMarkLoop
  -> RunMarkSlice + Scheduler/Pacer

MarkSweepCollector::DrainMarkTerminationToFixedPoint
  -> Quiesce + two-ready-head fixed-point；Debug-only plane verification

MSMarkBarrier::AppendMarkedRefInto/activeLog
  -> DirectShadeSmall/PublishRegionWord

MSRegionInfo::SweepRegion
  -> collect-set checked、alloc=~mark&valid
```

### 9.2 LLVM

| 文件 | 最终改造 |
|---|---|
| `llvm/include/llvm/CodeGen/CJMSGCABI.h` | state/Region/plane/ScanClass/activeRegion所有常量 |
| `llvm/lib/Transforms/Scalar/CJRuntimeLowering.cpp` | allocation ScanClass attribute、动态分类，public slow签名不变 |
| `llvm/lib/CodeGen/CJBarrierLowering.cpp` | 生成现有MSGC Runtime wrapper并标注barrier kind |
| `llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp` | 新 allocation/barrier fast path |
| `llvm/lib/Target/AArch64/AArch64CangjieCallSize.cpp` | 发码长度与 wrapper展开同步 |
| `llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp` | 成功展开/stack map记录契约 |
| `llvm/lib/Target/X86/X86MSGCEmitter.cpp` | 新 allocation/barrier fast path |
| `llvm/lib/Target/X86/X86MCInstLower.cpp` | wrapper识别/ABI lowering必要适配 |

前端 intrinsic 名称和 `Cangjie_GCMode` 定义不改；改的是 MSGC module 下的 wrapper 属性、
data ABI 和 target emission。

### 9.3 删除项

删除前先用 `rg` 确认没有 ABI/test 引用：

```text
AllocBufferMS::activeLog_
EnsureActiveLog / PublishAndSwapLog / PublishExitedLog
globalFull_ / workBufPool_ / gcThreadWork_
PerPGCWork ordinary Mark use
MSWorkBufScalePolicy for Mark
object-count assist ratio/debt
ordinary small GreyWorkQueue path
独立 RegionScanQueue
DIRTY flag和 role-bearing Region token
```

GreyWorkQueue 若仍被 CopyGC 或非 MSGC 公共代码使用，只删除 MSGC 引用，不删除共享实现。

## 10. 测试、指标与验收标准

### 10.1 ABI/LLVM tests

必须更新并保留现有契约：

- `test/MSGCFastPathStateABIContractTest.cpp/.sh`；
- `test/MSGCAllocBufferFastPathABIContractTest.sh`；
- `llvm/test/Transforms/CJRuntimeLowering/new_array_*_msgc.ll`；
- AArch64/X86 `cangjie-msgc-allocation-*.ll`；
- AArch64/X86 `cangjie-msgc-writebarrier-*.ll`；
- AArch64/X86 `cangjie-msgc-writebarrier-statepoint.ll`；
- unsupported CC/target/object-format slow-call tests；
- CopyGC/MSGC file-split 与 GCMode invalid tests。

新增检查：

```text
ScanClass static/dynamic分类
activeRegions[36][2] base=0/stride=8，旧tlRegion/activeLog offset不存在
原CJ_MRT_MSGCFastPathState符号的64B布局及三plane base，不生成V2符号
RegionInfo 64B最终 offset
AArch64/X86-64允许inline，非64位target拒绝inline MSGC fast path
IDLE barrier只做raw store/copy
MARK barrier调用现有Runtime wrapper，不出现新publish/shade helper符号
bulk overwrite前 batch flush
musttail + cj-frame-transparent
成功展开只有一个 CJ stack map
```

### 10.2 Runtime 单元/竞态测试

新增：

| 测试 | 覆盖内容 |
|---|---|
| `MSGCScanClassRegionTest` | 非空纯度、empty重绑定、bucket内双lane |
| `MSGCThreePlaneBitmapTest` | 三个独立MemMap、`alloc:1=free`、坐标/valid mask、NO_REF不主动commit scan、clear生命周期 |
| `MSGCCollectSetEpochTest` | SEALED冻结、ACTIVE/PreEnum后归还排除 |
| `MSGCOutsideSetResolverTest` | exact slot、SPAN primary、stateWord/TypeInfo、未发布对象和Reset gate |
| `MSGCRegionScanStateTest` | IDLE/QUEUED/OWNED合法转换 |
| `MSGCRegionScanRaceTest` | producer/owner release lost-wakeup压力 |
| `MSGCMarkBatchTest` | 同 word合并、newBits winner、flush边界 |
| `MSMarkSchedulerLedgerTest` | provisional publish/claim/requeue/child/complete、move-only lease和active析构fatal |
| `MSMarkTerminationFixedPointTest` | publication不产生orphan pending、STW closure |
| `MSWorkUnitPacerTest` | 1/8/1权重、Q16饱和、EWMA初值、runway/helper/slice/assist固定边界 |
| `MSGCSATBRegionPublishTest` | scalar/bulk old-edge与多atomic writer shade-before-CAS正确性 |
| `MSGCFinalizerSpecialRootTest` | 注册与GC快照竞态、working-root移除、resurrection后WeakRef顺序 |
| `MSGCRootStagingTest` | stackRoots drain、capacity复用、overflow chunk和EnumRoots返回前全空 |
| `MSGCSweepDiagnosticGateTest` | `CJ_MS_STW_SWEEP=1`禁止并发consumer并在下一轮PreEnum串行drain |

应修改而不是机械保留的旧测试：

- `MSRegionInfoBatchMarkTest.cpp` 改为 plane-base word batch；
- `MSGCSweepRegionBitmapContractTest.sh` 固定 `alloc=~mark&valid`；
- `MSGCActiveRegionBitmapClearContractTest.sh` 验证“不 flush但清 visit/scan”；
- WorkBuf/dual-lane/tail-drain contract 改为“旧结构不存在 + typed fixed-point”；
- `MSGCPostMarkSemanticClosureContractTest.sh` 保持 finalizer/WeakRef 顺序。

### 10.3 MarkTermination 硬性断言

Debug 或 `cjMSVerifyMark` 下：

```text
collect-set:
  mark & allocFree == 0
  scan & ~mark == 0
  HAS_REF: allocated & mark & ~scan == 0
  NO_REF: scanWordsTouched == 0

ACTIVE:
  reachable HAS_REF visit & ~scan == 0
  unreachable ACTIVE 不被整区扫描
  Region 未进入 current Sweep

Scheduler:
  outstanding == 0
  queues empty
  all Region state == IDLE
  pendingWordSummary == 0
  all worker batches empty
```

还要验证：

- Root closure 能穿过 ACTIVE HAS_REF 到达 collect-set child；
- ACTIVE 连续多轮保持 active 不会被强制 detach；
- PreEnum 后新对象没有普通 MarkNewObject，也不会被本轮 Sweep；
- mark bit不得落在 collect-set free slot；
- Finalizer resurrection 后才清 WeakRef；
- LARGE/SPAN base、marked flag和重用 generation 正确。

### 10.4 压力矩阵

至少覆盖：

```text
P = 1 / 5 / 50 / 100 / 150
heap = 128 MiB / 2 GiB持久集 / 接近limit
对象 = NO_REF leaves / HAS_REF链/图 / 混合size class
写入 = scalar / struct / ref array / atomic CAS
阶段 = concurrent natural / time pressure / OOM / CJ_MS_STW_SWEEP
worker = GC main only / helpers / assist enabled/disabled
ACTIVE = 长期不归还 / PreEnum后归还 / 新建Region
```

竞态测试循环触发：

- owner `OWNED -> IDLE` 窗口提交最后一个 bit；
- budget requeue 与其他 producer 同时提交；
- queue reserve后 CAS 失败；
- parent complete 前发布最后一个 child；
- natural zero 与 mutator scalar/bulk barrier交错；
- MarkTermination 与 finalizer worker交错。

### 10.5 必须新增的指标

```text
noRefWinners / hasRefWinners
resolvedRefs
markWordRMW
batchCandidates / batchNewBits / batchMergeRatio
regionPublished / regionClaimed / regionRequeued
pendingWords / scanWords / scanBytes
ownerAcquire / ownerReleaseRecheckHit
objectTasks / mainOnlyTasks
outstandingHighWater
completedWU(main/helper/assist)
wuPerUs / assistShare / helperTarget
activeVisitedObjects
collectSetRegions(noRef/hasRef)
```

最终删除的旧指标：

```text
markDebtObjects
markDebtBuffers
WorkBuf published/stolen/full
activeLog entries
regionOutstanding（独立于总账本）
```

### 10.6 性能验收

硬门槛：

1. NO_REF collect-set 路径 `objectHeaderLoads=0、typeInfoLoads=0、scanWords=0`；
2. ordinary small HAS_REF winner `WorkBufPush=0`；
3. 同 word N 个 candidate 的 mark RMW 显著小于 N；
4. 2 GiB 持久 `[]byte` payload 不扫描 payload bytes；
5. Mark 完整性、Finalizer、WeakRef、Sweep/Scavenge 全部通过。

case26 对比同时记录 128 MiB 与 2 GiB：

```text
Mark wall time
marked objects/s
resolved refs/s
mark RMW/s与candidate/RMW
scan bytes/s
GC CPU seconds
mutator throughput
p50/p95/p99 pause
RSS/committed plane bytes
```

性能目标不是让 2 GiB 与 128 MiB 的绝对速率完全相同，而是显著缩小“大持久集导致的
Mark 速率下降”，并且：

- 2 GiB case 的 Mark wall time和 GC CPU均不劣于当前基线；
- `MarkRate(2GiB)/MarkRate(128MiB)` 相对当前实现至少改善 30%；
- 非 GC mutator throughput 回退不超过 3%；
- 默认 p99 STW 不出现 plane全量遍历造成的新线性尖峰；完整扫描只在验证模式执行。

若未达到，按指标区分是 ScanClass未命中、batch合并率低、Region并行度不足、Runtime
barrier wrapper成本过高还是 Pacer估计失真，不回退到双轨中间架构。

### 10.7 SDK 验证纪律

每次报告明确：

```text
compiler 来源
runtime 来源
stdlib 来源
```

Runtime dylib替换后重新用该 SDK 编译 `.cj`；多 SDK 对比使用隔离的 `env -i` 子 shell，
运行前用 `DYLD_PRINT_LIBRARIES=1` 确认 `libcangjie-runtime` 来源。`gc.log` 未生成、
stdout为空或 dyld报错时按启动环境错误处理，不把它当成性能变慢。

## 11. 预期效果、代价与非目标

### 11.1 六项优化的作用

| 优化 | 直接减少 | 预期效果 |
|---|---|---|
| ScanClass Region | winner 的对象头/TypeInfo判断 | 数百万 NO_REF 叶子只做地址/bitmap mark |
| 三平面 | 重复扫描、逐对象合法性 | stable alloc按 word验证，scan记录增量完成 |
| Region scan | 对象 WorkBuf搬运、元数据跳跃 | 同 Region连续扫描，提高 cache/TLB局部性 |
| MarkWordBatch | mark word原子竞争 | 多引用合并一次 RMW，winner批量发布 |
| Scheduler/Ledger | 多套 queue/termination状态 | task生命周期唯一、STW fixed point清晰 |
| WorkUnit Pacer | 对象数计量失真 | helper/assist按真实工作量扩缩 |

### 11.2 依赖关系

```text
ScanClass ───────────────┐
                        ├─> Region scan
alloc/mark/scan planes ──┘        │
                                 ├─> unified Scheduler/Ledger
MarkWordBatch ─> pending publish ─┘             │
                                               └─> WorkUnit Pacer/Assist
```

- Region scan 必须同时依赖 ScanClass 与 scan plane；
- MarkBatch 的 HAS_REF newBits 是 pending/Region task 的 producer；
- Scheduler 把 Region/Object 的生命周期统一后，WorkUnit 才有唯一完成口径；
- Sweep 只依赖 final mark 与 collect-set，不依赖 WorkUnit。

六项一起做没有结构性冲突，前提是以下边界固定。

### 11.3 已消解的冲突

| 潜在冲突 | 最终解法 |
|---|---|
| batch 延迟 mark，终止可能过早 | batch受 TaskLease保护；bulk覆盖前、lease完成前强制flush |
| Region单 owner降低并行度 | 不同Region并行；budget到期requeue；不恢复普通small WorkBuf |
| producer并发写 mark，owner写 scan | mark原子多写者；scan单owner；pending release/acquire连接 |
| ACTIVE alloc正在变化 | GC不读ACTIVE alloc；精确对象验证后使用临时visit/scan |
| 新对象不mark，是否会被回收 | 新对象Region不在current collect-set/Sweep |
| 自然 outstanding=0 与 barrier竞态 | 只请求STW；STW等待producer并最终drain |
| Pacer预算到期与正确性 | 预算只导致requeue/请求STW，不丢任务 |
| alloc bit语义与Sweep公式 | 全局固定 `1=free`，Sweep写 `~mark&valid` |
| Runtime/LLVM layout同时变化 | 单一CJMSGCABI + 联合SDK，禁止混用 |

### 11.4 元数据与运行时代价

- 三个 plane 最坏全部物理提交为 `1536 / 32768 = 4.6875%` Region 容量；
- 相比当前 alloc+mark 两 plane，scan plane 增量为 1.5625%；
- NO_REF scan plane demand-zero，不应主动触页；
- TLS active pointer 从 36 增至 72，每个 64-bit P 增加 288B；
- worker-local batch、WeakRef local容量和Pacer history需要预分配 native memory；
- MARK阶段scalar barrier进入现有Runtime wrapper；Region的pending/state仍合并重复入队；
- 64B RegionInfo 不扩张，但字段 ABI 会整体重排。
- 按既定协议，长期不归还的 TLS ACTIVE Region 可跨多轮排除在 Sweep 外，其中不可达对象
  也会延迟回收；本步骤不通过强制 detach 改变这一取舍。

### 11.5 非目标

本步骤不解决：

- 单个超大引用数组内部并行与抢占；
- 分代/Old/Immutable/remembered set；
- Sweep/Scavenge 算法本身的吞吐重写；
- CopyGC 路径；
- 所有平台一次完成相同 inline fast path；未支持平台正确走 slow wrapper；
- 通过跳过完整对象图获得“近似 Mark”。

大引用数组成为 OBJECT 长尾时，应先用指标确认；只有未来单独重新批准 RangeQueue 后才能
设计，不能在 Step62 实现中暗藏 range cursor。

## 12. 最终流程摘要

```text
LLVM
  Cangjie_GCMode=MSGC
  -> allocation wrapper: sizeClass + ScanClass
  -> write wrapper: deletion SATB
  -> AArch64/X86使用同一FastPath/Region/plane ABI

Allocation
  -> activeRegions[sizeClass][scanClass]
  -> TLS owner执行BUMP或free-bit 1->0
  -> refill时收Mark WU debt
  -> PreEnum后Region不进入本轮回收

PreEnumRoots
  -> stop/wait Sweep
  -> STW推进epoch
  -> 只冻结Central SEALED为collect-set
  -> 不flush TLS ACTIVE，不复制全局alloc snapshot
  -> 清本轮mark/scan/pending并初始化Scheduler/Pacer

EnumRoots
  -> 安装mark barrier并handshake
  -> 枚举stack/global/export roots
  -> DiscoverRef + MarkWordBatch
  -> NO_REF完成；HAS_REF发布REGION

ConcurrentMark
  -> GC main/helper/assist共用RunMarkSlice
  -> Region owner扫描mark & ~scan
  -> child按目标Region批量mark
  -> scalar/bulk barrier在覆盖前shade old edge
  -> TaskLease保持parent/child/outstanding
  -> Pacer按WorkUnit调节helper/assist

MarkTermination STW
  -> quiesce producer/worker
  -> flush batch、acquire读取两个ready head/ledger
  -> Region/Object tasks drain到fixed point
  -> Finalizer分类与resurrection closure
  -> WeakRef清理
  -> 校验plane/state/ledger全闭合
  -> STW内恢复IDLE barrier

PostMarkTermination
  -> finalizer通知
  -> 只对large collect-set执行Large回收
  -> live/WU统计
  -> admission关闭时一次性发布small collect-set
  -> release打开本轮Sweep

Sweep
  -> 只pop本轮unswept；Debug assert current collectEpoch
  -> global sweepCycleId只隔离debt/worker/统计
  -> oldAllocated=~alloc&valid
  -> dead=oldAllocated&~mark
  -> alloc=~mark&valid
  -> empty/partial/full按ScanClass归类

Scavenge
  -> 只消费已空且已Sweep的Region/span
  -> FreeManager归还物理页
  -> 不读取mark/scan plane
```

Code Review 最后只问六个问题：

1. NO_REF winner 是否完全不读对象本体、不发布扫描任务？
2. HAS_REF 新 mark 是否一定落入 pending/QUEUED/OWNED/lease/batch 之一？
3. TaskLease 完成前是否已经 flush batch并发布所有 child？
4. ACTIVE 是否只沿正常 Root/对象图访问，且绝不进入本轮 Sweep？
5. MarkTermination 是否在 resurrection 后、WeakRef 后证明 fixed point？
6. Sweep 是否始终按 `alloc bit 1=free` 和 current collect-set执行？

六个答案全部为“是”，才是本文定义的最终 Step62 架构。

## 13. 方案精简审计与热路径判断清单

### 13.1 本轮源码核对结论

本轮审计以 2026-07-28 当前源码为准，确认以下事实：

- `MSRegionInfo` 当前确实是 64B，同时保存 `allocBitmap*`、`markBitmap*`、
  `sweepState`、`sweepGen` 和 `reserved` summary；
- `MSBitmapArena` 当前为每 Region 预留 1024B，前半 alloc、后半 mark；
- mark bitmap 当前是连续逻辑 word，`MarkBitmapWordResolved()` 直接访问
  `markBitmap[wordIdx]`，不是 8-lane striped；
- `PreEnumRoots()` 当前会先停止 Sweep，再在 STW 中清 Central/TLS active mark bitmap、
  reclassify并推进 Sweep generation；
- 当前 white candidate 在设置 mark bit前必须读取有效 TypeInfo，以防 stale/zeroed slot；
- 当前 WeakRef deferred main queue是实现选择，不是“必须由主线程登记”的 GC 语义；
- 当前 Finalizer 必须在普通闭包稳定后分类，自复活/working root仍需要 phase-gated专用
  shade语义；
- 当前 `SizeClassInfo` 已有全局 `slotDivMagic_[36]`，所以 RegionInfo 中的 `divMagic`
  是重复数据；`totalSlotCount` 同样可由 `MS_REGION_SIZE / slotSize` 推导。
- 当前主力 compiler 的36项 size-class table已集中到 `CJMSGCABI.h`，但
  AArch64/X86 MSGC emitter仍各自硬编码 RegionInfo
  `totalSlotCount/divMagic/bitmap pointer/regionType` offset；最终要把这些offset和派生
  descriptor继续收口到 `CJMSGCABI.h`，不能删Runtime字段后留下target副本。
- 当前所有 small Sweep source 最终都经 bucket lock 下的
  `ClaimUnsweptHeadForSweep()`，list head pop已经给出唯一 owner；其内部
  `sweepGen: sg-2 -> sg-1` CAS是第二套重复 ownership。
- 当前`globalSweepGen_`还被`AllocBufferMS::sweepDebtGen_`、BackgroundProcessor
  helper控制和REPORT统计读取；因此最终只删除Region内generation，把这个全局控制面
  收敛并改名为`sweepCycleId`，不能把跨轮debt/延迟任务隔离一起删掉。
- 当前 `partialUnswept/fullUnswept` 对 Sweep 执行同一公式，差别只反映 Sweep 前的
  空槽状态；在显式 collect-set架构中可以合并为一条 `unswept`。

因此 Step62 不是在当前 WorkBuf 上局部加一个 RegionQueue，而是一次 compiler/runtime
隐藏 ABI、allocator ownership、Mark 调度和 termination 协议的整体替换。

### 13.2 最终删除、合并或降级的结构

| 项目 | 最终处理 | 原因 |
|---|---|---|
| 普通 small `WorkBuf/globalFull_/PerPGCWork` | 删除 | Region mark/scan plane已表达灰工作 |
| `AllocBufferMS::activeLog_`及896B padding ABI | 删除 | scalar直接shade；bulk覆盖前flush batch |
| `MAIN_ONLY` queue/chunk pool | 删除 | WeakRef本地发现；Finalizer只在STW分类 |
| Release `externalVisitedRegions` | 删除 | OUTSIDE_SET复用state/pending/ledger |
| Release `regionGeneration` | 删除 | 生命周期由phase/ledger保证；Debug放side table |
| `MSSweepState` 与 `MSRegionInfo::sweepGen` | 删除 | unswept pop唯一claim；Sweep admission跟踪in-flight；结果stack表达完成 |
| `globalSweepGen_` | 保留语义并改名为`sweepCycleId` | 只做P debt lazy reset、stale worker gate和统计归属；不进入Region membership/claim |
| `partialUnswept/fullUnswept` | 合并为 `unswept` | Sweep前是否有空槽不影响同一回收公式 |
| bucket Region-list mutex | 删除 | 三条list改为ABA-safe tagged stack；只做head push/pop/steal |
| `prevRegionIdx` | 删除 | 最终Central/collect-set都不做双向list或任意节点删除 |
| 可变 `regionType` 生命周期 | 删除 | stable `regionKind` + owner-only `allocMode` + stack membership足够 |
| Region内 `totalSlotCount/divMagic` | 删除 | 由只读 `MSSizeClassDesc[36]` 推导并在task入口缓存 |
| `layoutSignature`热字段 | 改为reserved | contract test比每次barrier读取更合适 |
| LARGE `spanCount` | 删除 | 从`regionEnd-allocPtr`推导 |
| LARGE base的自身`baseRegionIdx` | 删除 | 只有SPAN follower需要base index |
| Queue ring slot/sequence/`Reserve/Cancel` | 删除 | RegionInfo永久node + tagged head没有容量失败 |
| OBJECT的`BaseObject* + kind/flags` payload | 删除 | OBJECT只表示LARGE primary，全部可由regionIdx推导 |
| Release全量pending repair | 删除 | publication必须无orphan；全量差集只做Debug |
| `scanBytes` pacing权重 | 降为统计 | payload bytes不是实际ref扫描成本 |
| 多权重在线Pacer/expected-cycle模型 | 延后 | 首版用可校准的两个EWMA闭环 |

`AllocBufferMS` 虽然从36个active pointer增加到72个，但删除 `activeLog_` 和为其保留的
896B固定前缀后，不需要继续保留旧padding。LLVM只固化实际使用的active pointer offset，
Runtime-only字段紧随二维数组，避免为了已删除ABI长期浪费TLS空间。

### 13.3 判断应放在哪一级

| 判断 | Release位置 | 处理结论 |
|---|---|---|
| `barrierPhase` | barrier入口 | 必须保留；只能在无safepoint基本块内证明后上提 |
| null/heap range | 每个外部candidate一次 | 必须保留；进入known-managed内部API后不重复 |
| stable regionKind small/LARGE/SPAN | candidate解析一次 | 必须保留；batch/owner内缓存，非空期不再变化 |
| `collectEpoch == current` | batch entry或scalar candidate一次 | 必须保留；决定能否读stable alloc |
| slot base/对齐/Region边界 | candidate解析一次 | 必须保留；防interior/stale ref |
| collect-set `allocatedMask` | batch flush每word一次 | 必须保留，但从逐对象上提到word |
| collect-set对象stateWord合法性 | producer不读 | 删除；stable alloc plane已给出slot存在性 |
| OUTSIDE_SET白对象stateWord/TypeInfo | mark前每个white slot一次 | 必须保留；alloc正在变化 |
| OUTSIDE_SET已mark对象TypeInfo | producer不再读 | 删除；首次winner已完成验证 |
| mark bit预探测 | candidate入口和batch flush各一次relaxed load | 保留为重复工作过滤；不能替代最终atomic winner |
| mark原子RMW | batch flush每word一次 | 必须保留；并发唯一winner |
| `HasRefField()` | producer/owner不逐对象判断 | 删除；由Region ScanClass保证 |
| TypeInfo/GCTib读取 | HAS_REF owner每对象一次 | 必须保留；用于真实字段枚举 |
| WeakRef判断 | owner取得TypeInfo后一次 | 必须保留；referent不是强边 |
| array element size/component layout | 每对象/数组一次 | 从element循环外提 |
| valid tail mask | 每Region最后word | 预计算并缓存，普通word不分支 |
| task role/collectEpoch重复检查 | producer决定一次，task claim不做 | Release删除，Debug assert |
| Sweep Region的`collectEpoch` | PostMark发布unswept时一次 | consumer Release删除，Debug assert |
| Sweep worker的`sweepCycleId` | `EnterSweep`前一次 | 必须保留；拒绝延迟到下一轮才执行的旧任务，且不逐Region重读 |
| ready empty与outstanding重复轮询 | helper扩容只看ledger；自然/STW终止检查head | 不维护ready count，不重复计算backlog |
| scan word atomic RMW | 不使用 | 单Region owner普通store |
| `mark & ~scan`全Region扫描 | 默认STW不做 | Debug/cjMSVerifyMark才做 |

特别注意：删除 collect-set stateWord 检查的前提不是“相信任意引用”，而是同时满足：

```text
Region属于current collect-set
  && Region在PreEnum前已经SEALED
  && alloc plane在整个Mark周期只读
  && candidate通过slot base/对齐/边界
  && alloc bit证明该slot在快照时已分配
```

缺少其中任意一个条件，都必须回到 OUTSIDE_SET slow validation；不能把这项优化扩大到
正在分配或PreEnum后归还的Region。

### 13.4 还能继续做、但不增加新语义的优化

下列第一优先级已经随主体或第14章落地：

1. Region owner claim后一次缓存 `regionStart/slotSize/totalSlots/validTailMask/scanClass/role`
   和三个plane base，word/object循环内不重复解码RegionInfo；
2. owner每个对象只读一次 TypeInfo，并把已知TypeInfo传给scanner；WeakRef判断、
   GCTib取得和数组component/element size复用该值；
3. MarkWordBatch固定16个entry，使用8组×2路查找；entry保持16B自然对齐；
4. collect-set flush按entry聚合后只执行一次alloc word load、一次mark RMW和最多一次
   pending OR；
5. `validMask/bitmapWords/slot reciprocal`按size class预计算，owner不在循环内做除法；
6. WeakRef local buffer复用上轮high-water；普通append无锁，overflow才走现有锁保护
   fallback；
7. 删除activeLog后同步删除旧896B padding、WorkBufPool refill/publish统计和ABI tests，
   不保留“以后也许会用”的兼容空洞；
8. empty/cold-free Region重新绑定时集中清理有效mark/scan/pending/state，不能把清理
   分散回每次对象分配；
9. `collectEpoch` wrap只在极低频epoch回绕时做一次STW全重置，不在普通热路径增加wrap判断。

第二优先级必须先profile再决定：

- RegionReady tagged head出现明显单cache-line竞争时，可做固定分片head加non-empty bitmap；仍共享
  一个TaskLedger，不改变token/state语义；
- owner对象头延迟明显时，可对candidate bit序列预取下一个对象的stateWord/TypeInfo；
- Region并行度长期不足时，先调整Region task slice和helper启动阈值，不直接引入同Region
  多owner或原子scan plane；
- PreEnum清plane成为瓶颈时，先按collect-set/ScanClass精确触页和批量memset优化，不先上
  双mark-plane或per-word epoch；
- MARK阶段Runtime barrier wrapper成本高时，先profile对象解析、batch flush和
  pending/state合并；只有证明确属调用边界成本后，才评估在保持无safepoint发布协议的
  前提下扩大compiler inline范围。

### 13.5 明确不做的“伪精简”

以下删除会破坏正确性或把复杂度转移到更差的位置：

- 不能删除 OUTSIDE_SET white slot的TypeInfo合法性检查；
- 不能用非原子mark store替代producer的fetch-or；
- 不能在scan bit设置后、child batch flush前complete TaskLease；
- 不能因为queue暂时为空就忽略in-flight outstanding；
- 不能把WeakRef referent当普通HAS_REF字段扫描；
- 不能让assist领取不可切分LARGE OBJECT；
- 不能为了去掉collectEpoch而在barrier中遍历Central/TLS ownership；
- 不能为了避免scan plane而每次重新扫描整Region；
- 不能在默认MarkTermination中靠全堆repair掩盖publication bug；
- 不能让`layoutSignature`热检查替代compiler/runtime联合ABI测试。

### 13.6 字段级访问矩阵：普通访问、relaxed、release/acquire

实现时按下表选访问方式。这里的“普通”是 C++ 普通 load/store，只能用于已经由 owner、
STW 或 publication生命周期证明没有并发写的数据；不能把“无 mutex”泛化成“所有字段
都非原子”。

| 数据 | 写者/读者 | 最终访问方式 | 同步依据 |
|---|---|---|---|
| TLS ACTIVE `allocPtr/allocMode/freeSlotCount/allocSummary` | 单个 AllocBuffer owner | 普通读写 | 其他线程不读取 ACTIVE allocation state |
| ACTIVE alloc plane | 单个 AllocBuffer owner | 普通读写 | Region 独占；GC 对 OUTSIDE_SET 不读 alloc |
| collect-set alloc plane | PreEnum 前已物化；Mark只读 | owner/producer普通读 | collect-set隔离后无写者，phase发布建立生命周期 |
| `regionEnd/sizeClass/layout(regionKind+scanClass)` | 绑定者写；非空期只读 | 普通读写 | 初始化先于Region发布，非空期不再改变 |
| `collectEpoch` | PreEnum STW写；本轮producer读 | 普通写、phase acquire后普通读 | `barrierPhase` 是该组字段的统一发布门 |
| `centralNextRegionIdx` | Central stack producer/consumer | relaxed atomic store/load | 防止node快速pop/reuse/re-push时旧pop与next重写形成C++ data race |
| `markTaskNextRegionIdx` | ready producer/consumer | relaxed atomic store/load | scan state防重复token；atomic link仍负责node快速复用的数据竞争 |
| mark plane small word | 多producer、owner、最终Sweep | relaxed atomic load/fetch-or | 只需要bit modification order；pending承担可见性传递 |
| `control.LARGE_MARKED` | 多producer、PostMark | relaxed atomic fetch-or/load | winner唯一性；最终STW/phase quiesce保证收口 |
| `pendingWordSummary` | 多producer、单Region owner | producer `fetch_or(release)`；owner `exchange/load(acquire)` | owner看到summary时必须看到此前mark OR |
| scan plane | 单Region owner写 | 普通load/store | owner唯一；handoff由state/ready/ledger发布 |
| scan state初始探测 | producer | relaxed load | 只是CAS fast reject；陈旧IDLE也会在CAS处失败 |
| `IDLE -> QUEUED` | producer | CAS成功 acquire、失败 relaxed | 接力旧owner的`OWNED -> IDLE(release)` |
| `QUEUED -> OWNED` | 唯一ready consumer | atomic relaxed store；Debug CAS/assert | ready pop已acquire，producer只观察不竞争该转换 |
| `OWNED -> QUEUED` | 当前owner requeue | atomic relaxed store + ready release push | 后续push统一发布scan/node写入 |
| `OWNED -> IDLE` | 当前owner | release store | 下一次成功的IDLE->QUEUED acquire接力 |
| Region/Object ready head | 多producer/consumer | tagged CAS；push release、pop acquire | node payload和此前owner写通过head发布；tag防ABA |
| `outstandingTotal` provisional add | task producer | relaxed fetch_add | 只建立计数，不发布payload |
| provisional CAS失败 rollback | 同一producer | relaxed fetch_sub | 没有工作需要发布 |
| TaskLease complete | task owner | release fetch_sub | 先发布scan、child task和batch flush |
| termination zero观察 | GC main | acquire load | 获取所有归零前TaskLease发布 |
| MarkWordBatch/WorkUnit/WeakRef local | 当前worker | 普通读写 | worker-local；quiesce/join后GC main汇总 |
| helper target/assist ratio/统计近似值 | GC main写、worker读 | relaxed atomic | 只是调度hint，不发布GC对象状态 |
| Region/Object ready近似count | 无 | 不存在 | 已删除；TaskLedger覆盖任务量，head只判断empty |
| `barrierPhase` | GC main/所有mutator | release store、acquire load | 统一发布epoch、heap范围、plane base和MarkContext |
| FastPath其余字段 | GC main写、barrier读 | phase release前普通写；phase acquire后普通读 | 不为同一批稳定配置逐字段再做acquire |
| Sweep alloc plane/free count/summary | 唯一Sweep owner | 普通写 | unswept acquire pop取得owner；swept release push发布结果 |
| Sweep读取mark word | marking已quiesce后唯一Sweep owner | relaxed atomic load | 与mark plane统一原子访问类型；此时已无并发producer |
| `sweepCycleId`与ratio/credit配置 | PreEnum初始化、P/helper读 | 配置普通/relaxed写，最后cycleId release发布；读方ID变化时acquire | 只发布全局控制面，不发布Region membership |
| packed Sweep admission | PostMark/worker/PreEnum | Enter acq_rel；Leave release；close/done CAS acq_rel | 同一原子字同时封闭新owner与统计active owner，避免分离检查窗口 |

tagged stack 的 node链接不能复用同一个字段：OUTSIDE_SET Region 可能仍在 Central swept
stack，同时进入 Mark ready stack，所以必须保留 `centralNextRegionIdx` 和
`markTaskNextRegionIdx` 两个单链字段。相反，`prevRegionIdx` 没有任何最终使用场景。

ready head 的最小实现是：

```text
Push:
  old = head.load(relaxed)
  node.next.store(index(old), relaxed)
  CAS(head, old, {idx, tag+1}, release, relaxed)

Pop:
  old = head.load(acquire)
  next = node(index(old)).next.load(relaxed)
  CAS(head, old, {next, tag+1}, acquire, acquire)
```

Pop 的失败序也必须是 acquire：CAS失败会把另一个 producer发布的新 head写回 `old`，
下一轮在读取该 node的 `next` 前必须取得它的发布。next本身仍要用 relaxed atomic：
tag只阻止ABA后的错误head CAS，不能阻止旧pop与node快速复用时并发访问next。现有
`LockFreeRegionStack` 可以抽成公共模板复用，但需要先修正/验证 Pop 的这条内存序契约，
也要把普通`nextRegionIdx`访问改成atomic link；不能直接复制当前版本。

32 位 tag只在全局 quiescence下重置：Mark ready head在每轮初始化前重置，Central
partial/full/unswept head在 PreEnum关闭Sweep并STW steal后重置。Debug 对单个quiescence
区间内的tag回绕做fatal assert，不能在仍可能存在旧pop时静默清零tag。

### 13.7 可以删除和必须保留的锁

最终普通 Mark 热路径没有 mutex：

```text
allocation owner       -> 普通读写
mark winner            -> relaxed RMW
mark -> owner handoff  -> pending release/acquire
task publication       -> tagged head release/acquire
Region scan            -> owner普通写
termination            -> TaskLedger release/acquire
```

可以删除或移出热路径：

| 当前/潜在锁 | 最终处理 |
|---|---|
| Region/Object queue mutex | 不存在；两个tagged intrusive ready head |
| size-class bucket Region-list mutex | 删除；partial/full/unswept均为tagged stack |
| Sweep claim锁内per-Region generation CAS | 删除；unswept pop本身给出唯一owner |
| `msAllocBufferLock_` 每次遍历持锁 | PreEnum/Termination只锁一次复制pointer snapshot，随后无锁遍历 |
| WeakRef每对象全局锁 | worker-local append；只在local容量失败时走锁保护overflow |
| WorkBufPool/globalFull mutex | 随普通Mark WorkBuf整体删除 |
| Pacer/WorkUnit统计锁 | worker-local累加，quiesce后合并；在线hint用relaxed atomic |

仍必须保留：

| 锁/同步 | 保留原因 |
|---|---|
| alloc-buffer registry注册/注销锁 | 线程创建/退出会改变容器；只从热扫描移到一次snapshot |
| classless empty-cache锁 | Scavenge/segment shrink需要按地址范围摘除任意entry；不是Mark热路径 |
| large live registry/hot-cache锁 | large分配/回收会并发修改变长容器，且不是small Mark热路径 |
| WeakRef overflow锁 | 固定local容量不能丢对象；该锁只作极少数兜底 |
| Finalizer表/list锁与GC gate | native background finalizer生命周期独立于普通Mark owner |
| FreeManager shard锁和grow锁 | alloc/scavenge会并发修改多word segment bitmap和拓扑 |
| worker park的mutex/condition_variable | 只用于无工作睡眠和quiesce，不进入每task push/pop |

所有诊断遍历遵守同一规则：要么在 STW/worker join后普通读取 owner-only数据，要么读取
单独发布的原子快照。不能为了让 REPORT 在线读取 `freeSlotCount`、scan plane或worker
local counter，就把这些热字段全部改成原子。

### 13.8 实现时还能删掉的重复分支

按最终数据结构，以下分支不进入 Release 热循环：

1. allocation fast path只判断 `allocMode=BUMP/BITMAP`，不再判断
   `regionType=BUMP/BITMAP/PARTIAL/FULL`；
2. collect-set producer解析一次 stable `regionKind/scanClass/collectEpoch`，MarkBatch
   flush不再重读；
3. owner从size-class descriptor一次取得
   `slotSize/totalSlots/bitmapWords/validTailMask/divMagic`，每word不再做范围推导；
4. owner从bitmap bit构造对象地址，不再执行slot对齐、Region边界、allocMode和
   `IsValidObject()`；这些是producer/collect-set alloc mask的职责；
5. owner不再调用 `HasRefField()`；HAS_REF Region纯度已经保证，只保留一次TypeInfo读取、
   WeakRef/array布局分派和真实ref枚举；
6. OBJECT handler不再判断objectKind/flags/role，只由LARGE primary `regionIdx`推导；
7. Sweep pop后不再检查`partial/full unswept`、`sweepState`、
   `MSRegionInfo::sweepGen`、Region CAS结果和Release `collectEpoch`；只在Debug
   assert发布契约；全局`sweepCycleId`只在`EnterSweep`前检查一次；
8. Central exact list count不参与完成判断；Release Pacer只读relaxed近似值，STW Debug
   才遍历stack重算；
9. helper/assist不维护 `workersInFlight/assistsInFlight` 作为Mark终止真值；
   park/quiesce只使用worker gate，任务闭包只使用`outstandingTotal`；
10. `currentMarkEpoch` wrap检查只在推进epoch的STW慢路径执行，不在barrier/owner中判断。

实现完成后的最小Release状态是：

```text
per Region:
  centralNext + markTaskNext
  + collectEpoch + allocSummary + pendingWordSummary
  + scanState/LARGE_MARKED + freeSlotCount/allocMode
  + sizeClass/stable regionKind/scanClass

global:
  RegionReadyHead + ObjectReadyHead + outstandingTotal
  + currentMarkEpoch + plane bases
  + SweepContext{sweepCycleId, admission, done, ratio/credit, Central heads}

per worker:
  MarkWordBatch + WorkUnit counters + WeakRef discovery
```

除此之外的role token、generation token、active-worker终止计数、MainOnly task、
Release visited list和全量repair状态都不进入最终实现。

### 13.9 实现前最终冻结决策

下表是代码落地时的最终选择；实现者不得再从旧Step或“等价方案”中任选：

| 事项 | 冻结决策 |
|---|---|
| ABI升级 | 原`CJ_MRT_MSGCFastPathState`符号原地扩为64B；helper不加V2后缀；不做运行时版本协商 |
| 组合兼容 | compiler/Runtime/stdlib/应用二进制必须同版联合构建；禁止任意新旧混用 |
| 平台 | 只支持AArch64与X86-64；不实现32位布局或原子fallback |
| AllocBuffer ABI | 删除`tlRegion_`、`activeLog_`及896B padding；`activeRegions[36][2]`从offset 0开始 |
| ScanClass | Runtime唯一`ClassifyMSScanClass(TypeInfo*)`；LLVM按同一矩阵对拍；内部allocation显式传递 |
| Bitmap arena | alloc/mark/scan三个独立MemMap；NO_REF不主动commit或触碰scan |
| OUTSIDE_SET | 统一`ResolveOutsideSetObject()`；不读live alloc plane；首次visit前完成精确对象验证 |
| Root staging | 保留每buffer `stackRoots_`，增加专用RootOverflowChunk；EnumRoots返回前全部drain |
| ordinary Mark work | 删除MSGC WorkBuf/globalFull/PerPGCWork/activeLog；只保留Region/Object ready |
| Mark batch | 固定16个word entry，8组×2路；key只有`(regionIdx,wordIdx)`；缓存epoch/plane base；按规定边界强制flush |
| Scheduler | 两个`atomic<uint64_t>` tagged head；RegionInfo 4B atomic link；一个`atomic<uint64_t>` ledger |
| TaskLease | move-only；child/replacement先发布；active lease析构fatal |
| Atomic barrier | AtomicWrite/Swap使用shade-before-CAS循环；CAS失败允许保守多shade |
| Finalizer | 保留现有`MarkNewObject`虚入口但限定为锁内Finalizer root发布；普通分配不调用 |
| WeakRef | worker-local发现；overflow及MarkTermination batch汇入现有`WeakRefBuffer`统一去重 |
| LARGE边界 | `largeLock_`保护next-supply append/swap/live batch append；collect-set为GC私有vector |
| LARGE地址 | `allocPtr`是span/internal起址；公开对象地址为`allocPtr+HEADER_SIZE`；spanCount由span边界推导 |
| large hot cache | 独立lock、精确span-count map；不做best-fit/split/coalesce |
| Empty cache | 专用mutex下复用`centralNextRegionIdx`的单链；范围摘除冷路径O(n) |
| collect-set移交 | PreEnum steal后线性初始化并建立准确tail/count；PostMark在admission关闭时O(1) adopt |
| Sweep诊断 | 恢复`CJ_MS_STW_SWEEP=1`；保持admission关闭并在下一轮PreEnum由GC线程串行drain |
| WorkUnit | `resolvedRef/RMW/scanWord = 1/8/1`，全部使用饱和整数 |
| Pacer | Q16 alpha=1/8；固定runway档位、helper target、slice预算、512WU assist threshold |

只有修改本表时才允许改变上述选择；普通实现提交只能实现和验证这些冻结契约，不能重新
引入兼容分支或替代容器。

## 14. Region 模式落地后的性能优化与验证（2026-07-29）

### 14.1 优化边界与最终结论

本节记录Step62主体落地后，针对case27回归继续实施的热路径优化。这里所有改动都复用
现有Region/Object ready、TaskLedger、MarkWordBatch和WorkUnit结构，不新增任务队列。

本轮冻结以下边界：

- 普通small灰工作仍只由mark/scan plane、`pendingWordSummary`和Region owner表达；
- LARGE HAS_REF仍使用既有ObjectReady，不增加RangeQueue或对象队列；
- `TaskLedger.outstanding`仍是任务生命周期唯一正确性账本；
- producer的mark更新仍由原子`fetch_or`裁决唯一winner；
- Region scan plane仍只有当前owner写；
- Assist仍只领取REGION任务，返回前必须flush batch并完成或重新发布lease；
- Release可以删除已经由phase/owner证明的重复检查，但Debug断言和STW终止断言保留。

最终加入的六组优化如下：

| 优化 | 主要减少的成本 | 是否增加容器 |
|---|---|---|
| MarkWordBatch定址与查找 | entry线性搜索、arena重复定址、Release检查分支 | 否 |
| 双层mark预检查 | 已标记对象重复进入batch和无效原子RMW | 否 |
| Region owner热路径 | RegionInfo重复读取、逐对象scan word写回 | 否 |
| Scheduler原子与唤醒 | 重复ledger加减、ready计数、无效广播 | 否 |
| WorkUnit/worker策略 | 局部饱和分支、backlog重复计算、helper不足 | 否 |
| 固定并发Mark runway | 小存活集下GC启动过密和Assist反馈放大 | 否 |

case27 60秒最终结果为903.46 req/s、GC平均18.16ms、54382/54382请求成功。相对
2026-07-29 11:12的初始Region版本，吞吐提升约413%，GC平均耗时下降约78%。

### 14.2 优化点一：MarkWordBatch定址与查找

#### 修改前的问题

batch容量增大后，如果每次candidate都从第0项开始线性查找，命中和miss都会比较多个
entry。Region/word坐标连续时，还容易让前几项成为固定热点。每个entry flush再经全局
arena对象取得alloc/mark plane，也会重复执行同一个epoch内不会变化的定址。

#### 最终实现

`MSMarkWordBatch`固定为：

```text
kSetCount = 8
kWays = 2
kCapacity = 16
entry = { uint32 regionIdx, uint8 wordIdx, uint64 mask }  // 16B
```

`HashSet(regionIdx, wordIdx)`选组，每次最多检查两路。命中时直接OR mask；有空路时填入；
两路都占用时按该组`nextVictimWay`选择victim，先flush再复用。该结构不分配内存、不加锁，
也没有全局共享状态。

batch首次使用时缓存：

- 当前`markEpoch`；
- alloc plane base；
- mark plane base。

后续定址直接使用`planeBase + regionIdx * kPlaneStrideBytes`。entry仍不保存ScanClass、
collect-set role或RegionInfo指针，避免Region重用后留下悬空派生状态。

Release热路径不再执行每次Add/flush的坐标、kind、word-count和TypeInfo一致性断言；这些
检查只在Debug保留。batch析构必须为空、跨epoch使用等生命周期错误仍是硬失败，不能降级。

#### 作用

- candidate查找从最多扫描整个batch缩短为固定两路；
- plane地址从“每entry重新取得arena”变为一次缓存后直接计算；
- 保留16个word的合并能力，但不把batch扩大成高冲突的大哈希表；
- 不改变flush边界、winner语义或TaskLease生命周期。

### 14.3 优化点二：重复mark过滤与批量RMW

mark bit在一个GC epoch中单调从0变1。基于这个不变量，最终实现增加两个relaxed load
过滤点：

```text
AddRefToMarkBatch:
  resolve Region/slot/word/bit
  if markWord & bit != 0:
      return
  validate OUTSIDE_SET white object if needed
  batch.Add(regionIdx, wordIdx, bit)

FlushMarkWordEntry:
  mask &= collect-set allocatedMask/validMask
  mask &= ~load(markWord, relaxed)
  if mask == 0:
      return
  old = fetch_or(markWord, mask, relaxed)
  newBits = mask & ~old
```

第一层避免共享对象、环和多父节点引用把同一slot反复放入局部batch。第二层处理“进入
batch时是白色，但等待flush期间已被其他worker标记”的情况，避免一次注定没有newBits的
写型RMW。

这两个load不能替代`fetch_or`。如果预检查读到0，另一个worker可以随后同时标记；
`fetch_or`返回的old值仍是唯一winner依据。只有预检查直接读到1时才能返回，因为首次
winner已经完成以下义务：

- NO_REF：mark bit本身已经表达全部工作；
- HAS_REF：首次winner已经发布pending word，并由QUEUED/OWNED/lease之一保护扫描；
- OUTSIDE_SET：首次从0到1之前已经完成精确对象合法性验证。

该优化没有缓存“对象已灰”状态，也没有额外visited set。所有去重都来自现有单调mark
plane和最终原子winner。

### 14.4 优化点三：Region owner扫描热路径

`RunRegionTask()`取得owner后，在进入summary循环前一次缓存：

| 缓存值 | 用途 |
|---|---|
| `currentEpoch/inCollectSet` | 决定是否允许读取stable alloc plane |
| `bitmapWordCount/validTailMask` | 处理最后一个word，不逐slot推导边界 |
| `regionIdx/regionStart/slotSize` | 直接计算对象地址 |
| `markBitmap/scanBitmap/allocBitmap` | 避免对象循环重复访问RegionInfo与arena |

每个pending word的执行顺序为：

```text
markWord = atomic_load(markBitmap[wordIdx], relaxed)
oldScanWord = scanBitmap[wordIdx]
scanWord = oldScanWord
candidates = markWord & ~scanWord
if collect-set:
    candidates &= ~allocWord & validMask

for each candidate bit:
    obj = regionStart + slotIdx * slotSize
    typeInfo = obj->GetTypeInfo()             // 每对象一次
    ForEachRefFieldGCKnownType(obj, typeInfo)
    scanWord |= bit

if scanWord != oldScanWord:
    scanBitmap[wordIdx] = scanWord             // 每word最多一次普通store
```

修改前逐对象更新scan bitmap会对同一个word反复执行load/OR/store。现在owner在寄存器中的
`scanWord`完成整个word的累积，只在发生变化时写回一次。scan plane仍是单owner普通store，
不增加CAS。TypeInfo只读取一次并传给known-type scanner；Release不重复检查null和
ScanClass一致性，Debug仍验证元数据纯度。

预算在完整word/完整对象边界检查。若summary中剩余word未处理，先release OR回
`pendingWordSummary`；随后flush child batch、flush WeakRef local batch，并按既有协议
requeue。不会为了批量写scan而跨TaskLease保存局部状态。

### 14.5 优化点四：Scheduler原子操作与唤醒

#### TryPublishRegion快速拒绝

高并发下，多条边经常命中一个已经QUEUED或OWNED的Region。旧路径先
`ledger.AddProvisional()`，再由scan-state CAS失败回滚，相当于每次重复发现都对全局
ledger执行一次加一和减一。

最终路径先执行：

```text
if scanState != IDLE:
    return false
ledger.AddProvisional()
CAS(IDLE -> QUEUED)
...
```

pending bit已经在调用前release发布。若读到QUEUED/OWNED，该Region现有lease会消费
pending；若owner正并发释放，则producer会观察到IDLE并发布replacement，或者owner在
OWNED->IDLE后的复查观察到pending并重新发布。原有lost-wakeup双边复查不变，因此快速
拒绝只删除常见失败路径的ledger原子，不改变正确性。

IDLE->QUEUED成功后如果pending已经被旧owner消费，仍执行原有“撤销QUEUED、二次检查
pending、决定回滚或重试”的协议，不能删除。

#### 删除ready计数

`RegionReady/ObjectReady`不再维护诊断`count_`。tagged head已经能回答终止时是否为空，
而queued任务又全部计入TaskLedger；共享count会让每次push/pop多触碰一条cache line，
helper计算若同时使用count和outstanding还会重复计算同一任务。

因此：

- helper target只读取outstanding；
- 自然终止检查`outstanding/workersInFlight/assistsInFlight`和两个head是否为空；
- STW终止用`OutstandingForTermination()`逐task drain，再断言head为空；
- REPORT需要任务量时使用每slice完成数，不恢复全局ready count。

#### 只在empty到non-empty时唤醒

`MSMarkReadyStack::Push()`返回push前head是否为空。Region/Object发布和Region requeue
只有在返回true时才调用`markWorkEpoch.Notify()`：

```text
oldHead == EMPTY  -> push -> notify
oldHead != EMPTY  -> push -> 不notify
```

已有可领取任务时，helper本来就不会因缺少这一轮广播而睡过工作；empty到non-empty才是
必须建立进度事件的边界。这样避免每个task发布都增加epoch原子并广播等待者。

### 14.6 优化点五：WorkUnit、Assist与Mark worker

#### WorkUnit局部计数

`MSWorkUnitCounter`属于一个task/slice，不会接近`uint64_t`上限。其`Total()`改为直接：

```text
resolvedRefs * 1 + markWordRMW * 8 + scanWords * 1
```

不在每次预算检查中调用三次饱和加法。跨task、跨worker和全周期汇总仍使用饱和helper，
所以只删除局部不可能触发的溢出分支。

#### helper只按outstanding扩容

最终helper target为：

```text
base = ceil(outstanding / 8)
NORMAL   -> base
HIGH     -> max(base, ceil(maxHelpers / 2))
CRITICAL -> maxHelpers
target   -> clamp(target, 1, min(maxHelpers, outstanding))
```

不再读取ready近似数。outstanding同时覆盖queued和leased任务；ready只代表其中可立即领取
的子集，叠加会重复放大压力，并制造额外原子成本。

#### Assist必须保留

普通压力下Assist比例继续使用`workDensity / 4`。case27中完全关闭Assist的20秒诊断结果
只有145.18 req/s，GC平均179.48ms；原因是分配线程持续扩展对象图，而background owner
追赶不足，最终形成更大的并发闭包与MarkTermination尾部。因而本轮只精简Assist计量和
任务获取，不把Assist关闭或降为接近零。

#### 默认Mark线程数

显式`cjGCThreads`仍优先。未配置时按可用逻辑CPU计算：

```text
available < 8: max(1, available / 2)
available >= 8:
    clamp(ceil(available / 4), min=6, max=16)
```

8核机器默认总Mark线程数为6，即GC main加5个helper，保留2个逻辑CPU给mutator和I/O。
在同一版Region热路径、尚未加入固定runway的短测中：

| 总Mark线程 | 吞吐(req/s) | GC平均(ms) | GC P95(ms) |
|---:|---:|---:|---:|
| 5 | 314.47 | 28.86 | 47.61 |
| 6 | 351.76 | 23.91 | 39.17 |
| 7 | 336.57 | 24.15 | 38.97 |

6线程取得最高吞吐且P95接近7线程，因此作为8核默认值；不是把helper固定为CPU数。

### 14.7 优化点六：小存活集的并发Mark runway

#### 问题

原阈值只使用：

```text
rawTarget = medianStableLive * (1 + cjHeapGrowth)
```

case27稳定存活集只有约20～30MiB。即使倍率语义正确，绝对可分配余量仍很小，GC会在上一轮
固定成本刚摊销后再次启动；allocation Assist、并发对象图增长和频繁PreEnum进一步形成
反馈。单纯把默认`cjHeapGrowth`提高到10或15虽然能改善case27，却会让2GiB持久存活集的
目标不必要地接近最大堆，不适合作为全局默认。

#### 最终策略

保留用户配置倍率，同时给并发Region Mark增加固定加法余量：

```text
minimumRunway = 5 * kMSShardSegmentBytes
              = 5 * 64MiB
              = 320MiB

runwayFloor = saturating_add(stableLive, minimumRunway)
minThreshold = min(max(20MiB, runwayFloor), maxHeap)
rawTarget = clamp(stableLive * (1 + cjHeapGrowth),
                  minThreshold, maxHeap)
```

所有加法都饱和，`maxHeap`仍是硬上限。日志新增`minRunway`和`minThreshold`，便于区分倍率
目标与固定余量。

该策略对不同存活集的行为：

| stable live | `cjHeapGrowth=1`倍率目标 | 固定余量目标 | 最终选择 |
|---:|---:|---:|---:|
| 24MiB | 48MiB | 344MiB | 344MiB |
| 2GiB | 4GiB | 2.3125GiB | 4GiB |
| 接近maxHeap | 可能超过上限 | 可能超过上限 | maxHeap |

因此320MiB只托高“小存活集的绝对runway”，不会把2GiB持久对象按16倍扩张。它也不改变
collect-set、Sweep或Scavenge语义；Scavenge仍由自己的resident reserve策略决定物理页
保留量。

#### 参数筛选证据

在最终固定余量落地前，用环境变量只做20秒上界筛选：

| `cjHeapGrowth` | 吞吐(req/s) | GC平均(ms) | 平均RSS(MB) | 失败 |
|---:|---:|---:|---:|---:|
| 3 | 574.46 | 18.84 | 317 | 0 |
| 5 | 706.60 | 17.98 | 377 | 0 |
| 7 | 802.66 | 17.79 | 387.70 | 0 |
| 10 | 888.75 | 17.12 | 434.43 | 0 |
| 15 | 993.15 | 17.20 | 548.01 | 0 |

该结果证明主要问题是小存活集的绝对runway，而不是需要永久采用超大倍率。最终固定
320MiB后仍使用`cjHeapGrowth=1`，稳定阈值约356～366MiB。

### 14.8 已回退的错误优化

曾尝试让`PublishPendingMarkWord()`返回“summary bit是否首次从0变1”，只有首次置位者才
调用`TryPublishRegion()`。目标是进一步减少scan-state读取，但这个条件不足以证明已有
QUEUED/OWNED/lease：

- owner可以已经exchange走summary，但尚未完成扫描；
- producer可能在owner释放窗口重新设置同一word；
- “summary bit原来为1”只表示内容曾pending，不表示当前一定还有负责该内容的task；
- 如果依据dirty首次置位跳过publication，owner与producer的双边lost-wakeup封闭会被绕开。

该版本在case27约5秒后出现`MCC_AcquireRawData`读取`addr=0x40`的SIGSEGV，说明存活对象边
被漏标后进入Sweep。对应诊断结果目录为：

```text
MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/result/
20260729_121036_region_hotpath_v4_p8_c100_100K_20000ms
```

该修改已经完整回退。最终协议固定为：

```text
publish pending bit
-> 每个winner都允许调用TryPublishRegion
-> TryPublishRegion先用scanState快速拒绝QUEUED/OWNED
-> IDLE竞态继续执行ledger + CAS + pending双重检查
```

也就是说，冗余抑制放在拥有完整任务状态的Scheduler中，不能放在只有content bit信息的
`PublishPendingMarkWord()`中。后续不得重新引入“仅dirty首次置位者发布”的捷径。

### 14.9 构建、测试与SDK来源

#### 三件套来源

| 组件 | 来源 |
|---|---|
| compiler | `/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/msgc` |
| Runtime源码 | 当前`cangjie_runtime_MSGC/runtime`工作区 |
| 安装后Runtime | `cangjie_compiler/output/msgc/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib` |
| stdlib/stdx | `cangjie_compiler/output/msgc/darwin_aarch64_cjnative` |

Runtime使用以下命令以Release+marksweep模式重建并安装：

```bash
python3 build/scripts/rebuild_msgc_runtime_output.py --skip-clean
```

脚本完成SDK覆盖、macOS codesign、安装文件存在性检查和导出符号对比。case27脚本随后使用
同一SDK重新编译`benchmark.cj`，没有复用替换Runtime前的binary。`DYLD_PRINT_LIBRARIES`
记录确认实际加载的是上述`output/msgc`路径。

#### 通过的定向验证

| 类别 | 验证 |
|---|---|
| 单元测试 | `MSMarkBatchTest`、`MSWorkUnitTest`、`MSMarkWorkerPolicyTest`、`MSHeapThresholdPolicyTest` |
| Scheduler契约 | ownership、TaskLedger、termination、MarkControl、TailDrain |
| Mark热路径契约 | DiscoverObject batch、Region owner scan、REGION/OBJECT dual-ready |
| Threshold契约 | fixed runway、硬上限、稳定存活集history、REPORT字段 |
| 构建 | Runtime Release完整构建与SDK安装通过 |
| 静态检查 | 目标源码和测试`git diff --check`通过 |
| 应用稳定性 | case27 20秒和60秒均退出0，无SIGSEGV/SIGABRT/wrong component type |

最终60秒运行元数据：

```text
logical_cpus=8
procs=8
concurrency=100
payload=100K
duration_ms=60000
heap_size=12gb
cj_heap_growth=1
cj_gc_threads=<default>      // 解析为总Mark线程6
```

### 14.10 case27性能结果

三个60秒结果目录：

```text
旧WBuf:
MSGC_RESULT/Result/case27_http_complex_json/result/
20260727_164243_output_msgc_vs_copygc_go_c100_p100K_compare

初始Region:
MSGC_RESULT/Result/case27_http_complex_json/result/
20260729_111210_output_msgc_vs_copygc_go_c100_p100K_compare

最终Region:
MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/result/
20260729_123325_region_hotpath_runway320_final_p8_c100_100K_60000ms
```

| 指标 | 旧WBuf | 初始Region | 最终Region |
|---|---:|---:|---:|
| 成功吞吐(req/s) | 769 | 176 | **903.46** |
| 成功/总请求 | 46324/46324 | 10744/10744 | **54382/54382** |
| GC次数 | 1719 | 673 | **1019** |
| GC总时间(ms) | 34927.06 | 55905.92 | **18500.41** |
| GC平均(ms) | 20.32 | 83.07 | **18.16** |
| GC P95(ms) | 39.58 | 126.92 | **42.55** |
| GC最大(ms) | 160.32 | 175.98 | **140.81** |
| RSS平均(MB) | 805.68 | 415.23 | **651.72** |
| RSS峰值(MB) | 954.22 | 425.09 | **733.88** |
| CPU平均(%) | 502.56 | 670.72 | **556.88** |
| 请求失败 | 0 | 0 | **0** |

相对初始Region：

- 吞吐提升约413.3%；
- GC平均耗时下降约78.1%；
- GC P95下降约66.5%；
- GC总时间下降约66.9%；
- CPU平均下降约17.0%；
- 平均RSS增加约57.0%，这是用更多安全runway换取更少反馈式GC的明确代价。

相对旧WBuf：

- 吞吐提升约17.5%；
- GC平均耗时下降约10.6%；
- GC总时间下降约47.0%；
- 平均RSS下降约19.1%；
- GC P95高约7.5%，说明Region模式的少数对象图膨胀长尾仍需单独观察。

20秒最终筛选为952.68 req/s、GC平均17.61ms、P95 36.22ms、平均RSS 471.11MB、
19227/19227成功。60秒数据作为最终验收值，20秒数据只用于说明短测与稳态之间存在RSS和
P95漂移，不能用短测替代长稳结论。

### 14.11 剩余长尾与后续验收

60秒末段的普通周期大致为：

```text
PreEnumRoots      0.6～0.9ms
EnumRoots         1.2～1.6ms
ConcurrentMark    5～15ms
MarkTermination   0.06～1.2ms
PostMark          0.3～0.9ms
```

少数请求对象图在并发窗口突然膨胀时，观察到ConcurrentMark 22.97ms、
MarkTermination 2.63ms；另有一轮EnumRoots达到6.64ms。`UpdateGCStats`通常只有12～20us，
固定runway的策略计算不是当前长尾来源。

当前不继续加入以下高风险结构：

- 不增加普通对象queue、RangeQueue、per-worker Region queue或第三条ready lane；
- 不允许同一Region多owner，不把scan plane改成多写者原子bitmap；
- 不用dirty首次置位替代Scheduler publication；
- 不在默认MarkTermination增加全collect-set repair扫描；
- 不因个别长尾恢复逐对象WBuf；
- 不用更大的全局`cjHeapGrowth`默认值替代固定加法runway。

后续验收按以下顺序：

1. 使用case26的128MiB与2GiB持久存活对象重新测Mark速率、ConcurrentMark和
   MarkTermination分布；
2. 验证2GiB存活集下阈值仍由`live * (1 + growth)`主导，RSS没有因320MiB固定余量成倍
   扩张；
3. 对case27重复多次60秒运行，观察P95/P99是否稳定，而不是继续按单次短测调参；
4. 若profile确认Region owner对象头延迟显著，再评估TypeInfo预取；
5. 若确认少数Region限制并行度，先调整slice预算和helper启动阈值，不引入同Region多owner；
6. 若EnumRoots长尾可重复，再独立分析root staging和线程栈枚举，不把它归因到Mark batch。

截至本节记录时间，当前版本是“保持Step62 Region任务模型、不增加队列”约束下已经完成
构建、正确性测试和case27长稳验证的基线。后续优化必须以该版本为比较点。
