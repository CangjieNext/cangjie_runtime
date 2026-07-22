# case25 stall 当前排查状态与下一步（2026-06-06）

## 背景

目标问题：

- `MSGC_RESULT/Result/case25_http_complex_json`
- 在 `dev260605` SDK + 当前 runtime/stdlib 组合下，MSGC side 存在明显平台期和偶发长时间停滞
- 表现为：
  - 某些 2s 窗口 `+last2s=0`
  - 随后按 `readTimeout=5s` 节奏出现一批 `fail`

本文件用于记录：

- 目前已经推进到什么程度
- 哪些假设已经被证伪
- 当前根因范围收敛到了哪里
- 下一步最值得继续打的点

## 当前有效结论

### 1. 最早那批“全 0 / fail>0”的对比数据无效

旧报告中曾出现：

- `ok=0`
- `fail>0`
- RSS / CPU 全 0

后确认是本地回环网络权限问题，`stdout.txt` 中存在：

- `SocketException: Failed to bind/connect ... Operation not permitted`

因此这批数据不能用于性能判断。

### 2. `EV_CLEAR` 去掉确实是一个真实大问题

代码位置：

```cpp
src/CJThread/src/runtime/netpoll/src/macos/netpoll.cpp
```

差异：

```diff
- EV_SET(&event, fd, events, EV_ADD | EV_ENABLE, 0, 0, data);
+ EV_SET(&event, fd, events, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, data);
```

恢复 `EV_CLEAR` 后：

- 一轮 case25 吞吐从约 `122 req/s` 恢复到约 `229 req/s`
- 之前那种 `32s` 左右几乎整段停死的现象明显缓解

结论：

- `EV_CLEAR` 去掉会严重放大问题
- 但恢复 `EV_CLEAR` 之后，case25 仍然不稳定
- 所以它是一个主问题，但不是当前剩余 stall 的唯一根因

相关结果目录：

- [20260606_103444_d605_current_src_runtime_reverify_c8_p100K_compare](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case25_http_complex_json/result/20260606_103444_d605_current_src_runtime_reverify_c8_p100K_compare)
- [20260606_114134_d605_evclear_restore_reverify_c8_p100K_compare](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case25_http_complex_json/result/20260606_114134_d605_evclear_restore_reverify_c8_p100K_compare)

### 3. 仅仅把 `CASE25_CLIENT_TIMEOUT_MS` 从 5s 调到 15s 不是根治

曾经有一轮 `15s timeout` 的好结果：

- MSGC `253 req/s`
- `15787 ok / 0 fail`

但后续相同配置复跑不稳定：

- 一轮 `172 req/s`
- 一轮几乎卡死到 `3 req/s`

结论：

- timeout 改大只能改变症状窗口
- 不能从根本上解决问题

### 4. 不是 GC 根因

目前已经有足够证据排除“GC 本身把程序卡住”：

1. stall 窗口里 CPU 会掉到很低，不像 GC 重负载
2. `gc.log` 在停滞窗口附近只有毫秒级 GC 活动，没有秒级长 pause
3. `sample` 主栈集中在：
   - `stdx.net.http::ConnNode.sendRequestTimeout`
   - `HttpNormalBodyProvider.read`
   - `CJ_SchdfdWaitInlockTimeout`
   - `CJ_SchdpollWait`

代表性现场：

- [iter_1_20260606_125350](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case25_http_complex_json/result/sample_loop/iter_1_20260606_125350)
- [iter_2_20260606_131259](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case25_http_complex_json/result/sample_loop/iter_2_20260606_131259)

### 5. 不是 `stdx` 的“旧 read timer 泄漏误杀已回池连接”

这是 2026-06-06 这轮最重要的新收敛。

为了验证 timer 是否误杀“后续复用”的连接，做了两件事：

1. 在 runtime 里补了 `CJThread` 诊断
   - `schdpoll.cpp`
   - `schdfd.cpp`
2. 在 `stdx.net.http` 里补了最小诊断版
   - 工作副本：`runtime/.tmp/cangjie_stdx_diag`
   - 关键点：
     - `ConnNode.sendRequestTimeout`
     - `setReadTimer`
     - `HttpEngine1.returnConn`
     - `closeConnInUse`
     - `HttpNormalBodyProvider.read`

并构建出一套诊断版 stdx：

- `runtime/.tmp/cangjie_stdx_diag/target/darwin_aarch64_cjnative/dynamic/stdx`

