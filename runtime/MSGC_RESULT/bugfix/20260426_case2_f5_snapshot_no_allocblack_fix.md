# case2 F5_HashMap 正式修复记录：MarkSetup snapshot + no alloc-black

## 1. 问题概述

- 问题场景：MSGC 下运行 `MSGC_RESULT/Result/case2_50x_pressure/benchmark.cj`，在 `seed5.env + cjHeapSize=4gb` 条件下，`F5_HashMap` 稳定失败。
- 失败现象：`HashMap<Int64, ArrayList<Int64>>` 中取出的 `ArrayList` 被破坏为 size=0，最终抛出 `IndexOutOfBoundsException: index: 0, size: 0`。
- 旧观察：把 `MSBitmapArena::kBitmapSlotBytes` 从 `*2` 改回 `*3` 可以恢复通过，但后续探针证明根因并不是“明确存在 direct third-slot 访问”。

## 2. 根因判断

结合 sweep 诊断、`CJ_MS_FORCE_ENQUEUE_MARKED=1` 验证 patch 和 CopyGC 行为对比，当前问题可以归纳为两点叠加：

1. MSGC 旧实现把本轮 small-region sweep snapshot 放在 `MarkTermination()` 之后。
   - 这意味着 GC-active 期间 mutator 正在使用的 TL/fresh region，等 `FlushAllAllocBuffers()` 归还后，会被立即卷入本轮 unswept backlog。
2. 新对象路径仍带 alloc-black 语义。
   - 小对象分配阶段会调用 `MarkNewObject()`。
   - large object 注册阶段直接设置 `FLAG_MARKED`。
   - 对象因此可能出现“已经 marked，但并没有被真正 trace/scanned”的窗口。

旧协议把 `marked` 近似当成“已经安全处理过”，而不是“已经发现但可能尚未扫描”。
在 case2/F5 这种容器图里，这会表现为：

- 某些对象在 GC 周期内被提早盖上 mark bit。
- 与此同时，它们所在 region 又在本轮结束前被纳入 sweep 候选集合。
- 闭包没有完整收敛到对象子图，最终部分仍可达子对象在 `SweepRegion()` 中被清零。

这也是为什么仅开启 `CJ_MS_FORCE_ENQUEUE_MARKED=1` 就能让 2-slot 版本通过：它暴露了“marked-but-not-scanned”缺口，而不是物理第三槽位本身。

## 3. 修复方案

本轮修复采用“CopyGC 风格的 collection-set 边界思路”，但不新增新的 per-object 标记位，只复用现有 `globalSweepGen_`：

1. 将本轮 sweep snapshot 前移到 MarkSetup。
   - 在 `PreEnumRoots()` 中先停掉 sweep、清空 mark bitmap、重分类残留 unswept，再调用 `PrepareForSweep()`。
   - 这样本轮 central-pool 的 sweep 候选集合在真正开始 root 枚举前就冻结。
2. GC-active 期间归还的 TL/fresh region 不进入本轮 unswept backlog。
   - `ReturnPartialRegion()` / `ReturnFullRegion()` 统一盖当前 `globalSweepGen`，放回 swept 队列。
   - 下一轮 `PrepareForSweep()` 再把它们翻入 unswept。
3. 移除 new-time alloc-black。
   - 小对象分配路径不再调用 `MarkNewObject()`。
   - `MarkSweepCollector::MarkNewObject()` 变为 no-op。
   - large object 注册不再 `AtomicSetMarked()`，而是给 large region 设置当前 sweep generation。
4. large object sweep 与 small region 语义保持一致。
   - `SweepLargeRegions()` 跳过“当前 generation”的 large region。
   - 只有旧 generation 且未标记的大对象才会在本轮回收。

一句话总结当前修复语义：

```text
本轮回收对象集合在 MarkSetup 冻结；GC 开始后新分配/新归还 region 统一逃过本轮 sweep；
对象本身不再因为 new 而直接标黑。
```

## 4. 关键 patch 摘要

下面只保留本次修复最关键的 patch 片段，不展开无关上下文。

### 4.1 在 MarkSetup 冻结本轮 sweep snapshot

文件：`src/Heap/MCollector/MarkSweepCollector.cpp`

