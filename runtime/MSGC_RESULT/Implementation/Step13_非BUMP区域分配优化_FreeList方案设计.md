# Step13 非 BUMP 区域分配优化：Free-list 方案设计

> 本文档记录对 MSGC 中 PARTIAL 类型 region（非 BUMP、经 sweep 后的碎片 region）
> 分配路径的优化探索过程，包含现状分析、候选方案实验、以及 Free-list 方案的
> 详细设计与正确性论证。

---

## 1. 背景：非 BUMP region 的分配路径

MSGC 中 region 分为两类分配路径：

| 类型 | 分配方式 | 文件 |
|------|---------|------|
| BUMP region | `AllocBump()`：`allocPtr += slotSize`，极简 | `MSRegionInfo.h` |
| PARTIAL region（经 sweep 后）| `AllocFromBitmapFast()`：两级 bitmap 查找 | `MSRegionInfo.h` |

`AllocFromBitmapFast()` 使用 summary word（`reserved` 字段）做 word 级跳跃，热路径如下：

```cpp
inline uintptr_t AllocFromBitmapFast() {
    uint64_t summary = reserved;  // bit[i]=1 表示 allocBitmap[i] 有空 slot
    while (summary != 0) {
        size_t wordIdx = __builtin_ctzll(summary);
        uint64_t word = allocBitmap[wordIdx];
        if (LIKELY(word != 0)) {
            int bit = __builtin_ctzll(word);
            allocBitmap[wordIdx] = word & ~(1ULL << bit);  // 清除该 bit
            freeSlotCount--;
            if (allocBitmap[wordIdx] == 0) {
                reserved &= ~(1ULL << wordIdx);  // 更新 summary
            }
            return RegionStart() + (wordIdx * 64 + bit) * slotSize;
        }
        // summary 过期，自我修正
        summary &= ~(1ULL << wordIdx);
        reserved &= ~(1ULL << wordIdx);
    }
    return 0;
}
```

每次分配的开销：
- 2 次 `ctzll`（summary + allocBitmap word）
- 1 次 allocBitmap bit 清除（写外部 bitmap 数组）
- 条件性写 `reserved`（word 耗尽时）

---

## 2. 候选优化方案实验记录

### 2.1 Slot 级游标方案（已实验，已回退）

**方案**：在 `reserved` 字段存储 allocCursor（slot 级位置），每次从 cursor 开始
向后用 `ctzll` 扫描，不清除 bitmap bit，sweep 时计算 `firstFreeSlot` 作为初始游标。

**测试结果**（frag_alloc 碎片堆场景，Run 2-4 稳态）：

| 版本 | Baseline | Phase2 |
|------|---------|--------|
| 旧版（summary bitmap） | 59–68 ms | 65–77 ms |
| cursor 方案 | 74–79 ms | 101–148 ms（高抖动） |

**回退原因**：Phase2 慢 30–100%。根因是每次分配多了移位 + 取模 + 条件 mask，
而 summary bitmap 的 `ctzll(reserved)` 本质上已是 word 级游标，
省写 bitmap bit 带来的收益被额外计算开销抵消。

**代码状态**：通过 `git stash` 回退，旧版 runtime 已重新部署。

### 2.2 Word-level cache 方案（AllocBuffer 层）

**思路**：在 `AllocBufferMS` 缓存 `{wordPtr, remainingMask}`，
消费完整个 word 才写回一次 `allocBitmap`，64 次分配只有 1 次外部写。
同时保留 summary 的 O(1) 跳跃。需改 `MSAllocBuffer`，不动 `MSRegionInfo`。

**状态**：待实现。

---

## 3. Free-list 方案设计

### 3.1 核心思路

参考 Go runtime 的 `mspan.freeindex + allocBits` 设计思路，在 `SweepRegion()` 阶段
将死亡 slot 串成单向链表（intrusive free-list），alloc 时直接从链表头弹出，
消除 `ctzll` 和 bitmap bit-clear 操作。

### 3.2 Sweep 阶段：构建链表

