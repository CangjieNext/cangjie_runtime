# GC 堆 Pre/Post 校验设计规格

- 版本：v2.0（2026-09-03）
- 状态：待实现
- 目标仓库：`cangjie_runtime_zb`（runtime 为 C++）
- 参考实现：Android Runtime `-Xgc:preverify`/`-Xgc:postverify`（`art/runtime/gc/heap.cc` 的 `PreGcVerification`/`PostGcVerification`/`VerifyHeapReferences`）
- 规范性用语：**必须**（MUST）、**不得**（MUST NOT）、**应当**（SHOULD）

## 1. 概述

仓颉运行时当前没有任何堆校验（heap verification）能力。GC 相关故障发生时，无法区分"堆是在本轮 GC 周期内被破坏，还是进入本轮周期前就已破坏"。

本特性在 GC 周期的两个时点插入堆一致性校验：

- **pre-verify**：本轮 GC 开始、任何标记/转发工作之前；
- **post-verify**：sweep 完成之后、mutator 被释放之前。

两个时点校验**同一组不变量**。配合使用即可完成二分定位：pre 通过而 post 失败 → 破坏发生在本轮 GC 周期内；pre 失败 → 进入本轮前堆已损坏。

## 2. 术语

| 术语 | 含义 |
|---|---|
| GC 周期 | `CopyCollector::RunGarbageCollection` 的一次完整执行 |
| FP | `FinalizerProcessor`（finalizer 处理线程） |
| STW | Stop-The-World，由 `ScopedStopTheWorld` 实现 |
| ghost from-region | 转发后保留的旧 region，内含 FORWARDED 态残留对象 |
| 一跳引用检查 | 校验对象的每个引用字段指向合法堆对象，不做传递遍历 |

## 3. 范围

**范围内**：GC 周期 pre/post 两个校验点；全堆对象 header 与引用字段校验；四类根集校验；失败报告（含 FP 活动证据）；环境变量开关。

**范围外**（v1 明确不做，见 ADR-9）：mark bitmap 一致性校验；传递可达性闭包；mutator 栈根校验；TypeInfo 深度合法性校验；采样/降开销机制；pre-sweeping 第三校验点。

## 4. 功能规范

### 4.1 启用方式

- 运行时**必须**支持环境变量 `cjPreVerify` 与 `cjPostVerify`，二者独立控制 pre-verify 与 post-verify。
- 默认值：**关闭**。
- 取值 `"1"`、`"true"`、`"on"`（大小写不敏感）视为开启，其余视为关闭。
- 开关**不得**依赖 `MRT_DEBUG` 编译宏：任意 build（含 release）均可启用。
- 读取时机：首次使用时 `std::getenv` 读取并缓存为进程级常量，后续 GC 周期不再重复解析。

### 4.2 校验时点

- pre-verify **必须**在本轮 GC 的任何标记、转发、回收工作开始之前执行。
- post-verify **必须**在 sweep（`CollectSmallSpace`）完成之后、`collectorResources.NotifyGCFinished`（释放等待 GC 完成的 mutator）之前执行。
- 两个校验点的执行期间**必须**持有 STW（`ScopedStopTheWorld`），全程暂停包括 FP 在内的所有 mutator。
- 校验**必须**只读：不得修复、不得改写任何对象或 GC 元数据，只允许报告。

### 4.3 校验内容

对**全堆每个对象**（`Heap::ForEachObj`，含 large/pinned/raw-pointer region 中的对象）：

1. 对象地址在堆范围内（`Heap::IsHeapAddress`）；
2. header 合法：`BaseObject::IsValidObject()` 为真（TypeInfo 指针非空）；
3. StateWord 状态码合法（规则见 §6.3）；
4. 若 `HasRefField()`，对每个引用字段执行一跳检查（规则见 §6.2）。

对**四类根集**（静态根、export 根、并发根、FP 根）的每个根：执行与引用字段相同的一跳检查（§6.2）。

### 4.4 失败语义

- 单次校验**必须**完整跑完（不得在首个失败处中断），统计失败总数。
- 失败总数 > 0 时：输出逐条失败明细（§8.1）与 FP 活动证据（§8.2）后，**必须**以 fatal 中止进程。
- 校验通过时**应当**输出一行 REPORT 日志（tag、对象数、根数、耗时）。

