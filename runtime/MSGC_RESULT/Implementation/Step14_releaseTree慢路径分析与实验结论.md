# Step14 releaseTree 慢路径分析与实验结论

> 本文档聚焦当前 MSGC 中 releasedTree 路径为什么会慢，以及现阶段哪些实验结论可信、哪些结论需要在修正开关后重新验证。

---

## 1. 问题定义

当前 MSGC 的 region 获取路径里，`releasedTree` 承担“已归还物理页的空闲区间”的复用职责。
用户侧现象是：

1. 在部分 benchmark 中，关闭 `releasedTree` 复用后，执行时间明显下降。
2. O0 下 MSGC 相比 CopyGC 放大更明显，怀疑 `releasedTree` 是重要热点。
3. 但已有若干“`CJ_MS_TAKE_RELEASED_TREE=0` 后变快”的实验，需要重新核对其前提是否成立。

本文结论分两层：

1. **releasedTree 路径本身确实是一个重热点**，尤其在 O0 下。
2. **历史上的 `CJ_MS_TAKE_RELEASED_TREE=0` / `CJ_MS_BG_RECLAIM=0` 开关实现存在缺陷**，所以修复前的 A/B 数据不能直接当成最终结论；修复后需要单独复测并与旧数据分开看。

---

## 2. 代码路径

### 2.0 当前源码状态补充

本文早期分析中的“单棵 releasedTree + 单把 mutex”描述，适用于修复前的历史实现和当时的实验数据。当前源码中的 `FreeManager` 已经引入 Step15 方向的双 bank 雏形：

1. `producerBank_` 接收 `ReclaimRegionsReleased()` 释放后的 span。
2. `consumerBank_` 供 `TryTakeRegions()` 取用。
3. `GrowOneSegment()` 成功后会调用 `PublishProducerToConsumers(true)`，把 producer 中冻结的 spans 发布/合并到 consumer。
4. 当前仍只有一个 consumer bank，尚未实现 Step15 文档里建议的 consumer shard，因此 consumer 取用侧仍可能存在单 bank 锁竞争。

因此阅读本文时需要区分两种上下文：

1. **历史单树实现**：对应本文第 3 节部分旧实验和第 5 节原始根因拆解。
2. **当前双 bank 实现**：producer/consumer 已分离，但 consumer 侧仍是单树单锁，冷页复用和 merge/publish 成本仍需继续验证。

### 2.1 Mutator 取 region 的上层循环

`MSRegionManager::TakeRegions()` 的 releasedTree 路径在下面这个循环中优先执行：

```cpp
bool MSRegionManager::TakeRegions(RegionCount num, RegionIndex& idx, AllocSource* source,
								  MSTakeRegionTiming* timing)
{
	bool tryReleasedTree = GetReleasedTreeTakeEnabled();

	while (tryReleasedTree) {
		FreeManager::TryTakeTiming tryTakeTiming;
		FreeManager::TryTakeResult result = freeManager_.TryTakeRegions(num, rawIdx, &tryTakeTiming);
		...
		if (result == FreeManager::TryTakeResult::SUCCESS) {
			...
			*source = AllocSource::RELEASED;
			return true;
		}
		if (result == FreeManager::TryTakeResult::EMPTY) {
			...
			tryReleasedTree = false;
			continue;
		}
		...
		timing->releasedBusyRetries += 1;
		ScopedEnterSaferegion enterSaferegion(true);
	}

	// 后续才走 bump / grow
}
```

这一层的三个特征：

1. `releasedTree` 默认先于 bump/grow 尝试。
2. 底层返回 `BUSY` 时不会立即放弃，而是不断重试。
3. `releasedBusyRetries`、`releasedPathUs`、`lockWaitUs` 都是在这里累计的。

### 2.2 FreeManager 的全局 try_lock

`FreeManager::TryTakeRegions()` 使用单个全局 `mutex_` 保护 releasedTree：

