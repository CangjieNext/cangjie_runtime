# MSGC Bugfix 汇总：问题、根因、修复与验证

> 本文汇总 `MSGC_RESULT/bugfix/` 中的 MSGC 问题记录，并以当前工作树源码为最终核验依据。
> 同一问题的状态更新、诊断实验和最终修复记录合并到同一章；尚未闭环的问题明确标注为“未完全修复”。

## 1. 阅读口径与结论总览

本文先阅读总体架构文档，再逐篇对照 `MSGC_RESULT/bugfix/` 的问题记录，最后检查当前工作树中的 Runtime/LLVM 实现。记录中有些是“已经合入的修复”，有些是为了缩小范围而做的实验，还有些只是防御性诊断代码；下面不把三者混为一谈。

总体结论如下：

| 类别 | 代表问题 | 结论 |
| --- | --- | --- |
| GC 标记闭包 | Case2/F5、Case27 finalizer/WBB | 根因集中在 old-edge 丢失、快照边界和“已标记但未扫描” |
| 元信息/边界 | Case8、managed entry | 高位污染和陈旧 phase 会把合法对象解释成非法 TypeInfo |
| 分配器/并发 | Case3、Case27 | 锁顺序、跨 chunk 搜索、堆端点发布和 active region 代际切换相互放大 |
| 编译器快路径 | Case27/28 | inline barrier 的位图、日志顺序、跳转表尺寸及 bulk copy 语义均可能破坏闭包 |
| 调度/同步 | Case25、Case28 | mutex 初值已定位；Case28 的完整 STW/saferegion 闭环仍未完成 |

“修复已落地”只表示当前源码可见对应实现，不表示所有压力场景均已通过。特别是 Case2 中的普通新对象 post-init 扫描、Case8 的若干防御改动，以及 Case27 的快路径改动，部分 bugfix 文档记录的是历史分支或实验版本；源码核验结果以本汇总中的“当前状态”为准。

## 2. Case2/F5：HashMap/ArrayList 被错误回收或 stale edge

### 2.1 bitmap slot stride 假设被排除

最初怀疑 2-slot bitmap 的 slot stride 计算错位，导致 HashMap 的对象槽位没有被扫描；切换到 3-slot 后 F5 恰好通过，因而很容易误判。逐条检查 bitmap 访问、region 边界和 heap shrink 指针后，没有找到第三槽位越界或悬空 region 指针。保留 2-slot、只打开强制入队诊断时问题仍会消失，说明“槽位步长”只是相关性，不是根因。

来源：[`20260426_case2_f5_bitmap_slot_stride.md`](../bugfix/20260426_case2_f5_bitmap_slot_stride.md)。

### 2.2 zero-dst struct barrier 漏记 old edge

并发 rehash/`ArrayList` 扩容会先把目标 struct 区域清零，再写入真实引用。旧实现看到目标区“全为 0”便走 zero-dst fastpath，跳过了对源对象 old edge 的 shade/enqueue；GC 随后可能回收仍被容器引用的对象，表现为 size=0、value mismatch 或 SIGSEGV。

修复是删除该 zero-dst 特判，让 `WriteStruct` 始终按类型信息扫描源/目标范围，再执行 `memcpy_s`；诊断中使用的 poisoned-object guard 只能帮助定位，不能作为生产修复。当前 [`MSMarkBarrier.cpp`](../../src/Heap/MBarrier/MSMarkBarrier.cpp) 已无 zero-dst 快速返回。

该结论同时覆盖 Case11 状态记录中的同一问题，不另列为独立根因。

### 2.3 Mark/Sweep 交叠与 sweep snapshot 边界

第二个独立问题是 sweep 尚未完成时，下一轮 mark 已经开始：GC-active TL、fresh region 或刚返回 central pool 的 region 仍可能被上一轮 sweep 看到。若 snapshot 在 `MarkTermination` 之后才建立，就会把本轮新分配/新发布的对象错误纳入上一轮的可回收集合。