```cpp
// SweepRegion() 新实现
size_t MSRegionInfo::SweepRegion() {
    uintptr_t base = RegionStart();
    size_t slotSize = SizeClassInfo::ClassToSize(sizeClass);
    size_t wordCount = (totalSlotCount + 63) / 64;

    uintptr_t freeHead = 0;  // freelist 头（0 表示空）
    int liveCount = 0;

    for (size_t i = 0; i < wordCount; i++) {
        liveCount += __builtin_popcountll(markBitmap[i]);
        uint64_t freeBits = ~markBitmap[i];

        // 处理尾 word 的无效位
        if (i == wordCount - 1) {
            size_t trailing = totalSlotCount % 64;
            if (trailing != 0) freeBits &= (1ULL << trailing) - 1;
        }

        while (freeBits != 0) {
            int bit = __builtin_ctzll(freeBits);
            uintptr_t slotAddr = base + (i * 64 + bit) * slotSize;

            // 先清零（与现方案等价），再写 next ptr（覆盖首 8 字节）
            memset((void*)slotAddr, 0, slotSize);
            *(uintptr_t*)slotAddr = freeHead;
            freeHead = slotAddr;

            freeBits &= freeBits - 1;
        }
    }

    liveSlotCount = (uint16_t)liveCount;
    freeSlotCount = totalSlotCount - liveSlotCount;
    sweepState = MSSweepState::SWEPT;

    // 用 reserved 字段存 freelist 头（region 内绝对地址）
    reserved = freeHead;

    return (size_t)liveSlotCount * slotSize;
}
```

### 3.3 Alloc 阶段：弹出链表头

```cpp
inline uintptr_t AllocFromFreelistFast() {
    uintptr_t head = reserved;   // freeHead
    if (UNLIKELY(head == 0)) return 0;

    reserved = *(uintptr_t*)head;  // 读 next ptr
    *(uintptr_t*)head = 0;         // 清 next ptr（还原首 8 字节为零）
    freeSlotCount--;
    return head;
}
```

每次分配的开销：
- 1 次 load（读 `reserved` = freeHead）
- 1 次 load（读 next ptr）
- 1 次 store（清 next ptr，8B）
- 1 次 store（更新 `reserved`）
- **无 ctzll，无外部 bitmap 访问**

### 3.4 MSRegionInfo 结构变化

#### freeHead 放在哪里

**`allocBitmap*`（+24，`uint64_t*`，8B）原位重用为 `uintptr_t freeHead`（8B）。**

- 所有其他字段偏移完全不变
- 结构体保持 64 字节（cache line 对齐不变）
- `reserved`（+56）不再存 summary word，清零即可（LARGE obj baseIdx 用途保留）

```
+0   allocPtr       (8B)  — bump pointer（ABI 约束，不变）
+8   regionEnd      (8B)
+16  nextRegionIdx  (4B)
+20  prevRegionIdx  (4B)
+24  freeHead       (8B)  ← 原 allocBitmap*，原位重命名（0 = freelist 空）
+32  markBitmap*    (8B)  ← 不变
+40  totalSlotCount (2B)  ← 不变
...
+56  reserved       (8B)  ← 不再存 summary，清零（LARGE baseIdx 用途不变）
```

#### 移除 `allocBitmap` array，缩减 MSBitmapArena slot

`allocBitmap*` 字段原来指向 arena slab 的前半段（`mem`），freelist 后不再使用，
因此 **arena slab 可以减半**：

```
旧：kBitmapSlotBytes = kMaxBitmapWords × 8B × 2 = 1024B/region
    ┌─────────────────────────────────┬─────────────────────────────────┐
    │  allocBitmap  (512B = 64 words) │  markBitmap   (512B = 64 words) │
    └─────────────────────────────────┴─────────────────────────────────┘
       ↑ 原 allocBitmap* = mem              ↑ markBitmap* = mem + 512B

新：kBitmapSlotBytes = kMaxBitmapWords × 8B × 1 = 512B/region  （-50%）
    ┌─────────────────────────────────┐
    │  markBitmap   (512B = 64 words) │
    └─────────────────────────────────┘
       ↑ markBitmap* = mem（整块给 markBitmap）
```

`InitForSizeClass` 中 `markBitmap = mem`（不再加 `bitmapBytes` 偏移）。

#### 需要同步修改的位置

