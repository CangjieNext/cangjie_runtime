# Step29：MSGC `Barrier::WriteGeneric` memcpy 热点定位与 LLVM 消解方案

> 2026-06-22
> 目标：整理 case28 中“吞吐未随 `gcread_generic` helper 消除而显著提升”的实测数据、根因定位，以及下一步最值得落地的 LLVM 优化方案。

> 当前前提：本文直接以当前 `llvm-project` 分支“整体服务 MSGC”作为默认前提，不额外引入新的 MSGC 编译器开关。

---

## 目录

1. [问题背景](#1-问题背景)
2. [实测数据](#2-实测数据)
3. [问题定位](#3-问题定位)
4. [核心结论](#4-核心结论)
5. [推荐优化方式](#5-推荐优化方式)
6. [实施顺序](#6-实施顺序)
7. [验证口径](#7-验证口径)

---

## 1. 问题背景

Step28 的重点是把 `cj_gcread_generic` 中能证明为纯拷贝的路径尽量在 compiler 侧消掉，
避免继续走 runtime helper。

但在 case28 的实际压测里，虽然“读 helper 调用量”已经明显下降，吞吐却没有出现与
“少掉上亿次 runtime 调用”相匹配的跃升，因此需要继续确认：

1. 现在剩余的大头到底是不是 `CJ_MCC_ReadGeneric`。
2. 如果不是，它实际卡在 runtime 的哪一类屏障/helper。
3. 下一步优化应该继续做 read fastpath，还是应该转去做 write generic 消解。

这一步排查的关键目标不是“继续假设读屏障是热点”，而是：

> 用带计数的 runtime 数据把热点 helper 精确锁定下来，再决定 LLVM 该优先消哪一类调用。

## 2. 实测数据

### 2.1 正常对比跑：case28 c10 p10K

使用重新构建后的 MSGC SDK 做正式三者对比，结果目录：

- `MSGC_RESULT/Result/case28_http_complex_json/result/20260622_152217_rebuilt_msgc_vs_copygc_go_c10_p10K_compare/`

其中 `compare_report.md` 给出的吞吐是：

| 侧别 | 吞吐 |
| --- | ---: |
| CopyGC | 4964 req/s |
| MSGC | 7534 req/s |
| Go | 9322 req/s |

这说明 rebuilt MSGC 在该组 case 上已经明显快于 CopyGC，但仍没有出现“因为 generic
helper 大幅减少而进一步跳升”的现象。

### 2.2 轻量计数跑：`READGENERIC_STATS` 已经归零

结果目录：

- `MSGC_RESULT/Result/case28_http_complex_json/result/manual_msgc_output_writestats_c10_p10K_20260622_163343/`

对应 `stdout.txt` 中可见：

- `throughput=7643 req/s`
- `[READGENERIC_STATS] total=0`

这条数据很关键，它说明在当前 case28 c10 p10K 场景里：

> runtime 层的 `CJ_MCC_ReadGeneric` / `Barrier::ReadGeneric` 已经不是主要热点了。

也就是说，“Step28 做掉的那部分读 helper”在这个场景下已经基本退出主战场。

### 2.3 深度 barrier 计数跑：真正爆量的是 `Barrier::WriteGeneric` 的 memcpy 分支

为了进一步确定吞吐没有同步提升的原因，在 runtime 里把 MSGC 读写屏障与
`Barrier::WriteGeneric` 分支都加了计数，结果目录：

- `MSGC_RESULT/Result/case28_http_complex_json/result/manual_msgc_output_barrierstats_debug3_20260622_190113/`

对应 `stdout.txt` 的关键数据如下：

| 统计项 | 调用次数 | 字节数 |
| --- | ---: | ---: |
| `base_write_generic_memcpy` | 569,089,392 | 632,944,256 |
| `base_write_generic_readstruct` | 68,385 | 547,080 |
| `base_write_generic_writestruct` | 75,491 | 663,499 |
| `ms_idle_write_struct` | 8,362 | 76,753 |
| `ms_idle_copy_struct_array` | 45,584 | 21,015,696 |
| `ms_mark_write_struct` | 2,016 | 17,752 |
| `ms_mark_copy_struct_array` | 11,932 | 5,548,904 |
| `ms_mark_write_reference` | 754 | 0 |

同一份结果里还有两个结论性统计：

- `[READGENERIC_STATS] total=0`
- `[MSGC_BARRIER_TOTAL] write_calls=569,316,184`

这说明：

1. 当前主要爆量点不是 read generic。
2. 当前绝对主热点是 `Barrier::WriteGeneric` 里“进去以后只做 `memcpy`”的那条分支。
3. 真正落到 MSGC old-edge 删除屏障语义的那部分写路径，数量其实很小。

### 2.4 关于带重计数版本的吞吐数字

`manual_msgc_output_barrierstats_debug3_20260622_190113` 这一轮因为在 runtime 热路径中增加了
大量原子计数，`stdout.txt` 中吞吐只有 `567 req/s`。

这个数字只用于“定位哪类 helper 在爆量”，不用于和正式性能结果做横向对比。

## 3. 问题定位

### 3.1 runtime 侧真实热点不在 `ReadGeneric`

当前 `Barrier::WriteGeneric()` 的逻辑分 3 条路：

1. `obj` 无 ref 字段，或 `obj/src` 都不在 heap 上
   - 直接 `memcpy`
2. `obj` 不在 heap、`src` 在 heap
   - 调 `ReadStruct`
3. `obj` 在 heap
   - 调 `WriteStruct`

case28 的深度计数结果表明，绝大多数调用都落在第 1 条：

> 进入了 runtime generic write helper，但很快被判定为“不需要真正的 MSGC 写屏障”，最后只做了一次 payload `memcpy`。

所以这里“爆量”的是：

- `runtime helper call` 数量

而不是：

- `MSGC deletion barrier` 数量

### 3.2 为什么吞吐不会随着“少掉很多读 helper”而线性提升

因为当前 case28 中真正仍然爆量的并不是 `ReadGeneric`，而是 `WriteGeneric` 的 memcpy 分支。

换句话说，之前消掉的主要是：

- `read helper`

而现在压住 CPU / 指令条数的主要是：

- `write helper -> runtime branch -> memcpy`

因此吞吐不会因为“读 helper 少了”就自动出现大幅提升，除非继续把这批
`write generic` 调用也在 compiler 侧消掉。

### 3.3 为什么这些调用没有在 LLVM 里提前消失

当前 compiler 里，`CJGenericIntrinsicOpt.cpp` 已经具备把

- `cj_assign_generic`
- `cj_gcread_generic`
- `cj_gcwrite_generic`

直接改写成 `llvm.memcpy` 的能力。

但它目前主要命中的条件仍偏保守，典型是：

1. 栈到栈纯拷贝
2. 类型信息可静态证明“不含 ref”
3. `gcread.generic` 的某些 fresh-dst 场景

而 case28 里大量 generic 写路径虽然最终在 runtime 里只走到 `memcpy`，
却没有在 LLVM 的更早阶段被证明为“可以直接 memcpy”，于是一路保留成
`CJ_MCC_WriteGeneric` 调用。

### 3.4 `CJBarrierLowering` 目前也没有接手 `gcwrite.generic`

当前 MSGC AArch64 写屏障 lowering 主要覆盖的是：

- `cj_gcwrite_ref`
- `cj_gcwrite_static_ref`
- `cj_gcwrite_struct`
- `cj_gcwrite_static_struct`
- `cj_array_copy_ref`

`cj_gcwrite_generic` 并没有接入这套发射层 fastpath。

因此只要它没在前面的 `CJGenericIntrinsicOpt` 中被消掉，后面大概率就会继续保留为
runtime helper 调用。

## 4. 核心结论

### 4.1 这次的主问题已经不是读屏障

结合 case28 c10 p10K 的计数数据，可以把问题重新表述为：

> `gcread_generic` 相关 helper 已经基本退出热点，但 compiler 仍在为大量“最终只需要 memcpy 的 generic 写”生成 runtime helper 调用。

### 4.2 爆量的是“generic write helper 开销”，不是“真正的 MSGC 删除屏障开销”

`base_write_generic_memcpy` 达到 5.69 亿次，而真正的 `WriteStruct` /
`WriteReference` 级 MSGC old-edge 保护次数只有万级到千级。

因此下一步最值得优化的不是继续盯着 `ReadGeneric`，而是：

> 尽可能把这些“最终只会落到 `Barrier::WriteGeneric` memcpy 分支”的 generic 写，在 LLVM 里提前改写成 `memcpy`。

### 4.3 第一优先级应该放在 `CJGenericIntrinsicOpt`

原因有 3 个：

1. 它已经具备 `generic intrinsic -> memcpy` 的现成能力。
2. 它运行得足够早，做掉后后续 `CJBarrierLowering` 根本看不到这条 generic helper。
3. 它更适合利用 `alloca` / fresh allocation / MemorySSA 这类 IR 级事实来证明“这里不需要 runtime helper”。

## 5. 推荐优化方式

### 5.1 Phase 1：`gcwrite.generic` 的 stack-dst 无条件编译期 `memcpy`

这是当前最值得先做的一步。

#### 5.1.1 目标

当 `gcwrite.generic` 的 `dst` 底层对象是 `AllocaInst` 时，直接在
`CJGenericIntrinsicOpt.cpp` 中把它改成 `llvm.memcpy`，不再保留为
`CJ_MCC_WriteGeneric`。

#### 5.1.2 为什么语义上成立

对于当前 MSGC 语义，这样做是成立的，原因是：

1. 栈上的 generic box 是根对象的一部分，会在 safepoint/root scan 中被直接扫描。
2. 覆盖一个栈槽不会丢失 heap old-edge，因此不需要 deletion barrier。
3. runtime 里 `WriteGeneric(obj非heap, src为heap)` 会走 `ReadStruct`；
   而 MSGC 的 `ReadStruct` 在 idle/mark 阶段本身就是 `memcpy`。

也就是说，对“栈 dst”这类路径，继续保留 runtime generic helper 只是多了一层：

- helper call
- heap 判定 / 分支
- 再退回 `memcpy`

这部分完全可以在 compiler 侧提前抹平。

#### 5.1.3 为什么不需要再要求 `memset` / fresh / no-clobber

对 stack dst 来说，当前若仍要求：

- 先证明 fresh
- 或先找到 zero-memset
- 或要求中间无 clobber

其实比必要条件更严格。

因为这里关心的不是“dst 里旧值是不是 0”，而是：

> 覆盖一个栈上的 generic payload，是否需要像 heap object 一样做 old-edge 保留。

答案是不需要。

所以只要 `getUnderlyingObject(Dst)` 是 `AllocaInst`，就可以直接 lower 成 `memcpy`。

### 5.2 Phase 2：fresh heap dst 的 `gcwrite.generic -> memcpy`

在 stack dst 做完之后，下一步可以扩到：

- `dst` 来自块内 fresh heap allocation
- 分配点到当前 `gcwrite.generic` 之间没有 clobber dst payload

这和当前 `gcread.generic` 的 fresh-dst folding 思路是一致的，只是对象从
read generic 扩展到 write generic。

这一步仍然适合放在 `CJGenericIntrinsicOpt.cpp`，因为：

1. 这里已经有 `findFreshHeapAllocInBlock()` / `hasFreshDstClobberInBlock()` 一套现成设施。
2. 它天然适合利用 `new_gen` + `NoAlias` + basic block 顺序来做保守证明。

### 5.3 Phase 3：若还剩很多 memcpy 分支，再考虑 `gcwrite.generic` 的 lowering fastpath

如果 Phase 1 / Phase 2 做完后，`base_write_generic_memcpy` 仍旧很高，说明还有一类路径：

- 编译期难以证明
- 但运行时一进 `Barrier::WriteGeneric` 就会快速判成 `memcpy`

这时才值得继续做第三步：

> 在 `CJBarrierLowering` / AArch64 发射层补 `cj_gcwrite_generic` 的 MSGC fastpath。

也就是把 runtime 当前 `WriteGeneric` 的前置判断部分挪到 codegen 侧，让更多 callsite
直接在机器码层完成 `memcpy`，只把真正需要 `WriteStruct` 的路径留给 slow path。

## 6. 实施顺序

推荐顺序如下：

1. 先改 `CJGenericIntrinsicOpt.cpp`
   - 为 `cj_gcwrite_generic` 增加 `stack dst -> memcpy` 的直接消解
2. 增量重编 `cjc`
   - 不做 clean，全量编译成本太高
3. 重新编译并安装 `stdlib`、`stdx`
   - 用新 compiler 产物把 SDK 重新拼齐
4. 重新编译 case28 benchmark
   - 必须用新 SDK 重新编译，不能复用旧 binary
5. 跑 case28 c10 p10K
   - 先看功能是否正确
   - 再看 `base_write_generic_memcpy` 是否明显下降
6. 若下降明显但吞吐收益仍不够，再做 Phase 2 fresh heap dst
7. 若 Phase 2 后仍剩大量 memcpy 分支，再评估是否补 `gcwrite.generic` lowering fastpath

这样做的好处是：

1. 每一步都能用 barrier stats 单独验证收益。
2. 可以先吃掉“最硬、最安全”的 stack 证明子集。
3. 避免一开始就把 `CJBarrierLowering` 和发射层改复杂。

## 7. 验证口径

### 7.1 功能正确性

至少验证：

1. `hello` 能正常编译运行
2. case28 benchmark 能正常编译运行
3. c10 p10K 60s 跑完无 crash、无明显语义回归

### 7.2 barrier 计数

重点看下面几项：

1. `base_write_generic_memcpy`
   - 这是 Phase 1 / Phase 2 的主目标，应显著下降
2. `base_write_generic_readstruct`
   - 允许保留，说明仍有 nonheap-dst / heap-src 场景
3. `base_write_generic_writestruct`
   - 允许保留，说明仍有真正 heap-dst 需要 old-edge 保护的路径
4. `READGENERIC_STATS`
   - 继续确认 read helper 没有重新回流

### 7.3 性能

正式性能比较时，不使用带重原子计数的 debug runtime。

推荐口径：

1. 同一组 case28 c10 p10K
2. 同样的 60s 持续时长
3. 同样的 SDK/stdlib/stdx 组合方式
4. 记录吞吐、RSS、GC 时间

### 7.4 预期变化

如果 Phase 1 命中率足够高，预期现象应是：

1. `base_write_generic_memcpy` 明显下降
2. 真正 `WriteStruct` / `WriteReference` 类屏障计数变化不大
3. 吞吐有提升，且提升来源主要是“少掉了 runtime generic helper 调用开销”

若只看到 `base_write_generic_memcpy` 下降而吞吐几乎不动，则说明：

- 新热点可能已转移到别的 generic helper
- 或热点主要在 JSON/stdlib 其他对象构造与 copy 路径

那时再继续做下一轮 profiling / barrier 分类即可。

---

## 8. 修正：memcpy 分支归因、优先级重排与 Phase 3 落地方案（2026-06-22 复核）

> 本节基于对 `runtime/src/Heap/Barrier/Barrier.cpp` 实际代码与 compiler 侧 `CJBarrierLowering` / `CJGenericIntrinsicOpt` 的复核，对前文 §3 / §5 / §6 做关键更正。原文保留，更正集中放在这里。

### 8.1 memcpy 分支的真实条件不是「stack-dst」

前文 §3.1 把 memcpy 分支描述为「obj/src 都不在 heap」。实际 `Barrier::WriteGeneric()` 的条件是（[Barrier.cpp:248](../../../src/Heap/Barrier/Barrier.cpp)）：

```c
if ((obj != nullptr && (obj->GetTypeInfo() == nullptr || !obj->HasRefField())) ||
    (!Heap::IsHeapAddress(obj) && !Heap::IsHeapAddress(src))) {
    // memcpy   ← 5.69 亿落在这里
}
```

即 memcpy 分支同时命中两类**完全不同**的情况：

- **(A)** `obj` 在 heap、但 **`!obj->HasRefField()`**（整个目标对象没有 ref 字段，例如装 Int/Float/纯值 struct 的 box/array）。
- **(B)** obj、src 都在栈（stack-to-stack）。

旁证：ReadStruct 分支(nonheap-obj+heap-src)只有 6.8 万、WriteStruct 分支(heap-obj+有ref)只有 7.5 万。若 obj 大量在栈，混合的 stack-obj/heap-src 会进 ReadStruct 分支，但它才 6.8 万。因此 **5.69 亿几乎必然由 (A) heap-obj-无-ref-字段 主导**，而非 (B) 栈。case28 是 JSON 堆树 + 海量原始值写入，完全符合 (A)。

**结论性更正：前文 §5.1 的 Phase 1（stack-dst 编译期 memcpy）只覆盖 (B)，对主峰 (A) 基本无效。**

### 8.2 实施前必须先拆分 `base_write_generic_memcpy` 计数

在写任何 Phase 代码前，把该计数按子条件拆三路，否则无法判断 Phase 1/2 是否有用：

1. heap-obj & `!HasRefField()`（预期占绝大多数 = 主峰 A）
2. stack-obj & stack-src（= B）
3. `GetTypeInfo()==nullptr` 守卫路径

这一个数据点直接决定优先级。强先验：(1) 占 95%+。

### 8.3 这些为什么活到 runtime（更正 §3.3）

现有 `CJGenericIntrinsicOpt::lowerGenericBarrier` **已经**会把「TypeInfo 静态可证无 ref」的 generic 写降成 memcpy。所以残留的 5.69 亿是**运行期泛型 T、编译期拿不到 TypeInfo**的那批——编译器对 runtime-generic-T **证明不了**无 ref。这才是它们活到 runtime 的真正原因；不是「再加点编译期证明」能解决的。

### 8.4 优先级重排：Phase 3 才是主杠杆，Phase 1/2 退化为锦上添花

**Phase 1（stack-dst 编译期 memcpy）—— 锦上添花。** 只覆盖 (B)（极小头）；且 Phase 3 在运行期对栈 obj 也兜得住（heap-range check 失败 → StoreOnly → memcpy）。其唯一增量是对可证明的栈 alloca 省掉内联前导。

**Phase 2（fresh heap dst 编译期 memcpy）—— 大部分被 Phase 3 吞掉，只留一条缝。** 区分目标对象有无 ref：

| dst 情况 | Phase 3 行为 | Phase 2 是否还有增量 |
| --- | --- | --- |
| heap、**无 ref**（主峰 A） | bitmap 全 0 → **全相位 StoreOnly → memcpy** | 无 |
| heap、有 ref、fresh、IDLE/SWEEP | StoreOnly → memcpy | 无 |
| heap、有 ref、fresh、**MARK** | 读 GCTib → 枚举 dst 旧 ref（fresh 即 null，被 guard 空转）→ memcpy | **有**：Phase 2 证 fresh 后直接 memcpy，省掉这趟扫描 |

即 Phase 2 唯一未被吞掉的是「fresh + 有 ref + 正处 MARK」这一小格；而主峰 (A) 是无 ref，Phase 3 全相位 StoreOnly，根本不进枚举。所以在 case28 归因下这条缝也偏小。

**两个会让 Phase 1/2 重新变重要的前提（需验证）**：
1. 若 §8.2 拆分显示「fresh + 有 ref」占比不小 → Phase 2 的 MARK 缝有量。
2. **非 AArch64 目标**：Phase 3 的内联 fast wrapper 只在 AArch64 生效，x86 仍是 helper-call；Phase 1/2 是编译期彻底消除、全平台有效。若 x86 MSGC 是目标，Phase 1/2 价值回升。

在「(A) 主导」成立时：**Phase 3 = 主杠杆，Phase 1/2 = 可选收尾。**

### 8.5 Phase 3 具体实现方案（推荐主攻）

思路：把 `cj_gcwrite_generic` 接进 Step28 已落地的 `CJ_MCC_WriteStructFieldFast_MSGC` 内联 fastpath，与 Step28 把 `gcread_generic` 改写成 WriteStruct **完全同构**。

#### 8.5.1 参数映射

arg 布局（[CJIntrinsics.h:61](../../../../cangjie_compiler/third_party/llvm-project/llvm/include/llvm/IR/CJIntrinsics.h)）：`GCWriteGeneric{ BaseObj=0, DstPtr=1, SrcPtr=2, Size=3 }`。runtime 语义是 `memcpy(fieldPtr, src+TYPEINFO_PTR_SIZE, size)` / `WriteStruct(obj, fieldPtr, size, src+8, size)`。

改写成 `CJ_MCC_WriteStructField(obj, dst, size, src, size)`（参数序同 `GCWriteStruct` 的 lowering）：

| WriteStructField 形参 | gcwrite_generic 来源 |
| --- | --- |
| obj（GCTib 基址） | `arg0` BaseObj |
| dst | `arg1` DstPtr（**直接 fieldPtr，不加 8**） |
| size | `arg3` Size（按目标 i32/i64 zext） |
| src | `arg2` SrcPtr **+ 8**（generic box payload，需 GEP；栈 box 时 addrspacecast 到 addrspace(1)） |
| size | `arg3` Size |

与 Step28 `replaceWithMSGCWriteStruct` 的**唯一差异**：
- gcread_generic：dst = box+8、src = 裸 field、obj = box（offset=0）。
- gcwrite_generic：dst = 裸 fieldPtr、src = box+8、obj = BaseObj 容器（offset = fieldPtr−(obj+8)，非 0）。

这个非零 offset 正好是写入字段在容器内的偏移，AArch64 fast path 已经在算 `dst−(obj+payloadOffset)` 并据此移位 GCTib（[AArch64AsmPrinter.cpp:2793-2802](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)），天然支持；`dst < obj+8` 时退 slow，安全。

#### 8.5.2 落点与改动

1. `CJBarrierLowering.cpp`：把 `cj_gcwrite_generic` 纳入 MSGC 写屏障 lowering。
   - 新增 `replaceWithMSGCWriteGenericAsStruct(II)`，按 8.5.1 组参，AArch64+`EnableMSGCWriteBarrierLowering` 时复用 `getOrCreateMSGCWriteStructWrapper`（Step28 已新增）发 `CJ_MCC_WriteStructFieldFast_MSGC`；否则发裸 `CJ_MCC_WriteStructField`。
   - 路由：可仿 Step28 在判定函数里加 `cj_gcwrite_generic` 分支，或在 `replaceInstWithMSGCWriteBarrier` 增 case。注意它是**写**路径，门控用 `EnableMSGCWriteBarrierLowering`（含 `isAArch64()`）；非 AArch64 保持原 `CJ_MCC_WriteGeneric` helper，避免丢掉 runtime 的 `!HasRefField` 快速出口而在 x86 上反退化。
2. 复用 Step28 已加的 `getOrInsertWriteStructRuntimeFunc()` / `getOrCreateMSGCWriteStructWrapper()`，不重复造轮子。

#### 8.5.3 为什么这条覆盖主峰

对 (A) heap-obj-无-ref：进 fast wrapper → GCTib bitmap 全 0 → StoreOnly → **内联 memcpy，完全不进 runtime helper**，且 IDLE/MARK/SWEEP 全相位如此。这正好命中 5.69 亿的主体，是 Phase 1/2 都做不到的。

#### 8.5.4 与 Step28 的叠加效应

read-generic 与 write-generic 最终都汇流到同一个 `CJ_MCC_WriteStructFieldFast_MSGC` wrapper。因此 Step28 §8 讨论的 **A1（小 struct 内联 ldr/str 替代 `bl _memcpy`）与 A2（size=12 不再 slow-fall）对这 5.69 亿 write-generic 同样直接生效**——两个 Step 的收益在这个 wrapper 上叠加。case28 那个热点 size=12 的小拷贝，正是 A1/A2 的目标形态。

#### 8.5.5 风险与回归口径

- wrapper 被 `gcwrite_struct` / `gcread_generic` / `gcwrite_generic` 共享，任何改动要用「只含真实 struct write」的小用例回归，确认没改坏。
- 正确性重点：非零 field offset 的 GCTib 移位、`dst<obj+8` 的 slow 回退、栈 obj 的 StoreOnly 路径、src 的 +8 与 addrspacecast。
- 计数：新增「gcwrite_generic 进 StoreOnly vs slow」分类，确认主峰确实落 StoreOnly。

##### 8.5.5.1 `stack obj + heap src` 经 StoreOnly→memcpy 的等价性（必须钉死）

无条件改写后，原本 runtime 走 `ReadStruct` 的 **`!heap(obj) && heap(src)`** 分支（`base_write_generic_readstruct`，case28 仅 6.8 万），在 AArch64 fast wrapper 里会因 `obj` 不在 heap 而在 heap-range check 处直接掉到 **StoreOnly → `memcpy(fieldPtr, src+8, size)`**。

这**对 MSGC 是等价、正确**的，原因：

1. MSGC 的 `ReadStruct` override 本身就是纯 memcpy——`MSMarkBarrier::ReadStruct` / `MSIdleBarrier::ReadStruct` 都是 `(void)obj; memcpy_s(dst, size, src, size)`（[MSMarkBarrier.cpp:114](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)、[MSIdleBarrier.h:51](../../../src/Heap/MBarrier/MSIdleBarrier.h)）。
2. 基类 `Barrier::ReadStruct` 里的 `ForEachRefInStruct + TryUpdateRefField` 是给**搬移式 GC 更新 forwarding 指针**用的；MSGC 非搬移，override 直接丢弃 `obj`，所以没有这层动作要保留。
3. 拷的字节也一致：原 ReadStruct 分支 `memcpy(fieldPtr, src+TYPEINFO_PTR_SIZE, size)`，StoreOnly `memcpy(fieldPtr, src+8, size)`，逐字节相同。
4. 更一般地：**所有 stack-obj 情况在 MSGC 里都收敛成 memcpy**（stack-obj 永不进 WriteStruct；stack+heap-src→ReadStruct=memcpy；stack+stack-src→memcpy 分支），故 StoreOnly 对 stack-obj 恒正确。

**护栏**：该等价仅在「非搬移 + ReadStruct=memcpy」的 MSGC 下成立。若未来把这套 `gcwrite_generic → WriteStructFieldFast` 改写在**搬移式 GC** 下复用，`ReadStruct` 必须更新 forwarding，StoreOnly→memcpy 即错。因此这条 lowering 必须始终 **MSGC-gated**（现状即 `EnableMSGCWriteBarrierLowering`），不得泄漏到其它 collector。

##### 8.5.5.2 `src == nullptr` 早退 guard 必须保留（强制约束）

读写两个 helper 的 guard **不对称**，这是 Phase 3 必须处理的硬约束：

| helper | guard |
| --- | --- |
| `CJ_MCC_ReadGeneric` | `if (size == 0) return;` —— 只有 size |
| `CJ_MCC_WriteGeneric` | `if (src == nullptr \|\| size == 0) return;` —— **多了 `src == nullptr`** |

（见 [CompilerCalls.cpp `CJ_MCC_WriteGeneric`](../../../src/CompilerCalls.cpp)）

这个不对称是强信号：runtime 单独为写加了 `src==null` 早退，说明**前端确实会给 gcwrite_generic 传 src==null**（读没加，所以 Step28 读改写裸跑没事）。因此写改写不能照搬读的「无条件」做法。

两个风险分开判定：

- **`size == 0`：安全，无需特殊处理。** fast path 开头 `CBZX X2(size) → StoreOnly`，StoreOnly 做 `memcpy(dst, src+8, 0)`——0 字节不解引用、不写，与 helper 的 `return` 观测等价。
- **`src == nullptr && size > 0`：真雷。** fast path 最后 `EmitMemcpyForWriteStruct` 才碰 src，执行 `memcpy(dst, null+8=0x8, size)` → 从地址 8 读 → **段错误**；即便不崩，语义也错（helper 是整条跳过、保留 obj 字段旧值，fast path 却会把地址 8 的垃圾拷入）。而这些 callsite 的 size 基本是正的编译期常量（case28=12），size>0 恒成立 → 一旦真有 src==null 就是必崩，不是偶发。

**落地约束**：Phase 3 必须保留 `src==null` 的 no-op 语义，二选一：

1. **可证 src 非空就直接改**：src 的 underlying object 是 alloca / noalias 分配 / 带 `nonnull` 属性 → 无条件发 fast wrapper。
2. **证不了就发带 guard 的形式**（忠实复刻 helper）：
   ```
   %c = icmp ne ptr %src, null
   br %c, %do, %skip          ; src==null → 跳过，等价 helper 的 return
   do:   call WriteStructFieldFast(obj, fieldPtr, size, src+8, size)
   skip: ; 不做任何写
   ```
   src 几乎不为 null，分支高度可预测，开销极低；`size==0` 那支可不显式 guard（StoreOnly 0 字节本就 no-op）。

对「常量非零 size + 可证非空 src」的常见 callsite，两个检查都会被折叠掉，退化成裸 call；只有真不确定时才保留那个 branch。这样既不埋空指针雷，也不丢 helper 的「src==null 即跳过」语义。

#### 8.5.6 与 §8.4 的关系

Phase 3 落地后，再回看 §8.2 拆分：若仍有可观的「stack-stack」或「fresh+有ref-MARK」残留，才补 Phase 1 / Phase 2 作为收尾；否则可不做。

---

## 9. Phase 3 实测结果（2026-06-22 落地 + A/B）

### 9.1 实现与构建

- compiler：`CJBarrierLowering.cpp` 把 `cj_gcwrite_generic` 接进 `replaceWithMSGCWriteGenericAsStruct`，改写成 `CJ_MCC_WriteStructFieldFast_MSGC`（替换掉此前无效的 `CJ_MCC_WriteGenericFast_MSGC` 空壳 wrapper——它只是再 `bl` 回 `CJ_MCC_WriteGeneric`，AsmPrinter 无展开）。参数映射 `WriteStructField(obj=arg0, dst=arg1 fieldPtr, size, src=arg2+8, size)`，按 §8.5.1。
- `llc` 隔离验证通过：`gcwrite.generic → bl _CJ_MCC_WriteStructFieldFast_MSGC`，无 `CJ_MCC_WriteGeneric`。
- 全量重编：libLLVM.dylib / runtime(marksweep) / std / stdx，分别装回 `cangjie_compiler/output` 与 `cangjie_stdx/target`。

### 9.2 落地中发现并修复的真实 bug：null-guard 触发 `memcpy_s(destMax==0)` 崩溃

§8.5.5.2 的 null-guard 用「src==null → size=0」实现 no-op。但：

1. `src+8 = 0+8 = 8`（非 0），通过 runtime `MCC_WriteStructField` 的 `dst!=0 && src!=0` 校验；
2. 该函数随后 `memcpy_s(dst, dstLen=0, src=8, srcLen=0)`；
3. **`memcpy_s` 拒绝 `destMax==0`**（返回非 EOK），而内联快路径 StoreOnly 用的是 libc `memcpy`（容忍 size 0）——两者语义不一致，慢路径直接 `Check failed: memcpy_s ... == EOK` → SIGABRT。

崩溃点：`src/CompilerCalls.cpp` 的 `MCC_WriteStructField` 非 heap 分支（`CJ_MCC_WriteStructField` 是其 asm alias）。case28 启动即触发。

**修复**：在 `MCC_WriteStructField` 入口加 `if (srcLen == 0) return;`，复刻原 `WriteGeneric` 的 `src/size==0` no-op 语义；一处覆盖汇入该 wrapper 的全部三类屏障（gcwrite_struct / gcread_generic / gcwrite_generic）。修复后 case28 干净跑完，0 fail。

### 9.3 同配置干净 A/B（case28 c8 p10K 60s，clean runtime）

唯一变量 = `cj_gcwrite_generic` 是否走 Phase 3（通过在 `shouldLowerMSGCWriteBarrier` 增删该 case 切换），std/stdx/runtime 两侧一致重编：

| 配置 | 吞吐 | requests | fail |
| --- | ---: | ---: | ---: |
| baseline（`gcwrite_generic → CJ_MCC_WriteGeneric`） | 7655 req/s | 460,236 | 0 |
| Phase 3（`→ WriteStructFieldFast_MSGC`） | 7767 req/s | 467,005 | 0 |
| 差异 | **+1.5%** | | |

> 注：基准吞吐运行间抖动约 ±2~3%，+1.5% 很可能在噪声内。**结论：Phase 3 在 case28 上基本中性，未达预期大幅优化。**

### 9.4 根因：写被搬到慢路径，而非内联 StoreOnly

带计数版本（同一轮）显示：

- `base_write_generic_memcpy`：**5.69 亿 → ~0**（`Barrier::WriteGeneric` 几乎不再被调用，证明 Phase 3 确实重定向了写）。
- 但 `WRITESTRUCT_HELPER_STATS`：`total ≈ 3.29 亿，heap_obj_barrier ≈ 3.29 亿`——这些写**落到了 `MCC_WriteStructField` 慢路径**（runtime helper + WriteStruct），而不是 AArch64 内联 StoreOnly memcpy。

也就是说：

> **Phase 3 把 5.69 亿写从「WriteGeneric helper」搬到了「WriteStructField 慢 helper」——换了个 helper，没有消掉 helper。** §8.5.3 预期的「无 ref → bitmap 全 0 → 全相位 StoreOnly 内联」在 case28 没成立。

最可能原因：case28 的 dst 大量是 **容器/数组 backing**（ArrayList / HashMap），其 `obj->TypeInfo->GCTib` 带 bit63（长 bitmap / 数组标记），fast wrapper 在 `TBZX X15,#63 → Slow`（[AArch64AsmPrinter.cpp:2748](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)）处直接退慢路径，收益被吃掉。

### 9.5 对 §8.5.3 的修正与后续方向

§8.5.3「Phase 3 命中主峰 (A)、覆盖 5.69 亿主体」过于乐观。真实情况：主峰里**大量是 array/容器 generic 写**，fast emission 把它们判为复杂布局退慢路径。

要把 Phase 3 的收益真正兑现，下一步应解决「**array / 容器 generic 写也能走内联快路径**」，候选：

1. 扩展 AArch64 `WriteStructFieldFast_MSGC` 的 IsStruct 分支，支持 array GCTib（bit63）路径的内联枚举（参照 `ArrayCopyStructFast_MSGC` 已有的数组对象模型处理），而不是一律退 slow。
2. 或对「数组元素 generic 写」单独走 `array_copy_struct` 那套 fast emission。
3. 在此之前，先用 §8.2 的拆分计数确认 case28 里 array-backing 写占比，给收益上限定量。

在 array 写仍退慢路径之前，Phase 3 对 case28 这类容器密集负载只是等价替换，不宜单独作为优化结论上报。

### 9.6 产物与工作树状态

- 实测产物：`MSGC_RESULT/Result/case28_http_complex_json/result/`：`baseline_*`（7655）、`phase3clean_*`（7767）、`phase3b_*`（带计数，含 base_write_generic 塌缩与 329M 慢路径证据）。
- runtime 工作树：当前为 **干净版 + `srcLen==0` 修复**；原屏障计数插桩已 `git stash`（stash 内含 5 个插桩文件 + 一份重复的 srcLen 修复，`pop` 会与当前修复冲突，需手动合并）。
- SDK（output / stdx target）：已恢复到 **Phase 3** 状态（libLLVM + std + stdx 均含 Phase 3）。

---

## 10. §9.5 array fast-emission 调研（2026-06-22，已用插桩定位）

### 10.1 定位方法

§9.4 只是「最可能」推测。为坐实，给 runtime `MCC_WriteStructField`（fast wrapper 的 slow 入口，三类屏障 gcwrite_struct / gcread_generic / gcwrite_generic 都汇于此）加临时插桩，对每次进入慢路径的写记录：`size%8!=0`、GCTib 是否长格式（`!GCTib::IsGCTibWord()`，bit63==0）、`obj->IsRawArray()`。case28 c8 p10K 跑 20s。

### 10.2 实测分布（决定性数据）

```
[WSF_SLOW_DIAG] total=99,979,980
                size_unaligned(%8!=0) = 98,477,198   (98.5%)
                longform_bit63_0      = 98,478,426   (98.5%)
                israwarray            = 98,478,426   (98.5%)
                size_eq_12            = 0
```

三个关键事实：

1. **98.5% 的慢路径写是 raw array 元素写**（`israwarray`）。
2. `israwarray == longform_bit63_0`（同为 98,478,426）：**数组对象的 `obj->TypeInfo->GCTib` 是长格式（bit63==0）**——因为数组的 ref 信息编码在 component type 的 GCTib 上，数组自身 GCTib 不是内联 bitmap。
3. 这批写的 `size` 也是非 8 对齐的（且不是 12——之前以为的 size=12 是另一回事）。

### 10.3 根因：数组写在 struct fast path 上有**两个独立的 slow-fall 触发点**

> **更正（重要）**：这些慢路径数组写**绝大多数是「无 ref」数组写**（component 无 ref，如 `Array<Int64>`、`Array<无ref struct>`），不是「含 ref」。依据：baseline §2.3 中 `base_write_generic_memcpy=5.69亿`（条件 `!obj->HasRefField()`）、`base_write_generic_writestruct` 仅 7.5 万——而 `TypeInfo::HasRefField()` 对数组**反映 component**（[MClass.inline.h:248-259](../../../src/ObjectModel/MClass.inline.h)：`IsArrayType() → componentTi->IsRef() || componentTi->HasRefField()`）。所以 5.69 亿 = component 无 ref 的数组。它们退慢路径**不是因为有 ref，而是因为数组自身 GCTib 是长格式，使内联路径在「判断 bitmap==0（无 ref）→ StoreOnly memcpy」之前就 bit63=0 退出了**。
>
> 对比：baseline 的 `Barrier::WriteGeneric` 用一次 `!HasRefField()` flag 检查就 memcpy 掉了（便宜）；Phase 3 改写成 `WriteStructFieldFast`，内联路径先读 obj 自身 GCTib → bit63=0 → Slow → runtime `WriteStruct → EnqueueStructRefsInRange`（**不查 HasRefField**，直接遍历 GCTib）→ 遍历发现无 ref → 再 memcpy。**等于把「一次 flag 检查 + memcpy」换成「helper call + 遍历 GCTib + memcpy」，更重**。这才是 Phase 3 在 case28 上没涨（这部分小回退）的真因。

case28 的热 generic 写其实是 `ArrayList<T>`（T 多为**无 ref** 的 Int/无ref struct）的元素写，`obj` = backing 数组。它在 `WriteStructFieldFast_MSGC` 的 `IsStruct` 内联路径上**必然退慢路径**，因为：

1. **bit63==0**：`LDR X15,[TypeInfo+GCTibOffset]; TBZX X15,#63,Slow`（[AArch64AsmPrinter.cpp:2746-2748](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)）——数组自身 GCTib 是长格式 → 直接 Slow。
2. **size%8!=0**：`EmitRequireAligned8OrSlow(X2)`（[同文件:2736](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)，即 Step28 §A2 那条）——元素 size 非 8 对齐 → Slow。

任一条单独成立即退慢路径，两条同时成立。所以 **Phase 3 把这些数组 generic 写从 `WriteGeneric` 搬到 struct fast path，结果 100% 退到 `WriteStructField` 慢路径**，等价替换，吞吐只 +1.5%。§9.4 的「array backing」推测**被证实正确**。

### 10.4 正确的 fast 路径已经存在：`ArrayCopyStructFast_MSGC`

数组的 ref 枚举不该读 `obj` 自身 GCTib，而应读 **component TypeInfo 的 GCTib**。这正是 `CJ_MCC_ArrayCopyStructFast_MSGC` 的 `IsArrayCopyStruct` 分支已经在做的（[AArch64AsmPrinter.cpp:3045-3074](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)）：

1. `LDR X14,[TypeInfo+ComponentTypeInfoOffset]`（取 component TI），非 0 才是数组；
2. `LDR X15,[X14+GCTibOffset]; TBZX X15,#63,Slow`（用 **component** GCTib，它通常是短格式 bit63==1）；
3. `LDR W7,[X14+ComponentSizeOffset]`（component size），按元素 stride 外层循环、内层按 component bitmap 枚举旧 ref。

即「数组写走 component GCTib」这套机器已具备，只是**当前只有 `array_copy_struct` 用它，单元素的 `gcwrite_struct` / `gcwrite_generic` 数组写没用**。

### 10.5 推荐方案

**目标**：让 array-dst 的单元素写（gcwrite_struct / Phase3 的 gcwrite_generic）也走 component-GCTib 内联，而不是退慢路径。

**方案 A（推荐）：在 `WriteStructFieldFast_MSGC` 的 `IsStruct` 路径加「数组分支」。**
当读到 obj GCTib bit63==0（现在直接 Slow）时，先尝试：`LDR ComponentTypeInfo`，非 0 → 是数组 → 读 **component GCTib**：
- **component bitmap==0（无 ref，case28 的 ~98.5% 热路径）→ 直接 StoreOnly memcpy**（这正是 §10.3 应有的廉价行为，把「helper+遍历」变回内联 memcpy）；
- component bitmap!=0（含 ref，极少）→ 按 `array_copy_struct` 内层循环对写入区间 `[dst, dst+size)` 枚举旧 ref 再 memcpy（单元素写 = array_copy 的 1 次外层迭代，是其子集）。

> 即方案 A 对**无 ref 数组**退化成「component-bitmap==0 → StoreOnly memcpy」，这是 case28 的主战场；含 ref 数组的内联枚举是次要、可分步。
- 同时必须**放宽 size 8 对齐**（Step28 §A2）：把 ref 扫描循环界从 `dst+size` 改为 `dst+(size>>3<<3)`，去掉 `EmitRequireAligned8OrSlow(X2)`；否则非对齐元素 size 仍退慢路径（component-size 那条 `EmitRequireAligned8OrSlow(X7)`，[:3070](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)，同样要处理）。
- 这一处改动同时惠及 gcwrite_struct / gcread_generic / gcwrite_generic 的全部 array-dst 写。

**方案 B（备选）：编译期把 array-dst 写直接路由到 `array_copy_struct` 那套**。但编译期 `gcwrite_generic` 拿不到「dst 是数组」的静态信息（T 泛型），所以数组判定只能放在后端 asm（运行期 `ComponentTypeInfo != null`）。故方案 A 更自然。

### 10.6 风险与验证

- wrapper 被三类屏障共享，改完用「纯 gcwrite_struct 数组写」小用例 + case28 双跑回归。
- 正确性：component bitmap 移位、写入区间只覆盖单元素（不是整数组）、非对齐尾部不被当 ref、`ComponentTypeInfo==null`（非数组）回落原 struct 逻辑、component GCTib 仍 bit63==0（超长）时回落 slow。
- 预期：case28 中 ~98.5% 的 `WriteStructField` 慢调用应转入内联（StoreOnly 或内联 enqueue），吞吐才可能真正抬升（远超 +1.5%）。

### 10.7 一句话结论

Phase 3 之所以只 +1.5%，**根因是 case28 的 generic 写绝大多数是数组元素写，而 struct fast path 因「数组长格式 GCTib」+「size 非 8 对齐」两条触发点必退慢路径**。真正的优化是把单元素 array 写接入已存在的 `ArrayCopyStructFast_MSGC` 的 component-GCTib 内联机器（方案 A），并配套放宽 size 8 对齐。这是 Phase 3 之后**最值得做的下一步**。

---

## 11. 方案 A 实施计划（`WriteStructFieldFast_MSGC` 加数组分支）

### 11.1 落点

唯一改动文件：`llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp`，函数 `emitMccWriteBarrierFastPath` 的 `IsStruct` 分支（约 2719–2851 行）。无需改 `CJBarrierLowering` 或 runtime——wrapper 名与 ABI 不变，纯粹增强内联展开。一处改动同时惠及汇入该 wrapper 的 gcwrite_struct / gcread_generic / gcwrite_generic 的 array-dst 写。

### 11.2 改动一：放宽 size 8 对齐（Step28 §A2，必做且独立有效）

1. 删除 `EmitRequireAligned8OrSlow(X2)`（[:2736](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)）。保留对 X1(dst) 的对齐检查。
2. ref 扫描循环终止界从 `X9 = X1 + X2`（[:2812 `ADDXrs X9,X1,X2`](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)）改为 `X9 = X1 + (X2 & ~7)`（即 `X1 + (slotCount<<3)`，slotCount 已在 [:2754 `UBFM X10,X2,#3,#63`](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp) = X2>>3 算好）。末尾 memcpy 仍拷完整 X2。
3. 正确性：ref 一定 8 字节 8 对齐，尾部 `size&7` 不可能承载 ref，floor 扫描不漏标（与 runtime `ForEachRefInStruct` 按 ref 字段枚举一致）。

### 11.3 改动二：bit63==0 时尝试数组分支（核心）

现状 [:2748 `TBZX X15,#63,Slow`](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)：bit63==0 直接 Slow。改成：

1. **先保存 TypeInfo 指针**。注意 [:2737-2739](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp) 把 X15 算成 TypeInfo 指针，但 [:2742-2746](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp) 又用 GCTib 覆盖了 X15 → **TypeInfo 指针丢失**。需在覆盖前把 TypeInfo 暂存到一个空闲 scratch（此处 X16/X17 = HeapStart/End 后续还要用；需挑真正空闲的，如 X8，实现时核对该分支寄存器占用）。
2. `TBNZX X15,#63, .Lstruct_short`（bit63==1 → 短格式，跳到现有 struct 逻辑）。否则落入数组尝试：
3. `LDR Xc,[Xti + MSGCTypeInfoComponentTypeInfoOffset/8]`；`CBZ Xc, Slow`（component TI 为 0 → 不是数组 → 原样 Slow，保持长格式大 struct 的行为不变）。
4. `LDR Xg,[Xc + MSGCTypeInfoGCTibOffset/8]`；`TBZ Xg,#63, Slow`（component 自身也是长格式 → 放弃，Slow）。
5. `UBFM Xg, Xg, #0, #62` 取 component bitmap；`CBZ Xg, StoreOnly`（component 无 ref → 纯 memcpy）。
6. 用 component bitmap 走枚举（见 11.4）。

> 关键差异：单元素 array 写 = `array_copy_struct` 的 **1 次外层迭代**。写入区间就是 `[dst, dst+size)` 一个元素，bitmap 直接以 **dst 为基址**对齐——不需要 struct 路径里 `dst-(obj+payloadOffset)` 的 field-offset 移位（[:2787-2802](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)）。所以数组分支应**跳过那段 offset 移位**，bitmap 起始 bit 对应 dst 的第 0 个 8 字节槽。

### 11.4 枚举循环复用

复用 `IsArrayCopyStruct` 的**内层循环**（[:3128-3152](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)）：以 dst 为基址，按 component bitmap 逐 bit（每 8 字节一槽）`TBZ bitmap,#0,skip` → `LDR ref` → `CBZ skip` → 堆范围检查 → 写入 WB buffer。WB buffer 容量预留与 count 回写沿用 struct 路径既有逻辑（slotCount = (size&~7)>>3）。枚举完 `EmitMemcpyForWriteStruct()` 拷完整 size，跳 Finish。

### 11.5 边界与回落

- `ComponentTypeInfo==0`（非数组）→ Slow（不变）。
- component GCTib bit63==0（component 自身超长 >63 槽）→ Slow。
- WB buffer 不足 / 未初始化 → 沿用现有 Slow 回落。
- 非 AArch64 / 关 `EnableMSGCWriteBarrierLowering` → 不走此路径，runtime 慢路径兜底（语义已对）。

### 11.6 验证

1. lit：构造 obj 为数组（component 短格式有 ref）的 `gcwrite_struct` / `gcwrite_generic`，CHECK 生成的 asm 进入数组分支、不再 `bl _CJ_MCC_WriteStructField`；负向：component 长格式 / 非数组仍 Slow。
2. 正确性小用例：`ArrayList<带ref的struct/enum>` 增删 + GC，断言无悬挂、无 stale ref。
3. case28 双跑：用 §10 同款插桩确认 `WSF_SLOW_DIAG` 的 `israwarray` 占比从 98.5% 大幅下降；干净 runtime 量吞吐，对照 §9.3 的 7655/7767 看是否显著抬升。
4. 回归：纯 gcwrite_struct 非数组小用例确认未改坏。

### 11.7 工作量与风险

- 改动集中在一个 asm 发射分支，~30–50 条 MCInst + 寄存器 bookkeeping，无新 ABI、无新 runtime 符号。
- 主要风险在寄存器复用（TypeInfo 暂存）和 bitmap 基址语义（dst 基址 vs obj-payload 基址）——11.3/11.4 已点明，实现时按这两点核对即可。
- 收益上限：case28 ~98.5% 的慢调用有望转内联，这才是 Phase 3 想要、但因数组退慢而没拿到的那部分吞吐。

---

## 12. 实测落地复盘（2026-06-23）

§11 的「数组分支」是基于「慢写卡在数组 GCTib」的推测。落地插桩后发现**真正的根因不是数组**，而是 GC 相位。本节记录全部实测结论。

### 12.1 真根因：GC 卡在 phase 3（RECLAIM_SATB_NODE），写在相位门就退慢路径

给 `MCC_WriteStructField` 入口加相位直方图插桩，case28 实测：

```
[WSF_PHASE] phase=3 count≈99.85%   (GC_PHASE_RECLAIM_SATB_NODE)
phase=20/21/23 合计 <0.2%
```

即 **99.85% 的慢写发生在 phase 3**。`WriteStructFieldFast_MSGC` 的相位门当时只放行 IDLE(1)/SWEEP(23)/MARK(20-22)→其余（含 phase 3）一律 `Bcc LO/HI → Slow`。而 PostGC 清理后 GC **一直停在 RECLAIM_SATB_NODE 没回 IDLE**，于是几乎所有写都退慢路径。这也解释了 §11 的数组分支为什么没用——**写根本到不了数组逻辑，在相位门就 slow 了**。

**三层修复（codex 主导，已验证）：**
1. `TracingCollector::PostGarbageCollection`：清理后补 `TransitionToGCPhase(IDLE)`——修掉「卡在 phase 3」的状态机遗漏（**真根因**）。
2. `HeapImpl::InstallBarrier`：把 `FINISH`/`RECLAIM_SATB_NODE` 归入 `msIdleBarrier`（无屏障）——印证这些是无屏障相位。
3. `AArch64AsmPrinter` 相位门：非 mark 相位（1-19、23+）→ StoreOnly，仅 [20-22] 走屏障，UNDEF(0)→Slow。与 runtime 屏障选择一致，对 MSGC 安全。

### 12.2 §11 数组分支：实测无效

相位修复后再测：慢写里 98.5% 是 raw array、其 component **无 ref**（`HasRefField` flag=0）、且 component GCTib 也是长格式（bit63=0）。我把 §11 的「读 component GCTib bitmap」改成「读 component HasRefField flag（offset 9, bit0）」后能正确判定，但插桩显示 **israwarray 占比不降**——因为这些写 99.87% 仍在 phase 3，相位修复后**直接 StoreOnly**，根本不进数组分支。结论：**数组分支对 case28 无收益**（相位门已经把它们接走了）；代码已留作正确性兜底，但不是 case28 的优化点。

### 12.3 `assign_generic` 内联：enum 崩溃，已回退

把 `cj_assign_generic` 仿 Phase3 路由到 `WriteStructFieldFast`（运行期从 `typeInfo->instanceSize` 取 size）。最小用例通过，但 **case28 SIGSEGV**。根因：case28 的 assign 绝大多数是 **enum（JsonValue）值拷贝**，enum 的 ref 布局**随 variant 变**，不能当固定 bitmap 的 struct 枚举——会把非 ref 的 variant 数据当 ref enqueue → 喂垃圾指针进 mark 栈 → GC 标坏 → 崩。**已回退**（assign_generic 重新走 `CJ_MCC_AssignGeneric` helper）。要做必须 enum 感知，超出快速内联范围，且收益对 case28≈0，暂不做。

### 12.4 路线 A：`bl _memcpy` → 小尺寸内联 load/store（已落地 + 实测）

StoreOnly（相位修复后的绝对主路径）和枚举后都走 `EmitMemcpyForWriteStruct` 的 `bl _memcpy`。对小拷贝，这次调用开销远大于真正拷贝。**路线 A = 常量 size 特化**：

- **CJBarrierLowering**：barrier 的 size 是 `ConstantInt` 且 ∈ `{8,12,16,24,32}`（`isMSGCInlineStructCopySize`）→ 用尺寸特化 wrapper `CJ_MCC_WriteStructFieldFast_MSGC_sz<N>`（`selectMSGCWriteStructWrapper`），接在 gcwrite_struct / gcread_generic / gcwrite_generic 三处；非常量/其它 size 回落普通 wrapper。
- **AsmPrinter.cpp 触发**：`startswith("..._sz")` → 走 fast path。
- **AArch64AsmPrinter**：`ParseSizedWrapper` 解析 `_sz<N>`；`EmitInlineSmallCopy` 对 8/12/16/24/32 发**精确 ldr/str**（struct 用 X1=dst/X3=src，static-struct 用 X0/X2），否则回落 `bl _memcpy`。
- 同时覆盖 `WriteStructFieldFast` 与 `WriteStaticStructFast`。

**验证**：llc 实测 size=16 的 struct 写 → `ldr x9,[x3]; ldr x10,[x3,#8]; str…`，`bl _memcpy` 计数 = **0**。

**case28 实测（clean runtime，60s，cjProcessorNum=6）：**

| 配置 | 吞吐 |
| --- | ---: |
| baseline（无 Phase3） | 7655 |
| Phase3 | 7767 |
| 相位修复（无 Route A） | ~7250 |
| **Route A（3 次）** | **7899 / 7065 / 8162**（均值 ~7709） |

**判断**：Route A 工程正确、确实消除了每次小拷贝的 `bl _memcpy`（16B：1 次调用 → 2 条 ldr/str），是实打实的指令数/CPU 下降；但 case28 吞吐三次跳 ±15%（机器热 + 网络/GC 噪声），**信号被方差盖过**，只能说「正确、低风险、CPU 有降、吞吐中性偏微正」。再次印证 **case28 非 barrier 绑定**。

### 12.5 runtime 工作区清理（保留 vs 移除）

落地后清理诊断脚手架，runtime 工作区**只保留 3 个真实修复**：

| 文件 | 改动 | 性质 |
| --- | --- | --- |
| `TracingCollector.cpp` | PostGC 后 `TransitionToGCPhase(IDLE)` | **真修复**（§12.1 根因），保留 |
| `Heap.cpp` | `InstallBarrier`：FINISH/RECLAIM→idleBarrier | **真修复**（相位一致性），保留 |
| `CompilerCalls.cpp` | `MCC_WriteStructField` 入口 `if (srcLen==0) return;` | **真修复**（memcpy_s destMax==0 崩溃，见 §9.2），保留 |
| ~~`CompilerCalls.cpp` BARRIER_VOL 计数 + BCHit~~ | 屏障调用量插桩 | 协助性，**已移除** |
| ~~`MutatorManager.cpp/.h`~~ STW 诊断 | `CJ_MS_STW_DIAG_MS` 周期日志 + DumpMutators 增强 | 协助性（env 门控），**已回退** |

> 编译器侧（llvm-project）保留：Phase3（gcwrite_generic 内联）、Step28（gcread_generic 内联）、相位门修复、Route A（小拷贝内联）、数组子路径（正确性兜底）。回退：assign_generic 内联（enum 崩）。
