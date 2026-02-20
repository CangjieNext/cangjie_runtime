# GC 性能基线报告（无 LOS）

> **日期**: 2026-02-20
> **Runtime 版本**: 无 LOS（原始 Copy GC）
> **测试平台**: macOS aarch64
> **堆配置**: 256MB (cjHeapSize=256mb)
> **编译器**: cjc (dev/20260220)
> **测试用例**: gc.cj（11 个测试）

---

## 1. 测试用例概览

| 测试编号 | 名称 | 参数 | 目的 |
|---------|------|------|------|
| Test 1 | Binary Tree Churn | depth=18, iter=200 | 短命对象风暴（52万节点×200） |
| Test 2 | Generational | long=18, short=14, iter=500 | 分代：长命+短命混合 |
| Test 3 | Large Objects | iter=100 | 64KB~1MB 大对象分配 |
| Test 4 | Linked List Payload | 1000节点×2048载荷×100轮 | 碎片化链表+负载 |
| Test 5 | ArrayList Stress | 50万元素×200轮 | 大容器扩容/释放 |
| Test 6 | HashMap Churn | 5万KV×100轮 | 哈希表高频翻搅 |
| Test 7 | Swiss Cheese | 40对象×20轮 | 128KB大对象→释放奇数→回填256KB |
| Test 8 | Interleaved Storm | 50轮 | 256KB/512KB大+小树交替 |
| Test 9 | Graduated Size Ladder | 20轮 | 128KB~1MB阶梯→部分释放→回填 |
| Test 10 | Pinned Large + Small Churn | 30×512KB常驻, 100轮 | 大对象钉死+小对象高频GC |
| Test 11 | OOM Stress | 无限累积 | 打爆内存至 OOM |

### 测试执行结果

所有测试均正常完成（Test 11 按设计 OOM 退出）：
- Test 1-6: Phase 1（常规压力测试）— 全部 PASSED
- Test 7-10: Phase 2（碎片化专项测试）— 全部 PASSED
- Test 11: Phase 3（OOM 测试）— 在 ~200MB 累积后 OOM

---

## 2. GC 总体统计

### 2.1 基本指标

| 指标 | 值 |
|------|----|
| GC 总周期数 | **141** |
| Heuristic 触发 | **103** (73.0%) |
| OOM 触发 | **38** (27.0%) |
| 堆大小 | 256MB = 268,435,456 B |
| Region 大小 | 64KB |
| 总 Region 数 | 16,384 units |
| Exemption 阈值 | 0.80 |
| 堆利用率 | 0.80 |
| GC 线程数 | 2 (1 helper) |

### 2.2 GC 耗时统计（微秒）

| 阶段 | 样本数 | 最小 | 中位数 | 平均 | P90 | 最大 |
|------|--------|------|--------|------|-----|------|
| Concurrent Mark | 141 | 11 | 35 | 3,784 | 14,024 | 35,048 |
| Forward/Copy | 141 | 23 | 168 | 787 | 3,496 | 4,740 |
| PostTrace | 141 | 1,262 | 2,022 | 2,361 | 3,040 | 20,814 |

### 2.3 Region 利用率

| 指标 | 值 |
|------|----|
| 最小活跃 units | 2,052 (12.5%) |
| 平均活跃 units | 16,231 (99.1%) |
| 最大活跃 units | 16,380 (100.0%) |

---

## 3. 大对象碎片化分析（核心基线数据）

### 3.1 总体概况

| 指标 | 值 |
|------|-----|
| 含大对象的 GC 周期数 | **112** / 141 (79.4%) |
| 不含大对象的 GC 周期数 | 29 (20.6%) |
| 大对象在 MAIN 中的最大占用 | **204.8MB** |
| 大对象在 MAIN 中的平均占用 | **57.1MB** |
| 存活大对象（stale pinned）最大 | **204.8MB** |
| 存活大对象（stale pinned）平均 | **17.6MB** |
| 最大碎片率 (live_large/total) | **97.78%** |
| 平均碎片率 | **9.42%** |

### 3.2 按测试阶段分析

#### Phase 1: Test 1-6（常规测试，含 Test 3 大对象）

| 指标 | 值 |
|------|----|
| 含大对象的 GC 周期 | 13 |
| 大对象累计在 MAIN | 196.5MB |
| 存活大对象累计 | 9.0MB |
| 每次 GC 平均存活大对象 | 0.7MB |
| 峰值存活大对象 | 1.0MB |

