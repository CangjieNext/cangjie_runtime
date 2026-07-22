# case27 JSON 偶发崩溃 / parse failure 根因排查记录（分配器 UAF 方向）

日期：2026-06-15
关联：[20260615_case27_msgc_diagnostics_and_status.md](20260615_case27_msgc_diagnostics_and_status.md)

## 0. 结论速览

- **现象未根治**：case27（HTTP + 复杂 JSON，c8/p100K/60s）下 MSGC 仍偶发
  `JsonObject.toString` 崩溃（SIGSEGV）与 JSON parse failure。CopyGC 无此问题。
- **已排除**：GC 标记不完整（missed-mark / 屏障 / 根枚举）。mark verifier 全程
  `violations=0`，标记是正确的。
- **已定位方向**：这是**小对象分配器把一个仍然存活的对象的内存复用/覆盖**，
  导致 live `JsonValue` 的 `TypeInfo`/子指针字段被写成别的内容（字符串字节或垃圾），
  在序列化遍历对象树时类型混淆 → 野指针解引用崩溃。属于 **use-after-free / 同槽双分配**
  性质，且对 GC 期的校验不可见（纯 mutator 分配期发生）。
- **本轮顺手修了 2 个真实并发缺陷**（保留，但都不是本问题根因）。
- **根因尚未最终钉死**：本轮新增的 `double-owner` / `double-alloc-slot` 诊断都还**没有**
  先于崩溃命中；“同一个 region 被并发交给两个 P”与“同一 slot 被重复发放”仍然可疑，
  但已从“最强单点结论”降为“仍需继续证伪/细化”的假设。
- **新增高优先级校正**：
  1. `server-body-fail` / `client-response-fail` dump 下来的 body 用 Python 校验都是
     **valid JSON**，说明 parse failure 不是“字节被写坏”；
  2. client 侧 `Unexpected character: 'I'` 实际上是 server 先抛异常后 HTTP 返回
     `Internal Server Error`，不是独立的客户端坏包；
  3. 当前源码里普通 `MObject::NewObject` / `MArray::NewKnownWidthArray` 路径没有直接命中
     `MarkNewObject()`，而 `MarkNewObject()` 在代码里能直接看到的显式调用点主要是
     finalizer 路径；因此“并发 mark 期间 parser/serializer 新建局部对象如何保活”已经升为
     与 allocator/UAF 并列的主线怀疑点。
  4. **新结论（本轮最重要）**：给普通 `NewObject/NewArray` 加诊断 gate 后，已经能稳定观察到
     大量 `GC_PHASE_MS_CONCURRENT_MARK(21)` 期间的新对象在默认路径下
     `markedBefore=0`；但即便开启诊断式 `force-mark`，case27 仍能在
     `JsonObject.toString -> WriteBuffer.appendEscape` 路径上崩溃。
     → “普通新对象并发 mark 保活不完整”是**真实因子**，但**不是全部根因**。
  5. **2026-06-16 进一步校正**：`PreEnumRoots()` 里临时接上
     `ReturnAllAllocBufferRegionsToCentral()` 并放在短 STW 内时，small old-gen mismatch
     可以稳定归零；但把这段 STW 去掉后，central 中又能重新观察到
     “出生在 old-gen active region 上的新对象”。
     → 真正起作用的不只是 `return-all` 本身，**STW 对 flush/gen-flip 窗口的收口也是关键因子**。

## 1. 复现与判定手段（本轮使用）

构建 + 替换 + 重签名（注意：实际构建目录是 `cangjie_runtime_MSGC/runtime`，
不是文档里旧的 `cangjie_runtime/runtime`；产物在
`output/temp/lib/aarch64_Release/libcangjie-runtime.dylib`）：

```bash
cd /Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime
python3 build.py build --gc-mode marksweep
SDK_LIB=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260609/cangjie/runtime/lib/darwin_aarch64_cjnative
cp output/temp/lib/aarch64_Release/libcangjie-runtime.dylib "$SDK_LIB/libcangjie-runtime.dylib"
codesign -f -s - "$SDK_LIB/libcangjie-runtime.dylib"
```

跑对比（在 case27 目录，env 见 README/上一份文档；本轮固定 c8/p100K/60s/heap12gb/proc6）。

> 注意：崩溃发生在 worker 协程，进程崩后会**挂死不退出**（卡住），run_compare 会被堵住；
> 需要手动 `kill -9` 残留 `*/msgc/benchmark` 进程。本轮清理过大量历史残留进程。

## 2. 关键事实链（按时间）

1. **标记正确**：`gc.log` 中 `[MS-GC][VERIFY] mark-completeness: ... violations=0,
   staleManaged=0, staleNonTraceable=0, staleInvalid=0`，每个 GC 周期都为 0
   （单周期 scanned 可达 20–28 万对象）。→ 不是 missed-mark/屏障/根问题。
2. **崩溃栈固定**：
   ```
   SIGSEGV
   #0 stdx.encoding.json assumeCapacity (write_buffer.cj:320/326, 递归)
   #1 JsonObject.toString (json_object.cj:764)
   #2 default.processRequest (benchmark.cj:328  -> root.toString())
   ```
   即修改后的 JSON 树**序列化**时，遍历到一个子节点指针非法。
3. **dump 的失败 body 用 Python 能正常解析**（字节合法）：
   - `case27_server-body-fail_req3404_slot-1.json` 与原始
     `payloads/100K.json` 是 byte-for-byte 一致；
   - `case27_client-response-fail_req3402_slot-1.json` 等“坏响应”文件本身也是 valid JSON。
   → 失败发生在 parser/序列化器的**内部 live 对象 / 解析状态**，不是输入字节坏。
4. **`Unexpected character: 'I'` 不是另一类独立坏包**：
   - 日志上同一批 reqId 里，先出现 server 侧
     `JsonException: ... Unexpected character: '}'`；
   - 紧接着 client 对对应请求报 `Unexpected character: 'I'`；
   - 之前开启 `CASE27_DUMP_BAD_BODY=1` 的 run 中，client dump 的 body 明确是
     `Internal Server Error`。
   → `'I'` 只是 server parse/处理失败后的级联现象。
5. **崩溃地址会变形**，本质都是“某个 live 对象的指针字段被写脏”：
   - 一次 `addr=0x326c6576656c`，小端解码即 ASCII `"level2"`——payload 里的 JSON key。
     说明一个 `JsonValue` slot 被复用、写入了字符串内容 `level2`。
   - 一次 `addr=0x1000000000017`，崩在 `TypeInfo::IsSubType`——TypeInfo 指针被写成
     带高位 tag 的小整数（垃圾）。
   - 早期还见过 `addr=nil`（TypeInfo 被清零）。
   → 都是同一个 live JSON 对象的头/指针被**异构内容覆盖**（type confusion / UAF）。
