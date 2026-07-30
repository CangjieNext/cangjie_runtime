# Wbuf 与 Region-scan 读写屏障实现对比

> 本文只比较 MSGC 的字段读写、结构体/数组拷贝、generic 和 atomic 引用屏障。
> primitive 数值字段写、跨语言 export handle/cross-access barrier、对象分配 fast path
> 不属于本文范围。
>
> 本文以 2026-07-30 检出的源码工作树为最终事实来源。下表 revision 是工作树的基线
> commit；工作树存在未提交修改，因此历史设计文档或基线 commit 与当前函数体不一致时，
> 以当前工作树源码为准。

## 1. 对比基线

本文严格使用下面两套互相匹配的 Runtime、LLVM 和实现文档，不交叉混用。

| 实现 | 组件 | 分支 / revision | 固定目录 |
|---|---|---|---|
| Region-scan | Runtime | `MSGC-bugfix-regionScan` / `24ca097ace46` | `/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime` |
| Region-scan | LLVM | `MSGC-bugfix-regionScan` / `60ffbc2e1794` | `/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler_region_mark/third_party/llvm-project` |
| Region-scan | Docs | 同 Region-scan Runtime 工作区 | `/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/MSGC_RESULT/Docs` |
| Wbuf | Runtime | `MSGC-bugfix-wbuf` / `a511f014e4e8` | `/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime` |
| Wbuf | LLVM | `MSGC-bugfix-wbuf` / `9abe5e2b2005` | `/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/third_party/llvm-project` |
| Wbuf | Docs | 同 Wbuf Runtime 工作区 | `/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Docs` |

下文用以下简称表示源码根目录：

```text
RS_RT   = Region-scan Runtime
RS_LLVM = Region-scan LLVM
WB_RT   = Wbuf Runtime
WB_LLVM = Wbuf LLVM
```

## 2. 核心结论

| 维度 | Wbuf | Region-scan | 结论 |
|---|---|---|---|
| GC 算法 | non-moving mark-sweep | non-moving mark-sweep | 都不需要 CopyGC forwarding read barrier |
| 普通读屏障 | LLVM 直接降成 load/memcpy | LLVM 直接降成 load/memcpy | 实现和成本基本一致 |
| atomic read | 当前仍调用 Runtime，Runtime raw atomic load | 同左 | 两边都有一个可继续优化的调用空档 |
| Idle 写屏障 | 支持的 target 内联 phase gate + raw store/copy | 同左 | 基本一致 |
| Mark 标量写 | 支持的 target 可内联 mark winner + `activeLog_` append | target 一律回 Runtime | Region-scan 标量写更容易出现调用开销 |
| Mark bulk 写 | bitmap-word 合并，winner 对象进入 WorkBuf | bitmap-word 合并，发布 pending word/Region task | dense/bulk 场景 Region-scan 可能更有利 |
| Mark 工作表示 | 每个待扫描对象一个 WorkBuf entry | `markBitmap & ~scanBitmap` | Region-scan 不需要普通灰对象队列 |
| 原子写顺序 | swap/CAS 先 mutation，成功后 enqueue old | old-before-mutation，store/swap 使用 CAS loop | Region-scan 顺序更强，但竞争时更重 |
| LLVM/Runtime ABI | LLVM 直接依赖 TLS、`activeLog_`、WorkBuf、Region/TypeInfo 布局 | 写屏障 LLVM 当前只依赖公开 `barrierPhase` | Region-scan ABI 更小，Mark fast path 也更保守 |
| 代码体积 | Mark inline 展开较大 | phase gate 后调用 Runtime | Wbuf 减少热调用，Region-scan 减少 callsite 代码体积 |

## 3. 总体编译与调用链

两套实现的前半段编译管线相同：

```text
Cangjie frontend
  -> llvm.cj.* barrier intrinsic
  -> CJBarrierOpt / CJBarrierSplit
  -> CJBarrierLowering（按 Cangjie_GCMode 分流）
       -> 普通 MSGC read：load / memcpy
       -> 六类 MSGC write：*_Fast_MSGC wrapper
       -> 其他 generic/atomic：CJ_MCC_* Runtime API
  -> AArch64/X86 target emitter
       -> 支持的路径在 callsite 展开
       -> 不支持或校验失败时执行 wrapper 的 Runtime slow call
```

