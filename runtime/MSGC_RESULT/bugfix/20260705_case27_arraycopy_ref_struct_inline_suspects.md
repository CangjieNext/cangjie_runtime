# case27 `cj_array_copy_ref` / `cj_array_copy_struct` inline 可疑点记录

## 1. 本轮现象收敛

基于你这轮手工注释实验，当前现象先记录为：

- `cj_array_copy_ref` 存在时，会报错
- `cj_array_copy_struct` 存在时，也会报错
- 两者都不存在时，不报错

这说明当前问题不应再只盯某一个 wrapper 名字，而应先把范围收敛为：

- **`cj_array_copy_ref` 与 `cj_array_copy_struct` 这两条 bulk copy 在 AArch64 MSGC 下的 inline fastpath 机制本身**

结合此前实验：

- 把 `CJ_MCC_ArrayCopyRefFast_MSGC` / `CJ_MCC_ArrayCopyStructFast_MSGC` 从 `tryEmitCangjieSpecificCallByMOSym()` 的 fastpath 分发里移除后，case27 的进程级崩溃曾明显收敛
- 现在进一步看到：**只要 `cj_array_copy_ref` 或 `cj_array_copy_struct` 任意一条还在，仍可能出错**

因此当前更合理的口径是：

> 问题中心已经从“单个写屏障 fastpath 名字”进一步收敛到“array-copy bulk inline 这一类路径”。

## 2. 当前代码链路

### 2.1 runtime 慢路径入口

`src/CompilerCalls.cpp`

- `CJ_MCC_ArrayCopyRef(...)`
  - 最终调用 `Heap::GetBarrier().CopyRefArray(...)`
- `CJ_MCC_ArrayCopyStruct(...)`
  - 最终调用 `Heap::GetBarrier().CopyStructArray(...)`

MSGC mark 期慢路径语义在 `src/Heap/MBarrier/MSMarkBarrier.cpp`：

- `CopyRefArray`
  - 先 `EnqueueRefArrayRange(dstField, dstSize)`
  - 再 `memmove_s(dstField, dstSize, srcField, srcSize)`
- `CopyStructArray`
  - 先 `EnqueueStructArrayRefsInRange(dstObj, dstField, copiedLen)`
  - 再 `memmove_s(dstField, dstSize, srcField, srcSize)`

也就是说，**慢路径的共同语义都是：先记录被覆盖的 old refs，再做 bulk memmove**。

### 2.2 LLVM lowering 到 MSGC wrapper

`llvm/lib/CodeGen/CJBarrierLowering.cpp`

- `shouldLowerMSGCWriteBarrier()` 当前已包含：
  - `cj_array_copy_ref`
  - `cj_array_copy_struct`
- `getOrCreateMSGCWriteBarrierWrapper()` 会为两者分别创建：
  - `CJ_MCC_ArrayCopyRefFast_MSGC`
  - `CJ_MCC_ArrayCopyStructFast_MSGC`

这些 wrapper 本身只是 thin slow body，内部仍然直接回调：

- `CJ_MCC_ArrayCopyRef`
- `CJ_MCC_ArrayCopyStruct`

### 2.3 AsmPrinter 分发到 AArch64 inline

`llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp`

当前 `tryEmitCangjieSpecificCallByMOSym()` 里，这两个 wrapper 仍会进入：

- `emitMccWriteBarrierFastPath(...)`

也就是说，只要前面 lowering 到了这两个 `*_Fast_MSGC` wrapper，AArch64 侧就会尝试在 callsite 直接展开 inline fastpath，而不是保留普通 runtime 调用。

## 3. `cj_array_copy_ref` 当前 inline 是怎么实现的

实现位置：

- `llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp`
- `emitMccWriteBarrierFastPath()`
- `IsArrayCopyRef` 分支

可以概括成下面的伪代码：

```text
if (TLS.gcFastState.bit0 == 0) goto StoreOnly

refCount = copySize / 8
if (refCount == 0) goto StoreOnly

allocBuf = TLS.allocBuf
if (allocBuf == null) goto Slow

count = wbBuf.count
if (count + refCount > capacity) goto Slow

cursor = dstPtr
repeat refCount times:
    old = *cursor
    if (old != null):
        wbBuf[count++] = old
    cursor += 8

wbBuf.count = count
memmove(dstPtr, srcPtr, copySize)
goto Finish

StoreOnly:
    memmove(dstPtr, srcPtr, copySize)

Slow:
    bl CJ_MCC_ArrayCopyRefFast_MSGC
```

