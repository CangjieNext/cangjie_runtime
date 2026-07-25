# Step52：MSGC 并发 Mark 效率瓶颈采样分析与优化优先级

## 0. 文档定位与结论

本文基于 Step50 的单线程/多线程对照、Step51 的双 lane WorkBuf 实现、case27
160P 长周期日志以及 macOS `sample` 线程栈，分析当前仍阻碍 MSGC Mark
吞吐和并行扩展的因素。本步骤只做分析和排序，不修改 Runtime 行为。

当前可以确认：

1. 4 个后台 Mark 线程相对单线程只有 **1.779x** 加速，按 4 个线程折算的
   并行效率约 **44.47%**，不是近似线性扩展。
2. 三个 Helper 真正开始扫描后负载很均衡，采样中的
   `RunMarkWorker` 活跃样本分别为 1070、1064、1062，最大值与最小值只差
   0.75%。因此首要问题不是“某个 Helper 长期没有工作”。
3. 活跃 Mark 的主热路径是
   `DrainCurrentBuf -> TraceObjectRefFieldsToGCWork -> DiscoverObject`。
   每条引用都要做 RegionInfo 解析、slot 合法性校验、mark bitmap 查询；
   新发现对象还要对 bitmap word 做原子 `fetch_or`。这是限制多核扩展的第一
   类成本。
4. P160 大堆场景中，后台 `gc-helper` 有 1628/3472 个样本位于
   `MemorySet -> memset_s -> __bzero`，另有 157 个样本位于 `mmap`。
   Scavenge 与 Mark 同时消耗内存带宽，是第一梯队的外部干扰。
5. WorkBuf 仍有两个共享锁热点：
   - Full/Partial Global lane 共用一个 mutex；
   - `WorkBufPool` free-list 使用另一个 mutex。
   GC Main 每处理一个 buffer 还会调用一次 `GetCounts()`。采样已经捕获到
   明显的 mutex wait，它不是纯理论问题。
6. Helper 扩容只看 Global 已发布 buffer 数，按每 8 个 buffer 增加 1 个
   Helper。私有 `putCur_`、正在扫描对象将产生的子节点以及 Assist 持有的
   工作都没有进入决策，可能在小 root 集合的宽图展开阶段低估并行度。不过
   本轮代表性长周期启动时已有 15～23 个 root WorkBuf，足以立刻启用全部
   3 个 Helper，所以它不是已测 1.779x 扩展效率的主因。
7. 在 8 核机器上，4 个后台 Mark 线程已经和业务线程竞争 CPU；再让大量
   mutator 执行 Assist 并不会增加物理核。本轮对照中，开启 Assist 没有使
   总 Mark 速率高于关闭 Assist，Step51 当前实现还出现了约 8.62% 的总速率
   回退。

因此下一轮不应继续只增加线程数。推荐顺序是：

```text
先减少 Mark 期间 Scavenge 带宽竞争
  -> 再降低 Global/Pool 每 buffer 锁开销
  -> 同时补齐 helper 启用时序观测
  -> 再做 DiscoverObject/bitmap 的每引用快路径
  -> 有证据后再改 helper 扩容信号
  -> 最后重新标定 Assist 比例和后台线程数
```

## 1. 分析范围与证据

### 1.1 环境

| 项目 | 值 |
| --- | --- |
| 平台 | macOS aarch64 |
| 逻辑/物理 CPU | 8 / 8 |
| Cangjie P | 160，即逻辑 CPU 数的 20 倍 |
| case | case27 HTTP complex JSON |
| payload / concurrency | 100K / 1000 |
| heap | 16 GB |
| Mark 后台宽度 | GC Main + 最多 3 个 Helper |
| WorkBuf 容量 | 1024 个对象指针 |

### 1.2 数据来源

基础对照沿用 Step50：

- 单 Mark 线程、Assist 关闭：
  `20260724_230719_single_gc_thread_no_mark_assist_p160_c1000_100K_60000ms`
- 4 个 Mark 线程、Assist 关闭：
  `20260724_232204_four_gc_threads_no_mark_assist_p160_c1000_100K_60000ms`
- Step51 前 4 线程 + Assist：
  `20260724_223606_after_mark_sweep_helper_policy_p160_c1000_100K_60000ms`
- Step51 当前 4 线程 + Assist：
  `20260725_092521_step51_dual_global_helper_assist_p160_c1000_100K_60000ms`

本轮额外采样：

```text
结果：
MSGC_RESULT/Result/case27_http_complex_json_msgc_high_p/result/
20260725_094350_step51_mark_profile4_p160_c1000_100K_60000ms/msgc

原始线程采样：
.tmp/step51-profile/sample_mark_profile4.txt
```

原始采样共 152,749 行，采样周期为 1 ms、目标时长 8 秒。长 Mark 周期与采样
窗口有重叠，但采样也覆盖了 GC 前后的等待、Sweep/Scavenge 和业务执行，
因此不能把整段线程 wait 样本直接解释为 ConcurrentMark 内部空转比例。

### 1.3 指标口径

本文仍采用 Step50 的两个口径：

```text
backgroundProcessed = mainProcessed + helperProcessed
allProcessed        = backgroundProcessed + assistProcessed
```

`processed` 是从 WorkBuf 取出并扫描的对象数，不是引用边数量、扫描字节数、
分配对象数或业务请求数。不同对象的引用字段数量不同，因此“对象/秒”只能在
相近 case 与 Runtime 配置之间比较。

### 1.4 异常边界

采样运行在下一轮 OOM GC 的 EnumRoots 阶段再次崩溃于
`gc-main-thread -> FrameInfo::ResolveProcInfo`，非法地址为 `0x18`。这是当前
已知的独立根枚举/栈解析问题，会阻止 60 秒稳态压测，但不改变此前已经完整
输出的 ConcurrentMark 周期统计。

采样后的 benchmark 进程 PID 66399 进入 macOS `UNE` 状态，父进程为 1；
显式 `kill -9` 后仍未退出。它必须被标记为旧挂死进程，不能当作当前测试仍在
正常推进。

## 2. 已确认的扩展效率

代表性长周期如下：

| 配置 | Mark 时间 | Helper | Main | Assist | 后台速率 | 全部速率 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 线程，Assist 关 | 12.129179 s | 0 | 72,563,060 | 0 | 598.25 万/s | 598.25 万/s |
| 4 线程，Assist 关 | 4.885344 s | 39,219,200 | 12,767,076 | 0 | 1,064.13 万/s | 1,064.13 万/s |
| 4 线程，Assist 开，Step51 前 | 6.452261 s | 29,349,888 | 9,975,073 | 30,590,976 | 609.48 万/s | 1,083.59 万/s |
| 4 线程，Assist 开，Step51 当前 | 6.952489 s | 29,626,087 | 10,234,683 | 28,983,296 | 573.33 万/s | 990.21 万/s |

关闭 Assist 时：

```text
4 线程相对单线程加速 = 10.641272 / 5.982520 = 1.7787x
4 线程并行效率       = 1.7787 / 4 = 44.47%
```

开启 Assist 后，后台对象被 mutator 分流，所以不能只比较后台速率；即使用
`Helper + Main + Assist` 的全部口径，Step51 当前的 990.21 万/s 也低于
Step51 前的 1,083.59 万/s，回退约 8.62%。后台口径回退约 5.93%。