记录中验证过的修复组合是：在 `PreEnumRoots`/MarkSetup 早期停止并等待旧 sweep、清空/重建 mark bitmap、按 sweep generation 重新分类，禁止依赖 alloc-black 掩盖未扫描对象；在诊断阶段还使用过 `CJ_MS_STW_SWEEP` 强制完成 backlog。当前 [`MarkSweepCollector::PreEnumRoots`](../../src/Heap/MCollector/MarkSweepCollector.cpp) 的确执行 `StopAndWaitForAllSweep -> ClearAllMarkBitmaps -> ReclassifyUnsweptToSwept -> PrepareForSweep`，但 `CJ_MS_STW_SWEEP` gate 在当前源码中没有找到，且该实现并不能替代对并发 sweep 根因的继续修复。

### 2.4 新对象 marked-but-not-scanned

旧分支的 allocator 曾在对象尚未完成初始化时将其标黑（小对象调用 `MarkNewObject`，大对象直接设置 marked flag）。若构造函数随后写入子对象，扫描器会因为对象“已经 marked”而不再入队，子图就会漏扫。正确顺序是：完成 `SetClassInfo`/长度等初始化后，再执行 mark，并把带引用字段的对象放入 deferred/main queue。

Case2/F5 的记录在该顺序修复后达到 5/5；LLVM 侧也移除了 new-object fastpath 的 alloc-mark。当前工作树中 `MarkNewObject` 仍实现为“MarkObject + 有引用则 deferred enqueue”，但普通 `MObject::NewObject`/`MArray::NewKnownWidthArray` 只完成分配和初始化，未看到相应的 post-init 调用；大对象注册仍有 `AtomicSetMarked()`。因此这部分应视为“历史修复结论，当前实现需要继续核对”，不能在本汇总中宣称已完全闭环。

来源：[`20260426_case2_f5_snapshot_no_allocblack_fix.md`](../bugfix/20260426_case2_f5_snapshot_no_allocblack_fix.md)、[`20260502_case2_f5_new_object_postinit_scan_fix.md`](../bugfix/20260502_case2_f5_new_object_postinit_scan_fix.md)、[`20260528_llvm_msgc_newobject_fastpath_remove_alloc_mark.md`](../bugfix/20260528_llvm_msgc_newobject_fastpath_remove_alloc_mark.md)。

## 3. Case8：HTTP JSON SIGSEGV 与 stateWord/TypeInfo 污染

Case8 的症状是 HTTP JSON 压测在 2–20 秒内随机 SIGSEGV，崩溃位置常落在 subtype 判断、对象解引用或 sweep 后访问。根因不是单一业务数据错误，而是 stateWord 高 16 位残留 GC phase/标记位，经过 runtime/managed 边界后被当作 TypeInfo；同时 sweep 发布、WBB 刷新和终止队列对失效对象缺少一致的保护，放大了坏指针的可见范围。

bugfix 记录提出并验证过五类修复：

1. `CJ_MCC_IsSubType` 等 CompilerCalls 入口统一掩掉 stateWord 高位；
2. `StateWord::InitStateWord` 用一次原子 release store 写入干净 TypeInfo 和 state=0；
3. sweep 在向 mutator 发布 bitmap/summary 前清理死对象；
4. WBB flush 过滤非 heap/非 traceable/invalid 对象；
5. MarkTermination 遇到失效对象时跳过并记录，而不是 FATAL 退出。

当前源码已确认前两项（[`CompilerCalls.cpp`](../../src/CompilerCalls.cpp)、[`StateWord.h`](../../src/Common/StateWord.h)），WBB flush 也有地址/有效性过滤；但 `ExecuteLikeDrainMainQueue` 仍对 invalid object `LOG(FATAL)`，`SweepRegion` 的具体发布顺序也与记录中的“三阶段 zero-before-publish”不完全一致。因此历史文档中的“九文件修复全部当前生效”不能直接照搬。

来源：[`20260524_case8_five_fixes_analysis.md`](../bugfix/20260524_case8_five_fixes_analysis.md)、[`src_current_bugfix_summary_20260525.md`](../bugfix/src_current_bugfix_summary_20260525.md)。

## 4. TypeInfo 高位污染与 managed entry stale phase

### 4.1 Runtime TypeInfo 归一化

