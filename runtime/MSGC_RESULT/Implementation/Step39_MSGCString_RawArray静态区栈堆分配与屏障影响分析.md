# Step39: MSGC String / RawArray 静态区、栈、堆分配与写屏障影响分析

> 记录时间：2026-07-11
>
> 前置：Step38 后续问题澄清；聚焦 `String`、`RawArray` 的真实布局与实际分配位置
>
> 口径：以当前 `cangjie_compiler` 与 `cangjie_runtime_MSGC` 源码为准
>
> 关系：服务于“尽量减少堆分配”和“哪些场景可以进一步消除 MSGC barrier”的判断

## 目录

1. 结论先行
2. `String` 与 `RawArray` 的真实形态
3. 哪些会落到静态区 / 栈上 / 堆上
4. 对 MSGC 写屏障的直接影响
5. 当前可继续推进的优化方向
6. 代码定位

## 1. 结论先行

1. `String` 当前本来就是 **值类型 struct**，不是“单独再分配一个 heap string object”。
2. `String` 里面真正的引用字段只有一个：`myData: RawArray<UInt8>`；另外两个字段是 `start`、`length`。
3. `RawArray` 不是普通裸 struct，而是 **托管数组对象**，有对象头，默认走 GC 堆分配。
4. **字符串字面量**的底层字节数组不是堆对象，而是编译器生成的 **静态全局 `RawArray<UInt8>` backing**；对应 `String` 也会被初始化为指向这块静态 backing。
5. “新写入的 `String.myData` 指向静态区”这件事，**本身不足以删除 MSGC 写屏障**。因为 MSGC 这里是删除屏障 / SATB 语义，保护的是“被覆盖掉的旧引用”，不是新引用。
6. 真正能直接减少 barrier 的场景仍然是：
   - 目标容器不在 heap；
   - 首次初始化写，旧值确定为 `null`；
   - 旧值确定是静态 / 非 heap 引用；
   - 整个 struct 本身可在 IR 层判定为 no-ref，或者是 fresh-init 写。
7. 若目标是“尽量减少堆中分配”，对 `String` 更有价值的方向不是把 `String` 再“做成 struct”，而是：
   - 让更多 backing 复用静态区；
   - 让更多短命 `RawArray<UInt8>` 被逃逸分析改写到栈上；
   - 在 fresh-init 路径上进一步消掉 `String` 的 ref/struct barrier。

## 2. `String` 与 `RawArray` 的真实形态

### 2.1 `String`：编译器视角下就是 struct

编译器里 `String` 不是特殊 heap class，而是 `std.core.String` 这个 struct：

- [`Types.cpp`](../../../cangjie_compiler/src/AST/Types.cpp) 的 `Ty::IsString()` 会判断它是否是 `std.core.String`
- [`CHIRContext.cpp`](../../../cangjie_compiler/src/CHIR/IR/CHIRContext.cpp) 的 `GetStringTy()` 直接返回 `GetStructType("std.core", "String")`

`CJNativeIRBuilder` 里有明确注释：

```text
struct String {
    private let myData: RawArray<UInt8>
    private let start: UInt32 = 0
    private let length: UInt32 = 0
}
```

对应 LLVM 侧布局在 [`CGContext.cpp`](../../../cangjie_compiler/src/CodeGen/CGContext.cpp) 中被建成：

```text
{ i8 addrspace(1)*, i32, i32 }
```

也就是说：

- 第 1 个字段是托管引用 `myData`
- 第 2/3 个字段是纯标量 `start` / `length`

### 2.2 `RawArray`：托管数组对象，不是普通裸 struct

runtime 里 `RawArray` 属于对象模型里的数组对象：

- [`Flags.h`](../../src/ObjectModel/Flags.h) 中 `TYPE_KIND_RAWARRAY = -126`
- [`MArray.h`](../../src/ObjectModel/MArray.h) 中 `MArray : public BaseObject`
- [`BaseObject.h`](../../src/Common/BaseObject.h) 中 `BaseObject` 自带对象头 `StateWord`
- [`MArray.inline.h`](../../src/ObjectModel/MArray.inline.h) 中 `GetContentOffset() = sizeof(MArray)`，数组内容紧跟在对象头与 `length` 之后

