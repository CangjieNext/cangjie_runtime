# Step48 X86 与 AArch64 MSGC 跨平台完整快路径适配设计与实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在不改变公共 lowering 和 wrapper ABI 语义的前提下，使 MSGC 分配与写屏障快路径覆盖 X86-64 Linux/ELF、X86-64 Windows/MinGW COFF、AArch64 Linux/ELF，并保持 AArch64 macOS/Mach-O 的现有能力和语义一致性。

**Architecture:** 公共 LLVM CodeGen 只识别 GCMode 和 MSGC wrapper 语义；Runtime 通过稳定 C ABI 暴露快路径状态；AArch64 与 X86 分别在最终机器码发射层实现相同语义，并按目标 ABI、ObjectFormat 和原子指令能力选择具体序列。任何不满足条件的路径必须在发射首条指令前返回 `false`，完整保留 wrapper 慢路径。

**Tech Stack:** LLVM 15 C++14、LLVM IR/MC、AArch64/X86 AsmPrinter、Mach-O/ELF/COFF、SysV/Win64 ABI、`x86_64-w64-windows-gnu` MinGW triple、Cangjie Runtime C++14、lit/FileCheck、llvm-readobj、llvm-objdump。

**Workspace note:** 当前 Runtime 与 LLVM 工作树包含用户已有的未提交 MSGC 修改。本计划在原工作区分阶段执行，只修改列明文件，不清理构建目录，不覆盖无关修改，也不启动 SDK、stdlib 或 stdx 构建。

---

## 1. 范围与非目标

### 1.1 目标平台

本轮只覆盖下列四种 64 位组合：

| 架构 | 操作系统 | ObjectFormat | 调用约定 | 本轮定位 |
|---|---|---|---|---|
| AArch64 | macOS | Mach-O | Apple AArch64 | 保持现有完整能力并做回归 |
| AArch64 | Linux | ELF | AAPCS64 | 补齐 `-lse`，使 `+lse/-lse` 均可内联 |
| X86-64 | Linux | ELF | SysV AMD64 | 补齐分配和 MARK 写屏障完整路径 |
| X86-64 | Windows | COFF | MinGW Win64 | 使用 `x86_64-w64-windows-gnu`，补齐稳定数据符号、参数映射、分配和写屏障 |

“效果一致”定义为：四个平台使用相同的 GC phase、对象/Region
判定、标记位更新、WorkBuf 入队、分配补货和 wrapper 回退语义；机器指令可以因 ISA、ABI
和 ObjectFormat 不同而不同，不承诺指令条数或吞吐率完全相同。

### 1.2 要覆盖的 MSGC 快路径

- 静态尺寸和动态尺寸的对象分配。
- 静态尺寸和动态尺寸的数组分配。
- `Ref`、`StaticRef`、`Struct`、`StaticStruct`、`ArrayCopyRef`、
  `ArrayCopyStruct` 六类写屏障。
- IDLE phase 的引用写入、`memcpy`、`memmove` 快路径。
- MARK phase 仅对 `Ref`、`StaticRef`、`Struct`、`StaticStruct` 执行堆范围判定、
  SMALL/LARGE Region 标记、成对字段原子提交、`GCTib` 引用位图扫描和
  `WorkBuf` 入队。`ArrayCopyRef/ArrayCopyStruct` 在 MARK phase 所有平台均保持 wrapper 慢路径。
- 快路径不能处理时原封不动调用现有 `CJ_MCC_*_MSGC` wrapper。
- CopyGC 路径、公共 intrinsic 到 wrapper 的选择和显式 GCMode 校验保持不变。

### 1.3 非目标

- 不适配 X86-64 macOS、AArch64 Windows、X86-32、ARM32、iOS、Android 或 OHOS。
- 不把对象布局、TLS、Region 位图和原子循环下沉到公共 LLVM IR lowering。
- 不改变现有 MSGC wrapper 名称、函数签名或前端 intrinsic。
- 不在本阶段修改 Runtime GC 算法、Region 布局或 WorkBuf 容量。
- 不在本阶段构建 SDK、runtime dylib、stdlib、stdx 或性能用例；完成 LLVM
  定向测试后另行确认构建边界。
- 不以单次 benchmark 数值作为代码正确性的替代；性能验证属于后续 SDK
  集成阶段。

## 2. 现状与差距矩阵

### 2.1 实施前代码能力

| 平台 | MSGC 分配 | IDLE 写屏障 | MARK 写屏障 | 关键限制 |
|---|---|---|---|---|
| AArch64 macOS/Mach-O | 静态/动态对象与数组 | 六类 inline | Ref/Static/Struct inline，ArrayCopy wrapper | 直接依赖 Itanium C++ 数据符号 |
| AArch64 Linux/ELF `+lse` | 静态/动态对象与数组 | 六类 inline | Ref/Static/Struct inline，ArrayCopy wrapper | 依赖 LSE 和 C++ 数据符号 |
| AArch64 Linux/ELF `-lse` | 静态/动态对象与数组 | wrapper | wrapper | 能力判断在发码前拒绝整条屏障 |
| X86-64 Linux/ELF | wrapper | phase gate 后 store/memcpy/memmove | wrapper | 没有分配 emitter，没有 MARK 实现 |
| X86-64 Windows/MinGW COFF | wrapper | wrapper | wrapper | emitter 明确拒绝 COFF，数据符号 ABI 不稳定 |

当前公共层已经完成以下拆分，不需要再次重构：

- `CJBarrierLowering.cpp` 仅依赖 64 位 wrapper ABI，不判断 AArch64/X86。
- `CJMSGCABI.h` 统一识别分配种类、写屏障种类、36 个 size class 和 GCMode。
- `AsmPrinter.cpp` 只调用目标 hook；目标 hook 返回 `false` 时保留 wrapper。
- AArch64 和 X86 已经各有独立 `*MSGCEmitter.cpp`，ISA 差异位于最终发码层。

### 2.2 必须补齐的差距

1. Runtime 快路径状态仍通过 C++ 静态成员的 Itanium mangled name 访问；这对
   Windows/MSVC 名字修饰不稳定，也让后端绑定 Runtime 内部类名。
2. AArch64 标记原子操作只实现 `LDSETAL*`，导致 Linux `-lse` 被整体降级为 wrapper。
3. X86 没有 MSGC 对象/数组分配 emitter、动态 size-class 表和函数结束时的数据表发射。
4. X86 MARK phase 没有 Region 分类、bitmap 原子设置、结构体引用扫描和 WorkBuf
   提交逻辑。
5. Win64 第五个及以后参数位于调用者栈参数区，COFF 外部数据要经 `__imp_` IAT
   间接访问；当前 X86 emitter 两者都未实现。
6. LLVM 的 MSGC inline Ref/StaticRef 已直接读写完整 64 位指针，但 Runtime MSGC
   barrier/collector 仍调用公共 `RefField::GetTargetObject/SetTargetObject`，会按 CopyGC
   tagged-pointer 布局只解释低 48 位；必须在 MSGC 专用路径统一为 raw 64-bit load/store。
7. 现有测试把 AArch64 ELF `-lse` 和 X86 数组分配 wrapper 当作最终预期；实现后要先把
   这些测试改成 RED，再更新为新能力的正向约束。

## 3. 方案比较与最终决策

### 3.1 方案 A：各目标复制现有 AArch64 实现并继续访问 C++ 符号

