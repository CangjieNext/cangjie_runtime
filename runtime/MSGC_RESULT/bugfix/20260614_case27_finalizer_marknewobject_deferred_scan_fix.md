# case27 finalizer 路径 marked-but-not-scanned 修复记录

## 基本信息

- 日期：2026-06-14
- 场景：`MSGC_RESULT/Result/case27_http_complex_json`
- 用例口径：`CASE27_CONCURRENCY=8`，`CASE27_PAYLOAD=100K`
- SDK：
  - MSGC：`/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260609/cangjie`
  - CopyGC：`/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie`
- 本次修改文件：
  - `src/Heap/MCollector/MarkSweepCollector.cpp`

## 问题背景

case27 的 `c8 / p100K` 在 MSGC 下此前出现过两类异常：

1. 进程级异常：历史回归中出现过 `SIGSEGV`、`SIGABRT` 或中途异常退出。
2. 正确性异常：即使进程未崩，也会出现 client 端报
   `The json data is Non-standard, please check:`，说明服务端偶发返回了坏 JSON。

这次记录聚焦的是其中一条明确的 GC 正确性问题：

- **finalizer 路径上的新对象会被提前 mark，但不保证被真正扫描其子图**

它会表现为：

- 对象本身存活；
- 但它引用到的部分子对象没有进入本轮 mark 闭包；
- 后续这些子对象可能被 sweep 掉，最终导致业务侧看到坏 JSON，严重时再进一步演化成更大的运行时问题。

## 关键源码链路

### 1. `MarkNewObject` 当前只在 finalizer 路径仍然生效

普通对象/数组分配路径中的 `MarkNewObject` 调用已经被注释掉：

- `src/ObjectModel/MObject.cpp:32-34`
- `src/ObjectModel/MObject.cpp:57-59`
- `src/ObjectModel/MArray.inline.h:141-143`

而 finalizer 对象仍会走：

- `src/Common/BaseObject.cpp:151-156`

```cpp
void BaseObject::OnFinalizerCreated()
{
    Heap& heap = Heap::GetHeap();
    heap.GetCollector().MarkNewObject(this);
    heap.GetFinalizerProcessor().RegisterFinalizer(this);
}
```

### 2. `RegisterFinalizer` 会经过 `WriteStaticRef`

- `src/Heap/Collector/FinalizerWorkList.cpp:124-129`

```cpp
void FinalizerWorkList::RegisterFinalizer(BaseObject* obj)
{
    RefField<> tmpField(nullptr);
    Heap::GetHeap().GetBarrier().WriteStaticRef(tmpField, obj);
    std::lock_guard<std::mutex> l(listLock);
    finalizers.push_back(reinterpret_cast<BaseObject*>(tmpField.GetFieldValue()));
}
```

### 3. `WriteStaticRef` 会尝试 discover 新对象

- `src/Heap/MBarrier/MSMarkBarrier.cpp:170-180`

```cpp
void MSMarkBarrier::WriteStaticRef(RefField<false>& field, BaseObject* ref) const
{
    BaseObject* oldRef = field.GetTargetObject();
    EnqueueRefFast(oldRef);
    field.SetTargetObject(ref);
    EnqueueRefFast(ref);
}
```

### 4. 但 MSGC 里“已经 mark”会直接阻止再次入队

- `src/Heap/MCollector/MarkSweepCollector.cpp:375-405`

```cpp
bool MarkSweepCollector::DiscoverObject(BaseObject* obj)
{
    if (UNLIKELY(!IsTraceableObjectAddress(obj) || !obj->IsValidObject())) {
        return false;
    }
    if (MarkObject(obj)) {
        return false;
    }
    return true;
}
```

因此，`OnFinalizerCreated()` 里如果先 `MarkNewObject(this)`，后续 `WriteStaticRef` 再对同一个对象做 discover 时，很可能因为“它已经 marked”而被直接跳过，不再进入后续扫描队列。

### 5. finalizer root 枚举也不是直接扫 `finalizers` 主链表

- `src/Heap/Collector/FinalizerWorkList.cpp:27-35`
- `src/Heap/Collector/TracingCollector.cpp:559-563`

`VisitGCRoots()` 只访问：

- `finalizables`
- `workingFinalizables`

并不直接访问 `finalizers` 主列表。

## 为什么这不是“新对象自己被 sweep 掉”

这里用户已纠正过一个很关键的点：**当前 GC 期间新分配的 TL region 不会在本轮被 sweep**。

代码证据：

- `src/Heap/MCollector/MarkSweepCollector.cpp:297-304`
- `src/Heap/MAllocator/MSCentralRegionPool.cpp:691-705`
- `src/Heap/MAllocator/MSAllocBuffer.cpp:582-585`

也就是说，这次问题更准确的描述不是：

- “finalizer 新对象自己在本轮被回收了”

而是：

- “finalizer 新对象自己活着，但它的部分子图没有被本轮扫描到”

这正是本次修复瞄准的点。

## 根因判断

综合上面的调用链，可以把根因收敛为：

1. finalizer 对象创建时仍会调用 `MarkNewObject`。
2. 旧实现里它会先被 `MarkObject(obj)`。
3. 之后 `RegisterFinalizer()` 通过 `WriteStaticRef` 试图把它作为“新 discover 的对象”纳入本轮扫描。
4. 但 `DiscoverObject()` 看到该对象已经 marked，会直接返回 false。
5. 结果是：
   - 对象本身被保活；
   - 但它不保证被真正扫描其引用字段；
   - 它引用到的某些子对象可能因此漏标。

这是一个典型的 **marked-but-not-scanned** 问题。

## 修复方案

