# Step64：MSGC Struct 写屏障 short-GCTib TLS fast-add inline 设计

## 0. 文档定位

本文定义 Region-scan MSGC 中以下两个写屏障 wrapper 的 target-callsite inline
方案：

- `CJ_MCC_WriteStructFieldFast_MSGC`
- `CJ_MCC_WriteStaticStructFast_MSGC`

本文既是实施规格，也是本轮代码落地后的实现说明。

固定代码范围如下，禁止与 Wbuf 工作区交叉修改：

- Region-scan Runtime：
  `/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime`
- Region-scan LLVM：
  `/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler_region_mark/third_party/llvm-project`

Wbuf 代码仅用于核对动态 TypeInfo/GCTib 枚举和函数内 leaf thunk 的组织方式，
不能把 Wbuf 的“直接修改全局 mark bitmap + activeLog”实现复制到 Region-scan。

本文基于 2026-07-31 当前源码，承接
`Step63_MSGC写屏障_TLS_BitmapBatch_Runtime_LLVM联动设计.md` 已经落地的约束：

- GCPhase 判断统一使用当前线程的 `encodedGCPhase`；
- 不使用 `WRAPPER_ONLY`、`TLS_BATCH_V3`、`MARK_WRAPPER_ONLY` 等开关；
- LLVM 只保留 `enable-msgc-inline-write-barrier`，默认开启；
- `buffer.markEpoch` 已删除，`currentMarkEpoch` 保留；
- Ref/StaticRef 已实现 collect-set Small old-ref 的 TLS buffer fast-add；
- Struct/StaticStruct 的 Runtime wrapper 已使用同一个持久
  `AllocBufferMS::barrierMarkBuffer_`；
- Runtime wrapper 会先完整捕获 old-ref，再执行 `memcpy_s`。

## 1. 目标与非目标

### 1.1 目标

1. 在非 Mark GCPhase 中继续直接执行 `memcpy`。
2. 在三个 MSGC Mark GCPhase 中：
   - `WriteStructField` 动态解析对象 TypeInfo 和 short GCTib；
   - `WriteStaticStruct` 直接解析参数中的 short GCTib；
   - RawArray 的 component 明确无引用时直接 CopyOnly；
   - 对覆盖前的每个 old-ref 执行与 Ref/StaticRef 相同的 TLS fast-add；
   - 全部 old-ref 处理成功后才执行 `memcpy`。
3. 通过函数内 local leaf thunk 复用完整 TLS 8-set/4-way 查找逻辑。
4. 任意 fast-add 失败时保留已加入 TLS buffer 的 bit，调用原 Runtime wrapper
   重新枚举整个范围。
5. AArch64 与 X86-64 保持相同语义，同时遵守各自 ABI 和寄存器约束。

### 1.2 非目标

本次不直接 inline：

- StdGCTib/long-form GCTib；
- RawArray 中引用元素或含引用 struct 元素的枚举；
- `ArrayCopyRef`、`ArrayCopyStruct` 的 Mark 路径；
- atomic write/swap/CAS；
- LARGE、OUTSIDE_SET 或不能证明为合法 collect-set Small 的 old-ref；
- Runtime flush、Region publication 或 mark worker 唤醒。

这些情况继续由 Runtime canonical 路径处理。

## 2. 当前实现

### 2.1 LLVM

Region-scan LLVM 已把两个 wrapper 识别为
`MSGCWriteBarrierKind::Struct/StaticStruct`，当前 target emitter 行为是：

```text
读取一次 encodedGCPhase
  -> IDLE/FINISH/RECLAIM_SATB_NODE：memcpy
  -> MS_MARK_SETUP/MS_CONCURRENT_MARK/MS_MARK_TERMINATION：
       short GCTib 枚举 + TLS fast-add + memcpy
  -> 未知 GCPhase：wrapper
```

两个三元素连续区间分别使用 unsigned range check，不再逐个执行六次等值比较。
Ref/StaticRef 与 Struct leaf thunk 共用以下 fast-add emitter body：