这个分支的几个关键特点：

1. **完全不看 `dstObj`**
   - 它直接信任 `dstPtr + size`
   - 不做 `dstObj` heap-range check
   - 不做 `dstPtr` 是否位于 `dstObj` payload 内的校验

2. **直接按 `size / 8` 把目标区旧值当成 ref 扫描**
   - 默认假设这是一个按 8 字节槽排列的 ref array 区域

3. **直接手写 wbBuf push**
   - 不走 runtime 的 `EnqueueRefFast()` helper
   - 直接依赖 `AllocBufferMS::wbBuf_` 的固定内存布局

4. **最后直接调 plain `memmove`**
   - 不再走 runtime 慢路径里的 `memmove_s + CHECK_DETAIL`

## 4. `cj_array_copy_struct` 当前 inline 是怎么实现的

实现位置同样在：

- `llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp`
- `emitMccWriteBarrierFastPath()`
- `IsArrayCopyStruct` 分支

伪代码可概括为：

```text
if (TLS.gcFastState.bit0 == 0) goto StoreOnly

if (!Heap::IsHeapAddress(dstObj)) goto StoreOnly

copyLen = min(dstSize, srcSize)
if (copyLen == 0) goto StoreOnly

要求:
  dstPtr 8 对齐
  copyLen 8 对齐
  componentSize 非 0 且 8 对齐
否则 goto Slow

从 dstObj -> TypeInfo -> componentTypeInfo -> GCTib
读取 component GCTib

如果 componentTypeInfo 为空 -> Slow
如果 component GCTib 是 long-form(bit63==0) -> Slow
如果 component GCTib 表示无 ref -> StoreOnly

elemCount = copyLen / componentSize
检查 wbBuf 容量

按 element 循环：
    对每个 element 按 component short GCTib bitmap 扫 old refs
    old != null 时写入 wbBuf

wbBuf.count = count
memmove(dstPtr, srcPtr, copySize)
goto Finish

StoreOnly:
    memmove(dstPtr, srcPtr, copySize)

Slow:
    bl CJ_MCC_ArrayCopyStructFast_MSGC
```

这个分支的关键特点：

1. 它**会**看 `dstObj`
   - 先做 heap-range check
   - 再从 `dstObj` 的 `TypeInfo` 追到 `componentTypeInfo`

2. 它不是调用 runtime 的 `MArray::ForEachRefFieldInRange()`
   - 而是自己在汇编里手搓：
     - `componentSize`
     - `component GCTib`
     - `outer element loop`
     - `inner bitmap loop`

3. 它只接受比较窄的 happy path
   - `dstPtr` / `copyLen` / `componentSize` 要满足 8 对齐
   - `component GCTib` 要是 short-form(bit63==1)
   - 否则直接 slow-fall

## 5. 它们和其他写屏障 fastpath 的关键差异

和 `WriteRefFieldFast_MSGC` / `WriteStructFieldFast_MSGC` / `WriteStatic*Fast_MSGC` 相比，这两个 array-copy 分支有几个明显不同：

1. **它们是 bulk 操作，不是单槽写**
   - 普通 `WriteRefFieldFast_MSGC` 只处理一个 old slot
   - `ArrayCopy*` 会先扫一整段旧目标区，再一次性 `memmove`

2. **它们是唯一一类“先批量 push old refs，再做 bulk memmove”的 inline 路径**
   - 这会比普通单字段写更猛烈地冲击：
     - wbBuf 容量
     - 旧值枚举正确性
     - `dstPtr/size` 合约正确性

3. **`ArrayCopyRef` 比普通 ref 写更激进**
   - 普通 `WriteRefFieldFast_MSGC` 至少还会看 `dstObj` 是否在 heap
   - `ArrayCopyRef` 直接忽略 `dstObj`
   - 它只信任“`dstPtr` 指向的是一段纯 ref array payload”

4. **`ArrayCopyStruct` 比普通 struct 写更依赖对象模型细节**
   - 普通 struct 写 inline 主要围绕当前对象/字段范围枚举
   - `ArrayCopyStruct` 需要额外做：
     - array object → component type
     - component type → component gctib
     - component size 驱动的 element/bitmap 双层循环

5. **两者都绕过了 runtime helper 的封装层**
   - slow path 是 C++ helper 组合
   - inline path 是汇编侧手搓 wbBuf push + memmove

## 6. 当前哪些原因已经不太像“共因”

如果按“`cj_array_copy_ref` 单独存在也会报错，`cj_array_copy_struct` 单独存在也会报错”这个实验结论来收敛，那么下面几条就不太像两者共享的根因：