本次修复不去改 finalizer 创建顺序，而是恢复 `MarkNewObject` 的“标记 + 延后扫描”语义。

### 1. `MarkNewObject()` 改为 mark 后压入 deferred queue

- `src/Heap/MCollector/MarkSweepCollector.cpp:1698-1712`

当前实现：

```cpp
void MarkSweepCollector::MarkNewObject(BaseObject* obj)
{
    ...
    if (UNLIKELY(obj == nullptr || !IsTraceableObjectAddress(obj) || !obj->IsValidObject())) {
        return;
    }
    (void)MarkObject(obj);
    PushSingleToDeferredMainThread(obj);
}
```

### 2. `ProcessDeferredWeakRefsPreSTW()` 不再提前扫描普通对象

- `src/Heap/MCollector/MarkSweepCollector.cpp:706-746`

新的策略是：

1. `WeakRef` 仍然在 pre-STW 阶段做特殊处理；
2. 非 `WeakRef` deferred item 不再在 concurrent 阶段直接扫字段；
3. 而是重新放回 `deferredMainThreadQueue_`；
4. 留到 `MarkTermination` 阶段由 GC 主线程在 STW 下统一扫描。

这样做的原因是：

- finalizer 对象在创建后可能仍处于“构造中”；
- 如果太早扫描，可能又只扫到一个未稳定的半初始化对象；
- 放到 STW 的 `MarkTermination` 再扫，能更接近“初始化写已静止”的安全点。

## 编译、替换与验证

### 1. 重新编译 MSGC runtime

```bash
python3 build.py clean
python3 build.py build --gc-mode marksweep
```

### 2. 替换到 `dev260609` 并重签名

```bash
cp output/temp/lib/aarch64_Release/libcangjie-runtime.dylib \
  /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260609/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
codesign -f -s - \
  /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260609/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
```

### 3. 回归命令

3 次回归都使用同一组参数：

```bash
MSGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260609/cangjie \
MSGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260609/darwin_aarch64_cjnative \
COPYGC_SDK_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie \
COPYGC_STDX_ROOT=/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/darwin_aarch64_cjnative \
CASE27_PAYLOAD=100K CASE27_CONCURRENCY=8 CASE27_DURATION_MS=60000 \
HEAP_SIZE=12gb PROCS=6 MSGC_HEAP_GROWTH=1 CJ_MS_BG_SCAVENGE=0 \
./run_compare.sh
```

## 3 次回归结果

| Run | 结果目录 | MSGC 结果 | CopyGC 结果 | 是否 crash | MSGC client-fail |
| --- | --- | --- | --- | --- | --- |
| run1 | `20260614_164106_finalizerfix_r1_c8_p100K_compare` | `total=69684 ok=69679 fail=5 throughput=1158req/s` | `total=51165 ok=51165 fail=0 throughput=850req/s` | 否 | 5 |
| run2 | `20260614_164356_finalizerfix_r2_c8_p100K_compare` | `total=70502 ok=70500 fail=2 throughput=1171req/s` | `total=51850 ok=51850 fail=0 throughput=861req/s` | 否 | 2 |
| run3 | `20260614_164639_finalizerfix_r3_c8_p100K_compare` | `total=69030 ok=69026 fail=4 throughput=1146req/s` | `total=52047 ok=52047 fail=0 throughput=864req/s` | 否 | 4 |

补充说明：

- 3 次 MSGC 运行都未再出现 `SIGSEGV` / `SIGABRT` / `OOM` 标记。
- 这说明本次修复**明显压住了此前的致命退出问题**。

## 为什么还不能算“完全修好”

虽然 3 次都没有再 crash，但问题并没有彻底收口：

1. MSGC 三轮仍然分别出现了 5 / 2 / 4 次 `client-fail`。
2. 这些 `client-fail` 不是退出收尾时的关连接噪音，而是中途真实出现的：

   - `The json data is Non-standard, please check:`

3. 例如：
   - run1：`reqId=2994, 5223, 54687, 69019, 69024`
   - run2：`reqId=9971, 66953`
   - run3：`reqId=10894, 52479, 52480, 60807`

这说明本次修复更准确的效果是：

- **把 fatal crash 收住了**

但还没有做到：

- **完全消除 case27 下的偶发坏 JSON**

## 残留问题

这 3 次运行里还有一个非常明显的残留信号：

- `EmptyCacheDiag reason=pop-without-in-cache-mark`

它在三轮中都大量出现，说明 empty cache / reclaim 相关的并发状态问题仍然存在。这个问题与本次 finalizer 修复不是同一个层次：

1. finalizer 修复解决的是一条对象图闭包不完整的问题；
2. `EmptyCacheDiag` 反映的是 empty-swept cache / take-local / background sweep 的另一条并发状态异常。

## 当前结论

这次修复可以定性为：

1. **已解决一部分问题**
   - finalizer 路径上的 `marked-but-not-scanned` 缺陷被补上了；
   - `case27 c8 p100K` 连续 3 次回归不再 crash。

2. **但不是最终修复**
   - 仍有少量 `client-fail`；
   - 仍有大量 `EmptyCacheDiag`；
   - case27 的“完全正确性”尚未恢复到 CopyGC 的 `fail=0` 水平。

## 建议的后续方向

建议后续顺着两条线并行排查：

1. 继续跟一条 `client-fail`，对齐请求 ID、响应内容和对应时刻的 GC 日志，确认残余坏 JSON 还来自哪一条对象图漏扫。
2. 单独顺着 `EmptyCacheDiag` 查 empty cache 并发状态，尤其是 `push-bg-sweep -> take-local` 这条链上 region 状态位与 in-cache 标记的同步关系。