优点是短期改动集中在后端；缺点是 X86 与 AArch64 会各自复制 Runtime
布局常量，Windows 还需要推导编译器相关的 C++ 修饰名。Runtime 类名或成员名一变，已生成代码
会静默失配。该方案不能形成稳定 COFF ABI，因此拒绝。

### 3.2 方案 B：在公共 lowering 中生成完整 LLVM IR 快路径

公共 IR 可以复用大量语义，但必须暴露 TLS 偏移、Region 布局、位图算法、WorkBuf
布局和目标原子能力。它会把当前已经建立的“公共 wrapper 语义、目标最终发码”边界重新打破，
也使 Win64 栈参数和 statepoint 处理更复杂，因此拒绝。

### 3.3 方案 C：稳定 C 状态 ABI + 共享语义常量 + 目标 emitter

这是本轮采用的方案：

1. Runtime 只导出一个稳定、定长的 C 数据符号，集中发布 phase、堆范围和
   `MSRegionInfo` 数组地址。
2. LLVM 在 `CJMSGCABI.h` 中共享与 Runtime `static_assert` 对应的偏移、枚举值和
   size class；不共享 ISA 指令。
3. AArch64 emitter 对 MARK 标记选择 LSE 或 LL/SC；X86 emitter 对 MARK 标记选择
   `lock bts/cmpxchg`。分配 bitmap 由当前 P 独占，AArch64/X86 都使用普通 load/store，不加锁。
   ObjectFormat 差异只出现在获取外部数据地址和 libc 符号的最终发码位置。
4. 公共 lowering 和 wrapper 始终可执行；目标 emitter 只有在能完整生成整条快路径时才接管。

该方案同时满足 Linux、macOS、Windows 的 ABI 稳定性，并允许两个架构独立调整寄存器分配，
而不复制 GC 语义定义。

## 4. Runtime 稳定 C ABI

### 4.1 数据结构和符号

新增 Runtime 内部头文件 `src/Heap/MSGCFastPath.h`，声明下列 C linkage 数据：

```cpp
struct MSGCFastPathState {
    uint8_t barrierPhase;       // offset 0: 0=IDLE，1=MARK/保守慢路径
    uint8_t reserved[7];        // offset 1
    uintptr_t heapReservedStart; // offset 8
    uintptr_t heapReservedEnd;   // offset 16，范围上界不包含
    uintptr_t regionInfoArray;   // offset 24，MSRegionInfo* 的整数表示
};

extern "C" MRT_EXPORT MSGCFastPathState CJ_MRT_MSGCFastPathState;
```

定义放在 `src/Heap/Heap.cpp`，避免增加独立静态库之间的符号归属。Runtime 必须添加：

```cpp
static_assert(sizeof(MSGCFastPathState) == 32, "compiler ABI");
static_assert(offsetof(MSGCFastPathState, barrierPhase) == 0, "compiler ABI");
static_assert(offsetof(MSGCFastPathState, heapReservedStart) == 8, "compiler ABI");
static_assert(offsetof(MSGCFastPathState, heapReservedEnd) == 16, "compiler ABI");
static_assert(offsetof(MSGCFastPathState, regionInfoArray) == 24, "compiler ABI");
```

旧的 `Heap::inlineFastPathGCPhase`、`Heap::heapReservedStartAddr`、
`Heap::heapReservedEndAddr` 暂时保留并同步更新，避免破坏仍引用旧符号的已有对象文件；新 LLVM
代码只引用 `CJ_MRT_MSGCFastPathState`。稳定 ABI 不导出 C++ 类名，也不导出
`MSRegionInfoArray::array_`。

### 4.2 发布位置与内存序

| 字段 | 更新位置 | 发布要求 |
|---|---|---|
| `heapReservedStart/End` | `Heap::OnHeapCreated` | 在任何 mutator 开始前写入；使用 release store |
| `regionInfoArray` | `MSRegionInfoArray::Init` 成功后 | 完成数组初始化后 release store |
| `regionInfoArray=0` | `MSRegionInfoArray::Cleanup` | 清理前先阻止并发 mutator，随后 release store 0 |
| `barrierPhase` | `HeapImpl::InstallBarrier` | 与旧 phase byte 同步做 release store |

发码端以 acquire 方式读取 `barrierPhase`。MARK 路径只有观察到 phase=1 后才读取堆范围和
Region 数组指针，从而继承初始化发布；IDLE 路径不读取其余字段。若状态指针、堆范围或 Region
指针为零，必须转到 wrapper，不能尝试恢复或部分执行。

### 4.3 ObjectFormat 访问方式

- AArch64 Mach-O/ELF：通过目标 mangler 获取语义名，再用目标已有 GOT
  page/pageoff 序列取得状态结构地址。
- X86-64 ELF：用 GOTPCREL 取得 `CJ_MRT_MSGCFastPathState` 地址。
- X86-64 COFF：先从 `__imp_CJ_MRT_MSGCFastPathState(%rip)` 读取 IAT 中的数据地址；
  不直接把 IAT 槽内容当成 phase。
- X86-64 PE 构建当前使用 MinGW `--export-all-symbols`；对象测试仍必须验证生成的是 IAT
  relocation。若未来取消 export-all，Runtime 构建系统必须显式 `dllexport` 此符号。

### 4.4 ABI 兼容策略

结构尾部只能追加字段，已发布字段不得改变偏移和含义。LLVM 仅引用固定偏移，不读取
`reserved`。若将来需要不兼容变化，应新增带版本后缀的符号，而不是原地修改结构布局。

## 5. LLVM 公共 ABI 与布局常量

### 5.1 常量归属

在 `llvm/include/llvm/CodeGen/CJMSGCABI.h` 的 `msgc::layout` 命名空间集中定义
稳定状态结构偏移，并共享 36 级 size-class 表。其余目标 emitter 布局常量仍是
目标 translation unit 内常量，由 Runtime `static_assert` 和 LLVM 汇编测试双向约束：

- `MSGCFastPathState`：偏移 `0/8/16/24`、结构大小 32。
- `ThreadLocalData::msAllocBuffer`：偏移 88。
- `AllocBufferMS`：`activeRegions_` 偏移 8、元素跨度 8、36 项，`activeLog_`
  偏移 896。
- `WorkBuf`：`len` 偏移 8、`objs` 偏移 16、容量 1024。
- `MSRegionInfo`：大小 64，以及 `allocPtr=0`、`regionEnd=8`、
  `allocBitmap=24`、`markBitmap=32`、`totalSlotCount=40`、
  `freeSlotCount=42`、`divMagic=44`、`regionType=50`、`flags=51`、
  `reserved=56`。
- Region 大小 32768，BUMP/PARTIAL/FULL/LARGE 值为 `1/2/3/4`。
- TypeInfo/GCTib 快路径使用的 `payload=8`、`gctib=16`、
  `componentTypeInfo=64`、flag byte=9，以及 RawArray/has-ref 标志。
- MSGC RefField 槽按完整 64 位原始指针解释，不定义地址掩码；公共 CopyGC
  tagged-pointer 布局不属于 MSGC emitter ABI。

size-class 表使用公共定义；`MSGCShortGCTibMaxRefSlots` 和其他目标内布局常量保留在 emitter。Runtime
对应类型的 `static_assert` 是源端防线，LLVM lit 汇编检查是消费端防线。两边不能通过
自动包含同一个 C++ 头文件耦合构建，但常量注释必须同时给出对端文件路径。

