# Step38:MSGC 写屏障 fastpath / slowpath 优化点分析与实施规格

> 记录时间:2026-07-10
> 前置:Step37(路线 B 基建 + 路线 A 写时标记 + §12 inline fastpath)已全部落地并超出规格(struct 路径也做了 inline 位图循环 + 数组元素子路径)。本文基于当前代码逐层审视 IR 降级 → 发射层 fastpath → C++ slowpath,给出下一轮优化清单。
> 口径:以当前代码为准;case27 基线 ~5700 req/s(c10/p10K/60s/heap4gb),目标 6500。
> 关系:屏障侧优化是叠加项;到 6500 的主路径仍是 GC 频率(threshold 20MB 地板)与 MarkTerm STW(见 §6)。

---

## 目录

1. [近期改动史与教训(必读)](#1-近期改动史与教训)
2. [当前三层实现快照](#2-当前三层实现快照)
3. [成本模型:为什么 GOT 全局近乎免费而 per-P 镜像翻车](#3-成本模型)
4. [优化点清单(按收益排序)](#4-优化点清单)
5. [不建议动的项](#5-不建议动的项)
6. [与 P0/P1 的关系](#6-与p0p1的关系)
7. [实施顺序与验证口径](#7-实施顺序与验证口径)

---

## 1. 近期改动史与教训

按时间线,2026-07-09 ~ 07-10 发生的事:

| 事件 | 结论 |
| --- | --- |
| **跳转表截断 bug**(case27 100% 失败,`Invalid range start: 17 or end: 16`) | `getCangjieSpecificCallInstSizeInBytes` 漏报 WB wrapper 的内联展开尺寸(按 4B 计,实际 ~340-490B),`AArch64CompressJumpTables` 误选 8-bit 表,表项静默截断(0x171→0x71),`br` 落进展开序列中间。已修:ref 384B / struct 512B / arraycopy 64B 上界。**约定:任何新的 AsmPrinter 调用点展开必须同步登记尺寸。** |
| **P3**:struct/static-struct 的 StoreOnly 从 SlowCall 改为内联 memcpy | 保留。全部到达路径(gate==0、非堆 obj、size 0、无 ref 位图窗口、无 ref 数组组件)语义均恰为 memcpy。 |
| **P4**:ArrayCopy 两条 wrapper 启用 gate 分流(idle→内联 memmove;mark→Slow) | 保留。⚠️ 原先被屏蔽的两段全内联循环是**不安全死码**(把 raw old-ref 塞 activeLog_ 而不做 mark-CAS,违反路线 A"buffer 全灰"不变式),已删除;若要复活必须逐元素加 EmitInlineMarkOrTo。 |
| **P2 第一版(AllocBufferMS 镜像)→ 吞吐 -20%,已回退** | 见 §3,本文最重要的教训。 |
| **P2 第二版(TLS encodedGCPhase gate)** | 当前状态。gate = `LDRB [X28,#78]` + UNDEF→Slow + 区间判 [20,22]→mark,否则 StoreOnly。runtime 零改动(encodedGCPhase 本就由 `SetMutatorPhase→PublishThreadLocalGCPhase` 在每次握手维护)。 |
| **bitmap-word 批量 mark(2026-07-11)** | 已实现 fastpath + slowpath。slowpath 覆盖 WriteStruct/WriteStaticStruct/CopyRefArray/CopyStructArray；AArch64 short-GCTib struct/static-struct fastpath 做相邻两 ref 同 word 合并。实测吞吐**基本与优化前一致,没有明显收益**。 |
| **批量 mark 引入的尺寸估算回归** | pair fastpath 让 struct wrapper 最大展开接近 270 条指令，原 `getCangjieSpecificCallInstSizeInBytes=512B` 低估，重建 std.collection 报 `fixup value out of range`。已把 struct 保守上界提高到 **1536B**，原失败 bitcode 与 `cangjieCJNATIVECollection` 均验证通过。 |

### P2 第二版的正确性锚点(复核过,后续改动别破坏)

1. **stale phase 窗口安全**:termination STW 后 mutator TLS phase 仍是 21,但 `FlushAllWriteBarrierBuffers` 已把 `activeLog_` 置 null([MarkSweepCollector.cpp:1478](../../src/Heap/MCollector/MarkSweepCollector.cpp)),inline 在 mark-CAS **之前**就 bail 到 Slow,C++ 侧 `GetActiveCollector()==null` 丢弃 → 不设 stale mark bit、不漏不多。
2. **UNDEF(0)→Slow**:未注册 mutator 的线程交 C++ 兜底。
3. `encodedGCPhase@78` 与 `msAllocBuffer@88` 同一条 TLS line,该 line 纯本线程写(代设仅每周期每线程一次)→ 零新增一致性流量。
4. offset 78 由 [ThreadLocal.cpp:89](../../src/Mutator/ThreadLocal.cpp) static_assert 钉死;X28 高字节打包 phase(TBI)不影响 `[X28,#imm]` 寻址。

---

## 2. 当前三层实现快照

```
IR 层 (CJBarrierLowering.cpp):
  cj_gcwrite_ref/static_ref        → 无条件替换为 CJ_MCC_Write*Fast_MSGC wrapper 调用
  cj_gcwrite_struct                → wrapper(obj, dst, size, src, size)
  cj_gcwrite_static_struct         → wrapper(dst, size, src, size, gctib)
  cj_gcwrite_generic               → Step29: 按 struct 形式转 WriteStructFieldFast(obj, fieldPtr, size, box+8, size)
  cj_array_copy_ref/struct         → wrapper(6 args)
  读侧                              → 直接 load / memcpy(零屏障,不动)

发射层 (AArch64AsmPrinter.cpp::emitMccWriteBarrierFastPath, Mach-O + flag 开):
  gate:  LDRB W9,[X28,#78]; CBZ W9→Slow; SUB/CMP/B.HI→StoreOnly   ; 5 条,1 load,线程私有
  ref:   old 判空/同值(4) → GOT range(6) + 容器范围检查(4) → activeLog 链(4) + len/容量(3)
         → EmitInlineMarkOrTo(~45: region 查表/类型分流/R-2 倒数乘法 slotIdx/LDSETAL mark-CAS)
         → push(4) → store(1-2) → Fin
  struct: 头 LDARX → gctib bit63 分流(短型位图循环逐 ref inline mark / 长型数组子路径按组件
         HasRefField 分流)→ memcpy;StoreOnly=内联 memcpy(P3)
  arraycopy: gate 后 idle→内联 memmove,mark→Slow(P4)
  Slow:  BL wrapper

C++ slowpath (MSMarkBarrier.cpp / CompilerCalls.cpp):
  wrapper 体: stp x29,x30 / bl CJ_MCC_Write* / ldp / ret        ← 非尾调用,多一层帧
  MCC_WriteRefField: IsHeapAddress(obj) → Heap::GetBarrier() 虚调度 → WriteReference
  WriteReference: old==new 快出 → EnqueueRefFast(old) → field.Set
  EnqueueRefFast: 非堆丢 → GetActiveCollector()==null 丢 → GetOrCreateAllocBufferMS(每 ref TLS 查找)
                  → EnqueueRefInto: ShadeRefForBarrier(mark-CAS 赢者才 log,源头完美去重)
                  → activeLog_ 满则 PublishAndSwapLog(O(1) 甩块)→ objs[len++]=ref
  bulk 路径: ResolveBulkLogOwner 一次解析(R-1)后逐 ref EnqueueRefInto
```

**slow 出口何时被走到**(决定 slowpath 优化权重):buffer 满(1/1024);region 类型/表项异常;UNDEF 线程;struct 非 8 对齐、长型 gctib 且组件带 ref;arraycopy 的 mark 相全部。old 指向 LARGE 对象已不再固定 slow-fall,ref/static-ref 与 struct/static-struct 单槽路径可 inline mark `flags_`。

---

## 3. 成本模型

**P2 第一版 -20% 复盘**——"指令条数"是错误的成本货币,一致性流量才是:

| | GOT 全局(9 条链式 load) | AllocBufferMS 镜像(翻车) | TLS encodedGCPhase(现状) |
| --- | --- | --- | --- |
| line 归属 | 全进程只读热点,Shared 常驻每核 L1,**永不失效** | 与 `gcWork_.list1_` 同 line,**GC 线程偷工作高频远程写** | 与 `msAllocBuffer@88` 同 line,纯本线程写 |
| 地址生成 | ADRP PC 相对,零寄存器依赖,乱序核可提前几百条发射 | `[X28]→[buf,#off]` 相依链 | `[X28,#78]` 单条 |
| idle 相是否新增触碰 | 否 | **是**(以前 idle 不碰 per-P 内存) | 否(该 line 本来就在读) |

推论(后续所有屏障改动的判据):
1. 往屏障热路径加**任何**内存读之前,先问:这条 line 的写者是谁、写频多少?
2. 只读共享数据放 GOT 全局是**好**方案,不是待优化项。
3. 有历史实测记录的(如 LDARB vs LDRB),实测优先于直觉。

---

## 4. 优化点清单

### ① no-ref struct 写的 IR/前端消除 —— 复核后修正(2026-07-10)

**复核结论:no-ref 整体消除前端早已做了**,本节初稿"无条件生成 wrapper 调用"的说法不成立。前端
[CJNativeIRBuilder.cpp](../../../cangjie_compiler/src/CodeGen/CJNative/CJNativeIRBuilder.cpp) 在**所有静态类型已知的调用点**都先判
`IsTypeContainsRef`([CGUtils.cpp:315](../../../cangjie_compiler/src/CodeGen/Utils/CGUtils.cpp),精确递归:addrspace(1) 指针即 ref):

| 调用点 | 行号 | no-ref 时 |
| --- | --- | --- |
| 对象字段 struct 写 | 636-651 | **直接 CreateMemCpy,不发 intrinsic** |
| VArray 写 | 652-660 | 同上 |
| 数组字面量元素 | 1176-1190 | 同上 |
| 泛型装箱初始化 | 51-62 | 同上 |
| 全局变量写 | 609 | 含 ref 才走 static barrier |

因此到达 CJBarrierLowering 的 `cj_gcwrite_struct` 只剩三类,剩余优化按此细分:

**(a) 含 ref 的静态 struct**(String{ref,start,len}、含 String 的业务 struct、Option 装箱)——JSON 最热;
**(b) 泛型/动态 size**(CJNativeIRBuilder.cpp:585-630,`GetSize()` 为空,类型运行时才知)——静态消除不可行,现状 fastpath 动态分流(数组子路径 HasRefField、P3 idle memcpy)已是合理形态;
**(c) 堆到堆 struct 拷贝对**(644-645):gcread_struct 到栈 temp + gcwrite_struct,双拷贝 + alloca。

#### ①a [大收益] 含 ref 小 struct 拆分(scalarize)

针对 (a)。String 赋值现在走完整 struct fastpath(gate + LDARX 头 + gctib 提取 + 对齐检查×2 +
3-slot 位图循环 + activeLog + BL memcpy);拆成:

```
gcwrite_ref(data 字段)   ; ref fastpath,自带 old==null/same-value 短路(struct 路径无 same-value 短路)
plain store(start)
plain store(len)
```

省掉头查表、位图循环骨架、memcpy 调用整段。判据:**常量 size ≤ ~32-48B 且 ref 字段数 ≤ 1**
(String 恰命中;≥2 个 ref 字段会重复 gate,需实测定界)。实现位置:CJBarrierLowering 单点
(拿 `GCInfo.getOrInsertTypeGCInfo(ST)` 位图逐字段拆),前置依赖 ①c。

> 状态更新(2026-07-20):**已移除**。2026-07-11 版曾利用 `AggType` metadata，
> 在 `CJBarrierOpt` 对“恰 1 个直接 ref 字段”的小 struct 做 scalarize，生成
> `gcwrite_ref(ref field) + 前后缀 memcpy/plain store`。case27 口径下效果
> **基本与优化前一致**，现已删除 `scalarizeOneRefStructBarrier()` 及辅助逻辑，
> 恢复保留整条 `gcwrite_struct` 屏障。`AggType` metadata 本身不因此结论自动删除。

#### ①b [中收益] gcread+gcwrite 对融合

针对 (c)。MSGC 读侧零屏障,堆到堆拷贝可直接 `gcwrite_struct(dstObj, dstPtr, srcHeapPtr, size)`
——barrier 语义只关心 dst 旧值,src 从堆直读等价,省一个 alloca + 一次栈拷贝。实现放
CJBarrierLowering(已按 MSGC 分支;**CopyGC 不能这么干**,读屏障有语义):pattern-match
「gcread_struct 的 dst 是 alloca、唯一 use 是相邻 gcwrite_struct 的 src、中间无安全点」。

#### ①c [一行,①a 前置] 给 cj_gcwrite_struct 补 AggType metadata

[CallGCWriteAgg](../../../cangjie_compiler/src/CodeGen/CJNative/CJNativeIRBuilder.cpp)(467-482)现在不挂
metadata(只有 static 版挂,verifier 也只对 static 版强制)。补上后 ①a 在 CJBarrierLowering
单点实现即可覆盖所有前端调用点,verifier 可同步加强制。

#### ①d [小收益] 泛型动态分流去调用

CJNativeIRBuilder.cpp:613-630 的 `CreateTypeInfoIsReferenceCall` 每次泛型成员写付一次运行时调用
做 ref/非 ref 分流;可换成 inline 读 TypeInfo flag 位(fastpath 的 LArrayElem 子路径已在读
offset 9 的同类位)。属 Step29 域的延伸。

**顺序**:①c(一行铺路)→ 编译 std/stdx 统计剩余 `cj_gcwrite_struct` 的 AggType 分布
(确认 String 占比、定 ①a 阈值)→ ①a → ①b。
**验证**:每步 `CJ_MS_VERIFY_MARK=1` + case27 对比;①a 注意拆分后 ref 字段写仍必须保持
原有内存序(plain store 即可,SATB 按字段独立无原子性要求)。

### ② [大收益,先验证不变式] 初始化写消除(init store elision)

**原理**:SATB 删除屏障只保护"被覆盖的旧值"。分配保证清零(现有 fastpath 的 old load 依赖这一点),所以**新分配对象的首次字段写 old==0 → 语义等价于裸 store**,连 gate 都不需要。

**改法**(IR 层,新 pass 或并入 CJBarrierLowering 前置):store 的基对象由同函数内 `CJ_MCC_NewObject` 结果直接支配、且分配点到 store 之间无安全点/无该字段别名写 → `cj_gcwrite_ref` 替换为 plain store。JSON 反序列化 = 海量新对象字段初始化,这可能是剩余屏障中占比最大的一类。

**⚠️ 前置验证(硬门槛)**:
1. 普通对象确为**白分配**(Step 记录:`MObject.cpp:32-34` 等处 `MarkNewObject` 已注释,仅 finalizer 路径保留)——白分配 + SATB 删除屏障下,新对象可达则 termination 前必被扫,init store 无需入 log ✓;若未来改回黑分配(alloc-black),init store 需要 insertion 语义,本优化必须同步回退。
2. 分配路径清零保证覆盖所有 size class(bump 与 bitmap 复用槽都要)。
3. `CJ_MS_VERIFY_MARK=1` 全绿后再放量。

> 状态更新(2026-07-20):**已移除**。2026-07-11 版曾在 `CJBarrierOpt` 识别
> fresh-init barrier，将部分 `cj_gcwrite_ref` / `cj_gcwrite_struct` 降为 plain store / memcpy。
> 该实验效果**基本与优化前一致**，现已删除 `optFreshInitBarrier()`、
> `preprocessBarriers()` 及辅助逻辑，fresh 新对象的首次写恢复保留原 barrier。

### ③ [中收益,一行改动] wrapper 尾调用化

**现状**:wrapper 体 `stp x29,x30 / bl CJ_MCC_Write* / ldp / ret`(反汇编确认),所有 slow 出口都多付一层帧。

**改法**:[CJBarrierLowering.cpp:349/394](../../../cangjie_compiler/third_party/llvm-project/llvm/lib/CodeGen/CJBarrierLowering.cpp) 两处 `Builder.CreateCall(Callee, Args)` 后加 `SlowCall->setTailCall(true);` → wrapper 变单条 `b _CJ_MCC_Write*`。
**注意**:确认 wrapper 与 runtime 函数 CC 一致(都是 C)且无 GC 语义阻止尾调用;非 Mach-O 平台(wrapper 是唯一路径)同样受益。

### ④ [中收益] slowpath 去虚调度

**现状**:`MCC_WriteRefField`([CompilerCalls.cpp:283](../../src/CompilerCalls.cpp))→ `Heap::GetBarrier()` 虚表 dispatch → `WriteReference`。大对象 old-ref(markBitmap==null → inline 必 Slow)让这条链在大 payload 下变热。

**改法**:`MRT_USE_MARK_SWEEP` 编译期已定,直连:
```cpp
extern "C" void MCC_WriteRefField(const ObjectPtr ref, const ObjectPtr obj, RefField<false>* field)
{
#ifdef MRT_USE_MARK_SWEEP
    if (UNLIKELY(!Heap::IsHeapAddress(obj))) { field->SetTargetObject(ref); return; }
    BaseObject* old = field->GetTargetObject();
    if (old != ref) { EnqueueRefFast(old); }   // 内部自判 marking 与否
    field->SetTargetObject(ref);
#else
    ... 现状虚调度 ...
#endif
}
```
即把 `MSMarkBarrier::WriteReference` 的逻辑上提内联,省虚表跳转 + 一层调用。idle 相语义不变(EnqueueRefFast 里 `GetActiveCollector()==null` 丢弃 = MSIdleBarrier 行为)。WriteStruct/StaticRef/ArrayCopy 同法。
**注意**:需确认 stwBarrier(unknown phase 保守分支)没有额外语义依赖虚调度。

### ⑤ [中收益,必须 A/B] 两处 LDARX → LDR

struct 路径读 obj 头与 `EmitInlineMarkOrTo` LTypeOk 读 old 头都是 load-acquire。TypeInfo 分配后不可变,acquire 防的是"读到未发布对象的头"。若对象发布(store ref)自带 release 序,可放宽为 plain LDR。
**鉴于 gate LDARB 的历史实测教训(§3 判据 3),此项禁止拍脑袋,必须 A/B 定夺后才能合入。**

### ⑥ [小收益] EnqueueRefFast 的 TLS 查找瘦身

单 ref 路径每次 `GetOrCreateAllocBufferMS()`(TLS 读 + create 分支);改 `GetAllocBufferMS()` + null 时冷路径 create。bulk 已有 R-1,单 ref 补齐。

### ⑦ [已优化,实测无明显收益] struct/array 按 mark bitmap word 批量 OR

**目标**:原实现对每个 old ref 分别做 region/slot 定位和一次 atomic OR。对于一次覆盖多个引用的
struct/array barrier,将落在同一 `markBitmap[wordIdx]` 的 bit 合并为 mask:

```text
oldWord = atomic_fetch_or(wordAddr, combinedMask)
newBits = combinedMask & ~oldWord
```

只有 `newBits` 对应的对象进入 `activeLog_`,继续保持路线 A 的“只有本次 mark winner 才入 log”不变式。

**slowpath 已实现**:
- `MSRegionInfo::MarkBitmapWord()` 提供 relaxed atomic OR 并返回本次新置位 bits;
- `BulkRefMarker` 用 2 个 word entry 做固定容量聚合,不分配内存;
- 覆盖 `WriteStruct`、`WriteStaticStruct`、`CopyRefArray`、`CopyStructArray`;
- LARGE/SPAN/无效对象仍回原 `ShadeRefForBarrier`,单 ref/atomic barrier 不变。

**fastpath 已实现**:
- `EmitInlineMarkOrTo` 拆成 descriptor 计算与 atomic commit 两段;
- dynamic/static short-GCTib struct 循环对相邻两个 ref 尝试聚合;
- 同 word 合并 mask 后发一次 `LDSETALX`,不同 word 分别提交;相同 bit 去重;
- 任一 descriptor 校验失败都发生在 mark 前,回原单槽 inline 循环;mark 后不再 slow-fall。
- arraycopy 在 mark phase 仍回 C++ slowpath,因此通过 `BulkRefMarker` 获得批量处理。

**效果**:case27/当前吞吐口径下与优化前基本一致,没有观察到明显提升。推测原因是实际 workload 中
单次 struct barrier 多为 String/one-ref struct(当时 scalarize 实验使更少请求进入本路径),而 old refs 指向的对象
未必集中在同一 bitmap word;arraycopy mark-phase 占比也不足。该优化减少 atomic 次数的理论收益没有成为
当前主瓶颈,主矛盾仍是 GC 频率、MarkTerm 和整体标记工作量。

**配套修复**:fastpath 展开增大后必须同步更新 AArch64 size accounting。struct wrapper 上界已从
`512B` 提高到 `1536B`;否则 std.collection 这类大函数会在 llc 阶段报三次
`<unknown>:0: error: fixup value out of range`。

#### ⑦a [已优化] LARGE old-ref inline mark

small-object descriptor 遇到 `MS_REGION_LARGE` 时转入独立 inline 路径:从当前 region entry 的
`reserved` 解析 primary `MSRegionInfo`,校验 primary 类型、`allocPtr == oldRef`、`regionEnd != 0`
和对象 TypeInfo 后,对 `flags_ + 51` 的 `FLAG_MARKED` 执行 `LDSETALB`。只有本次赢得 white→grey
的 oldRef 才进入 `activeLog_`;已 marked 直接 StoreOnly。覆盖 ref/static-ref 以及
struct/static-struct 单槽循环;pair descriptor 遇 LARGE 会先回单槽处理;arraycopy mark phase 保持
runtime。配套将 ref wrapper size accounting 从 `384B` 提高到 `512B`,struct 仍为 `1536B`。

#### ⑧ [已优化] slowpath 出口瘦身

- LLVM 生成的六类 MSGC wrapper 将最终 runtime call 标记为 `tail`,AArch64 最终形态由
  `BL MCC_*; RET` 收敛为直接 `B MCC_*`,省去 wrapper 栈帧和返回链。
- `MCC_WriteRef/Struct/Static/ArrayCopy*` 在 MarkSweep 构建下直接调用共享的非虚
  `MSBarrierOps`,不再经过 `Heap::GetBarrier()`、vtable load 和 indirect branch;原
  `MSMarkBarrier` 虚接口也转调同一实现,避免两套屏障语义漂移。CopyGC 路径保持原状。
- ref 与 bulk owner 获取先内联读取 `ThreadLocal::GetAllocBufferMS()`,仅 TLS 为空时进入
  `GetOrCreateAllocBufferMS()` 冷创建路径。
- 增量构建通过;runtime 反汇编确认 `MCC_WriteRefField` 和 array-copy 等入口直接尾跳
  `MSBarrierOps`,LLVM IR/汇编确认 wrapper 为 `tail call`/`b MCC_*`。该项减少 slow
  出口固定成本,不减少 slowpath 调用次数,实际吞吐效果需后续 case27 跑数确认。

---

## 5. 不建议动的项

| 项 | 理由 |
| --- | --- |
| mark 路径 heap range 的 6 条 GOT load | §3:GOT 近乎免费;搬 per-P 已实测 -20% |
| gate 结构(TLS encodedGCPhase 5 条) | 刚收敛,正确性锚点见 §1 |
| capacity 检查移位/合并 | 等价变换,无收益 |
| 读屏障 | 已是零成本直读 |
| ArrayCopy 全内联循环复活 | 死码已删;违反路线 A 全灰不变式,除非逐元素加 inline mark |

---

## 6. 与 P0/P1 的关系

22:37 validate run(5599 req/s)的 gc.log 记账:GC 周期 ~5.9ms 背靠背(155 次/s),STW ≈ 20% 墙钟(MarkTerm 932us + PreEnum ~100us + EnumRoots 251us),每周期把 6 万活对象整图重标一遍。

- **P0**:threshold 被 [MarkSweepCollector.cpp:2074](../../src/Heap/MCollector/MarkSweepCollector.cpp) 的 `min(gcThresholdCap, 20MB)` 硬地板托死(trueLive 5.5MB × 2 = 11MB < 20MB),4GB 堆下每 20MB 分配触发一轮。地板改可配(如 128MB)→ GC 频率降 ~6×,预计单独接近 6500。
- **P1**:MarkTerm 932us STW 内处理 25k 对象(`WriteBarrierFlush forwarded=26287, overflow=23552`)——published 日志块没在并发期消费完。方向:termination 前加并发 drain 轮/bg 线程优先吃 globalFull_。

**本文 ①-⑥ 是叠加项;P0/P1 才是主路径。** 且 P0 落地后 idle 相占比大增,①(no-ref struct)与 P3(idle struct 内联 memcpy)的权重进一步上升。

---

## 7. 实施顺序与验证口径

建议顺序(每步独立可测、可回退):

| 步 | 内容 | 改动面 | 风险 |
| --- | --- | --- | --- |
| 1 | ③ wrapper 尾调用化 | ✅ 已完成 | 低 |
| 2 | ①c 补 AggType metadata + 统计剩余 gcwrite_struct 类型分布 | ✅ 已完成 | 低 |
| 3 | ①a 含 ref 小 struct 拆分(String 优先) | 已移除(效果基本一致) | 低-中(纯编译期,需按位图逐字段验证) |
| 4 | ④ slowpath 去虚调度 + AllocBuffer TLS fast-get | ✅ 已完成 | 中 |
| 5 | ①b gcread+gcwrite 对融合 | CJBarrierLowering.cpp | 中(仅 MSGC 分支) |
| 6 | 跑数评估,再决定 ②(init store elision,先验证白分配/清零不变式)与 ⑤(LDARX A/B) | ② 已移除(效果基本一致);⑤ 待定 | ② 中高 / ⑤ 未知 |
| 7 | ⑦ bitmap-word 批量 mark(fastpath + slowpath) | ✅ 已完成,效果基本一致 | 中(发射层尺寸/控制流 + Route-A winner 语义) |

> 注:① 初稿设想的"no-ref struct 整体消除"经复核**前端已实现**(见 §4-①),此表已改为剩余细化项 ①a/①b/①c。

每步验证链(同 Step26 §9.4):改 → 重建 compiler(动 llvm 时)→ 重建 runtime(`--gc-mode marksweep`)→ 新 cjc 重建 std → 组 SDK → 重建 stdx → case27。

判据:
1. `CJ_MS_VERIFY_MARK=1` 干净(每步必测);
2. case27 c10/p10K 多轮无异常(SIGSEGV / bad json / invalid range),吞吐相对 5700 基线的增量;
3. 动发射层时:反汇编抽查 + 复核 `getCangjieSpecificCallInstSizeInBytes` 上界仍成立(ref ≤ 512B / struct ≤ 1536B / arraycopy ≤ 64B);本次已用原失败 `std.collection.opt.bc` 验证;
4. ① 加编译期命中统计(消除了多少条 gcwrite_struct),确认收益归因。

---

## 结语

Step37 把屏障结构对齐到了 Go(写时 shade + O(1) 甩块 + 消费端纯 scan);Step38 的主题是**少执行屏障**(①②:编译期消除)与**让必须走的 slow 链更短**(③④),外加一条用 -20% 学费换来的成本模型(§3):**屏障热路径上,一致性流量 > 指令条数;只读共享数据留在 GOT,线程私有状态放 TLS,谁都别碰会被别的核写的 line。**
