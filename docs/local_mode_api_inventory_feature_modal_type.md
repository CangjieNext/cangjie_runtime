# Cangjie `std` local-mode API 清单（相对 CJEEP-005）

- 基准：`CJEEP-005《标准库适配 local mode 策略问题闭环》`（快照 `doc-latest-20261015.md`，md5 `d35cb1a03118cd8d33079451010c2cd1`）。
- 适用分支：`feature/modal-type`。适用类型（§2）：`Array`、`ArrayList`、`String`、`HashMap`。
- 判定规则（用户裁定）：**保留某 local 成员的唯一理由是"文档要求"或"用户确认要实现"**；"被其它包（含 stdx/fs）消费"**不构成**保留理由。

## 0. 结论

| 项 | 数 |
|---|---|
| 文档要求但未实现 | **0** |
| 文档要求的 public local 成员 | 143（见 §1） |
| 文档外（"多实现"）的 public local 成员 | 25（见 §2；其中 18 项为接口义务覆写，7 项为支撑/编译强制） |
| 文档外、已删除 | 22（见 §3） |

---

## 1. 文档要求的 public local 成员（全部在位）

### 1.1 `Array`（36，`core/array.cj`、`core/array_common.cj`）
`init(@local!)`（§3.4.1 构造族另有 `init(@local!, size, repeat!)`、`init(@local!, size, initElement)`、`init(@local?, elements)`、`init(@local?, size, initElement)`）；
`[](@local!, index)`、`[](@local?, index)`、`[](@local!, range)`、`[](@local?, range)`、`[](@local!, index, value!)`、`[](@local?, range, value!)`；
`get(@local!)`、`get(@local?)`、`slice(@local!)`、`slice(@local?)`、`clone(@local!)`、`clone(@local!, range)`、
`copyTo(@local!)`、`copyTo(@local?)`、`concat(@local!)`、`splitAt(@local!)`、`repeat(@local!)`、
`fill(@local!)`、`fill(@local?)`、`swap(@local!)`、`swap(@local?)`、`reverse(@local!)`、`reverse(@local?)`、
`map(@local!)`、`flatMap(@local!)`、`filterMap(@local!)`、`step(@local!)`、`take(@local!)`、`skip(@local!)`、
`intersperse(@local!)`、`forEach(@local!)`、`all(@local?)`、`any(@local?)`、`none(@local?)`、`fold(@local!)`、
`indexOf(@local?)`×3、`lastIndexOf(@local?)`×4、`isEmpty(@local?)`、`first(@local!/?)`、`last(@local!/?)`。

### 1.2 `String`（40，`core/string.cj`）
`[](@local!, index)`、`[](@local!, range)`、`[](@local?, range)`、`clone(@local!)`、`toArray(@local!)`、`toRuneArray(@local!)`、
`iterator(@local!/?)`、`runes(@local!/?)`、`lines(@local!/?)`、`size @local?`、`isEmpty(@local?)`、`isAscii(@local?)`、
`isAsciiBlank(@local?)`、`get(@local?)`、`hashCode(@local?)`、`fromUtf8(@local!)`、`fromUtf8(@local?)`、`fromUtf8Unchecked(@local?)`、
`rawData(@local!)`、`split(@local!)`×2、`replace(@local!)`、`toAsciiLower/Upper/Title(@local!)`、`trimAscii/@local!`、`trimAsciiStart`、`trimAsciiEnd`、
`removePrefix(@local!)`、`removeSuffix(@local!)`、`contains(@local?)`、`startsWith(@local?)`、`endsWith(@local?)`、
`padStart(@local!)`、`padEnd(@local!)`、`equalsIgnoreAsciiCase(@local?)`、`compare(@local?)`、
`+(@local!)`、`*(@local!)`、`==/@local!`、`==/@local?`、`!=@local!`、`</<=/>/>= @local?`、`indexOf(@local?)`×2、
`join(strArray: Array<String> @local!, delimiter!: String @local?)`、`toString(@local?)`。

### 1.3 `ArrayList`（37，`collection/array_list.cj`）
`init(@local!)`×2、`init(@local!, size, initElement)`、`init(@local!, elements: Array<T> @local!)`、`init(@local!, elements: Collection<T> @local!)`、`init(@local?, elements: Array<T> @local?)`、`init(@local?, elements: Collection<T> @local?)`、`init(@local?, size, initElement)`；
`get(@local!/?)`、`[](@local!/?)`、`[](index,value! @local!)`、`[](range @local!/?)`、`slice(@local!/?)`、`clone(@local!/?)`、`toArray(@local!/?)`、
`size @local?`、`capacity @local?`、`first(@local!/?)`、`last(@local!/?)`、`isEmpty(@local?)`、`iterator(@local!/?)`、
`add(@local!)`×4、`remove(@local!)`×2、`removeIf(@local!)`、`clear(@local!)`、`reverse(@local!)`、
`step/take/skip/filter/map/flatMap/filterMap/intersperse/zip/enumerate(@local!)`、`fold(@local!)`、`reduce(@local!)`、
`all/any/none(@local?)`。

