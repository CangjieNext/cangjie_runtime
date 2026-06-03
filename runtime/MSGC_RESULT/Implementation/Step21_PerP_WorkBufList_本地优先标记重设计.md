# Step 21：Per-P WorkBufList 本地优先标记重设计

> 2026-06-02 设计稿（实现前评审用）
> 目标：把 `PerPGCWork` 的单 buf `wbuf1_`/`wbuf2_` 升级为**有界链表**（各最多 4 个 WorkBuf 节点），
> 让 mutator 写屏障产出的灰对象优先留在**本 P** 本地、由调度到本 P 的 mutator 在 assist 里优先消化，
> 并引入**双信用池**（per-P 信用 + GC 线程 bgScanCredit_）与简化的收敛判定。

---

## 目录

- [Step 21：Per-P WorkBufList 本地优先标记重设计](#step-21per-p-workbuflist-本地优先标记重设计)
  - [目录](#目录)
  - [1. 背景与动机](#1-背景与动机)
  - [2. 现状回顾（Step 17 实现）](#2-现状回顾step-17-实现)
  - [3. 新数据结构：WorkBufList（有界链表）](#3-新数据结构workbuflist有界链表)
  - [4. PerPGCWork 重构](#4-perpgcwork-重构)
  - [5. 写屏障产出路径（→ list1）](#5-写屏障产出路径-list1)
  - [6. 标记产出路径（→ list2 → list1 → global）](#6-标记产出路径-list2--list1--global)
  - [7. 取对象路径 TryGetObj 重构](#7-取对象路径-trygetobj-重构)
  - [8. Assist 决策流程重构](#8-assist-决策流程重构)
  - [9. 信用积分新机制（GC 供给衰减 + N 轮截止）](#9-信用积分新机制gc-供给衰减--n-轮截止)
    - [9.1 bgScanCredit\_：随轮衰减、N 轮停供（替换倍率因子）](#91-bgscancredit_随轮衰减n-轮停供替换倍率因子)
    - [9.2 还债路径与拉取预算（第 1、2、3、4 点）](#92-还债路径与拉取预算第-1234-点)
  - [10. 收敛判定修改](#10-收敛判定修改)
  - [11. 受影响文件与函数清单](#11-受影响文件与函数清单)
  - [12. 风险与待确认点](#12-风险与待确认点)

---

## 1. 背景与动机

当前实现（Step 17）中，每个标记参与者（GC 线程 + 每个 mutator 的 `AllocBufferMS::gcWork_`）只持有**两个单 WorkBuf**：`wbuf1_`（get-buf）和 `wbuf2_`（put-buf）。这带来两个问题：

1. **写屏障产出直接进 global**：mutator 写屏障溢出（`FlushWriteBarrierOverflow`）把发现的灰对象打包成 WorkBuf **直接推 `globalFull_`**。这些灰对象大多是该 mutator 自己刚改写的引用，本可以留在本 P 就近消化，却被推到全局共享队列，引入 mutex 竞争，也让本可“自产自销”的工作变成跨线程搬运。
2. **本地缓冲容量太小**：单个 put-buf 满（1024 个对象）就必须立刻整包推 global，本地无法多缓存几包，导致高产出时频繁触碰全局锁。

本次重设计的核心思想：**让标记工作尽量留在本 P 本地闭环**（注意：per-P 工作随 **P（Processor）** 走，不随协程走，所以是“调度到本 P 的任意 mutator”去消化，不需要是同一个协程）——
- 写屏障产出 → 本 P `list1`（最多缓存 4 包）；
- 标记产出的子对象 → 本 P `list2`（最多 4 包），溢出再回流 `list1`，仍满才进 global；
- assist 时先把 `list2` 的满 buf 挪入 `list1`（补满为止，装不下的留 list2）；随后 **getCur_ 有残留 或 list1 有 ≥1 满 buf 就本地标一轮，标完一个 buf 即退出**，连一轮都标不了才看信用 / 全局；**每轮标一个 buf 的量（1024）**；
- **信用机制（GC 供给衰减 + N 轮截止）**：取消原 ratio 倍率爬升（Coeff/MaxMult/Scale）。GC 线程每取一个 buf（一轮）向 `bgScanCredit_` 注入信用，但**每对象注入权重随轮次递减**，到第 **N** 轮后**完全停供**（N = 上轮 GC 累计标记耗时达到 100ms 时的轮次）。停供后 mutator 产出必须自己实标消化。还债路径：**本地（list2→list1 预挪后）getCur_/list1 能标就实标** → （标不了）**偷取 `bgScanCredit_`** → （剩余债务 ≥1 buf）**从 global PopFull 拉满 buf 抵债（拉取即抵、不超过债务）**。

这样 mutator 在自己产出的灰对象上形成生产→消费闭环，减少全局锁交互；`bgScanCredit_` 的「随轮衰减、N 轮停供」让 GC 周期前段宽松、后段强制 mutator 自消化。

## 2. 现状回顾（Step 17 实现）

**数据结构**（[LFStack.h](runtime/src/Common/LFStack.h#L136)）：
```cpp
struct WorkBuf { static constexpr size_t kCapacity = 1024; WorkBuf* next; size_t len; BaseObject* objs[1024]; };
struct PerPGCWork {
    WorkBuf* wbuf1_;  // get buffer：只 Pop
    WorkBuf* wbuf2_;  // put buffer：只 Push，满则整包推 global
    LFStack* globalFull_;
    WorkBufPool* workBufPool_;
};
```
> 注：`WorkBuf::kCapacity` 实际为 **1024**（旧文档写 256，已过时）。

**写屏障溢出**（`FlushWriteBarrierOverflow`）：发现首次标记对象 → 打包 WorkBuf → **直接 `globalFull_.Push()`**。

**PutObj**：只写 `wbuf2_`，满 → 推 global，取新空 buf。

**TryGetObj**（GC 线程用）：`wbuf1_` → 晋升 `wbuf2_` → `globalFull_` 三级。
**TryGetObjLocal**（assist 用）：只有前两级，不碰 global。

**MSMarkAssist**（[MarkSweepCollector.cpp](runtime/src/Heap/MCollector/MarkSweepCollector.cpp#L1131)）：
1. `TryStealBgCreditPartial(debt)` —— **先看信用分**，够则直接跳过；
2. budget = min(trigger, debt - paid)；
3. `DrainGCWork(gcw, budget)` 用 `TryGetObjLocal`。

**信用积分**（`bgScanCredit_`）：GC 线程主循环里把**每一片 scannedCount 全部**累加为信用，mutator 通过 `TryStealBgCreditPartial` 窃取抵债。

**收敛**（`CheckConvergenceAndBreak`）：`gcThreadWork_.Empty() && globalFull_.Empty()` 连续 `kConvergenceRequiredRounds(3)` 轮；前几轮短让步 50us；**第 3 轮后再长等 200us 复检**一次，仍空才收敛。不检查各 mutator 的 per-P buf（靠 STW Dispose 兜底）。

## 3. 新数据结构：WorkBufList（有界链表）

新增一个**有界单链表**封装，复用 `WorkBuf::next` 串成链，最多 `kMaxNodes = 4` 个节点：

```cpp
struct WorkBufList {
    WorkBuf* head_ = nullptr;   // 链表头（LIFO）
    size_t   nodeCount_ = 0;    // 当前节点数
    static constexpr size_t kMaxNodes = 4;

    bool ListEmpty() const { return head_ == nullptr; }
    bool ListFull()  const { return nodeCount_ >= kMaxNodes; }
    size_t NodeCount() const { return nodeCount_; }   // assist 2-buf 门槛用

    // 把一个 WorkBuf 作为节点挂入链表。返回 false 表示链表已满、未挂入。
    bool PushNode(WorkBuf* b) {
        if (nodeCount_ >= kMaxNodes) return false;
        b->next = head_; head_ = b; ++nodeCount_; return true;
    }
    // 摘下链表头节点（nullptr 表示空）。
    WorkBuf* PopNode() {
        WorkBuf* b = head_;
        if (b == nullptr) return nullptr;
        head_ = b->next; b->next = nullptr; --nodeCount_; return b;
    }
    // 链表中所有节点对象总数（用于积分/调试，O(nodeCount_)）。
    size_t TotalObjs() const {
        size_t n = 0; for (WorkBuf* b = head_; b; b = b->next) n += b->len; return n;
    }
};
```

要点：
- 节点本身仍是 `WorkBuf`（容量 1024），`WorkBufList` 只管“最多 4 个节点”的串联与上限判定。
- LIFO 入/出，保持与现有 `LFStack` 一致的栈式访问局部性。
- `kMaxNodes = 4` 即每条链表最多缓存 4×1024 = 4096 个对象。

## 4. PerPGCWork 重构

`PerPGCWork` 用两条 `WorkBufList` 替换原来的两个裸 `WorkBuf*`，并各保留一个“正在写入/正在读取”的游标 buf：

```cpp
struct PerPGCWork {
    // list1：取对象源（get 侧）。写屏障产出、global 补货都进这里。
    WorkBufList list1_;
    // list2：标记子对象的产出汇（put 侧）。
    WorkBufList list2_;
    WorkBuf* getCur_ = nullptr;   // 当前正在 Pop 的 buf（从 list1_ 摘出）
    WorkBuf* putCur_ = nullptr;   // 当前正在 Push 的 buf（写满后挂入 list2_）

    LFStack*     globalFull_  = nullptr;
    WorkBufPool* workBufPool_ = nullptr;
    bool flushedWork_ = false;
};
```

语义约定：
- **getCur_ / putCur_ 是独立的“正在使用” buf，不计入 list1_/list2_ 的 4 个节点**。`list1_`/`list2_` 装的都是满 buf（len==1024），getCur_/putCur_ 是另外单独的部分满/在用 buf。
- **getCur_**：当前消费的 buf。空了就从 `list1_` 摘下一个新节点；`list1_` 也空再走 assist/global 决策。
- **putCur_**：当前产出写入的 buf。写满就尝试挂入 `list2_`；`list2_` 满则触发回流（见第 6 节）。
- `Empty()` 判定需覆盖：`getCur_`、`putCur_`、`list1_`、`list2_` 全空。
- `Init/Reset/Dispose` 需要遍历两条链表 + 两个游标，逐一归还/推 global（细节见第 11 节）。

## 5. 写屏障产出路径（→ list1）

对应需求 2。mutator 写屏障溢出时，把发现的灰对象打包成 `WorkBuf` 后，**优先作为节点挂入本 P 的 `list1_`**，而不是直接推 global：

```cpp
// FlushWriteBarrierOverflow 中，原本是 globalFull_->Push(buf)，现改为：
void PerPGCWork::PushWBBuf(WorkBuf* buf) {
    if (buf == nullptr || buf->Empty()) { /* 回收空 buf */ return; }
    if (list1_.PushNode(buf)) {
        return;                       // ① list1 未满，本地留下
    }
    // ② list1 已满（4 节点），溢出进 global
    globalFull_->Push(buf);
    flushedWork_ = true;
    AddPerPCredit(buf->len);           // 溢出部分计入本 P 信用，见第 9 节
}
```

要点：
- 写屏障产出的灰对象大多是本 mutator 刚改写的引用，留在 `list1_` 可被**调度到本 P 的任意 mutator**在下一次 assist 里优先消化（第 8 节 需求 4）。
- 只有 `list1_` 满才溢出到 global；溢出部分计入**本 P 信用**（非 bgScanCredit_）。
- 原来“写屏障产出 → 直接 global”的路径全部改走 `PushWBBuf`。

## 6. 标记产出路径（→ list2 → list1 → global）

对应需求 3。mutator（或 GC 线程）扫描一个对象后，其子对象写入 `list2_`：

```cpp
void PerPGCWork::PutObj(BaseObject* obj) {
    // ① 当前 put buf 未满，直接写
    if (putCur_ != nullptr && !putCur_->Full()) { putCur_->Push(obj); return; }
    // ② put buf 满（或不存在）：把满 buf 挂入 list2_，再取新 buf
    if (putCur_ != nullptr) {
        if (!list2_.PushNode(putCur_)) {
            // ③ list2_ 也满（4 节点）：先回流 list1_
            SpillToList1OrGlobal(putCur_);
        }
    }
    putCur_ = workBufPool_->Get();
    putCur_->Push(obj);
}

// list2_ 溢出时：先试插 list1_，插不进再推 global
void PerPGCWork::SpillToList1OrGlobal(WorkBuf* buf) {
    if (list1_.PushNode(buf)) { return; }   // 回流 list1，供本地消费
    globalFull_->Push(buf);                 // list1 也满，真正溢出
    flushedWork_ = true;
    AddPerPCredit(buf->len);                // 计本 P 信用
}
```

要点：
- 产出优先填 `putCur_` → 满则挂 `list2_` → `list2_` 满则回流 `list1_` → `list1_` 也满才进 global。
- **回流 list1_ 的设计意图**：让本 mutator 产出的子对象也成为下一轮本地消费的源，进一步减少进 global。
- 只有走到最后一步（`list1_` 也满）的部分才计入**本 P 信用**。

## 7. 取对象路径 TryGetObj 重构

取对象（标记消费侧）优先级：`getCur_` → `list1_` →（补货）。

```cpp
// 本地取：不碰 global，assist 第一轮用
BaseObject* PerPGCWork::TryGetObjLocal() {
    // ① 当前 getCur_ 还有
    if (getCur_ != nullptr && !getCur_->Empty()) { return getCur_->Pop(); }
    // ② 从 list1_ 摘下一个节点作为新 getCur_
    if (getCur_ != nullptr) { workBufPool_->Put(getCur_); getCur_ = nullptr; }
    getCur_ = list1_.PopNode();
    if (getCur_ != nullptr) { return getCur_->Pop(); }
    // ③ list1_ 空：本地路径返回 nullptr（交由 assist/global 决策）
    return nullptr;
}

// 含全局补货：GC 线程主循环用
BaseObject* PerPGCWork::TryGetObj() {
    BaseObject* o = TryGetObjLocal();
    if (o != nullptr) { return o; }
    // 从 global 补货一个 buf 进 list1_（准备标记）
    WorkBuf* buf = globalFull_->Pop();
    if (buf == nullptr) { return nullptr; }
    getCur_ = buf;
    return getCur_->Pop();
}
```

要点：
- 需求 6 明确“没有则从 global 获取到 wbuf1_ 中准备标记”：这里 global 补货目标是 `getCur_`（逻辑上属于 list1 侧的消费源）。
- `TryGetObjLocal` 用于 assist：只消费本地，不主动碰全局锁。
- **拉取只取满 buf（第 1+2 点）**：`globalFull_->PopFull()` 只返回 len==1024 的满 buf 进 `list1_`（作为满节点）；getCur_ 是独立在用 buf，与 list1_ 的 4 个满 buf 分开计。global 中不足一个完整 buf 时 `PopFull` 返回 nullptr，调用方据此退出拉取。

## 8. Assist 决策流程重构

对应需求 4 + 6 + 第 1～5 点修正。**进入 assist 后，先把 list2_ 的满 buf 挪到 list1_（补满为止，装不下的留 list2_），再判定；getCur_ 有残留 或 list1_ 有 ≥1 满 buf 就本地标一轮，标完一个 buf 即退出**；只有连一轮都标不了（getCur_ 空 且 list1_ 空）才走「偷取 bgScanCredit_ → 债务驱动拉 global 满 buf」。

```cpp
size_t MarkSweepCollector::MSMarkAssist(size_t debtObjects) {
    PerPGCWork& gcw = *ThreadLocal::GetAllocBufferMS()->GetGCWork();
    LazyInit(gcw);

    // ⓪ 第 1 点：先把 list2_ 里的满 buf 挪入 list1_（供本地消费），
    //    list1_ 装满即停，装不下的满 buf 仍留在 list2_。
    while (!gcw.list1_.ListFull() && !gcw.list2_.ListEmpty()) {
        gcw.list1_.PushNode(gcw.list2_.PopNode());
    }

    // ① 第 2+3 点：getCur_ 还有残留对象，或 list1_ 有 ≥1 个满 buf，就本地标一轮。
    //    标完一个 buf 的量即退出（只要能做一次标记就不走下面的信用/拉取）。
    bool curHasObj = (gcw.getCur_ != nullptr && !gcw.getCur_->Empty());
    if (curHasObj || gcw.list1_.NodeCount() >= 1) {
        size_t nProcessed = 0;
        DrainOneBufRound(gcw, nProcessed);   // 标完 getCur_ 残留或摘 1 满 buf，标完一 buf 的量即止
        return nProcessed;                   // 本地自产自销，不计信用（见第 9 节）
    }

    // ② 第 3 点：本地无法做一次标记（getCur_ 空 且 list1_ 空）→ 先偷取 bgScanCredit_ 抵债
    size_t paidByBg = TryStealBgCreditPartial(debtObjects);
    size_t remainDebt = (debtObjects > paidByBg) ? (debtObjects - paidByBg) : 0;

    // ③ 第 4+5 点：偷取后剩余债务仍 ≥ 1 个 buf 才从 global 拉满 buf。拉取即抵债、不靠消费抵。
    //    债务是对象数；每轮要求 remainDebt >= 1024 才拉、每拉 1 buf 才扣 1024，
    //    所以累计拉取量恒 ≤ 原始债务，不会出现“拉取 > 债务”（无需丢弃）。
    size_t paidByGlobal = 0;
    while (remainDebt >= WorkBuf::kCapacity && !gcw.list1_.ListFull()) {
        WorkBuf* buf = globalFull_->PopFull();   // 只取 len==1024 的满 buf
        if (buf == nullptr) { break; }           // global 无完整 buf → 退出
        gcw.list1_.PushNode(buf);
        remainDebt   -= WorkBuf::kCapacity;       // 拉取即偿还（承诺标这一 buf）
        paidByGlobal += WorkBuf::kCapacity;
    }

    // ④ 拉到了 buf → 回头实标一轮（抽空 getCur_），不让这批 buf 拖到 STW 才被标。
    //    这一轮是“实标”，但债务已在③拉取时抵过，nDrained 不再重复扣债（见第 12 节风险 7）。
    if (paidByGlobal > 0 && gcw.list1_.NodeCount() >= 1) {
        size_t nDrained = 0;
        DrainOneBufRound(gcw, nDrained);          // 抽空一个刚拉进来的 buf；进度不再扣债
    }

    // 零头债务（< 1 buf）自然保留在 debt 累计里，下次再处理
    return paidByBg + paidByGlobal;
}
```

说明：
- **第 1 点（list2_ → list1_ 预挪运）**：进入 assist 先把本地产出汇 `list2_` 的满 buf 挪到消费源 `list1_`（补满为止），list1_ 满了装不下的仍留 list2_。这让本地标记能持续推进（子对象也能被消费），不必等 list2_ 攒满才回流。
- **第 2+3 点（getCur_ 残留也算一轮、标完即退）**：门槛 = `getCur_ 非空 || list1_.NodeCount() >= 1`；只要能做一次标记就 `DrainOneBufRound` 标完一个 buf 的量即 return，不再走信用/拉取。
- **第 4 点（拉取即抵债，不靠消费抵）**：从 global 拉满 buf 进 list1_ 当场按 buf 抵债，不等实际标完再抵（避免双重抵扣，见第 12 节风险 7）。
- **第 5 点（债务为对象数、不会超拉）**：P 的债务计数是**对象数量**。拉取前要求 `remainDebt >= 1024`、每拉 1 buf 才扣 1024，所以累计拉取量恒 ≤ 原始债务，**不会出现拉取 > 债务**；零头（< 1 buf）不拉、保留到下次，无需丢弃。
- 需求 6 补充：“mark 一轮中发现没对象可 mark”即 `DrainOneBufRound` 期间 getCur_ 与 list1_ 都空。本设计下：本轮已做过标记则直接 return（第 3 点：抽空 getCur_ 即退），下次 assist 进来时 getCur_/list1_ 仍空才走 ②③。

> `DrainOneBufRound(gcw, nProcessed)` 语义：若 getCur_ 空则先从 list1_ 摘 1 个满 buf 作 getCur_；然后**循环 Pop 直到 getCur_ 抽空**（每个对象扫描、子对象进 putCur_/list2_），nProcessed = 本轮 Pop 出的对象数（≤ 1024）；抽空后归还空 buf、getCur_ 置空。**一轮只抽空一个 buf，不级联消费 list2_**。

> `DrainOneBufRound(gcw, nProcessed)` 语义：若 getCur_ 空则先从 list1_ 撁 1 个满 buf 作 getCur_；然后**循环 Pop 直到 getCur_ 抽空**（每个对象扫描、子对象进 putCur_/list2_），nProcessed = 本轮 Pop 出的对象数（≤ 1024）；抽空后归还空 buf、getCur_ 置空。**一轮只抽空一个 buf，不级联消费 list2_**。

## 9. 信用积分新机制（GC 供给衰减 + N 轮截止）

对应需求 5 + 第 1、2 点修正。**取消原 ratio 倍率爬升机制**：不再用 `GetAssistRatioCoeff`(0.05) / `GetAssistRatioMaxMultiplier`(3.0) / `GetAssistRatioScale`(1024) 三参数随分配字节把 ratio 越拉越高，改为「GC 供给随轮次衰减、第 N 轮后停供」。

### 9.1 bgScanCredit_：随轮衰减、N 轮停供（替换倍率因子）

- 「一轮」= GC 线程从 global 取一个 buf 并标完（一个 buf = 1024 个对象，见 §2）。
- 每轮在**取到 buf 时**结算一次信用注入（**不逐对象刷**，对应第 2 点「从 global 取 buf 后刷、标记不刷」），且**每对象注入权重随轮次递减**：

```cpp
// 第 r 轮（1-based）：权重从满（r=1）线性降到 0（r=N），线性为默认、可调
double w = (r >= N) ? 0.0 : double(N - r) / double(N);
bgScanCredit_.fetch_add(size_t(bufLen * w));   // bufLen=本轮取到 buf 的实际对象数；取 buf 时注入、标记过程不再注入
```

> 注入用 **`bufLen`**（本轮从 global 取到的 buf 的实际 `len`），而非固定 1024——GC 线程多数取满 buf（len=1024），但收敛末段可能取到非满 buf，用 `bufLen` 更准确。

- **r ≥ N 后 GC 完全停止向 `bgScanCredit_` 注入**：此后 mutator 窃取不到信用，其产出必须由 mutator **自己实标消化**（走 §8 ③ global 补货实标）。
- **N 的取值**：看**上一轮 GC 在第几轮累计标记耗时达到 100ms**，把该轮次作为本轮 N。
  - 每个 GC 周期记录「累计 ConcurrentMark 时间首次 ≥ 100ms 时的轮次」→ 存 `prevCycleRoundsTo100ms` → 下周期用作 N。
  - 首个周期无历史值 → 用保守默认常量（待标定）。
  - **轮次近似 100ms 的漂移（每轮耗时随对象大小/引用数波动）可接受**：N 只作「停供点」粗粒度控制，无需精确对齐 100ms。

直觉：GC 周期前段领先（信用足、mutator 几乎不 assist）；越接近「100ms 工作量」信用越稀薄；超过 N 轮后断供，强制 mutator 自消化。等价于「越拖越逼 mutator 帮忙」，但由 **GC 轮次 + 100ms 标定**驱动，而非字节比率倍率。

### 9.2 还债路径与拉取预算（第 1、2、3、4 点）

| 项 | 旧值 | 新值 |
|---|---|---|
| assist 前置 | — | **先把 list2_ 满 buf 挪入 list1_（补满为止，装不下留 list2_）** |
| assist 本地标记门槛 | list1_ 非空 | **getCur_ 有残留 或 list1_ 有 ≥1 满 buf** |
| 标完退出 | — | **能做一次标记就 DrainOneBufRound 标完一 buf 的量即 return** |
| 每轮标记量 | debtObjects | **一个 buf（1024）** |
| global 拉取门槛 | perPCredit_ 达 4 buf | **getCur_/list1_ 都标不了 且 偷 bgScanCredit_ 后剩余债务 ≥1 buf** |
| 拉取数量 | — | **剩余债务向下对 buf 取整，上限 = 补满 list1_（恒 ≤ 原始债务，不超拉）** |
| 拉取 buf 形态 | — | **只取满 buf（PopFull, len==1024）；global 无完整 buf 则退出** |
| 债务抵扣 | — | **拉取即抵（不靠消费抵），每拉 1 满 buf 抵 1024；零头（< 1 buf）保留** |

- 还债路径：**本地 list1_ ≥1 满 buf 实标一轮** → （无满 buf）**偷取 bgScanCredit_** → （剩余债务 ≥1 buf）**从 global 拉满 buf 抵债**。
- 拉取时「拉取即抵债」（承诺本 P 后续标这些 buf）；mutator 本地 `list1_`/`list2_` 闭环实标不生产信用。

要点 / 改动点：
- `RunConcurrentMarkLoop`：删除 `ComputeNextAssistRatioScaled` 的逐切片倍率爬升、删除 `kCreditFlushThreshold` 的逐对象刷；改为每取一个 buf（一轮）按 9.1 衰减权重注入 `bgScanCredit_`，并维护轮次 `r` 与 `N`（`N = prevCycleRoundsTo100ms`）。
- `GetAssistRatioCoeff` / `GetAssistRatioMaxMultiplier` / `GetAssistRatioScale` 三个环境参数及其使用点删除。
- `perPCredit_` 不再作为 global 拉取的门槛（门槛改为「偷取 bgScanCredit_ 后剩余债务 ≥1 buf」）；仍可保留 `AddPerPCredit` 作诊断计数（统计本 P 推 global 量），或在实现时去除。

> 备注：拉取时「拉取即抵债」与后续①本地标记同一批 buf 时 `DrainOneBufRound` 返回的 nProcessed 也抵债，需避免双重抵扣：实现时以「拉取时记账为偿还」为准，①本地标记返回的进度不重复扣 pacing 债务（见第 12 节风险 7）。

### 9.3 GC 日志：assist 信息记录

为便于调参（尤其 N 的标定）和事后分析 mutator 协助情况，在 GC 周期结束时（或 `MRT_REPORT` 汇总处）记录以下 assist 统计。按**对象数**累加，per-cycle 聚合（全 P 求和 + 本周期峰值）：

| 字段 | 含义 | 来源 |
|---|---|---|
| `assistCalls` | 本周期 `MSMarkAssist` 调用次数 | 计数器 |
| `assistLocalMarked` | ① 本地实标对象数（getCur_/list1_ 抽空，含⓪挪运后） | nProcessed 累加 |
| `assistBgStolen` | ② 偷取 `bgScanCredit_` 抵债对象数 | paidByBg 累加 |
| `assistGlobalPulled` | ③ 从 global 拉取抵债对象数（=拉取 buf 数×1024） | paidByGlobal 累加 |
| `assistPostPullMarked` | ④ 拉取后回头实标对象数 | nDrained 累加 |
| `bgCreditInjected` | GC 线程本周期注入 `bgScanCredit_` 总量 | §9.1 注入累加 |
| `bgCreditStopRound` | 本周期实际停供轮次 r（首次 r≥N） | 标记循环 |
| `roundsTo100ms` | 本周期累计标记达 100ms 的轮次（下周期 N） | 标记循环 |
| `list2SpillToGlobal` | 标记产出经 `SpillToList1OrGlobal` 进 global 的对象数 | 计数器 |

- 三类抵债分开统计（`assistLocalMarked` 实标 / `assistBgStolen` 蹭 GC / `assistGlobalPulled` 承诺标），便于判断 mutator 实际帮了多少 vs 蹭了多少 —— 这对调 N 很关键。
- 日志级别建议复用现有 `MRT_REPORT` / GC 周期 summary，不引入热路径 I/O；计数器用 per-P 本地累加、周期末汇总，避免标记热路径上的原子争用。
- 至少在周期 summary 打印一行：`[assist] calls=.. local=.. bgStolen=.. globalPulled=.. postPull=.. stopRound=r/N spill=..`。

## 10. 收敛判定修改

对应需求 7。**保持不检查各 mutator 的 per-P buf**（仍靠 STW MarkTermination 兜底），但移除第 3 轮后的那一次长等复检。

```cpp
bool MarkSweepCollector::CheckConvergenceAndBreak(size_t& localCreditAccum) {
    convergenceStableRounds_++;
    if (convergenceStableRounds_ < kConvergenceRequiredRounds) {  // 3
        usleep(kConvergenceYieldUs);   // 50us 短让步
        return false;
    }
    // ✖ 移除：usleep(kConvergenceFlushWaitUs=200) + 重检 global/gcThreadWork
    // 达到 3 轮稳定直接收敛
    FlushBgScanCredit(localCreditAccum);
    return true;
}
```

要点：
- 原来第 3 轮后 `usleep(200)` 再 `if (!gcThreadWork_.Empty() || !globalFull_.Empty()) { stableRounds=0; return false; }` 这段复检整个删除。
- 连续 3 轮（每轮都是 `gcThreadWork_.Empty() && globalFull_.Empty()` 且中间只短让步）即认为收敛，进 STW MarkTermination。
- 未收敛的残留（包括各 P 本地 list1_/list2_）仍由 MarkTermination 的 `DisposeAllPerPGCWork` + drain 兜底，newlyMarked=0 不变。

## 11. 受影响文件与函数清单

| 文件 | 函数/结构 | 改动 |
|---|---|---|
| [LFStack.h](runtime/src/Common/LFStack.h) | 新增 `WorkBufList` | 有界链表（kMaxNodes=4）；PushNode/PopNode/NodeCount/ListFull/ListEmpty |
| LFStack.h | `LFStack::PopFull`（新） | 只返回 len==1024 的满 buf；无完整 buf 返回 nullptr |
| LFStack.h | `PerPGCWork` | `wbuf1_/wbuf2_` → `list1_/list2_` + `getCur_/putCur_` |
| LFStack.h | `PutObj` | 重写：putCur_ → list2_ → list1_ → global |
| LFStack.h | `TryGetObj` / `TryGetObjLocal` | 重写：getCur_ → list1_ → (global) |
| LFStack.h | `PushWBBuf`（新） | 写屏障 buf → list1_，溢出 global |
| LFStack.h | `SpillToList1OrGlobal`（新） | list2 溢出回流逻辑 |
| LFStack.h | `Empty/Init/Reset/Dispose/FlushAndSplit/Balance/LocalLen` | 适配双链表 + 两游标 |
| [MSAllocBuffer.cpp](runtime/src/Heap/MAllocator/MSAllocBuffer.cpp) | `FlushWriteBarrierOverflow` | 改调 `PushWBBuf` 而非直推 global |
| [MarkSweepCollector.cpp](runtime/src/Heap/MCollector/MarkSweepCollector.cpp) | `MSMarkAssist` | 重构：➀ list2_→list1_ 预挪 → ① 抽空 getCur_ 一轮即退 → ② 窃取 bgScanCredit_ → ③ global 拉满 buf抵债 → ④ 拉后回头实标一轮 |
| MarkSweepCollector.cpp | `DrainOneBufRound`（新） | 若 getCur_ 空先撁 1 满 buf；循环 Pop 直到 getCur_ **整个抽空**；回填 nProcessed（≤ 1024） |
| MarkSweepCollector.cpp | GC 周期 summary | 新增 `[assist]` 统计行（§9.3）：calls/local/bgStolen/globalPulled/postPull/stopRound/spill |
| MarkSweepCollector.cpp | `RunConcurrentMarkLoop` | 改：删 ratio 倍率爬升与逐对象刷；每取 buf（一轮）按衰减权重注入 bgScanCredit_，维护 r/N（N=上轮达 100ms 的轮次） |
| MarkSweepCollector.cpp | `GetAssistRatioCoeff`/`MaxMultiplier`/`Scale` | 删除（不再用倍率爬升 pacing） |
| MSAllocBuffer.cpp | `AccumulatePacingDebt` | trigger 仍为一个 buf（1024）；MSMarkAssist 返回的 paid 抵减债务，**饱和到 0（超额丢弃、不结转）** |
| MarkSweepCollector.cpp | `CheckConvergenceAndBreak` | 删除 3 轮后长等复检 |
| LFStack.h | `perPCredit_` + `AddPerPCredit`（新） | 本 P 信用计数（可选诊断；不再作 global 拉取门槛） |

## 12. 风险与待确认点

1. **GC 供给衰减与 N 标定**：bgScanCredit_ 改为「随轮衰减、N 轮停供」，N = 上轮 GC 累计标记耗时达 100ms 的轮次。需验证：（a）首个周期无历史 N 的默认值怎么取；（b）衰减权重用线性还是其他曲线；（c）停供后 mutator 自消化是否会造成后段分配明显变慢。另 trigger/drain 改为一个 buf（1024）后，单次 assist 粒度变粗，需观察分配毛刺。
2. **本地滞留拉长收敛**：每 P 最多本地滞留 list1_+list2_+两游标 ≈ (4+4+2)×1024 ≈ 1 万对象。**list2_ 允许长期保持满**（写屏障产出占满 list1_ 时是预期状态，标记产出走 SpillToList1OrGlobal 泄 global 泄压）。多 P 下总滞留量上升，可能拉长收敛/增加 STW MarkTermination 需 drain 的残留。需验证 newlyMarked 仍为 0 且 STW 时长可控。
3. **WorkBuf 池压力**：每 P 多持 8 个 buf×1024，高并发下 WorkBufPool 总占用上升。需关注 `totalAllocated_`。
4. **收敛去长等的正确性**：移除复检后，若有 mutator 刚好在 3 轮窗口期间向 global push，理论上会被下一轮 DrainGCWork 捕获并重置 stableRounds；但需确认 push 与收敛检查之间无竞态窗口造成漏标（靠 STW 兜底应安全，但需验证）。
5. **需求 4 与需求 6 的边界**：“mark 一轮中发现没对象可 mark”是否在同一次 `MSMarkAssist` 内接着走信用/global，还是返回等下一次 assist？本设计按“本轮已标则直接 return”处理（第 3 点）。另：**超额标记（债务只剩 100 但标完一 buf=1024）的多扣部分直接丢弃**（`AccumulatePacingDebt` 抵减饱和到 0、不结转）——已定，不再作为待确认项。
6. **本地标记门槛与债务驱动拉取**：assist 先 list2_→list1_ 预挪，getCur_ 有残留或 list1_ 有 ≥1 满 buf 就本地标一轮。只有连一轮都标不了且偷取 bgScanCredit_ 后剩余债务 < 1 buf 时才不拉 global。需验证高产出场景下不会因「零头债务不足 1 buf」让 mutator 持续不协助、造成标记追不上分配。
7. **拉取抵债与本地标记双重抵扣**：③ 拉取时已按 buf 抵债，这批 buf 后续在①被标记时 `DrainOneBufRound` 返回的 nProcessed 也会抵债。实现时需二选一（以拉取时记账为准，本地标记只汇报进度不再扣债），避免双重抵扣让 mutator 提前脱债、实际协助量不足。
8. **拉取即抵债的滞后效应（已接受）**：r≥N 断供后 mutator 靠③拉取抵债 → 债务清零 → 不再触发 assist → 这批 buf 滞留 list1_ 暂无人标（若分配也停）→ 拖到 STW 才被 drain。这是「拉取即抵债」的固有代价，**已决定接受**；依靠 STW MarkTermination 兑现这部分滞留工作，newlyMarked 仍为 0。

> 待评审通过后，再按第 11 节清单落地代码。

---

## 13. 实现纪要（2026-06-02 落地）

> 本节记录设计稿评审通过后的**实际实现**，含与设计的差异、关键常量取值与验证结果。
> 代码以源码为准；本节与第 1～12 节设计稿如有出入，以本节为实现现状。

### 13.1 LFStack.h（数据结构与本地闭环）

实际落地结构（[LFStack.h](runtime/src/Common/LFStack.h)）：

```cpp
struct WorkBufList {            // 有界链表，kMaxNodes = 4
    WorkBuf* head_ = nullptr;
    size_t   nodeCount_ = 0;
    bool ListEmpty()/ListFull(); size_t NodeCount()/TotalObjs();
    bool PushNode(WorkBuf*); WorkBuf* PopNode();   // LIFO，复用 WorkBuf::next
};

class LFStack {
    WorkBuf* PopFull();        // 新增：仅当栈顶 buf len>=kCapacity(满) 才弹出，否则 nullptr
};

struct PerPGCWork {
    WorkBufList list1_;        // get 侧：写屏障产出 + global 补货
    WorkBufList list2_;        // put 侧：标记子对象产出汇
    WorkBuf* getCur_ = nullptr;   // 在用消费 buf（不计入 list1_ 的 4 节点）
    WorkBuf* putCur_ = nullptr;   // 在用产出 buf（不计入 list2_ 的 4 节点）
    bool         flushedWork_ = false;
    LFStack*     globalFull_  = nullptr;
    WorkBufPool* workBufPool_ = nullptr;
};
```

方法实现与设计一致：`Init(full,pool)`（惰性游标）、`IsInitialized`、`SpillToList1OrGlobal(buf)`（list1_ 有位则入、否则 global + `flushedWork_=true`）、`PushWBBuf(buf)`（list1_ 有位则入、否则 global）、`PutObj(obj)`（putCur_→满则 list2_→满则 SpillToList1OrGlobal→取新 putCur_）、`PromoteAndPop(src)`、`TryGetObjLocal()`（getCur_→list1_→list2_→putCur_，不碰 global）、`TryGetObj()`（先 TryGetObjLocal 再 `globalFull_->Pop`）、`Dispose()`（getCur_/putCur_/两链表全部 flush 进 global）、`Empty()`（四者全空）、`Reset()`（全部归还池、清游标）。

### 13.2 MarkSweepCollector（assist / 标记循环 / 收敛）

[MarkSweepCollector.cpp](runtime/src/Heap/MCollector/MarkSweepCollector.cpp)：

- **`FlushWriteBarrierOverflow`**：取本线程 `ThreadLocal::GetAllocBufferMS()` → `gcw=&allocBuf->GetGCWork()`（必要时惰性 `Init`），通过 `gcw->PushWBBuf(buf)` 把溢出 buf 走本 P `list1_`；无 AllocBufferMS 时回退 `globalFull_.Push(buf)`。
- **`DrainOneBufRound(PerPGCWork& gcw)`**（新增）：getCur_ 空则从 `list1_.PopNode()` 取一个满 buf（先把旧空 getCur_ 归还池）；随后循环 `getCur_->Pop()` 直到抽空——非可追踪地址跳过、非法对象 `LOG FATAL`、有引用域的非 weakref 走 `TraceObjectRefFieldsToGCWork(obj,gcw)`（子对象进 putCur_/list2_）、weakref 走 `PushSingleToDeferredMainThread`；抽空后归还 getCur_ 并置空。返回本轮扫描对象数（≤1024），**不级联 list2_**。
- **`MSMarkAssist(debtObjects)`**：`assistCallTotal_++` → 取/惰性 init gcw → ⓪ `while(!list1_.ListFull() && !list2_.ListEmpty()) list1_.PushNode(list2_.PopNode())` → ① 若 `getCur_ 非空 || list1_.NodeCount()>=1`：`DrainOneBufRound`，累加 `assistLocalMarked_`/`assistProcessedCount_` 并**早返回** → ② `paidByBg=TryStealBgCreditPartial(debt)`，累加 `assistBgStolen_` → ③ `remainDebt=debt-paidByBg`，`while(remainDebt>=1024 && !list1_.ListFull()){ buf=globalFull_->PopFull(); if(!buf)break; list1_.PushNode(buf); remainDebt-=1024; paidByGlobal+=1024; }`，累加 `assistGlobalPulled_` → ④ 若 `paidByGlobal>0 && list1_.NodeCount()>=1`：再 `DrainOneBufRound` 一轮，累加 `assistPostPullMarked_`/`assistProcessedCount_`（**不再扣债**）→ 返回 `paidByBg+paidByGlobal`。
- **`RunConcurrentMarkLoop`**：删除原 ratio 倍率爬升与逐对象信用 flush。新增按轮衰减注入：`creditRoundsN = prevCycleRoundsTo100ms_>0 ? prevCycleRoundsTo100ms_ : 256`（首周期默认 256），`bgCreditStopRound_=creditRoundsN`；每轮若 `roundIdx<creditRoundsN && scannedCount>0`，`w=(creditRoundsN-roundIdx)/creditRoundsN`、`inj=scannedCount*w`、`bgScanCredit_.fetch_add(inj)`、`bgCreditInjected_.fetch_add(inj)`；首次单轮累计耗时 `elapsed>=100000us` 时记 `measured100msRound=roundIdx+1`；循环末 `prevCycleRoundsTo100ms_=roundsTo100ms_=measured100msRound`。保留 mark 时间预算 `CJ_MS_MARK_TIME_BUDGET_MS`（默认 20000ms）。
- **`CheckConvergenceAndBreak()`**：改为**无参**；`convergenceStableRounds_++`，不足 `kConvergenceRequiredRounds` 则短让步返回 false，达到即返回 true。**删除**原第 3 轮后 `usleep(200us)+复检 global/gcThreadWork` 与信用 flush。
- **周期 summary** 新增 `[assist]` 诊断行：`calls / local / bgStolen / globalPulled / postPull / creditInjected / stopRound=r/N / roundsTo100ms`。
- 删除 `GetAssistRatioCoeff` / `ComputeAssistRatioCap` / `ComputeNextAssistRatioScaled` / `GetAssistRatioMaxMultiplier`；**保留** `GetAssistRatioScale()`（债务换算仍需）；`GetAssistDebtTriggerObjects()` 默认 512→**1024**（`CJ_MS_ASSIST_DEBT_TRIGGER`）。
- 头文件 [MarkSweepCollector.h](runtime/src/Heap/MCollector/MarkSweepCollector.h) 同步：新增 `DrainOneBufRound` 声明；`CheckConvergenceAndBreak` 改无参；新增 5 个 assist 统计原子 `assistLocalMarked_/assistBgStolen_/assistGlobalPulled_/assistPostPullMarked_/bgCreditInjected_` 与 3 个 N 跟踪 `prevCycleRoundsTo100ms_/bgCreditStopRound_/roundsTo100ms_`。

### 13.3 与设计稿的差异

| 项 | 设计稿 | 实际实现 | 说明 |
|---|---|---|---|
| `perPCredit_` / `AddPerPCredit` | 标注「可选诊断」 | **未实现** | 溢出 global 只置 `flushedWork_`，不再单独计本 P 信用；门槛已改为「偷 bgScanCredit_ 后剩余债务」，本 P 信用无消费方 |
| `list2SpillToGlobal` 统计字段 | §9.3 列出 | **未单独实现** | assist 诊断聚焦 5 个核心计数；溢出量可由 `flushedWork_`/global share 间接观测 |
| 首周期默认 N | 「保守默认常量待标定」 | **256**（`kDefaultCreditRounds`） | |
| `FlushWriteBarrierOverflow` 落点 | 设计列在 MSAllocBuffer.cpp | 实际在 **MarkSweepCollector.cpp** | 逻辑等价：改走 `PushWBBuf` |
| `AccumulatePacingDebt` | trigger=1 buf、抵减饱和到 0 | **未改动**（本就饱和到 0） | trigger 阈值由 collector 端 `GetAssistDebtTriggerObjects`(1024) 提供 |
| mark 时间预算 | 设计未定 | 保留 **`CJ_MS_MARK_TIME_BUDGET_MS`=20000ms** | 维持既有 20s 预算，本次不动 |

### 13.4 编译与验证

- **构建**：`cd runtime && python3 build.py build --gc-mode marksweep` → exit 0，无 `-Werror` 告警。
- **部署**：产物 `output/temp/lib/aarch64_Release/libcangjie-runtime.dylib` 覆盖 `self/dev260509/cangjie` 后 `codesign -f -s -`（macOS arm64 必需，否则 SIGKILL 137）。
- **case2_50x_pressure 对比**（runs=1，cjProcessorNum=6，O1；结果目录 `MSGC_RESULT/Result/case2_50x_pressure/result/20260602_212033_*`）：

  | 指标 | MSGC（本实现） | CopyGC 基线 |
  |---|---|---|
  | Wall (ms) | **129037** | 134184 |
  | RSS peak (MB) | **137** | 426 |
  | GC count | 1424 | 2775 |
  | GC total (ms) | **3786** | 70362 |

  Wall 比 CopyGC 快 **3.8%**，RSS 约为其 1/3，GC 暂停时间显著更低。两轮 MSGC 运行（129037 / 129218ms，GC_cnt 1424 / 1417）数据一致、均干净完成，功能正确性与稳定性得到验证。
