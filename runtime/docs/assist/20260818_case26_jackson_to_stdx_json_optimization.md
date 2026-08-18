# Case26：Jackson 可借鉴的 stdx JSON 优化

## 1. 范围与基线

本文针对 `case26_http_complex_json` 的 DOM JSON 对比，分析 Java Jackson 2.18.0 的实现，整理可以落地到 MSGC stdx JSON 的优化点。

- case26 继续保持 DOM 语义：请求字节解析为 DOM，修改 DOM，再输出响应。
- case25 是独立的流式对比，使用 `JsonReader`/`JsonWriter`，不在本文中把 case26 改造成流式实现。
- Java 基线为 `ObjectMapper.readTree(byte[])`、`ObjectNode`/`ArrayNode` 修改和 `writeValueAsBytes(root)`。

参考文件：

- `MSGC_RESULT/Result/case26_http_complex_json/result/20260818_512M_c1000_100k_HeapGrow1_Scavenge2_Procs10_compare/java/Case27Benchmark.java.snapshot`
- `src/stdx/encoding/json/json_value.cj`
- `src/stdx/encoding/json/native.cj`
- `src/stdx/encoding/json/write_buffer.cj`

## 2. Jackson 的关键实现

Java case26 的主要路径是：

```text
HTTP body byte[]
  -> ObjectMapper.readTree(byte[])
  -> UTF-8 parser
  -> JsonNode DOM
  -> ObjectNode/ArrayNode 修改
  -> ObjectNode.serialize(JsonGenerator)
  -> UTF8JsonGenerator/ByteArrayBuilder
  -> response byte[]
```

Jackson 中值得借鉴的机制：

1. **字段名 canonicalization**：`ByteQuadsCanonicalizer` 对 UTF-8 字段名做哈希和复用，重复字段名不必反复创建等价的字符串对象。
2. **缓冲区复用**：`BufferRecycler`、`ByteArrayBuilder` 和 `TextBuffer` 减少解析、转义和输出过程中的临时数组分配。
3. **DOM 直接输出**：`ObjectNode.serialize(JsonGenerator)` 直接向 JSON generator 写出，不要求先构造中间文本对象。
4. **类型化节点 API**：`ObjectNode.put/add` 直接创建对应类型节点，避免通用 `Object` 转换路径。
5. **标量单例和小整数缓存**：`BooleanNode`、`NullNode` 使用单例；不可变的 `IntNode` 对小整数有缓存。
6. **原生数字路径**：UTF-8 parser 直接解析整数和浮点数，generator 直接写出数字，避免数字先转字符串再输出。

## 3. stdx 当前已经具备的能力

当前 stdx 已经覆盖了 Jackson 思路中的一部分：

- `JsonValue.fromOwnedUtf8Bytes`：已知输入生命周期时转移 UTF-8 buffer 所有权，避免一次完整复制。
- `JsonValue.fromUtf8Bytes`：保留安全复制入口，适用于调用方仍会修改输入的场景。
- `JsonValue.toStringWithCapacity`：按预估容量创建输出 buffer，减少扩容。
- `JsonFloat` 原始 token 保存和 Ryu 输出路径：减少不必要的浮点格式化开销。
- 原生整数/浮点解析、短字符串快速路径和 UTF-8 校验融合。
- `JsonNull`/`JsonBool` 共享节点。
- `JsonObject` 的小对象 flat storage，字段较多时再提升到 `HashMap`。
- `JsonInt.setValue`、`JsonFloat.setValue`、`JsonString.setValue`，可以原地更新已有节点。
- case25 已有 `JsonReader`/`JsonWriter` 的 raw copy API；该能力属于流式用例，不改变 case26 的 DOM 语义。

## 4. 建议落地的优化

### 已验证且不采用：解析器字段名 canonicalization

case26 的 `100K.json` 虽然有 6,055 次字段名出现、其中 5,968 次重复（98.6%），但这项优化的前提不适用于当前 stdx 路径：未转义字段名由 `parseJsonObjectKey` 通过 `String.withRawData(parser.data[start..offset])` 构造。`withRawData` 不复制底层字节，数组范围切片也引用原数组，因此原路径并不存在 Jackson `ByteQuadsCanonicalizer` 所针对的“重复复制字段名字节”成本。

曾实现 parser-local 的有界缓存：扫描时计算 hash，按 hash、长度和逐字节比较命中，缓存首个字段名的 `String` slice；第一版采用 `HashMap + ArrayList`，第二版改为固定 257 桶和 128 条目以降低容器分配。两版均已回退，不能合入。

每个字段名在该缓存中新增了 hash 计算、桶查找和命中后的逐字节比较；同时每个解析器仍需分配缓存表和条目。它没有消除字段名的字节复制，反而增加了热路径 CPU 工作。跨请求全局 intern 更不可取：`fromOwnedUtf8Bytes` 的字段名 slice 指向请求 body，会让缓存长期持有旧请求数据；若复制键以解除该生命周期约束，成本会更高。

#### 实测结果（case26 MSGC-only）