### 5.2 分类与能力边界

`CJMSGCABI.h` 只回答 wrapper 是什么、布局是什么，不回答某个 ISA 是否能发码。
能力判断仍放在：

- `AArch64CangjieCallSize.cpp`：AArch64/ObjectFormat/原子序列能力和 shadow 大小。
- `X86MCInstLower.cpp` 与 `X86MSGCEmitter.cpp`：X86-64、SysV/Win64、ELF/COFF
  能力和精确字节计数。

公共 `AsmPrinter` 不新增 `isAArch64()`、`isX86()`、`isMachO()` 或 `isELF()`
分支。CopyGC 和 MSGC 继续通过不同 wrapper 分类器互斥进入目标 emitter。

## 6. AArch64 Linux/ELF 非 LSE 路径

### 6.1 能力判断调整

`supportsMSGCWriteBarrier` 对 AArch64 Mach-O/ELF 均返回可尝试发射，不再因为
ELF 缺少 `+lse` 而整体拒绝。Emitter 内部选择：

- Mach-O：保持当前 Apple AArch64 LSE 序列，避免无关性能变化。
- ELF `+lse`：保持 `LDSETALX/LDSETALB`。
- ELF `-lse`：使用 AArch64 基线独占访问指令循环。
- 其他 ObjectFormat：在发射前返回 `false`，保留 wrapper。

`AArch64CangjieCallSize.cpp` 的能力判断和 emitter 必须调用同一谓词；最大 shadow
字节数要覆盖 LL/SC 重试序列，不能继续只按 LSE 指令数估计。

### 6.2 64 位 bitmap 原子 OR

新增 emitter 内部 helper，接口返回原值，调用方继续用原值判断谁赢得 mark：

```text
retry:
    ldaxr   old, [word]
    orr     new, old, mask
    stlxr   status, new, [word]
    cbnz    status, retry
```

LSE 路径的 same-word pair mark 合并 mask 后一次提交；different-word pair
分别提交。`-lse` 实现为逐引用回退到单对象 LL/SC helper，每个引用仍由
旧值决定是否入队；它不要求两个 bit 必须合并成一次 LL/SC。重试循环不得
重新计算或覆盖引用值、WorkBuf 地址和慢路径参数。

### 6.3 LARGE flag 字节原子 OR

LARGE Region 的 `flags_` 只能用字节独占访问，不能扩大为 32/64 位操作：

```text
retry:
    ldaxrb  oldW, [flags]
    orr     newW, oldW, #1
    stlxrb  statusW, newW, [flags]
    cbnz    statusW, retry
```

循环结束后检查 `oldW & FLAG_MARKED`。这与 Runtime 的
`__atomic_fetch_or(&flags_, FLAG_MARKED, ...)` 等价，不覆盖相邻的 `sweepGen`。

### 6.4 数据符号迁移

AArch64 emitter 删除 phase、heap start/end 和 `MSRegionInfoArray::array_` 四个
C++ 符号名，只解析一次 `CJ_MRT_MSGCFastPathState` 地址，再按公共偏移加载字段。
`memcpy/memmove` 继续交给目标 mangler 处理 Mach-O 前导下划线和 ELF 原名。

### 6.5 AArch64 RED 测试

先修改 `llvm/test/CodeGen/AArch64/cangjie-msgc-elf-capability.ll`：

- `+lse` 检查 `ldsetal/ldsetalb`，不出现无条件 wrapper call。
- `-lse` 检查 `ldaxr/stlxr` 和 `ldaxrb/stlxrb` 重试边，不再检查慢调用。
- ELF object 检查稳定 C 符号 GOT relocation，不再出现四个 C++ mangled name。
- Mach-O 回归检查稳定符号被目标 mangler 写成 `_CJ_MRT_MSGCFastPathState`。

## 7. X86-64 Linux/ELF 完整快路径

### 7.1 总体发射边界

扩展 `X86MSGCEmitter.cpp`，但只对 X86-64、ELF、C calling convention、普通
`CALL64pcrel32`/受支持的 statepoint 形式接管。X86-32、tail call、非 C calling
convention 和无法识别的符号在发射前返回 `false`。

所有指令必须经过 `EmitAndCountInstruction`，使 stackmap shadow 按实际 X86 编码字节数
计数。每条扩展序列只调用一次 `SM.recordCJStackMap`；不能让公共 AsmPrinter 再记录一次。

### 7.2 参数寄存器

SysV AMD64 wrapper 参数映射如下：

| wrapper | 参数寄存器 |
|---|---|
| `Ref` | ref=`RDI`，obj=`RSI`，field=`RDX` |
| `StaticRef` | ref=`RDI`，field=`RSI` |
| `Struct` | obj=`RDI`，dst=`RSI`，dstSize=`RDX`，src=`RCX`，srcSize=`R8` |
| `StaticStruct` | dst=`RDI`，dstSize=`RSI`，src=`RDX`，srcSize=`RCX`，GCTib=`R8` |
| `ArrayCopy*` | dstObj=`RDI`，dst=`RSI`，dstSize=`RDX`，srcObj=`RCX`，src=`R8`，srcSize=`R9` |

对象分配使用 typeInfo=`RDI`、size=`RSI`；数组分配使用 typeInfo=`RDI`、
length=`RSI`、size=`RDX`。静态尺寸仍保留原始 size 参数供 wrapper 慢路径使用，快路径
使用经过属性校验的 size class/slot size。

### 7.3 对象和数组分配

逐语义块移植 AArch64 MSGC emitter，而不是复用 CopyGC 的单一 bump pointer：

1. 从 `R15 + 88` 读取 `AllocBufferMS*`，空指针转慢路径。
2. 静态尺寸按 `activeRegions_ + sizeClass*8` 找 Region；动态尺寸先验证
   `0 < size <= 16384`，再用 36 个 size-class 阈值的 compare-chain 计算 class 和
   slot size。该实现不生成模块级数据表，因此 ELF/COFF 不新增本地表 relocation。
3. BUMP Region 校验 `allocPtr + slotSize <= regionEnd`，写对象头/数组头，更新
   `allocPtr`。
4. PARTIAL Region 执行 allocation bitmap 查找、普通 load/store 占位、summary
   repair/retry，语义与 Runtime `AllocFromBitmapFast()` 和 AArch64 当前实现一致。
   Region 由当前 P 独占，该分配位图不会并发共享；禁止在此路径引入 `lock`、
   `cmpxchg` 或其他原子 RMW。
5. 数组路径检查长度和尺寸溢出，初始化 typeInfo/length，并按 AArch64 现有约定清零必要
   payload。
6. finalizer 对象成功后调用目标符号 `CJ_MCC_OnFinalizerCreated`；所有失败边恢复原始
   wrapper 参数后调用原 slow target。

采用 compare-chain 是实施阶段对设计的收敛：它保留原始 size 参数直到确定已成功
占用 PARTIAL slot，且无需为 MinGW COFF 增加 `.rdata` 表的 section/finalization 管理。

### 7.4 写屏障

1. 通过 `CJ_MRT_MSGCFastPathState@GOTPCREL` 取得状态地址并读取 phase；X86 普通
   load 已满足该只读 gate 的 acquire 需求。
2. IDLE：`Ref/StaticRef` 写完整 64 位原始指针；结构体调用 `memcpy`；数组拷贝调用
   `memmove`。参数重排必须处理寄存器覆盖顺序。