**特征**: Test 1-2 全部 `old large 0 - 0 B`（无大对象），Test 3+ 开始出现大对象。Phase 1 大对象生命周期短，存活量小。

#### Phase 2: Test 7-10（碎片化专项）

| 指标 | 值 |
|------|----|
| 含大对象的 GC 周期 | 25 |
| 大对象累计在 MAIN | 1,710.2MB |
| 存活大对象累计 | 104.2MB |
| 每次 GC 平均存活大对象 | **4.2MB** |
| 峰值存活大对象 | **4.6MB** |

**特征**: 碎片化测试阶段大对象密度最高。典型 GC 周期中 73.4MB 大对象在 MAIN 中参与回收，但始终有 4.6MB 存活大对象钉在 MAIN 中无法移动，占据 MAIN region 空间。

#### Phase 3: Test 11（OOM）

| 指标 | 值 |
|------|----|
| 含大对象的 GC 周期 | 74 |
| 大对象累计在 MAIN | 4,489.1MB |
| 存活大对象累计 | 1,855.1MB |
| 每次 GC 平均存活大对象 | **25.1MB** |
| 峰值存活大对象 | **204.8MB** |

**特征**: OOM 阶段后期（GC#131-141），大对象 garbage=0（全部存活），从 44MB 增长到 205MB，占满整个堆。

### 3.3 逐 GC 周期大对象明细