然后用它重新跑 case25，抓到新现场：

- [iter_1_20260606_134539](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case25_http_complex_json/result/sample_loop/iter_1_20260606_134539)

关键证据：

以 `connId=2` 为例，在 [stdout.txt](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case25_http_complex_json/result/sample_loop/iter_1_20260606_134539/stdout.txt:41726) 附近：

- `13:46:10.xxx` 左右设置 `readTimerSeq=724`
- `13:46:15.867941` 触发 `[readTimer-fire] connId=2 reqSeq=724 readTimerSeq=724`
- 随后同一请求立刻进入：
  - `[engine-closeConn]`
  - `[conn-close]`
  - `[sendRequest-ex] ... SocketException: Socket is already closed`

同样模式出现在：

- `connId=6 reqSeq=725`
- `connId=9 reqSeq=722`
- `connId=7 reqSeq=726`

这说明：

- 触发超时的是**当前请求自己的 read timer**
- 不是“旧 timer 留下来，误杀后续复用请求”
- 这些请求是真的在读响应阶段卡满了 `5s`

## 当前仍然成立的现象判断

### 1. timeout 是症状出口，不是根因本体

现在可以更精确地说：

- `stdx` 的 `readTimer -> closeConnInUse(this)` 确实在执行
- 但它是在当前请求已经卡满 `5s` 之后才触发
- 所以它是“超时收尾机制”
- 不是“把正常请求误关掉”的根因

### 2. 卡住位置仍然在 client 读响应 / netpoll 等待链路

`sample_output.txt` 仍然主要落在：

- `HttpNormalBodyProvider.read`
- `ConnNode.sendRequestTimeout`
- `CJ_SchdfdWaitInlockTimeout`
- `CJ_SchdpollWait`

说明当前剩余问题仍更像：

- client 读响应没有继续推进
- 或者底层 fd readiness / wait 状态没有继续驱动这个读过程

### 3. runtime 侧看到的是 close-path 后果，不是最初因

之前 `CJThread` 日志里已经抓到：

- `PD_CLOSING`
- `SockClose success`
- `recv len: 0`

这些仍然更像：

- 上层 timeout 之后关连接
- runtime 负责做 close-path 清场

因此当前 remaining issue 仍然不应解读成“GC 导致卡住”，也不应简单解读成“stdx timer 泄漏”。

### 4. 新增 requestId 证据：多数 timeout 请求在 server 应用层已经处理完成

为了回答“超时请求 server 端到底有没有处理完”，在 case25 benchmark 中追加了最小追踪：

- `client-send reqId=<N>`
- `client-ok / client-fail`
- `server-start reqId=<N>`
- `server-done reqId=<N>`

并修复了 `run_sample_until_stall.sh` 对旧 binary 的复用问题：

- 当 `benchmark.cj` 比 `sample_loop/benchmark` 新时自动重编
- 支持 `CASE25_FORCE_REBUILD=1`

对应现场：

- [iter_1_20260606_135858](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case25_http_complex_json/result/sample_loop/iter_1_20260606_135858)

关键观察：

多数 timeout 请求，在 `client-fail` 之前已经出现：

- `server-start reqId=<N>`
- `server-done reqId=<N>`

例如：

- `reqId=6559`
- `reqId=6574`
- `reqId=6580`
- `reqId=6584`
- `reqId=6608`
- `reqId=6612`
- `reqId=6621`
- `reqId=6622`
- `reqId=6625`
- `reqId=6642`
- `reqId=6646`
- `reqId=6650`
- `reqId=6664`
- `reqId=6668`
- `reqId=6671`

代表性模式见 [stdout.txt](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case25_http_complex_json/result/sample_loop/iter_1_20260606_135858/stdout.txt:27129)：

```text
[CASE25-REQ][client-send] worker=2 reqId=6559
[CASE25-REQ][server-start] reqId=6559
[CASE25-REQ][server-done] reqId=6559 seq=6578
[CASE25-REQ][client-fail] worker=2 reqId=6559 ex=Client1.1 read response timeout and the connection is closed.
```

这说明：

- server benchmark handler 里的应用层处理已经完成
- 但 client 仍然可能在后面 `read response timeout`

因此当前更接近的判断是：

- 问题不主要在 server 业务计算本身
- 更可能在：
  - server writeResponse / socket 交付
  - 或 client read / fd readiness / 唤醒

进一步补充：