TypeInfo 句柄来自 stateWord/编译器 ABI。GC phase 或标记状态若泄漏到高位，`IsSubType`、itable/method lookup 等入口会读取一个并不存在的类型地址。提交 `c34e3163` 的分析确认这条路径是 Case8 崩溃的重要根因。

修复是集中增加 `NormalizeRuntimeTypeInfo`，在所有 CompilerCalls 入口先保留有效低位再参与查表；当前 [`CompilerCalls.cpp`](../../src/CompilerCalls.cpp) 的 `CJ_MCC_IsSubType` 等路径已调用该归一化。代价是额外 mask，记录的 Case1 性能回归约 20%，需在后续 benchmark 中持续观察。

### 4.2 managed entry 的 threadData/x28 快照陈旧

AArch64 managed entry 会缓存 `threadData`/x28 上下文。GC phase 变化只更新 TLS，而旧快照仍携带 mark/termination phase，导致进入 managed code 后安装错误 barrier；CJThread 回栈/恢复也可能把旧 x28 带回。

修复集中在 native→managed entry 和 context restore：入口重新从当前 TLS 读取 thread data/phase，恢复上下文时以当前 TLS 修正 x28（macOS 对应 r15），并在运行 managed code 前同步 mutator phase/stack。当前 `MRT_UpdateThreadLocalDataPhase`（[`Mutator.cpp`](../../src/Mutator/Mutator.cpp)）、`MRT_PreRunManagedCode`（[`CjScheduler.cpp`](../../src/CjScheduler.cpp)）和 `MRT_RestoreContext`（[`Exception.cpp`](../../src/Exception/Exception.cpp)）可见这些边界修复；源码中未找到记录里某些实验分支的 helper 原名，应以行为而非函数名核验。

来源：[`20260527_c34e3163_fix_http_sigv_analysis.md`](../bugfix/20260527_c34e3163_fix_http_sigv_analysis.md)、[`20260527_managed_entry_threaddata_phase_fix.md`](../bugfix/20260527_managed_entry_threaddata_phase_fix.md)。

## 5. Case25：HTTP stall、EV_CLEAR 假象与 CJMutex 初始化缺失

### 5.1 netpoll EV_CLEAR 回归：真实问题但不是最终根因

曾有一次 macOS netpoll 改动移除 `EV_CLEAR`，HTTP 请求在约 30 秒后停止；把超时时间调大只能延后症状，不能修复事件循环。回滚/恢复 `EV_ADD | EV_ENABLE | EV_CLEAR` 后 I/O 行为恢复，因此它是一次真实回归，但不是最终的 Case25 根因。

当前 [`netpoll.cpp`](../../src/CJThread/src/runtime/netpoll/src/macos/netpoll.cpp) 已恢复 `EV_CLEAR`。来源：[`20260606_case25_stall_status_and_next_steps.md`](../bugfix/20260606_case25_stall_status_and_next_steps.md)、[`20260606_current_src_case25_regression.md`](../bugfix/20260606_current_src_case25_regression.md)。

### 5.2 CJMutex.state 未初始化：最终根因与修复

真正的 stall 来自请求局部 mutex 复用未初始化的 `state`。对象内存中残留 `LOCKED/STARVING` 位，`tryLock` 永远失败，工作线程在等待路径上表现为 HTTP stall。

`MCC_MutexInit` 现在显式设置 `ownerThreadId=-1`、`ownCount=0`、`state=0`、`isSemaInit=false`，之后再按需初始化 semaphore。新鲜 60 秒运行达到 18,523/18,523 请求、约 308 req/s；当前 [`Sync.cpp`](../../src/Sync/Sync.cpp) 已包含这组初始化。

## 6. Case3：增长卡死与 large allocation 假 OOM

### 6.1 grow/shrink 锁顺序反转

增长路径先拿 shard 锁再拿 page-heap 锁，收缩路径相反，压力下形成 ABBA 死锁；`MaybeNotifyShrink` 还在 mutator 热路径执行重同步探测，进一步放大停顿。

