# Step27：Schdfd / netpoll 与大量阻塞 socket 下的调度行为优化点分析

> 2026-06-21
> 目标：基于 runtime 当前 `Schdfd -> Schdpoll -> netpoll -> scheduler` 实现，梳理大量阻塞 socket 场景下的真实调度行为，区分“看起来像问题”和“真正可能限制吞吐的结构点”，并给出后续优化优先级。

---

## 目录

1. [背景与结论](#1-背景与结论)
2. [当前代码路径概览](#2-当前代码路径概览)
3. [当前行为的关键观察](#3-当前行为的关键观察)
4. [值得优先推进的优化点](#4-值得优先推进的优化点)
5. [暂不建议直接动的点](#5-暂不建议直接动的点)
6. [建议的验证与实施顺序](#6-建议的验证与实施顺序)

---

## 1. 背景与结论

本次分析的目标不是改 GC，而是回答一个更靠近 runtime 调度层的问题：

> 在 `Schdfd / Schdpoll / netpoll` 这条 socket 异步等待路径上，当连接数很多、绝大多数连接长期阻塞、并且会出现成批 ready 事件时，当前实现会不会在调度层形成新的吞吐瓶颈？

对照 `src/CJThread` 里的实际代码后，可以先下 4 个结论：

1. 当前模型在语义上是成立的。
   `Schdfd` 的 wait 并不是把 OS 线程阻塞住，而是把当前 cjthread park 掉，因此“大量阻塞 socket”不会直接把底层 worker 线程全部睡死。

2. 真正可能限制吞吐的，不是单次 `recv/send` 的系统调用本身，而是 ready 事件回来之后的“批量唤醒与并发爬坡”路径。

3. 目前最像结构性上限的点有两个：
   - `SchdpollAcquire()` 单次只允许一个线程 drain netpoll ready 队列
   - `CJThreadAddBatch() -> ProcessorWake()` 的并发拉升策略偏保守，ready 一大批时更像串行爬坡

4. timeout 路径也值得重点看。
   当前每次带超时的 `SchdfdWait*Timeout()` 都会 `TimerNew()` 一次，在“连接多、超时多、阻塞多”的场景下，这部分元数据与 timer heap 开销可能被放大。

一句话总结：

> 当前 `Schdfd/netpoll` 不是“实现错误”，但在大量阻塞 socket 下，**更像是 ready 批量回流之后的调度扩容和 timeout 管理偏重**，而不是 poll syscall 本身慢。

## 2. 当前代码路径概览

### 2.1 fd 注册与 netpoll 绑定

`tcp socket` 的典型路径如下：

1. `TcpsockCreate()` 创建非阻塞 socket，并调用 `SchdfdRegister()`
2. `listen/connect/accept` 等进入可产生事件的阶段后，再调用 `SchdfdNetpollAdd()`
3. `SchdfdNetpollAdd()` 为 fd 分配 `SchdpollDesc`，并把 fd 加入 netpoll

对应实现位于：

- `src/CJThread/src/aio/tcpsock/src/tcpsock_common.cpp`
- `src/CJThread/src/aio/schdfd/src/schdfd.cpp`
- `src/CJThread/src/runtime/schedule/src/schdpoll.cpp`

在 macOS 上，底层是 `kqueue + EV_CLEAR`；在 Linux 上是 `epoll + EPOLLET`。两边都属于边沿/清空式通知，而不是每次都电平重扫。

### 2.2 recv / send / accept 的阻塞路径

`SockRecvGeneral()`、`SockSendGeneral()`、`TcpsockAccept()` 的结构基本一致：

1. 先通过 `SchdfdLock(fd, type)` 获取“同一 fd、同一方向”的顺序锁
2. 执行一次非阻塞 `recv/send/accept`
3. 若返回 `EAGAIN`，则进入：
   - `SchdfdWaitInlock()`，或
   - `SchdfdWaitInlockTimeout()`
4. 该 wait 最终走到 `SchdpollWait()`，把当前 cjthread park 掉，等待 ready 或 close
5. 被唤醒后重新执行系统调用

这里有一个重要判断：

> `SchdfdLock()` 用的是 `SemaAcquire()`，而 `SemaAcquire()` 在失败时走 `WaitqueuePark()`，所以它是协程级排队，不是内核线程级阻塞。

这意味着“同一 fd 的多 reader / 多 writer 竞争”虽然会排队，但不会把整个 worker 线程卡死。

### 2.3 ready 事件是怎么回来的

ready 事件回流有两条入口：

1. `ProcessorCJThreadGet()` 在本地/全局 runq 都没任务时，会 opportunistic 地调用一次 `SchdpollAcquire(..., timeout=0)`
2. `SchmonCycle()` 对 default scheduler 会执行一次带超时的 `SchdpollAcquire(..., timeout=10ms)`

注意这里有个容易误判的点：

> `10ms` 不是“固定 10ms 才扫描一次”，而是最长阻塞等待时间。只要 `kqueue/epoll` 上有 ready 事件，`NetpollWait()` 会立刻返回。

所以 `schmon` 默认 10ms 不等价于人为增加 10ms 网络时延。

### 2.4 ready 之后如何进入调度器

`SchdpollAcquire()` 拿到 ready 事件后，会：

1. 对每个 ready 的 `pd` 调用 `SchdpollReady()`
2. 取回需要唤醒的 cjthread
3. 先放进本地 `buf`
4. 再通过 `CJThreadAddBatch()` 把这一批 ready cjthread 重新灌回 scheduler

`CJThreadAddBatch()` 又会分两步：

1. 根据空闲 processor 数量，把一部分 cjthread 放入全局队列
2. 再循环调用 `ProcessorWake()` 试图拉起 processor

这条链路很关键，因为在“大量阻塞 socket 同时 ready”的场景里，吞吐是否能快速爬起来，主要取决于这一段，而不是取决于前面的 `recv/send` 本身。

## 3. 当前行为的关键观察

### 3.1 单个 fd 的读写是方向内串行的

`SchdfdFd` 内常驻两个 semaphore：

- `readSema`
- `writeSema`

这意味着：

1. 同一 fd 的多个 reader 会串行
2. 同一 fd 的多个 writer 会串行
3. 读和写之间可以并行，但同方向不会并行

这个设计本身没错，它的作用是防止同一 fd 同方向的等待状态和系统调用交叉踩踏；但如果上层让很多协程复用同一连接，它就会变成明显热点。

因此：

> 如果 benchmark 或业务层存在“多个协程共享一个连接并同时收发”的模式，runtime 这里天然不会扩展得很好。

### 3.2 `SchdpollAcquire()` 本质上是单 drain 模式

不管是 Linux 还是 macOS，`SchdpollAcquire()` 开头都会：

1. `pthread_mutex_trylock(&schedule->netpoll.pollMutex)`
2. 失败就直接返回 0
3. 成功的那个线程独自完成本轮 `NetpollWait() + ready 扫描 + wake list 构造`

这说明当前模型不是“多 poller 并行 drain”，而是：

> **同一时刻只允许一个线程处理 netpoll ready 批次。**

这在 ready 很零散时通常没问题，但在以下场景中可能形成上限：

1. 大量 socket 同时变 ready
2. 单次 ready 批次很大
3. poller 自己还要顺手做 `closingPd` 的 free 清理

### 3.3 ready 批次大小偏小

当前：

- `SCHDPOLL_EVENT_NUM = 64`
- `SCHDPOLL_ACQUIRE_MAX_NUM = 128`

也就是说单次 `SchdpollAcquire()` 最多处理 64 个底层事件，最多回填 128 个 `read/write` 方向唤醒结果。

如果出现几百、几千个连接同时 ready，那么当前实现会更像：

> 一次拉回一小批，再靠后续 acquire 慢慢清空 backlog。

这会放大单 poller 模式的影响。

### 3.4 `ProcessorWake()` 的扩容策略偏保守

`CJThreadAddBatch()` 虽然会在 ready 批次回来后循环调用 `ProcessorWake()`，但 `ProcessorWake()` 本身又受 `searchingNum` 保护：

1. 如果已经有 searching thread 存在，就直接返回
2. 只有成功把 `searchingNum` 从 0 CAS 到 1，才会真正启动/绑定新的 processor

这会导致一个现象：

> 即使一口气回来了很多 ready cjthread，processor 的并发拉升也更像“逐步爬坡”，而不是“按 ready 数量迅速放大”。

这正是当前最值得怀疑的吞吐上限之一。

### 3.5 timeout 路径是“每次 wait 新建一个 timer”

`SchdfdWaitTimeout()` 与 `SchdfdWaitInlockTimeout()` 的结构都是：

1. `TimerNew(timeout, ...)`
2. `SchdpollWait()`
3. ready 后 `TimerTryStop()` / `FreeSchdfdTimer()`
4. timeout 或 close 再走对应收尾

这意味着每一次带 deadline 的 I/O 阻塞，都要付出一次 timer 分配、挂堆、取消/释放的成本。

如果 workload 满足以下任一条件，这部分开销会比较显著：

1. 连接数大
2. 大多数连接都会带 timeout
3. 每个连接上阻塞等待的次数很多

### 3.6 `schmon 10ms` 不是主要问题

这里需要明确排除一个表面嫌疑点：

`schmon` 的 `NETPOLL_WAIT_TIME = 10` 看上去像“poll 太慢”，但实际不是固定轮询；ready 事件一到，`kqueue/epoll` 会立即返回。

因此这 10ms 更像是：

1. 在无 ready 事件时避免空转
2. 给默认调度器提供一个阻塞等待入口

它不是当前最该优先优化的对象。

## 4. 值得优先推进的优化点

下面按优先级列后续建议。

### 4.1 P0：优化 ready 批次回流后的 processor 拉升策略

这是当前最值得优先尝试的点。

原因：

1. 大量阻塞 socket 的主要问题，不是“怎么 park”，而是“ready 一批回来后能不能快速并行跑起来”
2. 当前 `CJThreadAddBatch() -> ProcessorWake()` 的组合明显偏保守
3. 这类优化不需要改 socket 语义，也不需要碰 fd 生命周期，风险相对可控

可考虑的方向：

1. 在 batch ready 回流时，不再完全依赖 `searchingNum == 0` 才扩容
2. 允许按 ready 批次大小或空闲 processor 数量，一次性唤醒多个 processor
3. 单独为 netpoll ready 批次设计比普通 `CJThreadReady()` 更积极的 wake 策略

预期收益：

> 提高“成批 ready 时”的并发爬升速度，减少 ready backlog 堆在 global/local queue 中等待被慢慢消费。

### 4.2 P1：把 timeout 从“每次新建 timer”改成可复用 deadline 模型

这是第二个很值得做的点。

当前每次 timeout wait 都会：

1. 分配 timer node
2. 挂入 timer heap
3. ready 后 stop / release

对大量阻塞连接来说，这是一层持续性的管理开销。

可考虑的方向：

1. 每个 `fd + direction` 复用一个 timeout state
2. 改成“更新 deadline”，而不是“每次 wait 都新建 timer”
3. 让 timer callback 更偏向状态推进，而不是参与一次性对象生命周期

预期收益：

> 降低带超时 socket 大量阻塞时的 timer 分配、堆维护和取消成本。

### 4.3 P1：把 write ready 改成按需 arm，而不是默认常驻监听

当前 `SchdpollAdd()` 对 socket 默认同时监听 `READ/WRITE`。

这带来的问题是：

1. 明明大多数连接平时只是在等读
2. 但写方向依然长期处于被关注状态
3. 在某些平台/负载下会增加额外的 ready 处理和状态切换

更稳妥的方向是：

1. `READ` 保持常驻
2. `WRITE` 只在 `send/connect` 真正遇到 `EAGAIN/EINPROGRESS` 时临时 arm
3. 写完成后再 disarm

这个点的收益可能不如前两项稳定，但在连接数很大时值得做成可开关实验。

### 4.4 P2：把 `closingPd` 清理从 poll critical section 中挪开

当前 Linux/macOS 都会把待释放的 `pd` 先挂进 `closingPd`，并在后续某次 `SchdpollAcquire()` 内统一 free。

这意味着：

1. poller 在处理 ready 批次时
2. 还要顺手做一段额外的释放工作

在连接 churn 很大时，这会把 free 成本叠到单 poller 临界区里。

优化方向可以是：

1. 单独的延迟回收链
2. 或在离开 `pollMutex` 后再清理

这个点属于“减小单 poller 临界区长度”的辅助优化。

### 4.5 P2：先加计数与观测，再决定是否需要更激进的 netpoll 改造

在真正改语义前，建议先加 4 类计数：

1. `SchdpollAcquire()`：
   - trylock 失败次数
   - 每次 acquire 的 ready 数量分布
2. `CJThreadAddBatch()`：
   - 每批 ready 数
   - 回填 global/local queue 的分布
3. `ProcessorWake()`：
   - 被 `searchingNum` 拦下的次数
   - 真正成功拉起 processor 的次数
4. `SchdfdWait*Timeout()`：
   - `TimerNew()` 次数
   - timeout 成功 stop 次数
   - 真 timeout 次数

如果这些计数证实瓶颈就在“单 drain + 串行爬坡 + 高 timer churn”，再做更大改动会更稳。

## 5. 暂不建议直接动的点

### 5.1 暂不建议直接删除 `readSema/writeSema`

虽然它会造成同方向串行，但它承担的是同一 fd 上状态一致性的责任。

如果直接去掉，会立刻引入更复杂的问题：

1. 同一 fd 多个协程同时 `recv/send`
2. waiter state 与实际系统调用错位
3. timeout / close / ready 的竞态变得更难收敛

所以更现实的方向是：

> 先减少上层“多协程共享同一连接同向并发”的使用模式，而不是先拔掉 runtime 这层保护。

### 5.2 暂不建议直接改成多 poller 并行 drain 同一 netpoll

表面上看，`pollMutex` 很像一个天然优化点，但这里牵涉：

1. `pd` 生命周期
2. `closingPd` 回收
3. 同一 ready 事件是否会被多方并发处理
4. `SchdpollReady()` 的状态机是否还成立

所以“去掉 `pollMutex`、改多线程同时 drain 同一 kqueue/epoll”不适合做第一步。

如果前面的批量唤醒和 timer 成本优化后仍不够，再考虑更激进的 netpoll 并发模型。

### 5.3 暂不建议把问题简单归因到 `schmon 10ms`

这会把注意力带偏。

当前更该看的不是：

1. `schmon` 是否固定 10ms 扫一次

而是：

1. ready 批次回来后能不能快速扩容
2. timeout 路径是否太重
3. 单 poller 临界区是否太长

因此 Step27 不把 `NETPOLL_WAIT_TIME` 当作主优化方向。

## 6. 建议的验证与实施顺序

建议后续按下面顺序推进：

### 第一阶段：先加观测，不改语义

先补 runtime 计数，确认 3 件事：

1. `SchdpollAcquire()` 的实际批次规模有多大
2. `ProcessorWake()` 被 `searchingNum` 抑制了多少次
3. 带 timeout 的 I/O 中，`TimerNew()` 的量级有多高

这一步完成后，基本就能判断主瓶颈更偏：

- ready 回流扩容
- timeout 管理
- 或上层连接复用模式

### 第二阶段：先做 batch wake 优化原型

优先做一个“ready 一批回来时更积极拉起 processor”的原型开关。

原因：

1. 风险相对可控
2. 收益最可能直观反映在高并发 socket ready 场景
3. 不会破坏 fd 生命周期和 poll 语义

### 第三阶段：再做 timeout 复用化

若计数显示 timeout wait 很多，再推进：

1. `fd + direction` 级 deadline 复用
2. 减少 `TimerNew/TimerRelease/TimerTryStop` 频度

### 第四阶段：按需评估 write-arm 与回收路径瘦身

这一步可以作为进一步吞吐优化项：

1. 按需监听 write ready
2. 缩短 `pollMutex` 临界区
3. 减少 `closingPd` 清理对 acquire 的干扰

---

## 最终结论

Step27 的核心结论可以收束成一句话：

> `Schdfd/netpoll` 当前在语义上没有明显错误；大量阻塞 socket 下更值得优先优化的是 **ready 批量回流后的 processor 扩容策略** 与 **timeout 的每次建 timer 成本**，而不是先去怀疑 `schmon 10ms` 或简单删除 fd 方向锁。