6. **`BG_SCAVENGE=0` 不影响**：失败照旧 → 不在后台 scavenge/reclaim 路径，在**同步分配/清扫**路径。
7. **早发**：失败 reqId 很小（< 800），压测早期就出现。
8. **仅 MSGC**：CopyGC 全程 0 失败 → 是 MSGC 分配器/清扫相关，而非应用层逻辑或 HTTP 框架。
9. **当前源码里 `CJ_MS_STW_SWEEP` 已不是有效代码分支**：
   - `StopAndWaitForAllSweep() + ForceFinishAllUnswept()` 已经是默认路径；
   - 全仓源码搜索已找不到 `CJ_MS_STW_SWEEP` 的实际判断分支，只剩文档残留。
   → `run_20260615_225903` 那次“跑满 60s 无崩溃”不能再归因到这个 env 开关，
   只能当作一次时序上的负证据。
10. **普通新对象 mark-at-alloc 诊断实验命中了真实缺口**：
    - 新增 gate：
      `CJ_MS_DIAG_NEW_ALLOC_LOG=1`
      `CJ_MS_DIAG_NEW_ALLOC_FORCE_MARK=1`
      `CJ_MS_DIAG_NEW_ALLOC_LOG_LIMIT=<N>`
    - 代码在普通 `MObject::NewObject` / `NewPinnedObject` /
      `MArray::NewKnownWidthArray` 成功后记录对象 phase、region、`markedBefore/After`，
      可选地调用 `MarkNewObject()`。
    - `run_20260615_232758`（c8/p100K/60s）里，`gc.log` 末尾最近一次 summary：
      `total=958453 phase20=65927 phase21=892526 phase22=0 markedBefore=161312 markedAfter=958453 forced=797141`
      。这说明：
      - 绝大部分普通新对象发生在 `phase21`；
      - 其中大多数在默认路径下确实是 `markedBefore=0`；
      - 开 gate 后它们被补成 `markedAfter=1`。
11. **但 force-mark 只是缓解，不是根治**：
    - 同一轮 `run_20260615_232758` 没有 stall，benchmark 跑满 `60.2s`，结果是
      `total=39758 ok=39690 fail=68 throughput=660req/s`；
    - 相比之前“跑挂/卡死”的形态，这是明显改善；
    - 但失败仍未清零，说明问题没有完全解决。
12. **dump + 短窗口复现证明剩余问题仍是 live-state / serializer 方向**：
    - `run_20260615_233034`（c8/p100K/15s，开启 `CASE27_BODY_DIAG=1` +
      `CASE27_DUMP_BAD_BODY=1`）中，进程在 `23:30:40` 仍撞到：
      `Thread "schd-worker" catched unhandled SIGUSR1 (Bus error: 10)`
      栈为：
      `std/core/array_common.cj::__CNat14CPointerHandleIG_E6<init>HPG_RNat5ArrayIG_E`
      → `stdx.encoding.json:WriteBuffer.appendEscape`
      → `jsonWriteWithoutFormat`
      → `JsonObject.toString`
      → `benchmark.cj:processRequest`
    - 同一轮 dump 结论：
      - `case27_server-body-fail_req651_slot-1.json` 与 `payloads/100K.json`
        **byte-for-byte 一致**；
      - `case27_client-response-fail_req651_slot-1.json` 的 body 只有
        `Internal Server Error`；
      - `case27_client-response-fail_req397_slot-1.json` /
        `req1645_slot-1.json` 分别是 `100408/100409` 字节的**valid JSON**，
        Python 可正常解析，但 Cangjie client 仍报
        `The json data is Non-standard, please check`。
    → 剩余问题至少还包含两类：
      1. server 侧 valid request body 偶发 parse fail；
      2. server 侧 serializer / write-buffer / string path 偶发硬崩溃。

## 3. 本轮做的修改（代码）

### 3.1 真实缺陷修复（保留，但不是本问题根因）

1. **lazy/background sweep 与 ClearAllMarkBitmaps 的 Dekker 握手内存序**
   - 文件：`src/Heap/MAllocator/MSCentralRegionPool.cpp`
   - `skipLazySweep_/stopSweepRequested_/bgSweepRunning_` 与 `activeLazySweepCount_`
     之间是 StoreLoad（Dekker）握手，原来只有 acquire/release，无法阻止 StoreLoad 重排；
     理论上可让一个 lazy/bg `SweepRegion` 与 `ClearAllMarkBitmaps` 并发，读到被清空的
     markBitmap 而误清活对象。
   - 修复：在 GetRegion P2/P3、`StopAndWaitForAllSweep`、`BackgroundSweep` 三处加
     `std::atomic_thread_fence(std::memory_order_seq_cst)` 补齐 StoreLoad 顺序。
   - 验证：失败现象不变 → 不是本问题根因（但属于真实缺陷，建议保留）。

2. **`LockFreeRegionStack::Pop` 的 ABA**
   - 文件：`src/Heap/MAllocator/MSCentralRegionPool.h`
   - partialSwept 用的无锁栈用裸 32 位 index 做 CAS，注释里“No ABA risk”是错的：region
     被 pop→复用→重新 push 时，in-flight 的 Pop CAS 会因 index 复原而误成功，把同一 region
     交给两个消费者。
   - 修复：head 改为 64 位 `{tag:32 | index:32}`，每次 Push/Pop 自增 tag。
   - 验证：失败现象不变 → 不是本问题根因（但属于真实缺陷，建议保留）。

### 3.2 诊断埋点（env 开关，默认关闭，不影响正常运行）

1. **`CJ_MS_SWEEP_POISON`**
   - `SweepRegion` 释放 slot 时把头字写成哨兵 `0xDEAD0000DEAD0000`（而非清零）；
     `MarkSweepCollector::DiscoverObject` 若标记时跟到被毒化对象则 FATAL。
   - 文件：`src/Heap/MAllocator/MSRegionInfo.cpp`、`src/Heap/MCollector/MarkSweepCollector.cpp`
   - 结果：哨兵**从未命中**，崩溃地址是 `"level2"`（被复用写入字符串）→ 说明被覆盖的
     slot 已被**重新分配**，毒化头被新对象盖掉；且 GC 期无人引用到毒化对象
     （与 verifier=0 一致）。→ 损坏发生在 GC 之间的 mutator 分配期。

2. **`CJ_MS_DETECT_REUSE_LIVE`**
   - `AllocBufferMS::SlowAlloc` 把 region 设为 active 前，扫描其“待分配（free）slot”，
     若仍存在 valid 对象则 FATAL（覆盖编译器内联 bump 路径，因为它在内联分配之前执行）。
   - 文件：`src/Heap/MAllocator/MSAllocBuffer.cpp`（`CheckRegionFreeSlotsEmpty`）
   - 结果：**未触发**。→ region 在“交付分配”那一刻 free slot 是干净的；损坏发生在
     region 已成为某 P 的 active region、正在被分配的**之后**。