可以把它理解成：

```text
RawArray object =
    [BaseObject header / stateWord]
    [length]
    [payload bytes or payload refs...]
```

所以 `RawArray<UInt8>` 的“底层串”并不是一块脱离 GC 的普通 C buffer，而是一个托管数组对象；只是**字面量场景**下，编译器会直接生成一份静态全局 backing，而不是在运行期 `malloc/newarray`。

### 2.3 为什么 `String` 会被当成“带 ref 的 struct”

[`CGUtils.cpp`](../../../cangjie_compiler/src/CodeGen/Utils/CGUtils.cpp) 的 `IsTypeContainsRef()` 会把 `addrspace(1)*` 判定为引用字段，并递归检查 struct/array/vector 成员。

因此从 barrier lowering 角度看，`String` 不是 no-ref struct，而是：

- 一个 **包含 1 个 ref 字段** 的小 struct；
- 对 `String` 的整体写入，依然属于 `gcwrite_struct` / `gcwrite_static_struct` 这一路的候选对象。

## 3. 哪些会落到静态区 / 栈上 / 堆上

| 场景 | `String` 本体位置 | `RawArray` backing 位置 | 说明 |
|---|---|---|---|
| 字符串字面量全局常量 | 静态区 / 全局初始化数据 | 静态区 / 全局 `RawArray` backing | 编译器在包初始化时直接构造 |
| 函数内拿一个字面量做局部值 | 栈槽 / SSA 临时值 | 静态区 | `String` 值本体可能在栈上，但 backing 仍指向静态区 |
| heap 对象中的一个 `String` 字段 | 这个 `String` 的 12/16 字节内容嵌在 heap 对象里 | 视来源而定：静态区或 heap | 这里写字段时可能触发 struct/ref barrier |
| 动态构造的新字符串 backing | `String` 本体常常是栈上临时值或嵌入宿主对象 | 默认 heap | `RawArray` 默认走 `AllocateArray` / `cj_malloc_array` |
| 短命、不逃逸、大小可知的 `RawArray` | `String` 本体仍是局部值 | 可能被改写到栈上 | 取决于 `CJPartialEscapeAnalysis` 是否命中 |

### 3.1 字面量：静态区，不进 heap

[`CJNativeIRBuilder.cpp`](../../../cangjie_compiler/src/CodeGen/CJNative/CJNativeIRBuilder.cpp) 的 `CreateStringLiteral()` 会先登记字符串字面量信息。

真正的全局构造发生在 [`EmitPackageIR.cpp`](../../../cangjie_compiler/src/CodeGen/CJNative/EmitPackageIR.cpp)：

1. 先把所有字面量内容拼接 / 去重到一个静态全局 backing；
2. 再为每个 `String` 常量初始化 `{ backingPtr, start, len }`。

因此“字面量 string 的底层串”已经不是 heap 对象，而是编译期就固定好的静态对象。

### 3.2 动态 `RawArray`：默认走 heap

动态数组分配路径在：

- [`ArrayImpl.cpp`](../../../cangjie_compiler/src/CodeGen/Base/ArrayImpl.cpp) `GenerateRawArrayAllocate`
- [`CJNativeIRBuilder.cpp`](../../../cangjie_compiler/src/CodeGen/CJNative/CJNativeIRBuilder.cpp) `AllocateArray`

最终会调用 `cj_malloc_array` / `cj_malloc_array_generic`，也就是正常的托管堆分配。

所以如果不是字面量、静态常量、或后续逃逸分析提升，`RawArray<UInt8>` 默认仍然在 heap 上。

### 3.3 哪些可以上栈

`CJPartialEscapeAnalysis` 会尝试把“不逃逸对象 / 数组”从 heap 改写到栈上，入口在：

- [`CJPartialEscapeAnalysis.cpp`](../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Transforms/IPO/CJPartialEscapeAnalysis.cpp)

这里对数组有几个关键约束：

1. 长度要能在编译期看成常量；
2. 元素大小要可确定；
3. 总大小不能超过阈值；
4. 整个对象 / 数组不能逃逸。

