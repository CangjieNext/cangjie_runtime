# case27 EnableMSGCInlineWriteBarrier 跳转表截断问题修复记录

## 基本信息

- 日期：2026-07-10
- 场景：`MSGC_RESULT/Result/case27_http_complex_json`
- 用例口径：`c1 / p10K / 1s`（诊断跑），`c10 / p10K / 60s`（validate 跑）同样复现
- 失败现场：
  - `result/20260709_232403_case27_diag_struct_storeonly_c1_p10K_1s_heap4gb_1run/run1/run.stdout.txt`
  - `result/20260709_231544_case27_c10_p10k_validate_c10_p10K_60s_heap4gb_1run`（同样症状）
- 开关关系：`-enable-msgc-inline-write-barrier` **开启必现，关闭即好**
- 本次修改文件（均在 `cangjie_compiler/third_party/llvm-project`）：
  - `llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp`
  - `llvm/lib/Target/AArch64/AArch64InstrInfo.cpp`

## 症状

开启 inline 写屏障后，case27 的**每一个请求 100% 确定性失败**（`total=1705793 ok=0
fail=1705793`），客户端在发请求前就抛异常，异常信息固定为：

```
[WARN] warmup request failed: Invalid range start: 17 or end: 16
```

- 吞吐 80 万 req/s 说明请求根本没走到网络，是 client 侧 URL 解析阶段就抛了。
- 异常抛自 `stdlib/libs/std/core/string.cj:1180` 的 `String[](range)`。
- 对 URL `http://127.0.0.1:19129/process`：host 结束于下标 16（`:`），端口起始
  17，`/` 在 22。`parsePort` 收到的却是 `(start=17, end=16)`，即第二个参数拿到了
  `hostEnd` 而不是 `/` 的位置——说明 `findHostOrPath` 内部控制流已经错乱。

## 排查过程（关键步骤）

1. 先审查 `AArch64AsmPrinter.cpp::emitMccWriteBarrierFastPath` 的六条 fastpath
   语义（gate 检查、StoreOnly、EmitInlineMarkOrTo、struct 位图循环等），并与运行时
   `MSMarkBarrier::WriteReference/WriteStruct`、`RefField::SetTargetObject` 逐条对
   照——**语义全部一致**，寄存器只用 X0–X17（对 BL 调用点合法），排除屏障逻辑本身。
2. 核对 `MSRegionInfo` / `AllocBufferMS::activeLog_` / `WorkBuf` 硬编码偏移与运行时
   `static_assert`——全部吻合，排除 ABI 漂移。
3. 转向反汇编实际失败的产物 `libstdx.encoding.url.dylib`（23:15 构建，含 inline
   展开）中的 `URL::parseFromHost`，找到 match 分发点：

   ```
   b5a8: adr  x20, 0x261ce        ; 8-bit 压缩跳转表基址
   b5ac: ldrb w11, [x20, x9]      ; 表项 = (目标块 - 锚点 0xb568) / 4
   b5b0: add  x10, x10, x11, lsl #2
   b5b4: br   x10
   ```

4. dump 表内容并与真实 handler 地址对照，**实锤截断**：

   | 字符 | 表项字节 | br 实际落点 | 真正的 handler | 真实偏移/4 |
   |---|---|---|---|---|
   | `#` | `0x14` | 0xb5b8 ✓ | 0xb5b8 | 0x14（未截断） |
   | `/` | `0x71` | 0xb72c（**WB 内联序列中间**） | 0xbb2c（`parsePath`+`parsePort` 块） | **0x171** |
   | `?` | `0x66` | 0xb700（WB 序列中间） | 0xbb00 | **0x166** |
   | `@` | `0x7b` | 0xb754（WB 序列中间） | 0xbb54（`parseUserInfo` 块） | **0x17b** |

   三个表项的真实偏移都超过了 8-bit 表上限（255×4=1020 字节），MC 层**静默截断了
   高位 0x100**。`'/'` 分支的 `br` 落进某个写屏障内联序列中间，带着残留寄存器
   （x25=portStart=17、x22=hostEnd=16）乱序执行后汇入 `parsePort` 调用点，于是每次
   都是 `parsePort(17, 16)` → `_rawUrl[17..16]` → 固定抛
   `Invalid range start: 17 or end: 16`。

## 根因

**AsmPrinter 晚期内联展开的尺寸没有登记进指令尺寸账本。**

- `emitMccWriteBarrierFastPath` 在 AsmPrinter 阶段把一条
  `bl CJ_MCC_Write{RefField,StaticRef,StructField,StaticStruct}Fast_MSGC`
  展开为 ~84–122 条指令（336–488 字节）。
