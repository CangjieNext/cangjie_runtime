# case27 空字符串 key 现象补充结论

## 结论

- `case27 c1000 p100K` 里触发 `MCC_AcquireRawData -> appendEscape(strKey)` 的 `strKey`，更像是 `HashMapEntry<String, JsonValue>.key` 被打成了 `zeroValue<String>()`，不是 payload 原始 JSON 自带空 key。
- `MCC_AcquireRawData` 的判空顺序错误只是把这个坏 key 放大成了 `addr=0x40` 的 SIGSEGV；真正上游更像是 `HashMap.resize()` 时 `Array<HashMapEntry<String, JsonValue>>` 的 struct bulk copy 破坏了 key 字段。

## 关键证据

1. `100K.json` 中未发现空 key / 空 string。
2. `stdx.encoding.json.parseJsonObject()` 会按对象字段数精确建 `HashMap<String, JsonValue>(count)`。
3. `case27` payload 中每个 `event` 对象固定有 `12` 个字段，而 `processRequest()` 会统一执行：
   - `item.put("server_marker", JsonString("m-${i}"))`
4. 这会让每个 `event` 对象从 `12 -> 13`，必然触发 `HashMap.add()` 中的 `resize(size + 1)`。
5. `HashMap.resize()` 在扩 `entries` 时会：
   - 先用 `HashMapEntry<K, V>()` 填满新数组
   - 再执行 `entries.copyTo(newEntries, 0, 0, oldEntriesSize)`
6. `HashMapEntry<K, V>()` 的默认 `key` 是 `zeroValue<K>()`；对 `K=String`，这会表现成空 `String` / null raw array。
7. `HashMapIterator.next()` 只看 `entries[index].hash >= 0`，不会再校验 `key/value` 是否仍然匹配。

## 当前最强推断

- 如果 `Array<HashMapEntry<String, JsonValue>>` 的 `copyTo` 在 MSGC 下走 `array_copy_struct` 时发生“部分字段复制/引用字段未正确维护”，就可能出现：
  - `hash` 已复制为有效值
  - `key` 仍保留目标槽位默认值 `zeroValue<String>()`
- 这样序列化时 iterator 会把该槽位当成 active entry 返回，最终在 `appendEscape(strKey)` 中看到“空字符串 key”。

## 对修复方向的含义

- 第一优先级：修 `MCC_AcquireRawData` 的 null / empty raw-array 判定顺序，避免坏 key 直接崩溃。
- 真正根因仍需继续回查：
  - `cj_array_copy_struct`
  - 次一级再看 `cj_gcwrite_struct`

## 2026-07-01 `MCC_AcquireRawData` 放大器修复验证

- runtime 改动：把 `MCC_AcquireRawData()` 的 `array == nullptr` 判定前移到
  `Heap::IsHeapAddress(array)` 之前，同时在入口先把 `isCopy` 置 `false`。
- 验证环境：
  - SDK：`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output`
  - STDX：`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/darwin_aarch64_cjnative`
  - 用例：`case27 c1000 p100K 60s + CASE27_BODY_DIAG=1`
  - 结果目录：
    `MSGC_RESULT/Result/case27_http_complex_json/result/20260701_234500_compiler_output_runtime_acquirerawdata_fix_c1000_p100K`

### 结果

1. 原来的 runtime-frame 崩溃链已经消失：
   - 不再出现 `MCC_AcquireRawData -> appendEscape -> JsonObject.toString`
   - 不再出现 fault addr `0x40`
2. 新暴露出来的首个症状变成：
   - `CASE27-DIAG][server-body-fail]`
   - `Parse Error: [Pos]: 99572, [Error]: Unexpected character: ','`
3. 随后进程在 managed frame 崩到：
   - `stdx.net.http.HttpServer.process`
   - fault addr `0x10`

### 含义

- `MCC_AcquireRawData` 这里确实只是放大器，不是根因制造点。
- 修掉判空顺序后，上游对象/字符串/请求体损坏仍然存在，只是现在不再被放大成 `addr=0x40`。
- 后续排查应继续围绕：
  - 请求体为什么会在 server 侧变成坏 JSON
  - `HashMapEntry<String, JsonValue>` / struct bulk copy / 写屏障路径是否仍在破坏对象状态

## 2026-07-01 预留容量实验

- 实验改动：把 `cangjie_stdx/src/stdx/encoding/json/native.cj` 中 `parseJsonObject()` 的
  `HashMap<String, JsonValue>(count)` 临时改为 `HashMap<String, JsonValue>(count + 8)`，目的是让
  `case27` 常见的 `put()` 不要立刻触发 `resize()`。
- 验证环境：
  - SDK：`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output`
  - STDX：`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/darwin_aarch64_cjnative/dynamic/stdx`
  - 用例：`case27 c1000 p100K 60s`
  - 结果目录：
    `MSGC_RESULT/Result/case27_http_complex_json/result/20260701_224750_compiler_output_stdx_hashmap_headroom_c1000_p100K`

### 结果

1. **没有修复问题。**
2. 第一轮运行里，server 先连续打印：
   - `JsonException: ... Unexpected character: ','`
   - 位置固定在 `Pos: 99572`（接近 `100K.json` 末尾）