| 位置 | 改动 |
|------|------|
| `MSBitmapArena.h`：`kBitmapSlotBytes` | `× 1` 替换 `× 2` |
| `MSRegionInfo.h`：+24 字段定义 | `uint64_t* allocBitmap` → `uintptr_t freeHead` |
| `MSRegionInfo.h`：`reserved` 注释 | 去掉 summary word 说明 |
| `MSRegionInfo.cpp`：`InitForSizeClass` | `markBitmap = mem`；删除 allocBitmap memset；`freeHead = 0` |
| `MSRegionInfo.cpp`：`ReleaseBitmaps` / `Reset` / `ResetToBump` | 移除所有 `allocBitmap = nullptr` / allocBitmap memset |
| `MSRegionInfo.cpp`：`SweepRegion` | 删除 `allocBitmap[i] = ~markBitmap[i]` 及 summary；改为构建链表并写 `freeHead` |
| `MSCentralRegionPool.cpp`：`allocBitmap != nullptr` 检查 | 改为 `markBitmap != nullptr` |
| log 打印中的 `allocBitmap=%p` | 替换为 `freeHead=0x%zx` |



---

## 4. 正确性分析

### 4.1 清零时机

- **sweep 时先 `memset`，再写 next ptr**：与现方案的 sweep 时清零等价，
  alloc 时只需清首 8 字节（写 0 覆盖 next ptr），**不新增 alloc 热路径清零开销**。
- alloc 时仅 1 次 8B store（`*(uintptr_t*)head = 0`），比现方案的 bitmap bit-clear 更轻量。

### 4.2 并发 Sweep 是否需要加锁

**不需要。** 原因：

1. `MSCentralRegionPool::GetRegion()` 通过 `CasSweepGen(sg-2, sg-1)` CAS
   确保一个 region 只有一个 sweep owner。
2. `SweepRegion()` 完成后 region 进入 `partialSwept` pool，
   alloc 线程才能通过 `GetRegion()` 取走该 region。
3. sweep 写 freelist、alloc 读 freelist，**时间上完全串行**，无竞争。

另：lazy sweep 在 concurrent mark 阶段被 `skipLazySweep_=true` 完全禁止，
`ReenableLazySweep()` 仅在 `MarkTermination` STW 完成后调用。
mark 阶段与 sweep 阶段时间上互斥。

### 4.3 GC Tracing 风险

**结论：不存在 freelist 引入的额外风险。**

分析：

1. **GC 追踪入口**：GC 只追踪从 live 对象引用字段到达的对象。
   freelist 中的 dead slot 的 `markBitmap` bit = 0，
   若 live 对象引用了 dead slot，则这是已有的 GC invariant violation，
   与 freelist 无关。

2. **时序隔离**：concurrent mark 阶段 sweep 完全禁止；
   sweep 阶段 concurrent mark 已结束、mutator write barrier 进入 idle 模式。
   两者时间上不重叠。

3. **IsValidObject 防护**（`DiscoverObject` 内部）：
   ```cpp
   if (UNLIKELY(obj == nullptr || !obj->IsValidObject())) { return false; }
   ```
   freelist 中 slot 的首 8 字节是 next ptr（一个堆内地址或 0），
   作为 `stateWord` 极大概率不符合有效对象格式，会被此处直接拦截。

---

## 5. 与现方案的对比

| 维度 | 现方案（summary bitmap） | Freelist 方案 |
|------|------------------------|--------------|
| Alloc 热路径计算 | 2× ctzll + 1 bit-clear + 条件写 reserved | 2× load + 1× 8B store + 1× store |
| Alloc 外部内存访问 | allocBitmap 数组（cache line 外） | slot 首 8B（已在 L1，因为刚从 reserved 取到） |
| Sweep 额外开销 | 无 | 每个死 slot 多 1 次 8B store（写 next ptr） |
| 内存占用 | allocBitmap 占 `bitmapWords*8` 字节 | 无 allocBitmap（长期可去掉） |
| 并发正确性 | sweepGen CAS 保证互斥，OK | 同，OK |
| GC tracing 风险 | 无 | 无 |
| 实现复杂度 | 低 | 低（改动集中在 SweepRegion + AllocFromBitmapFast） |

**潜在优势**：

- frag_alloc 场景下，freelist 的 alloc 热路径访问的 cache line 是 slot 本身，
  与 bump alloc 类似（写对象数据时已在 L1），而 bitmap 方案需要额外访问 `allocBitmap` 数组。
- 碎片越高，`allocBitmap` 数组的 cache miss 概率越高（每个 word 只剩少量 bit），
  freelist 此时相对收益更大。

**潜在劣势**：