```cpp
FreeManager::TryTakeResult FreeManager::TryTakeRegions(RegionCount num, RegionIndex& idx,
													   TryTakeTiming* timing)
{
	if (!mutex_.try_lock()) {
		return TryTakeResult::BUSY;
	}
	std::lock_guard<std::mutex> lg(mutex_, std::adopt_lock);

	if (!GetBackend().TakeUnits(unitCount, unitIdx)) {
		...
		return TryTakeResult::EMPTY;
	}

	...
	return TryTakeResult::SUCCESS;
}
```

也就是说：

1. releasedTree 是**全局单锁**。
2. 失败不是阻塞等待，而是 `try_lock -> BUSY -> 上层重试`。
3. 所有树查找和节点调整都发生在这把锁内部。

### 2.3 CartesianTree 的 `TakeUnits` 不是常数开销

当前 backend 是 `CartesianTreeBackend`，实际落到 `CartesianTree::TakeUnitsImpl()`：

```cpp
bool TakeUnitsImpl(Count num, Index& idx, bool refreshRegionInfo)
{
	ForwardIterator it(*this);
	Node** nodePtr = it.Next();
	...
	while ((nextNodePtr = it.Next()) != nullptr) {
		Node* nextNode = *nextNodePtr;
		if (nextNode != nullptr && nextNode->GetCount() < num) {
			break;
		}
		nodePtr = nextNodePtr;
	}

	node = *nodePtr;
	idx = node->GetIndex();
	node->UpdateNode(idx + num, count - num, refreshRegionInfo);
	...
	if (node->GetCount() == 0) {
		RemoveZeroNode(*nodePtr);
	} else {
		LowerNonZeroNode(*nodePtr);
	}
	return true;
}
```

这一层不是简单“拿一个指针就走”，而是：

1. 通过 `ForwardIterator` 向下扫候选节点。
2. 选中节点后还要更新 `(idx, count)`。
3. 如果节点未空，要继续做 `LowerNonZeroNode()` 维护 heap property。
4. 这些动作全都在 `FreeManager::mutex_` 持锁期间完成。

### 2.4 releasedTree 里的页已经被归还 OS

小对象 empty region 和 large region 最终都可能调用 `ReclaimRegionsReleased()`：

```cpp
void FreeManager::ReclaimRegionsReleased(RegionIndex idx, RegionCount num)
{
	ReleaseUnits(RegionToUnitIdx(idx), RegionCountToUnitCount(num));
	ScopedEnterSaferegion enterSaferegion(true);
	InsertRegions(idx, num);
}
```

其中 `ReleaseUnits()` 在 macOS 上会重新 `mmap(MAP_FIXED)`，在其他 Unix 上走 `madvise(..., MADV_DONTNEED)`。

这意味着 releasedTree 不是“普通空闲缓存”，而是**已经归还物理页的冷区间**。下次重新分配并真正写入对象时，会产生 OS fault-in / 零页重新建立成本。

---

## 3. 实验现象

### 3.0 本文引用的测试对象与运行方式

本节所有结论都不是来自“泛泛 benchmark”，而是来自 `case_mid/case` 目录下的 **P1-Single 顺序宽扇出大存活集用例**。

对应文件/二进制与特征如下：

| 项目 | 内容 |
|------|------|
| 用例源码 | `MSGC_RESULT/Result/case_mid/case/bench_p1_single.cj` |
| 用例输出 banner | `P1-Single: 2000 WideGroups x 1000 BigLeaf (~3.3GB), sequential` |
| O2 历史 gate 实验二进制 | `./bench_p1_single_gate` |
| O0 复测二进制 | `./bench_p1_single_msgc_O0`、`./bench_p1_single_msgc_O0_released_off`、`./bench_p1_single_copygc_O0` |
| 统一运行参数 | `cjHeapSize=6gb cjProcessorNum=6` |

其中本文涉及两类对比：

1. **O2 gate 历史实验**：使用 `bench_p1_single_gate`，比较 `CJ_MS_TAKE_RELEASED_TREE` 开/关名义上的差异。
2. **O0 当前复测**：使用 `bench_p1_single.cj` 重新编译，比较 MSGC / CopyGC，并补充一组“名义上 releaseTree off”的 MSGC 数据。

需要再次强调：