## 5. 架构

### 5.1 调用流与插入点

```
CopyCollector::RunGarbageCollection (CopyCollector.cpp:38)
  │  ScopedSTWLock stwLock (line 43)          ← 独占"发起 GC"权利，非 STW
  ├─ PreGarbageCollection(true) (line 47)
  │     └─ TracingCollector::PreGarbageCollection (TracingCollector.cpp:653)
  │           ├─ [NEW] fpSnapshot = FP::GetFinalizedCount() 等   ← FP 证据快照，最先取
  │           ├─ #if MRT_DEBUG: DumpBeforeGC() (:674)
  │           ├─ [NEW] if (HeapVerifier::IsPreVerifyEnabled()):
  │           │     ScopedStopTheWorld stw("pre-verify")
  │           │     HeapVerifier("pre-GC", fpSnapshot).Verify()   ← 门控读的是上一轮位图
  │           ├─ [NEW] ForwardDataManager::GetForwardDataManager().UnbindPreviousLiveInfo()
  │           │     （从上一轮 DoGarbageCollection 末尾移来：pre-verify 之后、
  │           │      本轮任何标记工作之前，见 §5.2 约束 4）
  │           ├─ SATB init / threadpool setup (:660-668)
  ├─ DoGarbageCollection() (line 54)
  │     └─ WCollector::DoGarbageCollection (WCollector.cpp:527)
  │           TraceHeap（内含 :390 ProcessFinalizers → fp.Notify() 唤醒 FP）
  │           PostTrace → Preforward → ForwardFromSpace
  │           → TransitionToGCPhase(IDLE) → CollectSmallSpace (sweep)
  │           → FlipTagID + ForwardDataManager::SetTagID
  │           （UnbindPreviousLiveInfo 已从这里移走，见 §5.2 约束 4）
  └─ PostGarbageCollection(gcIndex) (line 60)
        └─ TracingCollector::PostGarbageCollection (TracingCollector.cpp:679)
              ├─ TransitionToGCPhase RECLAIM_SATB_NODE (:682)
              ├─ SatbBuffer::ReclaimALLPages (:683)
              ├─ PagePool::Trim (:684)
              ├─ [NEW] if (HeapVerifier::IsPostVerifyEnabled()):
              │     ScopedStopTheWorld stw("post-verify")
              │     HeapVerifier("post-GC", fpSnapshot).Verify()
              ├─ collectorResources.NotifyGCFinished (:685)  ← 释放 mutator
              └─ #if MRT_DEBUG: DumpAfterGC() (:688)
```

（行号为写作时锚点，实现时以当前代码为准。）

### 5.2 顺序约束

1. **post-verify 位于 `NotifyGCFinished` 之前**：该调用释放等待 GC 的 mutator，之后堆恢复并发写，无法安全扫描。
2. **post-verify 位于 FP 唤醒之后**：FP 主唤醒（`fp.Notify()`）发生在 sweep 前的 `ProcessFinalizers`，post-verify 时 FP 可能已执行过部分 finalize（含 resurrection）。此为接受的行为（见 ADR-7），快照一致性由 STW 保证（STW 暂停包括 FP 在内的所有 mutator）。
3. **堆遍历使用 `ForEachObj(visitor, /*safe=*/false)`**：`ForEachObjSafe` 内部自带 STW（`RegionManager.cpp:532-537`），在已持 STW 的调用点会嵌套死锁。此组合照抄现有 `DumpHeap` 先例（`TracingCollector.cpp:589-595`：断言 `WorldStopped()` + `ForEachObj(v, false)`）。
4. **位图门控依赖 `metadata.liveInfo` 处于绑定状态，`UnbindPreviousLiveInfo` 从本轮末尾（`WCollector.cpp:543`）推迟到下一轮 `PreGarbageCollection` 的 pre-verify 之后执行**：
   - post-verify（`PostGarbageCollection` 内，位置不变）读取本轮位图——unbind 尚未发生，位图仍绑定；
   - pre-verify（`PreGarbageCollection` 内，位置不变）读取上一轮位图——unbind 在其之后才执行，语义恰为"上一轮结束时是否存活"；
   - 安全性：`metadata.liveInfo` 的全部读者（`IsSurvivedObject` 各调用点：sweep/forward/compaction，`RegionManager.cpp:148/659/704/1071/1109/1131`）均在本轮 GC 内部、位于两个 verify 之后执行；周期间无读者。`CheckAndClearLiveInfo` 的指针相等性检查（`RegionInfo.h:727`）保证延后 unbind 不会误清已复用 region 的新绑定；首个 GC 周期时前一槽为空，循环零迭代，天然安全。
   - 该改动使 hook 位置（`TracingCollector::PreGarbageCollection` / `PostGarbageCollection`）保持 §5.1 原状，GC 流程仅移动一行调用时序。
