# 2026-07-09 case27 `ref/static-ref` inline Route-A 顺序漏洞修复

## 根因

`CJ_MCC_WriteRefFieldFast_MSGC` / `CJ_MCC_WriteStaticRefFast_MSGC` 的 inline fast path
此前是：

1. 对 `oldRef` 先做 inline `mark-CAS`
2. 再去取 `activeLog_`
3. 再检查 `len < capacity`
4. 若 `activeLog_ == null` 或已满，则 slow-fall 到 wrapper

但当前 runtime 的 slow path 是 Route-A 语义：

- `ShadeRefForBarrier(old)` 只有 **白 -> 灰** 的赢家才返回 `true`
- 已经被 inline 标灰的对象，slow path 再进来时会被当成“已灰”直接丢弃，不再入 log

于是出现漏洞：

- inline 先把对象标灰
- 随后因为 `activeLog_` 不可用而跳 slow
- slow path 因为对象已经 marked，不再 enqueue
- 最终形成“**marked 但未 scan**”的坏状态

这会破坏 Route-A 的核心不变量：

> 任何赢得 mark-CAS 的对象，都必须保证后续一定会被 scan。

## 修复

把 `ref/static-ref` inline 路径改成：

1. `old == null` / `old == new` 等廉价 bail-out 先做
2. `activeLog_` 获取与 `len < capacity` 检查前移
3. 只有确认当前 inline 路径**确实能 push** 时，才执行 `LDSETAL` mark-CAS
4. mark 胜出后直接写 `activeLog_->objs[len++]`

这样就不会再出现“先 marked、后 slow-fall 丢 enqueue”的窗口。

## 验证

已通过以下编译器回归：

- `llvm/test/CodeGen/AArch64/cangjie-writebarrier-msgc-fastpath.ll`
- `llvm/test/CodeGen/AArch64/cangjie-writebarrier-msgc-ref-no-inline.ll`
- `llvm/test/CodeGen/AArch64/cangjie-writebarrier-msgc-struct-no-inline.ll`

并且新的 `fastpath` 汇编顺序已满足：

- `activeLog_` / `len` 检查在前
- `ldsetal` 在后

## 备注

- `struct/static-struct` 当前实现没有同样的问题，因为它们本来就在循环前先完成了
  `activeLog_` 与容量检查。
- `arraycopy` 当前已直接 slow-call，不是这次 case27 的主因。