```diff
 void MarkSweepCollector::PreEnumRoots()
 {
     ...
     if (centralPool != nullptr) {
-        centralPool->StopAndWaitForAllSweep();
-        centralPool->ClearAllMarkBitmaps();
-        centralPool->ReclassifyUnsweptToSwept();
+        centralPool->StopAndWaitForAllSweep();
+        centralPool->ClearAllMarkBitmaps();
+        centralPool->ReclassifyUnsweptToSwept();
+        centralPool->PrepareForSweep();
     }
     ...
 }
```

这一步把“本轮允许被 sweep 的 central-pool region 集合”提前冻结，而不是等到 `MarkTermination()` 后再冻结。

### 4.2 Flush TL region 后只重新开启 lazy sweep，不再做本轮快照

文件：`src/Heap/MCollector/MarkSweepCollector.cpp`

```diff
     if (centralPool != nullptr) {
         // Return active TL/cache regions after the MarkSetup sweep snapshot.
         // CentralRegionPool stamps them with the current sweep generation so
         // they stay out of this cycle's unswept backlog.
         centralPool->FlushAllAllocBuffers();
-        centralPool->PrepareForSweep();
         centralPool->ReenableLazySweep();
     }
```

这保证了 GC-active 期间 mutator 用过的 active/cache region，即使在 `MarkTermination()` 被归还，也不会被本轮 sweep 立刻拿去清扫。

### 4.3 归还的 TL/fresh region 只用当前 generation 表达“逃过本轮”

文件：`src/Heap/MAllocator/MSCentralRegionPool.cpp`

```diff
 void CentralRegionPool::ReturnPartialRegion(MSRegionInfo* region, uint8_t cls)
 {
     SizeClassBucket& bucket = buckets_[cls];
-    bucket.partialUnswept.Push(region);
+    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
+    region->SetSweepGen(sg);
+    bucket.partialSwept.Push(region);
 }

 void CentralRegionPool::ReturnFullRegion(MSRegionInfo* region, uint8_t cls)
 {
     SizeClassBucket& bucket = buckets_[cls];
     std::lock_guard<std::mutex> lg(bucket.lock);
     region->regionType = MSRegionType::MS_REGION_FULL;
-    bucket.fullUnswept.Prepend(region);
+    uint32_t sg = globalSweepGen_.load(std::memory_order_acquire);
+    region->SetSweepGen(sg);
+    bucket.fullSwept.Prepend(region);
 }
```

没有新增“本轮豁免”标记位，只是把“当前 generation”的 region 留在 swept 队列，等下一轮 `PrepareForSweep()` 再翻进 unswept。

### 4.4 `PrepareForSweep()` 的语义改成“翻转当前 snapshot”

文件：`src/Heap/MAllocator/MSCentralRegionPool.cpp`

```diff
 void CentralRegionPool::PrepareForSweep()
 {
     sweepStats_.Reset();

-    // old: called after marking completes
+    // new: MarkSetup freezes this cycle's central-pool sweep candidates
     for (uint8_t cls = 0; cls < kNumSizeClasses; cls++) {
         SizeClassBucket& bucket = buckets_[cls];
         std::lock_guard<std::mutex> lg(bucket.lock);
         bucket.partialSwept.StealChainTo(bucket.partialUnswept);
         bucket.fullSwept.Swap(bucket.fullUnswept);
     }

     globalSweepGen_.fetch_add(2, std::memory_order_release);
     sweepDone_.store(false, std::memory_order_release);
 }
```

### 4.5 小对象移除 alloc-black

文件：`src/Heap/MAllocator/MSAllocBuffer.cpp`

```diff
     if (UNLIKELY(addr != 0)) {
         Mutator* mutator = Mutator::GetMutator();
         GCPhase phase = mutator->GetMutatorPhase();
         if (UNLIKELY(IsMSMarkingPhase(phase))) {
-            Heap::GetHeap().GetCollector().MarkNewObject(reinterpret_cast<BaseObject*>(addr));
             gcAssistBytes_ += size;
             ...
         }
     }
```

文件：`src/Heap/MCollector/MarkSweepCollector.cpp`