5. **verify 走堆与 FP 元数据作业的握手（verifyRequested / metadataJobActive，ADR-11）**：FP 线程（"gc-helper"）的两个元数据改写作业（`ReclaimHeapGarbage → ReclaimGarbageRegions`、`FeedHungryBuffers → AllocateThreadLocalRegion → InitRegion`）以纯 C++ 执行、无 STW 协作点，`ScopedStopTheWorld` 无法停止其执行；与 verify 的 `ForEachObjUnsafe` 全地址空间遍历并发会改写 region/unit 元数据（实测 SEGV，`GetRegionInfoAt` 解引用 SUBORDINATE-with-null-owner 中间态）。协议（两侧 seq_cst store-then-load，Dekker 式互见保证，无"启动于走查期间"窗口）：
   - verify：先置 `verifyRequested`，再等 `metadataJobActive` 为假——**只等"正在执行"的作业**；随后取 STW 复查、走查、清除标志。3s 内 in-flight 未清则跳过并打 ERROR（安全阀）。
   - FP：启动元数据作业前 `TryEnterMetadataJob()`——先置 `metadataJobActive` 再查 `verifyRequested`，撞上则退避（`Wait(1)` 重试），否则执行作业、退出时清除。FP 的调度行为（2s 定时唤醒、bool 通知合并、pass 次数）**零改变**。
   - **pending 不阻塞**：`shouldReclaimHeapGarbage` 是 bool 提示（1522 次 store 仅 ~40 次 pass 执行，合并无损），FP 未拾取前不写任何元数据。历史教训：v1 门控等待该 flag 清零（= 唤醒定时 + pass 端到端），把 GC 饱和用例的周期串行化到 ~2s/轮（gccount_01：761 周期 × 747 次 ~2s 等待 ≈ 1502s，总 1540s）；握手版同用例 109.6s、2123 周期 4246 次 verify 全覆盖、残余停顿 41 次 24.3s（in-flight 等待）。

## 6. 校验规则

### 6.1 对象级规则（VerifyObject）

按序执行，前序失败仍继续后续对象的校验：

1. `Heap::IsHeapAddress(obj)` 为假 → 失败（遍历器自身异常）。
2. `obj->IsValidObject()` 为假 → 失败，且**不得**再对该对象做引用字段遍历（header 已不可信）。
3. 状态码检查（§6.3）失败 → 失败，仍可做引用遍历（StateWord 与字段区独立）。
4. **对象位于 ghost from-region（转发尸体）→ 到此为止，跳过引用字段遍历**。理由：尸体是被本轮 GC 回收的垃圾，其字段值是转发前的陈旧快照，可合法指向已死/未路由对象（`ForEachObjUnsafe` 会遍历到 ghost region）。与 ART 先例一致（`heap.cc:3328`："We don't want to verify the objects in the live stack since they themselves may be pointing to dead objects"）。
5. **位图门控：`Heap::IsSurvivedObject(obj)` 为假（floating garbage）→ 跳过引用字段遍历，header+状态码检查保留**。理由：exempted region（垃圾量低于 `exemptionThreshold` 被豁免搬迁，`RegionManager.cpp:479-492`）里的未标记对象（floating garbage）不参与本轮 trace，其字段是转发前的陈旧快照——tag 位只有 1 bit，跨代回绕后陈旧 tag 冒充 current tag；其目标可为已死对象（路由表对未标记对象无意义：`GetPreLiveBytes` 是活字节前缀和，死对象查询产生游程碰撞或触发 `LiveInfo.cpp:20` CHECK）。生产路径不读垃圾对象的字段（可达性前置过滤：活持有者每轮被 trace、引用至多滞后一代），verify 通过位图门控复制该过滤以继承其安全论证。header 是局部不变量（描述对象自身内存，不依赖路由/位图），对一切可遍历对象始终检查。
6. `HasRefField()` 为真 → `ForEachRefField` 逐字段进入 §6.2。