六个专用 write wrapper 是：

| 类型 | wrapper |
|---|---|
| 对象引用写 | `CJ_MCC_WriteRefFieldFast_MSGC` |
| 静态引用写 | `CJ_MCC_WriteStaticRefFast_MSGC` |
| 对象 struct 写 | `CJ_MCC_WriteStructFieldFast_MSGC` |
| 静态 struct 写 | `CJ_MCC_WriteStaticStructFast_MSGC` |
| 引用数组拷贝 | `CJ_MCC_ArrayCopyRefFast_MSGC` |
| struct 数组拷贝 | `CJ_MCC_ArrayCopyStructFast_MSGC` |

wrapper 自身是可执行的 thin slow body：target 不展开时，仍会 tail-call 原始
`CJ_MCC_*` Runtime API，不能把 wrapper 理解成只有 inline 才正确。

## 4. 全部屏障 intrinsic 与 MSGC lowering

### 4.1 完整清单

| 类别 | LLVM intrinsic | Runtime API / 下沉目标 | MSGC 专用 lowering | 是否进入六类 target write inline |
|---|---|---|---|---|
| 写 | `cj_gcwrite_ref` | `CJ_MCC_WriteRefField` | `WriteRefFieldFast_MSGC` wrapper | 是 |
| 写 | `cj_gcwrite_static_ref` | `CJ_MCC_WriteStaticRef` | `WriteStaticRefFast_MSGC` wrapper | 是 |
| 写 | `cj_gcwrite_struct` | `CJ_MCC_WriteStructField` | `WriteStructFieldFast_MSGC` wrapper | 是 |
| 写 | `cj_gcwrite_static_struct` | `CJ_MCC_WriteStaticStruct` | `WriteStaticStructFast_MSGC` wrapper | 是 |
| 写/generic | `cj_gcwrite_generic` | 语义等价转换为 `CJ_MCC_WriteStructField` | 将 `src` 调整为 box payload 后复用 struct wrapper | 间接进入 struct inline |
| 写/generic | `cj_gcwrite_generic_payload` | `CJ_MCC_WriteGenericPayload` | 保留 Runtime API | 否 |
| 写/generic | `cj_assign_generic` | `CJ_MCC_AssignGeneric` | 保留 Runtime API；Runtime 按 `HasRefField` 分流 | 否 |
| 写/copy | `cj_copy_struct_field` | `CJ_MCC_CopyStructField` | 保留 Runtime API；heap 目标转 `WriteStruct` | 否 |
| 数组写 | `cj_array_copy_ref` | `CJ_MCC_ArrayCopyRef` | `ArrayCopyRefFast_MSGC` wrapper | 是 |
| 数组写 | `cj_array_copy_struct` | `CJ_MCC_ArrayCopyStruct` | `ArrayCopyStructFast_MSGC` wrapper | 是 |
| 数组写/generic | `cj_array_copy_generic` | `CJ_MCC_ArrayCopyGeneric` | 保留 Runtime API，按 component type 分流 | 否 |
| 读 | `cj_gcread_ref` | 原 API 为 `CJ_MCC_ReadRefField` | 直接 IR `load` | 不需要 target inline |
| 读 | `cj_gcread_weakref` | 原 API 为 `CJ_MCC_ReadWeakRef` | 直接 IR `load` | 不需要 target inline |
| 读 | `cj_gcread_static_ref` | 原 API 为 `CJ_MCC_ReadStaticRef` | 直接 IR `load` | 不需要 target inline |
| 读 | `cj_gcread_struct` | 原 API 为 `CJ_MCC_ReadStructField` | 直接 `llvm.memcpy` | 不需要 target inline |
| 读 | `cj_gcread_static_struct` | 原 API 为 `CJ_MCC_ReadStaticStruct` | 直接 `llvm.memcpy` | 不需要 target inline |
| 读/generic | `cj_gcread_generic` | 目标 box 可能在 heap | 转成对目标 box payload 的 struct write | 间接进入 struct inline |
| atomic 写 | `cj_atomic_store` | `CJ_MCC_AtomicWriteReference` | 保留 Runtime API | 否 |
| atomic 读 | `cj_atomic_load` | `CJ_MCC_AtomicReadReference` | 保留 Runtime API | 否 |
| atomic 写 | `cj_atomic_swap` | `CJ_MCC_AtomicSwapReference` | 保留 Runtime API | 否 |
| atomic 写 | `cj_atomic_compare_swap` | `CJ_MCC_AtomicCompareAndSwapReference` | 保留 Runtime API | 否 |

