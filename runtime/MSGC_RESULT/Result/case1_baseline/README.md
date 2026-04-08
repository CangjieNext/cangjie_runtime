# Case 1: 基准测试（无 Idle 窗口）

## 测试目标

使用原始 `gc_benchmark` 的完整功能测试(F1-F5)和性能测试(P1-P5)，但**移除末尾的 2 分钟 idle 窗口和周期性 GC 调用**。

用于快速获取 GC 功能正确性和分配吞吐基线数据，不含 idle 期间的 RSS 回落观测。

## 测试内容

| 用例 | 类型 | 说明 |
|------|------|------|
| F1 TreeIntegrity | 功能 | depth=16 完全二叉树 ×20 轮，校验 checksum |
| F2 Generational | 功能 | 长期 depth=18 树 + 短命 depth=14 ×200 轮 |
| F3 LargeObject | 功能 | 64KB~1MB 大对象 ×50 轮 checksum |
| F4 LinkedList | 功能 | 500 节点 ×8KB payload 链表 ×30 轮 |
| F5 HashMap | 功能 | 10K KV HashMap ×20 轮 |
| P1 SmallAlloc | 性能 | depth=16 树 ×50 轮吞吐 |
| P2 LargeAlloc | 性能 | 256KB ×200 轮吞吐 |
| P3 MixedWorkload | 性能 | 大+小+HashMap 混合 ×30 轮 |
| P4 LongLivedChurn | 性能 | 20 × 512KB 长期 + 短命翻搅 ×100 轮 |
| P5 Fragmentation | 性能 | 交替分配释放制造碎片 ×15 轮 |

## 运行方式

```bash
source /path/to/cangjie/envsetup.sh
cjc benchmark.cj -o benchmark
MRT_REPORT=./runtime.log cjHeapSize=256mb ./benchmark
```