```diff
-// MarkNewObject: mark newly allocated objects during marking phases
+// MarkNewObject: no alloc-black in Mark-Sweep snapshot mode
 void MarkSweepCollector::MarkNewObject(BaseObject* obj)
 {
-    ... old mark-at-alloc logic ...
+    (void)obj;
 }
```

### 4.6 large object 也移除 alloc-black，并用 generation 保护本轮不被 sweep

文件：`src/Heap/MAllocator/MSRegionSpace.cpp`

```diff
 void RegionSpaceMS::RegisterLargeObjectMS(MAddress addr, size_t regionBytes)
 {
     ...
     baseInfo->InitForLargeObject(start, end);
     baseInfo->reserved = static_cast<uint64_t>(baseIdx);
-    baseInfo->AtomicSetMarked();
-    baseInfo->AtomicSetScanned();
+    baseInfo->SetSweepGen(CentralRegionPool::GetGlobalSweepGen());

     for (size_t i = 1; i < spanCount; i++) {
         MSRegionInfo* spanInfo = MSRegionInfoArray::GetRegionInfoByIdx(baseIdx + i);
         ...
-        spanInfo->flags_ = FLAG_MARKED;
+        spanInfo->flags_ = 0;
+        spanInfo->SetSweepGen(baseInfo->GetSweepGen());
         spanInfo->reserved = static_cast<uint64_t>(baseIdx);
     }
 }
```

文件：`src/Heap/MAllocator/MSCentralRegionPool.cpp`

```diff
 MSLargeSweepStats CentralRegionPool::SweepLargeRegions()
 {
     ...
     largeRegionList_.ForEach([&toRemove, sg](MSRegionInfo* region) {
+        if (region->GetSweepGen() == sg) {
+            return;
+        }
         if (!region->IsMarked()) {
             toRemove.push_back(region);
             return;
         }
+        region->SetSweepGen(sg);
     });
     ...
 }
```

## 5. 验证结果

### 5.1 构建验证

- 命令：`python3 build.py build --gc-mode marksweep`
- 结果：构建成功，`Build and preinstall completed successfully.`

### 5.2 case2 正式回归

- 结果目录：`MSGC_RESULT/Result/case2_50x_pressure/20260426_snapshot_no_allocblack_seed5`
- 平台：macOS aarch64
- SDK runtime：已替换 `libcangjie-runtime.dylib` 并执行 `codesign -f -s -`
- 重新编译：已重新 `cjc benchmark.cj -o <run_dir>/benchmark`，未复用旧 binary
- 环境：
  - `seed5.env`
  - `cjHeapSize=4gb`
  - `CJ_MS_FORCE_ENQUEUE_MARKED` 未设置
  - `CJ_MS_SWEEP_DIAG` 未设置

关键结果：

```text
[BENCH] F1_TreeIntegrity         PASS  9279 ms
[BENCH] F2_Generational          PASS  27653 ms
[BENCH] F3_LargeObject           PASS  100458 ms
[BENCH] F4_LinkedList            PASS  6892 ms
[BENCH] F5_HashMap               PASS  8034 ms

[BENCH] Functional: 5/5 passed
[BENCH] Total wall time:    332962 ms
Peak RSS: 117.8 MB
```

结论：

- 当前正式修复已经覆盖 case2/F5 的直接失败场景。
- 修复不依赖 `CJ_MS_FORCE_ENQUEUE_MARKED=1` 这个诊断开关。
- “把 sweep snapshot 前移 + 用现有 generation 让 GC-active region 逃过本轮 + 移除 alloc-black” 这一组改动，在当前回归上是成立的。

## 6. 当前结论

这次问题的正式修复方向，不是恢复物理第三 bitmap 槽位，也不是继续保留 alloc-black 后再打补丁兜底，而是：

1. 明确本轮 sweep 候选集合的冻结时刻；
2. 用已有 `globalSweepGen_` 表达“本轮之后才加入的 region”；
3. 取消 new-time alloc-black，避免对象只因分配而进入“已标记但未真正扫描”的危险状态。

如果未来又出现“新对象直接引用旧 sweep candidate，且没有经过任何后续 trace/屏障补齐”的新场景，再考虑是否需要恢复显式 scanned/black 状态位；但至少在当前 case2/F5 的失败链路上，这一轮修复已经闭环。