已知覆盖缺口（接受并记录）：本轮标记开始后新分配的对象（`MarkNewObject` 覆盖 ENUM/TRACE 期分配；TRACE 完成后分配的对象在 fresh region 无位图）在 post-verify 中仅获 header 检查——下一轮 pre-verify 起其引用被覆盖。FP 处理中的 finalizable 对象（未标记但保活）同样仅 header 检查；其引用字段的实际持有者（FP 根与 resurrect 闭包）另行覆盖。

### 6.2 引用目标规则（VerifyReference 与根检查共用）

引用字段**必须按 idle 期读语义解析**（复刻 `IdleBarrier::ReadReference` 的只读版，`IdleBarrier.cpp:19-40` + `TryUpdateRefFieldImpl`，`WCollector.cpp:57-96`）：

1. 字段未打 tag → 目标 = `GetTargetObject()` 直读；
2. 字段打了 tag（**不分 current/old era**）→ 目标 = `collector.FindToVersion(GetTargetObject())`；返回 null（不可路由）→ 回退目标 = 裸地址（对应 `TryUntagRefField` 回退语义）。

tagged 引用在周期间合法地持有转发前地址（lazy resolution 设计：`FlipTagID` 后由绑定了 current tag 的转发表解析，`WCollector.cpp:539-540`）。**禁止**重打 tag 或 CAS 写回（`GetAndTryTagRefField`/`TryUpdateRefField` 的写半边）——verify 只读。

> 注意：不得采用 ENUM 期语义（`IsCurrentPointer` → 直读）——那是周期内 tracing 的规则；周期间（pre/post-verify 时点）current-tagged 同样需要经转发表解析。`FindToVersion` 对"活对象持有指向不可路由目标的 tagged 引用"会触发 GC 内部 CHECK（`LiveInfo.cpp:20`）中止——这与生产读路径（idle barrier）的行为一致，属 fail-fast 检测，可接受。

对解析后的目标 `ref`：

| 条件 | 判定 |
|---|---|
| `ref == nullptr` | 合法，跳过 |
| `!Heap::IsHeapAddress(ref)` | 合法，跳过（常量字符串等非堆目标；与 collector 自身标记路径的规则一致，`TracingCollector.cpp:725-728`） |
| 在堆范围内 | **必须**满足：`theAllocator.IsHeapObject(ref)`（精确判定）为真；`ref->IsValidObject()` 为真；状态码符合 §6.3。任一不满足 → 失败 |

> 实现注记：`Allocator::IsHeapObject` 在本代码库中仅于 `MRT_DEBUG` 下声明（`Allocator.h:69-71`），且当前实现等价于 `IsHeapAddress`（`RegionSpace.cpp:278-283`）。因此该精确判定仅 debug build 执行（与库内既有调用方一致）；release build 退化为 range + header + 状态码三查，当前无检测能力损失。若未来 `IsHeapObject` 升级为真精查，debug build 自动受益。

### 6.3 状态码规则

StateWord 的 ObjectState（`StateWord.h:21`）：

| 状态 | 判定 |
|---|---|
| `NORMAL` | 合法 |
| `LOCKED` | 合法（mutator 持锁中被 STW 冻结属正常） |
| `FORWARDED` | 仅当对象位于 ghost from-region 时合法，否则失败 |
| `FORWARDING` | 仅当对象位于 ghost from-region 时合法，否则失败（正常转发流程在 verify 时点不应在途） |

### 6.4 根集（VerifyRoots）

v1 覆盖四个具备稳定访问 API、可在 STW 下直接调用的根集：