### 4.2 generic read 为什么可能变成 write barrier

`gcread_generic` 的“源读取”在 MSGC 中不需要 forwarding/self-healing，但它会把结果复制到
目标 box payload。如果目标 box 位于 heap，这次 copy 会覆盖目标 box 中原有引用，所以仍需
保护目标旧边。

当前 lowering 因此把它转换为：

```text
CJ_MCC_WriteStructField(
    dstObj,
    dstObj + TYPEINFO_PTR_SIZE,
    size,
    srcField,
    size)
```

这不是重新引入 CopyGC read barrier，而是读取操作附带的 heap destination write barrier。

## 5. phase 安装：两边相同

Wbuf 与 Region-scan 的 `HeapImpl::InstallBarrier()` 当前相同。

| GC phase | Runtime barrier | `barrierPhase` | 读语义 | 写语义 |
|---|---|---:|---|---|
| `GC_PHASE_IDLE` | `MSIdleBarrier` | 0 | raw load/memcpy | raw store/memcpy/memmove |
| `GC_PHASE_FINISH` | `MSIdleBarrier` | 0 | 同上 | 同上 |
| `GC_PHASE_RECLAIM_SATB_NODE` | `MSIdleBarrier` | 0 | 同上 | 同上 |
| `GC_PHASE_MS_MARK_SETUP` | `MSMarkBarrier` | 1 | 仍为 raw load/memcpy | old-only deletion barrier |
| `GC_PHASE_MS_CONCURRENT_MARK` | `MSMarkBarrier` | 1 | 仍为 raw load/memcpy | old-only deletion barrier |
| `GC_PHASE_MS_MARK_TERMINATION` | `MSMarkBarrier` | 1 | 仍为 raw load/memcpy | old-only deletion barrier |
| 未审计 phase | conservative `stwBarrier` | 1 | 保守 Runtime 行为 | 保守 Runtime 行为 |

两边共同的 Mark 写语义是：

```text
old = load(slot)
publish(old)       // 必须保护被删除的快照旧边
store(slot, new)
```

普通非原子写只处理 `old`，不会额外 shade `new`。

## 6. 读屏障实现

### 6.1 普通读

| 屏障 | Idle Runtime：Wbuf / Region-scan | Mark Runtime：Wbuf / Region-scan | LLVM |
|---|---|---|---|
| `ReadReference` | `MSLoadRawReference(field)` | 继承 `MSIdleBarrier`，完全相同 | `gcread_ref -> load` |
| `ReadWeakRef` | raw load | 完全相同 | `gcread_weakref -> load` |
| `ReadStaticRef` | raw load | 完全相同 | `gcread_static_ref -> load` |
| `ReadStruct` | `memcpy_s` | 完全相同 | `gcread_struct -> llvm.memcpy` |
| `ReadStaticStruct` | `memcpy_s` | 完全相同 | `gcread_static_struct -> llvm.memcpy` |
| `ReadGeneric` | 按源/目标是否在 heap，选择 memcpy、`ReadStruct` 或 `WriteStruct` | 读源仍不需要屏障；heap 目标写入走 Mark `WriteStruct` | lowering 直接转成目标 box 的 struct write |

MSGC 不移动对象，因此两边都没有以下 CopyGC 读侧职责：

- from-space/to-space 选择；
- forwarding；
- tagged ref 修正；
- field self-healing；
- 读时协助搬移。

### 6.2 atomic read

