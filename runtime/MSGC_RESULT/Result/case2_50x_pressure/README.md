# Case 2: 50 倍压力模式

## 测试目标

在 Case 1 基准测试的基础上，将所有循环次数放大 **50 倍**，测试 GC 在极端分配压力下的表现：

- **GC 触发频率**：高分配速率是否导致 GC 过于频繁
- **吞吐量稳定性**：持续高压下分配吞吐是否线性扩展
- **停顿时间**：大量对象存活/死亡时 GC 停顿是否可控
- **OOM 风险**：在高压力下堆增长策略是否合理

## 放大参数

| 测试项 | 原始值 | 50x 值 |
|--------|--------|---------|
| F1 rounds | 20 | 1000 |
| F2 temp trees | 200 | 10000 |
| F3 rounds | 50 | 2500 |
| F4 rounds | 30 | 1500 |
| F5 rounds | 20 | 1000 |
| P1 rounds | 50 | 2500 |
| P2 rounds | 200 | 10000 |
| P3 rounds | 30 | 1500 |
| P4 churn rounds | 100 | 5000 |
| P5 rounds | 15 | 750 |

## 运行方式

```bash
source /path/to/cangjie/envsetup.sh
cjc benchmark.cj -o benchmark
MRT_REPORT=./runtime.log cjHeapSize=4gb ./benchmark
```

> 注意：需要 4GB+ 堆空间，运行时间较长（预计 10-30 分钟）。