```text
old-ref heap/Region/slot 解析
  -> Region kind == Small
  -> collectEpoch == currentMarkEpoch
  -> 依赖 collect-set 元数据不变量直接计算 slot coordinate
  -> lastEntry
  -> 8-set/4-way lookup
  -> 匹配 entry：无条件 OR mask，并更新 pending-add 计数
  -> merge/insert
  -> pendingBitCount/occupancy/lastEntry 更新
```

Struct/StaticStruct 在进入循环前只解析一次 fast-path state 和 TLS buffer。AArch64
将二者保存在 `x16/x11`，X86 将二者保存在 caller-saved `xmm0/xmm2`，leaf thunk
不再为每个 old-ref 重复解析。

X86 仅在真正进入 Mark Struct 后保存 wrapper 参数。IDLE copy 和未知 phase slow
call 使用原始 ABI 参数，不再在 phase gate 前保存并在所有出口恢复。

### 2.2 Runtime

`MSBarrierOps::WriteStruct` 当前语义：

```text
srcLen == 0：return
  -> 在枚举前检查 srcLen <= dstLen 和地址合法性
  -> 按 TypeInfo/GCTib 枚举 [dst, dst + srcLen)
  -> 每个 old-ref 进入 BarrierMarkContext::AddOldRefActive
  -> memcpy_s(dst, dstLen, src, srcLen)
```

`MSBarrierOps::WriteStaticStruct` 当前语义：

```text
在枚举前检查 srcLen <= dstLen 和地址合法性
  -> gctib.ForEachBitmapWordInRange(dst, dst, dst + srcLen)
  -> 每个 old-ref 进入 BarrierMarkContext::AddOldRefActive
  -> memcpy_s(dst, dstLen, src, srcLen)
```

Runtime wrapper 已经满足 partial fast-add 失败后的完整重放要求，因此本设计不新增
Runtime continuation、flush 或“从第 N 个 ref 继续”的 ABI。

Collector 已将 buffer lookup 结果直接传给插入 helper，Runtime new-key
mark-plane probe 与插入不再对同一 `wordKey` 重复执行 4-way lookup。LLVM leaf
thunk 不执行 mark-plane probe。

## 3. 统一 GCPhase 分流

两个 target emitter 都只读取 `encodedGCPhase`：

```text
GC_PHASE_IDLE
GC_PHASE_FINISH
GC_PHASE_RECLAIM_SATB_NODE
    -> CopyOnly

GC_PHASE_MS_MARK_SETUP
GC_PHASE_MS_CONCURRENT_MARK
GC_PHASE_MS_MARK_TERMINATION
    -> Mark

其他值
    -> Slow wrapper
```

AArch64 从打包的 CJTLS 顶字节取得 `encodedGCPhase`；X86 从固定 CJTLS offset
读取同一字段。两个 target 都先判断 idle 区间，再判断 mark 区间。不能再引入第二个
barrier phase 字段或基于实现版本编号分流。

同一次 barrier 调用只在入口检查一次 GCPhase。Mark GCPhase 的切换受 GC
safepoint/STW 协议控制；函数内 leaf thunk 不是 safepoint，也不重复读取 GCPhase。

## 4. 复制参数与共同前置检查

### 4.1 `WriteStructField` ABI

```text
(obj, dst, dstSize, src, srcSize)
```

### 4.2 `WriteStaticStruct` ABI

```text
(dst, dstSize, src, srcSize, gctib)
```

### 4.3 共同规则

进入 CopyOnly 或 Mark 主体前执行：

```text
if srcSize == 0:
    finish

if dst == null or src == null:
    slow

if srcSize > dstSize:
    slow
```

`srcSize > dstSize` 必须回 Runtime，不能直接调用不带边界检查的 `memcpy`，否则会
偏离 Runtime `memcpy_s` 的错误处理语义。

成功 copy 仍使用：

```text
memcpy(dst, src, srcSize)
```

## 5. `CJ_MCC_WriteStructFieldFast_MSGC` Mark 路径

### 5.1 容器对象检查

先检查 `obj`：