补充校正（以当前代码为准）：

- 当前 MSGC 树里，`Allocator::EnsureThreadLocalBuffer()` 会把
  `ThreadLocalData::buffer` 指向 `AllocBufferMS`，但 `tlRegion_` 固定保持
  `NullRegion()`，故预编译 SDK 里的 LLVM fast-path 会**主动回退到 slow MCC_* 路径**，
  不会在这棵树里直接做内联 bump 分配。
- 代码位置：
  `src/Heap/MAllocator/MSAllocBuffer.cpp`（构造时 `tlRegion_ = NullRegion()`）、
  `src/Heap/MAllocator/MSRegionSpace.cpp`（注释明确说明 fast-path 读取到 NullRegion 后回退）。
- 因此，当前 case27 的 allocator 定位可以优先在 `AllocBufferMS::FastAlloc/SlowAlloc`、
  `MSRegionInfo::AllocBump`、`AllocFromBitmapFast` 这些 C++ 路径加检测，不必先担心“漏掉编译器
  内联 bump 路径”。

3. **`CJ_MS_DETECT_DOUBLE_OWNER`**
   - 在 `AllocBufferMS::activeRegions_[cls]` install / alloc / return 时记录并校验
     region 的当前 owner（buffer 指针 + 线程 tag + sizeClass + 获取来源）。
   - 文件：`src/Heap/MAllocator/MSAllocBuffer.cpp`
   - 结果：在 `result/until_stall_loop/run_20260615_225123/` 这轮复现中，仍旧先出现
     `JsonObject.toString` 的 managed SIGSEGV，但 **没有** 先打出 `DOUBLE-OWNER`。
     → “同一 region 同时挂到两个 active buffer”**不是一个容易直接命中的主路径**。

4. **`CJ_MS_DETECT_DOUBLE_ALLOC_SLOT`**
   - 小对象分配成功时登记 slot；sweep 释放 slot 时删除登记；若同一 slot 在释放前被再次发放，
     立即 FATAL。
   - 文件：`src/Heap/MAllocator/MSRegionInfo.cpp`、`src/Heap/MAllocator/MSAllocBuffer.cpp`
   - 结果：在 `result/until_stall_loop/run_20260615_225707/` 这轮中，benchmark 跑到
     `46.6s / 11043 req / fail=0`，**没有** 打出 `DOUBLE-ALLOC-SLOT` / `DOUBLE-OWNER` /
     `REUSE-LIVE`，最终被外部 `SIGKILL(9)` 终止。
     → 至少在这轮时序下，没有抓到“slot 在 sweep free 前被重复发放”的硬证据；但该诊断有明显
     时序/内存开销，不能单凭这轮就彻底否定 allocator 方向。

5. **`CJ_MS_DIAG_NEW_ALLOC_FORCE_MARK`（普通新对象诊断式强制 mark）**
   - 文件：
     `src/Common/BaseObject.{h,cpp}`
     `src/ObjectModel/MObject.cpp`
     `src/ObjectModel/MArray.inline.h`
     `src/Heap/MCollector/MarkSweepCollector.cpp`
   - 行为：
     - 普通对象/数组分配后，如果 phase 属于
       `MS_MARK_SETUP / MS_CONCURRENT_MARK / MS_MARK_TERMINATION`，
       记录一条 `[MS-GC][NEW-ALLOC-DIAG]`；
     - 开启 `CJ_MS_DIAG_NEW_ALLOC_FORCE_MARK=1` 时，对 `markedBefore=0`
       的对象调用 `MarkNewObject()`；
     - 每轮 GC summary 汇总 `total/phase20/phase21/markedBefore/forced`。
   - 结果：
     - `phase21 + markedBefore=0` 命中量非常大，说明默认路径确实存在
       “并发 mark 期间普通新对象默认不被立即 mark”的事实；
     - 但加 gate 后 case27 只从“容易 stall/崩”退化成“仍有 68 次 parse fail”，
       且短 run 仍能在 `WriteBuffer.appendEscape` 上 Bus error。
   - 结论：
     - 这是**已被证实的缓解因子**；
     - 但不能把它直接当成最终修复提交，当前更适合作为继续缩小范围的诊断实验。

## 4. 当前最强怀疑链（待确认）

region 交付时是干净的，但之后被写脏 → 损坏发生在 region 作为 active 区被使用的窗口里。
当前仍可疑，但已不再是唯一最高优先级的假设：**同一个 region 被同时交给两个 P 做 bump 分配**。
`MSRegionInfo::AllocBump()` 是非原子的（`allocPtr += slotSize; freeSlotCount--;`），
`AllocFromBitmapFast()` 同理（非原子 `allocBitmap[w] &= ~bit`）。若两个 P 并发从同一 region
分配，会把**同一个 slot 发给两个对象**：一个是 live `JsonValue` 树节点，另一个是字符串
`"level2"`，后者覆盖前者 → 序列化遍历到该节点时类型混淆崩溃。

我现有的 `CheckRegionFreeSlotsEmpty` 检测不到这种情况：双方在各自 install 时刻看到的都是
“干净”，随后才并发分配冲突。

可疑的“同一 region 交给两个 P”的来源（需逐一审）：
- 线程本地 region cache：`TakeCachedRegion` / `FillCache` / `BackgroundFillCache`。
- empty-region 本地缓存：`DrainLocalEmptyRegions` 与全局 `emptySweptCache_` 的交互。
- `RefillFromCentral` 与并行 take-region 路径（最近“takeregion 并行化”改动）。
- lazy sweep 把 region 返回时与另一个 P 取用之间的竞态。

补充修正（基于本轮新证据）：

- 目前更像是**两层问题并存**：
  1. `JsonObject.toString` 的硬崩溃路径，仍然和 allocator / sweep 并发破坏高度相关；
  2. 纯 JSON parse failure 路径，当前更像是**valid JSON 上的 parser/live-state 破坏**，
     未必能完全由“同槽双分配”解释。
- 一个新的具体怀疑点是：普通新对象在并发 mark 期间的保活链路可能不完整。
  代码上能直接看到：
  - `src/ObjectModel/MObject.cpp` 的普通 `NewObject()` / `src/ObjectModel/MArray.inline.h`
    的 `NewKnownWidthArray()` 只是 `Allocate + SetClassInfo`；
  - `src/Heap/MCollector/MarkSweepCollector.cpp` 虽然实现了 `MarkNewObject()`，
    但源码里能直接命中的显式调用点主要是 `BaseObject::OnFinalizerCreated()`。
  如果 parser / serializer 在 `GC_PHASE_MS_CONCURRENT_MARK` 期间创建的局部
  `JsonObject/JsonArray/StringBuilder/ArrayList` 仅存在于当前栈和临时变量里，而没有及时进入
  barrier / root / deferred queue，就可能在后续阶段表现为“valid JSON 解析失败”或对象图错乱。