这些数据说明：

- 增加 3 个 Helper 确实有收益，但收益远低于 4 倍；
- Assist 主要是在后台线程和 mutator 之间迁移工作，不等于增加硬件并行度；
- Step51 降低了每 buffer 的 in-flight/credit 控制开销，却没有消除每引用
  扫描、Global/Pool 锁和内存带宽瓶颈；
- Step51 的 Partial builder 发布增加了 Assist 的 Global/Pool 交换次数，
  需要作为回退候选单独验证。

## 3. 长周期线程采样

### 3.1 对应的已完成长 Mark 周期

本轮采样运行的第 8 个已完成 GC 周期为：

```text
PreEnumRoots       = 1,062,633 us
  stopSweep        = 1,003,217 us
  clearBitmaps     =    58,120 us
EnumRoots          =   448,568 us
ConcurrentMark     = 4,363,667 us
Helper processed   = 34,796,907
Main processed     = 11,493,237
Assist processed   =  1,680,384
MarkTermination    =    20,461 us
```

由此得到：

```text
后台 Mark 速率 = 46,290,144 / 4.363667 = 1,060.81 万对象/s
全部 Mark 速率 = 47,970,528 / 4.363667 = 1,099.32 万对象/s
Assist 占比     = 1,680,384 / 47,970,528 = 3.50%
```

这一次 Assist 占比较低，总速率接近关闭 Assist 的 4 线程对照，进一步说明
主要上限来自后台扫描本身，而不是缺少 mutator 参与。

### 3.2 三个 Helper

每个 `gc-pool` 线程各有 3472 个样本：

| 线程 | `RunMarkWorker` | `DrainCurrentBuf` | `TraceObjectRefFieldsToGCWork` 主要样本 |
| --- | ---: | ---: | ---: |
| t1 | 1070 | 897 | 481 + 270 |
| t2 | 1064 | 894 | 520 + 208 |
| t3 | 1062 | 860 | 475 + 255 |

`DrainCurrentBuf / RunMarkWorker` 分别为 83.8%、84.0%、81.0%。三个 Helper
的活跃样本最大/最小比为 1.0075，说明活跃后的工作分布很好。

每个线程还有 2107 至 2224 个 `GCPoolThread::WorkerFunc` 条件变量等待样本。
这些样本包含采样窗口中 Mark 周期之外的时间，不能直接说“Helper 在
ConcurrentMark 内等待了 60%”；但它们说明后续需要记录每个 Helper 的首次
启用时刻和活跃时长，才能准确判断动态扩容是否偏晚。

该 profile 长周期在进入 ConcurrentMark 时一次转移了 22,920 个 root entry，
即约 23 个 WorkBuf。按每 8 个 buffer 启用一个 Helper，理论上初始就达到
3 个 Helper 上限。另两组代表性长周期分别转移 14,708 和 20,324 个 entry，
也足以初始拉满。因此“扩容偏晚”不能解释这些长周期只有 1.779x 的扩展效率。

### 3.3 GC Main

`gc-main-thread` 中：

- `ConcurrentMark`：1227 个样本；
- `RunConcurrentMarkLoop` 的 drain 路径：1025 个样本；
- `DrainCurrentBuf`：811 个样本；
- `TraceObjectRefFieldsToGCWork`：524 个样本；
- `GetCounts()` 对应的 Global mutex wait：106 个样本。

GC Main 大部分 Mark 时间同样在对象扫描，而不是终止协议。该周期
`MarkTermination / ConcurrentMark` 仅为 0.47%。

### 3.4 后台 Scavenge

`gc-helper` 的 3472 个样本中：

| 路径 | 样本 | 占该线程样本 |
| --- | ---: | ---: |
| `BackgroundScavengeLoop` | 1791 | 51.6% |
| `MemorySet -> memset_s -> __bzero` | 1628 | 46.9% |
| `mmap` | 157 | 4.5% |
| 后台等待 | 1452 | 41.8% |

这不是 GC Mark worker，而是后台内存回收线程。它在 Mark 重负载期间持续大块
清零和重映射，正好与对象/RegionInfo/bitmap 的随机读写争用缓存与 DRAM
带宽。需要注意，`sample` 的聚合窗口也覆盖了长 Mark 开始前的 GC 阶段，
不能把 1628 个清零样本全部精确归到 ConcurrentMark 重叠区间；源码允许两者
并发、采样也证明该窗口内清零占用很重，下一轮仍要用 phase-aware 字节计数
定量。

## 4. 瓶颈一：每条引用的 Discover/Mark 成本

`DrainCurrentBuf()` 本身只负责消费一个已经取得所有权的 `WorkBuf`，真正按
引用边放大的热路径是字段扫描后的 `DiscoverObject()`。分析这一瓶颈时必须
区分三个粒度：

- 每个灰对象一次：WorkBuf Pop、TypeInfo/WeakRef 分类、GCTib/数组布局选择；
- 每条引用一次：引用 load、Region/slot 解析、bitmap probe；
- 每个新发现对象一次：stateWord 校验、atomic mark winner、HasRef 分类和
  putCur_ 入队。

### 4.1 `DrainCurrentBuf` 的实现与计数语义

当前调用链为：

```text
GC main / Helper / Mutator Assist
  -> 已取得 getCur_
  -> DrainCurrentBuf(gcw)
       while getCur_ 非空：
         obj = Pop()
         Debug：重新验证 traceable address 与 stateWord
         Release：编译期完全跳过 trusted-grey 重校验
         nProcessed++
         HasRefField?
           否 -> 跳过
           是 -> WeakRef?
                   是 -> deferredMainThreadQueue_
                   否 -> TraceObjectRefFieldsToGCWork
       输入 WorkBuf 扫空
       -> WorkBufPool::Put(getCur_)
```

`getCur_`、`putCur_`、WorkBuf 的 `len/objs` 都由当前 owner 独占，Pop 和普通
Push 本身没有 worker 间竞争。共享同步只在输出满后发布 Global、申请/归还
WorkBuf，以及 WeakRef 进入 deferred queue 时发生。

`nProcessed` 不是“本轮标记对象总数”，而是被 WorkBuf 消费的灰对象数：

- 普通 leaf 对象在 mark 成功后不进入 WorkBuf；
- 指向已标记对象的 duplicate edge 不计入；
- WeakRef 先计入 processed，再延迟到 main thread；
- 不同灰对象含有的引用字段数量可能差异很大。

因此此前的 `objects/s` 表示“灰对象 drain 速度”，不能直接等同于每秒
`DiscoverObject` 调用数。分析每条引用成本还需要 `visitedEdges/s`。

### 4.2 当前每条边的调用链

`DrainCurrentBuf()` 对每个有引用字段的灰对象调用：

```text
TraceObjectRefFieldsToGCWork
  -> ForEachRefFieldGCInline
  -> GCTib::ForEachBitmapWordFast
  -> 对每一个引用字段：
       MSLoadRawReference
       DiscoverObject(target)
         -> ResolveMarkTarget
         -> MSRegionInfoArray::GetRegionInfoDirect
         -> Region 类型/范围/size class/slot 对齐校验
         -> IsMarkBitSet
         -> 未标记时 IsValidObject
         -> MarkBitmapWord(__atomic_fetch_or)
       newlyMarked && target->HasRefField()
         -> PerPGCWork::PutObj
```