| 项目 | Wbuf | Region-scan |
|---|---|---|
| LLVM MSGC 专用 inline | 无 | 无 |
| 调用 | `CJ_MCC_AtomicReadReference` | 同左 |
| Idle Runtime | 按请求 memory order 执行 raw atomic load | 同左 |
| Mark Runtime | `MSMarkBarrier` 未覆盖 atomic read，继承 Idle raw atomic load | 同左 |

所以 atomic read 在语义上没有读屏障，但当前编译结果仍可能保留一次 Runtime 调用。

## 7. Idle 写屏障实现

### 7.1 Runtime

两套 Runtime 的 `MSIdleBarrier` 当前相同：

| 操作 | Idle Runtime 行为 |
|---|---|
| ref/static ref write | `MSStoreRawReference` |
| struct/static struct write | `memcpy_s` |
| ref/struct array copy | `memmove_s` |
| atomic store | raw atomic store |
| atomic swap | raw atomic exchange |
| atomic CAS | raw compare-exchange |
| generic write/assign/payload | 无引用或非 heap 情况直接 memcpy；含引用 heap 目标最终调用 Idle `WriteStruct` |
| generic array copy | Runtime 判定 component type，最终选择直接 memmove、`CopyRefArray` 或 `CopyStructArray`；Idle 下三者都是直接 copy |
| copy struct field | 非 heap 目标直接 memcpy；heap 目标调用 Idle `WriteStruct` |

### 7.2 LLVM target inline

对六类 `*_Fast_MSGC` wrapper，Wbuf 和 Region-scan 的 Idle 逻辑一致：

```text
load-acquire barrierPhase
if phase == 0:
    Ref / StaticRef       -> raw 64-bit store
    Struct / StaticStruct -> memcpy
    ArrayCopy             -> memmove
else:
    进入各自的 Mark 路径
```

AArch64 用 `LDARB` 读取 phase；X86 用普通 byte load，依赖 TSO 提供所需顺序。

| target | Wbuf Idle | Region-scan Idle |
|---|---|---|
| AArch64 Mach-O | 六类 wrapper inline | 六类 wrapper inline |
| AArch64 ELF，`+lse` | 六类 wrapper inline | 六类 wrapper inline |
| AArch64 ELF，`-lse` | 六类 wrapper inline | 六类 wrapper inline |
| X86-64 ELF | 六类 wrapper inline | 六类 wrapper inline |
| X86-64 MinGW/COFF | 六类 wrapper inline | 六类 wrapper inline |
| X86-64 Mach-O | 六类 wrapper 的 Idle gate/store/copy 可 inline | 同左 |
| X86-32、非支持 CC、indirect/tail call 等 | Runtime wrapper | Runtime wrapper |

atomic 和未进入六类 wrapper 的 generic/copy 入口，不使用这套 target inline。

## 8. Mark 写屏障：Runtime 实现对比

### 8.1 标量 ref/static-ref

| 步骤 | Wbuf Runtime | Region-scan Runtime |
|---|---|---|
| 读取旧值 | `old = MSLoadRawReference(field)` | 同左 |
| 相同值写 | `old == new` 时跳过 enqueue | 同左 |
| heap gate | null/非 heap 直接跳过 | 同左 |
| owner | active collector + 当前线程 `AllocBufferMS` | acquire-load `barrierPhase` + active collector |
| 标记 | old 对象竞争 mark bit | old 加入栈内 `MSMarkWordBatch`，flush 时 OR mark word |
| 去重 | 只有 mark-CAS winner 继续 | 只有 `newBits` 继续 |
| 无引用对象 | mark 后不进入 WorkBuf | mark 后不发布 scan task；普通情况依赖 Region `ScanClass` |
| 待扫描发布 | winner append 到当前 P 的 `activeLog_` | small：pending word + Region task；large：OBJECT task |
| 最终写入 | `MSStoreRawReference(field, new)` | 同左 |

Wbuf 标量路径：

```text
old
  -> ShadeManagedRefForBarrier
  -> mark winner?
       no  -> store new
       yes -> typeInfo->HasRefField?
                no  -> store new
                yes -> activeLog_->Objects()[len++] = old
  -> store new
```