1. `CJ_MS_TAKE_RELEASED_TREE=0` 在当前代码里并不能真正关闭 releasedTree。
2. 因此下面表格中的 “releaseTree off” 应理解为 **名义 off / 实验意图 off**，不是严格意义上的真实 off。

### 3.1 O2 条件下，GC off 时 releaseTree 影响很小

对应测试：`case_mid/case` 下的 P1-Single gate 版本，运行二进制为 `./bench_p1_single_gate`。

已有一组历史对照：

| 场景 | time |
|------|------|
| `bg_off_gc_off.time` | 0.662 s |
| `take_released_off_gc_off.time` | 0.684 s |

这一组只差 22 ms，说明**没有 GC reclaim / reuse 循环时，releaseTree 不是主要矛盾**。

为了避免歧义，这两组对应关系如下：

| 文件 | 语义 | 关键环境变量 |
|------|------|--------------|
| `bg_off_gc_off.time` | baseline | `CJ_MS_BG_RECLAIM=0 CJ_GC_DISABLE=1` |
| `take_released_off_gc_off.time` | 名义 releaseTree off | `CJ_MS_BG_RECLAIM=1 CJ_MS_TAKE_RELEASED_TREE=0 CJ_GC_DISABLE=1` |

也就是说，在 **无 GC** 条件下，即便切到这组“名义上关闭 releaseTree”的跑法，时间也基本不变。

### 3.2 O2 条件下，GC on 的历史“releaseTree off 更快”结论需要谨慎

对应测试仍然是 `./bench_p1_single_gate`，但这次开启 GC。

已有另一组历史结果：

| 场景 | time |
|------|------|
| `bg_off_gc_on_fixed.time` | 1.425 s |
| `take_released_off_gc_on.time` | 0.806 s |

表面上看，关闭 releasedTree 后快了 0.619 s，约 43.4%。

但这组结论现在不能直接当成最终证据，因为第 4 节会说明：

**当前 `CJ_MS_TAKE_RELEASED_TREE=0` 在代码里实际上无法关闭该路径。**

所以这组历史结果只能作为“怀疑 releasedTree 很关键”的线索，不能直接当成严格 A/B 结论。

这两组对应的实验意图如下：

| 文件 | 语义 | 关键环境变量 |
|------|------|--------------|
| `bg_off_gc_on_fixed.time` | baseline GC on | `CJ_MS_BG_RECLAIM=0` |
| `take_released_off_gc_on.time` | 名义 releaseTree off, GC on | `CJ_MS_BG_RECLAIM=1 CJ_MS_TAKE_RELEASED_TREE=0` |

按历史结果表面计算：

1. 从 `1.425 s` 降到 `0.806 s`。
2. 绝对下降 `0.619 s`。
3. 相对下降约 `43.4%`。

但由于 gate 失效，这里只能记录为：

1. **该用例在这组实验配置下出现了显著时间差。**
2. **不能据此直接宣称“真实关闭 releaseTree 后一定提升 43.4%”。**

### 3.3 O0 下 releasedTree 相关计数极重

本节前两小节记录的是 **修 gate 之前** 的历史 O0 数据，用于解释当时为什么会怀疑 releasedTree 是主要热点；第 3.3.3 小节再补充 **修 gate 之后** 的复测结果。

本次对 `bench_p1_single.cj` 的 O0 复测，实际跑了三组：

| 运行对象 | SDK / 配置 | 程序内时间 | 外层 `time` |
|---------|------------|------------|-------------|
| `bench_p1_single_copygc_O0` | CopyGC, O0 | 10396 ms | 10.821 s |
| `bench_p1_single_msgc_O0` | MSGC 默认, O0 | 30528 ms | 32.075 s |
| `bench_p1_single_msgc_O0_released_off` | MSGC + `CJ_MS_TAKE_RELEASED_TREE=0`, O0 | 28360 ms | 29.004 s |

其中第三组仍然只是 **名义 releaseTree off**，因为当前 gate 失效。

#### 3.3.1 O0 下默认 MSGC 的 releasedTree timing

`bench_p1_single_msgc_O0` 的关键计数如下：