当前阈值大致是：

- 普通对象：`cj-max-stack-obj = 1024`
- 数组：`cj-max-nonmove-array-size = 64 * 1024`

因此：

- **局部 `String` 本体** 很容易在栈上或寄存器里；
- **局部 `RawArray<UInt8>` backing** 只有在“不逃逸 + 固定大小 + 不太大”时，才有机会被改写到栈上。

## 4. 对 MSGC 写屏障的直接影响

### 4.1 先看当前 MSGC 写屏障保护什么

当前 MSGC 写屏障是 **删除屏障 / SATB 风格**，核心是先处理旧值，再真正写新值。

runtime 中：

- [`MSMarkBarrier.cpp`](../../src/Heap/MBarrier/MSMarkBarrier.cpp) `WriteReference()`
- [`MSMarkBarrier.cpp`](../../src/Heap/MBarrier/MSMarkBarrier.cpp) `WriteStruct()`

它们本质上做的是：

1. 先读出旧引用或旧 struct 范围内的 ref；
2. 把“被覆盖掉的旧 ref”尝试记到 log / mark；
3. 再执行真正的 store / memcpy。

因此 barrier 关心的是 **old ref**，不是 **new ref**。

### 4.2 “新值指向静态串”为什么不等价于“可以删屏障”

假设要写：

```text
obj.someString = "hello"
```

此时新写入的 `String.myData` 可能确实指向静态 backing，但 MSGC 仍然先关心：

```text
old = obj.someString.myData
```

如果 `old` 原来指向一个 heap `RawArray<UInt8>`，那这次覆盖就必须先把 `old` 按 SATB 语义处理掉。  
也就是说：

- `new` 是静态 / 非 heap：**不够**
- `old` 也是静态 / 非 heap / null：**才可能删**

这也是为什么“把 string backing 尽量静态化”能减少 heap 分配，但**不会自动等价成 barrier 消失**。

### 4.3 当前 runtime 已经有的快速放行

当前 runtime 已经有几类天然放行：

1. **目标容器不在 heap**

   [`CompilerCalls.cpp`](../../src/CompilerCalls.cpp) 的 `MCC_WriteRefField` / `MCC_WriteStructField` 会先判断宿主对象是否在 heap，不在 heap 就直接走普通写入。

2. **旧值为 `null`**

   `WriteReference()` 里若旧值为空，`EnqueueRefFast()` 会直接返回。

3. **旧值不在 heap**

   `EnqueueRefFast()` / `EnqueueRefInto()` 内部会先做 heap-range 过滤，非 heap 引用不入 log。

4. **inline fastpath 同样已有这三类放行**（补充：上面只列了 runtime C++ 侧）

   `emitMccWriteBarrierFastPath` 的 ref 路径里：`old==null`、`old==new` 在 gate 后 2-4 条指令即
   StoreOnly；`EmitInlineMarkOrTo` 的第一步就是对 old 做 heap-range check，**静态 backing 作为
   old 被覆盖时在 inline 内 ~10 条指令处 StoreOnly 放行**，不会走到 region 解析和 mark。
   所以"旧值是静态 backing"场景的运行期代价已经很低；编译期剪枝的净收益只剩 gate + old load
   （~7 条），只在极热点位值得做。

这几个点说明：  
如果编译器能在 IR 层提前证明“这次被覆盖的旧值一定是空 / 非 heap”，就有机会直接把 barrier 整体剪掉；
但要按 §4.3-4 的口径算净收益（运行期已有低成本放行），优先级低于 §5.4 的实测热点。

### 4.4 落到 `String` 上，真正值得做的删 barrier 点

围绕 `String`，更准确的优化点是：

1. **fresh-init 写**

   新对象字段第一次写 `String` 时，旧值若由分配清零保证为 `null`，则 `myData` 的 ref barrier 可以降成 plain store；整体 `gcwrite_struct(String)` 也可以往“初始化写消除”方向做。

2. **旧值可证明来自字面量 / 静态常量**

   若编译器知道被覆盖的旧 `String.myData` 一定是静态 backing，则 SATB 不需要记录它；这类场景可继续做更激进的 IR 消除。

