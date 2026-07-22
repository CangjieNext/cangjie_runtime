# 2026-06-07 case25 真正根因复盘：`CJMutex.state` 初始化缺失

## 1. 最终结论

这次 `case25_http_complex_json` 的真正问题，不是 Go client、不是 `stdx.net.http`
业务 handler、不是 GC pause 过长，也不是“正常锁竞争太重”，而是：

- `std.sync.Mutex` 的 runtime 初始化函数 `MCC_MutexInit()`
- 没有显式把 `CJMutex.state` 清零
- 导致部分新建出来的 request-local `writerMtx` 直接继承了旧对象残留的
  `LOCKED/STARVING` 脏位
- 于是请求在第一次进入 `synchronized(ctx.writerMtx)` 之前就已经“出生即坏”

最终修复是在 `src/Sync/Sync.cpp` 的 `MCC_MutexInit()` 中补上：

- `ownerThreadId = INVALID_THREAD_ID`
- `ownCount = 0`
- `state = 0`
- `isSemaInit = false`

修复后重新构建 MSGC runtime，并用同一套 60s case25 复跑，结果目录
`result/verify_go_sync_client.Dhkmdd` 显示：

- `total=18523 ok=18523 fail=0 throughput=308req/s`
- `verify_go_sync_client.sh` 退出 `[verify] PASS`

## 2. 用户最关心的四个问题

### 2.1 真正的问题是什么

真正的问题是 `CJMutex.state` 初始化缺失。

`HttpContext` 每个请求都会新建一个 `writerMtx`，但它底层对应的 `CJMutex`
在 `MCC_MutexInit()` 里没有清零 `state` 字段。只要对象内存来自复用过的 slot，
这个 mutex 就可能带着上一个死对象残留的 `LOCKED/STARVING` 位出生。

### 2.2 为什么 CopyGC 没有这个问题

已经验证到的事实是：

- 同一份当前 runtime 源码，编成 CopyGC 模式后跑 case25 是稳定的
- 官方 CopyGC SDK 跑同参数 case25 也是稳定的
- `rawdata_view_repro.cj` 在官方 CopyGC SDK 下也稳定通过

更深一层的解释是：

- 这个 bug 要暴露，前提不是“代码里有漏初始化”这一条就够了
- 还需要 GC/allocator 真把“带历史残留字节的对象内存”再次交给 `Mutex()`
- MSGC 的对象复用/region 复用模型更容易把这种脏值直接暴露出来
- CopyGC 的拷贝/半空间 fresh 分配模型更容易把这个问题掩盖掉

这里“为什么 CopyGC 没暴露”后半句是**基于源码模型的解释**，不是单独新做的一轮实验。

### 2.3 是怎么一步步验证过来的

主线验证顺序是：

1. 先确认现象真实存在于 case25 主场景，而不是偶然波动
2. 再用 Go sync client 证明“不是 Cangjie client 自己写坏了”
3. 再看 server 侧 marker，证明“不是 handler 没返回”
4. 再把日志收敛到 `writeResponse(ctx)` 的 `writerMtx` 获取阶段
5. 再把 probe 前移到 `HttpContext` 刚创建后，证明“锁一出生就坏”
6. 最后回到 runtime 源码，定位到 `MCC_MutexInit()` 漏清 `state`

### 2.4 最后是怎么解决的

解决方式非常小，但正好打中根因：

- 只改 runtime
- 只改 `src/Sync/Sync.cpp::MCC_MutexInit()`
- 不改 `stdx` 业务逻辑
- 不改 case25 benchmark

补完 `CJMutex` 的显式初始化后，原先的 `context-init-trylock-miss`
和 `server-write-trylock-miss` 都消失，case25 60s 长跑恢复为 `fail=0`。

## 3. 最初观察到的现象

最早的表象不是 crash，而是典型的“跑着跑着就开始卡住，然后读响应头超时”。

在真实 MSGC runtime + 诊断版 stdx + Go sync client 的早期样本里，例如：

- `result/verify_go_sync_client.1ODfbS`

同一组参数：

- `payload=100K`
- `concurrency=8`
- `duration=60000ms`
- `timeout=5000ms`

出现的结果是：

- 前 20s 基本正常推进
- 从 `t≈20s` 开始出现 `Client.Timeout exceeded while awaiting headers`
- 最终：
  - `total=11657 ok=11617 fail=40 throughput=194req/s`

更激进的诊断样本：

- `result/verify_go_sync_client.KAmGa1`

则表现为：

- 前 `20s` 左右正常
- `22s~24s` 出现整段 `+last=0`
- 第一批 timeout reqId 呈批量出现
- 最终：
  - `total=11400 ok=11357 fail=43 throughput=186req/s`