修复将增长过程改为 try-lock/让出后重试，释放 page lock 后再取 shard lock；mutator 侧只发送 shrink 通知，把重活移到后台。来源：[`20260621_case3_msgc_stall_and_false_oom_fix.md`](../bugfix/20260621_case3_msgc_stall_and_false_oom_fix.md)。

### 6.2 多 Region 连续分配缺少 donor steal 与跨 chunk 搜索

大对象/连续 Region 请求只在当前 shard 查找；本地没有连续区间时即返回 OOM，即使 donor shard 或 chunk 边界另一侧存在足够空间。旧 summary skip 还把跨 chunk 的候选直接跳过。

修复增加 `TryStealContiguousRegionRunLocked`，允许 donor shard 转移连续区间，并把 summary skip 限制在同一 chunk、对跨 chunk 区间做精确扫描。当前 [`MSFreeManager.cpp`](../../src/Heap/MAllocator/MSFreeManager.cpp) 已有 donor steal 和对应锁保护，Case3 压测不再出现假 OOM。

## 7. Signal dump 重入导致的次生 runtime-frame 噪音

一次真正的 native fault（显式 rethrow）会触发 signal dump；dump 在展开栈或读取 frame 时再次 fault，旧实现递归进入 signal handler，日志中出现大量重复 runtime-frame SIGSEGV，掩盖了首个故障。

修复使用 TLS `g_fatalSignalDumpDepth`/作用域 guard：顶层信号负责完整 dump，嵌套 fault 只输出最小信息并以 `_exit(128 + sig)` 结束，禁止递归展开。验证结果应是进程退出 139，`sigsegv` 与 runtime-frame 诊断各出现一次。来源：[`20260621_signal_dump_nested_fault_noise_fix.md`](../bugfix/20260621_signal_dump_nested_fault_noise_fix.md)。

## 8. Case27：复杂 JSON 压力下的 GC、分配器与编译器快路径问题

### 8.1 finalizer 新对象的 deferred scan 缺失

finalizer 创建的新对象已经被 `MarkNewObject` 标记为黑，但该函数随后用“对象已标记”判断而不再入队，构造过程中写入的子对象没有进入 mark closure。表现为客户端 JSON 解析失败、偶发 miss，且不一定立即崩溃。

修复为 `MarkObject` 后对含引用对象执行 deferred main-queue enqueue，在 MarkTermination 再完成闭包；当前 [`MarkSweepCollector::MarkNewObject`](../../src/Heap/MCollector/MarkSweepCollector.cpp) 已是该形式，调用点仍主要在 `BaseObject::OnFinalizerCreated`。短压测不再 crash，但 2–5 个 client failure 和 EmptyCacheDiag 残留，不能视为 Case27 完全解决。

### 8.2 mutator 退出时 WBB 丢失

线程/协程退出时，thread-local write-barrier buffer 可能在全局 flush 之前被释放；其中记录的 old edge 没有机会进入 mark work queue，导致对象在下一轮被错误回收。该问题解释了 Thread/Timer miss entries，但不能解释所有 JSON 失败。

修复增加退出路径的 `FlushExitedMutatorWriteBarrierBuffer`，并在 Transit/Destroy 前调用 `MaybeFlushPendingMSMarkWorkOnMutatorExit`。来源：[`20260614_case27_mutator_exit_wbb_flush_fix.md`](../bugfix/20260614_case27_mutator_exit_wbb_flush_fix.md)。

### 8.3 empty-cache、borrowed region 与 active-region generation 问题

Case27 还暴露出三类分配器状态异常：`pop-without-in-cache-mark`、cache slot 越界/invalid region、borrowed span 以裸 pair 在并发回收时发生竞争，以及 active allocation buffer 跨 generation flip 仍被旧 sweep 观察。实验显示：STW 阶段同时 flush active+cached region 后，小对象 old-gen mismatch 可降为 0；只 STW 或只 return-all 均不能完全消除，大对象 mismatch 仍存在。