- Sweep 时每个死 slot 多 1 次 8B store（构建 next ptr），
  对 liveCount 极低（≈0%存活）的 region，sweep 开销略增。
- `reserved` 语义改变，需同步修改所有使用 `reserved` 作为 summary word 的调用点。

---

## 6. 实现步骤（待执行）

1. **`MSBitmapArena.h`**
   - `kBitmapSlotBytes = kMaxBitmapWords * sizeof(uint64_t)` （去掉 `* 2`）

2. **`MSRegionInfo.h`**
   - +24 字段：`uint64_t* allocBitmap` → `uintptr_t freeHead`（原位替换，类型改变，偏移/大小不变）
   - `reserved` 注释：去掉 summary word 说明（LARGE baseIdx 含义保留）
   - 删除 `AllocFromBitmapFast()`，新增 `AllocFromFreelistFast()`

3. **`MSRegionInfo.cpp` → `InitForSizeClass()`**
   - 删除 `allocBitmap = mem` 及 allocBitmap 相关 memset
   - 改为 `markBitmap = mem`（整块 slot 给 markBitmap）
   - `reserved = 0`（初始 freeHead 为空；BUMP 区域不用 freelist）

4. **`MSRegionInfo.cpp` → `SweepRegion()`**
   - 删除 `allocBitmap[i] = ~markBitmap[i]` 及 summary 计算
   - 在 freeBits 遍历内：先 `memset(slot, 0, slotSize)`，再 `*(uintptr_t*)slotAddr = freeHead; freeHead = slotAddr`
   - 末尾：`reserved = freeHead`

5. **`MSRegionInfo.cpp` → `ReleaseBitmaps()` / `Reset()` / `ResetToBump()`**
   - 移除所有 `allocBitmap = nullptr` / allocBitmap memset

6. **`MSCentralRegionPool.cpp`**
   - `allocBitmap != nullptr` 检查 → 改为 `markBitmap != nullptr`
   - log 打印中删除 `allocBitmap=%p`

7. **`MSAllocBuffer.cpp`（分配调用点）**
   - 调用 `AllocFromBitmapFast()` 的地方改为 `AllocFromFreelistFast()`

4. **编译 + 部署**
   ```bash
   cd runtime && python3 build.py build --gc-mode marksweep
   cp output/temp/lib/aarch64_Release/libcangjie-runtime.dylib \
      /Users/lipeisheng/code/cangjie/cangjie-pkg/self/dev260509/cangjie/runtime/lib/darwin_aarch64_cjnative/libcangjie-runtime.dylib
   codesign -f -s - .../libcangjie-runtime.dylib
   ```

5. **测试**
   ```bash
   cd MSGC_RESULT/Result/case_mid
   cjc frag_alloc.cj -o bench_freelist
   for i in 1 2 3 4 5; do
     MRT_REPORT=/dev/null cjHeapSize=512mb cjProcessorNum=6 ./bench_freelist
   done
   ```
   对比基准：Run 2-4 稳态，Baseline ~60ms，Phase2 ~70ms。

---

## 7. 预期结果与决策标准

- **若 Phase2 ≤ 60ms（vs 旧版 65–77ms）**：接受，合入主线
- **若 Phase2 持平（65±5ms）**：中性，可接受（减少了 allocBitmap 内存）
- **若 Phase2 > 80ms**：回退，转向 Word-level cache 方案（Step13 附录）

---

## 9. LLVM AArch64 内联快速路径适配

### 9.1 现状：当前 PARTIAL 路径的内联汇编

编译器在 `AArch64AsmPrinter.cpp` 中为每个 `CJ_MCC_NewObject*Fast*` statepoint
生成内联 AArch64 汇编。相关入口函数：

| 函数 | 触发条件 |
|------|---------|
| `emitMccNewObjectForMSGC()` | wrapper 有 `cj-msgc-sizeclass` 属性（静态已知 sizeClass） |
| `emitMccNewObjectForMSGCDynamic()` | wrapper 有 `cj-msgc-dynamic` 属性（运行时查表） |

两个版本的 **PARTIAL 路径逻辑完全相同**，只在 SlotSize 来源上不同（静态版用 `movz` 立即数，动态版从 `X15` 寄存器读）。

当前 PARTIAL 路径关键常量（文件顶部定义，LLVM 与 runtime 必须对齐）：