3. MARK：先验证引用、容器和字段/范围，再验证 heap start/end 和 Region 元数据；任何
   元数据不一致转 wrapper。
4. SMALL mark 使用 `lock btsq` 原子设置单 bit，并通过 CF 判断旧值。
5. LARGE mark 使用 `lock cmpxchgb` 循环设置 `flags_` bit 0，禁止对相邻字段做宽访问。
6. 新赢得 mark 且对象含引用时，写入 `AllocBufferMS::activeLog_` 的 WorkBuf；空指针或
   容量不足转 wrapper。无引用对象只标记不入队。
7. 结构体按短 GCTib 扫描，由函数内 leaf thunk 逐引用执行与 Ref 相同的
   SMALL/LARGE mark 和 winner 判定。动态 Struct 保守扫描整个容器的短 GCTib，
   可能多标记未覆盖字段所指对象，但不会漏标旧边。RawArray 无引用 component
   可直接复制，含引用 component 转 wrapper。

### 7.5 寄存器策略

SysV 没有 Win64 home space。Ref 路径在尚可转 wrapper 时保留原始参数寄存器；
Struct/StaticStruct 在进入 phase gate 前把 5 个原参数保存到 SysV caller-saved
`XMM8..XMM12`，循环 cursor/bitmap 临时保存到 `XMM0/XMM1`。Emitter 不使用
red zone、callee-saved GPR 或 `push/pop`；函数内 thunk 不调用外部符号，不破坏这些 XMM 值。

## 8. X86-64 Windows/MinGW COFF 完整快路径

LLVM 代码生成、lit、object 和 relocation 测试必须使用真实 MinGW triple
`x86_64-w64-windows-gnu`，不得以 `x86_64-pc-windows-msvc` 代替。本机负责生成
COFF 对象并验证 Win64 ABI/IAT relocation；Runtime DLL、SDK 和真实程序运行由用户在
Windows/MinGW 机器验收。

### 8.1 Win64 参数和调用点栈布局

| wrapper | 寄存器参数 | 栈参数（替换 call 的位置） |
|---|---|---|
| `Ref` | RCX=ref，RDX=obj，R8=field | 无 |
| `StaticRef` | RCX=ref，RDX=field | 无 |
| `Struct` | RCX=obj，RDX=dst，R8=dstSize，R9=src | srcSize=`[rsp+32]` |
| `StaticStruct` | RCX=dst，RDX=dstSize，R8=src，R9=srcSize | GCTib=`[rsp+32]` |
| `ArrayCopy*` | RCX=dstObj，RDX=dst，R8=dstSize，R9=srcObj | src=`[rsp+32]`，srcSize=`[rsp+40]` |

这里的偏移是 **caller 中尚未执行原 call 时** 的 outgoing area；进入真实 callee 后因返回
地址会分别表现为 `[rsp+40]` 和 `[rsp+48]`。Emitter 位于 call site，必须使用前一组偏移。

对象分配使用 RCX/RDX，数组分配使用 RCX/RDX/R8。前 32 字节 home space 可保存前四个
原始参数或作为无调用 MARK 路径的 scratch；第五、第六参数槽不可覆盖，直到确认不会再转
wrapper。

### 8.2 COFF 外部状态和本地数据

COFF 路径先执行：

```text
movq __imp_CJ_MRT_MSGCFastPathState(%rip), stateReg
movzbl state.phase(stateReg), phaseReg
```

对象测试必须看到 `IMAGE_REL_AMD64_REL32 __imp_CJ_MRT_MSGCFastPathState`。
不生成 ELF GOTPCREL，也不拼接 MSVC/Itanium C++ 名字。动态 size-class
选择是纯代码 compare-chain，COFF 不需要额外的只读表 relocation。

### 8.3 Win64 发射规则

- 分配、Region、bitmap、WorkBuf 的算法与 X86 ELF 相同，只替换参数映射和外部符号访问。
- IDLE `memcpy/memmove` 使用 RCX/RDX/R8；原 call 已准备的 32 字节 home space 可直接
  供嵌套调用使用，不能再次调整 RSP。
- MARK 需要暂存参数时使用 home space；所有转 wrapper 的边必须恢复 RCX/RDX/R8/R9，
  并保持 `[rsp+32]/[rsp+40]` 原样。
- 禁止 emitter 动态 `sub/add rsp` 或 `push/pop`，避免生成未被 `.seh` unwind info
  描述的瞬时栈布局。
- 直接 call 慢 wrapper 和 libc 使用现有 `X86MCInstLower` 产生的 MC operand，不手写
  PLT/IAT 函数调用规则。

### 8.4 分阶段启用门槛

COFF 不是“一次打开所有 wrapper”的总开关。按 `Ref/StaticRef`、静态分配、动态分配、
结构体、数组拷贝逐类启用；每一类必须同时具备：正确 IAT relocation、Win64 参数恢复、
无额外 RSP 调整、slow wrapper 对照和 object 反汇编检查。未达到门槛的种类继续返回
`false`，但最终验收要求六类 IDLE、四类 MARK Ref/Struct 和四类分配达到门槛；
ArrayCopy 的 MARK 门槛是稳定保留 wrapper。

## 9. AArch64 macOS/Mach-O 回归边界

AArch64 macOS 是现有完整实现的基线，不在本轮重写其分配和屏障算法。允许的改动只有：

- 四个 C++ 数据符号合并为 `_CJ_MRT_MSGCFastPathState`。
- 使用 `CJMSGCABI.h` 中的公共布局常量替代文件内魔数。
- Ref/StaticRef 保持现有完整 64 位原始指针写入，不新增截断或 tag 编码。
- 原子 helper 抽取后，Mach-O 仍选择原有 LSE 指令序列。

必须补充或保留下列回归：静态/动态对象、静态/动态数组、六类 IDLE 写屏障、
Ref/StaticRef/Struct/StaticStruct 四类 MARK inline、两类 ArrayCopy MARK wrapper、SMALL/LARGE、
same-word/different-word pair、no-ref RawArray、WorkBuf full 慢路径、finalizer 回调、
`memcpy/memmove` 符号修饰和 Mach-O GOT relocation。

本轮不把 X86 macOS 扩展成完整快路径。其现有 phase-gated IDLE 行为不主动删除，但不作为
Step48 完成条件，也不能让它影响 Linux/Windows 的能力判断。

## 10. MSGC RefField 原始 64 位语义

### 10.1 代码事实

- AArch64 MSGC inline Ref/StaticRef 当前使用 `STR X0`，X86 使用 `MOV64mr RDI`，已经
  直接保存完整 64 位指针。
- Runtime 公共 `RefField` 仍是 CopyGC tagged-pointer 表达；其
  `GetTargetObject/SetTargetObject` 只解释或构造低 48 位地址。
- MSGC 不移动对象，也不使用 CopyGC 的 `isTagged/tagID` 语义。MSGC 专用 barrier、
  collector 和 weak-ref 路径应通过 raw helper 读取/写入 `GetFieldValue/SetFieldValue`，
  不能再隐式进入公共 tagged-pointer 编解码。

### 10.2 MSGC 规则

- Ref/StaticRef 的新值、旧值、same-value 比较、heap-range 判断、mark 和 WorkBuf 入队都
  使用完整 64 位 `ObjectPtr`。