当前 `ResolveMarkTargetImpl()` 以 heap-relative 32 KB Region 下标直接读取
metadata，并从同一个 `heapOffset` 得到 Region 内偏移。`MSRegionManager` 与
`FreeManager` 都按 `MS_REGION_SIZE` 下标管理 Region，因此 Mark 热路径不再
探测相邻 metadata entry；Debug 构建仍与 `GetRegionInfoValidated()` 比较，
用于尽早发现分配坐标不变量被破坏。随后它继续执行：

- LARGE base/span 解析；
- FREE/EMPTY/SPAN 和 size class 检查；
- slot size 与 slot count 检查；
- reciprocal slot index；
- `slotIdx * slotSize == offset` 精确对齐检查；
- BUMP `allocPtr` 上界检查；
- bitmap word/bit 计算。

这是每条引用边都支付的成本，不只是每个新对象支付。已经 marked 的重复引用
虽然不会执行 stateWord 校验和原子 RMW，仍要先完成 region/slot 解析和 bitmap
load。

每条引用按结果可进一步分成：

| 引用结果 | 当前主要成本 |
|---|---|
| null | 字段 load、null 分支 |
| 非堆地址 | 字段 load、heap range 判断 |
| 已标记对象 | 完整 Region/slot 解析、bitmap atomic load |
| 首次发现的 leaf | 解析、bitmap load、stateWord acquire、atomic fetch_or、TypeInfo/HasRef |
| 首次发现的引用对象 | 上述全部操作，再写入本地 putCur_ |
| 多线程同时发现 white | 多线程都做校验和 RMW，只有一个 mark winner |

### 4.3 每个灰对象的重复 TypeInfo 与数组成本

普通非数组灰对象当前可能对同一个对象 stateWord/TypeInfo 执行最多 4 次
acquire 读取：

1. `DrainCurrentBuf(): obj->HasRefField()`；
2. `DrainCurrentBuf(): obj->IsWeakRef()`；
3. `ForEachRefFieldGCInline(): obj->GetTypeInfo()` 后再次 `HasRefField()`；
4. `ForEachRefFieldInNonArrayObjectGC(): obj->GetGCTib()` 再次获取 TypeInfo。

`BaseObject::GetTypeInfo()` 定义在 `BaseObject.cpp`；没有 LTO 时，除了 acquire
load 和 48-bit TypeInfo 地址重组，还可能保留实际函数调用。这里的 48-bit
语义只属于 stateWord 中的 TypeInfo 编码，不是 MSGC RefField：MSGC 引用字段
仍由 `MSLoadRawReference()` 读取完整 64 位值。

新 target 的 white-slot 校验也已通过 `IsValidObject()` 读取一次 TypeInfo，
但 mark winner 随后又执行 `target->HasRefField()`，再次读取同一 stateWord。
当前 `DiscoverObject()` 只返回 bool，丢弃了已经加载的 TypeInfo。

struct array 还有一个稳定循环成本：

```cpp
for (MIndex i = 0; i < arrayLength; ++i) {
    gcTib.ForEachBitmapWordFast(contentAddr, visitor);
    contentAddr += mArray->GetElementSize();
}
```

`GetElementSize()` 内部再次通过数组对象取得 component TypeInfo。element size
在整个数组扫描期间不变，应在循环外只计算一次。object/ref array 的线性字段
访问本身已经接近最小成本。

### 4.4 为什么多线程不能线性

对象图扫描是典型的间接访问链：

```text
对象 -> TypeInfo/GCTib -> 引用目标 -> RegionInfo -> markBitmap
```

它的算术量不大，但缓存命中难以保证。增加线程后会同时放大：

- 对象、TypeInfo、RegionInfo 和 bitmap 的随机读；
- 同一 Region 的 bitmap cache line 争用；
- 新对象 mark winner 的原子 RMW；
- 线程间发布 full WorkBuf 后的工作迁移和局部性损失。

一个 bitmap word 覆盖 64 个 slot。多个线程扫描指向同一 Region 的相邻对象时，
即使设置不同 bit，也会在同一 cache line 上执行原子 RMW，形成所有权迁移。
这与“4 线程只有 1.779x”方向一致。`sample` 不能直接给出 cache miss 和
coherence 事件，所以这里是由源码和热点栈支持的机制判断，后续仍需硬件计数器
或专门 microbenchmark 定量。

竞争点需要区分为：

- **真正的共享写竞争**：small object 的 mark bitmap `fetch_or`。同一
  64-bit word 的 64 个 slot 直接争用；同一 cache line 中相邻 bitmap word
  还会产生 false sharing。AArch64 LSE 能减少原子指令序列和重试成本，但不能
  消除 cache-line ownership 迁移。
- **共享读写产生的 coherence 压力**：mutator 仍可能更新当前对象字段，GC
  relaxed load 与写屏障保证逻辑正确性，但硬件 cache line 仍可能在核间迁移。
- **只读带宽压力**：TypeInfo/GCTib、RegionInfo、目标 stateWord 和 bitmap
  通常不在同一 cache line，多线程共同放大 LLC/TLB/DRAM 压力。
- **每 WorkBuf 的锁竞争**：输出满时进入 Global mutex，输入扫完进入
  WorkBufPool mutex；它们不是每引用固定成本，单列在第 6 节。
- **WeakRef 特有竞争**：每个 WeakRef 当前都会获取
  `deferredMainThreadMutex_`，普通对象不触碰该锁。

Helper 把本地 `putCur_ >= 128` 提升为输入后，扫描时还需要申请一个新的输出
buffer，扫描结束又归还旧输入。没有 worker-local empty cache 时，最坏可按约
128 个灰对象触碰两次 WorkBufPool mutex，而不是始终按 1024 容量摊销。

### 4.5 WorkBuf producer 不变量的当前例外

绝大多数 WorkBuf producer 已满足：

```text
地址/slot 合法
  -> white-to-grey mark winner
  -> HasRefField
  -> WorkBuf
```

包括普通 root discover、字段 tracing、write-barrier activeLog 和 bulk barrier。
因此 WorkBuf 的设计目标是只保存 `marked + valid + has-ref` 的 trusted-grey。

但源码审计发现 `TracingCollector::EnumAllSurrectedExportRoots()` 当前仍直接：

```cpp
rootSet.push_back(obj);
```

`EnumAllCommonRootsSerial()` 随后把这个 raw root 放入 `markWorkStack_`，
`TransferRootsToGCWork()` 又原样搬进 WorkBuf。这条路径没有显式经过本轮
`DiscoverObject -> mark winner -> HasRefField`。从当前调用链不能证明它满足
完整 trusted-grey/HasRef contract，也不能证明 root 自身已经获得本轮 mark bit。

所以 `DrainCurrentBuf()` 与 scanner 中的两个 `HasRefField()` 最终可以合并，
但现在不能直接同时删除。必须先把 resurrected export root 统一改为通过
`EnumAndTagRawRoot/TryEnqueueGreyObject` 进入队列，再在 Debug 中 assert producer
contract，Release 才能删除消费者重复 gate。

### 4.6 不应直接删除的校验

`IsValidObject()` 和 slot/range 检查是为了阻止 Sweep 后 stale/zeroed slot 被
重新置 mark bit。当前分支仍有 invalid stateWord、并发 Sweep 和根枚举崩溃
问题，不能为了性能直接删掉这些校验。