| 指标 | 数值 |
|------|------|
| 程序内时间 | 30528 ms |
| 外层 `time` | 32.075 s |
| slow_alloc `released_path_total_us` | 19,139,065 us |
| slow_alloc `success_total_us` | 16,991,780 us |
| slow_alloc `success_count` | 261,267 |
| slow_alloc `busy_retries` | 188,196,340 |
| slow_alloc `lock_wait_total_us` | 4,659,365 us |
| background_fill `released_path_total_us` | 4,794,403 us |
| background_fill `success_total_us` | 2,762,065 us |
| background_fill `success_count` | 28,849 |
| background_fill `busy_retries` | 255,386,237 |
| background_fill `lock_wait_total_us` | 4,598,034 us |

这些数值至少说明三件事：

1. releasedTree 路径确实被大量命中。
2. 锁竞争非常重，`busy_retries` 已经到亿级。
3. 即使只看 allocator 计数，released path 也已经占了非常大的累计时间。

#### 3.3.2 O0 下“名义 releaseTree off”对比数据

`bench_p1_single_msgc_O0_released_off` 的关键计数如下：

| 指标 | 数值 |
|------|------|
| 程序内时间 | 28360 ms |
| 外层 `time` | 29.004 s |
| slow_alloc `released_path_total_us` | 16,926,432 us |
| slow_alloc `success_total_us` | 15,002,045 us |
| slow_alloc `success_count` | 232,912 |
| slow_alloc `busy_retries` | 167,375,658 |
| slow_alloc `lock_wait_total_us` | 4,114,513 us |
| background_fill `released_path_total_us` | 4,089,691 us |
| background_fill `success_total_us` | 2,440,002 us |
| background_fill `success_count` | 24,867 |
| background_fill `busy_retries` | 206,622,874 |
| background_fill `lock_wait_total_us` | 3,868,751 us |

把默认 MSGC 和“名义 off”并排放在一起，可以看到：

| 指标 | 默认 MSGC O0 | 名义 releaseTree off O0 | 差值 |
|------|--------------|-------------------------|------|
| 程序内时间 | 30528 ms | 28360 ms | -2168 ms |
| 外层 `time` | 32.075 s | 29.004 s | -3.071 s |
| slow_alloc `released_path_total_us` | 19.139 s | 16.926 s | -2.213 s |
| slow_alloc `success_total_us` | 16.992 s | 15.002 s | -1.990 s |
| slow_alloc `busy_retries` | 188,196,340 | 167,375,658 | -20,820,682 |
| slow_alloc `lock_wait_total_us` | 4.659 s | 4.114 s | -0.545 s |

这组数据的意义是：

1. 即使在当前 gate 失效的前提下，所谓“off”跑法也表现出更低的 released-path 累计成本。
2. 这进一步说明 releasedTree 相关路径确实和慢问题强相关。
3. 但它仍然**不能**被解释成“已经严格关闭 releasedTree 后的净收益”。

#### 3.3.3 修 gate 后的 O0 复测

在修复 `CJ_MS_TAKE_RELEASED_TREE=0` 与 `CJ_MS_BG_RECLAIM=0` 的布尔解析后，重新替换 MSGC runtime，并对同一个 `bench_p1_single.cj` O0 二进制复测，得到如下结果：

| 运行对象 | 关键环境变量 | 程序内时间 | 外层 `time` |
|---------|--------------|------------|-------------|
| `bench_p1_single_msgc_O0_gate_fixed` | `CJ_MS_BG_RECLAIM=0` | 10070 ms | 10.193 s |
| `bench_p1_single_msgc_O0_gate_fixed` | `CJ_MS_BG_RECLAIM=0 CJ_MS_TAKE_RELEASED_TREE=0` | 10243 ms | 10.892 s |
| `bench_p1_single_msgc_O0_gate_fixed` | `CJ_MS_BG_RECLAIM=0 CJ_MS_TAKE_RELEASED_TREE=0 CJ_GC_DISABLE=1` | 11413 ms | 12.019 s |

其中最关键的是第二组：

