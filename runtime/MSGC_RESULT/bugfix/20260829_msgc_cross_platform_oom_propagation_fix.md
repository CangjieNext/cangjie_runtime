# MSGC：跨平台提交/映射失败转为 OutOfMemoryError

## 结论

修复 Windows、macOS 和 Linux 上会绕过语言层 OOM 处理并被 `CHECK` 终止的 MSGC 内存提交/位图映射失败路径。现在失败会回到 `MSRegionManager::TakeRegions` / 分配器的空指针结果，最终进入既有 OOM GC 和 `OutOfMemoryError` 路径。

修改的 runtime 已通过 `build/scripts/rebuild_msgc_runtime_sdk.py` 构建、安装到 `C:\cangjie\cangjie_compiler_CMSGC\output\msgc`，构建脚本校验新 DLL 与 SDK 中 DLL 的 SHA-256 前缀均为 `5d4aeecd3b15bfa9`。

## 问题与根因

### 已有 Windows 触发证据

`MSGC_RESULT\Tests\Test-windows\artifacts\01-runtime-llt-corrected\results.json` 中，`Expection39_NewArray16` 和 `Expection41_NewArray64` 均以 marksweep 编译。二者分别申请约 4 GiB 的 `Int16` 数组和约 16 GiB 的 `Int64` 数组；旧 runtime 在 `FreeManager::ReuseUnits` 发生下列终止，而不是让仓颉异常机制处理失败：

```text
F Check failed: ret != nullptr
F VirtualAlloc failed in FreeManager::ReuseUnits
```

同批的 `wahStackmap_01` 还记录了 `VirtualAlloc commit failed in GetPage, errno: 1455`；Windows 错误 1455 表示系统提交额度/分页文件不足。这说明问题不是用例期望错误，而是 runtime 将可恢复的资源不足错误提升成了进程致命检查。

### 平台差异

- Windows：`FreeManager::ReuseUnits` 为已保留但被 scavenging 回收提交的区域调用 `VirtualAlloc(..., MEM_COMMIT, ...)`。原实现对空返回执行 `CHECK_DETAIL`，直接结束进程。
- macOS/Linux：`ReuseUnits` 本身不执行同一提交调用；释放阶段分别使用 `mmap(MAP_FIXED)` 和 `madvise`。因此 Windows 的该精确分支不是 macOS/Linux 的直接故障点。
- 三个平台共同路径：`MSBitmapArena::EnsurePlaneRangeCommitted` 调用 `RemapChunkRange` 激活对象分配所需的 alloc/mark/scan bitmap 平面。其底层在 Windows 为 `VirtualAlloc(MEM_COMMIT)`，在 macOS/Linux 为固定地址 `mmap`；原代码同样用 `CHECK_DETAIL` 处理失败。因此三平台都可能绕过 OOM 异常链。

## 修复设计

### FreeManager 提交失败

1. `ReuseUnits` 改为返回 `bool`；Windows 提交失败记录地址、长度和 `GetLastError()`，不再触发 `CHECK`。
2. `TryTakeResult` 增加 `COMMIT_FAILED`。本地或跨 shard 复用失败时，`TryTakeRegions` 将已选择的 span 回滚，再向上返回该结果。
3. `RollbackFailedReuseLocked` 恢复 free 计数、位图和统计信息，并把失败 span 保守地标记为 scavenged/cold。后续复用一定会再次提交，避免把未提交内存错误视为 hot-free 内存。
4. `TryTakeOrGrowRegions` 和 `MSRegionManager::TakeRegions` 遇到 `COMMIT_FAILED` 立即返回失败，不会继续扩容掩盖真实提交失败。常规分配器随后执行已有的 OOM GC / `OutOfMemoryError` 逻辑。

### 位图映射失败

1. `MSBitmapArena::EnsurePlaneRangeCommitted` 改为返回 `bool`，失败时保留系统错误日志并不再 `CHECK`。
2. `MSRegionInfo::InitForSizeClass` 将三个 bitmap 平面的初始化失败向上传播，并在失败时 `Reset()` 已写入的 region 状态。
3. `MSCentralRegionPool::GetFreshRegion` 和 `MSAllocBuffer` 在初始化失败时归还刚取得的 region，并把失败作为分配失败返回，避免 region 泄漏或携带半初始化 bitmap 指针继续运行。

## 验证结果

### Windows：已执行

构建命令：

```powershell
python build/scripts/rebuild_msgc_runtime_sdk.py `
  --runtime-root C:\cangjie\cangjie_runtime_CMSGC\runtime `
  --sdk-root C:\cangjie\cangjie_compiler_CMSGC\output\msgc `
  --build-type release --version 0.0.1 --skip-clean
```

构建退出码为 0。替换 runtime 后，两个用例均使用上述 MSGC SDK、`cjc.exe --gc-mode marksweep` 重新编译，未复用旧 binary。

| 用例 | 受限条件 | 可观察结果 | 结论 |
| --- | --- | --- | --- |
| `Expection39_NewArray16` | `cjMSMaxVirtualHeapSize=64MB`，`cjHeapSize=64MB` | 退出码 1；标准输出 `Out of memory`；runtime log 有 `reason=1(oom)` 和 `RawArray<Int16> ... throw OutOfMemoryError` | 通过 |
| `Expection41_NewArray64` | 同上 | 退出码 1；标准输出 `Out of memory`；runtime log 有 `reason=1(oom)` 和 `RawArray<Int64> ... throw OutOfMemoryError` | 通过 |

日志和新 binary 位于 `MSGC_RESULT\bugfix\2026-08-29_msgc_oom\`。这两个回归证明“分配失败 -> OOM GC -> `OutOfMemoryError`”链路在修改后的 SDK 中正常工作。

### 验证边界

本机在不设限制时能够实际提交 Expection39 所需的约 4 GiB；该次运行超过 120 秒后已人工停止，日志显示对象已进入大对象分配/GC，而没有再次出现旧的 `VirtualAlloc` 错误。因此，修改后的 Windows **真实系统 `VirtualAlloc` 提交失败分支没有在本机再次确定性重现**，不能将受限堆回归误写成该特定系统调用分支已覆盖。

本轮环境为 Windows，未在 macOS/Linux 主机执行二进制回归。两平台的共用 bitmap 映射错误传播已完成代码级修复和 Windows 编译验证；需要分别在对应主机用同一脚本构建 MSGC runtime 后重新编译并运行 OOM 用例，才能声明三平台运行时验证完成。

## 影响范围与后续项

涉及文件：

- `src/Heap/MAllocator/MSFreeManager.{h,cpp}`：提交失败的回滚和向上返回。
- `src/Heap/MAllocator/MSRegionManager.cpp`：将 `COMMIT_FAILED` 传回对象分配路径。
- `src/Heap/MAllocator/MSBitmapArena.{h,cpp}`、`MSRegionInfo.{h,cpp}`、`MSCentralRegionPool.cpp`、`MSAllocBuffer.cpp`：三平台 bitmap 映射失败的传播和 region 归还。

`PagePool` / `MemMap` 的启动期或运行时自身元数据原生分配仍有独立的 fatal 路径；它们可能发生在仓颉对象分配器和异常运行环境尚未可用的阶段，不能在没有明确所有权和调用方协议设计的情况下直接改成语言层 `OutOfMemoryError`。本次只覆盖已经位于用户对象分配链、可以安全返回失败的路径。

建议后续在 Windows 添加可控的 `VirtualAlloc` 失败注入或资源限制测试，在 macOS/Linux 分别注入 `mmap` 失败，以精确覆盖所有新错误分支；该测试设施不包含在本次最小修复中。