同样不能简单把：

```text
IsMarked load + stateWord validate + atomic fetch_or
```

改成无条件 atomic `fetch_or`。对 stale slot 先置位、再发现无效并回滚，会让
其他线程短暂观察到错误的 marked 状态，破坏 mark winner 语义。

### 4.7 可实施方向

#### 4.7.1 可直接实施的固定成本优化

1. **缓存当前对象 TypeInfo。** `DrainCurrentBuf()` 每个对象只获取一次
   `TypeInfo*`，用它完成 HasRef、WeakRef、RawArray、GCTib 和 component
   分类。
2. **增加 known-TypeInfo scanner。** 例如
   `ForEachRefFieldGCKnownType(obj, typeInfo, visitor)`，避免 scanner 再次读取
   stateWord；在 producer contract 修正前仍保留一次 HasRef gate。
3. **hoist struct-array element size。** 在循环外由缓存的 component TypeInfo
   计算一次，不在每个元素重新读取数组 TypeInfo。
4. **复用 Discover 已读取的 TypeInfo。** 将内部接口扩展为返回 mark winner
   校验阶段取得的 `TypeInfo*`，赢家直接用它判断 HasRef，避免
   `target->HasRefField()` 再读 stateWord。
5. **按输入初始 len 计算 Release processed。** Release trusted-grey 路径可在
   drain 前保存 `input->len`，去掉每对象 `nProcessed++`；Debug 保留逐项计数
   和非法 entry 跳过语义。
6. **增加 resolved-coordinate 内部 bitmap 接口。**
   `ResolveMarkTargetImpl()` 已经验证 markBitmap、slotIdx、wordIdx 和 bitMask；
   `ResolvedMarkTarget` 可调用内部 trusted load/RMW，避免
   `IsMarkBitSet()/MarkBitmapWord()` 再次计算 bitmapWords 和重复边界检查。
7. **为 Drain WeakRef 使用 trusted enqueue。** 已经成功完成 TypeInfo/WeakRef
   分类的 WorkBuf entry 不必在 `PushSingleToDeferredMainThread()` 中再次做同一
   地址和 stateWord 校验；保留通用安全入口，另加仅供 audited Drain 调用的
   Debug-assert 版本。

#### 4.7.2 修正 producer 后实施

1. 将 resurrected export root 统一走
   `EnumAndTagRawRoot/TryEnqueueGreyObject`。
2. 将 WorkBuf contract 固化为 `marked + valid + has-ref`，所有 producer 使用
   collector-owned enqueue API，禁止 raw `push_back` 进入并发 WorkBuf。
3. Debug 对 contract 做 assert；Release 删除 `DrainCurrentBuf` 与 scanner 的
   重复 HasRef gate。

#### 4.7.3 结构性并行优化

1. **已实施：RegionInfo 直接定位。** Mark 热路径按
   `(addr - heapStart) >> 15` 得到唯一 `MSRegionInfo`，同一次减法计算
   Region 内 offset；`GetRegionInfoValidated()` 只保留给非热路径和 Debug
   一致性检查。
2. 在 worker 局部保留小型 RegionInfo/size-class cache，复用连续对象的 slot
   解析信息；命中必须重新验证地址范围。
3. 复用 barrier 的 bitmap-word batching 思路：按
   `(MSRegionInfo*, wordIdx)` 聚合多个 bit，一次 `fetch_or(mask)` 获得多个
   winner，再分别入队。它可减少数组和高扇出对象的原子 RMW 数，但仍不能
   省略每个候选的地址、slot 和 white-stateWord 合法性判断。
4. **已实施：worker-local empty WorkBuf cache。** GC main/helper/assist
   在 `PerPGCWork` 中保留最多两个空 buffer，批量从 Pool 获取和归还，减少本地
   128 threshold promote 后的 Pool Get/Put。
5. WeakRef 使用 worker-local deferred list，buffer 满或 worker 退出后批量
   合并，避免弱引用密集场景逐对象争抢同一 mutex。
6. 在上述优化仍不足时，再考虑 Global Full/Partial 分片或 owner-local deque。

### 4.8 诊断与验收指标

计数必须保存在 worker-local stats，周期结束后由 GC main 汇总，不能为了诊断
在每条引用上增加新的全局原子竞争。至少记录：

- `visitedEdges`、null/non-heap 数；
- direct/prev/next RegionInfo lookup 次数；
- already-marked bitmap fast return；
- white stateWord valid/invalid 数；
- mark winner 与并发 RMW loser 数；
- small/large target 数；
- bitmap load、atomic RMW 和 batching mask bit 数；
- 含引用对象、leaf、WeakRef 数；
- WorkBuf global publish、pool Get/Put 和 local-cache hit 数。

验收不能只看灰对象/秒，还要同时比较：

- `visitedEdges/s` 与每扫描对象平均引用数；
- 每 1000 条边的 Region slow path、bitmap RMW 和 CAS loser；
- 单线程基线、多线程加速比和并行效率；
- ConcurrentMark 时间、HTTP 吞吐、失败请求；
- RSS、MarkTermination residual work 和下一轮 Sweep/Scavenge 压力。

## 5. 瓶颈二：并发 Scavenge 的内存带宽干扰

### 5.1 采样证据

覆盖第 8 个 GC 周期及其长 Mark 前半段的采样窗口里，后台 `gc-helper` 的主要
路径是：

```text
BackgroundScavengeLoop
  -> FreeManager::ScavengeFreeRegionsByRegionBudget
  -> FreeManager::ReleaseUnits
  -> MemorySet
  -> memset_s
  -> __bzero
```

`__bzero` 占该线程全部样本的 46.9%，`mmap` 另占 4.5%。该聚合窗口并非每个
样本都位于 ConcurrentMark，但其中包含长 Mark 前半段，而且当前源码没有在
ConcurrentMark 期间暂停 Scavenge；两者在重叠区间共同使用 LLC 和 DRAM。

这类干扰不会体现在 `RunMarkWorker` 的 mutex wait 中，但会让每个 Helper 的
对象、TypeInfo、RegionInfo 和 mark bitmap load 延迟同时上升，因此非常容易
出现“线程都在忙，吞吐却不随线程数增长”。

### 5.2 大堆规模证据

同一运行中：

- 第 8 周期 `PreEnumRoots` 为 1.062633 秒，其中 force sweep 为
  1.003217 秒，占 94.41%；
- 下一轮 OOM GC 开始时有 91,928 个 unswept Region；
- force sweep 需要 2.815757 秒；
- 清理 mark bitmap 需要 77.132 ms；
- 进程物理内存采样约 12.2 GB。

所以 P160 下不只是 Mark 线程数问题，还存在 Sweep/Scavenge backlog、巨大
Region 数和内存归还同时放大的规模问题。

### 5.3 可实施方向

1. ConcurrentMark 活跃期间降低 Scavenge 的字节预算或 duty cycle；不是仅按
   Region 个数限流，因为不同 span 的实际清零字节差异很大。
2. 将低压力下的 eager zero/re-map 延后到 Mark 结束后；高内存压力仍保留最小
   Scavenge 进度，避免因完全暂停导致 OOM。
3. 让 Scavenge budget 同时参考：
   - 当前 GC phase；
   - heap retained/active 差值；
   - unswept backlog；
   - 最近一个窗口的 Mark 吞吐。
