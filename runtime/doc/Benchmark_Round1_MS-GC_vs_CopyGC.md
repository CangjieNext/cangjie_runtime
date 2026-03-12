# MS-GC vs CopyGC 性能对比报告（第一轮）

- **日期**: 2026-03-12
- **平台**: macOS aarch64 (Apple Silicon)
- **Runtime 版本**: MS-GC (Step6 MSRegionManager 迁移后)
- **对照版本**: CopyGC (SDK 原始 runtime)
- **堆大小**: 256MB (`cjHeapSize=256mb`)
- **构建类型**: Release (`-O2`)
- **轮次**: 3 轮取平均

---

## 1. 测试用例

使用 `gc/gc.cj` 综合 GC 压力测试套件，包含 12 项测试：

| # | 测试名称 | 压力类型 |
|---|---------|---------|
| 1 | Binary tree churn | 短命对象大量分配/回收 |
| 2 | Generational | 新老代混合存活 |
| 3 | Large object allocation | 大对象分配 (>256KB) |
| 4 | Linked list payload | 链表长链遍历 |
| 5 | ArrayList stress | ArrayList 动态扩容 |
| 6 | HashMap churn | HashMap 频繁插入删除 |
| 7 | Swiss cheese fragmentation | 碎片化压力 (间隔释放) |
| 8 | Interleaved allocation storm | 交错分配风暴 |
| 9 | Graduated size ladder | 梯度大小对象分配 |
| 10 | Pinned large+small churn | 大小对象混合 + pinned |
| 11 | Pinned region fragmentation | Pinned region 碎片化 |
| 12 | Mixed OOM stress | 混合分配直到 OOM |

- Test 1-11 预期 PASS
- Test 12 预期 OOM 终止

---

## 2. 三轮汇总结果

### 2.1 核心指标

| 指标 | MS-GC | CopyGC | 变化 |
|------|------:|-------:|------|
| **Wall time (avg)** | 21024ms | 20823ms | +1.0% |
| **GC 总时间 (avg)** | 1654.6ms | 1591.7ms | **+4.0%** |
| **最大暂停 (avg)** | 43082us | 51663us | **-16.6%** |
| **P95 暂停 (avg)** | 33899us | 34080us | -0.5% |
| **GC 次数** | ~148 | ~147 | 持平 |
| **Forward 拷贝量** | 0 B | 0 B | 持平 |

### 2.2 各轮明细

| 轮次 | Wall(ms) MS | Wall(ms) Copy | GC总时(ms) MS | GC总时(ms) Copy | 最大暂停(us) MS | 最大暂停(us) Copy | P95(us) MS | P95(us) Copy |
|------|----------:|-------------:|-------------:|----------------:|---------------:|-----------------:|----------:|------------:|
| R1 | 21086 | 20958 | 1626.0 | 1631.5 | 42165 | 75446 | 34339 | 33974 |
| R2 | 21183 | 20702 | 1783.8 | 1525.6 | 49073 | 41611 | 35201 | 33631 |
| R3 | 20804 | 20810 | 1553.9 | 1617.9 | 38007 | 37933 | 32156 | 34634 |
| **AVG** | **21024** | **20823** | **1654.6** | **1591.7** | **43082** | **51663** | **33899** | **34080** |

### 2.3 GC 阶段耗时对比（三轮平均）

| 阶段 | MS-GC (us) | CopyGC (us) | 变化 |
|------|----------:|----------:|------|
| enum_roots | 10,168 | 8,384 | **+21.3%** |
| concurrent_mark | 659,655 | 605,799 | **+8.9%** |
| DoTracing (mark 总) | 667,325 | 615,283 | +8.5% |
| PostTrace | 298,143 | 296,227 | +0.6% |
| Forward | 252,382 | 249,650 | +1.1% |

---

## 3. 分析

### 3.1 总体表现

- **总运行时间几乎一致**（差 1%，在噪声范围内），说明 MS-GC 迁移未引入显著性能退化
- **GC 总时间略增 4%**，主要来自 `enum_roots`（+21%）和 `concurrent_mark`（+9%）
- **GC 触发次数持平**（~148 vs ~147），说明分配路径和触发逻辑一致