### 1.4 `HashMap`（30，`collection/hash_map.cj` + `collection/map_interface.cj`）
`init(@local!)`×2（构造族）、`get(@local!/?)`、`[](@local!/?)`、`[](@local!, key, value!)`、`contains(@local?)`×2、
`keys(@local!/?)`、`values(@local!/?)`、`entryView(@local!/?)`、`add(@local!)`×2、`remove(@local!)`×2、`removeIf(@local!)`、`clear(@local!)`、`reserve(@local!)`、
`addIfAbsent(@local!)`、`replace(@local!)`（doc §3.4.4 于 `interface Map` 声明，类经接口继承；见 §2.3）、
`size @local?`、`capacity @local?`、`isEmpty(@local?)`、`iterator(@local!/?)`、`toArray(@local!)`、
`filter(@local!)`、`fold(@local!)`、`mapValues(@local!)`、`reduce(@local!)`、`all/any/none(@local?)`、`forEach(@local?)`、`toString(@local?)`。

---

## 2. 文档外的 public local 成员（"多实现"）

### 2.1 接口义务（18 项，非真多余）

文档以"经接口继承的成员沿用 `Collection` 既有适配／不重复列出"豁免（§3.4.4 原文），故这些属**协议实现**：

| 类型 | 成员 | 接口 |
|---|---|---|
| `Array` | `iterator(@local!/?)`、`toArray(@local!)`、`size @local?`、`isEmpty(@local?)`、`next(@local!/?)`(ArrayIterator) | `Iterable`/`Iterator`/`Collection` |
| `Array` | `toString(@local?)` | `ToString`（§3.3） |
| `String` | `hashCode(@local!)`、`toArray(@local?)`、`next(@local!/?)`(String*Iterator) | `Hashable`/`Collection`/`Iterator` |
| `ArrayList` | `next(@local!/?)`(ArrayListIterator) | `Iterator` |
| `HashMap` | `next(@local!/?)`、`toArray(@local?)` | `Iterator`/`Collection<(K,V)>` |

上述接口成员本身也不在 §3.4.1–3.4.4 逐条列出，但 §3.3 明文定义 `ToString.toString(@local?)`，§3.4.4 说明派生规则。

### 2.2 域内 doc-forced 支撑（编译仲裁确认删不掉）

| 成员 | 位置 | 强制来源（域内 doc-required） |
|---|---|---|
| `Array.clone(this @local?)` | `array.cj` | `ArrayList.toArray(@local?)` / `ArrayList.clone(@local?)` 内 `myData.clone()`；删除 → `error: no matching function declaration for function call 'clone'` |
| `String.rawData(this @local?)` | `string.cj` | `String.iterator/runes/lines(@local?)` 体内 `rawData()`（接收者 `@local?` 绑定本重载） |
| `acquireArrayRawData(@local?)` / `releaseArrayRawData(@local?)` | `array_common.cj` | `String.split(@local!)` 的 `StringLazySplitIterator.init`（`string_common.cj:565-574`） |

### 2.3 构造类（不计入"多实现"）

`Array`/`ArrayList`/`HashMap` 的 `init(@local!, …)`/`init(@local?, …)` 属文档构造族（§3.2/§3.4.x），非多余；`HashMap.addIfAbsent/replace(@local!)` 由 §3.4.4 `interface Map` 声明，类继承即满足。

---

## 3. 已删除（相对初始 `feature/modal-type` 或文档外成员）

### 3.1 文档明示删除
- `String.indexOf(this @local!, …)`×2（§3.4.2 明写 `@local!` 版已删除）
- `String.toHeap(this @local?)`
- `Array.splitAt(this @local?)`（仅 `@local!` 版为文档内）

### 3.2 "其它类型不做适配"（§2）口径删除
- `convert/parsable.cj`：`Parsable` 接口 `parse/tryParse(String @local?)` + 12 原语 conformer（**全清，0 @local**）；stdx 侧 `native.cj:1055` 改 `Int64.parse(String(numStr))`
- `core/string_builder.cj` 的 **public** local 面（`append`×2）删除；所需 helper 降为 `internal`（见 §4）
- `core/c_string_resource.cj` 的 `createStrLocalFromCPointer` 降为包内（见 §4）
- `core/char_extension.cj`：`Rune` 的 7 个 local 比较操作（`compare`/`==`/`!=`/`<`/`>`/`<=`/`>=` `(@local?,…)`）删除
- `core/box.cj`、`core/duration.cj`、`core/print.cj`、`fs/common.cj`、`fs/file_info.cj`、`io/byte_buffer.cj`、`io/io_utils.cj`、`io/stream.cj` 的 local 面 → 整文件回退至未适配基线
- `fs/file.cj`、`fs/path.cj`、`fs/utils.cj`、`core/libc.cj` 的 local 面 → 外科式剥离（保留非 local 改动）
- `collection/hash_set.cj`、`collection/tree_map.cj`（除 EntryView 接口义务）、`collection/concurrent/concurrent_hash_map.cj`（除 EntryView 义务）的 local 面