| 指标 | 数值 |
|------|------|
| 程序内时间 | 10243 ms |
| 外层 `time` | 10.892 s |
| slow_alloc `released_path_total_us` | 0 us |
| slow_alloc `busy_retries` | 0 |
| slow_alloc `success_count` | 0 |
| background_fill `released_path_total_us` | 0 us |
| background_fill `busy_retries` | 0 |
| background_fill `success_count` | 0 |

这说明修复后的 `CJ_MS_TAKE_RELEASED_TREE=0` 已经**真正关闭**了 releasedTree 获取路径，不再只是“名义 off”。

第一组作为同配置下的 releaseTree-on 对照，其关键计数是：

| 指标 | 数值 |
|------|------|
| 程序内时间 | 10070 ms |
| 外层 `time` | 10.193 s |
| slow_alloc `released_path_total_us` | 17,727 us |
| slow_alloc `busy_retries` | 1,005 |
| slow_alloc `success_count` | 1 |
| background_fill `released_path_total_us` | 1,042 us |
| background_fill `busy_retries` | 16 |
| background_fill `success_count` | 0 |

从这组修复后复测可以得到三个新的结论：

1. 真正修好 gate 后，旧的 29 到 32 秒级 O0 结果没有复现，说明历史数据明显受到了错误 gate 的污染。
2. 在 `CJ_MS_BG_RECLAIM=0` 真正生效的前提下，这个用例即使 releaseTree 开着，也几乎不再命中 releasedTree 慢路径。
3. 对这个用例的这组单次样本而言，真实的 releaseTree on/off 差异已经缩小到 10 秒级内的小幅波动，远不是修复前看到的 20 秒级差距。

---

## 4. 历史关键发现：`CJ_MS_TAKE_RELEASED_TREE=0` 与 `CJ_MS_BG_RECLAIM=0` 曾失效，现已修复

修复前，相关布尔开关读取逻辑是：

```cpp
size_t ReadEnvCount(const char* name, size_t fallback)
{
	const char* env = std::getenv(name);
	if (env == nullptr) {
		return fallback;
	}
	size_t value = CString::ParsePosNumFromEnv(env);
	return value == 0 ? fallback : value;
}

bool GetReleasedTreeTakeEnabled()
{
	static const bool value = ReadEnvCount("CJ_MS_TAKE_RELEASED_TREE", 1) != 0;
	return value;
}
```

问题在于：

1. `ParsePosNumFromEnv("0")` 会得到 `0`。
2. `ReadEnvCount()` 把 `0` 直接当成“无效值”，返回 fallback。
3. `GetReleasedTreeTakeEnabled()` 的 fallback 是 `1`。

所以在当前实现下：

```bash
CJ_MS_TAKE_RELEASED_TREE=0
```

最后仍然会得到 `true`。

`CJ_MS_BG_RECLAIM=0` 也有同样的问题，因为它同样复用了 `ReadEnvCount(..., 1) != 0` 这条路径。

这意味着：

1. 当前树上的“releaseTree off”实验不能被默认视为真的关闭了 releaseTree。
2. 历史上的“bg off”实验也不能默认视为真的关闭了 background reclaim。
3. 在 gate 修复之前，对 releasedTree 的分析应优先基于 allocator timing、代码路径和锁竞争计数，而不是仅基于 env gate 的 A/B time。

本次已在 runtime 中改为显式布尔解析：缺省值保持原语义，但 `0/false` 会真正关闭对应功能。因此第 3.3.3 小节的复测结果，才是当前代码状态下可直接使用的新基线。

---

## 5. 根因拆解

### 5.1 第一层：全局单锁 + try_lock 重试

`FreeManager::TryTakeRegions()` 使用一把全局 `mutex_`；失败后不是阻塞，而是返回 `BUSY`，上层继续 retry。

结果是：

1. contention 高时，大量时间不在“真正树操作”上，而是在 try-lock 失败和重试上。
2. `releasedBusyRetries` 达到亿级后，哪怕单次 retry 很便宜，总体也会被放大成大头。

### 5.2 第二层：锁内树操作本身不轻