与之形成鲜明对照的是 CopyGC 控制样本：

- `result/20260606_142213_copygc_runtimecheck_sample`

关键结果为：

- `total=15078 ok=15078 fail=0 throughput=242req/s`
- 全程没有出现：
  - `+last2s=0`
  - `awaiting headers`
  - 成批 timeout fail

所以从一开始，问题就不是“case25 这个 benchmark 自己写坏了”，而是：

- **同一条业务压力路径**
- **CopyGC 稳定**
- **MSGC 会在中途进入 timeout / stall**

这也是为什么后面所有排查都必须带着“MSGC 和 CopyGC 的分叉点到底在哪”这个问题来做。

## 4. 为什么中途会被带偏

这次问题之所以不容易一下打到 `MCC_MutexInit()`，主要因为中途有三层非常像根因的假象。

### 4.1 第一层假象：`rawData()` / `String -> Array<Byte>` 生命周期问题

我们当时单独抽出了最小复现：

- `MSGC_RESULT/Result/case25_http_complex_json/bugfix/rawdata_view_repro.cj`

旧样本里，在当时未重新替换 runtime 的 `self/dev260605` / `self/dev260509`
现役状态下，它确实出现过同型崩溃，所以当时强烈怀疑：

- `String -> rawData() -> Array<Byte>` 生命周期
- 或 `stdx.net.http` / `stdx.encoding.json` 中的 raw view 相关路径

但后面严格按隔离环境、真实 MSGC runtime 重新验证后：

- `result/ms_dev260605_rawdata.ZHdKje`

显示：

- `F1_FullArrayView PASS`
- `F2_StructFullView PASS`
- `F3_StructSliceView PASS`
- `F4_ClassFieldView PASS`
- `SUMMARY PASS`

这一步非常关键，它说明：

- “最小 `rawData()` 复现会在真实 MSGC 下稳定崩”这个结论不成立
- `rawData` 线索顶多是历史干扰项，不能再当 case25 主问题的根因

### 4.2 第二层假象：是不是 Cangjie client 自己不稳定

为了排掉“client 实现问题”，我们切出了外部对照 client：

- `go_client_sync.go`
- `verify_go_sync_client.sh`

结果是：

- 换成 Go sync client 后，问题仍然在
- 而且错误类型非常统一：
  - `Client.Timeout exceeded while awaiting headers`

这说明：

- 问题不在原 benchmark 里 Cangjie client 的并发调度实现
- 更像是 server 端或者 runtime/stdlib 交界层的问题

### 4.3 第三层假象：是不是 GC pause 太长

这次 case25 的表象很像“某次 GC 把世界停太久，导致 client 5s timeout”。

但后续对齐 `gc.log` 后，最大 pause 只有十几毫秒量级，并没有能解释
`awaiting headers` 的 5s 级别超时。

因此：

- GC 当然参与了运行时环境
- 但它不是“直接把请求卡 5 秒”的主因

### 4.4 第四层假象：是不是 user handler 没跑完

server 侧早期 marker 已经证明，很多失败请求其实已经走到了：

- `server-done`
- `server-write-enter`
- `server-write-before-lock`

也就是说：

- 业务 handler 已经执行完成
- 卡点发生在“准备发响应”之后更靠里的位置

这一步把排查重点从：

- JSON 解析
- 业务改写
- body 构造

收敛到了：

- `writeResponse(ctx)` 以及更具体的 `writerMtx`

## 5. 一步步是怎么验证过来的

下面按真正起作用的收敛步骤来整理。

### 第 1 步：先确认“MSGC 会错，CopyGC 不会”

控制样本：

- CopyGC：`result/20260606_142213_copygc_runtimecheck_sample`
- MSGC：多轮 `verify_go_sync_client.*`

结论：

- 同样是 case25、同样是 100K、同样是 8 并发
- CopyGC 可以稳定 `fail=0`
- MSGC 会在中途出现 timeout / stall

这一步只回答一件事：

- **问题确实与 MSGC 栈相关，而不是 case25 本体必然有问题**

### 第 2 步：切出 Go sync client，排除 Cangjie client 自身实现

关键样本：

- `result/verify_go_sync_client.1ODfbS`

结论：

- 换成 Go client 后，问题依旧
- 错误形态仍然是 `awaiting headers`

因此排除：

- “原 benchmark 里 Cangjie client 的 async/sync 调度实现不稳”

### 第 3 步：server 侧加 marker，确认不是 handler 没返回

关键 marker 包括：

- `server-done`
- `server-write-enter`
- `server-write-before-build`
- `server-write-after-build`
- `server-write-before-lock`

结论：

