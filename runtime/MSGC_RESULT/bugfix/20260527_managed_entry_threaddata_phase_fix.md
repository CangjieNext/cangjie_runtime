# MSGC managed entry stale threadData/x28 phase 问题分析与修复方案

**日期**: 2026-05-27  
**问题类型**: MSGC aarch64 managed 入口 phase 不一致  
**影响范围**: `MRT_USE_MARK_SWEEP` + `__aarch64__`  
**核心症状**: 进入 managed 代码时，`threadData` 或 `x28` 携带的高位 phase 过期，导致编译器 fast path / runtime 入口看到错误 GC phase，进一步引出漏屏障、错误分派或随机崩溃。

---

## 1. 问题结论

这次问题的本质不是“GC phase 变化时没有更新 TLS phase”，而是：

1. phase 变化时，`ThreadLocalData::encodedGCPhase` 实际上已经会被更新；
2. 但进入 managed 代码时，调用链手里拿着的 `threadData` 可能是**更早缓存下来的旧值**；
3. 对 CJThread 来说，保存在线程上下文里的 `x28` 也可能是**park 时快照下来的旧值**；
4. 这些旧快照不会因为 TLS 内存后来被更新而自动同步。

因此，修复不能只放在 GC phase 切换点，还必须覆盖：

1. native/runtime -> managed 的入口边界；
2. CJThread context -> managed 的恢复边界。

---

## 2. 机制背景

### 2.1 aarch64 下的 `threadData` 编码

`MRT_GetThreadLocalData()` 在 aarch64 上返回的不是纯裸指针，而是：

1. 低 56 bit: `ThreadLocalData*` 地址；
2. 高 8 bit: `encodedGCPhase`。

对应实现：

- `src/Mutator/Mutator.cpp`
- `src/Mutator/ThreadLocal.h`
- `src/Mutator/Mutator.h`

关键逻辑：

```cpp
extern "C" uintptr_t MRT_GetThreadLocalData()
{
    return MRT_UpdateThreadLocalDataPhase(reinterpret_cast<uintptr_t>(ThreadLocal::GetThreadLocalData()));
}

extern "C" uintptr_t MRT_UpdateThreadLocalDataPhase(uintptr_t tlDataAddr)
{
#if defined(__aarch64__)
    tlDataAddr &= 0x00FFFFFFFFFFFFFFULL;
    auto* local = reinterpret_cast<ThreadLocalData*>(tlDataAddr);
    tlDataAddr |= (static_cast<uint64_t>(local->encodedGCPhase) << 56);
#endif
    return tlDataAddr;
}
```

### 2.2 phase 切换时 TLS 本来就会更新

MSGC phase 迁移时并不是完全没更新 TLS：

1. `Mutator::SetMutatorPhase()` 会更新 `gcFastState` 和 `encodedGCPhase`；
2. `MutatorManager::TransitionAllMutatorsToGCPhase()` 会在 MSGC 下预发布 phase 到 mutator TLS；
3. `Mutator::PreparedToRun()` 会在 mutator 恢复运行时，再按当前 `mutatorPhase` 刷一次 TLS phase。

所以问题不在“TLS phase 从未写入”，而在“旧快照值仍然在被继续使用”。

---

## 3. 为什么只在 GC phase 变化时设置还不够

### 3.1 C/C++ 调用链里的 stale `threadData`

多个 managed 入口原先都是这种顺序：

```cpp
uintptr_t threadData = MRT_GetThreadLocalData();
MRT_PreRunManagedCode(mutator, 0, reinterpret_cast<ThreadLocalData*>(threadData));
ExecuteCangjieStub(..., reinterpret_cast<void*>(threadData), ...);
```

问题在于：

1. `threadData` 在调用 `MRT_PreRunManagedCode()` 之前就已经取出来；
2. `MRT_PreRunManagedCode()` 里面会 `LeaveSaferegion()`；
3. `LeaveSaferegion()` -> `PreparedToRun()` 会刷新 TLS phase；
4. 但调用者手里的 `threadData` 还是旧的那一份。

也就是说，即使当前线程自己在 `PreRunManagedCode()` 期间已经把 TLS phase 刷新了，后面传给 `ExecuteCangjieStub()` 的仍然可能是 stale phase。

### 3.2 CJThread 上下文里的 stale `x28`

对 aarch64 CJThread 来说，`x28` 会被直接保存到调度上下文里，再在恢复时原样恢复：

1. `ContextSave` 保存 `x28`；
2. `ContextResume` 恢复 `x28`。

如果某个 CJThread 在 phase 变化前 park，下次恢复时拿到的还是旧 `x28`。这和 TLS 当前内存里已经是新 phase 并不矛盾，因为 context 保存的是历史快照，不会自动回填。

所以：

1. phase 变化点只能更新“当前 TLS 内存”；
2. 不能自动修复“已缓存的 threadData”；
3. 也不能自动修复“已保存的 x28 context 快照”。

---

## 4. 为什么 CopyGC 没有同类问题

CopyGC 没暴露这个问题，不是因为它在 phase 切换上更完整，而是因为它**不依赖 MSGC 这套 phase-tagged TLS fast path 语义**。

当前代码里有三个直接证据：

