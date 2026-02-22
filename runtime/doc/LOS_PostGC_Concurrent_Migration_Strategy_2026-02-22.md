# LOS 大对象迁移策略（Post-GC，非 STW）

> 日期: 2026-02-22  
> 目标: 将 MAIN → LOS 大对象迁移从 STW 中移出，采用并发安全方案；并与 OOM 后 LOS compact 流程保持一致。

---

## 1. 目标与边界

### 1.1 目标

1. 大对象迁移不在 STW 中执行，降低停顿；
2. 迁移时机固定在 **GC 结束后**（先完成 LOS 垃圾回收/压缩，再迁移）；
3. 与 OOM 路径一致：**OOM 触发 LOS compact 后，迁移同样走 Post-GC 并发流程**；
4. 保证引用一致性与对象可达性，不引入并发崩溃（SIGSEGV/断言）。

### 1.2 非目标

- 本文不改变 OOM 时是否进入 compact 的判定策略；
- 本文不要求一次迁移完所有对象（允许分批、可中断、可续跑）。

---

## 2. 现状问题

现有 `MigrateLargeObjectsFromMain + UpdateMigratedReferences` 的典型风险是：

- 直接复制对象后再全堆扫更新引用；
- 若不在 STW，mutator 可能并发读取旧地址，发生竞态。

结论：**非 STW 下不能用“先搬再全量同步改引用”的模型**，必须复用并发 forwarding/barrier 语义。

---

## 3. 推荐方案（方案 A）

### 3.1 核心思想

采用“**迁移计划 + 转发表安装 + barrier 渐进收敛 + 延迟回收**”模式：

1. 在 GC 结束后生成 `postGcMigrationPlan`（来源于 fallback 记录或筛选集合）；
2. 为每个条目分配 LOS 目标区并安装 old->new route（对象状态进入 FORWARDING 语义）；
3. mutator/GC 线程通过读屏障访问到 to-version，逐步完成引用修正；
4. 后台迁移线程做预算化推进（不抢占业务线程）；
5. 仅在条目“已收敛”后回收旧 MAIN region。

### 3.2 为什么可行

- 复用现有 `TryForwardObject/ForwardBarrier` 并发收敛机制；
- 避免一次性全堆改引用；
- 迁移与回收解耦，旧 region 生命周期可通过状态机控制。

### 3.3 无后台迁移线程的简化模式（可选，建议优先）

可不引入专用后台迁移线程，改为：

1. GC 结束后仅保留 `postGcMigrationPlan`；
2. 运行期由 barrier 按访问触发迁移收敛；
3. 下一轮 GC 的 `Preforward/ForwardFromSpace` 阶段执行“强制收敛”，处理未命中 barrier 的剩余条目；
4. finalize 时仅回收已 `FORWARDED` 条目，其余保留到下一轮。

该模式实现更小、并发面更窄，适合 Phase 1 先落地。

---

## 4. 与 OOM 路径对齐

统一流程：

1. OOM GC 内执行 LOS compact（可 STW）；
2. compact 完成后，不在 STW 做 MAIN→LOS 迁移；
3. 将可迁移对象加入 `postGcMigrationPlan`；
4. GC 退出后由并发迁移器推进。

即：**OOM 后“整理”和“迁移”解耦，迁移与普通周期保持同一并发机制**。

---

## 5. 并发安全策略

### 5.1 状态机（每个迁移条目）

- `PLANNED`：已记录 oldObjAddr，未分配目标；
- `ROUTED`：已分配 LOS 目标并安装 route；
- `FORWARDING`：已进入并发收敛；
- `FORWARDED`：对象已完成转发；
- `RECLAIMED`：旧 MAIN region 已安全回收。

### 5.2 关键约束

1. **先 route 后暴露**：old->new 映射安装完，再允许并发读看到迁移对象；
2. **禁止直接 free 老区**：必须满足 `FORWARDED + 无未决依赖` 才可回收；
3. **计划容器加锁**：计划增删、状态推进、统计更新必须受 mutex 保护；
4. **分批预算执行**：每次迁移固定条目/字节预算，避免长尾抢占；
5. **失败可恢复**：目标分配失败/冲突不 fatal，条目回退到 `PLANNED` 下轮重试。

### 5.3 数据结构建议