- 在这轮稳定 stdx 的现场里，没有看到 server 侧 `write response timeout`
- 但 sample 中可以看到：
  - `HttpEngineConn1.writeResponse`
  - `TcpSocket.write`
  - client 侧 `HttpNormalBodyProvider.read -> CJ_SchdfdWaitInlockTimeout -> CJ_SchdpollWait`

这说明当前现象更像：

- server 写响应路径在运行
- 但响应写出后的交付 / client 读侧推进仍可能异常

这更支持当前判断：

- server 写响应路径在运行
- 剩余问题更像“写出后的交付/唤醒链路不稳定”，而不是 server 业务代码卡住

补充说明：

- 少数 timeout 请求只看到 `client-send -> client-fail`，没有 `server-start`
- 例如 `reqId=6638`、`reqId=6653`
- 这更像是前一批 timeout/close 之后的后继影响，不像首批 stall 的起点

## 已经做过的排查/验证

### runtime 侧

1. 恢复 `EV_CLEAR`
2. 在 `schdpoll.cpp` 加状态迁移日志
3. 在 `schdfd.cpp` 加 timeout 路径日志
4. 通过 `MRT_LOG_CJTHREAD` / `MRT_LOG_LEVEL=i` 将日志落盘

### stdx 侧

1. 在工作区复制了诊断副本：
   - `runtime/.tmp/cangjie_stdx_diag`
2. 按 `build_cangjie_compiler_one_shot_zh.md` 的链路，参考 stdx 自身 `build.py`
   - `python3 build.py build -t release --target-lib=/opt/homebrew/opt/openssl@3/lib`
   - `python3 build.py install`
3. 将诊断版 `dynamic/stdx` 直接用于 case25 复现

### benchmark 侧

1. `case26_http_keepalive_minimal` 已经建好
2. `case26` 当前无法稳定复现 case25 的 stall
3. 因此当前主排查重新回到 case25 本体

## 当前剩余根因范围

当前根因已经收敛到下面这条链：

- `stdx.net.http` client 读响应
- `TcpSocket.read`
- `SchdfdWaitInlockTimeout`
- `SchdpollWait / SchdpollReady`
- macOS `netpoll`

更具体一点：

- 某些请求在 client 读响应阶段不再推进
- 5 秒后由当前请求自己的 `readTimer` 关闭连接
- 随后 worker 看到 `SocketException`

## 2026-06-06 新增对照：当前源码构建的 CopyGC runtime 在 CopyGC SDK 中无此错误

为了回答“为什么会出现在 MSGC 中”，补做了一轮控制实验：

- 用当前 runtime 源码重新编译 **CopyGC runtime**
- GC 模式：`--gc-mode copygc`
- 临时替换到：
  - `/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/cangjie`
- 使用 CopyGC SDK 自己的 stdx：
  - `/Users/lipeisheng/code/cangjie/cangjie-pkg/dev/20260507/darwin_aarch64_cjnative/dynamic/stdx`
- 重新编译 case25 并单跑 sample

结果目录：

- [20260606_142213_copygc_runtimecheck_sample](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case25_http_complex_json/result/20260606_142213_copygc_runtimecheck_sample)

关键结果：

- `15078 total / 15078 ok / 0 fail`
- `242 req/s`
- 全程没有出现 MSGC 那种：
  - `+last2s=0`
  - `read response timeout`
  - 成批 timeout fail

代表性 progress 见 [stdout.txt](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case25_http_complex_json/result/20260606_142213_copygc_runtimecheck_sample/stdout.txt:2091)：

- `t=30144ms total=7430 ok=7430 fail=0 +last2s=465`
- `t=40193ms total=9795 ok=9795 fail=0 +last2s=469`
- `t=60279ms total=15070 ok=15070 fail=0 +last2s=531`

### 这个对照说明什么

这说明：

- **同一份当前 runtime 源码**
- 在 **CopyGC 模式** 下
- 跑 case25 是稳定的

因此当前问题**不是简单的“共享的 netpoll / schdfd / schdpoll 代码只要来自当前源码就必坏”**。

它更像是：

- 只有在 **MSGC runtime / MSGC SDK 栈**
- 或 MSGC 特有的 GC/调度交互条件叠加时
- 才会触发这类 stall

### 补充说明

这轮还验证到一个细节：

- `dev/20260507` 当前正在使用的 `libcangjie-runtime.dylib`
- 与本次源码重编的 CopyGC runtime 的 MD5 一致：
  - `77aeb52c488b3836847c854ee82ea96f`