- MSGC 原子读写、swap 和 CAS 同样使用完整 64 位 bit pattern。
- Struct/ArrayCopy 按完整 8 字节引用槽做 memcpy/memmove 和扫描，不逐槽掩码。
- 公共 `RefField` 布局及 CopyGC 使用方式不修改；`MSRefField.inline.h` 只提供 MSGC raw
  helper，MSGC 也会执行的公共源码通过 `GCRefField.inline.h` 按构建模式分派。
- AArch64 emitter 中从对象头读取后执行的 `UBFM #0,#47` 是在解码
  `StateWord` 内 48 位 TypeInfo 与 16 位 object state，不是 RefField 截断；本轮保留。
- `CompilerCalls.cpp::NormalizeRuntimeTypeInfo` 同样属于 TypeInfo/StateWord 防御，不按
  引用字段规则删除。若未来把 StateWord 改为完整 TypeInfo 指针，需要独立 ABI 方案。

### 10.3 回归要求

- AArch64/X86 汇编检查 Ref/StaticRef store 直接使用原始参数寄存器，store 前不存在
  48 位 mask、shift 或截断。
- Runtime 单元测试使用仅比较 bit pattern、不解引用的高位样本，验证 MSGC raw helper 的
  普通及 atomic load/store/swap/CAS 完整保留 64 位。
- Runtime MSGC barrier/collector 以及 MSGC 可执行的公共源码契约测试禁止直接调用
  `GetTargetObject/SetTargetObject`；清空 weak ref 的 null 写入也走 raw helper。
- CopyGC emitter、公共 `RefField` 和公共 tagged-pointer 读屏障测试保持不变。

## 11. Statepoint、寄存器与调用安全

### 11.1 全有或全无

目标 hook 在以下内容全部确认前不得发射首条指令：目标是受支持的 64 位平台、wrapper
分类有效、C calling convention、call opcode 受支持、慢调用 operand 可 lowering、静态分配
属性合法。发射开始后遇到的动态失败只能跳到本序列内的 slow label，不能再向公共
AsmPrinter 返回 `false`。

### 11.2 慢路径状态

- 任何 slow label 执行原 wrapper call 前，参数寄存器、Win64 栈参数和 RSP 必须与原 call
  point 一致。
- tail call、indirect call、非 C calling convention 和手写不完整 wrapper 保留原调用。
- WorkBuf 空/满、Region 元数据空/越界、size overflow、非法 GCTib、动态尺寸超阈值都属于
  正常慢路径，不是 fatal error。
- IDLE 的 libc 调用和 finalizer 回调使用平台 ABI；进入这些调用前不能留下临时栈偏移。

### 11.3 Stackmap 与 shadow

- AArch64 的 `getCangjieCallSizeInBytes` 要把 LSE/LLSC 两种最大序列纳入上界，且与实际
  capability 谓词一致。
- X86 每条 MCInst 通过 `EmitAndCountInstruction`，不能用 `OutStreamer` 绕过编码长度
  统计；分支 label 不计入指令长度。
- 每个被完整替换的 statepoint 只记录一次 Cangjie stackmap；保持 wrapper 的路径继续走
  原始公共记录逻辑。
- 定向测试必须覆盖 wrapper 位于 statepoint、普通 call、unsupported calling convention
  和 tail-call opcode 的情况。

### 11.4 X86 寄存器审计门槛

每类复杂 wrapper 在实现前先在测试注释或代码注释中列出：入口 live 参数、scratch、
首次允许破坏原参数的位置和所有 slow edge。SysV 禁止隐式使用 callee-saved 寄存器；Win64
只允许使用已经存在的 home space，不改变 RSP。若实际寄存器表无法闭合，该子路径继续
wrapper，不能以遗漏参数恢复换取表面上的 inline。

## 12. 文件修改清单

### 12.1 Runtime

| 文件 | 修改 |
|---|---|
| `src/Heap/MSGCFastPath.h` | 新增稳定 C ABI 状态结构、导出符号和布局说明 |
| `src/Heap/Heap.h` | `OnHeapCreated` 同步发布堆范围 |
| `src/Heap/Heap.cpp` | 定义状态对象、布局断言、phase 双写 |
| `src/Heap/MAllocator/MSRegionInfo.cpp` | Init/Cleanup 发布和清空 Region 数组地址 |
| `src/Heap/MAllocator/MSRegionInfo.h` | 保持/补强与 Compiler ABI 对应的布局断言注释 |
| `src/Heap/MAllocator/MSAllocBuffer.cpp` | 将 AArch64 专属 ABI 注释改为 AArch64/X86 共享契约 |
| `src/Mutator/ThreadLocal.cpp` | 将 offset 注释更新为跨目标 emitter 契约 |
| `src/Heap/MBarrier/MSRefField.inline.h` | 新增 MSGC 专用 raw 64-bit load/store/swap/CAS helper |
| `src/Heap/MBarrier/MSIdleBarrier.h` | 普通及 atomic 引用访问改用 raw helper |
| `src/Heap/MBarrier/MSMarkBarrier.cpp` | old-edge 扫描、写入和原子操作改用 raw helper |
| `src/Heap/MCollector/MarkSweepCollector.cpp` | 对象图、root、weak-ref 扫描改用 raw helper |
| `src/CompilerCalls.cpp` | MSGC 非堆容器直写使用 raw helper；TypeInfo 归一化保持不变 |

### 12.2 LLVM 公共层

| 文件 | 修改 |
|---|---|
| `llvm/include/llvm/CodeGen/CJMSGCABI.h` | 集中稳定状态偏移、size-class 表和 wrapper 分类 |
| `llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp` | 原则上不改；只有目标 hook 接口确有缺口时做最小改动 |

### 12.3 AArch64

| 文件 | 修改 |
|---|---|
| `llvm/lib/Target/AArch64/AArch64CangjieCallSize.h/.cpp` | ELF `-lse` 能力和最大 shadow 估计 |
| `llvm/lib/Target/AArch64/AArch64MSGCEmitter.cpp` | 稳定 C 状态、公共常量、LL/SC helper；保持 raw 64-bit Ref store |
| `llvm/lib/Target/AArch64/AArch64AsmPrinter.h/.cpp` | 仅在 helper/capability 接口需要时调整，不改公共 lowering |

### 12.4 X86

| 文件 | 修改 |
|---|---|
| `llvm/lib/Target/X86/X86AsmPrinter.h` | 分配/数组/屏障 emitter 声明 |
| `llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp` | 让直接 array slow call 也能进入目标 array hook |
| `llvm/lib/Target/X86/X86MCInstLower.cpp` | 允许 MSGC 分配分类进入 X86 hook，保持 CopyGC 分流 |
| `llvm/lib/Target/X86/X86MSGCEmitter.cpp` | ELF/COFF 状态访问、分配、MARK 屏障、Win64 ABI |
| `llvm/lib/Target/X86/CMakeLists.txt` | 现已包含 emitter；实施时只验证不重复添加 |

### 12.5 测试和知识文档

- 修改 AArch64 `cangjie-msgc-elf-capability.ll`、symbol/relocation 和完整屏障测试。
- 新增 AArch64 `-lse` 原子循环、稳定状态 ABI、RefField raw 64-bit 测试。
- 新增 X86 ELF/MinGW COFF 静态与动态分配、MARK Ref、MARK Struct、ArrayCopy MARK
  wrapper 保留、Win64
  参数恢复、COFF relocation 和负向 fallback 测试。
- 更新原来断言 X86 MSGC 数组必须慢调用的
  `msgc-unsupported-target-array-slow-call.ll`。