4. 增加 `scavengeBytesDuringConcurrentMark` 与 `scavengeActiveUsDuringMark`
   日志，避免只凭线程采样判断。
5. 做一组严格 A/B：仅在 ConcurrentMark 暂停 Scavenge，其他参数不变，
   比较 4 线程对象/秒、进程 RSS、OOM 时间和下一轮 force sweep。

如果暂停 Scavenge 后 Mark 明显提升但 RSS 失控，应改成带宽限流而不是永久
关闭。

## 6. 瓶颈三：Global Work 与 WorkBufPool 锁

### 6.1 两类共享 mutex

当前结构不是只有一把队列锁：

```text
globalFull_ LFStack mutex
  - Push Full/Partial
  - Pop / PopFull / PopPartial
  - GetCounts

workBufPool_.freeList_ LFStack mutex
  - WorkBufPool::Get
  - WorkBufPool::Put
```

Full/Partial 已经是 O(1) 的逻辑 lane，但两条 lane 仍共用 Global mutex。
每个输入 buffer 被 drain 后，`DrainCurrentBuf()` 又调用
`workBufPool_->Put(getCur_)`，所以还会触碰 free-list mutex。

### 6.2 采样证据

三个 Helper 中，停在 `RunMarkWorker()` 第 1000 行、即 Global 获取链上的
mutex wait 分别为：

| Helper | Global 获取 wait | `RunMarkWorker` 活跃样本占比 |
| --- | ---: | ---: |
| t1 | 86 | 8.0% |
| t2 | 66 | 6.2% |
| t3 | 72 | 6.8% |

处理完输入后回收 WorkBuf，停在 `WorkBufPool::Put -> LFStack::Push` 的 wait
样本分别为 21、27、22。

GC Main 每处理一个 buffer 都执行：

```cpp
LFStack::Counts backlog = globalFull_.GetCounts();
```

采样中该行有 106 个 mutex wait，占 `RunConcurrentMarkLoop` drain 路径
1025 个样本的 10.3%。GC Main 除了 Pop 和 Pool Put，还多支付了一次 Global
锁。

这些百分比是采样栈占比，不是精确 CPU 时间；但它们足以确认 mutex 已进入
Mark 热路径。

### 6.3 当前 Helper 还有一次可避免的双锁探测

本地 `putCur_ < 128` 时，Helper 依次调用：

```cpp
AcquireOneFullBuffer();
AcquireOnePartialBuffer();
```

Full 为空而 Partial 有工作时，会先后获取同一把 Global mutex 两次。现有
`LFStack::Pop()` 本身已经能在一次加锁内完成“Full 优先、其次 Partial”，
因此 Helper 的这两个调用可以合并为一次 `AcquireOneBuffer()`，不改变选择
顺序。

### 6.4 可实施方向

按优先级：

1. Helper 用一次 `AcquireOneBuffer()` 代替 Full/Partial 两次串行探测。
2. 为 GC Main 和每个 Helper 增加 2～4 个空 WorkBuf 的线程本地 cache，
   批量向 `WorkBufPool` 归还，避免每 drain 一个 buffer 都锁 free-list。
3. Helper 从 Global 小批量 PopN 到私有输入 cache，减少获取次数。批量不宜
   过大，否则会隐藏 backlog、让一个 Helper 囤积工作并破坏均衡。
4. 将 `count_`、`fullCount_` 改为可无锁读取的近似原子计数，策略调节读取
   approximate count；只有收敛判定使用 mutex 下的精确空快照。
5. 或者 GC Main 每 N 个 buffer 才更新一次 backlog/EMA，遇到空队列、target
   扩容和收敛候选时再强制精确采样。
6. 若上述仍不足，再评估 Full/Partial 双锁或带 ABA 版本戳的无锁栈。它的
   正确性风险远高于本地 cache 和批量操作，不应作为第一步。

这一组优化需要同时记录 `globalLockWaitNs`、`poolLockWaitNs`、Pop/Push 次数、
平均 buffer 长度和每个 Helper 的私有 cache 峰值，防止只把工作隐藏起来。

## 7. 潜在瓶颈四：Helper 并行度打开偏晚

### 7.1 当前策略

`MSMarkWorkerPolicy` 使用：

```text
desiredHelpers = ceil(publishedBacklogBuffers / 8)
activeHelpers  = max(previousActiveHelpers, desiredHelpers)
```

周期内只升不降。即使 `maxHelpers == 3`，初始 Global 只有一个 root partial
buffer 时也只启用一个 Helper。

### 7.2 Step51 本地优先与动态扩容的冲突

Helper 当前选择顺序是：

```text
本地 putCur >= 128
  -> Global Full
  -> Global Partial
  -> 本地任意非空 putCur
```

这有利于局部性，但存在一个并行播种问题：

1. 初始只有 1 个 Helper eligible；
2. 它 drain 一个输入，产生 128～1023 个灰对象；
3. `putCur_` 未满，不会进入 Global；
4. 下一轮优先把本地 `putCur_` 提升为 `getCur_`；
5. 其他 Helper 看不到 published backlog，也没有可 Pop 的输入。

如果对象图持续产生“超过 128、但不足 1024”的局部子树，这一个 Helper 可以
长时间独自深挖闭包。只有某轮输出真正填满并发布 Global，GC Main 才可能看到
足够 backlog 并提高 target。

本轮采样中三个 Helper 最终都已活跃且负载均衡，说明该问题不会永久饿死
Helper。更重要的是，该周期初始有约 23 个 root WorkBuf，按策略已经足够立即
启用全部 3 个 Helper；Step50 的两个代表性长周期也分别有约 15 和 20 个 root
WorkBuf。因此本机制是小 root 集合、宽对象图场景中的潜在盲点，而不是当前
case27 长周期扩展不足的已确认主因。

### 7.3 可实施方向

1. 先增加周期日志：
   - 每次 target 从 N 扩到 N+1 的时间戳；
   - 扩容时的 Full/Partial buffer 数和对象数；
   - 每个 Helper 首次进入、活跃微秒数、drain buffer 数；
   - 每个 Helper 本地 promote 次数和对象数。
2. 在 `activeHelpers < maxHelpers` 时，把第一个达到 128 的 partial 输出发布到
   Global，作为并行 seed；全部 Helper 启用后恢复本地优先。
3. 或让 worker 每处理一个输入 buffer 后报告 `generatedChildren`，GC Main
   使用“已发布 backlog + 本轮新生对象”估计需求，而不是只看 Global 节点数。
4. 可以直接对长周期启用全部 Helper，但必须保留小周期门槛，否则前几个只有
   数百对象的 GC 会被 task 唤醒和 quiesce 开销淹没。
5. target 只升不降在单个周期内是合理的；当前问题主要是升得晚，不应先引入
   高频 scale-down 抖动。

推荐先增加可观测性，不立即改 seed 策略。只有新用例证明 helper 初始没有拉满
且 local promote 长时间隐藏工作时，再采用“单次 seed publish”；它保留
Step51 的局部性目标，改动面小于重写整个 worker policy。

## 8. 瓶颈五：Assist 与后台 Mark 争用 CPU

### 8.1 Assist 不增加物理核

本机只有 8 个逻辑/物理核。4 个后台 Mark 线程运行时，HTTP 业务、网络调度、
分配和后台 Scavenge 也在运行。mutator Assist 使用的是业务线程本身：

