# Case 5: 混合对象模型 (大对象 + 锁对象 + 普通对象)

## 测试目标

构建一个包含**多种对象类型**的混合场景，同时保持 50% 存活率和 50% 访问/创建比例，测试 GC 在以下方面的表现：

- **对象类型多样性**：大对象（>16KB）、带锁同步对象、普通小对象在同一堆中的共存
- **分配器路径覆盖**：不同大小对象走不同分配路径（TLAB / Region / HugeRegion）
- **并发标记复杂度**：对象图包含多种引用模式（树形、链表、HashMap、数组）
- **存活率 50%**：一半对象持久存活，一半临时死亡
- **访问/创建平衡**：一半时间在访问已有对象，一半时间在创建新对象

## 对象类型说明

| 类型 | 大小范围 | 分配路径 | 说明 |
|------|----------|----------|------|
| 普通小对象 | 32-256 bytes | TLAB | TreeNode, 小 ArrayList |
| 中等对象 | 1-16KB | Region | PayloadNode, HashMap |
| 大对象 | 64KB-8MB | HugeRegion | LargeObject |
| 锁保护对象 | 任意 | 含 Monitor | 通过原子计数器模拟并发访问模式 |

## 运行方式

```bash
source /path/to/cangjie/envsetup.sh
cjc benchmark.cj -o benchmark
MRT_REPORT=./runtime.log cjHeapSize=1gb ./benchmark
```