## 5. 2026-06-16 新分配对象 sweep/gen 诊断结果

本轮针对用户提出的两个问题专门加了诊断：

1. 新分配对象**有没有在本轮被 sweep 掉**；
2. 新分配对象**是不是落在了错误的 sweep generation**。

使用的开关与运行：

- 结果目录：
  `MSGC_RESULT/Result/case27_http_complex_json/result/diag_new_alloc_gen_20260616_001927/`
- 负载：
  `payload=100K`、`concurrency=8`、`duration=15s`
- 开关：
  `CJ_MS_DIAG_NEW_ALLOC_TRACK_SWEEP=1`
  `CJ_MS_DIAG_NEW_ALLOC_TRACK_LIMIT=1048576`
  `CJ_MS_DIAG_NEW_ALLOC_LOG=1`
  `CJ_MS_DIAG_NEW_ALLOC_LOG_LIMIT=8`
- **未开启** `CJ_MS_DIAG_NEW_ALLOC_FORCE_MARK`，即观测的是默认行为。

### 5.1 “本轮被 sweep 掉”这一问：这轮没有抓到

- 本轮 benchmark 正常退出：
  `total=8113 ok=8112 fail=1 throughput=502req/s`
- `gc.log` 中**没有任何**
  `[MS-GC][NEW-ALLOC-DIAG][SWEPT]`。
- 每轮 `sweep-track` summary 也都是：
  `swept=0 sweptGenMatch=0 sweptGenMismatch=0`

结论：

- **在这轮 run 中，没有抓到“普通新分配对象在同一轮 GC 里被 sweep 掉”的直接证据。**
- 因此，当前不能把 case27 的主症状直接定性为
  “new object same-cycle sweep”。

### 5.2 “不属于本轮 gen”这一问：已经明确命中

`gc.log` 最后一轮 summary：

```text
total=9674 phase20=901 phase21=8773
markedBefore=1843 markedBeforeZero=7831
genMatch=6892 genMismatch=2782
phase21GenMismatch=2311
markedBeforeZeroGenMismatch=939
```

进一步把 `genMismatch` 按 region 类型分桶：

- `1843` 条是 `regionType=4` 的 large object / large array，
  `objectSweepGen=0`，这部分更像 large-region 元数据没有参与 small-region
  `sweepGen` 协议，**不应直接和 small-region 的 gen 语义等价解读**；
- **真正关键的是剩下的 `939` 条 non-large mismatch**：
  - 全部发生在 `regionType=1/2`（BUMP / PARTIAL）；
  - 全部是 `markedBefore=0`；
  - 全部满足 `objectSweepGen = currentSweepGen - 2`；
  - 也就是：**对象分配在上一轮 gen 的 small region 上。**

典型样例：

```text
phase=21 obj=... regionType=1 sizeClass=1/3/4/30
objectSweepGen=58 currentSweepGen=60 genMatch=0
markedBefore=0 markedAfter=0
```

结论：

- **“新分配对象落在 old gen” 这一点已经被正面证实。**
- 而且不是极个别噪声，small/pinned 路径上至少已经命中 `939` 次。

### 5.3 当前最可疑源码链：active alloc-buffer region 跨过了 gen flip

这轮证据和源码能直接对上：

1. `gc.log` 的 `preGCMark` snapshot 一直显示：

```text
alloc-buffer regions: total=129/131/154... active=同值 cached=0
```

也就是说 GC 开始前**几乎全是 active region，没有 cached region**。

2. `MarkSweepCollector::PreEnumRoots()` 当前只做了：

```cpp
centralPool->DrainAllAllocBufferCachedRegions();
centralPool->ReclassifyUnsweptToSwept();
centralPool->PrepareForSweep();
```

也就是**只 drain cached，不处理 active**。

3. `CentralRegionPool::PrepareForSweep()` 注释写的是：

```cpp
FlushAllAllocBuffers returns TL regions to swept.
```

但当前实际调用链里，并**没有**在 `PreEnumRoots()` 触发
`AllocBufferMS::ReturnAllRegionsToCentral()`。

4. `AllocBufferMS::ReturnAllRegionsToCentral()` 明明已经实现好了，会把
`activeRegions_[cls]` 和 cached region 一起回收到 central，并在
`ReturnRegionToCentral()` 时打上当前 `sweepGen`。

因此，当前非常像下面这个问题：

- GC 周期切换时，`PrepareForSweep()` 把全局 `sweepGen` 从 `sg` 翻到 `sg+2`；
- 但 mutator 手里已经 install 的 `activeRegions_[cls]` 没有被 flush / retag；
- 后续 phase20/21 的新对象继续从这些 old-gen active region 上分配；
- 于是出现本轮观测到的大量
  `objectSweepGen = currentSweepGen - 2`。

这已经比“泛泛怀疑 allocator/UAF”更具体，当前应把它提升为**第一优先级**问题链。

## 6. 2026-06-16 full-flush 诊断实验结果：small old-gen mismatch 已直接归零

按上面的怀疑链，本轮临时把 `AllocBufferMS::ReturnAllRegionsToCentral()` 接到了
`PreEnumRoots()` 的诊断分支里，并且把：

- `StopAndWaitForAllSweep()`
- `ClearAllMarkBitmaps()`
- `ReturnAllAllocBufferRegionsToCentral()`
- `PrepareForSweep()`（gen flip）

放进了**同一个短 STW 窗口**，避免 mutator 在 flush 和 gen flip 之间再拿到旧代际 region。

使用的开关与运行：

- 结果目录：
  `MSGC_RESULT/Result/case27_http_complex_json/result/preenum_return_all_20260616_003801/`
- 负载：
  `payload=100K`、`concurrency=8`、`duration=15s`
- 开关：
  `CJ_MS_DIAG_PREENUM_RETURN_ALL_REGIONS=1`
  `CJ_MS_DIAG_NEW_ALLOC_TRACK_SWEEP=1`
  `CJ_MS_DIAG_NEW_ALLOC_TRACK_LIMIT=1048576`
  `CJ_MS_DIAG_NEW_ALLOC_LOG=1`
  `CJ_MS_DIAG_NEW_ALLOC_LOG_LIMIT=8`

### 6.1 实验行为本身生效了

`gc.log` 每轮都能看到：

```text
PreEnumRoots: full alloc-buffer flush activeBefore=101 cachedBefore=0 returned=101 activeAfter=0 cachedAfter=0
...
PreEnumRoots: full alloc-buffer flush activeBefore=164 cachedBefore=0 returned=164 activeAfter=0 cachedAfter=0
```

中间还出现过一轮：

```text
activeBefore=162 cachedBefore=103 returned=265 activeAfter=0 cachedAfter=0
```