1. **`heapCurrentEnd` 发布 / acquire 读取问题**
   - `ArrayCopyStruct` 分支会读 heap 边界
   - **`ArrayCopyRef` 分支根本不读 heapStart/heapCurrentEnd**
   - 所以它不能解释两者的共同问题

2. **component GCTib / componentTypeInfo 解析问题**
   - 只影响 `ArrayCopyStruct`
   - 解释不了 `ArrayCopyRef`

3. **struct array 长/短 GCTib 形态差异**
   - 同样只影响 `ArrayCopyStruct`
   - 解释不了 `ArrayCopyRef`

所以从“共因”角度看，当前更该怀疑的是**两条 bulk inline 共享的那部分机制**，而不是 struct 专有那部分。

## 7. 当前最可疑的方向

### 7.1 最高优先级：bulk inline 共享的“旧值批量扫描 + 直写 wbBuf + memmove”实现

这是 `ArrayCopyRef` 和 `ArrayCopyStruct` 真正共享的核心。

共同点是：

- 先按目标区旧值做批量 old-ref 枚举
- 直接手写 `wbBuf[count] = oldRef`
- 最后 `wbBuf.count = count`
- 然后直接 `memmove`

这条共享链路一旦有问题，会同时打到：

- `cj_array_copy_ref`
- `cj_array_copy_struct`

而不会要求它们必须共享同一个 `dstObj`/`componentTi` 解析逻辑。

### 7.2 `ArrayCopyRef` 的专属高危点：它完全信任 `dstPtr + size`

这是当前 `ArrayCopyRef` 最扎眼的地方。

如果某个 callsite 虽然叫 `cj_array_copy_ref`，但：

- `dstPtr` 不是严格的 heap ref-array payload 起点
- `size` 不是严格的 ref-slot 字节数
- `dstPtr` 覆盖区里存在半初始化/非对象头/脏值

那它就会把那段旧内存按 ref 槽直接扫进 wbBuf。

### 7.3 `ArrayCopyStruct` 的专属高危点：汇编里手搓 component bitmap 遍历

它没有复用 runtime 的 `MArray::ForEachRefFieldInRange()`，而是自己在 AArch64 发码侧重建了一套逻辑。

这意味着只要下面任何一点和 runtime 真实对象模型不完全一致，就可能只在 inline 下出错：

- `dstPtr` 是否总是 element 边界
- `copyLen` 是否总是完整 element 个数
- component `GCTib` 的 short/long form
- component size / bitmap 位图解释

## 8. 当前阶段结论

当前可以先把结论写成：

1. 依据本轮实验，`case27` 的问题已经进一步收敛到：
   - **`cj_array_copy_ref` / `cj_array_copy_struct` 在 AArch64 MSGC 下的 inline fastpath**
2. 两者当前都通过：
   - `CJBarrierLowering.cpp` → `*_Fast_MSGC wrapper`
   - `AsmPrinter::tryEmitCangjieSpecificCallByMOSym()` → `emitMccWriteBarrierFastPath()`
   - `AArch64AsmPrinter.cpp` 直接展开 inline
3. `ArrayCopyRef` 与 `ArrayCopyStruct` 虽然细节不同，但它们共享同一类高风险机制：
   - **bulk old-range scan**
   - **raw wbBuf push**
   - **随后 bulk memmove**
4. 如果“任一条 array-copy intrinsic 单独存在都还能报错”这一现象稳定成立，那么当前最该优先怀疑的不是 `heapCurrentEnd`、也不是 `component GCTib` 这类 struct 专属问题，而是：
   - **array-copy inline 共享实现本身**
   - 以及 callsite 是否真的满足这些 inline 假设

## 9. 下一步更有判断力的验证建议

1. 对 `cj_array_copy_ref`
   - 抓实际 callsite 的 `dstObj / dstPtr / size`
   - 确认它是否永远都是 heap ref-array payload，且 `size` 恒为 8 的整数倍

2. 对 `cj_array_copy_struct`
   - 抓实际 callsite 的 `componentTypeInfo / componentSize / componentGCTib`
   - 确认是否存在“inline happy path 以外但仍被错误走进 inline”的样本

3. 如果要优先找“共享根因”
   - 最值得先验证的是：**把两条 array-copy 分支内部统一强制 slow-fall，只保留 wrapper，不改其它写屏障**
   - 如果这样稳定，则说明问题确实在 array-copy inline 自身，而不是 runtime 慢路径语义
