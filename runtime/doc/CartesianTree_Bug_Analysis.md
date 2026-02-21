# CartesianTree Bug 分析与修复记录

**日期**：2026年2月21日  
**状态**：修复已提交，待完整验证

---

## 1. 问题现象

替换编译后的 `libcangjie-runtime.dylib` 到 SDK 后，运行 `gc.cj` 测试套件时程序崩溃：

- **Test 1** (Binary Tree Churn, depth=18, 200 rounds) 在 round 20~30 时崩溃
- 信号：`illegal hardware instruction` (exit code 132) 或 `SIGSEGV` (exit code 139)
- 崩溃栈帧：`CartesianTree::MergeInsertInternal`，访问地址 `0x10`（近 NULL 解引用）

### 崩溃日志

```
Thread "gc-main-thread" catched unhandled SIGSEGV (Segmentation fault: 11)
signal pc: 0x104f5893c, addr: 0x10
  #0  0x104f5893c in _ZN12MapleRuntime13CartesianTree19MergeInsertInternalEjjb
      from libcangjie-runtime.dylib
```

## 2. 定位过程

### 2.1 创建最小化测试 (`gc_minimal.cj`)

将测试分为递进的 Phase A~G，逐步加大 GC 压力：

| Phase | 描述 | 结果 |
|-------|------|------|
| A | 小对象 (depth=10) | PASS |
| B | 中等树多轮 (depth=14, 10 rounds) | PASS |
| C | 大树单次 (depth=18) | PASS |
| D | 大树多轮 (depth=18, 5 rounds) | PASS |
| E | 大对象分配 (128KB~1MB) | PASS |
| F | 大对象 + GC 压力 (10 rounds) | PASS |
| **G** | **持续 GC 压力 (depth=18, 50 rounds)** | **CRASH at round 0** |

Phase G 在第一轮就导致 SIGSEGV，说明问题出在多次 GC 周期后的树结构破坏。

### 2.2 分析崩溃函数

崩溃在 `CartesianTree::MergeInsertInternal()` 中解引用了一个已释放的节点（悬空指针）。

`addr: 0x10` 正好是 `Node` 结构体中 `index` 字段的偏移（跳过 `l` 和 `r` 两个 8 字节指针），说明某个 NULL 指针的 `->GetIndex()` 被调用。

### 2.3 根因：`TakeUnitsLowAddrImpl` 中的指针语义错误

**文件**：`src/Heap/Allocator/CartesianTree.h`

这是我们在 LOS M3 中新增的函数，用于从最低地址分配空闲单元。

**错误代码**：

```cpp
bool TakeUnitsLowAddrImpl(Count num, Index& idx, bool refreshRegionInfo)
{
    Node* node = FindLowestSuitable(root, num);  // 返回 Node* 局部变量
    // ...
    if (node->GetCount() == 0) {
        RemoveZeroNode(node);   // ❌ 传入局部变量！
    } else {
        LowerNonZeroNode(node); // ❌ 传入局部变量！
    }
    return true;
}
```

**`RemoveZeroNode` 和 `LowerNonZeroNode` 接收 `Node*&`（引用）**：

```cpp
void RemoveZeroNode(Node*& n);         // 需要修改树内部的指针
void LowerNonZeroNode(Node*& n) const; // 需要修改树内部的指针
```

当传入局部变量 `node` 时，`RemoveZeroNode` 修改的是栈上的局部变量，而不是树中父节点的 `l` / `r` 指针。结果：
1. 节点被 `nodeAllocator.Deallocate()` 释放
2. 但父节点的 `l` 或 `r` 仍然指向已释放的内存
3. 后续 `MergeInsertInternal` 遍历树时访问悬空指针 → SIGSEGV

**对比正确的 `TakeUnitsImpl`**：使用 `ForwardIterator` 获取 `Node**`，直接操作树内部指针。

## 3. 修复方案