这说明这个实验分支确实把 alloc-buffer 手里的 `active + cached` region 都清空了，不是“看起来接上了，实际上没跑到”。

### 6.2 关键结果：939 条 small old-gen mismatch 直接归零

baseline（未 full-flush）的最终 summary：

```text
total=9674 phase20=901 phase21=8773
markedBefore=1843 markedBeforeZero=7831
genMatch=6892 genMismatch=2782
phase21GenMismatch=2311
markedBeforeZeroGenMismatch=939
```

full-flush 实验后的最终 summary：

```text
total=8660 phase20=1009 phase21=7651
markedBefore=1295 markedBeforeZero=7365
genMatch=7365 genMismatch=1295
phase21GenMismatch=1144
markedBeforeZeroGenMismatch=0
```

进一步按 regionType 分桶后：

- `small/pinned non-large mismatch: 939 -> 0`
- 剩余 `1295` 条 mismatch **全部是 `regionType=4` large object**
- 本轮仍然**没有**抓到 `[NEW-ALLOC-DIAG][SWEPT]`

也就是说：

- **small object / pinned object 落在 old gen region 上分配**这条链已经从“强怀疑”升级成了**被实验正面证实**；
- 而且问题点就收敛在 alloc-buffer 的 active region 没有随 gen flip 一起处理。

### 6.3 业务现象也同步改善

- baseline（上一轮 gen 诊断）：
  `total=8113 ok=8112 fail=1 throughput=502req/s`
- full-flush 实验：
  `total=8380 ok=8380 fail=0 throughput=508req/s`

这说明这 939 条 small old-gen mismatch 并不只是“统计噪声”，而是和 case27 的用户态异常是同方向相关的。

### 6.4 当前可以收敛出的结论

1. **“新对象有没有在本轮被 sweep 掉”**：
   这两轮都没有抓到直接证据，`[SWEPT]` 一直是 0。
2. **“新对象是不是拿到了不属于本轮的 gen”**：
   对 small/pinned 路径已经可以明确回答“是”，而且根因就是 active alloc-buffer region 跨过了 gen flip。
3. **large object 仍有 `objectSweepGen=0` mismatch**：
   这部分在 full-flush 后仍然保留，说明它和 TL active region 不是同一个问题，后续应单独分析 large-region 的 `sweepGen` 语义，而不要把它混进 small-path 根因里。
4. **这个实验还不是最终修复**：
   当前实现依赖 `CJ_MS_DIAG_PREENUM_RETURN_ALL_REGIONS=1` 和短 STW，只适合作为诊断/验证结论，不适合作为最终默认策略直接提交。

### 6.5 五轮稳定性复查（2026-06-16）

- 结果目录：
  `MSGC_RESULT/Result/case27_http_complex_json/result/preenum_return_all_5runs_20260616_093233/`
- 负载保持不变：
  `100K / c8 / 15s / heap12gb / proc6`
- 开关保持不变：
  `CJ_MS_DIAG_PREENUM_RETURN_ALL_REGIONS=1`
  `CJ_MS_DIAG_NEW_ALLOC_TRACK_SWEEP=1`
  `CJ_MS_DIAG_NEW_ALLOC_TRACK_LIMIT=1048576`
  `CJ_MS_DIAG_NEW_ALLOC_LOG=1`
  `CJ_MS_DIAG_NEW_ALLOC_LOG_LIMIT=8`

5 轮结果：

- `run_01`: `total=8262 ok=8262 fail=0 throughput=504req/s`
- `run_02`: `total=10208 ok=10208 fail=0 throughput=627req/s`
- `run_03`: `total=9924 ok=9924 fail=0 throughput=615req/s`
- `run_04`: `total=9658 ok=9658 fail=0 throughput=593req/s`
- `run_05`: `total=9310 ok=9310 fail=0 throughput=576req/s`

GC 诊断一致性：

- `markedBeforeZeroGenMismatch=0`，5/5 轮全部为 0；
- `sweptHits=0`，5/5 轮全部为 0；
- 剩余 `genMismatch` 仍然存在，但都落在 large-object 线，范围约 `1429~1669`；
- `phase21GenMismatch` 也仍存在，范围约 `1286~1569`，与上面的 large-path 保持同向。

这 5 轮说明两件事：

1. **small old-gen mismatch 归零不是单次偶然**：
   在相同诊断 gate 下，至少这 5 个短跑批次里都稳定维持 `0/5`。
2. **当前剩余问题已经明显收敛到 large-object 支线**：
   没有再看到 “small 新对象拿到旧 gen” 这条链回潮，但 large-path mismatch 还在，所以下一步重点应转向 large region / `objectSweepGen=0` 的语义，而不是回头重复 small-path 证伪。

### 6.6 去掉 `PreEnumRoots()` 里的 STW 后复验：central old-gen 新对象重新出现

为了区分“是 `ReturnAllAllocBufferRegionsToCentral()` 真修好了”，还是“其实是短 STW 把竞态压住了”，
本轮做了一个更窄的复验：

- 保留 `CJ_MS_DIAG_PREENUM_RETURN_ALL_REGIONS=1`
- 保留新分配对象 sweep/gen 追踪
- **只去掉** `PreEnumRoots()` 诊断分支里包裹 `return-all` 的 `ScopedStopTheWorld`
- 继续在 `AllocBufferMS::ReturnRegionToCentral()` 中记录
  `[MS-GC][NEW-ALLOC-DIAG][RETURN-CENTRAL]`

实现位置：

- `src/Heap/MCollector/MarkSweepCollector.cpp`：
  `PreEnumRoots()` 诊断分支现在直接执行
  `ClearAllMarkBitmaps() -> ReturnAllAllocBufferRegionsToCentral() -> ReclassifyUnsweptToSwept() -> PrepareForSweep()`
- `src/Heap/MAllocator/MSAllocBuffer.cpp`：
  `ReturnRegionToCentral()` 在 `SetSweepGen(CentralRegionPool::GetGlobalSweepGen())`
  之前调用
  `BaseObject::DiagnoseMSNewObjectReturnedToCentral(region, "return-region-to-central")`
- `src/Common/BaseObject.{h,cpp}`：
  新增按 region 追踪“出生于 old-gen region，后来又回到 central”的对象统计

运行：

- 结果目录：
  `MSGC_RESULT/Result/case27_http_complex_json/result/preenum_return_all_no_stw_central_only_20260616/run_20260616_103118/`
- 负载：
  `100K / c8 / 15s / heap12gb / proc6`
- 开关：
  `CJ_MS_DIAG_PREENUM_RETURN_ALL_REGIONS=1`
  `CJ_MS_DIAG_NEW_ALLOC_TRACK_SWEEP=1`
  `CJ_MS_DIAG_NEW_ALLOC_TRACK_LIMIT=1048576`
  `CJ_MS_DIAG_NEW_ALLOC_LOG=1`
  `CJ_MS_DIAG_NEW_ALLOC_LOG_LIMIT=8`