1. `MRT_GetManagedEntryThreadData()` 只在 `MRT_USE_MARK_SWEEP && __aarch64__` 下才刷新 phase；
2. `NormalizeRuntimeTypeInfo()` 等高位归一化逻辑也只在 `MRT_USE_MARK_SWEEP` 下启用；
3. MSGC 的编译器/运行时 fast path 直接消费 `encodedGCPhase` / `gcFastState`，而 CopyGC 不走同一套约束。

所以这次 bug 的触发前提是：

1. 使用 MSGC；
2. 使用 aarch64；
3. managed 入口 fast path 依赖高位 phase-tagged `threadData` / `x28`。

CopyGC 没满足这一组条件，因此不会以同样形式暴露。

---

## 5. 当前已落地修复

### 5.1 managed stub 调用前统一刷新 `threadData`

新增统一 helper：

- `src/RuntimeConfig.h`

```cpp
static inline uintptr_t MRT_GetManagedEntryThreadData(uintptr_t threadData)
{
#if defined(MRT_USE_MARK_SWEEP) && defined(__aarch64__)
  return MRT_UpdateThreadLocalDataPhase(threadData);
#else
  return threadData;
#endif
}
```

然后在以下 managed 入口调用点中，在 `MRT_PreRunManagedCode()` 之后统一刷新：

1. `src/CangjieRuntimeApi.cpp`
2. `src/CjScheduler.cpp`
3. `src/ExceptionManager.cpp`
4. `src/Heap/Collector/FinalizerWorkList.cpp`
5. `src/Loader/CjFileLoader/CjFileLoader.cpp`

这修掉的是“调用方缓存了 stale `threadData`”的问题。

### 5.2 CJThread 恢复前统一刷新 `context.x28`

新增：

- `src/CJThread/src/runtime/schedule/src/cjthread.cpp`
- `src/CJThread/src/runtime/schedule/include/schedule.h`

```cpp
void CJThreadExecuteManaged(struct CJThread *nextCJThread, void **gCJThread, uintptr_t threadData)
{
#if defined(MRT_USE_MARK_SWEEP) && defined(__aarch64__)
    nextCJThread->context.x28 = MapleRuntime::MRT_UpdateThreadLocalDataPhase(threadData);
#else
    (void)threadData;
#endif
    CJThreadExecute(nextCJThread, gCJThread);
}
```

并让 `processor.cpp` / `cjthread.cpp` 的 managed 恢复路径统一走这个入口。

这修掉的是“恢复的 CJThread context 里保存了 stale `x28`”的问题。

---

## 6. 更优的统一修复方案

当前修复已经正确，但仍有一个维护问题：新增 native/runtime -> managed 入口时，调用方需要记得再补一次 `MRT_GetManagedEntryThreadData()`。

更好的收口方式是把逻辑下沉到**公开 managed entry stub** 本身。

### 6.1 推荐下沉位置

优先级最高的统一点：

1. `src/arch/aarch64_linux/N2CStub.S` 中的 `ExecuteCangjieStub`
2. `src/arch/aarch64_macos/N2CStub.S` 中的 `ExecuteCangjieStub`
3. 同文件中的 `ApplyCangjieMethodStub`
4. `src/arch/aarch64_linux/ExclusiveScope.S` 中的 `ExecuteExclusiveCangjieStub`

这些 stub 当前只是把来参 threadData 直接塞进 `x28`，没有自己刷新 phase。

### 6.2 推荐做法

在 stub 内部统一执行：

1. `MRT_UpdateThreadLocalDataPhase(threadData)`；
2. 再把返回值写入 `x28`。

这样后续新增绝大多数 managed 入口，只要复用这些公开 stub，就不需要在 C++ 调用点手工补 `MRT_GetManagedEntryThreadData()`。

### 6.3 为什么不能只保留一个总闸口

即使把 stub 统一好了，CJThread 恢复路径仍然不能删除。

原因：

1. 协程恢复不一定经过 `ExecuteCangjieStub`；
2. 它的问题来自 context 里保存的旧 `x28`；
3. 这个问题只能在 resume 边界处理。

因此更合理的最终形态是两个统一点：

1. **stub 入口统一点**: 负责 direct managed entry；
2. **CJThread 恢复统一点**: 负责 saved context resume。

---

## 7. 推荐的后续收敛步骤

### Step 1

保留当前 `CJThreadExecuteManaged()`，不动。

### Step 2

把 `ExecuteCangjieStub` / `ApplyCangjieMethodStub` / `ExecuteExclusiveCangjieStub` 内部统一改成在写 `x28` 前调用 `MRT_UpdateThreadLocalDataPhase()`。

### Step 3

完成后，回头梳理并删除 C++ 调用点里冗余的 `MRT_GetManagedEntryThreadData()`，只保留少数不走统一 stub 的特殊路径。

### Step 4

验证 case17 / 相关 correctness benchmark：

1. managed direct call 路径正常；
2. CJThread park/resume 路径正常；
3. 不再出现 stale phase 导致的崩溃面迁移。

---

## 8. 最终建议

结论上，这个问题最容易误判成“phase 迁移时忘记更新 TLS”。但从代码机制上看，真正的 bug 是：

1. stale `threadData` 快照被继续传入 managed stub；
2. stale `x28` 快照被从 CJThread context 恢复回来。

因此：

1. 当前修复方向是对的；
2. 更适合长期维护的方案是把 direct managed entry 的 phase 刷新下沉到公开 stub；
3. CJThread resume 仍需保留单独统一点，不能依赖 stub 统一兜底。

这两处合起来，才是这个问题比较稳定的最终修复形态。