```
  GC#     Reason  LargeTotal   LargeGarb  LiveLarge  SmallTotal    Alloc
--------------------------------------------------------------------------------
   19        oom       0.3MB       0.3MB      0.0MB     127.4MB    128MB
   20  heuristic      16.5MB      15.5MB      1.0MB     173.0MB    190MB
   21  heuristic      17.9MB      16.9MB      1.0MB     181.1MB    199MB
   22  heuristic      19.8MB      18.8MB      1.0MB     189.8MB    209MB
   23  heuristic      17.9MB      16.9MB      1.0MB     190.1MB    208MB
   24  heuristic      19.8MB      18.8MB      1.0MB     190.4MB    210MB
   25  heuristic       1.0MB       0.0MB      1.0MB       4.1MB      5MB
   26  heuristic      16.9MB      16.4MB      0.5MB     171.2MB    188MB
   27  heuristic      16.6MB      16.5MB      0.1MB     173.2MB    190MB
   28  heuristic      18.8MB      17.7MB      1.0MB     190.4MB    209MB
   29  heuristic      18.2MB      18.0MB      0.3MB     189.4MB    208MB
   30  heuristic      18.7MB      17.7MB      1.0MB     193.4MB    212MB
   31  heuristic      14.1MB      14.1MB      0.0MB     194.8MB    209MB
   43        oom      17.8MB      16.5MB      1.4MB     175.3MB    193MB
   44  heuristic      66.1MB      61.5MB      4.6MB     122.6MB    189MB
   45  heuristic      73.4MB      68.8MB      4.6MB     130.1MB    204MB
   46  heuristic      73.4MB      68.8MB      4.6MB     133.0MB    207MB
   47  heuristic      73.4MB      68.8MB      4.6MB     133.9MB    207MB
   48        oom      68.8MB      65.8MB      3.1MB     126.6MB    196MB
   49  heuristic      62.7MB      58.1MB      4.6MB     121.3MB    184MB
   50  heuristic      73.4MB      68.8MB      4.6MB     127.8MB    201MB
   51  heuristic      73.4MB      68.8MB      4.6MB     133.3MB    207MB
   52  heuristic      73.4MB      68.8MB      4.6MB     134.0MB    208MB
   53  heuristic      73.4MB      68.8MB      4.6MB     134.9MB    208MB
   54        oom      68.8MB      65.8MB      3.1MB     125.4MB    194MB
   55  heuristic      62.7MB      58.1MB      4.6MB     121.1MB    184MB
   56  heuristic      73.4MB      68.8MB      4.6MB     129.1MB    203MB
   57  heuristic      73.4MB      68.8MB      4.6MB     134.4MB    208MB
   58  heuristic      73.4MB      68.8MB      4.6MB     132.8MB    206MB
   59  heuristic      73.4MB      68.8MB      4.6MB     135.6MB    209MB
   60        oom      68.8MB      65.8MB      3.1MB     123.6MB    193MB
   61  heuristic      62.7MB      58.1MB      4.6MB     122.2MB    185MB
   62  heuristic      73.4MB      68.8MB      4.6MB     129.9MB    203MB
   63  heuristic      73.4MB      68.8MB      4.6MB     132.4MB    206MB
   64  heuristic      73.4MB      68.8MB      4.6MB     133.9MB    208MB
   65  heuristic      73.4MB      68.8MB      4.6MB     133.3MB    207MB
   66        oom      65.8MB      63.7MB      2.0MB     122.9MB    189MB
   67  heuristic      64.8MB      60.2MB      4.6MB     121.4MB    186MB
   68  heuristic      73.4MB      68.8MB      4.6MB     129.6MB    203MB
   69  heuristic      73.4MB      68.8MB      4.6MB     131.0MB    205MB
   70  heuristic      73.4MB      68.8MB      4.6MB     134.1MB    208MB
   71        oom      68.8MB      65.8MB      3.1MB     128.7MB    198MB
   72  heuristic      62.7MB      58.1MB      4.6MB     122.2MB    185MB
   73  heuristic      73.4MB      68.8MB      4.6MB     129.6MB    203MB
   74  heuristic       4.6MB       0.0MB      4.6MB       2.1MB      7MB
   75  heuristic      73.4MB      68.8MB      4.6MB     124.3MB    198MB
   76  heuristic      68.8MB      65.8MB      3.1MB     128.9MB    198MB
   77  heuristic      71.9MB      68.8MB      3.1MB     133.6MB    206MB
   78  heuristic      66.8MB      65.4MB      1.4MB     132.0MB    199MB
   79  heuristic      75.3MB      70.2MB      5.1MB     135.6MB    208MB
   80        oom      60.2MB      57.1MB      3.1MB     109.4MB    170MB
   81  heuristic      62.7MB      58.1MB      4.6MB     122.7MB    186MB
   82  heuristic      73.4MB      68.8MB      4.6MB     129.1MB    203MB
   83  heuristic      73.4MB      68.8MB      4.6MB     134.8MB    208MB
   84  heuristic      73.4MB      68.8MB      4.6MB     134.7MB    208MB
   85  heuristic      73.4MB      68.8MB      4.6MB     133.3MB    207MB
   86  heuristic      44.9MB      42.8MB      2.1MB     148.8MB    194MB
   87  heuristic      31.6MB      31.1MB      0.4MB     158.9MB    190MB
   88  heuristic      34.0MB      31.4MB      2.6MB     156.5MB    191MB
   89  heuristic      37.1MB      34.5MB      2.6MB     169.2MB    207MB
   90  heuristic       2.6MB       0.0MB      2.6MB       5.8MB     17MB
   91  heuristic      30.5MB      30.3MB      0.2MB     147.8MB    178MB
   92  heuristic      34.3MB      31.7MB      2.6MB     152.5MB    187MB
   93  heuristic      34.5MB      32.6MB      1.9MB     162.1MB    196MB
   94  heuristic      32.0MB      29.4MB      2.6MB     157.8MB    190MB
   95  heuristic      23.3MB      20.7MB      2.6MB     101.7MB    125MB
   96  heuristic      34.5MB      32.9MB      1.6MB     158.4MB    193MB
   97  heuristic      31.8MB      29.2MB      2.6MB     153.5MB    185MB
   98  heuristic      37.1MB      34.5MB      2.6MB     168.3MB    206MB
   99  heuristic      37.1MB      33.9MB      3.1MB     167.0MB    203MB
  100  heuristic      33.4MB      32.4MB      1.0MB     162.7MB    196MB
  101  heuristic      32.8MB      30.1MB      2.6MB     158.1MB    191MB
  102  heuristic      37.1MB      34.5MB      2.6MB     170.4MB    208MB
  103  heuristic      37.1MB      34.5MB      2.6MB     170.2MB    207MB
  104  heuristic      36.1MB      32.9MB      3.2MB     163.0MB    197MB
  105  heuristic      32.3MB      31.9MB      0.4MB     158.1MB    190MB
  106  heuristic      34.3MB      31.7MB      2.6MB     157.5MB    192MB
  107  heuristic      37.1MB      34.5MB      2.6MB     167.9MB    205MB
  108  heuristic      33.8MB      30.2MB      3.7MB     170.6MB    204MB
  109  heuristic       4.8MB       0.0MB      4.8MB       6.7MB     11MB
  110        oom      38.8MB      35.9MB      2.8MB     213.3MB    252MB
  111        oom      37.8MB      33.1MB      4.7MB     217.6MB    256MB
  112        oom      38.6MB      35.8MB      2.8MB     213.8MB    255MB
  113        oom      37.1MB      35.9MB      1.1MB     218.1MB    255MB
  114        oom      34.8MB      30.1MB      4.7MB     212.9MB    256MB
  115        oom      39.0MB      36.6MB      2.4MB     208.6MB    256MB
  116        oom      35.1MB      31.4MB      3.7MB     212.8MB    256MB
  117        oom      36.6MB      34.0MB      2.7MB     211.0MB    256MB
  118        oom      36.7MB      34.1MB      2.7MB     210.9MB    256MB
  119        oom      35.6MB      31.4MB      4.2MB     212.2MB    256MB
  120        oom      38.2MB      36.6MB      1.6MB     209.4MB    256MB
  121        oom      34.6MB      31.4MB      3.2MB     213.2MB    256MB
  122        oom      37.6MB      37.6MB      0.0MB     218.0MB    256MB
  123        oom      36.5MB      33.2MB      3.3MB     219.1MB    256MB
  124        oom      40.0MB      33.2MB      6.7MB     215.5MB    256MB
  125        oom      68.3MB      52.5MB     15.9MB     184.1MB    256MB
  126        oom      82.8MB      67.3MB     15.6MB     169.7MB    256MB
  127  heuristic      18.1MB       0.2MB     17.9MB       0.2MB     19MB
  128        oom      83.1MB      67.6MB     15.5MB     167.9MB    256MB
  129        oom      83.4MB      68.0MB     15.5MB     170.3MB    256MB
  130  heuristic      39.8MB      31.8MB      8.0MB     160.2MB    200MB
  131        oom      44.2MB       0.0MB     44.2MB     210.0MB    254MB
  132        oom      72.3MB       0.0MB     72.3MB     179.8MB    252MB
  133        oom     100.4MB       0.0MB    100.4MB     155.2MB    256MB
  134        oom     120.5MB       0.0MB    120.5MB     134.9MB    256MB
  135        oom     140.5MB       0.0MB    140.5MB     114.9MB    256MB
  136        oom     156.6MB       0.0MB    156.6MB      98.8MB    256MB
  137        oom     172.7MB       0.0MB    172.7MB      82.6MB    255MB
  138        oom     184.7MB       0.0MB    184.7MB      70.6MB    255MB
  139        oom     192.8MB       0.0MB    192.8MB      62.5MB    255MB
  140        oom     200.8MB       0.0MB    200.8MB      54.3MB    255MB
  141        oom     204.8MB       0.0MB    204.8MB      24.8MB    230MB
```