```text
obj 不在 heap reserve
    -> CopyOnly

obj 在 heap reserve
    -> 动态解析 TypeInfo
```

这与 Runtime `MCC_WriteStructField` 一致：非堆容器不需要执行 MSGC old-edge
barrier，但仍需完成复制。

### 5.2 动态 TypeInfo 解码

对象第一个机器字是 StateWord。64 位 MSGC 中 TypeInfo 位于 StateWord 低 48 位：

```text
stateWord = acquire-load [obj]
typeInfo = stateWord & ((1ULL << 48) - 1)

if typeInfo == null:
    slow
```

AArch64 使用 acquire load；X86 普通 load 在 TSO 下提供所需读取顺序。

然后读取：

```text
typeKind = *(uint8_t*)(typeInfo + 8)
gctib    = *(uint64_t*)(typeInfo + 16)
```

上述 offset 必须在 `CJMSGCABI.h` 中集中声明，并与 Runtime
`TypeInfoCompilerABIContract` 的 `static_assert` 保持一致。

### 5.3 支持边界

动态 TypeInfo 先按对象类别分流：

```text
if typeKind == TYPE_KIND_RAWARRAY:
    componentTypeInfo = *(TypeInfo**)(typeInfo + 64)
    if componentTypeInfo == null:
        slow

    componentFlags = *(uint8_t*)(componentTypeInfo + 9)
    if componentFlags & FLAG_HAS_REF_FIELD:
        slow

    CopyOnly
```

即 RawArray 不在 LLVM 中枚举引用元素，但 component 明确无引用时不需要
old-edge capture，可以与 Wbuf AArch64/X86 一样直接执行 `memcpy`。

普通非数组对象继续检查 short GCTib：

```text
if gctib.bit63 == 0:
    slow

bitmap = gctib & 0x7fffffffffffffff

if bitmap == 0:
    CopyOnly
```

以下情况回 Runtime：

- RawArray 的 `componentTypeInfo == null`；
- RawArray component 含引用；
- GCTib bit63 为 0，即 StdGCTib/long-form 指针；
- TypeInfo/GCTib 布局异常；
- 地址计算溢出或目标范围不能安全映射到 short GCTib。

### 5.4 精确覆盖范围

普通非数组对象的 GCTib 描述从 `obj + TYPEINFO_PTR_SIZE` 开始的 payload：

```text
payloadBase = obj + 8
copiedLen   = srcSize  // 已先验证 srcSize <= dstSize
scanStart   = alignUp(dst, 8)
scanEnd     = dst + copiedLen
```

如果 `scanStart >= scanEnd`，没有 ref slot 起点落在覆盖范围内，直接 CopyOnly。

否则检查：

```text
scanStart >= payloadBase
scanEnd 计算没有溢出
slotOffset = (scanStart - payloadBase) / 8
slotOffset < 63
```

然后：

```text
bitmap >>= slotOffset
cursor = scanStart

while bitmap != 0 and cursor < scanEnd:
    if bitmap & 1:
        oldRef = *(BaseObject**)cursor
        if TLSFastAdd(oldRef) == FAILURE:
            slow
    bitmap >>= 1
    cursor += 8
```

该范围计算镜像 Runtime `ShortGCTib::ForEachBitmapWordInRangeFast` 的核心语义，
不采用 Wbuf X86 当前“扫描整个容器 GCTib”的保守方式。

## 6. `CJ_MCC_WriteStaticStructFast_MSGC` Mark 路径

StaticStruct 不需要从对象 StateWord 动态解析 TypeInfo，GCTib 由第五个参数直接传入。

GCTib 是一个 64 位 tagged union。虽然 LLVM IR/ABI 中可能使用 pointer-shaped
参数承载它，target emitter 必须把它当作 64 位 bit pattern：

```text
if gctib.bit63 == 0:
    slow

bitmap = gctib & 0x7fffffffffffffff
if bitmap == 0:
    CopyOnly
```

short GCTib 的 bit0 对应 `dst` 的第一个 8 字节 slot：

