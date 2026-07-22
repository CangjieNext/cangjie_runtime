# Step37：MSGC 路线 B —— deferred-discover flush 重构实现规格

> 记录时间：2026-07-09
> 目标：把 Step36 §5 反复论证的「flush 把 mutator 拉进重 collector 工作」问题，用 **Go 风格的 deferred-discover** 一次性解决——**flush 退化成 O(1) 甩块，discover/mark 挪到消费端（assist/GC 线程）**。本文是可直接落地的实现规格，后续按本文分步实施。
> 口径：**以当前代码为准**。所有引用行号基于 Step37 记录时刻的仓库状态；实施时以实际代码为准复核。
> 关系：本文是 Step36 §5.3（swap-and-publish）+ §5.11（LFStack 全局锁）+ §5.19/§5.20（R-2 等）几条线的**收敛落地版**，并明确选定 **路线 B（消费端标记）而非路线 A（写时标记）**。

---

## 目录

1. [为什么是路线 B](#1-为什么是路线-b)
2. [现状快照（对象头 / fast path / slow path / flush）](#2-现状快照)
3. [目标形态](#3-目标形态)
4. [数据结构改动](#4-数据结构改动)
5. [各环节改动详解](#5-各环节改动详解)
6. [正确性不变式](#6-正确性不变式)
7. [分步落地计划](#7-分步落地计划)
8. [ABI 与回退](#8-abi-与回退)
9. [验证口径](#9-验证口径)
10. [附：路线 A 为什么暂缓](#10-附路线-a-为什么暂缓)

---

## 1. 为什么是路线 B

Step36 的诊断实验（丢弃三条 flush → case27 从 ~4000 抬到 ~9600）证明：当前瓶颈不是 `wbBuf.Push()`，而是 **buffer 满后 mutator 同步做 `1024 × DiscoverObject + repack + route`**。

有两条根治方向：

- **路线 A（写时标记）**：屏障 inline 里就把 old 标记掉，wbBuf 直接存 grey，flush 纯甩块。**问题**：MSGC 小对象 mark 位在 external region `markBitmap`，标记 = region 查表 + 除法 + bitmap 原子，inline 进屏障会让快路径翻倍，且要么加对象头（+2B/对象，小对象代价大），要么换字节表（内存 8×）+ 改 sweep。改动大、风险高。
- **路线 B（消费端标记）**：屏障照旧 cheap store 原始 old-ref，flush 纯甩块，**discover/mark 推迟到消费端**（assist/GC drain 时做）。

**Go 就是路线 B**：写屏障只把指针塞进 P 本地 `wbBuf`，真正标记发生在 flush/scan 时的 `greyobject()`（span 查表 + `divMul` 倒数乘法 + `gcmarkBits` 原子）。Go 十年生产验证。

因此选路线 B：**不动对象头、不涨内存、sweep 不改，用最小改动拿到「flush = O(1) 甩块」**，并顺带把除法用 R-2 倒数乘法消掉。若实测发现「消费端 discover 仍是瓶颈且去重不够」，再评估路线 A。

---

## 2. 现状快照

### 2.1 对象头 `StateWord`（8 字节，[src/Common/StateWord.h:80](../../../src/Common/StateWord.h)）

```
[ b0-31 typeInfoLow32 | b32-47 typeInfoHigh16 | b48-63 objectState(仅2位stateCode) ]
```

- TypeInfo 占 48 位；`objectState` 16 位里只用 2 位 → **~14 位空闲**（路线 B 不用，记录备查）。
- `IsValidObject() = GetTypeInfo() != nullptr`（[BaseObject.h:26](../../../src/Common/BaseObject.h)）——回收/清零槽 TypeInfo 为 null，是**消费端 discover 挡野指针的判据**。

### 2.2 写屏障 inline fast path（[AArch64AsmPrinter.cpp:2665 起](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)，ref 写）

**注意：当前代码已比 Step36 §2 文档更进一步**——§5.5 连续去重、old 堆检查延迟到 flush 都已落地：

| 步 | 动作 | 备注 |
| --- | --- | --- |
| 1 | phase = `LDARB acquire`（GOT 间接）；==0 → StoreOnly | §3.1/§3.2 未做 |
| 2 | load old；`old==0` → StoreOnly；`old==new` → StoreOnly | |
| 3 | **obj** 堆检查（GOT globals）→ 非堆 StoreOnly | **old 的堆检查已延迟到 flush**（[:2709](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)） |
| 4 | load allocBuf（`[X28,#off]`）→ null → Slow | |
| 5 | load count；`count>=1024` → Slow | overflow 入口 |
| 6 | 连续去重：`old==buf_[count-1]` → StoreOnly | 已落地（[:2741](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)） |
| 7 | `STR old,[buf_+count]; count++` | |
| 8 | StoreOnly：`STR new,[field]` | |

关键 ABI 常量（[AArch64AsmPrinter.cpp:101](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp)）：
```cpp
MSGCWBBufferOffsetInAllocBuffer = 896;   // wbBuf.buf_ 在 AllocBufferMS 内偏移
MSGCWBBufferCountOffset          = 8192;  // wbBuf.count_ 偏移
MSGCWBBufferCapacity             = 1024;
```

### 2.3 runtime slow path（[src/Heap/MBarrier/MSMarkBarrier.cpp](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)）

- `WriteReference`/`WriteStaticRef`（[:134](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)）：`old==new` skip；否则 `EnqueueRefFast(old)` + store。
- `EnqueueRefFast`（[:25](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)）：`IsHeapAddress` 过滤 → `GetOrCreateAllocBufferMS` → wbBuf；满则 `FlushWriteBarrierOverflow`；Push。
- bulk（`WriteStruct`/`CopyRefArray`/`CopyStructArray`，[:166/:203/:223](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)）：枚举 dst range 内旧 ref 逐个 `EnqueueRefFast`。
- 读侧全零成本直读（[:97](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)），**路线 B 不碰**。

### 2.4 flush 现状（[MarkSweepCollector.cpp](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）

- `FlushWriteBarrierOverflow`（[:968](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）：mutator 同步 `for each: DiscoverObject → repack 进 workBufPool_.Get() 的 WorkBuf → PushWBBuf(list1_/globalFull_)`。
- `FlushExitedWriteBarrierBuffer` / `FlushAllWriteBarrierBuffers`（final STW）。
- 消费：`DrainOneBufRound`（[:1177](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）从 `getCur_→list1_→list2_→putCur_→globalFull_` 取 WorkBuf，`TraceObjectRefFieldsToGCWork` 扫字段。
- **`LFStack`（globalFull_ 与 workBufPool_.freeList_）是 mutex**（[LFStack.h:49](../../../src/Common/LFStack.h)），`list1_` 只有 4 节点（`kMaxNodes=4`）。

> 已完成的前置（Step36 H 系列）：H-1 `DiscoverObjectNoCount`（去重复 region 查表）、H-2 批量原子计数、H-5 去判空死分支、H-6 删死模板。**路线 B 直接复用 `DiscoverObjectNoCount` 作为消费端 discover 实现。**

---

## 3. 目标形态

一句话：**把「buf 满时那 1024 次 discover」从 mutator 挪到消费端；flush 退化成 O(1) 甩块；三种存储收敛成一种 `WorkBuf`，per-P 缓存供给。**

```
写屏障(mark阶段): 读 old → 去重 → 把 raw old 写进 activeLog_ (一个 WorkBuf)
buf 满         : swap —— 把满的 activeLog_ 标 needsDiscover 甩给 list1_/globalFull_(O(1))
                        从 per-P logCache_ 取一个空 WorkBuf 当新 activeLog_
消费(assist/GC): 取块 → 若 needsDiscover 先 DiscoverObjectNoCount(shade+去重) → 再 scan
final STW      : 把 published 但未 drain 的日志块 + 各 P activeLog_ discover 干净再 sweep
```

对照现状的本质变化：**discover 的总工作量不变，但从 mutator 关键路径上移走、摊进消费端正常 drain**——正是诊断实验暴露的 ~9600 上界来源。且 discover 变成 assist 的正常工作 → **pacing/保活不丢失**（区别于「直接丢 flush」导致的 OOM）。

---

## 4. 数据结构改动

### 4.1 `WorkBuf` 增加 `needsDiscover`（[src/Common/LFStack.h:24](../../../src/Common/LFStack.h)）

```cpp
struct alignas(64) WorkBuf {
    static constexpr size_t kCapacity = 1024;
    WorkBuf* next = nullptr;
    size_t   len = 0;
    bool     needsDiscover = false;   // 新增：true=屏障原始日志(未discover); false=已灰对象
    BaseObject* objs[kCapacity];
    // Reset 里要清 needsDiscover = false
};
```
- `Reset()` 增加 `needsDiscover = false;`。
- 注意 64B 对齐下 bool 只占用原 padding，不改变 `objs` 起始（需 `static_assert` 复核 objs 偏移，见 §8）。

### 4.2 `AllocBufferMS`：wbBuf → `WorkBuf* activeLog_` + per-P 缓存（[src/Heap/MAllocator/MSAllocBuffer.h](../../../src/Heap/MAllocator/MSAllocBuffer.h)）

```cpp
// 旧: WriteBarrierBuffer wbBuf_;   (inline BaseObject*[1024] + count_)
// 新:
WorkBuf* activeLog_ = nullptr;      // 屏障正在写的日志块 (就是一个 WorkBuf)
WorkBuf* logCache_ = nullptr;       // per-P 空闲 WorkBuf 单链 (复用 next, 无锁单所有者)
size_t   logCacheCount_ = 0;
```
- 屏障 ABI 现在指向 `activeLog_` 指针偏移 + WorkBuf 内 `objs`/`len` 偏移（见 §5.2、§8）。
- `logCache_` 用 `WorkBuf::next` 串联，per-P 单所有者，无需锁。

### 4.3 `WorkBufPool` 增加批量接口（[src/Common/LFStack.h:111](../../../src/Common/LFStack.h)）

```cpp
size_t GetN(WorkBuf** out, size_t n);   // 一次锁取 ≤n 个 (不足则 new 补)
void   PutN(WorkBuf** bufs, size_t n);  // 一次锁还 n 个
```
供 per-P 缓存 miss/溢出时批量与全局池交互，把「每 WorkBuf 一次全局锁」降到「每 K 个一次」。

---

## 5. 各环节改动详解

### 5.1 per-P WorkBuf 池缓存（阶段1 —— 先行，零 ABI）

在 `AllocBufferMS` 上封装两个无锁快路径：

```cpp
// 取一个空 WorkBuf 作日志/工作块
WorkBuf* AllocBufferMS::AcquireWorkBuf(WorkBufPool& pool) {
    if (logCache_ != nullptr) {
        WorkBuf* b = logCache_; logCache_ = b->next; b->next = nullptr; --logCacheCount_;
        b->Reset(); return b;
    }
    // miss: 一次锁批量取 K 个, 留 K-1 在本地
    WorkBuf* bufs[kRefill];
    size_t got = pool.GetN(bufs, kRefill);          // kRefill 建议 8
    for (size_t i = 1; i < got; ++i) { bufs[i]->next = logCache_; logCache_ = bufs[i]; ++logCacheCount_; }
    bufs[0]->Reset(); return bufs[0];
}
// 归还一个空 WorkBuf
void AllocBufferMS::RecycleWorkBuf(WorkBuf* b, WorkBufPool& pool) {
    if (logCacheCount_ < kHigh) {                    // kHigh 建议 16
        b->next = logCache_; logCache_ = b; ++logCacheCount_; return;
    }
    // 攒够才批量还全局
    WorkBuf* batch[kHigh]; size_t k = 0; batch[k++] = b;
    while (logCacheCount_ > kHigh/2) { WorkBuf* t=logCache_; logCache_=t->next; --logCacheCount_; batch[k++]=t; }
    pool.PutN(batch, k);
}
```
- **生命周期**：GC cycle 末尾（`ResetAllPerPGCWork` / `DisposeAllPerPGCWork`，[MarkSweepCollector.cpp:1160 附近](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）把每个 P 的 `logCache_` 残块 `PutN` 归还全局池并清零。
- 现有 `FlushWriteBarrierOverflow` 里的 `workBufPool_.Get()/Put()`（[:1024/:1039/:1047](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）先改成走 `AcquireWorkBuf/RecycleWorkBuf`。**这一步不改 flush 语义，只把全局锁摘掉，可单独测。**

### 5.2 fast path 改动（最小，动 ABI）

发射层第 5/7 步的 buffer 访问从「inline 数组」变「指针解引用」：

```
旧: LDR X11,[X28,#allocBufOff]                    ; allocBuf
    LDR X13,[X11,#CountOffset/8]                  ; count
    ADD X12,X11,#WBBufferOffset                   ; &buf_[0]
    STR old,[X12,X13,lsl#3]; count++
新: LDR X11,[X28,#allocBufOff]                    ; allocBuf
    LDR X11,[X11,#activeLogOff]                   ; ← +1 解引用: activeLog_ (WorkBuf*)
    LDR X13,[X11,#WorkBufLenOff/8]                ; len
    ADD X12,X11,#WorkBufObjsOff                   ; &objs[0]
    STR old,[X12,X13,lsl#3]; len++
```
- **唯一净增：+1 条依赖 load**（activeLog_ 解引用）。与域一（去 3 个 GOT load）叠加后净指令仍大降。
- 连续去重（第 6 步）逻辑不变，只是基址/计数换成 `objs`/`len`。
- 满判定阈值仍 `kCapacity=1024`。
- ABI 常量替换：`MSGCWBBufferOffsetInAllocBuffer` → `activeLogOffsetInAllocBuffer`；`MSGCWBBufferCountOffset` → WorkBuf 内 `len` 偏移；新增 `WorkBufObjsOffset`。全部 `static_assert` 钉死（§8）。

### 5.3 slow path / overflow → swap-and-publish（[MSMarkBarrier.cpp:25](../../../src/Heap/MBarrier/MSMarkBarrier.cpp) + [MarkSweepCollector.cpp:968](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）

`EnqueueRefFast` 与 asm slow stub 的 overflow 分支统一改成：

```cpp
inline void EnqueueRefFast(BaseObject* ref) {
    if (ref == nullptr || !Heap::IsHeapAddress(ref)) return;
    AllocBufferMS* ab = AllocBufferMS::GetOrCreateAllocBufferMS();
    WorkBuf* log = ab->activeLog_;
    if (log == nullptr) { log = ab->activeLog_ = ab->AcquireWorkBuf(pool); }
    if (UNLIKELY(log->Full())) {
        // swap-and-publish: O(1), 不 discover / 不 repack / 不拷贝
        log->needsDiscover = true;
        PublishLog(ab, log);                    // → gcw->PushWBBuf(log) (list1_/globalFull_)
        log = ab->activeLog_ = ab->AcquireWorkBuf(pool);
    }
    log->objs[log->len++] = ref;                // (连续去重可选保留)
}
```
- `FlushWriteBarrierOverflow` 的「逐条 DiscoverObject + repack」**整段删除**，替换为上面的 swap 逻辑（或保留函数名但改成 publish）。
- `PublishLog` 复用现有 local-first 路由：`gcw->PushWBBuf(log)`（[LFStack.h:241](../../../src/Common/LFStack.h)）。
- bulk 路径（WriteStruct 等）：R-1（§5.5 下述）把 `GetOrCreateAllocBufferMS`/`activeLog_` 提到循环外，lambda 内只 `IsHeapAddress + 写 objs`。

### 5.4 消费端 deferred discover（核心，正确性关键）⚠️（[MarkSweepCollector.cpp:1177](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）

`DrainOneBufRound` 的循环体按 `needsDiscover` 分流：

```cpp
size_t localForwarded = 0;
while (getCur_ && !getCur_->Empty()) {
    BaseObject* obj = getCur_->Pop();
    if (getCur_->needsDiscover) {
        // 屏障日志块: 先 shade (mark-CAS), 赢者才 scan → 天然去重
        if (!DiscoverObjectNoCount(obj)) continue;    // 复用 H-1 fast path
        ++localForwarded;
    }
    // needsDiscover=false 的块 (标记遍历产出) 已在源头 discover, 直接扫:
    if (UNLIKELY(!IsTraceableObjectAddress(obj))) continue;   // 仅保守校验(见注)
    if (UNLIKELY(!obj->IsValidObject())) { /* 现有 FATAL 处理 */ }
    if (!obj->HasRefField()) continue;
    if (UNLIKELY(obj->IsWeakRef())) { PushSingleToDeferredMainThread(obj); continue; }
    TraceObjectRefFieldsToGCWork(obj, gcw);
}
if (localForwarded) markedObjectCount.fetch_add(localForwarded, std::memory_order_relaxed);  // H-2
```
- **为什么不能所有块都 discover-on-consume**：`needsDiscover=false` 的块来自 `TraceObjectRefFieldsToGCWork → PutObj`，已在源头 `DiscoverObject` 去重；若对它们也无条件 discover+入队会导致队列爆量。故**只有屏障日志块** discover-on-consume。
- 对 `needsDiscover=true` 且 `DiscoverObjectNoCount` 已包含 `IsTraceable+IsValid`，其后的保守校验可省（避免重复），实现时二选一，别双算。
- `list2_`/`putCur_` 产出的块 `needsDiscover=false`（在 `PutObj` 侧保证初值）。

### 5.5 bulk 路径 R-1（顺带，接 Step36 §5.20）

`WriteStruct`/`CopyRefArray`/`CopyStructArray` 把 owner 解析提到循环外：
```cpp
AllocBufferMS* ab = AllocBufferMS::GetOrCreateAllocBufferMS();   // 一次
obj->ForEachRefInStruct([&](RefField<false>& f){
    BaseObject* r = f.GetTargetObject();
    if (r && Heap::IsHeapAddress(r)) PushToActiveLog(ab, r);     // 无 per-ref TLS 查找
}, start, start+size);
```

### 5.6 R-2 倒数乘法（消费端 discover 提速，[MSRegionInfo.cpp:337](../../../src/Heap/MAllocator/MSRegionInfo.cpp)）

`TestAndSetMarkBit` 的 `slotIdx = (addr-regionStart)/slotSize` 换 per-region 预存倒数魔数（Granlund–Montgomery），除法 → 乘加。size class 非 2 幂（`8,16,24,32,48,...`）故需魔数而非移位。`MSRegionInfo` 增加 `slotSizeMagic`（分配 region 时算好）。消费端 discover 现在是主成本，值得。

---

## 6. 正确性不变式

### 6.1 SATB 完备性
published 日志块里的 old-ref 在 sweep 前必须**全部被 discover**。保证链：
1. mark 阶段内老 ref 不会被回收（sweep 是后续独立阶段）——deferred discover 只是把时机后移，安全性同现状（现状 flush 也在 mark 阶段做）。
2. final STW `collectPendingMarkWork`（[MarkSweepCollector.cpp:1326](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）里 `FlushAllWriteBarrierBuffers + DrainGlobalLFStackToMainQueue` 必须遍历所有 published 块 + 各 P `activeLog_`，对 `needsDiscover` 块逐条 discover 后入 mainQueue，再 sweep。**这是硬门槛**。

### 6.2 pacing / 防 OOM
Step36 §5.2 第二实验：assist 归零 → 12s 内 OOM。路线 B **不是关 assist**，而是把 discover 变成 assist 正常 drain 的一部分：
1. 消费端 discover = assist 干活 → 保活不变。
2. 背压仍看 `globalFull_.Count()`（[MarkSweepCollector.cpp:700](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）：published 块进 globalFull_/list1_ 后 Count 可见 → assist ratio 照常升。
3. **要确认**：assist debt 模型把「published 但未 discover 的块」计入欠债（`globalShareCount_` / debt trigger），否则 mutator 甩完就跑、背压滞后。若滞后，在 `PublishLog` 里给 debt 记账。

### 6.3 去重正确性
`DiscoverObjectNoCount` 的 mark-CAS 赢者唯一：同一 old 出现在多个日志块，只有赢 CAS 的消费者会 scan，其余 already-marked 跳过。漏过滤只多记不漏记，安全。

---

## 7. 分步落地计划

每步可单独编译 / syntax-check / 测，且可独立回退。

| 步 | 内容 | 文件 | ABI | 风险 | 可测点 |
| --- | --- | --- | --- | --- | --- |
| **1** | per-P WorkBuf 池缓存（§5.1）+ `WorkBufPool::GetN/PutN` | MSAllocBuffer.{h,cpp}, LFStack.h, MarkSweepCollector.cpp | 无 | 低 | 全局 pool 锁竞争下降；case27 吞吐/CPU |
| **2** | `WorkBuf.needsDiscover` + 消费端分流（§4.1、§5.4，默认 false → 行为不变） | LFStack.h, MarkSweepCollector.cpp | 无 | 低 | 回归无差异（未产生日志块前） |
| **3** | R-2 倒数乘法（§5.6） | MSRegionInfo.{h,cpp} | 无 | 低 | 消费端 discover 变快；verify 稳定 |
| **4** | bulk R-1（§5.5） | MSMarkBarrier.cpp | 无 | 低 | 容器 grow/copy 负载 |
| **5** | wbBuf → `activeLog_` + swap-and-publish（§4.2、§5.3）——**runtime 侧** | MSAllocBuffer.h, MSMarkBarrier.cpp, MarkSweepCollector.cpp | 无(内部) | 中 | 需配合步6 才完整，可先用 runtime-only 屏障路径验证语义 |
| **6** | fast path 指针化 + ABI 偏移切换（§5.2）——**发射层** | AArch64AsmPrinter.cpp | **是** | 高 | 反汇编核对；`.ll` CHECK 更新；完整验证链 |

**建议先做 1→2→3→4**（全零 ABI、纯 runtime，可叠加已完成的 H-1/H-2 直接测一版）；确认「free flush 雏形（步5 runtime 路径）+ pool 去锁」的收益后，再上步 6 的发射层 ABI 改动。

> 每一步都走 Step26 §9.4 完整验证链：改 → 重建 compiler（若动发射层）→ 重建 runtime（**必须 `--gc-mode marksweep`**）→ 新 cjc 重建 std → 组 SDK → 重建 stdx → 跑 case27。**纯 runtime 步骤（1-5）可先用 marksweep syntax-check 快速自检，但最终仍需完整链上 case27。**

---

## 8. ABI 与回退

### 8.1 ABI 偏移（步 6）
发射层硬编码偏移必须与 runtime 同步，并在 runtime 侧 `static_assert` 钉死：
```cpp
// runtime 侧新增/修改 (示意):
static_assert(offsetof(AllocBufferMS, activeLog_) == activeLogOffsetInAllocBuffer);
static_assert(offsetof(WorkBuf, len)  == WorkBufLenOffset);
static_assert(offsetof(WorkBuf, objs) == WorkBufObjsOffset);
```
- 旧常量 `MSGCWBBufferOffsetInAllocBuffer=896` / `MSGCWBBufferCountOffset=8192` 废弃或改指。
- `WorkBuf` 加 `needsDiscover` 后要复核 `objs` 偏移未意外位移（64B 对齐下应吸收进原 padding）。

### 8.2 回退
- 步 1-5（runtime）：可用 `AllocBufferMS` 内部开关在「新 activeLog_ 路径」与「旧 inline wbBuf 路径」间切；或直接 git revert。
- 步 6（发射层）：`EnableMSGCWriteBarrierLowering` 关闭 → 回到通用 `CJ_MCC_Write*` slow path（此时 slow path 已是 swap-and-publish，仍正确）。

---

## 9. 验证口径

### 9.1 正确性
1. `CJ_MS_VERIFY_MARK=1` 下 `VerifyMarkCompleteness` 不报错（**每步都测**）。
2. case27 连续多轮无 `SIGSEGV`/`Abort trap: 6`、无 invalid object / stale pointer / json parse fail。
3. 重点盯 §6.1：故意在 final STW 前注入未 drain 的日志块，确认被 discover 干净（可加临时断言）。

### 9.2 性能
1. case27 `c8/10K`、`c8/100K`、`c10/p10K/heap4gb`。
2. 指标：吞吐、CPU（是否更接近 CopyGC）、mark 总时长、`wbFlushStats_`（现在应显示 flush=纯 publish，forwarded 统计移到消费端）、`globalFull_`/pool 锁竞争、原子 CAS 次数。
3. 判据：多轮稳定，且**无 OOM**（§6.2 pacing 生效）。

### 9.3 分步预期
| 步 | 主要看 |
| --- | --- |
| 1 | pool 全局锁竞争下降、overflow 路径 CPU 下降 |
| 5+6 | mutator 侧 flush 成本消失（swap O(1)）、吞吐向 ~9600 逼近 |
| 3 | 消费端 discover 除法消失 |
| 全 | 无 OOM、verify 稳定、多轮吞吐稳定 |

---

## 10. 附：路线 A 为什么暂缓

路线 A（写时 inline mark）需要先让 mark 便宜：
- 小对象 mark 位在 external `markBitmap`，inline = region 查表 + 除法 + bitmap 原子，会让屏障翻倍。
- 要便宜就得二选一：**in-header mark 位**（对象头加位 + sweep 改 epoch，中等改造）或**字节 side 表**（内存 8× + sweep 重写）；技巧2（slotIdx 入头）对 8-24B 小对象 +2B 代价过大。

路线 B 完全不碰这些，用最小改动拿到同样的「free flush」。**决策：先路线 B 落地 + 实测**；仅当实测暴露「消费端 discover 仍是瓶颈且去重不足」时，再回头评估路线 A（届时 §2.1 记录的对象头 14 空闲位可用）。

> **更新（后续）**：在 Step37 route-B 落地之上，已进一步做了 route-A 转换（写时 mark），保留 external bitmap、**不改对象头/sweep**，靠 R-2 倒数乘法让 mark 便宜。做法与现状见 §11；inline 快路径的实现与开启见 §12。

---

## 11. 路线 A 落地快照（已实现，以当前代码为准）

route-A 在 route-B 基础设施（activeLog_ WorkBuf / per-P 缓存 / swap-publish / 消费端 scan）之上，把 **discover(mark) 从消费端前移到写时**：写屏障 shade(old)，只有赢 mark-CAS 的新灰对象才入 buffer；buffer 全灰（`needsDiscover=false`）；消费端只 scan。

### 11.1 当前模式：inline 发射关闭，全走 C++

- 编译器 `cl::opt EnableMSGCInlineWriteBarrier` **默认 false**（[AsmPrinter.cpp](../../../../cangjie_compiler/third_party/llvm-project/llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp)）。off 时 MSGC 写屏障**不发 inline**，fall through 到 `BL CJ_MCC_Write*_MSGC` wrapper → runtime C++ 屏障。
- Step37 的指针化 inline asm 代码仍在编译器里，但**不发射**（死码），因为它 push 的是 raw old（未 mark），与 route-A「buffer 全灰」不一致。启用前必须先给它加 inline mark（§12）。

### 11.2 三条链路现状

| 链路 | 现状实现 |
| --- | --- |
| **fast path** | 当前 = 一次 `CJ_MCC_Write*_MSGC` wrapper 调用（无 inline）。 |
| **slow path (C++ 屏障)** | `WriteReference`（[MSMarkBarrier.cpp:134](../../../src/Heap/MBarrier/MSMarkBarrier.cpp)）：`old==new` 快出 → `EnqueueRefFast(old)` → store。`EnqueueRefFast`：非堆/null 丢；`GetActiveCollector()==null`（非标记期）丢；否则 `EnqueueRefInto`。`EnqueueRefInto`：`ShadeRefForBarrier(old)`（= `DiscoverObjectNoCount`：IsTraceable+IsValid+原子 mark+R-2 slotIdx，返回是否白→灰赢家）→ 赢才 `activeLog_->objs[len++]=old`（源头完美去重）。bulk 路径 `ResolveBulkLogOwner` 一次拿 owner（R-1）再逐 ref 走 `EnqueueRefInto`。 |
| **flush = swap-and-publish** | `activeLog_` 满 → `PublishAndSwapLog`（[MarkSweepCollector.cpp](../../../src/Heap/MCollector/MarkSweepCollector.cpp)）：`needsDiscover=false`；`markedObjectCount += len`；`gcw.PushWBBuf`（local-first list1_，满则 globalFull_）；`AcquireWorkBuf` 换新块（per-P 缓存优先无锁）。O(1) 甩块，无 discover/repack/拷贝。owner 退出走 `PublishExitedLog`。 |
| **消费 (assist/GC)** | `DrainGCWork`/`DrainOneBufRound`：`needsDiscover=false` → 纯 scan 分支，`TraceObjectRefFieldsToGCWork` 扫字段、DiscoverObject(child) 发现子对象入 putCur_。route-B 的 discover 分支变死码但保留。 |
| **final STW** | `FlushAllWriteBarrierBuffers`：各 P `activeLog_`（已灰）→ **`PushMovedGrey` 直接送扫**（**不能** TryEnqueue，否则 re-discover 见已标记会漏扫字段）+ `DrainLogCacheToPool`。再 `DisposeAllPerPGCWork` → `DrainGlobalLFStackToMainQueue`（needsDiscover=false 走校验+PushMovedGrey）。 |

### 11.3 计数正确性
每个 buffer entry = 一次赢 mark-CAS，故 mark 总数 = publish 的 `len` 之和 + final STW activeLog_ 的 len。barrier 根在 publish 计，children 在消费端 `DiscoverObject` 计，无重复。

### 11.4 当前代价（重要）
inline off → 每个 ref 写一次 C++ 调用（`GetActiveCollector` acquire load + 标记期还有 mark 原子），**非标记期也付函数调用**。**大概率比 route-B 慢**（接近 4000）——当前这步价值是「验证 route-A 语义正确 + 源头完美去重」，**提速要靠 §12 把 mark 做进 inline 后开启**。

### 11.5 待验证的对接风险
关掉 Mach-O inline 后，`CJ_MCC_WriteRefFieldFast_MSGC` wrapper 必须作为真实可链接函数存在（非-Mach-O 一直如此）。若链接报未定义符号 → 该 wrapper 在 Mach-O 只被 inline、无实体，需让前端物化 wrapper 或 fall-through 到 `CJ_MCC_WriteRefField`。

---

## 12. inline fast-path（route-A 版）实现方案与开启

目标：把 §11 的「C++ 里 `ShadeRefForBarrier + push`」搬进 inline 快路径，去掉 wrapper 调用，同时保留 StoreOnly 各廉价出口。

### 12.1 前置依赖：域一（强烈建议先做）
inline mark 需要 `heapStart`、`MSRegionInfoArray::array_`、`phase`。若不做域一，这三个是跨 image GOT 间接全局，每次屏障多条链式 load，inline mark 收益被吃掉。**域一 TLS 化后再开 inline 才划算。**

### 12.2 runtime 侧准备（零 asm）
1. **`MSRegionInfo` 加 `uint32_t divMagic`**：region 绑定 sizeClass 时从 `SizeClassInfo::ClassToDivMagic(cls)` 拷一份。inline 直接 `[msInfo + divMagicOff]`，省掉 sizeClass→查表（否则又一次全局数组 GOT load）。
2. （可选）直接存 `regionStart`，省 `regionEnd - 32K` 一步。
3. 暴露 `heapStartAddress_` / `array_` 的可寻址符号（域一里一并 TLS 化）。

### 12.3 发射层：只给 ref/static-ref 路径加 inline mark
struct/array/static-struct/copy 是**多 ref 循环**，逐迭代 inline mark 过重 → **这 4 条路径改为 bail-to-slow**（C++ 里 `EnqueueRefInto` 会 mark，正确）。只有最热的单 ref 写（ref/static-ref）走 inline mark。

单 ref inline mark 序列（`old` 在 X10，已过 phase/null/same-value；`sizeof(MSRegionInfo)=64`、`REGION=2^15`、markBitmap@32、regionEnd@8）：
```asm
; --- old 堆检查（route-A 不能再延迟：要往 old 的 mark 位写）---
SUB   X9,  X10, heapStart
CMP   X9,  (heapEnd-heapStart)      ; 越界 → StoreOnly（非堆 old 无需记）
B.HS  StoreOnly
; --- 解析 region（REGION 2^15、struct 64B）---
LSR   X11, X9,  #15                 ; regionIdx
ADD   X11, array_, X11, LSL #6      ; msInfo = array_ + regionIdx*64
LDR   X13, [X11, #32]               ; markBitmap
CBZ   X13, Slow                     ; 无 bitmap（大对象/未初始化）→ 交 C++（大对象走 flags_）
; --- slotIdx（R-2 倒数乘法，无除法）---
LDR   X14, [X11, #8]                ; regionEnd
SUB   X14, X14, #0x8000             ; regionStart
SUB   X15, X10, X14                 ; slotOff
LDR   W9,  [X11, #divMagicOff]
UMULL X15, W15, W9
LSR   X15, X15, #32                 ; slotIdx
; --- word/bit ---
LSR   X9,  X15, #6                  ; wordIdx
AND   X15, X15, #63
MOV   X14, #1 ; LSL X14, X14, X15   ; bit
ADD   X13, X13, X9, LSL #3          ; &markBitmap[wordIdx]
; --- 原子 test-and-set（LSE 单指令；否则 LDXR/STXR 循环）---
LDSETAL X14, X9, [X13]              ; X9=旧字；[X13]|=bit
TST   X9,  X14
B.NE  StoreOnly                     ; 已灰 → 去重，不 push
; --- 赢：走 Step37 已有的 push 序列（EmitLoadActiveLogOrSlow + objs[len++]）---
```
- ~16-18 指令 + 1 原子；LSE `LDSET` 单条，否则 LL/SC +3~4。
- 寄存器紧张：`old(X10)` 要活到 push；mark 用 X9/X11/X13/X14/X15 + heapStart/array_/phase 所在寄存器。push 复用。需重排 Step37 的 push 序列寄存器。
- 大对象（markBitmap 为空）→ bail Slow，C++ 走 `AtomicTestAndSetMarked`（flags_），不在 inline 处理。
- **删掉连续去重逻辑**：mark-CAS 本身就是完美去重。

### 12.4 开启开关
1. 完成 12.2 + 12.3 后，把 `EnableMSGCInlineWriteBarrier` 默认改为 `true`（或临时用 `-mllvm -enable-msgc-inline-write-barrier` 传入，便于 A/B 对比）。
2. runtime `PublishAndSwapLog` 等已是 route-A（needsDiscover=false），与 inline 一致，无需再改。
3. 反汇编核对：ref 屏障内出现 mark 序列（`LDSET`/`fetch_or`）与 activeLog_ 指针 push；无 wrapper 调用。

### 12.5 正确性/验证
- old 堆检查已加回（inline 内），大对象/非堆 bail-to-slow → C++ 安全兜底。
- `CJ_MS_VERIFY_MARK=1` 干净；case27 无 OOM。
- 对比开/关 inline 的吞吐，确认 inline 版把 route-A 从 ~4000 拉回并超过 route-B。

---

## 结语

Step37 = 把 MSGC 的 flush/mark 结构**向 Go 对齐**：写屏障只 buffer、flush 只甩块、discover/mark 在消费端 assist/GC 上做、除法用倒数乘法。核心不变式只有两条——**final STW 必须 drain 干净所有 published 日志块（SATB 完备）**、**assist 必须继续消费以维持 pacing（防 OOM）**。按 §7 的 1→6 顺序推进，前 4 步零 ABI 可先行验证收益。

> **先把 flush 变成 O(1) 甩块、把 discover 挪到 assist，再谈发射层 ABI 指针化。**