3. 同一轮随后在 client 解析响应时崩到：
   - `stdx.encoding.json.parseJson -> Rune.fromUtf8`
4. 紧接着复跑并打开 `CASE27_BODY_DIAG=1` 后，又回到了原来的：
   - `MCC_AcquireRawData -> appendEscape -> JsonObject.toString`
   - fault addr 仍是 `0x40`

### 含义

- “只要避免 `HashMap.resize()` 就能解决 case27”的假设**不成立**。
- 这个实验最多说明：
  - `resize()` 可能是触发条件之一；
  - 但当前损坏面并不只剩这一处，或者还有别的路径会把 `String` / JSON object 状态打坏。
- 因此后续仍应优先盯：
  - `MCC_AcquireRawData` 判空顺序
  - `array_copy_struct` / `gcwrite_struct`
  - 以及 `JsonObject.toString()` 过程中 `HashMap` 迭代到坏 entry 的路径

## 2026-07-02 phase gate 诊断实验：直接去掉 non-mark-phase `StoreOnly` 判断

### 改了什么

- 目标：
  - 验证 `WriteRefFieldFast_MSGC` / `WriteStaticRefFast_MSGC` 的 callsite inline fastpath
    里，`phase < 20 || phase > 22 -> StoreOnly` 这个分支，是否会因为 phase 切换窗口
    导致旧边漏记，进而打坏 case27。
- compiler 改动：
  - 文件：
    `/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/third_party/llvm-project/llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp`
  - 做法：
    - 保留 `phase == 0 -> Slow`
    - 把原来的
      `phase < 20 -> StoreOnly`
      `phase > 22 -> StoreOnly`
      这两段 emission 整块注释掉，但**不删除原代码**
    - 也就是说，这轮实验里只要 phase 非 0，就继续尝试走 old-edge logging /
      `_wbuf` push 路径，不再因为“非 mark phase”直接 `StoreOnly`

### 怎么重编和安装

1. **增量重编 compiler**
   - 在 LLVM build 目录执行 `ninja llc LLVM`
   - 把新产物覆盖回：
     - `cangjie_compiler/output/third_party/llvm/bin/llc`
     - `cangjie_compiler/output/third_party/llvm/lib/libLLVM.dylib`
2. **重编 MSGC runtime**
   - 明确使用 `--gc-mode marksweep`
   - 再安装到 `cangjie_compiler/output`
3. **重编 std / stdx**
   - 用 `compiler/output` 这套 SDK 重新构建
   - 安装/同步到：
     - `cangjie_compiler/output`
     - `cangjie_compiler/output/darwin_aarch64_cjnative`

### 怎么验证

- 汇编核对目录：
  - `MSGC_RESULT/Result/case27_http_complex_json/compile_temps_case27_20260702_nophasegate`
- 运行结果目录：
  - `MSGC_RESULT/Result/case27_http_complex_json/result/20260702_095800_compiler_output_sdk_nophasegate_c1000_p100K`

验证分 3 步：

1. **先确认 compiler 改动真的生效**
   - 重新用 `compiler/output` SDK 编译 case27
   - 检查生成汇编，确认 inline barrier 里已经看不到
     `cmp w9, #20` / `cmp w9, #22`
   - 说明：
     - 这轮跑的不是“旧编译器 + 新 runtime”的混合结果
2. **再确认实际加载的是 MSGC runtime**
   - `runtime_load.txt` 显示实际加载的是：
     `/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib`
   - 同目录 `gc.log` 开头明确有：
     - `[MS-GC] MSRegionManager initialized`
     - `MS subsystem initialized`
   - 说明：
     - 这轮不是误用 CopyGC runtime
3. **最后跑 case27**
   - 短探针：
     - `c1 + p100K + 1s`
   - 正式压力：
     - `c1000 + p100K + 60s`
   - 使用三件套：
     - SDK：`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output`
     - runtime：同上 SDK 中安装后的 `libcangjie-runtime.dylib`
     - stdx：`/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/output/darwin_aarch64_cjnative`

### 验证结果

1. **短探针已经更早暴露损坏**
   - `c1 + p100K + 1s` 在 warmup 就出现：
     - `Parse Error: [Pos]: 99572, [Error]: Unexpected character: '\0'`
   - 这说明去掉 phase gate 后，不需要高并发长跑，单并发短探针就已经能看到
     请求体/字符串状态被打坏
2. **正式 `c1000 + p100K` 仍然崩溃**
   - 最终仍然崩在：
     - `MCC_AcquireRawData -> appendEscape -> JsonObject.toString`
   - fault addr 仍是：
     - `0x40`
3. **因此这轮实验没有修复问题**
   - “phase 切换时误走 `StoreOnly`，所以只要放开 phase gate 就能修好”
     这条解释不成立

### 对根因判断的含义

- 这轮结果更像是在说明：
  - 当前 inline fastpath 的 phase gate 不是主问题本身
  - 它反而像是现有实现上的一个保护带
- 把 non-mark-phase `StoreOnly` 彻底拿掉之后：
  - 非 mark phase 也开始往 `_wbuf` 打 old edge
  - 对象/字符串/请求体损坏会更早暴露
- 所以当前不能把 case27 主因继续收敛到：
  - “phase 切换窗口导致漏记 old edge”