```text
cursor = dst
end = dst + srcSize

while bitmap != 0:
    if cursor >= end:
        slow
    if bitmap & 1:
        oldRef = *(BaseObject**)cursor
        if TLSFastAdd(oldRef) == FAILURE:
            slow
    bitmap >>= 1
    cursor += 8
```

descriptor 超出 `srcSize` 的部分不在本次覆盖范围内，直接停止枚举并 copy；Runtime
同样只按 `[dst, dst + srcSize)` 枚举。地址溢出或 descriptor 在覆盖范围内无法安全
解析时仍回 Runtime。

## 7. 函数内 TLS fast-add leaf thunk

### 7.1 目的

不能为每个 old-ref 复制一份完整的 Region/slot 解析和 4-way lookup。
每个已展开的 Struct/StaticStruct barrier 在同一 managed caller 内只生成一份
local leaf thunk：

```text
StructLoop:
    oldRef = *cursor
    call .LMSGCWBTLSFastAdd
    if result == FAILURE:
        goto Slow
    goto StructLoop

.LMSGCWBTLSFastAdd:
    ...
    return HANDLED or FAILURE
```

该 label：

- 不是 Runtime 导出符号；
- 不产生新的 C/C++ ABI；
- 不调用其他函数；
- 不是 safepoint；
- 不调整全局 mark bitmap；
- 不执行 Runtime flush；
- 只修改当前 owner 的 `MSBarrierMarkBuffer`。

### 7.2 输入输出

逻辑接口：

```text
input:
    oldRef

result:
    HANDLED = 0
    FAILURE = 1
```

HANDLED 包含：

- oldRef 为 null；
- oldRef 成功 merge；本地已经 pending 的 bit 也执行幂等 OR；
- oldRef 成功插入空 way。

FAILURE 包含：

- 非 heap old-ref；
- 非 Small Region；
- `AllocBufferMS == nullptr`；
- 不属于 `currentMarkEpoch` 对应 collect-set；
- 4-way set 冲突；
- `pendingBitCount` 达到阈值；
- 任何 LLVM fast path 未覆盖的对象类别。

### 7.3 与当前 scalar 逻辑共用

实现时把当前 Ref/StaticRef 的 fast-add 机器码生成整理为一个可复用 emitter body，
由调用者提供两个终点：

```text
scalar:
    HANDLED -> Store
    FAILURE -> Slow

struct leaf thunk:
    HANDLED -> return 0
    FAILURE -> return 1
```

scalar 可以继续保持没有 `call/ret` 的直接展开，Struct 只在循环中调用 local thunk。
这样既不增加 scalar 调用开销，也避免 TLS fast-add 规则在源码中漂移。

### 7.4 循环不变量与 TLS 修改顺序

Struct 主体在第一次 local leaf call 前完成 fast-path state 和
`AllocBufferMS::barrierMarkBuffer_` 的解析。leaf thunk 只为每个 old-ref 重做
Region/slot、epoch 和 4-way lookup；不读取全局 mark plane，GCPhase、state 地址和
buffer 基址也不在循环中重复读取。

继续遵守当前 buffer contract：

```text
merge existing:
    先检查 threshold
    直接对 entry.mask 做幂等 OR，不检查 bit 是否已经 pending
    pendingBitCount++
    lastEntry = entry

insert new:
    先检查 threshold 和空 way
    先写 wordKey/mask
    再设置 occupancy bit
    pendingBitCount++
    lastEntry = entry
```

因此 LLVM 的 `pendingBitCount` 是保守的 fast-add budget，重复 bit 也会消耗一次
budget。达到阈值后的 wrapper 入口先 flush，再执行 Runtime 的精确 lookup，避免
饱和 buffer 因 Runtime 去重而长期不被提交。

TLS buffer 是 owner-private，以上操作不使用 LSE、LL/SC、`lock` 或 cmpxchg。

## 8. 任意 fast-add 失败后的完整重放

Struct inline 允许在失败前已经成功加入若干 TLS bit，但不允许已经覆盖目标内存：