所以这轮控制实验本质上也等价于再次确认：

- 当前 CopyGC runtime 栈在 case25 上是稳定的

## 2026-06-06 新增控制：`CJ_MS_STW_SWEEP=1` 不能消除 stall

为了判断问题是否主要来自 MSGC 的并发 sweep / stale edge 路径，追加跑了一轮：

- SDK：`self/dev260605/cangjie`
- STDX：`self/dev260605/darwin_aarch64_cjnative/dynamic/stdx`
- 环境变量：`CJ_MS_STW_SWEEP=1`

结果目录：

- [20260606_143402_sample_profile](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case25_http_complex_json/result/20260606_143402_sample_profile)

结果：

- 仍然出现明显 stall
- 仍然出现成批 `Client1.1 read response timeout`
- 末尾结果：
  - `12996 total`
  - `12980 ok / 16 fail`
  - `208 req/s`

代表性现象见 [stdout.txt](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case25_http_complex_json/result/20260606_143402_sample_profile/stdout.txt:51952)：

- `t=54216ms total=12963 ok=12963 fail=0 +last2s=0`
- `t=56222ms total=12963 ok=12963 fail=0 +last2s=0`
- 然后开始出现一批 `client-fail ... read response timeout`

结论：

- `CJ_MS_STW_SWEEP=1` **不能消除** case25 的 stall
- 所以当前问题**不太像主要由并发 sweep 本身导致**
- 也就是说，诊断开关保留仍然有价值，但它不是这次 case25 stall 的主刀口

## 当前最值得继续做的事

### 1. 继续确认“超时请求对应的 server 端到底有没有完成响应”

当前还差一个关键闭环：

- client 侧已经确认“当前请求真的等了 5s”
- 但还没有把这个请求与 server 端处理结果一一对应

下一步建议：

- 给 case25 请求加一个可追踪 `requestId`
- client 侧发送时打日志
- server 侧收到/处理完成/写回时打同一个 `requestId`

这样可以回答：

- server 是否已经处理完并写回，但 client 没读到
- 还是 server 压根也卡在读/写某一步

### 2. 把 `connId -> socket local/remote address -> runtime fd/pd` 串起来

当前 `ConnNode` 诊断已经有：

- `connId`
- `reqSeq`
- `readTimerSeq`

但 runtime `CJThread` 侧主要还是：

- `pd`
- `sock`
- `PD_CLOSING`

下一步最好把：

- client connId
- local/remote port
- runtime fd/pd

串成一条线，才能最终确认是否存在：

- 可读事件丢失
- ready/closing 状态异常迁移
- 某些 fd 上的读等待没有被继续唤醒

### 3. 暂时不要优先修改 `stdx` timeout 行为

当前证据已经表明：

- timeout 不是误杀已回池连接
- 它只是“当前请求卡满 5s 后”的收尾动作

因此不建议优先做：

- 改 `readTimeout` 默认值
- 修改 `setReadTimer -> closeConnInUse(this)` 的策略

这类改动最多只能改变症状窗口，不太可能直接修复根因。

## 本轮代表性文件

- 诊断 stall 现场：
  - [stdout.txt](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case25_http_complex_json/result/sample_loop/iter_1_20260606_134539/stdout.txt:1)
  - [sample_output.txt](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case25_http_complex_json/result/sample_loop/iter_1_20260606_134539/sample_output.txt:1)
  - [gc.log](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case25_http_complex_json/result/sample_loop/iter_1_20260606_134539/gc.log:1)
- 当前 case25 benchmark：
  - [benchmark.cj](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case25_http_complex_json/benchmark.cj:1)
- 诊断版 stdx 副本：
  - [http_client1_1.cj](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/.tmp/cangjie_stdx_diag/src/stdx/net/http/http_client1_1.cj:1)
  - [http_body.cj](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/.tmp/cangjie_stdx_diag/src/stdx/net/http/http_body.cj:1)

## 与旧文档关系

旧文档：

- [20260606_current_src_case25_regression.md](/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/bugfix/20260606_current_src_case25_regression.md:1)

其中关于：

- `mutator assist`
- `heapThreshold`
- “stdx timer 可能误杀后续复用连接”

这些判断，已经被当前这轮证据明显弱化或部分证伪。

后续请优先以本文件为新的状态基线继续推进。