```cpp
// AArch64AsmPrinter.cpp 顶部
constexpr int64_t MSRegionInfoAllocBitmapOffset  = 24;  // allocBitmap* 字段偏移
constexpr int64_t MSRegionInfoMarkBitmapOffset   = 32;  // markBitmap*  字段偏移
constexpr int64_t MSRegionInfoFreeSlotCountOffset = 42; // freeSlotCount 字段偏移
constexpr int64_t MSRegionInfoReservedOffset     = 56;  // reserved/summary 字段偏移
```

**当前 PARTIAL 路径汇编伪代码**（共约 26 条指令，有 Retry 回路）：

```asm
LPartialRetry:
  ldr  x12, [x10, #56/8]     ; x12 = reserved (summary word)
  cbz  x12, LSlow             ; summary == 0 → 无空 slot → slow
  rbit x13, x12
  clz  x13, x13               ; x13 = 最高设位的 word index（bitmap word 编号）
  ldr  x14, [x10, #24/8]     ; x14 = allocBitmap*（bitmap 数组基址）
  add  x14, x14, x13, lsl #3 ; x14 = &allocBitmap[wordIdx]
  ldr  x16, [x14]             ; x16 = bitmap word
  cbz  x16, LPartialRepair    ; word 为 0（summary 与 bitmap 不一致）→ repair
  rbit x17, x16
  clz  x17, x17               ; x17 = bit index（slot index in word）
  mov  x11, #1
  lsl  x11, x11, x17
  bic  x16, x16, x11          ; 清除该 bit
  str  x16, [x14]             ; 写回 bitmap word
  ldrh w11, [x10, #42/2]     ; w11 = freeSlotCount
  sub  w11, w11, #1
  strh w11, [x10, #42/2]     ; freeSlotCount--
  cbnz x16, LPartialSummaryDone ; word 还有其他 bit → 不更新 summary
  ; word 已清零 → 清除 summary 中对应 bit
  mov  x11, #1
  lsl  x11, x11, x13
  bic  x12, x12, x11
  str  x12, [x10, #56/8]     ; 写回 summary
LPartialSummaryDone:
  ; 计算对象地址：slotIdx = x13*64 + x17，objAddr = slotIdx*SlotSize + regionStart
  mov  x11, #64
  madd x13, x13, x11, x17    ; x13 = global slot index
  ldr  x14, [x10, #1*8]      ; x14 = regionEnd
  mov  x11, #32768            ; MSRegionSize
  sub  x14, x14, x11          ; x14 = regionStart = regionEnd - 32KiB
  madd x16, x13, x15, x14    ; x16 = slotIdx * SlotSize + regionStart（x15=SlotSize）
  str  x0, [x16]              ; slot[0] = KlassInfo
  mov  x0, x16                ; 返回对象地址
  ; （接 MarkNewObject + Finalizer + B LFin）

LPartialRepair:
  ; summary bit 已损坏 → 清除 summary 中该 bit，retry
  mov  x11, #1
  lsl  x11, x11, x13
  bic  x12, x12, x11
  str  x12, [x10, #56/8]
  b    LPartialRetry
```

总指令数：约 26 条（不含 MarkObject），含 1 个 Retry 回路和 1 个 Repair 跳转。

---

### 9.2 Freelist 适配：需要的变化

#### 变化1：常量重命名（语义更新）

```cpp
// 旧名（语义是 bitmap 指针）
constexpr int64_t MSRegionInfoAllocBitmapOffset = 24;

// 新名（语义是链表头）—— 偏移值不变，仍为 24
constexpr int64_t MSRegionInfoFreeHeadOffset = 24;   // uintptr_t freeHead
```

`MSRegionInfoReservedOffset = 56` 在 PARTIAL 路径中不再访问（freelist 不需要 summary word）。`MSRegionInfoMarkBitmapOffset = 32` 和 `MSRegionInfoFreeSlotCountOffset = 42` **保持不变**，`MarkNewObject` 子程序继续使用它们。

#### 变化2：PARTIAL 路径整体替换

旧 PARTIAL 路径（26 条指令、含 Retry+Repair 回路）→ 新 freelist pop（**9 条指令，无回路**）：

**新 PARTIAL 路径汇编伪代码**：