结果先看业务面：

- benchmark 这轮**没有直接炸**：
  `total=6816 ok=6816 fail=0 throughput=422req/s`

但看 GC 诊断，新的事实非常关键：

1. **central 中确实重新出现了 old-gen born new object**

`gc.log` 中明确打出了多条：

```text
[MS-GC][NEW-ALLOC-DIAG][RETURN-CENTRAL] ...
allocObjectSweepGen=8 allocCurrentSweepGen=10 ...
returnRegionSweepGen=8 currentSweepGen=10
```

其中既有 `RawArray<UInt8>`，也有 `stdx.encoding.json:JsonArray` 这样的普通对象。
这说明我们这次关心的那类对象，并不只是“停留在 TL region 里”，而是真的**被带回了 central**。

2. **small-path mismatch 并没有因为 no-STW return-all 而稳定归零**

同一轮 `gc.log` 的多次 summary 都出现了非零：

```text
returnedCentralOldGen=2/5
returnedCentralPhase21OldGen=1/3
returnedCentralMarkedBeforeZeroOldGen=2/5
markedBeforeZeroGenMismatch=2/7/14
```

最终一轮累计为：

```text
total=3047 phase20=242 phase21=2805
markedBefore=462 markedBeforeZero=2585
genMatch=2571 genMismatch=476
phase21GenMismatch=408
markedBeforeZeroGenMismatch=14
```

虽然这轮没有出现用户态失败，但它已经证明：

- **去掉 STW 后，small old-gen 问题统计会回潮**；
- “full flush + no-STW” 不能等价替代“full flush + 短 STW”。

3. **`activeAfter` 多次非零，说明 mutator 确实在和 no-STW flush 竞态**

`gc.log` 多轮出现：

```text
activeBefore=143 returned=143 activeAfter=3
activeBefore=145 returned=145 activeAfter=17
activeBefore=156 returned=155 activeAfter=20
```

这类 `activeAfter != 0` 在带 STW 的实验里不会出现。它非常像：

- collector 正在做 `return-all`
- mutator 同时又重新拿到/保留了一批 active region
- 结果就是“flush 过的窗口”没有真正封闭，gen flip 前后仍有旧代际 active region 漏过去

### 6.7 这轮实验后的结论修正

1. **`ReturnAllAllocBufferRegionsToCentral()` 本身不是全部答案**：
   它在带 STW 的诊断实验里有效，但把 STW 去掉后，central old-gen 新对象又会重新出现。
2. **短 STW 不是无关噪声，而是在实打实地收口 flush/gen-flip 竞态窗口**。
3. **当前更像是“full flush 时序边界不够硬”**，而不是“只要把 TL region 归还给 central 就自然安全”。
4. **因此正式修复方向应从“单纯接 return-all”升级成“如何在安全边界内 flush/retag active region”**，
   包括但不限于：
   - 在 `PreEnumRoots()` 前后找到更硬的同步点；
   - 或改成不依赖全量回收的 active-region retag 协议；
   - 或把“collector 观察到的 active set”和“mutator 可重新安装 active region”的窗口彻底分离。

### 6.8 `STW-only / no-return-all` 对照实验：STW 单独不够

为了进一步区分“到底是 STW 重要，还是 `return-all` 重要”，本轮又做了一次反向对照：

- 保留 `CJ_MS_DIAG_PREENUM_RETURN_ALL_REGIONS=1` 这个诊断分支入口；
- **在分支里重新加回短 STW**；
- 但**刻意不调用** `ReturnAllAllocBufferRegionsToCentral()`；
- 也就是只做：
  `STW + ClearAllMarkBitmaps + ReclassifyUnsweptToSwept + PrepareForSweep`
  ，不 flush alloc-buffer active/cached region。

实现位置：

- `src/Heap/MCollector/MarkSweepCollector.cpp`
  当前日志关键字变成：
  `PreEnumRoots: alloc-buffer STW-only diag ... returned=0 ...`

运行：

- 结果目录：
  `MSGC_RESULT/Result/case27_http_complex_json/result/preenum_stw_only_no_return_all_20260616/run_20260616_111952/`
- 负载不变：
  `100K / c8 / 15s / heap12gb / proc6`
- 开关不变：
  `CJ_MS_DIAG_PREENUM_RETURN_ALL_REGIONS=1`
  `CJ_MS_DIAG_NEW_ALLOC_TRACK_SWEEP=1`
  `CJ_MS_DIAG_NEW_ALLOC_TRACK_LIMIT=1048576`
  `CJ_MS_DIAG_NEW_ALLOC_LOG=1`
  `CJ_MS_DIAG_NEW_ALLOC_LOG_LIMIT=8`

业务结果：

- 这轮 benchmark 仍然**没有直接炸**：
  `total=7465 ok=7465 fail=0 throughput=461req/s`

但 GC 诊断结果比“full-flush + STW”差很多，而且比“no-STW + return-all”也明显更差：

1. **STW 并没有把 active region 清走**

多轮日志都是：

```text
PreEnumRoots: alloc-buffer STW-only diag activeBefore=169 cachedBefore=0 returned=0 activeAfter=169 cachedAfter=0
```

这说明短 STW 只是把 mutator 暂停住了，但因为没有 `return-all`，old active region 仍然原样留在 alloc-buffer 里跨过 gen flip。

2. **small old-gen mismatch 大量回潮**

最终 summary：

```text
total=3067 phase20=164 phase21=2903
markedBefore=540 markedBeforeZero=2527
genMatch=2265 genMismatch=802
phase21GenMismatch=705
markedBeforeZeroGenMismatch=262
```

对比前两组实验：

- `full-flush + STW`：`markedBeforeZeroGenMismatch=0`
- `return-all + no-STW`：`markedBeforeZeroGenMismatch=14`
- `STW-only + no-return-all`：`markedBeforeZeroGenMismatch=262`

这说明：

- **STW 单独并不能解决问题**；
- 仅靠“把世界停住一下”，但不把 active region 真正处理掉，small-path 错代际分配仍然会大量存在。

3. **后续运行中仍然会看到 old-gen born new object 回 central**

虽然 `PreEnumRoots()` 这一刻没有 flush active region，但这些 region 在后续运行中因为耗尽/换出，仍会正常走
`ReturnRegionToCentral()`；本轮 `gc.log` 里仍然打出了 `65` 条
`[NEW-ALLOC-DIAG][RETURN-CENTRAL]`。

典型样例包括：

- `stdx.encoding.json:JsonObject`
- `std.collection:HashMap<std.core:String,stdx.encoding.json:JsonValue>`
- `RawArray<UInt8>`

并且都满足：

```text
allocObjectSweepGen = currentSweepGen - 2
```