```text
oldRef0 fast-add 成功
oldRef1 fast-add 成功
oldRef2 fast-add 失败
    -> 保留 oldRef0/oldRef1 对应的 TLS mask
    -> 不执行 memcpy
    -> 恢复原始 wrapper 参数
    -> 调用原 Struct/StaticStruct wrapper
```

Runtime wrapper 必须重新枚举整个范围，而不是只处理 oldRef2 之后的引用：

```text
wrapper 全范围枚举
  -> 已有 bit 在 TLS entry 中合并/去重
  -> threshold/4-way conflict 时由 Runtime flush 后重试当前 ref
  -> LARGE/OUTSIDE_SET/无 AllocBufferMS 走 Runtime canonical 路径
  -> 所有 old-ref capture 完成
  -> memcpy_s
```

不能在 LLVM 与 Runtime 之间增加 cursor、bitmap 或“剩余 ref 数量”参数。完整重放
保留现有 wrapper calling convention、statepoint 和 stackmap contract。

## 9. RawArray 为什么只有无引用分支 inline

RawArray 不能复用普通对象的：

```text
payloadBase = obj + 8
bitmap = objTypeInfo->gctib
bitmap >>= (dst - payloadBase) / 8
```

原因如下。

### 9.1 内容起点不同

普通对象的 GCTib 描述从 `obj + TYPEINFO_PTR_SIZE` 开始的固定 payload。

RawArray 的对象头还包含动态 `length`，数组内容起点是：

```text
obj + MArray::GetContentOffset()
    == obj + sizeof(MArray)
```

因此直接使用 `obj + 8` 会把数组 length/header 当作元素数据，ref slot 地址错误。

### 9.2 引用描述属于 component TypeInfo

RawArray 自身 TypeInfo 只描述“这是一个数组”和它的 component TypeInfo。
真正的元素引用布局由：

```text
arrayTypeInfo->componentTypeInfo
```

决定：

- component 是含引用 struct：`WriteStruct` 对每个相关元素重复应用 component
  GCTib；
- component 是 ref/object/array/interface：其他数组/ref barrier 会把每个元素
  作为一个 ref slot；
- component 是无引用 struct/primitive：不需要 old-edge capture。

对象自身的 GCTib 不能替代 component GCTib。

### 9.3 数组布局需要运行时长度和元素大小

普通 short GCTib 最多描述固定的 63 个 payload slot，只平移一次即可。

RawArray 的实际引用位置还依赖：

```text
array.length
componentTypeInfo
elementSize
contentOffset
dst 所在元素下标
覆盖范围跨越的元素数量
```

对于含引用 struct 数组，还需要：

```text
elementBase = contentBase + index * elementSize
对每个 elementBase 重复应用 component GCTib
```

这不是一次 bitmap 右移能够表达的。

### 9.4 `bit63 == 0` 不等于 RawArray

GCTib bit63 为 0 只表示当前值是 StdGCTib 指针，不表示对象一定是 RawArray：

- 大型普通 struct/object 也可能使用 StdGCTib；
- RawArray 是否含引用必须看 component TypeInfo；
- 无引用 RawArray 与含引用 RawArray 的 barrier 行为不同。

因此 LLVM 必须先检查 `TypeInfo::type == TYPE_KIND_RAWARRAY`，不能用 GCTib tag
推断数组类别。

### 9.5 当前 Runtime 已有 canonical 范围枚举

Runtime `BaseObject::ForEachRefInStruct` 遇到 RawArray 时会进入
`ForEachAggRefFieldInArray`：

```text
读取 length
  -> 读取 component TypeInfo
  -> 要求 component 为 struct
  -> 读取 elementSize
  -> 把 dst 映射到元素下标
  -> 按覆盖范围遍历元素
  -> 对每个元素应用 component GCTib
```

`MArray::ForEachRefFieldInRange` 是其他 struct-array/bulk 路径使用的范围枚举，
它另外区分 struct component 和 ref component。对于本设计涉及的
`WriteStructField`，canonical 路径是
`ForEachRefInStruct -> ForEachAggRefFieldInArray`，调用者应传入 struct 元素范围。