- 它能对分配做背压并保证 GC 取得进展；
- 它能在后台 Mark 没吃满 CPU 时补充扫描；
- 但在 CPU 和内存带宽已经饱和时，只会把业务 CPU 转成 GC CPU，并与后台
  worker 争用 bitmap 和 Global mutex。

因此 Assist 的目标不应是让 `allProcessed` 数字最大，而应是在限制分配速度的
同时提高“业务吞吐 + GC 收敛”的整体结果。

### 8.2 当前共享点

一次有实际工作的 `MSMarkAssist()` 包含：

```text
TryEnterAssist                    assistsInFlight 原子
TryStealBgCreditPartial           共享 bgScanCredit CAS
AcquireOneFullBuffer              Global mutex
AcquirePartialOutputBuffer        Partial mutex 或 Pool mutex
DrainCurrentBuf
PublishPartialOutput              Global mutex 或 Pool mutex
ExitAssist                        assistsInFlight 原子
```

Step51 要求每次 Assist 结束发布 partial builder，解决了 TLS 隐藏尾部，但也使
160 个 P 可能高频触碰同一 Global 和 Pool。

### 8.3 数据解释

Step51 前的代表周期中，Assist 承担 43.75% 的全部扫描；Step51 当前周期承担
42.10%，但全部速率从 1,083.59 万/s 降到 990.21 万/s。该对照不是完全固定
对象图，不能单独证明回退全部来自 Partial lane；不过它至少说明“更多 mutator
参与”没有突破后台扫描的硬件上限。

本轮 profile 第 8 周期 Assist 只占 3.50%，全部速率仍达到 1,099.32 万/s。
这进一步支持：在当前 8 核环境中，低 Assist 占比不妨碍获得约 1100 万对象/s，
盲目提高 Assist 并发不是首要优化。

### 8.4 可实施方向

1. 增加 `peakAssistsInFlight`、Assist 调用次数、真正取得 Full 的次数、
   credit-only 次数和锁等待时间。
2. 给同时执行真实扫描的 Assist 设置上限，避免 P160 同时进入；credit-only
   路径可以不占扫描名额。
3. 根据后台 worker 最近吞吐和 Global Full backlog 调节 Assist：
   - 后台有空闲或 Full 持续增长时允许；
   - 后台已吃满 CPU、Full 不增长时以 pacing 为主，减少并发扫描。
4. 对 Partial builder 做 TLS/小型局部缓存，减少每次 Assist 的 Pop+Push；
   但必须设置发布时间/对象阈值，不能重新引入直到 MarkTermination 才可见的
   隐藏尾部。
5. 重新对比三组：
   - Assist 关闭；
   - Assist 开启但扫描并发封顶；
   - 当前无限并发 Assist。

验收必须同时看 HTTP 吞吐、失败请求、Mark 时间、Assist 占比和 RSS，不能只看
GC 对象/秒。

## 9. 次级开销与非首要问题

### 9.1 重复 TypeInfo/引用检查

详细调用次数、数组循环成本和 producer 前置条件见 4.3、4.5 与 4.7。
重复 TypeInfo/stateWord 读取是可压缩的稳定开销，但相对 RegionInfo/bitmap
随机访问属于第二梯队；在 resurrected export root 统一进入 Discover/HasRef
过滤前，不能把消费者的两个 HasRef gate 直接同时删除。

### 9.2 WorkEpoch 与 50 微秒等待

Global Push 会执行一次 `WorkEpoch::Notify()` 原子递增；存在 waiter 时还会取
通知 mutex。Helper 无工作后最多等待 50 微秒。

三个 Helper 的 `WaitForChange` 样本分别约 40、60、61，低于 active scan 和
Global 获取 mutex 样本。当前没有证据证明 50 微秒本身是主要吞吐瓶颈。先修
工作可见性和锁，再调整等待策略。

### 9.3 MarkTermination

profile 第 8 周期：

```text
MarkTermination / ConcurrentMark = 20.461 ms / 4.363667 s = 0.47%
```

所以当前长周期的对象/秒瓶颈不在 tail drain/termination。用户此前要求不对
tail drain 做特殊扩线程，这与本轮证据一致。

### 9.4 Worker 分工不均

三个 Helper 的活跃样本只差 0.75%，且 Helper 处理量约为 Main 的 3.03 倍，
接近 3 个 Helper 对 1 个 Main 的宽度关系。没有证据支持优先开发复杂的
Helper-to-Helper stealing。

### 9.5 Root Enum、force sweep 和 crash

这些问题严重影响总 GC 时延与稳定性，但不属于 ConcurrentMark 每秒扫描对象
的内部瓶颈：

- 第 8 周期 force sweep 1.003 秒、EnumRoots 448.568 ms；
- 下一轮 force sweep 2.816 秒；
- 随后在 `FrameInfo::ResolveProcInfo` 崩溃。

它们需要独立修复，否则任何 60 秒性能结论都不稳定。

## 10. 优化优先级与验证矩阵

| 优先级 | 改造 | 预期作用 | 风险 | 最小验证 |
| --- | --- | --- | --- | --- |
| P0 | 增加 helper 首次启用/活跃时长、region lookup、bitmap winner、两类锁和 Scavenge bytes 计数 | 将推断变成可量化证据 | 低，注意计数本地化 | 诊断开关开/关自身开销 A/B |
| P1 | Helper 单次 `Pop()` 取 Full/Partial；GC Main 降低 `GetCounts()` 频率 | 去掉已采样到的 Global 重复锁 | 低到中 | WorkBuf invariant + case27 对象/秒 |
| P1 | GC Main/Helper 本地空 WorkBuf cache、批量回 Pool | 去掉每 buffer free-list 锁 | 中 | pool 无泄漏、quiesce/Dispose 全回收 |
| P1 | ConcurrentMark 期间按字节限流 Scavenge | 释放 LLC/DRAM 带宽 | 中，可能增大 RSS | Mark 速率、RSS、OOM、后续 force sweep |
| P2 | 有证据后，在未满 helper 时发布一次 partial seed | 更早打开小 root 宽图的并行度 | 中，可能损失局部性 | 每 helper start time、工作均衡和锁次数 |
| P2 | Assist 扫描并发封顶 | 避免 P160 oversubscription | 中，可能增加分配等待 | HTTP 吞吐、Mark 时间、失败数 |
| P2 | 去掉重复 `HasRefField()`，增加 known-has-ref 内部扫描入口 | 降低每对象固定成本 | 低 | 数组/struct/ref 扫描测试 |
| P2 | Region lookup 对齐/direct fast path | 降低每条边 metadata 成本 | 中到高，涉及分配布局 | 跨 Region 边界与 LARGE/SPAN 测试 |
| P3 | 按 bitmap word 批量 mark | 降低原子 RMW/cache line 争用 | 高，改变 winner/入队协议 | 专项并发正确性和 stale-slot 测试 |

建议分三轮实施：

```text
第一轮：P0 指标 + 两个低风险锁优化 + Scavenge 暂停 A/B
第二轮：本地 WorkBuf cache + Assist 并发封顶；按指标决定是否 partial seed
第三轮：Region/bitmap 每引用快路径
```

每轮固定以下对照：

