# 当前源码 (MSGC-bugfix, HEAD 6d54d0e9) 构建 MSGC 运行 case25 性能回归分析

## 2026-06-06 补充定位结论

基于重新编译当前源码 runtime 并复跑 `case25_http_complex_json` 的最新证据，前面把回归主要归因到
`mutator assist` / `heapThreshold` 的判断并不准确。当前更符合现象的**首要根因**是：

> **macOS netpoll 注册代码去掉了 `EV_CLEAR`，导致 case25 这种长连接 HTTP 压测在运行约 30s 后进入 I/O 停滞状态。**

对应代码差异：

```diff
src/CJThread/src/runtime/netpoll/src/macos/netpoll.cpp

- EV_SET(&event, fd, events, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, data);
+ EV_SET(&event, fd, events, EV_ADD | EV_ENABLE, 0, 0, data);
```

### 为什么这比 GC 参数更像根因

1. **回归不是“全程都慢”，而是“先正常，后停住”**

最新回归 run（`20260606_103444_d605_current_src_runtime_reverify_c8_p100K_compare/msgc/stdout.txt`）里：

- 前 30s 吞吐基本正常，`10s` 时约 `244 req/s`
- 到 `~32s` 左右后突然停止前进：
  - `t=32093ms total=7589 ok=7589 fail=0`
  - `t=34093ms total=7589 ok=7589 fail=0`
  - 后续只零星增加 `fail`

这不像 GC 阈值把**全程** mutator 压慢，更像**连接/事件通知进入坏状态**。

2. **失败请求增长节奏与 `readTimeout=5s` 完全吻合**

`benchmark.cj` 的 client 配置是：

```cj
ClientBuilder()
    .poolSize(8)
    .readTimeout(Duration.second * 5)
```

掉速后，`fail` 不是瞬间暴涨，而是按 5s 左右的节奏从 `0 -> 5 -> 8 -> 9` 增长，
这和“worker 卡在读响应，最后靠 `readTimeout(5s)` 失败返回”高度一致。

3. **GC/RSS 下降更像“工作没跑起来的结果”，不是“优化成功”**

回归 run 中：

- 吞吐从上一次有效 run 的 `~241 req/s` 掉到 `122 req/s`
- `NewArray`、`TakeRegion`、RSS、GC 总时间都明显下降

这更像**后半程几乎不再处理请求**，因此分配和 GC 自然一起下降；不能解读为 GC 本身更优。

4. **`gc.log` 在停滞点之后几乎不再推进**

回归 run 的 `msgc/gc.log` 最后活跃时间在 `10:36:23~10:36:25` 左右，对应 benchmark 运行到约 `31~33s`。
而 benchmark 总运行到 `~60s`。这说明：

- 后半程不是“GC 一直很忙”
- 更像是**业务请求不再推进，后面也就没有新的分配/GC 活动**

### 为什么 `EV_CLEAR` 改动很危险

在 macOS 上，`SchdpollAdd()` 会为同一个 fd 同时注册：

- `EVFILT_READ`
- `EVFILT_WRITE`

case25 又使用：

- HTTP keep-alive / 连接池（`poolSize(8)`）
- 长时间复用同一批连接

去掉 `EV_CLEAR` 后，kqueue 事件语义会和原先不同。对这种长连接场景，最容易出现的问题就是：

- 可写事件长期处于“就绪”
- 旧 readiness 没有按原先方式清掉
- `SchdpollAcquire()` / `SchdpollReady()` 处理到大量 stale 事件
- 真正的读唤醒或状态迁移被打乱
- 最终 worker 卡在 read path，靠 5s timeout 才退出来

这和当前 case25 的“前半程正常，后半程几乎只剩 timeout fail”现象高度一致。

### 当前判断

| 项目 | 结论 |
| --- | --- |
| 首要嫌疑 | `src/CJThread/src/runtime/netpoll/src/macos/netpoll.cpp` 去掉 `EV_CLEAR` |
| 现象匹配度 | **高** |
| 与 32s 后停滞的吻合度 | **高** |
| 与 5s 超时节奏的吻合度 | **高** |
| `mutator assist` / `heapThreshold` | 更可能是次要放大项，**不是这次 case25 停住的主因** |

### 下一步最小验证

1. 恢复 `EV_CLEAR`
2. 重新编译当前 runtime
3. 替换到 `dev260605` SDK
4. 重跑 case25

如果恢复后吞吐重新维持到全程 `~230-240 req/s`，并且不再出现 `32s` 左右的平台期，
就可以把这次回归主因基本坐实为 `netpoll.cpp` 的该处改动。

## 测试环境

| 项目 | 值 |
| --- | --- |
| 用例 | case25_http_complex_json |
| Payload | 100K |
| 并发数 | 8 |
| 运行时长 | 60s |
| 堆大小 | 12gb |
| CPU | 6 processors |

## 对比结果总览