后续修复包括 shrink 时保留 shard 0 的最小 segment、fresh region 使用完整 `Reset()`、borrowed span 恢复为带 shard 锁保护的 `deque`，并增加诊断计数。当前 [`MSCentralRegionPool.cpp`](../../src/Heap/MAllocator/MSCentralRegionPool.cpp) 已按 sweep generation 分类，`MSFreeManager.cpp` 已有锁保护的 borrowed/donor 路径；但 heap active-end 仍是普通静态地址，尚未看到记录中所述的 atomic release/acquire 发布闭环。

来源：[`20260615_case27_msgc_diagnostics_and_status.md`](../bugfix/20260615_case27_msgc_diagnostics_and_status.md)、[`20260615_case27_uaf_allocator_investigation.md`](../bugfix/20260615_case27_uaf_allocator_investigation.md)。

### 8.4 WriteRef/WriteStruct fastpath、heap publish 与 barrier gate

高强度 c1000/p100K 运行中，关闭全部 AArch64 `WriteRefFieldFast`/`WriteStaticRefFast`/相关 struct 快路径后进程可退出；只开 ref 或只开 struct 仍会 crash，说明 runtime slow path 不是唯一问题，inline barrier 语义或其 gate/寄存器状态是主要嫌疑。另有独立的堆扩展发布竞态：新 segment 已可见而 `heapCurrentEnd` 尚未同步，可能触发 `NewObject non-heap addr`；历史补丁曾移动发布顺序并在 compiler 端使用 acquire load。

phase gate 的实验表明，以全局 runtime `g_msgcWBEnabled`（release/acquire）作为真值比 TLS shadow 稳定；TLS shadow 在 warmup 后可能陈旧。当前 [`Heap.cpp`](../../src/Heap/Heap.cpp) 的 barrier 安装按 phase 映射，`heapCurrentEnd` 在当前工作树仍是普通 `MAddress`，因此相关历史文档不能当作当前源码已完成的原子发布修复。

来源：[`20260630_case27_ref_fastpath_rootcause.md`](../bugfix/20260630_case27_ref_fastpath_rootcause.md)、[`20260703_case27_msgc_barrier_installed_state_vs_tls_phase_conclusion.md`](../bugfix/20260703_case27_msgc_barrier_installed_state_vs_tls_phase_conclusion.md)、[`20260704_case27_msgc_heap_range_check_publish_fix.md`](../bugfix/20260704_case27_msgc_heap_range_check_publish_fix.md)。

### 8.5 ArrayCopy bulk inline fastpath

`ArrayCopyRefFast`/`ArrayCopyStructFast` 共享“批量 old-range 扫描 + 直接写 WBB + memmove”的复杂路径：目标范围、GCTib 解析或缓冲区容量任一判断出错，就会漏记 old edge 或破坏 struct 中的 key/value。移除 AsmPrinter 的 inline 分发后，部分多轮运行可稳定退出，但请求级失败仍存在，说明它是高风险放大器而非已经证明的唯一根因。

当前诊断策略是强制走 runtime slow call，再分别恢复 ref/struct fastpath；Case27 的后续记录也暂时将 arraycopy 设为非 inline。来源：[`20260703_case27_arraycopy_tryemit_removed_sigv_status.md`](../bugfix/20260703_case27_arraycopy_tryemit_removed_sigv_status.md)、[`20260705_case27_arraycopy_ref_struct_inline_suspects.md`](../bugfix/20260705_case27_arraycopy_ref_struct_inline_suspects.md)。

### 8.6 MCC_AcquireRawData 放大器与 HashMap resize 破坏

Case27/28 的坏 key 常在 `HashMap<String, JsonValue>` 扩容后的 `array_copy_struct` 出现。`MCC_AcquireRawData` 先调用 `Heap::IsHeapAddress(array)`，再检查 `array == nullptr`；当 key 已被破坏成低地址（例如 0x40）时，会在 null/非法地址上继续做对象判断，把原始内存破坏放大成 SIGSEGV。

修复方向是先判空、再做 heap-range/type 检查，并继续追查 `array_copy_struct` 的引用布局与 barrier 语义。当前 [`CompilerCalls.cpp`](../../src/CompilerCalls.cpp) 仍保持“heap check 在前、null check 在后”，所以该放大器仍未完全消除；Case28 的 resize 根因也尚未闭环。