- 更新 `cangjie-llvm-skill/references/gc-barriers.md` 和 `file-index.md`，记录稳定 C
  状态 ABI、AArch64 LSE/LLSC、X86 ELF/COFF 能力；Runtime 对应知识只补充本次新增 ABI。

## 13. TDD 分阶段实施任务

统一工具路径：

```bash
CJLLVM_ROOT=/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/third_party/llvm-project/llvm
CJLLVM_BUILD=/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/build/build/third_party/cjnative-prefix/src/cjnative-build
```

每个任务严格执行 RED（新断言失败）→ 最小实现 → GREEN（该文件通过）→ 邻近回归。

### Task 1：建立稳定 Runtime C 状态 ABI

**Files:** `src/Heap/MSGCFastPath.h`、`src/Heap/Heap.h/.cpp`、
`src/Heap/MAllocator/MSRegionInfo.cpp`、`CJMSGCABI.h`、AArch64 stable-symbol test。

- [x] 新增 AArch64 Mach-O/ELF 汇编与 object test，要求只引用
  `CJ_MRT_MSGCFastPathState`；运行并确认旧实现因仍引用 C++ mangled name 而失败。
- [x] 增加 Runtime 状态结构、固定偏移 `static_assert` 和三个发布点，旧字段暂时双写。
- [x] 在 `CJMSGCABI.h` 增加状态偏移、size-class 表及配对校验。
- [x] 把 AArch64 phase/heap/region 地址加载迁移到新结构，重新运行新测试至通过。
- [x] 用 `llvm-readobj --relocations` 分别确认 Mach-O/ELF 没有旧 C++ 数据符号 relocation。

### Task 2：锁定 MSGC RefField raw 64-bit 语义和 macOS 基线

**Files:** `MSRefField.inline.h`、`GCRefField.inline.h`、`MSIdleBarrier.h`、
`MSMarkBarrier.cpp`、`MarkSweepCollector.cpp`、`CompilerCalls.cpp`、`Mutator.cpp`、
finalizer/inspector/公共 collector 路径、AArch64/X86 raw-ref 与 Runtime 测试。

- [x] 先添加 Runtime raw helper bit-pattern 测试及 MSGC 路径禁止 generic RefField API 的
  源码契约，确认当前实现失败。
- [x] 新增 MSGC raw helper 和 GC-mode 分派 helper，并替换 MSGC barrier、collector、
  weak-ref、CompilerCalls、栈根枚举、finalizer 和 inspector 路径。
- [x] 添加 AArch64/X86 Ref/StaticRef 汇编检查，确认最终 store 使用完整 64 位参数且前面
  没有 mask/shift；AArch64 当前 store 应作为已有正确行为通过。
- [x] 保留对象头 TypeInfo 的 `UBFM #0,#47` 和 `NormalizeRuntimeTypeInfo`，增加注释区分
  StateWord 解码与 RefField 语义。
- [x] 运行 Mach-O 分配、屏障和 relocation 相关测试，确认除预期符号迁移外无能力回退。
- [x] 检查 CopyGC AArch64/X86 和公共 tagged-pointer 相关测试没有变化。

### Task 3：实现 AArch64 Linux/ELF `-lse`

**Files:** `AArch64CangjieCallSize.h/.cpp`、`AArch64MSGCEmitter.cpp`、
`cangjie-msgc-elf-capability.ll`、新 no-LSE atomic test。

- [x] 把 `-lse` 预期从 wrapper 改为 `ldaxr/stlxr` 和 `ldaxrb/stlxrb`，运行确认 RED。
- [x] 抽取 64 位/8 位 atomic-or helper，LSE 保持原指令，ELF `-lse` 走 LL/SC。
- [x] 更新 capability 和最大 shadow 上界；检查两处使用同一谓词。
- [x] 运行 `+lse/-lse` 的 SMALL、LARGE、same-word pair、different-word pair 测试。
- [x] 对 `-lse` object 反汇编，确认没有 LSE 指令；对 `+lse` 确认没有意外 LL/SC。

### Task 4：打通 X86 ELF/COFF 状态访问和 Ref/StaticRef IDLE 路径

**Files:** `X86AsmPrinter.h`、`X86MCInstLower.cpp`、`X86MSGCEmitter.cpp`、
X86 fastpath/win64/relocation tests。

- [x] 修改 Linux 和 Windows 测试，要求稳定 C 状态访问和 raw 64-bit store；确认 ELF
  仍出现旧 C++ 符号、COFF 仍是 wrapper。
- [x] ELF 使用 GOTPCREL，COFF 使用 `__imp_` IAT，按 SysV/Win64 映射发射两类 IDLE。
- [x] 运行 object relocation 检查；确认 COFF 不是直接读取 IAT slot 的首字节。
- [x] 保持 X86-32、tail call、非 C calling convention 为 wrapper 并运行负向测试。

### Task 5：实现 X86 静态对象和数组分配

**Files:** `X86AsmPrinter.h`、`X86MCInstLower.cpp`、`X86MSGCEmitter.cpp`、
X86 MSGC static-allocation test、原 unsupported-array test。

- [x] 先添加 ELF/COFF 静态对象与数组的 BUMP、PARTIAL、slow label 汇编断言，确认只有
  wrapper call。
- [x] 允许 `MSGCStatic` 分类进入专用 emitter，按 SysV/Win64 参数表实现对象分配。
- [x] 实现数组 header/length、原始 size 慢路径保留和 PARTIAL bitmap 路径；
  用汇编负向断言确认 allocation bitmap 只有普通 load/store，不出现 `lock/cmpxchg`。
- [x] 更新原“X86 数组一定慢调用”测试，只让 X86-32/不支持格式继续慢调用。
- [x] 运行 CopyGC X86 分配测试，确认 CopyGC `NewArrayFast` statepoint 仍走原 emitter，普通 array call 不被新 hook 误接管。

### Task 6：实现 X86 动态对象和数组分配

**Files:** `X86AsmPrinter.h`、`X86MCInstLower.cpp`、`X86MSGCEmitter.cpp`、dynamic allocation tests。

- [x] 添加 0、边界 size class、16384、16385 与慢路径原始 size 保留的 RED 检查。
- [x] 实现 36 级 compare-chain 的 size-class/slot-size 查找，不生成数据表。
- [x] 验证静态/动态对象与数组共存时无重复数据符号。
- [x] 用 MinGW COFF object/relocation 测试确认只有预期的 slow-target relocation。

### Task 7：实现 X86 MARK Ref/StaticRef

**Files:** `X86MSGCEmitter.cpp`、X86 mark-ref 和 WorkBuf tests。

- [x] 添加 phase=MARK 结构检查：heap range、Region 类型、SMALL `lock btsq`、LARGE
  byte cmpxchg、WorkBuf enqueue；确认当前只调用 wrapper。
- [x] 实现旧引用归一化、same-value/null/container/field 校验和稳定状态字段加载。
- [x] 实现 SMALL/LARGE 原子 winner 判定和 has-ref 决策。
- [x] 实现 activeLog null/full 慢路径及新 winner 单次入队。
- [x] 在 ELF 与 COFF 都检查 slow edge 前参数恢复；emitter 本身不改 Win64 RSP。

### Task 8：实现 X86 MARK Struct/StaticStruct

**Files:** `X86MSGCEmitter.cpp`、X86 mark-struct、GCTib 和 pair tests。