| 根集 | 访问接口 |
|---|---|
| 静态根 | `Heap::VisitStaticRoots(RefFieldVisitor&)` |
| export 根 | `ExportRootTable::VisitGCRoots(RootVisitor&)` |
| 并发根 | `Runtime::Current().GetConcurrencyModel().VisitGCRoots(&visitor)` |
| FP 根 | `FinalizerProcessor::VisitGCRoots(RootVisitor&)` |

每个根的目标按 §6.2 判定。mutator 栈根不在 v1 范围（见 ADR-9）。

## 7. 组件设计

### 7.1 新文件 `runtime/src/Heap/HeapVerifier.{h,cpp}`

```cpp
class HeapVerifier {
public:
    static bool IsPreVerifyEnabled();    // cjPreVerify，§4.1 语义
    static bool IsPostVerifyEnabled();   // cjPostVerify，§4.1 语义

    HeapVerifier(const char* tag, const FpSnapshot& fpSnapshot);
    void Verify();       // §4.3 全流程 + §4.4 失败语义

private:
    void VerifyObject(BaseObject* obj);                  // §6.1
    void VerifyReference(BaseObject* holder, RefField<>& field);  // §6.2
    void VerifyRoots();                                   // §6.4
    void RecordFailure(const char* fmt, ...);             // 明细日志 + failCount_++

    const char* const tag_;      // "pre-GC" / "post-GC"
    const FpSnapshot fpSnapshot_;
    size_t failCount_ = 0;
    size_t objCount_ = 0;
    size_t rootCount_ = 0;
    uint64_t startTime_ = 0;
};
```

调用点（两处，见 §5.1）：

```cpp
if (HeapVerifier::IsPreVerifyEnabled()) {     // post 同理
    ScopedStopTheWorld stw("pre-verify");
    HeapVerifier("pre-GC", fpSnapshot).Verify();
}
```

### 7.2 FinalizerProcessor 增量（FP 活动证据的数据源）

新增四个原子字段（ relaxed 序即可，仅用于诊断报告）：

| 字段 | 语义 | 更新点 |
|---|---|---|
| `std::atomic<uint64_t> finalizedCount` | 累计已完成 finalize 数 | `ProcessFinalizableList` 每完成一个 +1 |
| `std::atomic<uint64_t> enqueuedCount` | 累计入队 finalizables 数 | `EnqueueFinalizables` 内每移动一个 +1 |
| `std::atomic<FpState> fpState`（IDLE/PROCESSING） | FP 当前是否在 finalize 执行中 | `ProcessFinalizableList` 进入/退出时设置 |

GC 周期开始时（`PreGarbageCollection` 最开头、pre-verify 之前）读取 `finalizedCount`/`enqueuedCount` 二者存为 `FpSnapshot`，供本周期 pre/post 两个 verify 实例共用；`fpState` 不做快照——`FP mid-run` 语义为"STW 冻结时 FP 是否正在 finalize 中"（§8.2），故在失败报告时点实时读取。

## 8. 失败报告

### 8.1 逐条明细

每个失败**必须**立即输出一条 ERROR 日志，字段：tag（pre-GC/post-GC）、失败对象/holder 地址、holder 的 TypeInfo 名、字段偏移（引用检查时）、目标地址、失败原因（哪条规则、实际值）。

pre-verify 失败的报告**必须**注明：堆在本轮 GC 开始前已损坏（归因于上一轮 GC、mutator 或 FP 的周期外执行）。

### 8.2 FP 活动证据

post-verify 失败时**必须**输出 FP 活动证据行：

```
[post-GC] heap verify failed with 3 failures
FP activity this cycle: finalized 2 (from 5 to 7), enqueued 4, FP mid-run: yes
```

（输出顺序：逐条明细 → FP 证据行 → fatal 汇总；pre-verify 失败时在明细后另输出"heap was already corrupted BEFORE this GC cycle started"注记行。）

- `finalized` = 周期内 `finalizedCount` 增量；为 0 时基本排除 FP 嫌疑，指向 GC
- `enqueued` = 周期内 `enqueuedCount` 增量
- `FP mid-run` = STW 冻结时 `fpState == PROCESSING`