将 `FindLowestSuitable(Node*)` 改为 `FindLowestSuitablePtr(Node**)`，返回指向树内部指针的指针：

```cpp
// 修复前：返回 Node*（无法通过它修改树结构）
Node* FindLowestSuitable(Node* node, Count num) const;

// 修复后：返回 Node**（指向树内部的 l/r/root 指针）
Node** FindLowestSuitablePtr(Node** nodePtr, Count num) const;
```

**修复后的 `TakeUnitsLowAddrImpl`**：

```cpp
bool TakeUnitsLowAddrImpl(Count num, Index& idx, bool refreshRegionInfo)
{
    Node** nodePtr = FindLowestSuitablePtr(&root, num);  // ✅ 树内部指针
    if (nodePtr == nullptr) {
        return false;
    }
    Node* node = *nodePtr;
    idx = node->GetIndex();
    auto count = node->GetCount();
    node->UpdateNode(idx + num, count - num, refreshRegionInfo, baseIndex);
    DecTotalCount(num);
    if (node->GetCount() == 0) {
        RemoveZeroNode(*nodePtr);   // ✅ 传入树内部指针的引用
    } else {
        LowerNonZeroNode(*nodePtr); // ✅ 传入树内部指针的引用
    }
    return true;
}
```

## 4. 修复后验证（部分）

修复后重新编译，运行 `gc_quick.cj`：

| Phase | 描述 | 修复前 | 修复后 |
|-------|------|--------|--------|
| A~F | 小对象到大对象混合 | PASS | PASS |
| G | 持续 GC 压力 (depth=18, 20 rounds) | **CRASH round 0** | 运行中（未超时完成） |

Phase G 不再崩溃，但因 depth=18 树分配量大（262143 nodes × 20 rounds），运行时间较长，超时被中断。

## 5. 待完成事项

> 以下内容计划次日继续处理。

### 5.1 完成 gc_quick.cj 全量验证
- 给足够的超时时间运行 `gc_quick.cj`（Phase G+H），确认 `ALL PHASES PASSED`
- 或减小 Phase G 的 depth/rounds 加速验证

### 5.2 运行完整 gc.cj 测试套件
- 用修复后的 runtime 运行完整 `gc.cj`（11 个 test case）
- 逐个验证 Test 1~10 的正确性
- Test 11 (OOM) 预期崩溃，确认是 OOM 而不是 SIGSEGV

### 5.3 GC 日志分析
- 检查 FRAG-EXT/INT/LARGE 和 MEMORY-MAP 输出是否正确
- 特别关注 Test 7~10（碎片化测试）中 LOS 区域是否有大对象出现
- 当前观察到 `MigrateLargeObjectsToLOS time: 0us`（大对象迁移未发生），需分析原因

### 5.4 考虑补充的测试场景
- 专门触发 `TakeUnitsLowAddr` 的测试（LOS 分配 + 回收 + 再分配）
- LOS Compact 路径的测试
- Reserve 借用/归还路径的测试

### 5.5 代码审查
- 检查其他使用 `Node*` 局部变量调用 `RemoveZeroNode` / `LowerNonZeroNode` 的地方
- 确认 `AllocateLowestAddressFromNode(Node*& node, ...)` 的调用处传入的是树内部指针

---

## 6. 关键经验

**CartesianTree 的 `Node*&` 语义陷阱**：

树操作函数 (`RemoveZeroNode`, `LowerNonZeroNode`, `MaintainHeapPropertyForZeroNode`) 都接收 `Node*&`，语义要求传入的是**树结构内部的指针**（如 `root`、`node->l`、`node->r`），而非栈上的局部 `Node*` 变量。传入局部变量会导致树操作"成功"（节点被释放/旋转），但树结构不更新，产生悬空指针。

**修复模式**：任何需要操作树结构的函数都应使用 `Node**` 来定位节点，确保修改通过指针链传播到树内部。