- [x] 为短 GCTib、no-ref/RawArray 分流、函数内 marker 和慢路径添加 RED。
- [x] 在原子 mark 前完成 Region/bitmap/WorkBuf 容量检查，并为每个 marker error 边恢复 wrapper 原参数。
- [x] 实现 GCTib 位扫描、函数内逐引用原子 mark、winner 去重和 has-ref 过滤。
- [x] MARK 扫描完成后执行与 wrapper 相同参数的 memcpy；IDLE 保持直接 memcpy。
- [x] 验证 SysV 不使用 callee-saved/red-zone，Win64 只使用 home space 且恢复原参数。

### Task 9：锁定跨平台 ArrayCopy MARK 慢路径

**Files:** `X86MSGCEmitter.cpp`、AArch64/X86 arraycopy phase、overlap 和 MinGW Win64 tests。

- [x] 添加 AArch64 Mach-O/ELF 和 X86 ELF/MinGW COFF 的 phase 分流检查。
- [x] IDLE 保持 `memmove` inline fast path，覆盖重叠区间、零长度和参数重排。
- [x] MARK 必须保留 `CJ_MCC_ArrayCopyRefFast_MSGC`/
  `CJ_MCC_ArrayCopyStructFast_MSGC` wrapper call，不生成内联位图扫描。
- [x] 验证六参数 SysV 和 MinGW Win64 `[rsp+32]/[rsp+40]` 读取；IDLE 快路径和
  MARK wrapper 都不改变原参数语义。

### Task 10：Statepoint、调用约定和 fallback 审计

**Files:** AArch64/X86 statepoint、unsupported-cc、tail-call、32-bit tests。

- [x] 添加“hook 发射前拒绝”和“hook 接管后内部跳 slow”两类断言。
- [x] 检查每个扩展 statepoint 只有一个 stackmap 记录，slow wrapper call 的 PC/stack
  布局仍与原记录相符。
- [x] 检查 X86 所有指令经过 byte counter；检查 AArch64 no-LSE 最大 shadow 不截断。
- [x] 对 X86-32、unsupported ObjectFormat、非 C CC、indirect/tail call 保持原 wrapper。

### Task 11：定向构建和全量相关回归

- [x] 只构建本轮需要的 LLVM 工具：

  ```bash
  ninja -C "$CJLLVM_BUILD" llc FileCheck llvm-readobj llvm-objdump
  ```

- [x] 先逐文件运行新增测试，再运行：

  ```bash
  "$CJLLVM_BUILD/bin/llvm-lit" -sv \
    "$CJLLVM_ROOT/test/CodeGen/AArch64/CangjieGC" \
    "$CJLLVM_ROOT/test/CodeGen/AArch64"/cangjie-msgc-*.ll \
    "$CJLLVM_ROOT/test/CodeGen/X86"/cangjie-msgc-*.ll
  ```

- [x] 扩展运行 GCMode、CJLTO、CJBarrierLowering、CopyGC 分配/屏障相关测试。
- [x] 对四个平台运行代表性汇编和 relocation 检查；未启动 SDK/stdlib 构建。
- [x] Windows 的所有 CodeGen/object 命令显式使用
  `-mtriple=x86_64-w64-windows-gnu`；输出一份可在异机执行的 MinGW Runtime/SDK 验收清单。

### Task 12：文档与知识回写

- [x] 在本节勾选实际完成项，并记录测试数、命令和失败项。
- [x] 更新 LLVM skill 中 barrier/file index；更新 Runtime skill 中稳定状态 ABI。
- [x] 若实现与本文寄存器、offset 或能力矩阵不同，以通过测试的代码为准同步修正文档。
- [x] 明确列出仍为 wrapper 的组合；不得用“已完成跨平台”掩盖未启用子路径。

## 14. 测试与验收标准

### 14.1 能力矩阵验收

| 平台 | 静态/动态对象 | 静态/动态数组 | 六类 IDLE | 四类 MARK Ref/Struct | ArrayCopy MARK | MARK 原子序列 |
|---|---|---|---|---|---|---|
| AArch64 macOS/Mach-O | inline | inline | inline | inline | wrapper | LSE |
| AArch64 Linux/ELF `+lse` | inline | inline | inline | inline | wrapper | LSE |
| AArch64 Linux/ELF `-lse` | inline | inline | inline | inline | wrapper | LL/SC |
| X86-64 Linux/ELF | inline | inline | inline | inline | wrapper | lock bts/cmpxchg |
| X86-64 Windows/MinGW COFF | inline | inline | inline | inline | wrapper | lock bts/cmpxchg |

表中的 inline 表示具备完整快路径和内部 slow edge，不表示每次运行都不会调用 wrapper。
动态输入不合法、Region/WorkBuf 状态不满足和元数据无法证明安全时仍应调用 wrapper。

### 14.2 必须同时满足

- 公共 lowering 对四个平台产生相同 MSGC wrapper，不含架构/ObjectFormat 分支。
- 生成代码只引用 `CJ_MRT_MSGCFastPathState`，不再引用 phase/heap/region 的 C++
  mangled name。
- Mach-O、ELF、COFF object relocation 与各自 ABI 一致；COFF 用
  `x86_64-w64-windows-gnu` 生成且数据经 `__imp_`。
- AArch64 ELF `-lse` 对象中不存在 LSE 指令；`+lse`/Mach-O 不发生能力回退。
- X86 Win64 emitter 不额外改变 RSP，栈参数按 call-site 偏移读取，slow wrapper 参数恢复。
- MSGC RefField 新旧引用均按完整 64 位解释；Struct/ArrayCopy 保持 8 字节槽位复制。
- SMALL/LARGE/pair mark 的 winner 语义和 WorkBuf 单次入队与 Runtime 一致。
- BUMP/PARTIAL allocation bitmap 仅使用普通 load/store，不引入原子 RMW 或锁。
- `ArrayCopyRef/ArrayCopyStruct` 在 MARK 中四个目标组合均保持 wrapper 慢路径。
- CopyGC 测试不引用 MSGC 状态符号，不进入 MSGC emitter；MSGC 测试不进入 CopyGC
  allocation emitter。
- 所有负向组合稳定保留 wrapper，不崩溃、不发生部分发射。
- `git diff --check` 对本轮作用域文件无 whitespace error。

### 14.3 本阶段不宣称的内容

Runtime/SDK 尚未构建时，只能宣称 LLVM 定向构建和 CodeGen 测试通过，不能宣称 dylib
已链接、四平台 SDK 已可用或吞吐率一致。Windows 本机验收仅包含真实 MinGW triple
的 CodeGen、COFF object 和 relocation；Runtime DLL/SDK 真实运行由用户在其他
Windows/MinGW 机器执行。后续集成验证必须分别说明 compiler、runtime、stdlib 来源，
并用对应 SDK 重新编译 `.cj` 用例。

## 15. 风险、回退与兼容性

