# LOS 落地执行方案（可逐步实施）

## 0. 目标与边界

目标：在不破坏当前 `WCollector + RegionSpace` 主体架构下，新增可控的 LOS（Large Object Space）能力，支持：

1. 大对象优先进入 LOS；
2. LOS 不足时可降级到现有大对象路径；
3. GC 后可尝试把“主堆大对象”回迁 LOS；
4. 全流程可观测、可开关、可回退。

边界：

- 第一阶段不做并发整理，仅做 STW 整理能力预留；
- 第一阶段不改变小对象路径；
- 通过开关保证可 100% 回退到当前行为。

---

## 1. 里程碑与顺序（建议严格按序）

### M1：LOS 骨架与开关（只搭框架，不切流量）

**改动点**

- 新增模块：`src/Heap/Allocator/LargeObjectSpace.h/.cpp`
- 新增配置项（环境变量即可）：
  - `cjEnableLOS`（0/1，默认 0）
  - `cjLOSInitialRatio`（默认 0.1）
  - `cjLOSReserveRatio`（默认 0.2）
- 在 `RegionManager` 增加 LOS 成员与初始化入口（仅初始化，不参与分配）。

**验收标准**

- `cjEnableLOS=0/1` 均可正常构建与启动。
- `cjEnableLOS=0` 时行为完全等同当前版本（日志和统计无变化）。

**回退策略**

- 编译期和运行期开关双保险：关闭 `cjEnableLOS` 即回退。

---

### M2：大对象分配分流（LOS 优先 + 降级）

**改动点**

- 在 `RegionSpace::TryAllocateOnce()` 的大对象分支加入：
  - 若 `cjEnableLOS=1`，先 `LOS.AllocLarge(size)`；
  - 失败后走现有 `RegionManager::AllocLarge(size)`（降级）。
- 为对象头或 region 元数据增加“来源标记”：`IN_LOS / IN_MAIN_LARGE`。

**验收标准**

- 人工构造大对象压力下，可见 LOS 命中率统计。
- LOS 满时分配可稳定降级，无崩溃、无死锁。

**回退策略**

- 保留原路径完整代码；LOS 分配异常时直接降级。

---

### M3：LOS 回收（标记-清扫）与统计

**改动点**

- 在 `WCollector::PostTrace()` 后增加 `CollectLOSGarbage()`。
- LOS 对象先按“标记-清扫”回收，不做搬迁。
- 增加统计：
  - `losSpaceSize`
  - `losGarbageSize`
  - `losAllocFailCount`
  - `losFallbackCount`
- 日志与 `TRACE_COUNT` 接入。

**验收标准**

- GC 后 LOS 垃圾可回收。
- LOS 统计随压力变化符合预期。

**回退策略**

- 关闭 LOS 回收分支即不影响主堆回收链路。

---

### M4：GC 后回迁（主堆大对象 -> LOS）

**改动点**

- 在 `PostTrace` 或 `Forward` 之后新增“回迁窗口”：
  - 遍历可迁移的大对象（仅无 raw-pointer、无特殊 pin 条件）；
  - 尝试在 LOS 分配并复制；
  - 成功后更新引用（复用现有 fix/update 基础设施）。
- 控制每轮迁移预算，避免长停顿。

**验收标准**

- 可观测到 `migrate_in_count` 增长。
- 回迁后对象可达性正确，功能测试稳定。

**回退策略**

- 回迁失败保持原地对象，不影响正确性。

---

### M5：STW 整理（首次可落地版）

**改动点**

- 增加 LOS 整理触发条件：
  - 分配连续空间失败次数超阈值；
  - 最大连续块低于请求大小；
  - 碎片率超阈值。
- 实现 STW 原地整理（先做最小可行版本）：
  - 关闭 LOS 新分配；
  - 线性紧凑存活对象；
  - 修正引用；
  - 恢复分配。

**验收标准**

- 整理前后对象一致性正确。
- 大对象连续分配成功率明显提升。

**回退策略**

- 整理触发开关可关闭；失败则保持现状并降级分配。

---

### M6：扩缩容（LOS 与预留区动态调度）

**改动点**

- 实现 `MAIN | RESERVE | LOS` 的弹性边界：
  - LOS 扩容：从 RESERVE 借；
  - LOS 缩容：整理后归还 RESERVE；
  - MAIN 紧张且 RESERVE 空时，可向 LOS 申请回收/缩容。
- 增加保护阈值，避免频繁抖动（hysteresis）。

**验收标准**

- 长压测下无明显抖动，吞吐与内存稳定。

**回退策略**

- 动态扩缩容可单独关闭，LOS 退化为固定区。

---

## 2. 每阶段执行模板（你让我开始执行时按这个来）

每次只做一个 Mx：

1. 改代码（只改该阶段范围）；
2. 编译验证：`python3 build.py build`；
3. 若有编译问题，先修到通过；
4. 输出变更清单 + 风险点 + 下一步。

---

## 3. 代码触点清单（首批）

- 分配入口：`src/Heap/Allocator/RegionSpace.cpp`
- 大对象主路径：`src/Heap/Allocator/RegionManager.h` / `src/Heap/Allocator/RegionManager.cpp`
- GC 阶段接入：`src/Heap/WCollector/WCollector.cpp` / `src/Heap/WCollector/WCollector.h`
- 统计结构：`src/Heap/Collector/GcStats.h` / `src/Heap/Collector/GcStats.cpp`
- 参数初始化：`src/CangjieRuntime.cpp`（必要时）

---

## 4. 风险与控制

- **引用修正风险（最高）**：回迁与整理先做 STW，减少并发复杂度。
- **性能回退风险**：每轮迁移/整理设置预算；默认关闭高级功能。
- **兼容性风险**：保留降级路径，所有新能力可通过开关关闭。

---

## 5. 推荐启动方式

建议你先让我从 **M1** 开始执行（骨架 + 开关）。

原因：

- 对现有行为影响最小；
- 可以最早把“可回退机制”和“可观测点”建起来；
- 为后续 M2~M6 提供稳定底座。