```asm
LPartialRetry:  ; 标签保留，慢路径跳回入口不变
  ldr  x12, [x10, #24/8]     ; x12 = freeHead（freeHead 在 +24，LDRXui imm = 3）
  cbz  x12, LSlow             ; freeHead == 0 → region 已满 → slow path
  ldr  x13, [x12]             ; x13 = *freeHead（slot 的第一个字是 next 指针）
  str  xzr, [x12]             ; 清除 slot 中的 next 指针（内容清零，避免 GC 误追踪）
  str  x13, [x10, #24/8]     ; freeHead = next（弹出链表头）
  ldrh w11, [x10, #42/2]     ; w11 = freeSlotCount
  sub  w11, w11, #1
  strh w11, [x10, #42/2]     ; freeSlotCount--
  str  x0,  [x12]             ; slot[0] = KlassInfo（X0 = klass ptr）
  mov  x0,  x12               ; 返回对象地址
  ; （接 MarkNewObject + Finalizer + B LFin，与旧版完全相同）
```

指令数：**9 条**（vs 旧版约 26 条）。无条件跳转回路，指令缓存友好。

#### 变化3：BUMP 路径不变

BUMP region（`regionType == MSGCRegionBump`）的内联汇编逻辑**完全不涉及 allocBitmap / freeHead**，`allocPtr`（偏移 0）、`regionEnd`（偏移 8）的布局不变，无需修改。

---

### 9.3 需要修改的 LLVM 文件

| 文件 | 修改内容 |
|------|---------|
| `llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp` | ① `MSRegionInfoAllocBitmapOffset` 重命名为 `MSRegionInfoFreeHeadOffset`（值仍为 24）<br>② 在 `emitMccNewObjectForMSGC()` 中，将 PARTIAL 路径（约 70 行 LPartialRetry～LPartialRepair）替换为 9 条 freelist pop 指令<br>③ 在 `emitMccNewObjectForMSGCDynamic()` 中，同样替换 PARTIAL 路径（两个版本逻辑相同，但 SlotSize 来源不同；freelist pop 两版完全一致，可进一步合并为 lambda） |

> **注意**：`emitMccNewObjectForMSGCDynamic()` 的 BUMP 路径用 `X15` 代替静态 SlotSize
> 立即数，但 PARTIAL 路径的 freelist pop **不依赖 SlotSize**，因此两个版本的新 PARTIAL
> 路径代码完全相同，可以提取为公共 lambda。

---

### 9.4 ABI 约束：偏移硬编码规则

LLVM 内联汇编直接把 `MSRegionInfo` 字段偏移编码为 `LDRXui/STRXui` 的立即数，
**不经过符号/宏间接引用**，因此 runtime 结构体偏移与 LLVM 常量必须严格一致：

| 字段 | 结构体偏移（字节） | LLVM 立即数（= 偏移 / 8） |
|------|---------|---------|
| `freeHead`（原 `allocBitmap*`） | +24 | `3` |
| `markBitmap*` | +32 | `4` |
| `freeSlotCount` | +42 | `21`（halfword，以 2 字节为单位：`42/2=21`） |
| `reserved` | +56 | `7`（freelist 后不再在 PARTIAL 路径访问） |

> 修改 runtime 结构体时，必须同步更新 `AArch64AsmPrinter.cpp` 顶部的常量定义；
> 修改 LLVM 常量时，必须确认 runtime 结构体偏移一致。两者不匹配会导致**静默内存踩踏**，极难排查。

---

## 8. 附录：Word-level cache 方案（备选）

若 freelist 实验结果不理想，下一个候选方向：

**在 `AllocBufferMS` 中缓存当前 word 的剩余 mask：**

```cpp
struct CachedWord {
    uint64_t* wordPtr;      // 指向 allocBitmap[w]
    uint64_t  remaining;    // 当前 word 的剩余空闲 bit mask（已从 bitmap 加载但未写回）
};
```

每次 alloc：
1. 若 `remaining != 0`：`ctzll(remaining)` 得 bit，`remaining &= remaining-1`，**不写 allocBitmap**
2. `remaining == 0`：flush（`*wordPtr = 0`，可能更新 summary），加载下一个 word

**收益**：每个 word（64 个 slot）只有 1 次 `allocBitmap` 写（而非 64 次）。
**代价**：`AllocBufferMS` 增加 16 字节字段，region 切换时 flush 逻辑稍复杂。