---

## 4. 分阶段 GC 行为详细分析

### 4.1 Phase 1: Test 1-2（纯小对象，GC#1-18）

这是纯小对象阶段，所有 GC 周期 `old large 0 - 0 B`。

**典型 GC 周期特征:**
- 回收量: 200MB~250MB/次
- Garbage ratio: 100%（全部为垃圾）
- 大对象: 0
- 这是理想的 MAIN 使用场景 — 无碎片

### 4.2 Phase 1: Test 3-6（混合大小对象，GC#19-42）

Test 3 引入了 64KB~1MB 大对象。此后大对象与小对象在 MAIN 中混排。

**关键观察:**
- GC#19: 首次出现大对象（0.3MB），由 OOM 触发
- GC#20-31: 大对象 16~20MB/次在 MAIN 中，存活部分 0.1~1.0MB
- GC#32-42: 大对象回归 0（Test 4-6 不产生大对象级别的分配）
- 1MB 存活大对象占 MAIN 的 ~0.5%，碎片影响较小

### 4.3 Phase 2: Test 7-10（碎片化专项，GC#43-109）

这是碎片化测试的核心阶段，数据最有价值。

#### Test 7-8 (Swiss Cheese + Interleaved Storm, ~GC#43-85)

**关键数据:**
- 大对象在 MAIN 中稳定占 **73.4MB/次**（满负荷时）
- 存活大对象固定为 **4.6MB**（30个512KB对象 = 15MB 中的部分）
- 周期性 OOM 触发（GC#48, 54, 60, 66, 71, 80）
- 每次 OOM 后 live_large 降到 3.1MB，然后回升到 4.6MB

**碎片化表现:**
```
无 LOS 时 MAIN 布局示意:
[small][small][LARGE 512KB][small][small][LARGE 256KB][small][LARGE 128KB][small]...
   ↑ 小对象            ↑ 大对象钉住              ↑ 大对象释放后的空洞
```

- 大对象在 MAIN 中占 73.4 / (73.4 + 133) ≈ **35.5%** 的回收空间
- 存活 4.6MB 钉在 MAIN 中，碎片率 = 4.6 / 207 ≈ **2.2%**

