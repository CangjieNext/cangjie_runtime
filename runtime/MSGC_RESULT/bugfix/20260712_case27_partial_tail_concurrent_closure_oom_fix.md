# Case27 private partial 并发闭包导致 OOM

## 现象

执行：

```bash
python3 build/scripts/run_case27_c10_p10k.py --run-timeout-sec 100
```

约 8 秒后 regular allocation 即使申请 23B 也失败，进程随后因未处理 OOM 路径退出为 139。

## 根因

初版任意 WorkBuf 消费改动在 `QuiesceMarkWorkers()` 中把 GC main/helper 的私有 partial `putCur_`
发布回 Global，并恢复 helper 继续并发闭包。在持续写屏障负载下形成正反馈：闭合 partial 延长 mark，
mutator 在更长 mark 窗口内继续分配并产生 old-ref，`MarkNewObject()` 又将期间新对象保活，Global
工作持续增长。

失败版首轮 GC 从约 23MB active heap 开始，但 ConcurrentMark 持续 2.49s，写屏障记录 2756 万对象，
mark bitmap 达 7700 万对象/5.05GB，heap threshold 被推到 9.99GB。第二轮尚未完成即耗尽 region。

统计本地化不是根因：在 Step43 落地前的 01:09/01:10 运行中已出现同样 OOM。

## 修复

- GC main/helper 继续用 `Pop()` 获取 Global 中已经发布的 full/partial buffer。
- quiesce 不再发布 owner 私有 partial `putCur_`。
- Global late work 仍会取消收敛并恢复 helper。
- 私有 partial 由 `PerPGCWork::Dispose()` 发布，在 MarkTermination STW 中固定点扫描。
- mutator assist 仍只通过 `PopFull()` 获取满 buffer。

## 验证

用户原命令完整运行 60 秒：6658 req/s、400392 成功、0 失败，无 OOM/FATAL。首轮 ConcurrentMark
降至 602us，marked objects 为 43181，heap threshold 保持 20MB；末轮仍正常回收。

结果目录：
`MSGC_RESULT/Result/case27_http_complex_json/result/20260712_013018_case27_c10_p10k_validate_c10_p10K_60s_heap4gb_1run`