3. **目标不是 heap object**

   比如局部变量、栈上聚合值、临时 `String`，本来就不需要 runtime barrier。

反过来说：

- 仅仅因为“这次要写入的 `String` 新值引用的是静态 backing”，**不能**直接删掉 heap object 上的 `gcwrite_struct(String)`。

## 5. 当前可继续推进的优化方向

### 5.1 减少 heap 分配：优先盯 `RawArray<UInt8>` backing

如果目标是减少 heap 压力，优先级应放在 backing 上，而不是 `String` 本体上：

1. 扩大编译期静态化命中面  
   让更多常量字符串、切片常量、拼接后常量折叠结果直接共享静态 backing。

2. 提升 `RawArray<UInt8>` 栈化命中率  
   重点看短命构造、局部 builder、中间临时 buffer 是否能被 `CJPartialEscapeAnalysis` 改写成 `alloca`。

3. 避免不必要的中间 backing  
   某些字符串操作如果能只传 `(backing,start,len)` 视图，而不是重新 materialize 新 backing，会比单纯做 barrier 优化更直接。

### 5.2 减少 barrier：优先盯“旧值已知安全”的场景

对 `String` 来说，屏障优化优先级更高的是：

1. `gcwrite_ref` 的 fresh-init 消除；
2. `gcwrite_struct(String)` 的 fresh-init 消除；

   ——1/2 即 Step38 ②（init store elision）：SATB 删除屏障 + 分配清零保证下，新对象字段首次写
   old==0，等价于裸 store，连 gate 都不需要。硬门槛（Step38 §4-②）：普通对象白分配、清零覆盖全部
   size class、`CJ_MS_VERIFY_MARK=1` 全绿。JSON 反序列化的 `String` 字段初始化是这类的最大来源。

3. “旧值确定来自静态 backing / 非 heap”的编译期剪枝——注意 §4.3-4：inline fastpath 运行期已在
   ~10 条指令处放行这类 old，编译期剪枝净收益只有 gate+old load，优先级最低；
4. **`String` struct 写拆分（scalarize，= Step38 ①a，String 是首要目标类型）**：

   现状：`obj.someString = s` 走 `gcwrite_struct` 完整 fastpath——gate + LDARX 对象头 + gctib
   提取/bit63 分流 + dst/size 对齐检查 + **3-slot 位图循环**（其中只有 1 个 slot 是 ref）+
   activeLog 链 + `BL memcpy`。拆成：

   ```text
   gcwrite_ref(myData)   ; 走 ref fastpath：自带 old==null / old==new 短路（struct 路径无 same-value 短路）
   plain store(start)    ; 纯标量
   plain store(len)
   ```

   收益：省掉头查表、位图循环骨架、memcpy 调用整段；`String` 反复被赋相同值时 same-value 短路
   直接 StoreOnly。判据：常量 size ≤ ~32-48B 且 ref 字段数 ≤ 1（String 的 {ref,u32,u32} 恰命中）。
   实现位置：CJBarrierLowering 单点（用 `GCInfo.getOrInsertTypeGCInfo(ST)` 位图逐字段拆），
   前置依赖 Step38 ①c——前端 `CallGCWriteAgg` 给 `cj_gcwrite_struct` 补挂 AggType metadata
   （与 static 版对齐，一行）。先编译 std/stdx 统计剩余 `gcwrite_struct` 的类型分布确认 String
   占比，再定拆分阈值。

> 状态更新（2026-07-11）：本节对应的两项编译期优化现已落地：
> 1. `gcwrite_ref` / `gcwrite_struct(String)` 的 fresh-init 消除已在 `CJBarrierOpt` 实现；
> 2. `String`/同类 one-ref 小 struct 的 scalarize 已实现，当前覆盖"恰 1 个直接 ref 字段"的
>    `cj_gcwrite_struct`，生成 `gcwrite_ref + memcpy/plain store` 组合。
>
> 实测口径上，**效果基本与之前一致**，没有形成新的吞吐台阶。这与本文前面的分析一致：`String`
> 相关优化更多是在清理 struct/ref barrier 的常数项与无效工作，当前主瓶颈仍然在 GC 周期频率、
> MarkTerm 和 mutator assist，而不是 `String` 这条单点路径。