### 6.9 这组对照后的最终判断

1. **“STW 是根因”这个说法不成立**：
   STW 单独并不能把 small old-gen mismatch 压到可接受水平。
2. **`return-all` 是主效应，STW 是边界收口效应**：
   - 不做 `return-all`，即使有 STW，active old-gen region 也会继续跨过 gen flip；
   - 做了 `return-all` 但没有 STW，效果会大幅改善，但仍残留少量竞态窗口；
   - 只有 `return-all + STW` 这组组合，目前才把 small-path mismatch 稳定压到 0。
3. **更准确的修复目标是**：
   - 既要处理 active/cached region 的代际归属问题；
   - 又要保证 flush 与 gen flip 之间有足够强的同步边界；
   - 不能只押注其中一个。

### 6.10 `Clear` 放到 STW 外、只保护 `Reclassify + Prepare` 的实验结果

又做了一轮更窄的边界实验：

- 不做 `DrainAllAllocBufferCachedRegions()`；
- `ClearAllMarkBitmaps()` 放在 STW 外；
- 短 STW **只**包：
  `ReclassifyUnsweptToSwept() + PrepareForSweep()`；
- 诊断日志关键字变成：
  `PreEnumRoots: reclassify/prepare STW-only diag ...`

实现与结果目录：

- 实现位置：`src/Heap/MCollector/MarkSweepCollector.cpp`
- 结果目录：
  `MSGC_RESULT/Result/case27_http_complex_json/result/preenum_reclassify_prepare_stw_only_20260616/run_20260616_115210/`
- 负载：
  `100K / c8 / 15s / heap12gb / proc6`

业务结果：

- 这轮 **没有** 出现 `SIGSEGV` / JSON fail；
- benchmark 正常结束：
  `total=7416 ok=7416 fail=0 throughput=459req/s`

但 GC 诊断没有改善，反而比上一轮 `STW-only + no-return-all` 更差：

1. **active region 仍然原样跨过边界**

多轮都看到：

```text
PreEnumRoots: reclassify/prepare STW-only diag activeBefore=168 cachedBefore=0 activeAfter=168 cachedAfter=0
...
PreEnumRoots: reclassify/prepare STW-only diag activeBefore=175 cachedBefore=0 activeAfter=175 cachedAfter=0
```

说明这条路径仍然没有处理 alloc-buffer 的 active/cached region。

2. **small old-gen mismatch 继续存在，而且更重**

最终 summary：

```text
total=2753 phase20=213 phase21=2540
markedBefore=529 markedBeforeZero=2224
genMatch=1901 genMismatch=852
phase21GenMismatch=703
markedBeforeZeroGenMismatch=323
```

对比前两组小范围对照：

- `return-all + no-STW`：`markedBeforeZeroGenMismatch=14`
- `STW-only + no-return-all`：`markedBeforeZeroGenMismatch=262`
- `Clear outside STW + STW only on Reclassify/Prepare`：`markedBeforeZeroGenMismatch=323`

3. **old-gen born new object 继续回到 central**

- 本轮 `gc.log` 里共有 `67` 条
  `[MS-GC][NEW-ALLOC-DIAG][RETURN-CENTRAL]`
- 其中最终 summary 前的 sweep-track 仍能看到：

```text
tracked=215 trackedGenMismatch=60
trackedPhase21GenMismatch=27
trackedMarkedBeforeZeroGenMismatch=13
returnedCentralOldGen=12
returnedCentralPhase21OldGen=8
returnedCentralMarkedBeforeZeroOldGen=12
```

4. **这一轮说明什么**

- 这版不只是 mismatch 更高，尾部还出现了非常差的 GC 行为：
  - `PreEnumRoots: 26,963,026us`
  - `current heap: 42,748,346,368 B`
  - `active-region bytes: 20,879,409,152 B`
- 说明把 `ClearAllMarkBitmaps()` 放到 STW 外，不仅没有收敛问题，反而可能让后续 sweep / region 状态进一步恶化；
- 仅把 STW 缩到 `Reclassify + PrepareForSweep`，**不能**解决 small-path 错代际分配；
- 而且把 `ClearAllMarkBitmaps()` 放到 STW 外，并没有带来更好的结果；
- 因此如果后面仍沿着“短 STW 收口”这条线走，当前证据更支持：
  - 至少应把 `ClearAllMarkBitmaps() + ReclassifyUnsweptToSwept() + PrepareForSweep()`
    放在同一个受保护窗口里；
  - 如果目标是把 small mismatch 真正压下去，仍然需要 `return-all` 或等价的 active-region retag/flush。

## 7. 下一步建议

1. **把 “active alloc-buffer old-gen 假设” 改成正式修复设计**：
   - 这条假设已经验证通过，不需要再重复做“会不会归零”的实验；
   - 下一步应讨论**正式边界**：是在现有 `MarkSetup`/`PreEnumRoots` 的哪一个安全点 flush/retag active region，
     还是改成更轻量的“只 retag 不全量回收”策略；
   - 原则是：不能长期依赖这个临时 STW 诊断开关。
2. **单独分析 large-object `genMismatch=1295` 的语义**：
   - 当前 full-flush 后剩余 mismatch 已全部收敛到 `regionType=4`；
   - 需要确认 large path 的 `objectSweepGen=0` 是“设计如此但无害”，还是 large-region 也有独立的 gen/mark 协议缺口。
3. **长跑回归验证正式修复方向**：
   - 当前 full-flush 只证明了 `15s / 100K / c8` 的短跑现象；
   - 正式修复落地后，至少要回到 `60s` 多轮验证，看 `JsonValue.fromStr` parse failure 与
     `JsonObject.toString` / `WriteBuffer.appendEscape` 崩溃是否继续出现。
4. **不要再把 `CJ_MS_STW_SWEEP` 当成有效分流器**：
   - 当前源码已经没有这个 env 的实际判断分支；
   - `run_20260615_225903` 只能当作“那一轮时序下没崩”的负证据，不能当作受控实验结论。
5. **把“force-mark 后仍失败”的剩余部分单独成线排**：
   - 继续抓 `JsonException: Unexpected character: '}'` 与 `Unexpected character: 'I'`
     两类失败；
   - `Unexpected character: 'I'` 已可视为 server 失败后的级联现象，重点应回到
     server/client 共用的 `JsonValue.fromStr` / `JsonParser` 路径；
   - 现在应优先分成两支并行验证：
     - `JsonValue.fromStr`：valid request / valid response 上的 parse fail；
     - `JsonObject.toString`：`WriteBuffer.appendEscape` / `String.rawData()` /
       `Array` backing store 路径上的 Bus error。
