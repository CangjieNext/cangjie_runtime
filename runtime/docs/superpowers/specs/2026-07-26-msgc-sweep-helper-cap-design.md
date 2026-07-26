# MSGC Sweep Helper 默认上限调整设计

## 目标

取消 MSGC Sweep Helper 默认固定为 8 的策略，使默认最大 Helper 数与
`ResolveMSMarkTotalThreads()` 解析出的 GC 总线程数一致：

```text
defaultMaxSweepHelpers = max(gcThreads - 1, 0)
```

这里减去 1 是因为 `gcThreads` 包含 GC main，GC thread pool 只包含剩余 Helper。

## 行为

`ComputeMSMaxSweepHelpers(gcThreads, configuredMaxHelpers)` 保持现有接口：

- `configuredMaxHelpers == 0`：返回 `gcThreads - 1`，不再额外限制为 8。
- `configuredMaxHelpers != 0`：返回
  `min(gcThreads - 1, configuredMaxHelpers)`，继续保留显式人工上限。
- `gcThreads <= 1`：返回 0。

例如：

| GC 总线程 | 显式 Sweep 上限 | 最大 Sweep Helper |
| ---: | ---: | ---: |
| 4 | 0 | 3 |
| 16 | 0 | 15 |
| 16 | 3 | 3 |
| 1 | 0 | 0 |

## 范围

本次只调整 Sweep Helper 默认上限及其对应契约：

- 修改 `src/Heap/MAllocator/MSParallelSweepPolicy.h`。
- 更新 `test/MSParallelSweepPolicyTest.cpp` 中受影响的期望值。
- 同步修正 Runtime GC 知识文档中“默认最多 8 个 Helper”的描述。

明确不修改：

- `ResolveMSMarkTotalThreads()`。
- Mark 或 Sweep pressure 计算。
- Helper 动态扩缩逻辑。
- GC thread pool 架构和 Mark/Sweep lease。
- 任何 Runtime 日志或配置参数。

## 验证约束

按用户要求，本次不执行构建、单元测试、Runtime 替换或 P128 benchmark。只更新实现、
契约测试源码和对应知识文档。
