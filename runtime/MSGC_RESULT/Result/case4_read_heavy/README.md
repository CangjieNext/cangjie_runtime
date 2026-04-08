# Case 4: 读密集型模型 (访问:创建 = 10:1)

## 测试目标

构建一个**访问远大于创建**的场景（10:1 比例），测试 GC 在以下方面的表现：

- **写屏障开销**：读操作不触发写屏障，验证读密集场景下屏障开销是否可忽略
- **标记精度**：大量读操作是否对并发标记产生干扰
- **GC 触发频率**：低分配速率下 GC 应较少触发
- **吞吐量**：读密集型工作负载的纯计算吞吐

## 模型设计

1. **初始化阶段**：构建工作集（树、链表、HashMap、大对象数组）
2. **工作阶段**：每轮执行 10 次读/遍历操作，仅 1 次写/重建操作
   - 读操作：遍历树、计算校验和、查询 HashMap、读数组
   - 写操作：重建一棵树或刷新 HashMap 的一个 key batch

## 运行方式

```bash
source /path/to/cangjie/envsetup.sh
cjc benchmark.cj -o benchmark
MRT_REPORT=./runtime.log cjHeapSize=512mb ./benchmark
```