本次为了把机器码预算、参数保存和 partial fallback contract 控制在可审计范围内，
RawArray 只实现无引用 component 的 CopyOnly：

```text
RawArray
  -> componentTypeInfo == null：Slow
  -> component HasRefField：Slow
  -> component 无引用：CopyOnly
```

这个分流与 Wbuf AArch64/X86 当前实现一致。无引用 component 不存在需要保存的
old-edge，因此不需要读取 length、elementSize 或枚举 component GCTib。

含引用 component 仍回 Runtime，因为只有这一类才需要执行前述按元素定位、重复
component GCTib 和逐个 old-ref capture。

## 10. AArch64 发码约束

AArch64 ABI：

```text
WriteStruct:
    x0=obj, x1=dst, x2=dstSize, x3=src, x4=srcSize

WriteStaticStruct:
    x0=dst, x1=dstSize, x2=src, x3=srcSize, x4=gctib
```

要求：

1. 所有 slow edge 能恢复原始 `x0-x4`。
2. StateWord 使用 acquire load。
3. TypeInfo/GCTib、cursor、bitmap 和 fast-add scratch 使用 caller-saved GPR。
4. 跨 local thunk 调用的 cursor/bitmap 可暂存在 caller-saved SIMD 寄存器。
5. local thunk 放在主路径 Finish 跳转之后，正常流不能跌入 thunk。
6. 更新 `AArch64CangjieCallSize.cpp` 中 Struct/StaticStruct 的最大展开尺寸；
   该值必须覆盖循环、local thunk、slow call 和 memcpy。

## 11. X86-64 发码约束

### 11.1 SysV

参数：

```text
rdi, rsi, rdx, rcx, r8
```

原始参数可保存在 SysV caller-saved XMM 寄存器；cursor/bitmap 使用另外两个
caller-saved XMM 寄存器，leaf thunk 不得覆盖它们。

### 11.2 Win64/MinGW

参数：

```text
rcx, rdx, r8, r9, [rsp+32]
```

要求：

- 使用现有 32B home space 保存四个寄存器参数；
- 第五个参数继续从 `[rsp+32]` 读取；
- emitter 不调整 RSP；
- 不破坏 Win64 callee-saved XMM6-XMM15；
- COFF 继续通过 `__imp_CJ_MRT_MSGCFastPathState` 取得 fast-path state。

### 11.3 共同要求

X86 leaf thunk 只使用两个目标共同允许的 caller-saved寄存器集合及约定好的临时
XMM 寄存器，返回值使用 EAX：

```text
eax = 0：handled
eax = 1：slow
```

## 12. LLVM/Runtime 隐藏 ABI

`CJMSGCABI.h` 需要集中增加或确认以下常量：

```text
StateWord TypeInfo 有效位数       = 48
TYPEINFO_PTR_SIZE                 = 8
TypeInfo::type offset             = 8
TypeInfo::flag offset             = 9
TypeInfo::gctib offset            = 16
TypeInfo::componentTypeInfo offset= 64
TYPE_KIND_RAWARRAY                = 130
short GCTib tag bit               = 63
short GCTib valid mask            = 0x7fffffffffffffff
```

Runtime 已有对应 `static_assert` 的字段继续作为真实布局约束。LLVM 不新增第二份
GCPhase 状态、buffer epoch 或 barrier mode signature。

## 13. 预计修改文件

### 13.1 LLVM

- `llvm/include/llvm/CodeGen/CJMSGCABI.h`
  - 增加 StateWord、TypeInfo 和 GCTib 布局常量。
- `llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp`
  - 抽取可复用 TLS fast-add emitter body；
  - 增加动态 TypeInfo/short-GCTib 范围循环；
  - 增加 RawArray component 无引用时的 CopyOnly 分支；
  - 增加 StaticStruct short-GCTib 循环；
  - 增加函数内 fast-add local block 和统一 slow edge；Struct 循环通过直接分支
    进入/退出该 block，不产生内部 `BL/RET`。
