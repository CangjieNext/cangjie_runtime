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
- PR 的唤醒逻辑已**完整对齐上游**（第 2、3 轮 secbot 白盒扫描后的修复）：
  1. **条件边沿 `signal`**在**释放本方 Monitor 之后**才发出，避免了 `putMonitor↔takeMonitor`
     的 ABBA 死锁；
     - 消费者唤醒（`signalNotEmpty`）基于**已提交 `count`**（`prev==0`），与消费者阻塞所依据的
       计数口径一致；
     - **生产者唤醒（`signalNotFull`）基于槽位预约计数 `putQsize`**：`tryRemoveCore` 释放槽位时取
       `prevPut = putQsize.fetchSub(1)`，仅当 `prevPut == capacity`（释放前预约恰为满）才发信号。
       这样唤醒谓词与生产者阻塞谓词（`putQsize >= capacity`）口径一致，消除了第 3 轮 secbot 指出的
       "队列有空位但生产者永久挂起"活性缺陷（若用 `count==capacity` 判定会因预约/提交瞬时错位而漏唤醒）。
  2. 恢复上游的**链式 `notify`**：`add` 成功且 `prev+1 < capacity` 时 `putMonitor.notify()`
     唤醒另一生产者，`remove` 成功且 `prev > 1` 时 `takeMonitor.notify()` 唤醒另一消费者，
     消除 MPMC 下"有元素但消费者永挂 / 有空位但生产者永挂"的活性饥饿；
  3. `tryAddCore/tryRemoveCore` 的 OOM 回滚由 `catch (e: Exception)` 改为同时 `catch (e:
     Error)`（`OutOfMemoryError <: Error`，原 `catch (e: Exception)` 捕获不到），预约/认领
     计数在 OOM 时真正回补，不再泄漏。
- 以上均为局部改动，无锁快路径（满/空才落 Monitor）不受影响，SPSC 吞吐预期与上游持平。
- 若实测出现非预期回归，按 issue #947 口径 revert 性能部分（保持上游原版）。