- 但 `AArch64InstrInfo::getCangjieSpecificCallInstSizeInBytes` 只覆盖了
  NewArrayFast(256)、safepoint(24/12)、stack-check、GetGCPhase、NativeStub、
  GetCJThreadId，**没有写屏障 wrapper 的 case**，这些调用仍按 4 字节计。
- 依赖该尺寸的**发射前** pass 因此拿到错误的布局：
  - `AArch64CompressJumpTables`：以为所有 case 块都在 1020 字节内，误选 8-bit
    跳转表；实际展开后偏移超限，`.byte (LBB-anchor)/4` fixup 被静默截断。
  - `BranchRelaxation`：同理可能让 `tbz/cbz/b.cc` 短分支越界（本例函数 ~10.7KB
    未触发，属潜在风险）。
- 这正是 `AsmPrinter.cpp` 中 `tryEmitCangjieSpecificCallByMOSym` 上方注释早已警告
  的约定："emit specific inst should update inst size info in
  AArch64InstrInfo::getInstSizeInBytes"。写屏障这组 wrapper 是被遗漏的。
- 关闭开关时调用保持为真实的 4 字节 `bl`，账实相符，所以一切正常——与"开启必现、
  关闭即好"完全吻合。

## 修复内容

### 1. `AsmPrinter.cpp`：导出开关

`EnableMSGCInlineWriteBarrier` 由 `static cl::opt` 改为 `namespace llvm` 下的非
static 定义，使 AArch64InstrInfo 能读到**同一个 flag**，保证"报的尺寸"和"实际是否
展开"永远同步（仓库已有 `CJPipeline`、`EnableStackGrow` 同样先例）。

### 2. `AArch64InstrInfo.cpp`：补尺寸 case

在 `getCangjieSpecificCallInstSizeInBytes` 末尾新增（flag 开启且 Mach-O 时生效，
与 AsmPrinter 的展开条件逐字一致）：

- `CJ_MCC_WriteRefFieldFast_MSGC` / `CJ_MCC_WriteStaticRefFast_MSGC` → **384** 字节
  （逐条数最坏 ~84 条指令 = 336B，向上取整）
- `CJ_MCC_WriteStructFieldFast_MSGC` / `CJ_MCC_WriteStaticStructFast_MSGC` →
  **512** 字节（最坏 ~122 条 = 488B）
- `CJ_MCC_ArrayCopy{Ref,Struct}Fast_MSGC` 目前只发射普通调用，不需要 case。

`getInstSizeInBytes` 的契约是返回**上界**：高估只让跳转表提前升到 16-bit、分支松弛
更保守（代价可忽略）；低估则是本次这种静默控制流损坏。

## 影响面

- **flag 开 + macOS**：含写屏障的大函数跳转表用 16-bit 表项（每项多 1 字节
  `.rodata`），极少数短分支被改写为长跳；运行时指令序列（屏障 fastpath 本身）
  **一条未改**，无运行时性能影响。
- **flag 关**：尺寸 case 被 flag 短路，生成代码与改动前逐字节一致，on/off 对比基线
  不受污染。
- **非 Mach-O**：triple 检查直接跳过，零影响。

## 遗留 / 后续建议

1. **同类隐患**：`EnableCJNewObjFast` 对非 statepoint 的 `CJ_MCC_NewObject` /
   `CJ_MCC_NewFinalizer` 直呼点也做内联展开且无尺寸 case。目前 MSGC 模式下这些
   调用都走 outlined 的 `_MSGC_dynamic` wrapper（小函数、无跳转表）未触雷，建议
   后续一并补上。
2. **MC 层加固**：`.byte` 标签差 fixup 溢出目前是静默截断而非报错，这才让 bug 藏
   得深。可考虑给 AArch64 压缩跳转表的 FK_Data_1 fixup 加 range check，独立加固项。
3. **维护约定**：以后任何新的 Cangjie 专属 AsmPrinter 调用点展开（或现有写屏障序列
   加指令），必须同步检查 `getCangjieSpecificCallInstSizeInBytes` 的上界是否仍然
   成立（Ref ≤ 384B / Struct ≤ 512B）。

## 验证状态

- 修复已提交到本地 llvm-project 源码，**尚未重新编译编译器 / 重建 stdx 回归
  case27**（失败的 `libstdx.encoding.url.dylib` 是 23:15 用带 bug 的编译器构建的，
  必须重建后验证）。
- 验证口径：重建后开启 `-enable-msgc-inline-write-barrier` 跑
  `case27 c1/p10K` 与 `c10/p10K/60s`，预期 warmup 不再报
  `Invalid range start`，`ok` 恢复正常；可另行反汇编确认 `parseFromHost` 的跳转表
  已升为 16-bit 表项（`ldrh` 取表）。