### 5.3 不建议误判的方向

下面这个判断当前不成立：

> “只要 string 的底层串是静态保活的，就可以不用做 barrier”

严格说，应该改成：

> “只要这次**被覆盖掉的旧 ref** 可证明是 `null` 或非 heap，就可以不用做 SATB 删除屏障；新 ref 是否静态，只影响分配压力，不直接决定屏障是否可删。”

### 5.4 结合 c1000/p100K 实测的补充优化点（2026-07-11 补）

背景数据（`20260710_222417_case27_validate_c1000_p100K_60s_heap4gb_5runs/run3` gc.log，
吞吐 285 req/s，161 周期/60s，threshold 已自适应到 341MB——**非** 20MB 地板问题）：

```text
WriteBarrierFlush forwarded = 54万~119万 objects / 周期   ← mutator 每周期百万次 region 解析 + 原子 fetch_or
ConcurrentMark 146~202ms / 周期（mark 相占墙钟 ~40-55%）
assist processed 314万/352万 ≈ 93% 的标记由 mutator 完成（bgCredit 仅 ~1.5万，后台近乎旁观）
MarkTermination 50~83ms STW，STW 内 newlyMarked 85万~99万
```

在此数据下，围绕 `String`/`RawArray` 的新增优化点：

#### 5.4.1 大对象 old 的 inline flags_ pre-test ❌ 已实施并实测劣化，已回退（2026-07-11）

原设想：p100K 的 ~97KB `RawArray<UInt8>` backing 是 LARGE 对象，被覆盖时 inline 必走 slow 链
（读码修正：LARGE 实际是在 `regionStart = regionEnd - 32KB` 下界检查处被踢走的，不是
markBitmap CBZ）。实施为：下界检查 LO 目标改指 `LLargeMaybe` 块（regionType==LARGE →
allocPtr==value 校验 → `LDRB flags_@51, TBNZ FLAG_MARKED → Already`），小对象路径零新增指令；
C++ `AtomicTestAndSetMarked` 同步加 pre-test。

**实测劣化,原因见 5.4.3;已全部回退**(发射层 + C++ + 尺寸账本 448/576 还原 384/512)。
残余价值:大 backing 被重复当 old 覆盖的频率本来就低(每请求个位数:ArrayList 扩容尾部、
buffer 换代),收益上限撑不起代码体积与分支成本。

#### 5.4.2 mark-CAS 前 pre-test（test-and-test-and-set）❌ 已实施并实测劣化，已回退（2026-07-11）

实施为：发射层 `LDSETALX` 前插 `LDR word → ANDS → B.NE Already`；C++
`TestAndSetMarkBit` 同步加 relaxed 读 pre-test。正确性无问题（位图周期内单调置位，
读到 1 即定论，过期 0 落回原子裁决）——**但性能实测劣化，已全部回退**。

#### 5.4.3 劣化复盘（重要教训，后续屏障/mark 改动的判据）

pre-test 的收益模型：`收益 ≈ p×原子成本 − (1−p)×pre-test成本 − 分支错判 − icache 成本`，
其中 **p = 已标命中率**。这次错在对 p 的估计：

1. **mark 位图是 set-once 结构**：每个对象每周期恰好被成功标记一次；"已标命中"只来自重复
   访问（屏障碰已标 old、扫描碰已发现 child）。而 case27 的堆是 **JSON 树**：入度 ≈ 1、
   边数 ≈ 节点数、对象每请求新建用完即死——**first-mark 占绝对多数，p 极低**，pre-test
   几乎永远失败，3 条指令（相依 load + ANDS + 数据依赖分支）全是纯开销。
2. **最重的一刀砍在消费端热循环**：C++ `TestAndSetMarkBit` 是 `DiscoverObjectNoCount` 的
   核心，mark/discover 主循环每周期 330 万+ 对象**按定义几乎全是首标**——给全系统最热的
   循环每次迭代加了一个必失败的 load+branch。