#### Test 9-10 (Graduated Ladder + Pinned Churn, ~GC#86-109)

**关键数据:**
- 大对象波动大：31~37MB/次
- 存活大对象: 0.2~4.8MB
- 两个里程碑式 GC（#90, #109）: live_large = 2.6MB/4.8MB，garbage = 0

**碎片化表现:**
- Test 9 的阶梯尺寸（128KB~1MB）造成**不同大小的空洞**
- Test 10 的 30 个 512KB 常驻对象（约 15MB）永远钉在 MAIN 中
- 小对象只能在这些大对象之间的间隙分配 → MAIN 利用率下降

### 4.4 Phase 3: Test 11（OOM 压力，GC#110-141）

**标志性的碎片恶化过程:**

| 阶段 | GC 周期 | 特征 |
|------|---------|------|
| 累积期 | #110-124 | 大对象 35~40MB, 存活 1~7MB, OOM 连续触发 |
| 恶化期 | #125-130 | 存活大对象跳到 8~18MB, 开始无法回收 |
| 崩溃期 | #131-141 | **garbage=0, 大对象全部存活**, 44→205MB 单调递增 |

GC#141 终态: 大对象 204.8MB（存活）+ 小对象 24.8MB = 总堆接近耗尽

---

## 5. 基线对比指标汇总

以下为新 Runtime（带 LOS）运行后应重点对比的指标：

### 5.1 必比指标

| # | 指标 | 基线值（无 LOS） | 预期值（有 LOS） |
|---|------|------------------|------------------|
| 1 | GC 周期中含大对象的次数 | 112 / 141 (79.4%) | **0 / N** (0%) |
| 2 | 大对象在 MAIN 中峰值 | 204.8MB | **0 MB** |
| 3 | 存活大对象在 MAIN 平均 | 17.6MB | **0 MB** |
| 4 | 平均碎片率 | 9.42% | **~0%** |
| 5 | 最大碎片率 | 97.78% | **~0%** |
| 6 | Phase 2 存活大对象/次 | 4.2MB | **0 MB (in MAIN)** |

### 5.2 性能指标

| # | 指标 | 基线值（无 LOS） | 变化方向 |
|---|------|------------------|---------|
| 1 | Concurrent Mark 中位数 | 35 μs | 应持平或下降 |
| 2 | Concurrent Mark P90 | 14,024 μs | 应持平或下降 |
| 3 | Forward/Copy 中位数 | 168 μs | **应明显下降** |
| 4 | Forward/Copy P90 | 3,496 μs | **应明显下降** |
| 5 | PostTrace 中位数 | 2,022 μs | 应持平 |
| 6 | Heuristic GC 次数 | 103 | 应持平或减少 |
| 7 | OOM GC 次数 | 38 | 应减少 |

### 5.3 Region 使用指标

| # | 指标 | 基线值（无 LOS） | 变化方向 |
|---|------|------------------|---------|
| 1 | 平均活跃 units | 16,231 (99.1%) | 应下降（MAIN 更宽裕） |
| 2 | 最大活跃 units | 16,380 (100.0%) | 应下降 |
| 3 | garbage_ratio 均值 | ~99% | 应提高（MAIN 更干净） |

### 5.4 新增 LOS 指标（基线无数据）

以下指标仅在有 LOS 时有意义，基线值全为 0：

| # | 指标 | 基线值 | 预期值 |
|---|------|--------|--------|
| 1 | LOS 分配次数 | 0 | >0 |
| 2 | LOS 回收次数 | 0 | >0 |
| 3 | LOS 空间占用 | 0 | 根据大对象总量 |
| 4 | LOS FreeRegion 合并次数 | 0 | >0 |
| 5 | LOS 碎片率 | N/A | 应低 |

---

## 6. 原始数据文件

| 文件 | 说明 |
|------|------|
| `gc/gc.log` | GC 原始日志（6773 行） |
| `gc/baseline_gc_cycles.json` | 141 个 GC 周期的结构化数据 |
| `gc/gc.cj` | 测试用例源码（575 行，11 个测试） |
| `gc/analyze_frag.py` | 碎片化分析脚本 |
| `gc/extract_baseline.py` | 基线数据提取脚本 |

---

## 7. 如何运行对比测试