- 失败请求很多已经明确走过了 user handler
- 甚至已经走到 `writeResponse(ctx)` 早期阶段

因此排除：

- JSON 业务逻辑卡死
- handler 本身没返回

### 第 4 步：在 `writerMtx` 前插 `tryLock`，确认真卡在锁上

关键样本：

- `result/verify_go_sync_client.56Q82A`

现象：

- 一批失败请求在 `server-write-before-lock` 后
- 直接出现 `server-write-trylock-miss`
- 而且这批请求后面不再有 `after-lock/start/done`

结论：

- 这不是“慢一点”
- 而是这些请求在第一次尝试进入 `synchronized(ctx.writerMtx)` 时就已经拿不到锁

### 第 5 步：把 probe 前移到 `HttpContext` 刚创建后

这是最关键的一步。

关键样本：

- `result/verify_go_sync_client.KAmGa1`

新增 probe：

- `server-context-init`
- `server-context-post-handler`

结果：

- `server-context-init-trylock-miss=24`
- `server-context-post-handler-trylock-miss=24`
- `server-write-trylock-miss=24`

而且这三类 miss 的 reqId 高度重合。

这一步把问题彻底从“谁先加锁未释放”切成了：

- **这把 request-local mutex 不是后来才变坏**
- **而是 `HttpContext` 一创建出来就已经有一部分是坏的**

### 第 6 步：回到 runtime 源码，看 `Mutex()` 到底怎么初始化

这一步的调用链是：

- `HttpContext.writerMtx = Mutex()`
- `std.sync.Mutex.init()`
- intrinsic `mutexInit(this)`
- runtime `MCC_MutexInit()`

源码对上之后，缺口就非常明显了：

- `MCC_MutexInit()` 初始化了 `ownerThreadId`、`ownCount`、`isSemaInit`
- **但没有初始化 `state`**

到这里，根因已经基本坐实。

## 6. 真正的问题到底是什么

根因链条其实很短：

1. `HttpContext` 上有一个 request-local 字段：
   - `let writerMtx = Mutex()`
2. `Mutex()` 会走到 `std.sync.Mutex.init()`
3. `Mutex.init()` 只调用 intrinsic：
   - `mutexInit(this)`
4. intrinsic 对应到 runtime：
   - `src/Sync/Sync.cpp::MCC_MutexInit()`

而修复前的 `MCC_MutexInit()` 有一个致命缺口：

- 它会初始化 `ownerThreadId`
- 会初始化 `ownCount`
- 会初始化 `isSemaInit`
- **但不会初始化 `CJMutex.state`**

与此同时，`MCC_MutexTryLock()` 的逻辑是：

- 如果 `ownerThreadId != INVALID_THREAD_ID`，失败
- 如果 `state` 里带 `LOCKED`，失败
- 如果 `state` 里带 `STARVING`，失败

所以一旦 `state` 带着历史对象的残留位，哪怕这是一个“刚 new 出来的 mutex”，它也会表现成：

- 还没被任何合法请求真正使用
- 但第一次 `tryLock()` 就失败
- 第一次 `synchronized(...)` 就卡死

这和 `KAmGa1` 的观测完全一致：

- `server-context-init-trylock-miss`
- `server-context-post-handler-trylock-miss`
- `server-write-trylock-miss`

三者是同一批 reqId。

也就是说，这次 case25 的本质不是“锁竞争太大”，而是：

- **锁对象本身在出生时就已经是不干净的**

这其实和我们之前排查过的“新对象继承旧 `stateWord` / 旧状态位”的问题族是同一类：

- 真正危险的不是某个 API 名字
- 而是“对象内存复用 + 初始化不完整”

这次只不过受害者不是普通对象头 `stateWord`，而是 `CJMutex.state` 这个字段。

## 7. 为什么 CopyGC 没有这个问题

这里必须分成两层说。

### 7.1 已经验证到的事实

事实 1：

- `result/20260606_142213_copygc_runtimecheck_sample`

是在当前源码重新构建的 CopyGC runtime 上做的控制样本，结果为：

- `total=15078 ok=15078 fail=0 throughput=242req/s`

事实 2：

- 官方 `dev/20260507` CopyGC SDK
- 在 case25 和 `rawdata_view_repro.cj` 上都能稳定通过

事实 3：

- 同样的 case25 业务压力路径
- 一旦换到真实 MSGC runtime
- 就会在未修复时稳定复现 `awaiting headers` / `trylock-miss`

所以仅从实验层面能确认的是：

- **同样的上层业务路径，CopyGC 稳定，MSGC 未修复时不稳定**

### 7.2 基于源码模型的解释