Region-scan 标量路径：

```text
old
  -> PublishManagedRefForBarrier
  -> 创建 16-entry MSMarkWordBatch
  -> AddManagedRefToMarkBatch(old)
  -> 立即 FlushBarrierMarkBatch
       -> atomic OR mark word
       -> small HAS_REF: PublishPendingMarkWord + TryPublishRegion
       -> large HAS_REF: PublishObject
  -> store new
```

Region-scan 的 16-entry batch 在单次标量屏障中只加入一个 old ref 后立即 flush，因此不能在
多个标量写之间合并；这个 batch 的主要合并收益来自 tracing 和 bulk barrier。

### 8.2 struct/static-struct 与数组 bulk

| 操作 | 旧引用枚举 | Wbuf | Region-scan |
|---|---|---|---|
| object struct write | 按目标对象 GCTib/TypeInfo 枚举被覆盖范围 | `MSMarkWordBatch` 聚合同 word；winner 对象 append activeLog | 16-entry word batch；flush 后发布 pending word/Region task |
| static struct write | 按传入 GCTib 枚举 static destination | 同上 | 同上 |
| ref array copy | 按目标覆盖范围逐 ref | 同 word 聚合，winner 入 activeLog | 同 word 聚合，发布 Region/OBJECT work |
| struct array copy | 按 component TypeInfo/GCTib 枚举 | 同上 | 同上 |
| 覆盖动作 | 必须先 flush，再 memcpy/memmove | 满足 | 满足 |

Wbuf bulk batch 当前有 4 个 word entry，每个 entry 最多缓存 16 个 HAS_REF candidate；
flush 后只把本次 mark winner 写入 WorkBuf。

Region-scan bulk batch 是 8 set × 2 way，共 16 个 word entry。entry 只保存
`(regionIdx, wordIdx, mask)`，flush 时重新取得 bitmap plane 和 collect epoch。

### 8.3 工作发布与消费

| 项目 | Wbuf | Region-scan |
|---|---|---|
| producer 私有状态 | `AllocBufferMS::activeLog_` | 无灰对象 log |
| 默认 buffer capacity | WorkBuf 默认 1024 个对象地址 | 不适用 |
| 全局发布 | activeLog 满、owner 退出或 MarkTermination | Region 从 IDLE 变 QUEUED 时发布 token；large 发布 OBJECT |
| small work 粒度 | object pointer | bitmap word + Region task |
| 去重 | mark winner 保证对象只进入 grey work 一次 | `markBitmap & ~scanBitmap` 保证对象只扫描一次 |
| 同 Region 新 work | 每个 winner 占一个 WorkBuf entry | `pendingWordSummary` 合并，同一 QUEUED/OWNED Region 不重复发 token |
| MarkTermination | flush 所有 P 的 partial activeLog，drain global WorkBuf | scheduler ledger closure + pending summary/Region state reconciliation |

Region task不是整区盲扫。owner 只处理 pending summary 指定的 bitmap word，并计算：

```text
candidates = markBitmap[word] & ~scanBitmap[word]
```

已扫描对象不会因为 Region 再次被调度而重复扫描。

## 9. Mark 写屏障：LLVM inline 对比

### 9.1 target 能力矩阵

| target / 屏障 | Wbuf Mark | Region-scan Mark |
|---|---|---|
| AArch64 ref/static-ref | inline small/large mark、HasRef 判断、activeLog append；异常情况回 wrapper | 全部回 Runtime wrapper |
| AArch64 struct/static-struct | 尽量 inline GCTib 枚举和 mark；short descriptor 可 pair commit；校验失败回 wrapper/helper | 全部回 Runtime wrapper |
| AArch64 array-copy ref/struct | Mark 一律回 Runtime bulk barrier | Mark 一律回 Runtime bulk barrier |
| X86-64 ELF ref/static-ref | inline small `lock btsq`、large mark、HasRef、activeLog append | 全部回 Runtime wrapper |
| X86-64 ELF struct/static-struct | inline/leaf helper 枚举，异常情况回 wrapper | 全部回 Runtime wrapper |
| X86-64 COFF ref/struct | 与 ELF 类似，并按 Win64 ABI 保存/恢复参数 | 全部回 Runtime wrapper |
| X86-64 Mach-O Mark | 回 Runtime wrapper | 回 Runtime wrapper |
| X86-64 array-copy Mark | 回 Runtime wrapper | 回 Runtime wrapper |
| atomic read/write/swap/CAS | 无 MSGC target inline | 无 MSGC target inline |
| assign/payload/array-generic/copy-struct | 无专用 target inline | 无专用 target inline |