- `postGcMigrationPlan: vector<MigrationEntry>`
- `fallbackLiveSet: set<uintptr_t>`（来自 LOS 插入失败回退对象）
- `postGcMigrationEnabled: atomic<bool>`
- `postGcMigrationEpoch: uint64_t`（用于轮次与日志对齐）

### 5.4 双份内存窗口与“先占位”策略

#### 现象

在迁移条目 `ROUTED` 后到 `RECLAIMED` 前，通常会出现 old MAIN + new LOS 的双份占用窗口。

#### 是否可以“先占据位置但不申请内存”

可以做“逻辑占位”，但要区分两层含义：

1. **逻辑占位（推荐）**
   - 仅在 `losFreeMgr`/计划里预留目标区间（slot），暂不安装 route、暂不复制对象；
   - 这种方式可避免立即进入双份实占窗口。

2. **已安装 route 但目标不分配（不推荐）**
   - 不可直接这么做；一旦 route 对外可见，barrier 可能访问 to-version，必须保证目标对象内存与内容有效。

3. **虚拟地址预留但延迟物理提交（工程增强项）**
   - 理论可行（reserve VA，按需 commit）；
   - 需要 allocator/页管理层支持，当前实现不应默认依赖该能力。

#### 推荐落地

采用两阶段：

- `RESERVED_SLOT`：先占位，不 route，不 copy；
- `ROUTED`：在准备实际迁移时一次完成“分配/拷贝/安装 route”。

这样可以在正确性不退化前提下，显著缩短双份内存持续时间。

---

## 6. 迁移对象选择策略

默认采用两级策略：

1. **一级（必选）**：仅迁移 fallback 记录对象（可控、与需求一致）；
2. **二级（可选）**：若 LOS 有余量，可按大小/年龄阈值追加部分 MAIN 大对象。

筛选条件：

- 排除 raw-pointer/pinned 对象；
- 排除已在 LOS 或已被回收对象；
- 仅保留当前仍在 oldLargeList 且存活对象。

---

## 7. 执行时序（推荐）

1. `PostTrace` 完成 LOS garbage collect/compact；
2. 采集 fallback 存活对象，生成 `postGcMigrationPlan`；
3. 退出 GC（不延长 STW）；
4. gc-helper/专用 migrator 线程按预算执行：
   - 分配 LOS 目标
   - 安装 route
   - 推动 forward 收敛
   - finalize 回收 old MAIN region
5. 下一轮 GC 前若仍有 pending，继续推进，不丢计划。

> 若采用“无后台线程简化模式”，第 4 步由 barrier + 下一轮 GC 强制收敛共同承担。

---

## 8. 可观测性（必须补）

建议新增日志与计数：

- `postGcMig.planSize`
- `postGcMig.routed / forwarded / reclaimed`
- `postGcMig.bytesMoved`
- `postGcMig.allocFail`
- `postGcMig.pending`
- 轮次维度：`epoch` + `gcReason`

判定“迁移生效”的最小信号：

- `migrateIn > 0`
- `pending` 下降
- `MAIN largeInMainRatio` 下降（多轮对比）

---

## 9. 分阶段落地建议

### Phase 1（先打通）

- 仅处理 fallback 对象；
- 单线程后台迁移；
- 不做激进并行。

### Phase 2（再优化）

- 预算化并行推进；
- 加入选择策略（二级候选）；
- 增强失败重试与退避。

### Phase 3（性能化）

- 自适应预算（按 mutator 压力调节）；
- 指标联动（LOS 利用率/碎片阈值）。

---

## 10. 风险与缓解

1. **计划长期积压**：限制每轮新增，优先清 pending；
2. **并发路径遗漏**：所有对象读取路径必须经过 barrier；
3. **回收时序错误**：严格以 `FORWARDED` 判定回收；
4. **OOM 下二次抖动**：OOM 后先 compact，再延后迁移，避免单次 GC 过重。

---

## 11. 与当前实现关系

- 当前 OOM compact 路径可保留；
- `MigrateLargeObjectsToLOS` 的 STW执行方式不再作为目标实现；
- 新目标为：**Post-GC 并发迁移统一路径**（普通周期与 OOM 后一致）。

---

## 12. 待实现检查项

- [ ] 新增 `postGcMigrationPlan` 与状态机
- [ ] fallback 存活对象采样与去重
- [ ] route 安装与并发收敛接入
- [ ] finalize 回收安全门
- [ ] 迁移统计与日志
- [ ] OOM 路径切换到同一迁移队列