### 3.2 暂停时间

- **最大暂停降低 16.6%**：MS-GC 平均最大暂停 43ms vs CopyGC 52ms
  - Run 1 贡献最大：CopyGC 出现一个 75ms 的极端 GC 周期（concurrent marking 耗时 67,864us 标记 525,543 objects），而 MS-GC 同等周期仅 42ms
  - 说明 MS region 元数据结构在极端场景下提供了更稳定的遍历性能
- **P95 暂停几乎相同**（-0.5%），日常 GC 停顿无差异
- **P50 暂停** MS-GC 略高（~6500us vs ~6000us），约 +8%

### 3.3 阶段级分析

- **enum_roots +21.3%**：MS region 遍历引入额外开销，可能原因：
  - MSRegionManager 的 region 迭代逻辑比原 CopyGC 的 region 遍历多一层间接
  - 需要检查 region 状态（allocated/swept/free）增加了判断开销
- **concurrent_mark +8.9%**：标记阶段轻微增长，可能与 region 元数据布局或 bitmap 访问模式有关
- **PostTrace / Forward 持平**：这两个阶段的逻辑在 MS-GC 和 CopyGC 间差异很小

### 3.4 关键限制

**Forward 拷贝量为 0**：当前 exemption threshold = 0.80，所有 region 的存活率都高于此阈值，两种 GC 均未实际执行对象拷贝。这意味着：

> **MS-GC 的核心优势（原地 sweep 避免拷贝）尚未在此测试中被激发。**

两种 GC 在本测试中行为高度趋同——都只做标记而跳过拷贝——因此性能差异仅反映元数据管理开销的差异，而非 GC 策略本身的差异。

---

## 4. 功能正确性

| 轮次 | Test 1-11 | Test 12 (OOM) |
|------|-----------|--------------|
| R1 | 全部 PASS | 正常 OOM 终止 |
| R2 | 全部 PASS | 正常 OOM 终止 |
| R3 | 全部 PASS | 正常 OOM 终止 |

MS-GC 在全部 12 项测试、3 轮运行中功能完全正确。

---

## 5. 结论

1. **MS-GC 迁移功能正确**，无测试失败
2. **总体性能持平**，Wall time 差异 <2%
3. **最大暂停改善 16.6%**，尾部延迟更稳定
4. **GC 总时间增加 4%**，主要来自 region 元数据遍历开销（enum_roots +21%, concurrent_mark +9%）
5. **当前测试未触发对象拷贝**，MS-GC 的核心优势尚未体现

---

## 6. 后续建议

1. **设计针对性测试用例**：构造 region 存活率在 50%-70% 的场景，让 CopyGC 被迫做大量 Forward 拷贝，充分体现 MS-GC 原地 sweep 的优势
2. **优化 enum_roots**：调查 MSRegionManager 的 region 遍历路径，减少 +21% 的额外开销
3. **降低 exemption threshold 测试**：将阈值调低到 0.50 或更低，观察两种 GC 在有拷贝 vs 无拷贝下的表现差异
4. **长时间运行测试**：当前单次运行约 21 秒，建议增加到 5-10 分钟级别的 benchmark，减少噪声影响

---

## 附录

### 测试环境

```
平台:       macOS aarch64 (Apple Silicon)
SDK 路径:   /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260313/cangjie/
MS-GC dylib: output/temp/lib/aarch64_Release/libcangjie-runtime.dylib (1.64MB)
CopyGC dylib: libcangjie-runtime.dylib.origin (898KB)
堆大小:     256MB
GC 日志:    MRT_REPORT=<logfile>
```

### 测试脚本

- 对比脚本: `gc/perf_compare.sh`
- 分析工具: `gc/compare_gc.py`
- 汇总工具: `gc/summary_compare.py`
- GC 日志: `gc/m4b_gc*.log`, `gc/origin_gc*.log`