更深一层的解释，是这次 bug 的触发不只依赖“漏初始化”这一件事，还依赖：

- allocator/GC 是否会把带历史残留字节的对象内存再次交给新对象

对这次 bug 来说，最合理的解释是：

1. `CJMutex.state` 漏清零本身是通用 bug
2. 但它只有在“新建 mutex 拿到的是脏内存”时才会暴露
3. MSGC 的对象复用/region 复用模型更容易把这种脏字节直接暴露出来
4. CopyGC 的拷贝/半空间 fresh 分配模型更容易把这个问题掩盖掉

换句话说：

- **真正的 bug 在 runtime 初始化**
- **真正把它放大成用户可见问题的是 MSGC 的对象复用条件**

这一点和我们以前分析过的“新对象继承旧 `stateWord` / 脏 objectState”
是同一种风险模型。

需要强调：

- 上面这一段是**源码模型解释**
- 不是我们专门新增的一轮“只比较 allocator 脏字节复用”的实验结论

但它与现有现象是完全一致的，也能解释为什么 CopyGC 一直没有以同样形式暴露。

## 8. 最后是怎么解决的

最终修复非常集中，只改了一个点：

- 文件：`src/Sync/Sync.cpp`
- 函数：`MCC_MutexInit(void* ptr)`

修复后的初始化逻辑是：

```cpp
CJMutex* mutex = reinterpret_cast<CJMutex*>(ptr);
mutex->ownerThreadId.store(INVALID_THREAD_ID, std::memory_order_relaxed);
mutex->ownCount = 0;
mutex->state.store(0, std::memory_order_relaxed);
mutex->isSemaInit = false;
int ret = MRT_NewSem(&mutex->sema);
if (ret == 0) {
    mutex->isSemaInit = true;
}
```

这个修复之所以能直接打中根因，是因为它把“新建 mutex 继承旧状态位”的可能性在出生点上直接掐掉了：

- 不再依赖“对象内存刚好是干净的”
- 不再依赖“GC/allocator 暂时帮我们掩盖脏值”
- 每次 `Mutex()` 都保证 `state` 从 `0` 开始

换句话说，这不是绕过问题，而是把初始化缺口直接补上。

## 9. 修复后的验证结果

修复后做的是完整 fresh 验证，不是只跑了一个局部脚本。

### 9.1 fresh build

执行：

- `python3 build.py build --gc-mode marksweep`

结果：

- build 退出码 `0`

### 9.2 fresh case25 回归

验证组合：

- compiler：`self/dev260605/cangjie`
- runtime：工作区 `output/temp/lib/aarch64_Release`
- stdx：工作区诊断版 `target/darwin_aarch64_cjnative/dynamic/stdx`

复现目录：

- `result/verify_go_sync_client.Dhkmdd`

结果：

- `total=18523 ok=18523 fail=0 throughput=308req/s`
- `verify_go_sync_client.sh` 退出 `[verify] PASS`

### 9.3 marker 级验证

修复前 `KAmGa1` 中统计到：

- `server-context-init-trylock-miss=24`
- `server-context-post-handler-trylock-miss=24`
- `server-write-trylock-miss=24`

修复后 `Dhkmdd` 中手工统计为：

- `server-context-init-trylock-miss=0`
- `server-context-post-handler-trylock-miss=0`
- `server-write-trylock-miss=0`
- 三类 `trylock-hit` 都为 `18523`

这说明修复后消失的不是“表面上的 timeout”，而是：

- **最早的底层坏状态 marker 本身**

这也是本轮验证里最有力的一条证据。

## 10. 本次复盘的结论

这次 case25 复盘有四个值得记住的点：

1. **不要把“MSGC 下更容易暴露”误写成“问题一定在 GC 主逻辑里”**
   - 这次真正的 bug 在同步原语初始化，不在 collector 主循环。

2. **先把“事实”和“解释”分开**
   - 事实是 CopyGC 稳、MSGC 未修复时不稳；
   - 解释才是“MSGC 对脏对象复用更敏感”。

3. **一旦怀疑锁问题，probe 要尽快前移到对象创建点**
   - 这次真正破局的是 `server-context-init`，不是更后面的 `server-write-before-lock`。

4. **对象初始化遗漏在 MSGC 下特别危险**
   - 只要对象可能复用旧内存，任何“默认应该是 0”的字段都不能依赖运气。

最终可以把这次问题浓缩成一句话：

- **case25 不是被 HTTP 逻辑拖死的，也不是被 GC pause 拖死的，而是被一个 request-local mutex 的脏 `state` 拖死的；这个脏 `state` 来源于 runtime 初始化缺口，MSGC 的对象复用模型把它稳定放大成了用户可见故障。**