| 配置 | 目的 |
| --- | --- |
| 1 Mark thread，Assist off | 单执行通道基线 |
| 2 Mark threads，Assist off | 观察第一段扩展 |
| 4 Mark threads，Assist off | 观察多核扩展 |
| 4 Mark threads，Assist capped | 评价 pacing 与业务影响 |
| 4 Mark threads，Assist current | 保留生产路径对照 |

至少记录：

- `main/helper/assist processed` 和 Mark 时间；
- 每个 Helper 活跃时间，而不只记录 `configured`；
- Global/Pool 操作与等待；
- Scavenge during Mark 的字节数；
- HTTP throughput、成功/失败请求；
- RSS/retained/active、unswept backlog；
- 运行是否完整退出。

## 11. 本轮边界与遗留问题

本轮完成的是源码审计和采样分析，没有修改 Mark、Helper、Assist、Sweep 或
Scavenge 实现，也没有重新构建 SDK。

当前不能声称：

- Step51 已提高 Mark 性能；已有代表性周期反而显示 8.62% 的全部速率回退；
- 4 个线程能线性扩展；实测只有 1.779x；
- Helper 长期负载不均；活跃样本恰好显示非常均衡；
- 50 微秒等待或 MarkTermination 是首要瓶颈；
- case27 已稳定运行 60 秒；进程仍会在 OOM 后的根枚举路径崩溃。

必须单独处理的遗留项：

1. `FrameInfo::ResolveProcInfo` 地址 `0x18` 崩溃；
2. PID 66399 的 macOS `UNE` 旧挂死进程；
3. 下一轮先加诊断计数，避免同时改多个机制后无法归因；
4. 任何 Runtime 替换后必须用同一 MSGC SDK 重新编译 case27 binary。

## 12. 第一、二批优化实施与验证（2026-07-25）

### 12.1 已实施：低风险热路径优化

本轮按 4.7.1 的固定成本方向完成以下改造：

1. `DrainCurrentBuf()` 每个 trusted-grey 对象只读取一次 `TypeInfo*`，后续
   HasRef/WeakRef/数组布局判断和字段扫描都复用该值。
2. 新增 `ForEachRefFieldGCKnownType()` 及数组、非数组子入口；struct array 的
   component TypeInfo、GCTib 和 element size 移到元素循环外计算。
3. `DiscoverObject()`、`DiscoverManagedObject()` 和 barrier mark winner 返回
   已经完成白对象校验时取得的 `TypeInfo*`，避免赢家再次读取 stateWord。
4. `DrainCurrentBuf()` 的 Release processed 直接取输入 WorkBuf 初始长度；
   Debug 仍逐项计数并校验 trusted-grey contract。
5. `ResolvedMarkTarget` 使用已经解析好的 bitmap word/mask 进行 load/RMW，
   不再重复计算 bitmap 边界。
6. WeakRef 从 audited drain 路径进入专用 trusted deferred enqueue；完整校验
   保留在 Debug。

`DiscoverObject` 的 stale-slot 保护没有删除：
`IsValidObject()` 在当前实现中等价于一次 acquire `GetTypeInfo() != nullptr`，
本轮只是保留并复用这次读取结果。

### 12.2 已实施：修正 WorkBuf producer 不变量

`TracingCollector::EnumAllSurrectedExportRoots()` 在 MSGC 下不再把
`BaseObject*` 直接放入 `rootSet`，改为包装成 `ObjectRef` 后走虚调用
`EnumAndTagRawRoot()`。因此 resurrected export root 与普通 root 一样经过：

```text
地址/slot 合法
  -> white-to-grey mark winner
  -> valid TypeInfo
  -> HasRefField
  -> WorkBuf
```

CopyGC 分支保持原来的直接 root push 行为。MSGC WorkBuf 的 Release 消费者据此
删除重复 HasRef gate；Debug 继续断言每个 entry 满足
`marked + valid + has-ref`。

### 12.3 构建、安装与专项测试

- `MSRegionInfoBatchMarkTest`：resolved bitmap coordinate 接口通过。
- `MSGCKnownTypeWorkBufContractTest.sh`：known-TypeInfo scanner、单次 TypeInfo
  读取和 WorkBuf consumer contract 通过。
- `MSGCDiscoverObjectFastPathContractTest.sh`、post-mark closure、mark stack drain
  和 tail-drain contract 均通过。
- `python3 build/scripts/rebuild_msgc_runtime_sdk.py --skip-clean` 成功构建 Release
  marksweep Runtime，并安装到
  `cangjie_compiler/output/msgc/runtime/lib/darwin_aarch64_cjnative`；安装脚本完成
  archive/dylib 符号核对与 codesign 校验。
- 每轮 case27 都由该 MSGC SDK 重新编译 `benchmark.cj`，没有复用旧 binary。

### 12.4 P160 对象处理速率

P160、c1000、100K、Assist 开、4 个 Mark 总线程、16GB 下得到两个完整长周期：

| 运行 | ConcurrentMark | Helper | Main | Assist | allProcessed | 全部速率 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `20260725_115554...` | 7.372087 s | 47,810,379 | 16,717,324 | 7,011,328 | 71,539,031 | 970.39 万/s |
| `20260725_120316...` | 6.543280 s | 49,382,434 | 17,803,990 | 1,612,800 | 68,799,224 | 1051.45 万/s |

两周期加权：

```text
140,338,255 / 13.915367 s = 1008.51 万对象/s
```

所以本轮可报告的实际区间为约 **970～1051 万对象/s**，加权约
**1009 万对象/s**。相对 Step52 既有两个代表性周期的加权约
1032 万对象/s，本轮没有显示出超出运行波动的净提升，不能宣称性能增益。

### 12.5 验证边界

- P160/c1/100K/1s smoke 正常退出，`benchmark_rc=0`，证明新 Runtime、SDK、
  重新编译的用例和基本 GC 流程可闭环。
- P160/c1000 的长 Mark 周期本身完整结束，但下一轮 OOM GC 在
  `EnumRoots -> GCStackInfo::FillInStackTrace -> FrameInfo::ResolveProcInfo`
  中因空 `MFuncDesc` 访问 `0x18` 而退出 139。
- LLDB 两次稳定抓到同一 faulting thread 和调用链。该签名与本文件 1.4、
  9.5、11 节记录的修改前遗留问题一致，不是
  `DrainCurrentBuf()/ForEachRefFieldGCKnownType()` 的崩点。
- 因此本轮对象/s只取崩溃前已经完整落盘的 ConcurrentMark 周期；不能把这次
  运行描述为“60 秒稳态 benchmark 已通过”。根枚举/栈帧元数据问题仍需单独
  修复。

## 13. RegionInfo 与 Global 队列优化实施验证（2026-07-25）

### 13.1 RegionInfo 直接定位

`ResolveMarkTargetImpl()` 改为调用
`MSRegionInfoArray::GetRegionInfoDirect(addr, regionOffset)`：

```text
heapOffset  = addr - heapStart
regionIndex = heapOffset >> 15
regionOffset = heapOffset & (32KB - 1)
```

small object 的 slot index 直接由 `regionOffset` 计算，不再先构造
`RegionStart()`、重复做 Region 范围判断，也不再走相邻 metadata entry
fallback。LARGE span 仍按 `reserved` 找 canonical base，small object 仍保留
Region type、size class、slot 精确对齐、BUMP allocPtr、white-slot TypeInfo
和 mark bitmap 原子语义。