统一配置：`payload=100K`、`concurrency=1000`、`duration=60s`、`heap=20gb`、`PROCS=10`、`cjHeapGrowth=1`、`cjScavengeGrowth=2`、`cjMSMinGCThreshold=20mb`。

| 版本 | 吞吐 (req/s) | 成功 / 失败 | 结论 |
| --- | ---: | --- | --- |
| 无缓存历史基线 | 1391 | 84811 / 0 | 基线 |
| 缓存首版（`HashMap + ArrayList`） | 1296 | 78551 / 0 | 明显回退 |
| 缓存简化版（固定桶/固定条目） | 1331 | 82022 / 0 | 仍无收益 |
| 回退缓存后重新构建安装 | 1388 | 84515 / 0 | 与基线差 0.2%，属测试波动 |

最后一项已重新构建 stdx 并同步到 MSGC SDK，target 与 SDK 的 `libstdx.encoding.json.so` 哈希一致。60 秒单次结果不能精确定义 0.2% 差异，但足以确认回退恢复了缓存版约 4.3% 的吞吐损失，且该方案没有正向证据。

**结论：不要再将字段名 canonicalization / parser-local field-name cache 列为 case26 或当前 stdx DOM JSON 的优化项。** 只有在字段名解析路径改为实际复制 UTF-8 内容、且 profile 证实该复制是主要分配来源时，才重新评估；重新评估前必须以同一构建做至少三轮 A/B/A 对照。

### P0：增加 DOM 直接 UTF-8 输出接口

当前 case26 MSGC 路径仍然是：

```text
DOM -> toStringWithCapacity -> String -> HttpResponseBuilder.body(String)
```

建议在 `JsonValue` 增加以下接口之一：

```text
writeTo(output: OutputStream, capacity: Int)
```

或：

```text
toUtf8BytesWithCapacity(capacity: Int): Array<Byte>
```

实现上复用现有 `WriteBuffer` 和 DOM writer，直接生成 UTF-8 bytes。这样仍然是 DOM 解析和 DOM 修改，只是去掉最终的 String 中间层，更接近 Jackson 的 `writeValueAsBytes`。

需要注意：当前 `HttpResponseBuilder.body(String)` 已经可以直接使用 String 底层只读 bytes，不应再写成 `String.toArray()` 或 `ByteBuffer(respStr.toArray())`，否则会重新复制整个响应。新接口只有在 HTTP 层能够直接接收并持有该 byte buffer 时才有收益。

### P1：解析和输出 buffer 回收

当前 `WriteBuffer.getAndClear()` 会返回有效数据后重新分配空数组，原 backing array 不再复用。建议：

- 将 buffer 的 backing array 和逻辑长度分离；
- 增加 bounded worker-local buffer pool，按容量档位回收；
- 复用 `JsonParser` 的输入状态和临时缓存，但每次请求都重置 offset、depth 和长度；
- 大于上限的数组不放回池中，避免单个大请求长期占用内存。

回收接口必须清晰区分“数组已经交给 HTTP 层持有”和“数组仍可复用”两种状态，避免响应发送完成前复用同一 backing array。

### P1：case26 修改阶段使用原地 setter

case26 当前仍有创建新标量节点的写法，例如：

```text
co.put("in", JsonInt(...))
item.put("seq", JsonInt(...))
ao.put("credits", JsonFloat(...))
```

如果字段已经存在，应优先取得已有节点并调用 `setValue`，减少 `JsonInt`/`JsonFloat` 对象分配和对象表更新。该优化只影响 benchmark 用例的修改方式，不改变 DOM 结果。

## 5. 不建议直接照搬 Jackson 的部分

- **不要直接复制 Jackson 的小整数缓存**：Jackson 的 `IntNode` 是不可变对象；stdx 的 `JsonInt` 可通过 `setValue` 修改，共享后会发生数据串改。
- **不要用无界全局 String intern**：请求字段和值的基数可能很高，会导致缓存长期持有内存。字段名缓存应限制在 parser 或 worker 生命周期内。
- **不要为当前 DOM 字段名增加 parser-local canonicalization**：已在 case26 验证为无收益且会回退吞吐，详见上一节。
- **不要因为 Jackson 使用 map 就移除 flat storage**：case26 中大量对象字段数较少，stdx 当前 flat storage 对小对象更合适。
- **不要把 case26 改成 JsonReader/JsonWriter 流式处理**：这会改变对比模型；流式优化应继续放在 case25。

## 6. 推荐实施顺序

1. 增加 DOM 直接 UTF-8 输出，并让 HTTP 层明确接管输出 buffer，避免 `String.toArray()` 复制。
2. 增加 `WriteBuffer`/parser 的 worker-local 回收，分别测量分配速率、GC 时间和 RSS。
3. 修改 case26 benchmark 的字段更新方式，优先使用 `setValue`。
4. 每一步记录吞吐、成功数、平均 CPU、GC 次数/暂停和响应校验结果，确保性能提升没有改变 DOM 语义。

当前最值得优先验证的是 DOM 直接 UTF-8 输出，其次是 `WriteBuffer` 回收。这两项可减少仍然真实存在的输出 buffer 分配和 String/bytes 往返，同时保持 case26 与 Java 的 DOM 对比公平。