### 8.7 Route-A mark-before-log 顺序漏洞

old-only Route-A inline barrier 曾先对目标对象做 mark CAS，再检查 active log 是否存在或是否有容量。CAS 成功后若发现 log 满而转慢路径，慢路径看到对象已经 marked，便认为无需再次入队；结果是“标记位已置、扫描任务却丢失”，正是典型 marked-but-not-scanned。

修复顺序为先检查并预留 log/capacity，再执行 mark CAS，最后写入 log；任何一步失败都直接走可重复入队的 slow path。LLVM FileCheck/汇编检查已通过。来源：[`20260709_case27_ref_inline_mark_before_log_check_fix.md`](../bugfix/20260709_case27_ref_inline_mark_before_log_check_fix.md)。

### 8.8 inline bitmap word 与 region-neighbor 解析错误

inline barrier 的 bitmap word 索引误用 `slotIdx >> 3`，而 runtime bitmap 一个 word 覆盖 64 个 bit，应为 `slotIdx >> 6`；同时 inline region 解析没有处理对象落在 region 首尾时的 prev/next neighbor 修正，可能读错 region metadata，产生 SIGBUS 或错误标记。

修复了 `>>6` 计算并补齐 neighbor correction；LLVM FileCheck 与生成汇编已验证位操作和边界分支。来源：[`20260709_inline_mark_bitmap_word_and_slot_alignment_fix.md`](../bugfix/20260709_inline_mark_bitmap_word_and_slot_alignment_fix.md)。

### 8.9 inline 写屏障跳转表尺寸低估

AArch64 inline expansion 的真实指令长度没有注册到指令尺寸估算器，链接器选择 8-bit compressed jump table 后，超过 255 字节的偏移被截断，分支会落入 barrier 指令中间。典型症状是 URL parse 出现 `start=17,end=16` 这类确定性错误。

修复是在 `AArch64InstrInfo` 导出并保守报告 ref 384B、struct 512B 的最大尺寸，令跳转表选择足够宽的编码；arraycopy 不再 inline。该记录要求重新构建 compiler/stdx 后再做最终验证，不能把补丁提交本身当作验证完成。来源：[`20260710_case27_inline_wb_jumptable_truncation_fix.md`](../bugfix/20260710_case27_inline_wb_jumptable_truncation_fix.md)。

### 8.10 private partial work 导致并发闭包 OOM

quiesce 阶段把 GC main/helper 的 private partial workbuf 发布回全局队列，会在 mutator 仍能写屏障时重新打开并发 closure feedback。标记量从正常规模膨胀到数百万对象/数 GB，最终 OOM；这不是普通堆容量不足。

修复是 quiesce 不发布 private partial，只允许已经在 global late queue 的工作重新开启并发；线程 dispose 时才把剩余 private work 发布给 STW MarkTermination。修复后的 60 秒 c10/p10K 结果为 400,392 success、0 fail。来源：[`20260712_case27_partial_tail_concurrent_closure_oom_fix.md`](../bugfix/20260712_case27_partial_tail_concurrent_closure_oom_fix.md)。

## 9. Case28：struct bulk barrier 崩溃与 STW 停止卡住

### 9.1 HashMap resize/array_copy_struct 引用破坏

Case28 在 `HashMap<String, JsonValue>` 批量 put 触发 resize 后，于 `MCC_AcquireRawData` 以 0x40 一类低地址崩溃；CopyGC 不复现。现有证据指向 `array_copy_struct` 的 bulk barrier/布局处理破坏了 String key 或 value，`MCC_AcquireRawData` 的错误判空顺序又把坏指针变成确定性 SIGSEGV。

当前仍未证明是单一字段偏移、GCTib 解析还是 inline barrier；正确的隔离方法是强制 struct slow path、先修正 `MCC_AcquireRawData` 判空，再用最小 HashMap resize 用例复现。来源：[`20260621_case28_msgc_hashmap_resize_struct_barrier_crash.md`](../bugfix/20260621_case28_msgc_hashmap_resize_struct_barrier_crash.md)。