```bash
# 1. 编译带 LOS 的 runtime
python3 build.py build --target native --build-type release -v 0.0.1

# 2. 重新编译测试用例（用新 runtime 的 cjc）
source <新cjc环境>
cjc gc.cj -o main

# 3. 运行
export MRT_REPORT=./gc_with_los.log
export cjHeapSize=256mb
./main

# 4. 对比分析
# 修改 analyze_frag.py 和 extract_baseline.py 的输入文件名
# 或者用 diff 对比两份 JSON：
python3 -c "
import json
baseline = json.load(open('baseline_gc_cycles.json'))
los = json.load(open('los_gc_cycles.json'))
# ... 对比逻辑
"
```

---

## 附录：测试输出日志

```
========================================
   Cangjie GC Stress Test Suite
========================================
  Tip: export MRT_REPORT=./gc.log   to enable GC logging
  Tip: export cjHeapSize=256mb       to limit heap size
========================================

[Test 1] Binary Tree Churn  depth=18  iterations=200
  round 0: nodes=524287  checksum=18
  ...
  total checksum: 3600
  DONE

[Test 2] Generational  long-lived=18  short-lived=14  iterations=500
  long-lived tree checksum: 18
  ...
  long-lived tree intact: PASSED
  DONE

[Test 3] Large Object Allocation  iterations=100
  64KB: elements=8192  checksum=33550336
  256KB: elements=32768  checksum=536854528
  512KB: elements=65536  checksum=2147450880
  1MB: elements=131072  checksum=8589869056
  ...
  DONE

[Test 4] Linked List + Payload  length=1000  payload=2048  iterations=100
  round 0: sum=499500
  ...
  DONE

[Test 5] ArrayList Stress  maxSize=500000  iterations=200
  round 0: size=500000  sum=41666541666750000
  ...
  DONE

[Test 6] HashMap Churn  size=50000  iterations=100
  round 0: entries=50000  total=7499850000
  ...
  DONE

========================================
   Phase 2: Fragmentation-focused tests
   (LOS vs no-LOS comparison)
========================================

[Test 7] Swiss Cheese Fragmentation  objects=40  iterations=20
  round 0: live_large=20  bigAllocs=10  smallTrees=1000
  round 5: live_large=20  bigAllocs=10  smallTrees=1000
  round 10: live_large=20  bigAllocs=10  smallTrees=1000
  round 15: live_large=20  bigAllocs=10  smallTrees=1000
  DONE

[Test 8] Interleaved Large/Small Storm  rounds=50
  persistent small objects: 500 trees
  round 0: large10×256KB + small500 + large5×512KB  bigChecksum=24158453760
  round 10: large10×256KB + small500 + large5×512KB  bigChecksum=561012981760
  round 20: large10×256KB + small500 + large5×512KB  bigChecksum=1097867509760
  round 30: large10×256KB + small500 + large5×512KB  bigChecksum=1634722037760
  round 40: large10×256KB + small500 + large5×512KB  bigChecksum=2171576565760
  persistent trees intact, checksum=4000
  DONE

[Test 9] Graduated Size Ladder  iterations=20
  round 0: ladder_live=4  elements=221184  fillCheck=4999507968
  round 5: ladder_live=4  elements=221184  fillCheck=12923189587968
  round 10: ladder_live=4  elements=221184  fillCheck=25841379667968
  round 15: ladder_live=4  elements=221184  fillCheck=38759569747968
  DONE

[Test 10] Pinned Large + Small Churn  pinned=30  iterations=100
  pinned 30 large objects (~15MB)
  round 0: small_trees=200  chain_sum=124750  arr_size=100000  pinned_ok=0
  round 20: small_trees=200  chain_sum=124750  arr_size=100000  pinned_ok=0
  round 40: small_trees=200  chain_sum=124750  arr_size=100000  pinned_ok=0
  round 60: small_trees=200  chain_sum=124750  arr_size=100000  pinned_ok=0
  round 80: small_trees=200  chain_sum=124750  arr_size=100000  pinned_ok=0
  final pinned checksum: 998564659200
  DONE

========================================
   Phase 2 (fragmentation tests) done!
   Now entering Phase 3: OOM stress...
========================================

[Test 11] OOM Stress — accumulate until crash
  WARNING: This test is designed to exhaust heap memory!
  Set cjHeapSize to limit heap, e.g. export cjHeapSize=256mb

  round 10: accumulated ~40MB  chunks=10
  round 20: accumulated ~80MB  chunks=20
  round 30: accumulated ~120MB  chunks=30
  round 40: accumulated ~160MB  chunks=40
  round 50: accumulated ~200MB  chunks=50
An exception has occurred:
    Out of memory
```