6. **继续补“serializer/live-state”方向的更窄诊断**：
   - 在 `processRequest()` 里保留 `CASE27_GRAPH_DIAG`，确认 `root.toString()` 前
     JSON 对象图 walk 是否总是完整；
   - 对 `WriteBuffer` / `String` / `RawArray<UInt8>` 的关键 backing object
     补 type/stateWord/phase 诊断，确认是“对象已死/头被写坏”还是“内容数组被覆写”；
   - 如需再收口，可单独做“valid payload 并发 `JsonValue.fromStr` 循环”和
     “构造后只做 `root.toString()` 循环”的双支最小验证，但当前还不建议把主场景
     直接降成最小压测替代 case27。

## 7. 注意事项 / 踩坑

- 构建目录是 `cangjie_runtime_MSGC/runtime`，旧文档/README 写的 `cangjie_runtime/runtime`
  是 CopyGC 老树（没有 MS 分配器源码），不要在那边构建。
- macOS arm64 替换 dylib 后必须 `codesign -f -s -`，否则进程被 SIGKILL。
- 崩溃后 benchmark 进程会挂死，需手动清理残留进程，否则会占核拖慢/堵塞后续 run。
- 诊断开关均默认关闭，对正常压测无影响；定位完成后可保留或按需移除。

## 8. 待解决问题清单

- [ ] 钉死“硬崩溃路径”到底来自 allocator 复用、并发 sweep，还是两者叠加。
- [ ] 钉死 active alloc-buffer region 是否确实跨过了 `PrepareForSweep()` 的 gen flip，
  并确认它和 case27 fail/崩溃的因果强度。
- [ ] 钉死“parse failure 路径”是否本质上是并发 mark 期间 parser 新对象 / 局部 live-state 保活不完整。
- [ ] 给出最小修复并多轮验证 case27 c8/p100K 连续 0 失败、0 崩溃。
- [ ] 评估是否需要把 `AllocBump`/`AllocFromBitmapFast` 的计数/位图原子化，或仅保证单 owner。
- [ ] 复核本轮保留的两个并发修复（Dekker 屏障、区域栈 ABA）是否要单独提交。

## 9. 2026-06-16 新增：`PreEnumRoots` 分段耗时实测

- 目录：
  `MSGC_RESULT/Result/case27_http_complex_json/result/preenum_reclassify_prepare_stw_timing_20260616/run_20260616_145046/`
- 配置：
  `100K / c8 / 15s / heap12gb / proc6`
- 变更：
  把
  `ReclassifyUnsweptToSwept()`
  和
  `PrepareForSweep()`
  一起放进 STW，
  并新增
  `StopAndWaitForAllSweep / ClearAllMarkBitmaps / stw`
  三段计时。

业务结果仍然正常：

```text
total=8063 ok=8063 fail=0 throughput=499req/s
```

最后一轮 `PreEnumRoots` 分段：

```text
stopSweep=24,756,543us
clearBitmaps=10,387,425us
stw=249us
PreEnumRoots=35,144,231us
```

这轮结论很直接：

1. **短 STW 本身不是长尾来源**：
   `Reclassify+Prepare` 放一起后，尾轮 `stw` 只有 `249us`。
2. **尾轮真正的重头已经坐实**：
   时间主要花在
   `StopAndWaitForAllSweep()` 和 `ClearAllMarkBitmaps()`，
   而不是 `PrepareForSweep()` 这段 STW。
3. **因此后续如果继续追 `PrepareForSweep` 相关竞争**，
   更合理的方向应是：
   - `PrepareForSweep()` 为什么只要 STW 包起来就能规避业务错误；
   - 但 `PreEnumRoots` 长尾为什么又主要卡在 `StopAndWaitForAllSweep()` /
     `ClearAllMarkBitmaps()`；
   - 这两条线需要拆开分析，不能再把“业务是否修复”和“PreEnumRoots 为什么长”
     混成一个问题。

## 10. 2026-06-16 新增：`ClearAllMarkBitmaps` 非 STW 读写锁实验失败，回退到 STW 后恢复稳定

### 10.1 实验 A：只给 `ClearAllMarkBitmaps` ↔ `SweepRegion` 加保护，不放进 STW

- 变更：
  - 目标是验证“只补 clear-vs-sweep 冲突”是否足够；
  - `ClearAllMarkBitmaps()` 移出 STW；
  - `ReclassifyUnsweptToSwept()` 与 `PrepareForSweep()` 继续在 STW 中；
  - `PrepareForSweep()` 继续承担 `SetSweepGen` 归一化。
- 结果目录：
  `MSGC_RESULT/Result/case27_http_complex_json/result/20260616_191349_non_stw_bitmap_lock_verify_c8_p100K_compare/`

结果失败，MSGC 直接复现：

```text
MSGC rc=139
Parse Error: Unexpected character: '}'
Parse Error: Unexpected character: 'I'
Thread "schd-worker" catched unhandled SIGSEGV
```

关键观察：

1. `mark-completeness` 全程仍然是 `violations=0`，说明这把实验锁并没有把问题转换成 verify 可见的 missed-mark。
2. 业务侧仍然先出现 JSON parse failure，随后在
   `stdx.encoding.json.jsonWriteWithoutFormat`
   崩溃，说明“只防 sweep 读 markBitmap 与 clear markBitmap 冲突”**不够**。
3. 这轮结果反证了：
   当前 `ClearAllMarkBitmaps` 不能简单从 STW 拿出去；
   至少在现有实现下，它还依赖 STW 避免别的并发窗口。

### 10.2 实验 B：移除这次锁实验，`SetSweepGen` 挪回 `ClearAllMarkBitmaps`，恢复 STW 包裹

- 回退：
  - 移除 `ClearAllMarkBitmaps` 非 STW 的实验路径；
  - 去掉这轮“只防 clear-vs-sweep”的锁方案；
  - 把 `PrepareForSweep()` 中的 `SetSweepGen` 归一化重新挪回 `ClearAllMarkBitmaps()`；
  - 恢复 STW 一次包住：
    `ClearAllMarkBitmaps()` +
    `ReclassifyUnsweptToSwept()` +
    `PrepareForSweep()`
- 结果目录：
  `MSGC_RESULT/Result/case27_http_complex_json/result/20260616_192218_stw_clear_reclassify_prepare_restore_c8_p100K_compare/`

结果恢复稳定：

```text
MSGC rc=0
total=9270 ok=9270 fail=0
json fail=0
SIGSEGV=0
verify violations=0
```

当前结论：

1. **把 `ClearAllMarkBitmaps` 放回 STW 是当前已验证稳定的做法。**
2. **“只补 clear-vs-sweep”不是充分条件**；
   `ClearAllMarkBitmaps` 在 STW 外面时，仍然会重新带回 case27 的 parse fail + SIGSEGV。
3. 如果后续还要继续缩小 STW，
   需要重新拆时序，找出 `ClearAllMarkBitmaps` 在 STW 外到底还和谁竞争，
   不能再只盯 `SweepRegion()` 一条线。