Release 热路径只执行 direct lookup；Debug 构建额外与
`GetRegionInfoValidated()` 比较并失败即停，从而把“Region 分配坐标必须与
heap-relative 32KB 下标一致”固化成可检查不变量。

### 13.2 Global 队列和 WorkBufPool 访问

本轮完成以下改造：

1. `LFStack::PopWithCounts()` 在一次 mutex 临界区中同时返回领取的 WorkBuf
   和 pop 后精确的 Full/total 数，GC main 不再对同一轮先 Pop、再单独
   `GetCounts()`。
2. epoch-backed Global 队列在 Push/Pop 时发布 packed approximate counts。
   GC main 扫描非空输入后用 lock-free snapshot 观察扫描期间新发布的输出；
   只有进入收敛候选且 snapshot 为零时才用 `GetCounts()` 做一次精确确认。
3. Helper 的 Global 领取改为一次 `AcquireOneBuffer()`，同一把锁内保持
   Full-first、Partial-second，避免 Full miss 后再为 Partial 连续拿第二次锁。
4. `PerPGCWork` 保留最多两个本地空 WorkBuf。drained input 直接进入本地
   cache；空 cache 通过 `WorkBufPool::GetN()` 批量补充，溢出和周期结束通过
   `PutChain()` 批量归还。
5. approximate count 只用于 helper 扩展、assist backlog 和普通轮次调度；
   GC 正确性与最终收敛仍由 exact count、in-flight 状态和 one-way quiesce
   共同决定。

### 13.3 构建与功能验证

- 两个 C++ 单测覆盖 direct Region 坐标、resolved bitmap、PopWithCounts、
  approximate counts、本地空 buffer 复用和 `PerPGCWork` standard-layout。
- 8 个 MSGC contract test 覆盖 Full/Partial lane、helper 领取顺序、
  Discover direct lookup、known-TypeInfo、MarkTermination closure 和诊断计数。
- 第一次 Release 构建暴露 `PerPGCWork` 非 standard-layout：新增 private 数据
  与原 public 数据访问控制不同。修正数据成员访问控制并增加静态断言后，
  runtime Release 构建通过。
- `rebuild_msgc_runtime_sdk.py --skip-clean` 已把当前 marksweep Runtime 安装到
  `cangjie_compiler/output/msgc`，archive/dylib 符号、hash 与 macOS codesign
  校验通过；stdlib/stdx 没有重建。
- case27 副本每轮都用该 SDK 的 `cjc --gc-mode=marksweep` 重新编译。

### 13.4 P160 实测

配置为 macOS aarch64、P160、c1000、100K、16GB、Assist 开、4 个 Mark
总线程。两轮新实现的最长完整 ConcurrentMark：

| 运行 | ConcurrentMark | Helper | Main | Assist | allProcessed | 速率 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `20260725_125910...` | 7.518941 s | 43,515,594 | 14,620,452 | 9,046,016 | 67,182,062 | 893.5 万/s |
| `20260725_130021...` | 7.942598 s | 45,739,732 | 16,747,804 | 8,834,048 | 71,321,584 | 898.0 万/s |

旧两轮加权基线为 1008.5 万对象/s，新两轮约 895.8 万对象/s，表面下降约
11.2%。但是新轮次压测自身平均 CPU 只有 169.5%/184.5%，旧轮次为
203.3%/204.4%；复测时 WindowServer 和音乐应用等后台持续占用约 1.5 个核。
因此当前证据只允许得出：

- 功能路径与 WorkBuf 不变量通过；
- 本轮 A/B **没有证明性能提升**；
- 约 -11% 的观测值同时受主机 CPU 竞争影响，不能直接归因于本次代码；
- 在同一主机负载、同一二进制配置下做 baseline/new 交替多轮前，不应宣称
  Region/队列优化带来净收益，也不应只据当前数据回退正确性改造。

两轮仍在第 7 个 OOM GC 的 `EnumRoots` 以同一 `FrameInfo::ResolveProcInfo`
地址 `0x18` 签名退出 139，与 12.5 的修改前遗留问题一致。此次另修复了
压测脚本的端口探测：`set -o pipefail` 下 `lsof | grep -q` 会因 `lsof`
收到 SIGPIPE 返回 141，错误地把 18230 判为空闲；现在改用无管道的
`lsof -t ... -sTCP:LISTEN`，可正确跳过旧 `UNE` 进程占用的端口。

### 13.5 新旧锁等待复核

为判断 13.4 的表面回退是否由本轮队列改造引入，追加了两组 1ms
线程采样：

```text
冷启动到首个长 Mark：
.tmp/step53-lock-profile/sample_current_early.txt

延迟到 9.465793s 长 ConcurrentMark 内：
.tmp/step53-lock-profile/sample_current_delayed2.txt
```

旧实现三个 helper 的 Global 获取 mutex wait 分别占活跃
`RunMarkWorker` 样本的 8.0%、6.2%、6.8%；新实现首轮样本下降到约
1.65%、1.28%、1.10%，延迟长 Mark 样本为约 1.42%、1.68%、2.00%。
GC main 旧实现每 buffer 的 `GetCounts()` wait 为 106/1025，即 10.3%；
新实现延迟样本只剩 `PopWithCounts()` 领取 wait 20/1551，约 1.29%，
没有再次出现逐 buffer `GetCounts()` wait。Pool 归还 wait 在延迟长 Mark
样本中也没有进入热点。

因此可以确认：

- Full-first 单次领取、`PopWithCounts()`、approximate snapshot 和本地空
  buffer cache 实际降低了已知的 Mark 队列/Pool mutex 等待；
- 当前稳态长 Mark 的主要时间仍在
  `DrainCurrentBuf -> TraceObjectRefFieldsToGCWork -> DiscoverObject`，
  不是被最新 Global mutex 改造拖慢；
- `approxCounts_` 的 release/acquire 可能有 cache-line 成本，但当前
  sample 没有证据表明它是 11.2% 表面回退的来源。

冷启动样本同时发现另一类独立问题：160P 写屏障在
`PublishAndSwapLog/EnsureActiveLog -> AllocBufferMS::AcquireWorkBuf
-> WorkBufPool::GetN -> LFStack::PopN` 上形成明显 mutex convoy。
该窗口从进程启动 0.13s 即开始，包含冷 Pool 和早期 GC，不能直接与旧版
第 8 个 GC 的稳态样本做因果 A/B；延迟长 Mark 样本中该路径没有出现。
后续应把它作为“mutator 吞吐/Mark 启动突发”问题单独做同阶段 A/B，
不能用它解释 helper 的稳态扫描速率。

同一当前二进制的完整周期也显示很大状态波动：

| 周期 | ConcurrentMark | allProcessed | 速率 | 状态 |
| --- | ---: | ---: | ---: | --- |
| 首轮长周期 | 4.620170s | 51,215,371 | 1,108.5 万/s | 未进入堆极限 |
| 延迟采样周期 | 9.465793s | 71,613,421 | 756.5 万/s | active Region 接近 16GB，随后大量分配失败 |

这说明同一实现内的 live-set、堆压力、Sweep/Scavenge 和主机调度波动，
足以大于 13.4 的 11.2% 差值。要判断 RegionInfo/队列改造的净收益，仍需
在相同 GC 序号或相近 live-set 下交替 A/B，并将冷启动 Pool convoy 与
稳态 Mark 分开计量。