### 9.2 Wbuf 标量 inline 如何实现

Wbuf 的 ref/static-ref 常见 inline 路径包含：

1. acquire-load `CJ_MRT_MSGCFastPathState.barrierPhase`；
2. phase 0 直接 store；
3. Mark 时读取 old，`old == null` 或 `old == new` 直接 store；
4. 对 object container 和 old 做 heap range 校验；
5. 从 TLS 取得 `AllocBufferMS`，再取得 `activeLog_`；
6. 检查 WorkBuf `len < capacity`，否则回 wrapper；
7. 根据 heap address 定位 `MSRegionInfo`；
8. 校验 Region type、slot 坐标、对象 StateWord/TypeInfo；
9. small 对 mark bitmap 原子置位，large 对 primary marked flag 原子置位；
10. 已 marked 直接 store；
11. mark winner 做 per-object `HasRefField` 判断；
12. HAS_REF winner 写入 `activeLog_->Objects()[len]`，更新 len；
13. 写入新引用。

AArch64 `+lse` 使用 `LDSETALX/LDSETALB`；`-lse` 使用
`LDAXR/STLXR`、`LDAXRB/STLXRB` 重试循环。X86 small mark 使用带 `lock` 的位操作，
large mark 使用原子 byte 更新。

这条路径减少了 Mark 时的 Runtime call，但把 TLS、WorkBuf、RegionInfo、TypeInfo 和对象头
布局都变成 LLVM/Runtime 隐藏 ABI。

### 9.3 Region-scan inline 如何实现

Region-scan 当前 target emitter 只实现 phase gate：

```text
phase = acquire_load(CJ_MRT_MSGCFastPathState.barrierPhase)
if phase == 0:
    raw store / memcpy / memmove
else:
    call *_Fast_MSGC wrapper slow body
```

LLVM 当前不会直接修改：

- mark/scan plane；
- `pendingWordSummary`；
- Region scan state；
- scheduler ledger；
- Region/Object ready lane。

这些状态都由 Runtime 统一维护，降低了 ABI 耦合和 target-specific 正确性风险，但也意味着
Mark 标量写会跨越 Runtime 调用边界。

## 10. atomic 写屏障对比

atomic 屏障目前两边都没有 MSGC LLVM target inline，差异完全在 Runtime。

| atomic 操作 | Wbuf Mark Runtime | Region-scan Mark Runtime |
|---|---|---|
| atomic store | relaxed load old -> enqueue old -> atomic store new | load expected -> enqueue expected -> CAS；失败后重新读取并重试 |
| atomic swap | atomic exchange 得到 old -> enqueue old -> 返回 old | load expected -> enqueue expected -> CAS；成功返回 expected，失败重试 |
| atomic CAS | 先 CAS；仅成功后 enqueue expected old | CAS 前保守 enqueue expected old；失败也可能产生冗余 discover |
| atomic load | raw atomic load | raw atomic load |

Region-scan 的 publication 在可能删除该 exact edge 的 mutation 之前完成，关闭了
mutation-success 与 old publication 之间的窗口。代价是：

- store/swap 从单条 atomic operation 变成 CAS loop；
- 竞争时可能重复读取、重复 discover；
- failed CAS 也可能保守标记 expected old；
- Region publication 路径可能被多次进入。

因此原子引用高竞争负载是 Region-scan 最需要单独测量的屏障场景，但这部分额外成本同时
换取了更强的 old-before-mutation 顺序。