### 3.3 零消费者删除
- `Array.cloneToNotLocal(this @local?)`（原 `fs/path.cj` 消费者已随 fs 去 local 消失）
- `String.fromUtf8UncheckedLocal(this @local?)`（原 `fs/utils.cj` 消费者同上）
- `Array.contains(this @local?, element)`
- `Array.zip<R>(this @local!)`（文档仅列 `ArrayList.zip`）
- `Array.enumerate(@local!)`、`Array.flatten(@local!)`、`Array.reduce(@local!)`、`Array.toRawArray(@local!)`
- `String.toHeap(@local?)`
- `ArrayList.forEach(@local!)`、`ArrayList` 的 Copyable `@local?` 变体（`add`×4、`remove`×2、`removeIf`、`clear`、`reserve`、`reverse`）
- `HashMap.clone(@local?)`、`HashMap.forEach(@local!)`
- 孤儿 helper `localGrowUnsupportedMessage`

---

## 4. 文档外但**域内 doc-forced**、以最小可见性保留

| 成员 | 位置 | 可见性 | 强制来源 |
|---|---|---|---|
| `Rune.fromUtf8(Array<UInt8> @local?, index)` | `char_extension.cj:173` | 包内 | `StringRuneIterator.next(@local!/?)`（`string_common.cj:337/347`） |
| `Rune.intoUtf8Array(Rune, Array<UInt8> @local?, index)` | `char_extension.cj:251` | 包内 | `String.init(@local!, Array<Rune>/Collection<Rune> @local?)`（`string.cj:75/104/114`） |
| `Rune.fromUtf8(RawArray<UInt8> @local?, …)` | `char_extension.cj:180` | 包内 | `String.toRuneArray/split/replace(@local!)` |
| `checkIndexValid(…@local?)` | `char_extension.cj:280` | private | 上述 `intoUtf8Array` helper |
| `StringBuilder.init(@local!)` 及 `toStringLocal/growLocal/appendRune/appendString/appendValue(@local!)` | `string_builder.cj:48/131/174/189/204/218` | **internal** | `Array.toString(@local?)`（`array_common.cj:141`） |
| `numberToString(…, Array<Byte> @local!)` | `string_builder.cj:503` | 包内 | `Int64/UInt64.toString(@local?)`（`to_string.cj:44/59/105/111`） |
| `createStrLocalFromCPointer` | `c_string_resource.cj:32` | 包内 | `Float64/Float32.toString(@local?)`（`to_string.cj:168`） |
| `RunesToUtf8Bytes.getBytesLocal/getBytesLocalCollection/getSize(@local?)` | `string.cj:2870/2903/2939` | 包内（非 public 类） | 两个 doc-required `String` local 构造器 |

## 5. 接口层（`std.core`/`std.collection`）local 成员

文档 §3.3 与 §3.4.4 明确要求，或为其派生义务：

- `Iterable`: `iterator(@local!)`、`iterator(@local?)`
- `Iterator`: `next(@local!)`、`next(@local?)`
- `Collection`: `isEmpty(@local?)`、`toArray(@local?)`、`toArray(@local!)`
- `ToString`: `toString(@local?)`（§3.3）
- `Hashable`: `hashCode(@local?)`、`hashCode(@local!)`
- `Equatable`: `==`/`!=` `(@local!/?)`
- `Comparable`: `compare`/`==`/`<`/`>`/`<=`/`>=` `(@local?)`
- `MapEntryView`: `prop key: K @local?`、`mut prop value: ?V @local!`（抽象；conformer `TreeMapEntryView`/`ConcurrentHashMapEntryView` 必须实现）
- `ReadOnlyMap`/`Map`/`EquatableCollection`/`OrderedMap`：doc §3.4.4 全族
- `List`（`list_interface.cj`）：doc §3.4.3 全族

---

## 6. 已知遗留缺陷（未修）

1. **`HashMap @local!` GC 崩溃**（高，运行时）：构造后经历 GC → `heap object must not reference (non-)local object` → SIGABRT。复现于 2026-10-14 旧 SDK，非本轮引入。域内类型，需单独修复。
2. **编译器 ICE**：`String @local!` 字符串插值 `${x}` → `Internal Compiler Error: null pointer`（CJNative 0.0.1）。
3. 编译器限制：`@local?` 接收者不可重绑 non-Copyable 成员（`ArrayList.add(@local!)` 满容量抛 `UnimplementedException`，为既定契约）。