- `llvm/lib/Target/AArch64/AArch64CangjieCallSize.cpp`
  - 更新 Struct/StaticStruct 最大展开尺寸。
- `llvm/lib/Target/X86/X86MSGCEmitter.cpp`
  - 增加 SysV/Win64 参数保存；
  - 增加动态/静态 short-GCTib 循环；
  - 增加 RawArray component 无引用时的 CopyOnly 分支；
  - 增加函数内 TLS fast-add local block，Struct 循环不产生内部
    `CALL/RET`。

### 13.2 Runtime

以下路径继续作为 slow fallback，同时做了不改变 buffer ABI 的热路径收敛：

- `src/Heap/MBarrier/MSMarkBarrier.cpp`
  - `BarrierMarkContext` 构造时只读取一次 TLS `encodedGCPhase`；第一个通过 heap
    range 的 old-ref 才解析 collector/owner buffer，且只有 buffer 存在时读取 epoch；
  - bulk 循环改用 `AddOldRefActive()`，不为每个引用重复 phase gate；
  - 相邻 old-ref 去重由栈上 `BulkOldRefVisitor` 承担，scalar context 不保存
    `lastOldRef`；
  - 普通对象缓存一次 TypeInfo，并通过模板化
    `ForEachBitmapWordInRangeFast()` 直接按 GCTib 枚举，避免
    `RefFieldVisitor` 类型擦除；RawArray 复用已解析 component/GCTib 并通过
    `ForEachBitmapWordFast()` 保留 canonical 枚举；RawArray 在读取
    `componentTypeInfo` 前不调用数组 `HasRefField()`；
  - 所有 copy 都先校验 `srcSize <= dstSize`，并只扫描实际覆盖的 `srcSize`。
- `src/Heap/MBarrier/MSBarrierMarkBuffer.h`
  - lookup 同时返回 existing/empty entry；有效 `lastEntry` 按 owner-only
    生命周期直接比较 key，MRU miss 后才读取 occupancy 并扫描 4-way；
  - Runtime 单 old-ref 使用 one-bit insert helper，复用调用方已完成的 threshold
    和 exact-pending 检查，不再重复 popcount/newBits。
- `src/Heap/MCollector/MarkSweepCollector.cpp`
  - Runtime add 不再读取全局 mark plane；共享 bitmap 过滤统一延迟到 flush；
  - buffer insert 复用同一次 4-way lookup，只有 flush 清空后才重新 lookup；
  - flush 保留全局 mark-word relaxed 过滤和 `atomic_fetch_or` winner 过滤。
- `src/Common/BaseObject.cpp`、`src/ObjectModel/MArray.cpp`
  - 继续提供 RawArray canonical 范围枚举，不复制到 LLVM。

LLVM 隐藏 ABI 常量仍需与 Runtime 已有 `static_assert` 保持一致，不新增重复运行时字段。

## 14. 测试与验收

### 14.1 CodeGen

AArch64、X86 ELF、X86 MinGW COFF 至少覆盖：

1. 非 Mark GCPhase 直接 memcpy。
2. Mark + dynamic TypeInfo + short GCTib 进入 set-bit 循环和 fast-add local
   block，内部没有 `CALL/RET`。
3. Mark + StaticStruct short GCTib 进入 set-bit 循环和 fast-add local block。
4. short GCTib bitmap 为 0 直接 memcpy。
5. StdGCTib/long-form 回 wrapper。
6. RawArray + 无引用 component 直接 memcpy。
7. RawArray + 含引用 component 回 wrapper。
8. RawArray + null component TypeInfo 回 wrapper。
9. null TypeInfo 回 wrapper。
10. buffer null、Region `collectEpoch` 与 `currentMarkEpoch` 不一致、非 Small、slot
    非法时回 wrapper。
11. pending threshold 和 4-way conflict 回 wrapper。
12. 任意 partial failure 前没有 memcpy。
13. `enable-msgc-inline-write-barrier=false` 保持原 wrapper call。
14. slow edge 保留原始 ABI 参数。