成功拿到锁以后，还要在 `CartesianTree::TakeUnitsImpl()` 里：

1. 扫候选节点。
2. 修改节点 `(idx, count)`。
3. 下降/旋转维护 treap heap property。

这解释了为什么 `success_avg_us` 在 O0 下能到 65 us / 95 us，而不是一个很小的常数级别。

### 5.3 第三层：releasedTree 复用的是“冷页”，不是“热空闲缓存”

`ReclaimRegionsReleased()` 在插回 tree 之前，已经先做了 `ReleaseUnits()`。

这意味着 releasedTree 的语义是：

1. 地址还在。
2. 物理页已经被 OS 回收或置为懒重新建立。

因此 releasedTree 成功分配后，真正的对象写入阶段还会承担 fault-in / 零页建立成本。这个成本**不一定完全体现在 `released_path_total_us` 里**，因为 timing 只覆盖“取 region”这段，而不是后续 mutator 对页的第一次真正触碰。

也就是说，当前 releasedTree 的慢，不只是“树查找慢”，还是“冷页重新热身慢”。

### 5.4 第四层：策略顺序有问题

`TakeRegions()` 当前默认先试 releasedTree，再走 bump/grow。

这在 steady-state 小对象分配里有两个问题：

1. 如果 releasedTree 命中率低，会反复支付 try-lock + EMPTY + fallback 成本。
2. 如果 releasedTree 命中率高，又会把 mutator 引到 cold page fault-in 路径。

换句话说，**无论命中还是不命中，都可能慢**：

1. 不命中：慢在锁竞争和空转重试。
2. 命中：慢在树操作 + OS 页重新建立。

---

## 6. 结论与后续动作

### 6.1 当前结论

可以先确认的结论是：

1. `releasedTree` 路径是 MSGC 当前 allocator 热路径中的一个真实热点。
2. 慢点不只一个，而是“全局单锁 + try_lock 重试 + 锁内 treap 操作 + cold page fault-in”叠加。
3. 历史上的 `CJ_MS_TAKE_RELEASED_TREE=0` 与 `CJ_MS_BG_RECLAIM=0` 开关实现有 bug，导致修复前的 off/on A/B 结果需要重新审视。
4. 修复 gate 并重跑后，`bench_p1_single.cj` 这条 O0 实验线已经回到约 10 到 12 秒；在 `CJ_MS_BG_RECLAIM=0` 真正生效时，releaseTree on/off 的单次差异已经很小。

### 6.2 建议的下一步顺序

建议不要直接先微调 `CartesianTree::TakeUnitsImpl()`，而按下面顺序推进：

1. **按修复后的 runtime 重新做多次 A/B**：同一 benchmark、同一 binary、同一 runtime，分别做 releaseTree on/off、bg reclaim on/off 的重复样本，不再混用修复前数据。
2. **优先确认 bg reclaim 的真实贡献**：从本次复测看，旧的慢结果很可能更多来自 `CJ_MS_BG_RECLAIM=0` 实际未生效，而不是 releaseTree 单独造成。
3. **如果修复后的多次 A/B 仍显示 releasedTree 明显更慢，再分层优化**：
   - 策略层：steady-state 优先走 empty/unswept/bump，把 releasedTree 降为冷备来源。
   - 并发层：减少 `try_lock -> BUSY -> retry` 空转。
   - 数据结构层：如果 releasedTree 仍然必须走热路径，再考虑更适合“小块频繁取”的 backend，而不是直接沿用当前 CartesianTree 遍历策略。
4. **把 releasedTree 视为冷池而非默认供给池**：既然它对应的是已释放物理页，那么默认优先级就不应高于热缓存或 bump。

### 6.3 对后续设计文档的影响

本 Step14 的定位是“问题诊断与前提校正”。

后续如果继续推进优化，建议至少拆成两条线：

1. **Step14.x 修 gate + 重跑实验**
2. **Step15 策略改造：releasedTree 降级为冷备来源 / 热路径优先空缓存与 bump**

在 gate 修好之前，不建议再对“releasedTree off 的速度收益百分比”下最终结论。