3. **数据依赖分支错判**：bit 是否已置位对预测器接近随机，错判 13-15 cycle；原 LDSETAL 前
   没有任何分支。
4. **代码体积**：每个 `EmitInlineMarkOrTo` 实例 +13 条（52B）× 全 binary 数千个内联点位，
   I-cache/ITLB 压力全线上升。
5. LDSETAL 在 Apple Silicon 上 line 已本地时并不贵，收益上限本来就低。

**判据（与 §3 的 -20% 教训并列）**：
- pre-test/去重类优化只在 **p 高的堆形状**（共享子图、循环引用、热字段高频覆盖同一 old）
  才可能为正；树形 + 短命对象 workload **禁用**；
- 想复核必须先实测 p：诊断构建里给 `TestAndSetMarkBit` 加 attempts/already-hit 计数，
  拿到数字再决定，**不许按直觉估 p**（本次教训）；
- 百万级/秒执行的路径上，"+2~3 条必执行指令"不是免费的，哪怕它理论上省一个原子。

#### 5.4.3 [大改，依赖后台产能] 半路线 B：屏障降级为 pre-test + raw log

route-A 让 mutator 每次 log 付"region 解析 ~20 条 + 原子 RMW"×百万次/周期。可改成：
pre-test 已标 → skip（保留大部分去重）；未标 → **直接 append（needsDiscover=true），不做原子**；
CAS 挪到消费端 `DiscoverObjectNoCount`——**消费端分流代码 Step37 §11.2 明说保留着，现成**。
mutator 屏障从 ~70 条（含原子）降到 ~35 条（纯读）。

⚠️ 前提是先解决**后台 mark 线程饿死**（gc thread 2 + helper 1 on 8P，bgCredit 1.5万 vs assist
314万；疑似 local-first list1_ 留活在 per-P、后台偷不到）：否则只是把活从 mutator 屏障挪到
mutator assist，换汤不换药。后台分担 + MarkTerm 提前收敛（published 块并发 drain，别堆到 STW）
属于 mark 侧主线，见 Step38 §4/§6 与后续专项。

#### 5.4.4 优先级（本节口径）

```text
后台 mark 分担（先查饿死机制） > MarkTerm 提前收敛 > 5.4.2 pre-test（顺手）
> 5.4.1 LARGE pre-test（p100K 直接命中） > §5.2-4 String 拆分（①a）
> §5.2-1/2 fresh-init（②，需白分配/清零门槛） > 5.4.3 半路线B（拿到后台产能数据后再定）
```

## 6. 代码定位

### 6.1 `String` 结构与 lowering 判定

- `../../../cangjie_compiler/src/AST/Types.cpp`
- `../../../cangjie_compiler/src/CHIR/IR/CHIRContext.cpp`
- `../../../cangjie_compiler/src/CodeGen/CGContext.cpp`
- `../../../cangjie_compiler/src/CodeGen/Utils/CGUtils.cpp`
- `../../../cangjie_compiler/src/CodeGen/CJNative/CJNativeIRBuilder.cpp`

### 6.2 字面量 backing 的静态化生成

- `../../../cangjie_compiler/src/CodeGen/CJNative/CJNativeIRBuilder.cpp`
- `../../../cangjie_compiler/src/CodeGen/CJNative/EmitPackageIR.cpp`

### 6.3 `RawArray` / `MArray` 对象模型

- `../../src/ObjectModel/Flags.h`
- `../../src/ObjectModel/MArray.h`
- `../../src/ObjectModel/MArray.inline.h`
- `../../src/Common/BaseObject.h`

### 6.4 动态数组分配与栈化

- `../../../cangjie_compiler/src/CodeGen/Base/ArrayImpl.cpp`
- `../../../cangjie_compiler/src/CodeGen/CJNative/CJNativeIRBuilder.cpp`
- `../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Transforms/IPO/CJPartialEscapeAnalysis.cpp`

### 6.5 当前 MSGC 写屏障入口

- `../../src/Heap/MBarrier/MSMarkBarrier.cpp`
- `../../src/CompilerCalls.cpp`
