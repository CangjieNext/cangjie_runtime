# c34e3163 修复 http 用例 SIGSEGV 的问题说明

**日期**: 2026-05-27  
**提交**: c34e31632183b0db284c602eda8d64a55f1fe733  
**标题**: 修复http用例的sigv问题

## 1. 这个提交修了什么问题

这个提交的核心目标是修复 MSGC 下 http 用例运行时出现的 SIGSEGV。

从代码改动看，根因不是普通业务逻辑错误，而是 runtime 侧对 TypeInfo 指针的消费不够稳健：

1. 在 MSGC 模式下，某些路径上传入 runtime 的 TypeInfo 指针可能带有高 16 bit 的附加状态位。
2. 如果这些高位不先清掉，后续在分配、数组创建、反射、泛型、类型判断等路径里直接把它当成真实 TypeInfo 指针解引用，就可能读到非法地址并触发 SIGSEGV。
3. http 用例命中了这类 runtime 入口，因此这个提交选择在 runtime 入口统一做 TypeInfo 归一化，而不是只修某一个局部调用点。

这次提交新增了 `NormalizeRuntimeTypeInfo()`，并把它铺到了大量 `CompilerCalls.cpp` 的 runtime 入口上，包括：

1. `MCC_NewObject`
2. `MCC_NewWeakRefObject`
3. `MCC_NewPinnedObject`
4. `MCC_NewFinalizer`
5. `MCC_NewArray` / `MCC_NewArray8/16/32/64`
6. `MCC_GetTypeForAny`
7. `CJ_MCC_IsSubType`
8. 一批泛型、反射、数组与类型查询相关入口

归一化的核心逻辑是：在 MSGC 下把 TypeInfo 指针做一次高位掩码，仅保留低 48 bit 的实际地址。

## 2. 这个提交还改了什么

除了 TypeInfo 归一化，这个提交还顺手清理了前一阶段为了排查问题加上的一批诊断逻辑，主要包括：

1. `MSMarkBarrier.cpp` 里用于 barrier invalid-ref 诊断的计数器和日志
2. `MarkSweepCollector.cpp` 里用于 WBB overflow / global queue drop 检查的诊断逻辑
3. `MSAllocBuffer.cpp`、`AllocBuffer.cpp`、`MSRegionManager.cpp`、`RegionManager.cpp` 里的 alloc/lifetime 统计打印
4. `CangjieRuntime.cpp` 中少量相关辅助代码

这些改动本身更像是“清理调试代码”，不是这次修 SIGSEGV 的主功能点。

## 3. 为什么我认为真正修崩溃的是 CompilerCalls.cpp 里的 TypeInfo 归一化

原因有三点：

1. 这次提交标题明确指向运行期 SIGSEGV，而 `CompilerCalls.cpp` 正好是几乎所有对象创建、数组创建、类型判断、反射入口的汇聚点。
2. 新增的 `NormalizeRuntimeTypeInfo()` 直接针对“高位污染的 TypeInfo 指针”做防御，这和 MSGC 下 state/typeinfo 混用导致崩溃的症状完全吻合。
3. 其它 allocator / collector 文件的改动多数是在删除诊断和统计，不像是会直接修掉 http 用例的非法指针崩溃。

## 4. 这个提交的性能副作用

虽然这个提交修掉了 http 用例的 SIGSEGV，但它对 case1 baseline 的性能有明显副作用。

对比结果：

1. 8132a840 版本：MSGC Run 2-3 均值相对 CopyGC 为 **-2.5%**
2. c34e3163 版本：MSGC Run 2-3 均值相对 CopyGC 为 **+19.7%**

退化最明显的子项：

1. `F5_HashMap`: 从 91ms 恶化到 130ms
2. `P2_LargeAlloc`: 从 388ms 恶化到 584ms
3. `F3_LargeObject`: 从 704ms 恶化到 976ms

因此，这个提交的结论应当分成两部分看：

1. **正确性上**：它很可能确实修掉了 http 用例里的 MSGC SIGSEGV。
2. **性能上**：它也是 8132a840 到当前慢版本之间，最明显把 case1 拉坏的分水岭提交。

## 5. 当前建议

如果后续要继续拆解这个提交，最值得优先验证的是：

1. 只保留 `CompilerCalls.cpp` 里的 TypeInfo 归一化修复
2. 暂时不带入这次提交中其它清理性改动
3. 重新跑 case1，看性能是否仍然回退

如果只保留 TypeInfo 归一化就已经回退，那么主因就在 runtime 入口全面加掩码这件事本身；如果不回退，再继续检查这个提交中其它一起带入的语义变化。
