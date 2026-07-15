# MSGC Parallel Sweep 默认开启设计增量

## 1. 目标

将标准 Runtime 环境初始化路径中的 `cjMSParallelSweep` 默认值从关闭改为开启，使 HEU 因
`DEFER_FOR_SWEEP` 延后时，默认允许 `gc-helper` 协调 bounded Region Sweep 并借用空闲 GC helper。

这次变更只调整发布默认值，不改变 Step44 已实现的并行 Sweep 算法和正确性协议。

## 2. 配置语义

- 未设置 `cjMSParallelSweep`：默认开启。
- 显式设置 `cjMSParallelSweep=false`：关闭动态 parallel Sweep，保留快速回退能力。
- 显式设置 `cjMSParallelSweep=true`：开启，行为与默认状态一致。
- `cjMSParallelSweepMaxHelpers=0`：继续使用自动上限，最多借用 `cjGCThreads - 1` 个 helper。
- `cjMSBackgroundSweep=false`：继续同时禁用 parallel Sweep，不改变原有依赖关系。
- 外部调用者通过 `RuntimeParam.gcParam.msParallelSweep=0` 显式传参时：继续表示关闭，不改变公共 API 的
  0/1 语义，也不引入三态字段。

## 3. 代码与文档改动

1. 在 `src/CjScheduler.cpp` 中把 `ReadBoolParameter("cjMSParallelSweep", false)` 改为默认 `true`。
2. 扩展源码契约测试，明确断言标准环境初始化默认开启，同时保留显式关闭配置路径。
3. 更新 `MSGC_RESULT/Implementation/Step44_MSGC_NotifyMark动态SweepHelper设计.md`：
   - 状态改为默认开启；
   - 修正配置、发布策略、验证结论和限制说明；
   - 保留 feature-off 对照和故障回退说明。
4. 更新 Runtime GC 知识文档，避免继续把 dynamic helper 描述为默认关闭。
5. 历史实施计划保留其当时的 default-off 落地记录，不改写历史；本设计增量记录默认值切换。

## 4. 不变项与安全边界

- `CJ_MS_STW_SWEEP` 诊断规则不删除、不改义。
- `StopAndWaitForAllSweep()`、`ForceFinishAllUnswept()` 和 active-counter quiescence fence 保持不变。
- `IDLE/SWEEP/MARK/STOPPING` worker-pool lease 保持不变。
- helper 计算公式、Region claim、deadline、quantum 和 GC preemption/交接协议保持不变。
- 显式关闭后必须完全退回普通 background Sweep 路径，helper 计数应保持为 0。

## 5. 验证设计

验证分为三层：

1. 契约与单元测试：先让“默认必须为 true”的断言在旧代码上失败，再修改实现并确认通过；同时运行
   parallel Sweep 策略、worker-pool owner 和异步 HEU 状态测试。
2. MarkSweep Release 全量构建：验证 C++14、多平台公共头和链接没有回归。
3. macOS aarch64 SDK 验证：替换并重新签名 runtime 后，在 `env -i` 隔离环境中重新编译 `.cj` 用例；
   用 `DYLD_PRINT_LIBRARIES=1` 核对实际加载路径，再分别运行：
   - 不设置 `cjMSParallelSweep`，确认默认路径会在 sweep debt 条件满足时进入 parallel Sweep；
   - 显式 `cjMSParallelSweep=false`，确认程序功能正常且不会启动 helper Sweep。

SDK 三件套固定为：compiler、runtime、stdlib 均来自
`/Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260509/cangjie`，其中 runtime dylib 使用当前工作区
新构建产物覆盖；覆盖后所有 `.cj` 测试必须重新编译。

## 6. 完成标准

- 源码默认值为 `true`，环境变量显式 `false` 仍能关闭。
- 专项测试、契约测试、`git diff --check` 和 MarkSweep Release 构建全部退出 0。
- 实机默认开启与显式关闭两条路径都完成重新编译、dylib 来源核对和功能验证。
- Step44 与 Runtime GC 知识文档和源码默认行为一致。