### 14.2 Runtime 语义

验证用例需要覆盖：

- 普通 short-GCTib struct 中多个引用；
- dst 从 struct 中间开始的范围写；
- 同一 mark word 的重复 old-ref；
- 多个 buffer set；
- 中途触发 threshold/4-way conflict 后完整重放；
- StaticStruct；
- StdGCTib 大 struct；
- RawArray<含引用 struct>；
- RawArray<无引用 struct>；
- Mark 期间高频 struct copy 后最终可达性正确。

## 15. 构建安装规则

本设计主要修改 LLVM；若只修改 LLVM，或 LLVM 与 Runtime 同时修改，均固定使用：

```text
/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_region_mark/runtime/build/scripts/incremental_rebuild_msgc_sdk.py
```

不得自行拼装 cmake、ninja、make 或手工复制产物代替该脚本。验证必须基于脚本构建并
安装到 `/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler_region_mark/output/msgc`
的 SDK。

## 16. 实施状态

截至本轮实现完成时：

- Runtime 持久 TLS barrier buffer 和 full replay slow path：已存在；
- TLS fast-add emitter body：Ref/StaticRef 保持 direct branch，Struct/StaticStruct
  循环通过同一 callsite-local block 复用，进入和返回均为直接分支；
- Struct/StaticStruct 非 Mark memcpy：已存在；
- AArch64/X86-64 Struct/StaticStruct Mark short-GCTib target inline：已实施；
- RawArray 无引用 component CopyOnly：已实施；
- RawArray 含引用 component 枚举：不在本次范围，继续回 Runtime；
- Runtime bulk：phase/collector 只判断一次，普通对象 TypeInfo/GCTib 只解析一次，
  RawArray component/GCTib 也只解析一次，GCTib 范围扫描使用模板化 Fast iterator，
  scan/TSAN 范围统一为实际覆盖长度；
- Runtime buffer：add-time 不读取共享 mark plane，insert 复用 lookup，MRU miss
  后才读取 occupancy 且不再重复比较同一 entry；Runtime one-bit insert 不重复
  popcount/threshold，LLVM fast-add local block同样不读取全局 mark plane；
- TypeInfo/StateWord/short-GCTib 常量：已集中到 `CJMSGCABI.h`；
- AArch64 callsite size 上界：Struct/StaticStruct 已调整到 1536 bytes；
- LLVM 回归覆盖：包含 AArch64、X86-64 SysV 和 X86-64 Win64 ABI。

Runtime wrapper 仍承担 full replay、RawArray/StdGCTib 枚举和 flush 后重试；这些优化没有
增加 continuation ABI，也没有改变 TLS buffer 布局。

### 16.1 本轮热路径收敛

- 动态 Struct 的 `TypeInfo::type` 与 `TypeInfo::flag` 合并为一次 16-bit load；
  非 Raw 且 `HAS_REF == 0` 时直接进入已校验 copy。
- short-GCTib bitmap 先按写覆盖的完整 slot 数裁剪；bitmap 为 0 时不读取
  TLS `AllocBufferMS`。
- AArch64 使用 `rbit + clz`，X86 使用 `bsf` 枚举 set bit，不再逐个右移跳过
  zero bit，也不再为每个 bit 重复范围检查。
- Struct scan 期间缓存 `regionInfoArray` 与 `currentMarkEpoch`；堆保留区上下界
  仍在 fast-add 内读取，以避免增加 ABI 参数保存成本。
- null 或非托管堆 old-ref 与 Runtime 语义一致，视为已经处理，不再整段回退。
- Mark 成功后跳到已校验 copy label，不重复执行 `srcSize/dstSize/null` 检查。
- X86 existing/new-key fast-add 在 mask/occupancy 发布后直接对
  `pendingBitCount` 做一次 owner-private memory increment，不再二次 load/add/store。
- MSGC Runtime atomic read 直接执行 raw atomic load；atomic write/swap/CAS 在
  old/new 相同、不删除边时跳过 shade，但仍执行原 CAS/atomic 操作。
