# case28 MSGC `HashMap<String, JsonValue>` 二次扩容后序列化崩溃定位记录

## 基本信息

- 日期：2026-06-21
- 场景：`MSGC_RESULT/Result/case28_http_complex_json`
- 复现命令：
  - `CASE28_PAYLOAD=1K`
  - `CASE28_CONCURRENCY=10`
  - `CASE28_DURATION_MS=60000`
  - `cjHeapGrowth=1`
  - `cjScavengeGrowth=10`
- 结果目录：
  - `MSGC_RESULT/Result/case28_http_complex_json/result/20260621_171712_msgc_vs_copygc_go_c10_p1K_compare`
- 相关源码：
  - `MSGC_RESULT/Result/case28_http_complex_json/benchmark.cj`
  - `src/CompilerCalls.cpp`
  - `stdlib/libs/std/collection/hash_map.cj`
  - `cangjie_stdx/src/stdx/encoding/json/json_object.cj`
  - `cangjie_stdx/src/stdx/encoding/json/write_buffer.cj`

## 现象

- `CopyGC` 与 `Go` 正常完成 60s。
- `MSGC` 在 `case28 c10 p1K` 下约 `18s` 左右崩溃。
- 直接报错点出现在：
  - `MCC_AcquireRawData`
  - `stdx.encoding.json.WriteBuffer.appendEscape`
  - `JsonObject.toString`
  - `benchmark.cj:653`

`stdout.txt` 中的关键栈：

```text
SIGSEGV addr=0x40
#0 MCC_AcquireRawData
#2 stdx.encoding.json::WriteBuffer.appendEscape
#4 stdx.encoding.json::JsonObject.toString
#5 processRequest ... benchmark.cj:653
```

后续还有：

```text
TryAllocateRegularObjectOnce FAILED
libc++abi: terminating due to uncaught exception of type std::system_error: mutex lock failed: Invalid argument
```

这部分是首个 `SIGSEGV` 之后的次生噪音，不是 primary root cause。

## 第一层结论：不是 `MCC_AcquireRawData` 主动坏掉，而是上游传了 null raw array

`src/CompilerCalls.cpp` 中：

```cpp
extern "C" void* MCC_AcquireRawData(const ArrayRef array, bool* isCopy)
{
    if (!Heap::IsHeapAddress(array)) {
        return array->ConvertToCArray();
    }
    ...
    if (UNLIKELY(array == nullptr)) {
        return nullptr;
    }
```

这里在判空前先走了 `!Heap::IsHeapAddress(array)` 分支。

因此：

- 如果 `array == nullptr`
- `Heap::IsHeapAddress(nullptr)` 为 false
- 会直接执行 `array->ConvertToCArray()`
- 于是得到 `addr=0x40` 这类 null-base 偏移崩溃

所以这次 `SIGSEGV` 的直接含义是：

- `appendEscape(str)` 里拿到的 `str.rawData()` 已经是 null
- 也就是某个 `String` 的底层 `myData` 已经坏了

## 第二层结论：坏的是 `JsonObject` 里的 key，不是 retain store 里的 body

`cangjie_stdx/src/stdx/encoding/json/write_buffer.cj`：

```cj
for ((strKey, jv) in jo.getFields()) {
    buff.append(b'\"').appendEscape(strKey).append("\":".toArray())
    jsonWriteWithoutFormat(jv, buff, depth + 1, htmlSafe: htmlSafe)
}
```

崩溃点在 object key 的 `appendEscape(strKey)`，不是 value string。

`benchmark.cj` 里对应第二次序列化前后：

```cj
let serializedBody = root.toString()
...
root.put("read_pass_count", ...)
...
root.put("retained_limit_bytes", ...)
root.toString()
```

也就是：

- 第一次 `root.toString()` 成功
- 之后做一批 `root.put(...)`
- 第二次 `root.toString()` 在遍历 `root.getFields()` 的 key 时崩溃

## 最小探针验证

### 探针 A：去掉第二批 `root.put(...)`，保留 retain store

做法：

- 保留：
  - `collectReadHeavySummary(root, READ_PASS_COUNT)`
  - `RETAINED_RESPONSES.maybeRetain(seq + 1, serializedBody)`
  - 第二次 `root.toString()`
- 去掉：
  - `root.put("read_*"... )`
  - `root.put("retained_*"... )`

结果：

- `c10 p1K 30s`
- 正常跑完
- 无 `MCC_AcquireRawData` / `SIGSEGV`

结论：

- 问题不在 `retainedBodies.add(body)` 本身
- 也不在“第二次 `toString()`”这个动作本身
- 问题被收敛到第二批 `root.put(...)` 路径

### 探针 B：第二次只 `put` 一个字段

做法：

- 第二批只保留：
  - `root.put("read_pass_count", JsonInt(READ_PASS_COUNT))`
- 其余 `root.put(...)` 全部移除

结果：

- `c10 p1K 30s`
- 正常跑完
- 无崩溃

结论：