### 8.3 中止方式

`CHECK_DETAIL(false, "<tag> heap verify failed with %zu failures", failCount_)`——对齐 collector 对 invalid object 的 fatal 先例（`WCollector.cpp:154`）。

## 9. 验收标准

运行时无 C++ 单测基建，验收以仓颉程序 + 环境变量进行：

| # | 项目 | 通过标准 | 状态 |
|---|---|---|---|
| 1 | 冒烟：`cjPreVerify=1 cjPostVerify=1` 跑 helloworld | 无 fatal；每次 GC 出现两行 verify passed REPORT 日志 | ✅ |
| 2 | GC 压力：含 finalizer/弱引用/大数组的程序 + 双开关 + `HeapManager::RequestGC` 强制 GC | 无 fatal | ✅ |
| 3 | 阴性：临时注入损坏（改坏一个引用目标/涂改一个 header） | verify fatal，明细含正确地址/偏移/原因；验毕撤销注入 | ✅ |
| 4 | FP 证据：finalizer 密集程序 + 注入损坏 | 报告中 `finalized`/`enqueued` 数与实际执行一致 | ✅ |
| 5 | 开关独立性：仅开 pre / 仅开 post | 各自只跑对应校验点 | ✅ |
| 6 | 性能记录：中等堆量一次 pre/post 耗时 | informational，无门限 | ✅（见 ADR-11） |
| 7 | 全量回归：HLT+LLT（71,611 例，verify 双开） | 0 verify fatal；非环境类回归为 0 | ✅（9/7 全量 + 2,179 例补跑 + 14 例握手版收口） |
| 8 | 并发鲁棒性：gc_concurrency 系列（原 10/10 SEGV） | 握手版 3/3 零崩溃 | ✅ |
| 9 | 预算收敛：14 个 verify 造成的失败用例，握手版 + 标准 600s | 全部 PASS（合计 889s，最重 169s） | ✅ |

## 10. 设计决策记录（ADR）

**ADR-1 校验范围 = header + 一跳引用 + 根集（对齐 ART `VerifyHeapReferences`）**
- 理由：覆盖绝大多数 GC bug（漏标/错标/错转发最终都表现为非法引用或非法 header）；遍历全堆对象使传递闭包被隐式覆盖（每个存活对象的每个引用都被检查）。
- 否决备选：仅 header+地址——抓不到引用级错误；传递可达性闭包——开销显著增大而增量收益小。

**ADR-2 不校验 mark bitmap**
- 理由：pre 时点 bitmap 是上一轮的残留快照，"标记 iff 存活"不成立；post 时点 bitmap 已被 sweep 消费，且 FP 并发的 resurrection 使"未标记却存活"合法存在。ART 同样不校验。
- 否决备选：post 校验"未标记对象确被回收"——需区分 from/to region 语义且被 resurrection 干扰，误报率高。

**ADR-3 仅两点（无 pre-sweeping 第三点）**
- 理由：产品要求两点；ART 的 pre-sweeping 点服务于"保护可达对象不被 sweep 回收"的 mark-sweep 语义，仓颉 copying collector 的对应风险已由 post 点覆盖。

**ADR-4 启用 = 环境变量 opt-in，默认关，不依赖编译宏**
- 理由：GC 损坏常只在 release/压力场景复现，校验必须在那里可用；默认关保证零开销退出。
- 否决备选：`MRT_DEBUG` 编译期门控（release 不可用，恰恰是最需要它的场景）；debug build 默认开 post（非预期开销惊吓用户）。

**ADR-5 失败 = 跑完统计后 fatal**
- 理由：堆损坏后继续执行是 UB，中止保留现场；完整跑完收集全部失败，最大化单次复现的诊断信息。对齐 ART（`heap.cc:3575-3579`）。
- 否决备选：仅记日志不中止（损坏后行为不可预测，日志本身不可信）；首个失败即中止（诊断信息最少）。