| 风险 | 防线 | 回退方式 |
|---|---|---|
| Runtime/LLVM 布局漂移 | Runtime `static_assert` + 公共常量 + 汇编检查 | 禁用对应 target hook，走 wrapper |
| COFF 数据符号误用 IAT | relocation + 反汇编检查 | COFF 分类返回 `false` |
| Win64 栈参数偏移错误 | call-site assembly fixture，检查 32/40 | 单独关闭该 wrapper kind |
| X86 寄存器不足破坏 slow args | 每类 live-register 表和 slow-edge test | 该复杂种类保留 wrapper |
| LL/SC 活锁或错误覆盖 | 明确重试 label、byte/word 分开测试 | ELF `-lse` 临时回 wrapper |
| stackmap shadow 低估 | AArch64 最大值、X86 实际字节计数 | 不接管不支持 opcode |
| dynamic compare-chain 边界选择错误 | 36 级阈值与 16384/16385 边界汇编检查 | 动态分配保留 wrapper，静态不受影响 |
| RefField 行为变化 | MSGC raw-helper 单测、源码契约和 AArch64/X86 双回归 | 回退 MSGC helper 调用，不改公共 Runtime 布局 |
| 旧对象仍依赖 C++ 符号 | Runtime 暂时双写旧字段 | 新旧符号并存一个兼容周期 |

所有快路径都有现成 wrapper 作为可执行回退，因此回滚按“目标 + wrapper kind”粒度完成，
不需要回滚公共 intrinsic/lowering。禁止通过 `_MSGC_dynamic` 等符号存在性启发式切换；能力
始终来自显式 GCMode、wrapper 属性、target/ABI/ObjectFormat 和 subtarget feature。

Runtime 新状态符号只在 `MRT_USE_MARK_SWEEP` 构建中定义。CopyGC Runtime 不需要导出它，
因为显式 GCMode 校验禁止 CopyGC module 生成 MSGC wrapper；这也维持两种 Runtime 的宏隔离。

## 16. 执行状态

**状态（2026-07-22）：Runtime/LLVM 实现与本机定向验证完成；Linux x86_64 和 Windows/MinGW Runtime/SDK 真机运行待异机验收。**

- [x] 复核公共 lowering、AArch64/X86 emitter、Runtime 布局和现有测试。
- [x] 明确目标矩阵：AArch64 Linux/macOS、X86-64 Linux/Windows。
- [x] 选择稳定单一 C 状态结构，不使用 Windows C++ 修饰名。
- [x] 明确 AArch64 ELF `-lse` 使用 LL/SC，ISA 差异只留在 emitter。
- [x] 明确 X86 ELF/COFF 的分配、MARK 屏障、Win64 栈参数和回退边界。
- [x] 锁定 ArrayCopy MARK 在所有平台保持慢路径，分配 bitmap 使用非原子
  load/store，Windows 测试使用 `x86_64-w64-windows-gnu` 并由异机完成运行验收。
- [x] 确认 LLVM MSGC Ref store 已是完整 64 位，并把 Runtime MSGC raw 64-bit 统一纳入计划。
- [x] 用户审阅并确认 Step48。
- [x] 完成 Task 2 的 Runtime raw64 实现、RED/GREEN 定向测试与 LLVM 语义注释；汇编回归随 LLVM 定向构建执行。
- [x] 完成 Task 1、3–10 的跨平台实现和负向 fallback 审计。
- [x] 修正 X86 SysV Struct IDLE 路径，`memcpy` 长度从 wrapper 的 `srcSize`
  参数取得；动态 PARTIAL 位图改为显式普通 load、寄存器 `btr` 和普通 store。
- [x] 为 SysV `srcSize` 参数映射及 Linux/MinGW 动态位图普通 load/store 增加
  RED/GREEN CodeGen 回归测试。
- [x] 完成 Runtime ABI 测试/本机 dylib 构建、LLVM 定向构建与回归；未替换 SDK，未构建 stdlib/stdx。

### 16.1 本机验证记录

- Runtime：`bash test/MSGCFastPathStateABIContractTest.sh` 通过；
  `cmake --build CMakebuild --target cangjie-runtime -j8` 通过；macOS dylib
  已导出 `_CJ_MRT_MSGCFastPathState`。
- LLVM 工具：`llc FileCheck llvm-readobj llvm-objdump` 构建通过。
- X86 MSGC/CopyGC：15/15 通过，包含 Linux ELF、`x86_64-w64-windows-gnu`
  assembly/COFF object/IAT relocation、allocation bitmap 无 lock、MARK atomic、
  动态 PARTIAL 显式 load/register-btr/store、SysV Struct `srcSize`、ArrayCopy
  MARK slow、statepoint、非 C CC、tail call 和非法静态属性。
- AArch64 MSGC：18/18 通过；AArch64 `CangjieGC` 目录 13 通过、
  1 个既有 XFAIL，无 unexpected failure。
- CopyGC X86：1/1 通过；`CJRuntimeLowering` + GCMode/CJLTO 定向共
  10/10 通过。
- `git diff --check` 通过。

## 17. Linux x86_64 与 Windows/MinGW 异机验收清单

### 17.1 Linux x86_64

1. 在原生 Linux x86_64 主机上构建包含本轮改动的 compiler 和 MSGC Runtime；记录
   compiler、runtime、stdlib 三件套来源，不得用 macOS 产物代替。
2. 使用 `python3 build.py build --target native --build-type release
   --gc-mode marksweep -v 0.0.1` 构建 Runtime，并确认主机与产物架构均为
   `x86_64`。
3. 将新 Runtime 集成进 Linux SDK 后，重新编译 `.cj` 功能及压力用例；不得复用
   替换 Runtime 前生成的 binary。
4. 功能用例覆盖静态/动态 object 和 array 分配、Ref/StaticRef、
   Struct/StaticStruct、两类 ArrayCopy，并分别覆盖 IDLE 与 MARK；压力用例重点检查
   PARTIAL slot 重复分配、非法 stateWord/typeInfo、WorkBuf 满回退和多轮 GC。
5. 保存构建命令、`uname -m`、Runtime 加载来源、用例重编译命令、stdout/stderr 和
   GC 日志，作为 Linux x86_64 真机验收证据。

### 17.2 Windows/MinGW

1. 确认三件套来源：compiler 必须包含本轮 LLVM 改动，runtime 必须从本
   MSGC Runtime 源码构建，stdlib 记录实际使用的 SDK 版本。
2. 使用 MinGW 工具链构建 Runtime，不得用 MSVC ABI 替代：

   ```bash
   python3 build.py build --target windows-x86_64 --build-type release \
     --gc-mode marksweep -v 0.0.1 --target-toolchain <mingw-w64-toolchain>
   ```

3. 检查 DLL/import library 实际导出 `CJ_MRT_MSGCFastPathState`；若无此导出，
   先检查 Runtime 链接命令是否保留 `--export-all-symbols`，不要运行用例。
4. 用该 compiler 生成一个 MSGC COFF object，确认 target triple 是
   `x86_64-w64-windows-gnu`，且 relocation 含
   `IMAGE_REL_AMD64_REL32 __imp_CJ_MRT_MSGCFastPathState`。
5. 替换 SDK Runtime 后，必须用该 SDK 重新编译所有 `.cj` 用例，不得复用旧 binary。
6. 最小功能用例覆盖：静态/动态 object 和 array 分配、Ref/StaticRef、
   Struct/StaticStruct、两类 ArrayCopy；分别覆盖 IDLE 与 MARK。MARK 中
   ArrayCopy 应正常进 wrapper，不是“未优化”失败。
7. 再做小堆压力和多轮 GC 验证，关注：非法 stateWord/typeInfo、丢失旧边、
   WorkBuf 满时崩溃、PARTIAL Region 重复 slot、Win64 unwind 异常。
8. 记录最终 compiler/runtime/stdlib 路径、构建命令、DLL 导出检查、
   用例重编译命令和运行结果，作为 Step48 真机验收证据。