### 9.2 phase 3 gate 误判与 post-GC 未回切 IDLE

旧 AArch64 gate 只把 mark phase 视为需要 WBB 的状态，phase 3 `RECLAIM_SATB_NODE` 被误判为 slow/store-only；同时 PostGarbageCollection 清理后没有明确回到 IDLE，下一批写入继续走错误 barrier。

修复将所有非 mark 的 phase 映射为 StoreOnly，并在 SATB reclaim 完成后显式 `GC_PHASE_IDLE`；当时 runtime 的并发 Sweep 状态也映射到 idle barrier。LLVM FileCheck 已通过，当前 [`TracingCollector.cpp`](../../src/Heap/Collector/TracingCollector.cpp) 和 [`Heap.cpp`](../../src/Heap/Heap.cpp) 可见回切逻辑。

### 9.3 PreEnumRoots 等待所有 mutator 停止卡住

phase gate 修复后，Case28 暴露出新的阻塞点：第一次 GC 停在 `PreEnumRoots -> StopTheWorld -> WaitUntilAllMutatorStopped`。部分 worker 在 `HandleSafepoint` 条件变量等待，HTTP worker 仍处于 read/LeaveSaferegion 路径，说明 saferegion 计数、唤醒和停止协议尚未闭合。

这是独立于 struct barrier 崩溃的调度问题；不能用增大超时或关闭 GC 代替修复。需要继续核对 mutator 进入/离开 saferegion、CJThread worker 唤醒及 `StopTheWorld` 的“所有线程已停”判据。来源：[`20260622_case28_msgc_phase3_phase_gate_and_idle_transition.md`](../bugfix/20260622_case28_msgc_phase3_phase_gate_and_idle_transition.md)、[`20260623_case28_msgc_stw_wait_until_all_mutator_stopped.md`](../bugfix/20260623_case28_msgc_stw_wait_until_all_mutator_stopped.md)。

## 10. 当前工作树中的通用修复归类

从源码复核可归纳出以下已经存在的通用机制：

1. **Mark/Sweep 周期隔离**：`PreEnumRoots` 先等待旧 sweep，再清 bitmap、重分类 generation 并准备新一轮 sweep；central pool 返回 region 时按 `globalSweepGen_` 进入 swept/unswept 队列。
2. **TypeInfo/StateWord 边界清理**：runtime 入口统一做 TypeInfo 低位归一化；MSGC 初始化 stateWord 时原子写入干净值，避免 phase 位进入元信息查表。
3. **分配器并发保护**：grow/shrink 的锁顺序已调整，连续 Region 有 donor steal，fresh region 采用完整 reset；这些修复直接对应 Case3 的卡死和假 OOM。
4. **Barrier phase 安装**：mark setup/concurrent/termination 使用 MS mark barrier，IDLE/finish/SATB reclaim/MS sweep 使用 idle/store-only barrier；PostGarbageCollection 会回切 IDLE。
5. **退出与信号边界**：mutator 退出前 flush WBB，fatal signal dump 通过 TLS 深度防止递归。

同时，以下项目在当前工作树仍需谨慎表述：普通新对象 post-init `MarkNewObject` 调用、large-object alloc-black 移除、`CJ_MS_STW_SWEEP` 诊断 gate、sweep zero-before-publish 三阶段顺序、invalid queue 的“跳过而非 FATAL”、`heapCurrentEnd` 原子发布以及 `MCC_AcquireRawData` 先判空。这些在 bugfix 文档中出现过，但当前源码核验并不全部支持。

## 11. 未完全闭环的问题与后续优先级

### P0：先保证正确性，再谈性能

- Case27/28 的 AArch64 WriteRef/WriteStruct/ArrayCopy inline fastpath 仍可导致 marked-but-not-scanned、坏 key 或 SIGSEGV。建议默认关闭高风险 inline，分别以最小用例恢复，并为每条路径增加 old-edge、log-full、region-boundary 和 phase 切换测试。
- 修正 `MCC_AcquireRawData` 的 null 检查顺序，之后重新定位 `array_copy_struct` 的字段布局/GCTib/写屏障问题。
- 继续闭环 Case28 `PreEnumRoots` 的 saferegion/STW 等待协议；当前不能用延长超时掩盖停滞。