**ADR-6 结构 = 独立 `HeapVerifier` 类 + flag 直读环境变量**
- 理由：校验逻辑与 collector 职责分离，可独立演进；env 直读照搬 `cjEnableGC` 先例（`Heap.cpp:33-53`），改动面最小。
- 否决备选：内联进 `TracingCollector`（类已很大，校验逻辑耦合难扩展）；走 `GCParam` 结构体接线（为调试特性改动 `Cangjie.h`/`CjScheduler.cpp` 多处，过度工程）。

**ADR-7 post-verify 位置 = FP 唤醒之后、`NotifyGCFinished` 之前；不挪动 `fp.Notify()`**
- 理由：与 ART 相对顺序一致（其 finalizer daemon 同样在 sweep 前被通知）；不改动 GC 主流程；快照一致性由 STW 保证（`VisitAllMutators` 显式包含 FP，`MutatorManager.cpp:245-252`）。代价（post 快照含 FP 已执行部分的影响）由 ADR-2 的"不查 bitmap"消化。
- 否决备选：把 `fp.Notify()` 推迟到 post-verify 之后——FP 有多个唤醒路径（`NotifyToFeedAllocBuffers` 亦调 `Notify()`），只推迟一处不严密；改变时序有 heisenbug 风险；verify 路径与生产路径不一致。

**ADR-8 FP 归因 = 证据报告，不做"推迟唤醒"**
- 理由：并发 GC 下归因歧义是固有的（所有 mutator 都在并发写堆），FP 只是嫌疑较大者；证据行（finalized=0 可排除 FP）配合"无 finalizer 复现用例二分"可覆盖实际排查；约十行代码、零流程改动。
- 否决备选：同 ADR-7 否决项。

**ADR-9 v1 边界裁剪：栈根、TypeInfo 深校验、采样均范围外**
- 栈根：枚举复用 ENUM 阶段机制（依赖 alloc buffer/phase 状态机），在 IDLE 相位 STW 下调用有风险；全堆扫描已覆盖堆对象自身完整性。
- TypeInfo 深校验（TypeInfoManager 区间确认）：v1 沿用 `IsValidObject` 现有判定，与 collector 标记路径一致（`WCollector.cpp:154,180,219,259`）。
- 采样：v1 定位纯调试工具，O(n) 全量扫描可接受。

**ADR-10 引用检查采用位图门控 + `UnbindPreviousLiveInfo` 延后；tagged 解析采用 idle 读语义（不可路由回退裸地址）**
- 问题（实测，压力程序第 2 轮 GC 起 post-verify 误报 268+，单开关配置 CHECK 崩溃）：误报持有者全部是 exempted region 中的 floating garbage；其字段携带跨代陈旧 tag（1 bit tag 回绕冒充 current）；目标多为已死对象。
- 根因：verify 的"全堆无差别读"违反了惰性引用修复的隐含契约——生产只解引用可达对象的字段（引用至多滞后一代、目标必被标记、路由必有效）；垃圾对象的字段生产无人读，允许持有任意失效引用。
- 决策：引用检查仅对位图判定存活的对象执行（复制生产的可达性前置过滤）；tagged 引用按 idle 读语义解析（`IdleBarrier::ReadReference` 的只读版：tagged 一律经 `FindToVersion` 查表，不可路由回退裸地址）；为使 post-verify 能读本轮位图、pre-verify 能读上轮位图，`UnbindPreviousLiveInfo` 从本轮末尾延后到下一轮 pre-verify 之后（安全性论证见 §5.2 约束 4）。
- 否决备选 1（post-verify 前移进 `DoGarbageCollection`）：可行但需 hook 进 GC 主流程内部，改动面更大；延后 unbind 一行达成相同数据依赖，hook 保持原位。
- 否决备选 2（不可路由引用静默跳过、不回退裸地址）：漏检活对象合法持有的惰性未解析引用（实测存在该类引用），校验强度受损。
- 否决备选 3（用 `liveInfo0` 做门控）：`liveInfo0` 仅存在于被搬迁成为 ghost 的 region（`PrepareForwardableRegion` 快照），exempted region 根本没有；且其语义是"搬迁轮存活"而非"本轮存活"，跨代 resurrect/死亡变迁中二者分歧，会导致误判。
- 否决备选 4（post-verify 改为 pre-sweep 时点、全部裸读）：放弃 post 语义（需求明确只要 pre/post 两点），且转发阶段（本 collector 最复杂部分）的 bug 要滞后一轮才可见。