| 对比维度 | CopyGC (基线) | MSGC-SDK (dev260605) | MSGC-当前源码 (HEAD) |
| --- | ---: | ---: | ---: |
| **吞吐 (req/s)** | **220** | **144** | **81** |
| **总请求数** | 13712 | 9001 | 5077 |
| **成功数** | 13712 | 8988 | 5060 |
| **失败数** | 0 | 13 | 17 |
| **GC 次数** | 499 | 1153 | 969 |
| **GC 总时间 (ms)** | 48108 | 25395 | 14095 |
| **GC 平均时间 (ms)** | 96.41 | 22.03 | 14.55 |
| **RSS 平均 (MB)** | 665.84 | 113.71 | 84.92 |
| **RSS 峰值 (MB)** | 1230.80 | 908.47 | 327.84 |
| **CPU 平均 (%)** | 484.40 | 326.34 | 223.35 |
| **NewArray (次)** | 520M | 278M | 157M |
| **TakeRegion (次)** | 2.26M | 1.36M | 0.66M |

## 现象

### 1. 当前源码 MSGC 吞吐严重劣化

当前源码构建的 MSGC runtime 吞吐仅 **81 req/s**，相比 SDK 版本 MSGC 的 **144 req/s** 下降了 **43.8%**，相比 CopyGC 基线的 **220 req/s** 下降了 **63.2%**。

### 2. 分配量大幅减少但吞吐不升反降

当前源码相比 SDK 版本：
- NewArray 减少 **43.6%** (278M → 157M)
- TakeRegion 减少 **51.5%** (1.36M → 0.66M)

分配减少说明 GC 触发更少或对象生命周期行为改变，但吞吐反而大幅下降，暗示 **mutator 本身变慢了**（mutator 阶段代码路径开销增大，而非 GC pause 时间变长）。

### 3. GC 总时间减少但未带来吞吐收益

当前源码 GC 总时间 **14.1s** 远低于 SDK 版本的 **25.4s**，但吞吐从 144 降到 81 req/s，说明：
- GC 不再是性能瓶颈
- **mutator 执行效率** 成为主要瓶颈

### 4. RSS 内存大幅下降

当前源码 MSGC 的 RSS 峰值 **327.84 MB** 和平均 **84.92 MB** 均远低于 SDK 版本 (908/113 MB) 和 CopyGC (1230/665 MB)。内存占用降低是好方向，但不应以大幅牺牲吞吐为代价。

## 根因分析

### 近期关键提交变更

按时间逆序，最近影响 MSGC 性能的提交：

1. **96edfffa** - 添加 newobj/takeregion 的数据记录,退出时打印
   - 增加了运行时统计记录开销，但只影响 exit 时打印，不应影响稳定运行吞吐

2. **bfb2c00b** - 改了 mutator assist 的实现,整体内存与时间优化回来了
   - 这个提交标题说"优化回来了"，但实际当前版本吞吐从 144→81，严重劣化
   - mutator assist 是 GC 让 mutator 协助标记/回收的机制，实现不当会严重拖慢 mutator

3. **d67a93be** - 改了一下 truelive 的计算
   - truelive 影响 GC 触发阈值计算，可能改变了 GC 频率和触发时机

4. **4ac7b913** - 添加标记校验器
   - 增加了标记阶段的校验逻辑，可能在 release 模式仍保留额外检查

5. **6c180e07** - GC 触发阈值改为纯 truelive 操纵,GC判断改为 swept region 触发
   - 改变了 GC 触发策略，可能导致 heap 利用率偏低或 GC 触发时机不优

6. **03aed14e** - GC 代际切换到 STW 中,GC期间 region 也在本轮 sweep 中
   - 改变了 GC 阶段模型，可能引入额外的 STW 开销

7. **13c0405e** - 大对象缓存
   - 大对象缓存可能影响分配路径

8. **bd77fe15** - 修复还在 mark-at-alloc 问题
   - mark-at-alloc 的修复可能改变了 write barrier 行为

### 最大嫌疑

**bfb2c00b (改了 mutator assist 的实现)** 和 **6c180e07 (GC触发阈值改为纯truelive操纵)** 是两个最可能引入回归的变更：

1. **Mutator assist 过多或实现低效**：如果 mutator 在每次分配时都要检查是否需要协助 GC，且 assist 路径的开销过大，会直接拖慢所有 mutator 线程，表现为总分配量下降 + 吞吐下降。这与观测数据(NewArray/TakeRegion 下降 40-50%，吞吐下降 44%)高度吻合。

2. **GC 触发阈值不当**：swept region 触发 + truelive 操纵可能导致了 heap 未充分利用就被触发 GC，或者 GC 触发过于保守/激进，导致 mutator 被不必要的 GC 活动拖慢。

## 结论

| 项目 | 状态 |
| --- | --- |
| 当前源码 MSGC 吞吐 | **81 req/s** —— 严重退化 |
| 对比 SDK (dev260605) MSGC | **144 req/s** —— 当前源码退化 43.8% |
| 对比 CopyGC | **220 req/s** —— 当前源码慢 63.2% |
| 主要嫌疑提交 | `bfb2c00b` (mutator assist 重写), `6c180e07` (GC 触发阈值改为 truelive) |
| 内存表现 | **优秀** —— RSS 峰值仅 327MB，远低于 SDK 的 908MB 和 CopyGC 的 1230MB |
| 核心矛盾 | 内存降到优秀水平，但吞吐劣化到不可接受的程度 |

**下一步建议**：
1. 在 `bfb2c00b` 处 `git revert` 或对比 benchmark，确认 mutator assist 改写的实际影响
2. 在 `6c180e07` 处检查 GC 触发策略的性能影响
3. 目标是找到一个 **内存与吞吐的折中点**：RSS 峰值 < 500MB 且吞吐 > 140 req/s（接近 SDK 水平）
