# 2026-07-02 case27 验证：重新打开 4 个 MSGC 写屏障 fastpath 后失败

## 结论

- 基于当前 `compiler/output` 重新完成了 `cjc + runtime + std + stdx` 的增量重编与安装。
- `AArch64 AsmPrinter` 分发中重新包含：
  - `CJ_MCC_WriteRefFieldFast_MSGC`
  - `CJ_MCC_WriteStructFieldFast_MSGC`
  - `CJ_MCC_WriteStaticRefFast_MSGC`
  - `CJ_MCC_WriteStaticStructFast_MSGC`
- 使用 fresh SDK 跑 `case27` 默认验证（`c1000 + p100K`）时，**运行失败**，退出码 `139`。

## 验证配置

- SDK：`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output`
- STDX：`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/darwin_aarch64_cjnative/dynamic/stdx`
- case：`/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case27_http_complex_json`
- result：
  - `/Users/lipeisheng/code/cangjie/cangjie/cangjie_runtime_MSGC/runtime/MSGC_RESULT/Result/case27_http_complex_json/result/20260702_150754_compiler_output_sdk_reenable_4msgc_write_fastpath_c1000_p100K_run2`
- 参数：
  - `CASE27_PAYLOAD=100K`
  - `CASE27_CONCURRENCY=1000`
  - `CASE27_DURATION_MS=60000`
  - `cjHeapSize=12gb`
  - `cjHeapGrowth=1`
  - `cjScavengeGrowth=10`

## 现象

- `dyld probe` 通过，确认实际加载的是：
  - `compiler/output/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib`
  - `compiler/output/darwin_aarch64_cjnative/dynamic/stdx/*.dylib`
- 正式运行时在 warmup 阶段崩溃：
  - `stdout.txt` 最后停在 `Warmup: 20 sequential requests...`
  - `stderr.txt` 显示 `SIGSEGV`
  - `gc.log` 显示第一次 heuristic GC 刚启动后即发生崩溃

## crash 指纹

`stderr.txt` 栈顶：

- `__CNai12parseCompIntHRNat5ArrayIhEmmmb`
- `__CN18stdx.encoding.json15parseJsonNumberHCNY_10JsonParserE`
- `__CN18stdx.encoding.json9JsonValue7fromStrHRNat6StringE`
- `benchmark.cj:294 processRequest`

`gc.log` 时间关系：

- `15:08:02.919882`：`Begin GC log. GCReason: heuristic`
- `15:08:02.920094`：`[GC] Start Smooth Collector`
- `15:08:02.925869`：`Handle signal: 11`

这说明崩溃发生在 **case27 JSON parse 路径**，且时间上紧贴 **第一次 GC 启动**，与“重新打开这 4 个写屏障 fastpath 后重新引入 GC 相关对象/引用破坏”高度一致。