## 11. 涉及文件

| 文件 | 改动 |
|---|---|
| `runtime/src/Heap/HeapVerifier.h` / `.cpp` | **新增**：HeapVerifier 类（§7.1） |
| `runtime/src/Heap/Collector/TracingCollector.cpp` | `PreGarbageCollection`：FP 快照 + pre-verify 调用；`PostGarbageCollection`：post-verify 调用（`NotifyGCFinished` 前） |
| `runtime/src/Heap/Collector/TracingCollector.h` | `FpSnapshot` 成员声明 |
| `runtime/src/Heap/Collector/FinalizerProcessor.h` / `.cpp` | `finalizedCount` / `enqueuedCount` / `fpState` 三个原子字段 + getter + 更新点（§7.2） |
| `runtime/src/Heap/WCollector/WCollector.cpp` | `DoGarbageCollection` 末尾移除 `UnbindPreviousLiveInfo` 调用（§5.2 约束 4，移至 `PreGarbageCollection`） |
| runtime 构建脚本（CMake） | 注册新 .cpp |

**ADR-11 verify/FP 握手（metadataJobActive + verifyRequested）取代 pending 排干门控**
- 问题（实测）：v1 门控等待 `shouldReclaimHeapGarbage` 清零。该 flag 是 bool 提示：GC 每`NotifyToReclaimGarbage` 一次 store(true)，FP 以 2s 定时唤醒（`DEFAULT_FINALIZER_TIMEOUT_MS=2000`）拾取并在 pass 末尾清零——761 轮 GC 的通知合并为 ~40 个后台 pass，本无阻塞。门控把"通知到完成"全程当作危险窗口，GC 饱和负载下周期被串行化到 FP 唤醒节拍（gccount_01 761 周期×~2s=1540s，其中 verify 扫描仅 39.1s；bench_finalizers 334 周期×~2s=676s，扫描仅 4.5s）。
- 决策：真正的竞态窗口只有"作业执行中"（纯 C++、无 safepoint）。握手协议（spec §5.2 约束 5）：verify 置 `verifyRequested` 后仅等 `metadataJobActive` 清零；FP `TryEnterMetadataJob` 先置 active 再查 verifyRequested，撞上退避 1ms 重试。seq_cst store-then-load 双检保证无"启动于走查期间"窗口。FP 调度零改变（ADR-7）。
- 实测（去重后；注：框架 case log 对运行时日志双写——实时流 + 结尾 dump，以 `\t`/`\t\t` 前缀区分，统计须按 scope-end 时间戳去重）：gccount_01 1540s→109.6s（verify 关 81.2s，1.35×），2123 周期 4246 次 verify 全覆盖，扫描 62.5s（avg 14.7ms/中位 2.7ms），残余 >0.5s 停顿 41 次 24.3s（in-flight 等待），0 skip；bench_finalizers 676s→9.18s（verify 关 7.2s，1.28×），576 周期 1152 次 verify 全覆盖，扫描 5.6s，残余停顿 5 次 2.8s。安全回归：gc_concurrency 系列（原 10/10 SEGV）3/3 零崩溃；压力程序通过。
- 否决备选 1（`NotifyToReclaimGarbage` 补调 `Notify()` 立即唤醒 FP）：改变生产 FP 行为（合并 pass 退化为逐次、唤醒风暴），违反 ADR-7。
- 否决备选 2（v1 的 pending 排干）：如上，把惰性合并的特性当缺陷，序列化代价 ~2s/轮。

**ADR-11 补记（14 例收口验证，2026-09-08）**：握手版 runtime、verify 双开、标准 600s 预算下，全部 14 个"verify 造成的失败"用例 PASS（合计 889s / 14.8 分钟；最重 test_gccount_03 = 169s，占预算 28%；test_gccount_01 = 112s）。结论：verify 全量回归无需白名单或加长预算；按 180s 标准预算推断亦全部可过（最重余量 11s）。此前 19×/93× 病态放大全部归因于已废弃的 pending 排干门控。
