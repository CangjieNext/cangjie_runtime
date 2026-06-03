# Step 7：泛型运行时辅助开销分析与优化方向

> 对应分析产物：[sample_seed5_msgc.txt](../Result/case1_baseline/analysis/20260422_seed5_msgc_profile/sample_seed5_msgc.txt)
>
> 2026-04-22 基于 case1_baseline seed5 profile、`TypeInfoManager`、`CompilerCalls`、`Barrier` 代码路径整理

---

## 目录

1. [问题背景](#1-问题背景)
2. [核心结论：seed5 下稳定热簇已转向泛型运行时辅助路径](#2-核心结论seed5-下稳定热簇已转向泛型运行时辅助路径)
3. [直接证据](#3-直接证据)
4. [根因拆解](#4-根因拆解)
5. [优化方向](#5-优化方向)
6. [优化优先级](#6-优化优先级)
7. [结论](#7-结论)

---

## 1. 问题背景

在 Step 6 中已经确认：seed5 场景下，MSGC 的吞吐下降不再只来自 allocation refill / lazy sweep，而是出现了两簇更稳定的 runtime helper 热点：

- `TypeInfoManager::GetOrCreateTypeInfo`
- `Barrier::WriteGeneric` / `WriteStruct`

这两簇热点都反复出现在 Array / HashMap / Iterator 相关路径中，说明它们不是 collector 主循环本身的慢点，而更像是 **泛型容器运行时支持逻辑** 在高频调用下被整体放大。

这部分问题与 allocator 主链不同：

- allocator 主链的痛点是 miss 后做了过多同步 refill / sweep
- 泛型问题的痛点则是“每次容器操作都多做一点 typeinfo 查询、generic copy、barrier copy”

因此它值得单独作为 Step 7 处理，而不继续并入 Step 6。

---

## 2. 核心结论：seed5 下稳定热簇已转向泛型运行时辅助路径

当前 seed5 的泛型热点可以归纳为两条主线：

```text
泛型 TypeInfo 查询：
  CJ_MCC_GetOrCreateTypeInfo
    -> TypeInfoManager::GetOrCreateTypeInfo
      -> GetTypeInfo
        -> GenericTiDesc::computeHash
        -> GenericTiDescHashMap::GetGenericTiDesc
        -> GenericTiDesc::operator==

泛型对象/payload 拷贝：
  CJ_MCC_WriteGeneric / CJ_MCC_AssignGeneric / CJ_MCC_WriteGenericPayload
    -> Barrier::WriteGeneric
      -> memcpy_s / WriteStruct
    -> Barrier::WriteStruct
      -> memcpy_s
```

对应的关键判断是：

1. **`TypeInfoManager` 的热点主要不是“创建”新 `TypeInfo`，而是“反复查找”已经存在的泛型实例。**
2. **`Barrier` 的热点主要不是 GC phase 的屏障算法，而是 generic payload copy 被包装成了 `WriteGeneric` / `WriteStruct`。**
3. **热点集中出现在 `HashMap`、`Array`、`ArrayIterator` 等泛型容器路径上，属于语言运行时对泛型容器的辅助开销。**

也就是说，seed5 放大的不是“collector 变慢”，而是“泛型容器每次操作附带的 runtime helper 太多”。

---

## 3. 直接证据

### 3.1 `TypeInfoManager` 热点主要落在查表而不是创建

在 [sample_seed5_msgc.txt](../Result/case1_baseline/analysis/20260422_seed5_msgc_profile/sample_seed5_msgc.txt#L194-L224) 中，可以看到：

- `TypeInfoManager::GetOrCreateTypeInfo` 反复命中
- 其下方最宽的子调用不是 `CreatedTypeInfoImpl`
- 而是 `GenericTiDescHashMap::GetGenericTiDesc`
- 以及 `GenericTiDesc::computeHash` / `operator==`

更细的容器调用点也很清楚，例如 [sample_seed5_msgc.txt](../Result/case1_baseline/analysis/20260422_seed5_msgc_profile/sample_seed5_msgc.txt#L8782-L8902) 中：

- `_CNac7HashMapIG_G0_E6resizeHl`
- `_CNac7HashMapIG_G0_E3addHG_G0_`
- `_CNac7HashMapIG_G0_E3getHG_`

这些 benchmark 内部的 HashMap 操作会反复调用 `GetOrCreateTypeInfo`。

源码也印证这一点：

- [TypeInfoManager.cpp](../../src/TypeInfoManager.cpp#L367-L385) 的 `GetOrCreateTypeInfo()` 入口总是先做 `GetTypeInfo()`
- [TypeInfoManager.cpp](../../src/TypeInfoManager.cpp#L316-L333) 的 `GetGenericTiDesc()` 先按 hash 找 bucket，再对 bucket 内的 `GenericTiDesc` 逐个比较
- [TypeInfoManager.cpp](../../src/TypeInfoManager.cpp#L81-L102) 的 `computeHash()` 会遍历所有 type args
- [TypeInfoManager.cpp](../../src/TypeInfoManager.cpp#L104-L147) 的 `operator==()` 也会逐个比较 type args UUID

因此，这簇热点的本质是：

**同一批泛型实例被高频重复查，而当前查找路径每次都要重新做 hash 与 bucket 内 compare。**

### 3.2 `Barrier` 热点主要落在 generic copy

在 [sample_seed5_msgc.txt](../Result/case1_baseline/analysis/20260422_seed5_msgc_profile/sample_seed5_msgc.txt#L226-L255) 和 [sample_seed5_msgc.txt](../Result/case1_baseline/analysis/20260422_seed5_msgc_profile/sample_seed5_msgc.txt#L460-L520) 中，可以看到：

- `Barrier::WriteGeneric`
- `Barrier::WriteStruct`
- `memcpy_s`

这里真正占宽度的是 `memcpy_s` / `_platform_memmove`，而不是 phase-specific mark 逻辑。

对应源码：

- [Barrier.cpp](../../src/Heap/Barrier/Barrier.cpp#L36-L45) 的 `WriteStruct()` 本质就是一次 `memcpy_s`
- [Barrier.cpp](../../src/Heap/Barrier/Barrier.cpp#L209-L239) 的 `WriteGeneric()` 主要按 heap / non-heap 组合分支，最后仍然落到 `memcpy_s` 或 `WriteStruct()`
- [CompilerCalls.cpp](../../src/CompilerCalls.cpp#L1643-L1684) 的 `CJ_MCC_WriteGeneric` / `CJ_MCC_AssignGeneric` / `CJ_MCC_WriteGenericPayload` 都会进入这条 generic copy 路径

只有在少量样本里，才会进一步落到 `MSMarkBarrier::WriteStruct()` 做旧值 / 新值 enqueue，例如 [sample_seed5_msgc.txt](../Result/case1_baseline/analysis/20260422_seed5_msgc_profile/sample_seed5_msgc.txt#L8773-L8795)。

这说明：

**seed5 下 `Barrier` 热点的主体其实是泛型对象/结构体复制，而不是 Mark barrier 算法本身。**

### 3.3 热点集中在泛型容器路径，而非 collector 主路径

从 sample 中反复出现的 benchmark 符号看，最集中的调用者是：

- `_CNat5ArrayIG_E6<init>HlF0G_lE`
- `_CNat13ArrayIteratorIG_E4nextHv`
- `_CNac7HashMapIG_G0_E6resizeHl`
- `_CNac7HashMapIG_G0_E3addHG_G0_`
- `_CNac7HashMapIG_G0_E3getHG_`

这说明热点与“泛型集合/容器 API 的高频操作”直接绑定，而不是与 GC 周期切换绑定。

所以 Step 7 要处理的对象不是 collector，而是：

1. 泛型实例查询
2. 泛型对象复制
3. 容器热路径对这两类 helper 的调用频率

---

## 4. 根因拆解

### 4.1 根因 A：泛型 TypeInfo 查询缺少热路径缓存

当前 `TypeInfoManager::GetOrCreateTypeInfo()` 每次都从描述符路径重新走一遍：

```text
GetOrCreateTypeInfo
  -> GetTypeInfo
    -> GenericTiDesc(desc)
      -> computeHash
    -> genericTypeInfoDescMap.GetGenericTiDesc(desc)
      -> bucket lookup
      -> vector walk
      -> operator==
```

这套逻辑对“第一次创建”是合理的，但对 seed5 中已经存在、且被重复使用的泛型实例来说，就显得过重。

特别是 HashMap / Array 这类操作在循环里不断触发同类泛型查询时，当前实现没有“最近命中”或“小范围 hot cache”，于是每次都要重新：

- 计算 hash
- 进 bucket
- 做 `operator==`

这就是 seed5 下 `TypeInfoManager` 热点稳定存在的第一原因。

### 4.2 根因 B：`GenericTiDesc` 的 compare 成本与泛型参数个数线性相关

无论是 [computeHash](../../src/TypeInfoManager.cpp#L81-L102) 还是 [operator==](../../src/TypeInfoManager.cpp#L104-L147)，都会对 `args` 做逐个遍历。

这意味着：

- 泛型参数越多，单次查找越贵
- 即使参数个数不多，只要调用频率足够高，累计成本也会显著

当前实现对 correctness 很稳，但对 hot-path locality 不够友好。

### 4.3 根因 C：generic copy 路径把普通数据复制包装成 barrier API

`CJ_MCC_WriteGeneric`、`CJ_MCC_AssignGeneric`、`CJ_MCC_WriteGenericPayload` 在 runtime 层统一落到 [CompilerCalls.cpp](../../src/CompilerCalls.cpp#L1643-L1684)，再进入 [Barrier.cpp](../../src/Heap/Barrier/Barrier.cpp#L209-L239)。

这条路径的优点是语义统一，但代价是：

- 即便最终只是一次普通 `memcpy_s`
- 仍然先经历 generic API 路由、heap/non-heap 分支、部分 typeinfo 判断

在容器路径里，这种“本来是 copy，最后经 barrier API 表达”的设计会带来固定辅助开销。

### 4.4 根因 D：HashMap / Array 热路径正在重复触发同一类 runtime helper

从 sample 看，HashMap 的 `add/get/resize` 不仅触发：

- `GetOrCreateTypeInfo`

还会触发：

- `WriteGeneric`
- `WriteStruct`
- `ReadGeneric`

也就是说，单次容器操作不是只多一个 helper，而是同时多了几类 helper：

1. 类型查询
2. 泛型 payload copy
3. 结构体/引用读写封装

在 seed5 这种高频压力下，这些 helper 会一起稳定上墙。

### 4.5 根因 E：seed5 把这类固定小成本整体放大

seed5 不是直接把某个单点成本变成大 stall，而是让以下事情发生得更频繁：

- 分配
- 容器扩容/复制
- 泛型实例查询
- generic payload 写入

因此那些在 baseline 下“还没明显宽到需要单独处理”的固定辅助成本，在 seed5 下就稳定变成了主热点。

---

## 5. 优化方向

### 5.1 方向 A：给 `TypeInfoManager` 增加最近命中 fast path

这是当前最值得先试的一条线。

目标：

- 针对最近一次或最近少量命中的 `TypeTemplate + type args UUID` 组合
- 直接返回已有 `TypeInfo`
- 避免每次都重新做 `computeHash + bucket lookup + operator==`

最小实现可以从 very-hot last-hit cache 开始，例如：

- thread-local 一项缓存
- 或 `TypeInfoManager` 内的一项 lock-free 最近命中缓存

只要命中率足够高，就能显著削掉 seed5 下最稳定的 `TypeInfoManager` 热簇。

#### 2026-04-22 runtime-only 实现

本轮先只在 runtime 内完成了优先级 1 的第一版完整实现，落点在 [TypeInfoManager.cpp](../../src/TypeInfoManager.cpp)：

1. 把原先“单项最近命中”思路扩成了 **4 项 thread-local 小缓存**，用于覆盖 `Array` / `HashMap` 热路径里多种泛型实例交替命中的场景。
2. 对常见的 `argSize <= 2` 场景使用 **内联参数槽位** 保存 `TypeInfo*`，避免命中缓存前后还要做 `vector` 分配/拷贝。
3. 只有 `argSize > 2` 时才退回 `vector` 保存参数列表，把额外开销限制在较少见的大参数泛型实例上。
4. 命中后会刷新本地时间戳，未命中更新时优先替换最旧槽位，避免单项缓存下“来回抖动导致一直走慢路径”。

这版实现仍然是 **runtime-only 优化**：

- 它不会减少 `CJ_MCC_GetOrCreateTypeInfo` 的调用次数。
- 它优化的是“同样的调用已经发起了，runtime 内部怎样更快返回已有 `TypeInfo*`”。
- 因此它对应的是“优先级 1：查询命中更快”，而不是“优先级 2：上层少发起调用”。

#### 2026-04-22 runtime-only 验证结果

验证命令：

```bash
DISABLE_AUTO_UPDATE=true .venv/bin/python build.py build --gc-mode marksweep
DISABLE_AUTO_UPDATE=true bash MSGC_RESULT/Result/script/run_case.sh \
  --case case1_baseline --gc-mode marksweep --tag typeinfocache_tls4
```

构建结果：通过。

与无该优化的 `noprefetch` 基线相比：

| 指标 | noprefetch | typeinfocache_tls4 | 变化 |
| --- | ---: | ---: | ---: |
| Benchmark 总时间 (ms) | 6247 | 5722 | -525 ms |
| RSS 平均 (MB) | 80.58 | 70.89 | -9.69 MB |
| P1_SmallAlloc (ms) | 289 | 283 | -6 ms |
| P2_LargeAlloc (ms) | 1079 | 968 | -111 ms |
| P3_MixedWorkload (ms) | 661 | 599 | -62 ms |
| P4_LongLivedChurn (ms) | 520 | 510 | -10 ms |
| P5_Fragmentation (ms) | 895 | 865 | -30 ms |
| GC 总时间 (ms) | 71.79 | 87.29 | +15.50 ms |

当前可以确认的信号是：

1. **mutator 总时间有明显正向收益**，总 wall time 从 `6247 ms` 降到 `5722 ms`，约下降 `8.4%`。
2. 收益最集中的阶段是 `P2_LargeAlloc` 和 `P3_MixedWorkload`，说明高频容器/泛型辅助路径确实被削薄了。
3. GC 总时间没有同步下降，甚至略有上升，因此这次收益不应解释为“collector 变快”，而更像是 **mutator 侧 helper 查询成本下降**。
4. 这轮结果足以说明“优先级 1 值得保留并继续深挖”，但仍建议后续补一轮 seed5 flamegraph 复测，确认 `TypeInfoManager::GetOrCreateTypeInfo` 宽度是否按预期收缩。

#### 2026-04-22 第二层 runtime-only 实验：轻量 key 只特化 `argSize == 1 / 2`

在 4 项 TLS cache 之上，又继续尝试了第二层优化：

1. 让 [GenericTiDesc::computeHash](../../src/TypeInfoManager.cpp) 在构造阶段缓存 `TypeTemplate` UUID。
2. 给最常见的 `argSize == 1 / 2` 场景额外缓存前 1 到 2 个 type arg UUID，避免 bucket compare 时再做 `GetUUID` / `AddTypeInfo`。
3. 最初版本曾把“大于 2 个参数”的场景也一并缓存为扩展 key，但单次 case1 结果不如 `tls4`，因此已收窄为：**只特化 `argSize == 1 / 2`，大参数场景仍走原有逐项比较路径。**

两轮二层实验结果如下：

| 版本 | Benchmark 总时间 (ms) | RSS 平均 (MB) | GC 总时间 (ms) | 结论 |
| --- | ---: | ---: | ---: | --- |
| `typeinfocache_tls4` | 5722 | 70.89 | 87.29 | 第一层优化，稳定正收益 |
| `typeinfocache_uuidkey` | 5804 | 90.97 | 94.05 | 全量 UUID key 扩展版，较 `tls4` 回退 |
| `typeinfocache_uuidkey_narrow` | 5704 | 86.31 | 96.03 | 只特化 `argSize == 1/2` 后，wall time 略好于 `tls4` |

当前对第二层优化应保持克制判断：

1. `argSize == 1/2` 的轻量 key **有机会再挤出一点 mutator 时间**，本轮单次 case1 从 `5722 ms` 进一步到 `5704 ms`。
2. 但这部分收益只有 `18 ms`，明显小于第一层 `tls4` 相对 baseline 的收益量级。
3. 同时 RSS 平均和 GC 总时间都比 `tls4` 更差，说明它还不能被直接解释为“更优的整体版本”。
4. 因此更稳妥的表述是：**第二层优化目前只有边际正信号，还需要 seed5 flamegraph 或至少多轮 case1 平均值来确认是否值得保留。**

### 5.2 方向 B：降低 `GenericTiDesc` 的查找成本

如果 5.1 有收益，下一步可继续做：

1. 为常见 `argSize == 1 / 2` 的情况做更轻的 key 构造
2. 减少 bucket 内 `vector<GenericTiDesc*>` 线性扫描
3. 尽量让 `operator==` 更早失败，而不是完整扫描 args

这类改造属于“把当前查找逻辑做瘦”，但不改变语义。

### 5.3 方向 C：把重复 `GetOrCreateTypeInfo` 尽量从容器热路径里 hoist 出去

当前从 sample 看，容器代码会在循环里反复调用 `CJ_MCC_GetOrCreateTypeInfo`。

如果调用点可控，更值得做的是：

- 在上层先缓存 `TypeInfo*`
- 然后循环体里复用

这种优化不一定在 runtime 内部做，也可能更适合编译器或容器库实现侧完成。

但从收益形态看，它会比单纯微调 hash 更有潜力，因为它能直接减少 helper 调用次数。

#### 2026-04-22 建议实现路径：优先在编译器 pass 中做 loop hoist

这条线的核心不是“让 runtime 查得更快”，而是：

> **同一组 `TypeTemplate + type args` 如果在循环内反复构造并查询，应尽量在循环外先求一次 `TypeInfo*`，循环体里直接复用。**

结合当前代码，最合适的第一落点不是 runtime，而是编译器侧的 [CJGenericIntrinsicOpt.cpp](../../../llvm-project/llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp)。

原因有两个：

1. [CJRuntimeLowering.cpp](../../../llvm-project/llvm/lib/Transforms/Scalar/CJRuntimeLowering.cpp#L99) 已经把 `cj_get_type_info` lowering 成 `CJ_MCC_GetOrCreateTypeInfo`，因此这个 pass 看得到真实 runtime call。
2. 当前 [sameTIOpt](../../../llvm-project/llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp#L318) 只做“支配关系下的重复调用合并”，还没有利用 loop preheader 做真正的 loop-invariant hoist。

也就是说，现状大致是：

- 已能合并“同一函数里先调用一次、后面又调用一次”的显式重复。
- 还不能把“每次循环迭代都重新调一次”的 invariant 查询提到循环外。

#### 第一版建议只覆盖最安全的形态

不要一开始就做通用 LICM。第一版建议只处理如下模式：

1. 调用目标明确是 `CJ_MCC_GetOrCreateTypeInfo`。
2. 调用位于某个有 preheader 的 loop 中。
3. `typeTemplate` 实参 loop-invariant。
4. `argSize` 是常量，优先只支持 `1` 或 `2`。
5. 第三个实参 `typeArgs[]` 对应的是局部 `alloca [1/2 x %TypeInfo*]` 缓冲区。
6. 缓冲区里的每个元素都来自 loop-invariant value，且 loop 内没有额外写回或 clobber。

这是当前最值得先做的切片，因为从已有 IR 样例看，`CJ_MCC_GetOrCreateTypeInfo` 常见形态就是：

```llvm
%buf = alloca [1 x %TypeInfo*]
store %TypeInfo* %arg0, %TypeInfo** %slot0
%args = bitcast [1 x %TypeInfo*]* %buf to i8**
%ti = call i8* @CJ_MCC_GetOrCreateTypeInfo(%tt, i32 1, i8** %args)
```

这里真正应该判断 invariant 的，不是 `i8** %args` 这个指针值本身，而是：

- `tt`
- `argSize`
- `%slot0/%slot1` 里装进去的 `TypeInfo*` 值

#### 可落地的实现步骤

建议分 4 步做：

1. **给 `CJGenericIntrinsicOpt` 增加 `LoopInfo` 依赖。**
  目的：拿到 call 所在 loop、preheader、loop-invariant 判断入口。

2. **新增一个专门处理 `CJ_MCC_GetOrCreateTypeInfo` 的 hoist 子逻辑。**
  不要混进 `sameTIOpt`，单独做一个类似 `tryHoistTypeInfoCall(CallInst *CI, LoopInfo &LI, ...)` 的入口，职责更清晰。

3. **把 `typeArgs[]` 解析成 canonical key。**
  第一版只识别：
  - `alloca [1 x %TypeInfo*]` / `alloca [2 x %TypeInfo*]`
  - `store` 到槽位
  - `bitcast` 后传给 `CJ_MCC_GetOrCreateTypeInfo`

  然后把它归一成：
  - `TypeTemplate*`
  - `argSize`
  - `SmallVector<Value*, 2>` 形式的 type arg values

4. **若 key 在 loop 内 invariant，则在 preheader 插入一次 call，并替换 loop 内同 key 调用。**
  这样收益才是“减少调用次数”，不是仅仅“同一基本块里复用结果”。

#### 安全条件建议

第一版建议保守，不追求全覆盖。只在以下条件都满足时 hoist：

1. loop 有唯一 preheader。
2. `tt` 与各个 type arg value 对当前 loop 都是 invariant。
3. `typeArgs[]` 缓冲区只在本 call 对应准备逻辑中写入，loop 内不存在别的写入。
4. 不跨异常边界、不跨可能改写 type arg 槽位的 memory clobber。
5. 如果无法把 `typeArgs[]` 解析成固定长度、固定槽位的值列表，就直接放弃，不做激进猜测。

#### 为什么不建议先在 runtime 里继续硬做

runtime 可以继续把单次 lookup 做快，但做不到下面这件事：

```text
原来：每次迭代 1 次 runtime 调用 + 1 次 lookup
目标：整个 loop 只做 1 次 runtime 调用
```

这正是方向 C 和方向 A/B 的本质区别：

- A/B 是“每次查得更便宜”
- C 是“尽量少查”

因此方向 C 的主战场必须是编译器或容器库，而不是 runtime。

#### 推荐验证方式

建议按以下顺序验证：

1. 在 `llvm/test/Transforms/Cangjie/` 下补一个 loop 场景测试。
  目标：验证 loop body 中多个相同 `CJ_MCC_GetOrCreateTypeInfo` 被提到 preheader，loop 里只剩复用值。

2. 先对 `argSize == 1` 的最小用例做 IR 检查。
  通过后再扩到 `argSize == 2`。

3. 编译一版 SDK，复跑 seed5 flamegraph。
  关注：
  - `TypeInfoManager::GetOrCreateTypeInfo`
  - `GenericTiDesc::computeHash`
  - `GenericTiDesc::operator==`
  是否按预期缩窄。

4. 再看 case1/seed5 的 wall time 是否比 runtime-only 版本继续下降。

#### 当前建议

如果要真正推进“优先级 2”，最小可落地方案是：

1. 保留现有 runtime `tls4` 版本作为兜底。
2. 在 `CJGenericIntrinsicOpt` 里新增 `CJ_MCC_GetOrCreateTypeInfo` 的 loop-invariant hoist。
3. 第一版只支持 `argSize == 1 / 2` 且 `typeArgs[]` 来自局部固定槽位 `alloca` 的模式。

这样改动面最小，收益路径最直接，也最容易通过 IR test 精确验证。

### 5.4 方向 D：把 generic copy 问题当成“copy 问题”而不是“barrier 问题”处理

对于 `WriteGeneric` / `WriteStruct` 这条线，当前不应优先去改 `MSMarkBarrier` 算法。

更合理的拆法是：

1. 哪些路径最终只是 plain old data copy
2. 哪些路径其实是 struct copy，但 ref 布局已知
3. 哪些路径确实需要完整 barrier 语义

只有先把这三类路径拆开，后续才能决定是否：

- 让更多路径直接走 plain copy
- 或把 generic payload copy 做成更轻的专用 helper

#### 2026-04-22 runtime-only 试验：把 no-ref payload 的 fast path 前移，结果回退

这轮先做了一个最小 runtime-only 试验，目标不是改 barrier 算法，而是验证下面这个假设：

> 如果 `src` 或 `dst` 对应的 generic payload `TypeInfo` 本身 **没有 ref field**，那么很多 `WriteGeneric` / `ReadGeneric` 调用其实可以更早直接落成 `memcpy_s`，不用继续走 `WriteStruct` / `ReadStruct` 的 phase barrier 路径。

试验点放在：

- `Barrier::WriteGeneric / ReadGeneric`
- `TraceBarrier::WriteGeneric / ReadGeneric`
- `EnumBarrier::WriteGeneric / ReadGeneric`

做法是：

1. 读取 generic box 的 `TypeInfo`。
2. 若 payload `HasRefField() == false`，则直接走 `memcpy_s`。
3. 跳过后续 `WriteStruct` / `ReadStruct`，以及 trace/enum 下 heap-to-heap 场景里的额外 helper 路径。

验证命令：

```bash
DISABLE_AUTO_UPDATE=true .venv/bin/python build.py build --gc-mode marksweep
DISABLE_AUTO_UPDATE=true bash MSGC_RESULT/Result/script/run_case.sh \
  --case case1_baseline --gc-mode marksweep --tag genericcopy_noref
```

结果如下：

| 版本 | Benchmark 总时间 (ms) | RSS 平均 (MB) | GC 总时间 (ms) | 结论 |
| --- | ---: | ---: | ---: | --- |
| `typeinfocache_tls4` | 5722 | 70.89 | 87.29 | 当前已确认正收益基线 |
| `typeinfocache_uuidkey_narrow` | 5704 | 86.31 | 96.03 | 当前第二层实验版本 |
| `genericcopy_noref` | 5852 | 81.47 | 106.36 | 5.4 试验版，明显回退 |

当前可以得出的判断是：

1. **“按 payload 是否含 ref 提前 memcpy” 这个切口，在当前 runtime-only 形态下不是正收益。**
2. 它虽然理论上减少了部分 `WriteStruct` / `ReadStruct` helper 链，但在 case1 上没有转化成更好的 wall time。
3. 更可能的原因是：`WriteGeneric` 热路径里额外的 `GetTypeInfo()/HasRefField()` 判断本身引入了新成本，而真正高频的 generic copy 里并没有足够多的“可被该规则直接摘掉”的 plain payload 场景。
4. 因此这版代码**不保留**，只把实验结论记录下来，避免后续在同一 runtime-only 切口上重复投入。

这也反过来说明：

- 5.4 更值得继续做的，不是“在 barrier 入口多加一层 runtime 判断”，
- 而是更上游地把 plain copy / struct copy / true barrier copy 三类路径拆开，让调用方少进 `WriteGeneric` 这层统一入口。

### 5.5 方向 E：减少容器内部 copy 次数，比优化单次 copy 更重要

从 HashMap `resize`、Array `init`、Iterator `next` 的样本看，很多 generic copy 的问题来自“调用次数太多”。

因此第二优先级的方向不是先把 `memcpy_s` 换实现，而是：

- 看容器内部是否存在可合并 copy
- 看扩容/搬迁时是否能减少 helper 调用粒度
- 看是否能用批量操作代替 repeated generic API

这类优化虽然落点可能不在 runtime 核心文件里，但从 sample 形态看，很可能比单独优化 `Barrier.cpp` 更有效。

#### 2026-04-22 最小 runtime 实现：marksweep 下为 zeroed fresh dst 跳过 old-ref 扫描

在继续追 `HashMap::resize` 的热路径后，结合 benchmark binary 反汇编与 runtime 分配器实现，确认了一个更窄的前提：

- `HashMap::resize` / `ArrayIterator::next` 一类路径里，很多 generic struct write 最终会落到 `MSMarkBarrier::WriteStruct`
- marksweep 分配侧会保证 fresh slot / recycled dead slot 在复用前被清零
- 对这类 destination，如果当前写入区间仍然全零，那么 `WriteStruct` 里的 old-ref 扫描实际上没有工作要做

因此先做了一版非常保守的 runtime-only 最小实现：

- 仅限 `MRT_USE_MARK_SWEEP`
- 仅改 `src/Heap/MBarrier/MSMarkBarrier.cpp`
- 在 `WriteStruct` 中新增 `dstLen <= 64` 的 zero-dst 检查
- 若目标写入区间当前全零，则跳过 `discover(old)`；`memcpy` 与 `discover(new)` 保持不变

验证命令：

```bash
DISABLE_AUTO_UPDATE=true bash MSGC_RESULT/Result/script/run_case.sh \
  --case case1_baseline \
  --gc-mode marksweep \
  --tag writezero_dst64
```

结果目录：`MSGC_RESULT/Result/case1_baseline/result/20260422_222943_writezero_dst64_marksweep/`

| 版本 | Benchmark 总时间 (ms) | RSS 平均 (MB) | GC 总时间 (ms) |
| --- | ---: | ---: | ---: |
| `typeinfocache_tls4` | 5722 | 79.87 | 84.66 |
| `typeinfocache_uuidkey_narrow` | 5704 | 82.95 | 90.33 |
| `writezero_dst64` | 5625 | 94.46 | 78.54 |

结论：

- 这版最小 5.5 实现是正收益，`case1` 相比当前 Step7 最优基线再降约 `79ms`（`5704 -> 5625`, 约 `1.4%`）
- 收益形态符合预期：不是减少 `TypeInfo` 查询，而是削掉了 fresh-entry 初始化时的一部分 barrier old-scan 成本
- 当前实现仍然是保守版本，只覆盖 `zeroed dst + small write (<= 64B)`；如果后续还要继续挖，可以优先看是否值得把同类 fast path 扩到 `CopyStructArray` 或更明确的 fresh-object 初始化接口

---

## 6. 优化优先级

结合当前证据，建议按以下顺序推进：

### 优先级 1：`TypeInfoManager` 最近命中缓存

理由：

- 热点稳定
- 改动面小
- 风险低
- 最容易用 seed5 benchmark 快速验证

当前状态：**已完成一轮 runtime-only 实现与 case1 验证，结果为正向收益，适合继续保留并作为后续优化的基线。**

### 优先级 2：减少容器热路径里的重复 `GetOrCreateTypeInfo`

理由：

- 这是真正减少 helper 次数的做法
- 潜在收益可能大于单纯优化 hash/compare

### 优先级 3：拆 generic copy 路径，区分 plain copy 与 true barrier copy

理由：

- 当前 `Barrier` 热点主体其实是 copy
- 应先按 copy 语义拆，再决定是否优化 barrier

### 优先级 4：进一步瘦身 `GenericTiDescHashMap`

理由：

- 这是对优先级 1 的继续深挖
- 适合在确认 cache 有效后再做

---

## 7. 结论

seed5 暴露出的泛型问题，本质上不是“collector 变慢”，而是：

> **泛型容器每次操作都会附带一组 runtime helper：类型查询、generic copy、结构体/引用读写，这组 helper 在高频路径里被整体放大。**

其中当前最清晰、最值得优先验证的点是：

- `TypeInfoManager` 缺少热路径缓存
- 同一批泛型实例被循环反复查找

而 `Barrier` 那一簇热点则更应该被理解成：

- “generic copy 成本”
- 而不是“mark barrier 成本”

因此 Step 7 的首要目标不是改 collector，而是先让：

1. 泛型 `TypeInfo` 查询更少
2. 查询命中更快
3. generic copy 的 helper 层次更薄

只要这三点能压下去，seed5 下的泛型热点才有机会明显收敛。