## 11. generic 与 copy 入口的 Runtime 分流

| 入口 | Runtime 判断 | 最终 barrier |
|---|---|---|
| `CJ_MCC_WriteGeneric` | `src/size`、obj/src 是否在 heap、obj TypeInfo/HasRef | memcpy、`ReadStruct` 或 `WriteStruct` |
| `CJ_MCC_ReadGeneric` | dst/src object 是否在 heap | memcpy、`ReadStruct` 或目标 `WriteStruct` |
| `CJ_MCC_AssignGeneric` | `typeInfo->HasRefField()` | 无引用 memcpy；含引用 `WriteGeneric` |
| `CJ_MCC_WriteGenericPayload` | dst TypeInfo `HasRefField()` | 无引用 memcpy；含引用 `WriteStruct` |
| `CJ_MCC_ArrayCopyGeneric` | component TypeKind、IsStruct、HasRef | raw memmove、`CopyRefArray` 或 `CopyStructArray` |
| `CJ_MCC_CopyStructField` | dstBase 是否在 heap | 非 heap memcpy；heap `WriteStruct` |

这些入口本身没有专用 target emitter；一旦进入 Runtime，最终调用的 `WriteStruct`、
`CopyRefArray` 或 `CopyStructArray` 会分别使用当前安装的 Idle/Mark 语义。

## 12. ABI 差异

| ABI 项 | Wbuf LLVM write inline | Region-scan LLVM write inline |
|---|---|---|
| `CJ_MRT_MSGCFastPathState.barrierPhase` | 使用 | 使用 |
| heap reserved start/end | Mark inline 使用 | 当前 write inline 不使用 |
| RegionInfo array base | Mark inline 使用 | 当前 write inline 不使用 |
| Region type/slot/bitmap offset | 使用 | 当前 write inline 不使用 |
| large marked flag | 使用 | 当前 write inline 不使用 |
| TLS `AllocBufferMS` offset | 使用 | 不使用 |
| `AllocBufferMS::activeLog_` offset | 固定为 896，使用 | 已移除，不使用 |
| WorkBuf len/capacity/objects offset | 使用 | 不使用 |
| TypeInfo payload/flag/component offset | 使用 | 不使用 |
| pending summary / scan state / ledger | 不使用 | 也不由 LLVM 使用，统一留给 Runtime |

Wbuf 的 inline fast path性能更积极，但任何相关 Runtime 布局变化都要求 LLVM、Runtime、
stdlib/stdx 和应用整体重编。Region-scan 当前 barrier ABI 主要收缩到 phase byte 与 wrapper
函数签名，维护边界更清晰。

## 13. 性能影响判断

### 13.1 不容易劣化的路径

- 普通 ref/weak/static/struct read：两边都在 LLVM 层变成直接内存操作；
- Idle 六类 write wrapper：两边都是 phase gate 后 raw store/copy；
- dense bulk write：Region-scan 的 16-entry word batch、pending-word 合并和单 Region token
  可能减少 object-pointer queue 流量；
- callsite 代码体积：Region-scan Mark 路径较小，可能减少 I-cache 和二进制体积压力。

### 13.2 Region-scan 更容易劣化的路径

- Concurrent Mark 中高频标量 ref/static-ref write：
  - Wbuf 可在 target 中完成常见路径；
  - Region-scan 每次进入 Runtime，且标量 batch 立即 flush；
- 稀疏、随机、跨大量 Region 的 old ref：
  - bitmap word/pending summary/Region task不容易聚合；
- 多 mutator 同时发现同一 Region 中不同对象：
  - `pendingWordSummary` 和 scan state 所在 Region metadata 可能发生 cache-line 竞争；
- atomic-reference-heavy 且 CAS 冲突高：
  - Region-scan CAS loop 与失败前 discover 可能放大成本；
- atomic read：
  - 两边当前都保留 Runtime call，属于共同优化空间。

### 13.3 Region-scan 可能更有利的路径

- old refs 集中在少量 Region/bitmap words；
- struct/array bulk 覆盖大量相邻引用；
- 一个 Region 内连续发现多个对象；
- WorkBuf 对象地址流量、per-P active log 和 partial log flush 成为瓶颈；
- 应用大部分时间处于 Idle，且屏障 callsite 数量很多、对代码体积敏感。