### P1：收敛并发分配与 GC 周期边界

- 统一 active allocation buffer、cached/borrowed region 的 generation 快照和 STW flush 策略，补上 `heapCurrentEnd` 的 release/acquire 发布协议。
- 明确普通小对象和 large object 的 alloc-black 策略：若采用 post-init mark，必须保证所有 allocator/LLVM fastpath 都遵守同一顺序；若不采用，则删除残余 marked 设置。
- 保留 `CJ_MS_STW_SWEEP` 作为诊断开关，并将其行为、默认并发 sweep 路径及退出条件写入源码/测试，避免文档与实现再次漂移。

### P2：性能与回归防护

- TypeInfo 归一化和 slow barrier 的性能回归需要以隔离 SDK、重新编译 `.cj` 用例后的数据为准。
- 每次替换 runtime dylib 后重新编译测试；多 SDK 对比必须使用 `env -i` 隔离并验证实际加载的 dylib 来源。
- 为 Case2/F5、Case8 HTTP JSON、Case25 HTTP、Case27 c1000/p100K、Case28 HashMap resize 建立固定的成功数、失败数、退出码、GC 日志和加载路径检查。

## 附录 A：bugfix 原始文档归并索引

- **Case2/F5**：`20260426_case2_f5_bitmap_slot_stride.md`、`20260426_case2_f5_snapshot_no_allocblack_fix.md`、`20260501_case2_f5_hashmap_mssatb_two_bugs.md`、`20260502_case11_two_issues_status.md`、`20260502_case2_f5_new_object_postinit_scan_fix.md`、`20260502_case2_current_analysis.md`、`20260502_two_issue_fixes_extracted.md`、`20260528_llvm_msgc_newobject_fastpath_remove_alloc_mark.md`。
- **Case8/TypeInfo**：`20260524_case8_five_fixes_analysis.md`、`src_current_bugfix_summary_20260525.md`、`20260527_c34e3163_fix_http_sigv_analysis.md`、`20260527_managed_entry_threaddata_phase_fix.md`。
- **Case25/Case3/Signal**：`20260606_case25_stall_status_and_next_steps.md`、`20260606_current_src_case25_regression.md`、`20260607_case25_msgc_mutex_init_root_cause.md`、`20260621_case3_msgc_stall_and_false_oom_fix.md`、`20260621_signal_dump_nested_fault_noise_fix.md`。
- **Case27**：`20260614_case27_finalizer_marknewobject_deferred_scan_fix.md`、`20260614_case27_mutator_exit_wbb_flush_fix.md`、`20260615_case27_msgc_diagnostics_and_status.md`、`20260615_case27_uaf_allocator_investigation.md`、`20260630_case27_ref_fastpath_rootcause.md`、`20260630_case27_write_ref_fastpath_fix_plan.md`、`20260701_case27_empty_string_key_followup.md`、`20260702_case27_reenable_4_msgc_write_fastpaths_validation_fail.md`、`20260702_case27_writebarrier_removed_cjc_runtime_a4cb33f6_multirun_status.md`、`20260703_case27_arraycopy_tryemit_removed_sigv_status.md`、`20260703_case27_msgc_barrier_installed_state_vs_tls_phase_conclusion.md`、`20260704_case27_msgc_heap_range_check_publish_fix.md`、`20260705_case27_arraycopy_ref_struct_inline_suspects.md`、`20260706_case27_reclaim_diag_conclusion.md`、`20260709_case27_ref_inline_mark_before_log_check_fix.md`、`20260709_inline_mark_bitmap_word_and_slot_alignment_fix.md`、`20260710_case27_inline_wb_jumptable_truncation_fix.md`、`20260712_case27_partial_tail_concurrent_closure_oom_fix.md`。
- **Case28**：`20260621_case28_msgc_hashmap_resize_struct_barrier_crash.md`、`20260622_case28_msgc_phase3_phase_gate_and_idle_transition.md`、`20260623_case28_msgc_stw_wait_until_all_mutator_stopped.md`。
