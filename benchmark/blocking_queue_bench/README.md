# LinkedBlockingQueue 吞吐 / 正确性基准（MR #1782）

本目录是为回应 MR #1782 评审意见「补基准数据」而提交的压测 harness：在同一二进制内
对比 **PR 版 `LinkedBlockingQueue`** 与 **上游原版 `LinkedBlockingQueueUpstream`** 的吞吐与
并发正确性，作为 CI 跑出数据的入口。

## 它测什么
- `benchmarkThroughput`：SPSC（1 生产者 / 1 消费者）与 MPMC（8 生产者 / 8 消费者，容量 1024）
  两种场景，各投递 2,000,000 条，输出 PR vs 上游的 **ops/s** 对照表。
- 4 个正确性压测（复用 `Collector.verify`）：
  - `scenarioBlocking`（8p/8c，100k，容量 16）
  - `scenarioTryAddProducer`（仅 tryAdd 生产者，看门狗 60s）
  - `scenarioBlockingAddTryRemove`（add + tryRemove 混合）
  - `scenarioTimeout`（add/remove 超时路径）
- 看门狗：300s 内未完成视为死锁 / 丢失唤醒 / 计数错乱，进程以非 0 退出。
- 正确性失败或看门狗触发时进程退出码非 0；否则 0。

## 构建
需要可用的 Cangjie 工具链（`cjc`），且 `CANGJIE_HOME` 指向 SDK 根，相关 `bin / tools / runtime-lib`
已在 `PATH`（通常 source SDK 自带的 `envsetup` 即可）。

方式一（cjpm）：
```sh
cd benchmark/blocking_queue_bench
cjpm build
```

方式二（cjc 直接编译，本仓库验证所用）：
```sh
cjc -o lbq_bench \
  src/block_support.cj src/bq_interface.cj src/blocking_queue.cj \
  src/blocking_queue_upstream.cj src/main.cj
```

## 运行
```sh
./lbq_bench          # Linux
lbq_bench.exe        # Windows（cjc 在 Windows 上产物带 .exe）
```

## 关于 `src/blocking_queue.cj`（PR 快照）
它是 `stdlib/libs/std/collection/concurrent/blocking_queue.cj` 在 MR HEAD 处的**副本**，
为独立编译做了三处适配，与真实 stdlib 文件不同：
1. `package` 由 `std.collection.concurrent` 改为 `blocking_queue_bench`；
2. 删除了 `public type BlockingQueue<E> = LinkedBlockingQueue<E>` 类型别名及其 `@Deprecated` 注解；
3. 显式声明 `class LinkedBlockingQueue<E> <: BQueue<E>`（Cangjie 不做隐式子类型推断，
   独立编译需要；真实 stdlib 由类型别名/接口接线，不需要此句）。

若上游 stdlib 的 `blocking_queue.cj` 发生变更，请同步刷新本快照（重新生成并保留上述 3 处适配）。

## 重要环境说明
本 harness 由一台**无法运行**已编译仓颉二进制的开发机准备（系统 CRT 不兼容，运行必崩于
`msvcrt.dll`）。因此：
- 仅能在本机验证 **编译 / 类型检查 / 链接**（`cjc` 退出码 0、无 error）；
- 真实吞吐数字必须由 **CI runner / 具备可用仓颉运行时的机器** 实际运行产出。

## 结果解读
- 若 PR 在 **SPSC** 下出现回归：这是已知权衡——PR 的 `tryAdd/tryRemove` 每次成功都无条件
  `signal` 对端 Monitor，而上游是条件 `signal`。若数据不显著，按 issue #947 口径改回条件
  `signal` 或 revert 性能部分（保持上游原版）。
- MPMC 下若持平或占优，说明无锁预约协议的收益成立。