因此不能仅从“是否有 Mark LLVM inline”推导整个 GC 的最终吞吐。当前可以从源码确认机制和
热点风险，但 Wbuf 的历史 benchmark 数字不能直接归因给当前 Region-scan revision；需要用
同一 compiler/runtime/stdlib、同一 workload 和同一采样口径重新测量。

## 14. 建议验证矩阵

| 维度 | 用例 |
|---|---|
| phase | Idle、MarkSetup、ConcurrentMark、MarkTermination |
| 普通引用 | null old、same-value、already-marked、white small、white large、非 heap |
| bulk | short/long GCTib、跨 bitmap word、跨 Region、重叠 memmove、空范围 |
| generic | heap->heap、heap->stack、stack->heap、no-ref payload、ref payload |
| atomic | store/swap/CAS success/CAS fail、低竞争、高竞争、全部 memory order |
| target | AArch64 Mach-O、AArch64 ELF ±LSE、X86-64 ELF、MinGW COFF、X86 Mach-O fallback |
| 正确性 | `cjMSVerifyMark`、reachable-unmarked 检查、phase 切换并发写压力 |
| 性能 | barrier calls/s、Runtime fallback 次数、mark-word RMW、pending publish、Region tasks、WorkBuf publish、P99 |

## 15. 源码索引

### 15.1 两边共同关注

| 组件 | 文件 |
|---|---|
| intrinsic 定义 | `llvm/include/llvm/IR/Intrinsics.td` |
| intrinsic -> API/wrapper lowering | `llvm/lib/CodeGen/CJBarrierLowering.cpp` |
| target 共享 ABI | `llvm/include/llvm/CodeGen/CJMSGCABI.h` |
| Runtime API | `src/CompilerCalls.cpp`、`src/CompilerCalls.h` |
| phase 安装 | `src/Heap/Heap.cpp` |
| Idle barrier | `src/Heap/MBarrier/MSIdleBarrier.h` |
| Mark barrier | `src/Heap/MBarrier/MSMarkBarrier.cpp`、`MSMarkBarrier.h` |
| raw 64-bit RefField helper | `src/Heap/MBarrier/MSRefField.inline.h` |

### 15.2 Wbuf 特有

| 组件 | `WB_RT` / `WB_LLVM` 下的文件 |
|---|---|
| activeLog producer | `WB_RT/src/Heap/MBarrier/MSMarkBarrier.cpp` |
| activeLog 生命周期 | `WB_RT/src/Heap/MCollector/MarkSweepCollector.cpp` |
| WorkBuf 布局/池 | `WB_RT/src/Common/LFStack.h` |
| TLS activeLog ABI | `WB_RT/src/Heap/MAllocator/MSAllocBuffer.h` |
| bulk mark-word batch | `WB_RT/src/Heap/MCollector/MSMarkWordBatch.h` |
| AArch64 inline | `WB_LLVM/llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp` |
| X86 inline | `WB_LLVM/llvm/lib/Target/X86/X86MSGCEmitter.cpp` |

### 15.3 Region-scan 特有

| 组件 | `RS_RT` / `RS_LLVM` 下的文件 |
|---|---|
| barrier producer | `RS_RT/src/Heap/MBarrier/MSMarkBarrier.cpp` |
| mark-word flush / Region consumer | `RS_RT/src/Heap/MCollector/MarkSweepCollector.cpp` |
| 16-entry mark batch | `RS_RT/src/Heap/MCollector/MSMarkBatch.h` |
| pending summary / scan state | `RS_RT/src/Heap/MAllocator/MSRegionInfo.h` |
| REGION/OBJECT scheduler | `RS_RT/src/Heap/MCollector/MSMarkScheduler.cpp`、`.h` |
| AArch64 phase-only inline | `RS_LLVM/llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp` |
| X86 phase-only inline | `RS_LLVM/llvm/lib/Target/X86/X86MSGCEmitter.cpp` |