- 单个 post-serialize `put` 不足以触发问题
- 原始 case28 崩溃需要“成批追加字段”

## 根因收敛：问题集中在 `HashMap<String, JsonValue>` 的 post-serialize 扩容/struct bulk write

### 1. root 的初始容量与 case28 的写入模式

`native.cj::parseJsonObject()` 会按对象字段数建：

```cj
let map = HashMap<String, JsonValue>(count)
```

`1K` payload 顶层 key 数实测是 `7`：

```text
config, counters, events, ingestion_time, pipeline, request_id, schema_version
```

case28 在第一次 `toString()` 之前已经写入：

- `processed_seq`
- `server_ok`

因此 root size 从 `7 -> 9`。

第二批再写入 `13` 个字段后，root size 会从 `9 -> 22`。

这意味着第二批写入期间会跨过多个 `entries.size` 阈值，触发 `HashMap.resize()`。

### 2. `HashMap.resize()` 会对 `Array<HashMapEntry<K, V>>` 做 struct bulk copy

`stdlib/libs/std/collection/hash_map.cj`：

```cj
struct HashMapEntry<K, V> {
    let hash: Int64
    let next: Int64
    let key: K
    let value: V
}
```

`resize()` 中：

```cj
let newEntries = Array<HashMapEntry<K, V>>(...)
entries.copyTo(newEntries, 0, 0, oldEntriesSize)
entries = newEntries
```

而插入时也会做：

```cj
entries[entryIndex] = HashMapEntry<K, V>(hash, buckets[bucketIndex], key, value)
```

对 `HashMap<String, JsonValue>` 来说，这两类操作分别对应：

- `entries[entryIndex] = ...`
  - struct field write
  - 对应 `gcwrite_struct`
- `entries.copyTo(newEntries, ...)`
  - struct array bulk copy
  - 对应 `array_copy_struct`

### 3. 为什么当前更怀疑 `array_copy_struct`

当前证据更偏向 `array_copy_struct`，原因有 3 点：

1. 单个 post-serialize `put` 的探针稳定
   - 说明仅靠一次 `entries[entryIndex] = HashMapEntry(...)` 不足以复现
2. 原始 case28 只有“成批追加字段”才崩
   - 这正好会把 root `HashMap` 推过多个扩容点
3. `case27` 也会做少量 `root.put(...)`，但没有这次崩溃
   - case27 没有这批 `9 -> 22` 的二次扩容压力

因此本次问题最像：

- `Array<HashMapEntry<String, JsonValue>>` 在 MSGC 并发期走了 `array_copy_struct` fastpath
- bulk copy 后某些 `HashMapEntry.key` / `HashMapEntry.value` 的 ref 字段没有被正确维护
- 之后第二次 `JsonObject.toString()` 遍历到损坏 entry
- `strKey.rawData()` 变成 null
- 最终在 `MCC_AcquireRawData` 的空指针窗口里炸成 `addr=0x40`

## 与 Step26 的对应关系

`MSGC_RESULT/Implementation/Step26_MSGC写屏障Lowering与发射层分流设计.md` 已记录：

- `gcwrite_struct` fastpath 已开启
- `array_copy_struct` fastpath 已开启
- 两者都属于 bulk struct barrier 路径

case28 这次崩溃刚好命中了：

- `JsonObject.fields: HashMap<String, JsonValue>`
- `HashMapEntry<String, JsonValue>`
- `entries.copyTo(newEntries, ...)`

这是一个非常典型的 `array_copy_struct` 压测点。

## 最终结论

### primary root cause

case28 的第二批 `root.put("read_*"/"retained_*")` 会在 `JsonObject.fields` 的 `HashMap<String, JsonValue>` 上触发多次 post-serialize 扩容；该扩容会走 `Array<HashMapEntry<String, JsonValue>>` 的 struct bulk copy。当前 MSGC 下，`array_copy_struct`（次一级是 `gcwrite_struct`）在这个引用型 struct 场景里存在缺陷，导致 `HashMapEntry.key/value` 破坏；第二次 `root.toString()` 遍历 key 时读到坏 `String`，最终在 `MCC_AcquireRawData` 里因 null raw array 崩溃。

### direct crash site

`MCC_AcquireRawData()` 的空指针判定顺序不安全，把“上游传了 null rawData”放大成了 runtime frame 的 `SIGSEGV addr=0x40`。

### not root cause

- `TryAllocateRegularObjectOnce FAILED`
- `mutex lock failed: Invalid argument`
- `Handle unexpected signal action`

这些都是首个 `SIGSEGV` 之后的连锁噪音。

## 后续建议

1. 优先回查 / 关闭 `array_copy_struct` fastpath，验证 case28 是否恢复稳定
2. 若仅关闭 `array_copy_struct` 仍复现，再继续看 `gcwrite_struct`
3. 独立补一条 runtime hardening：
   - `MCC_AcquireRawData()` 先判 `array == nullptr`
   - 再走 `Heap::IsHeapAddress(array)`
   - 避免把上游坏数据放大成低可读性的 `addr=0x40`
