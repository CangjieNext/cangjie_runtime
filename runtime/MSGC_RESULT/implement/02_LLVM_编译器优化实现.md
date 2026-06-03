# MSGC LLVM 编译器优化实现详解

## 概述

MSGC 相关的 LLVM 优化主要由两部分组成：
1. **`CJBarrierLowering` Pass**（`llvm/lib/CodeGen/CJBarrierLowering.cpp`）：决定把 `CJ_MCC_NewObject` 替换成哪个 wrapper 函数
2. **`AArch64AsmPrinter`**（`llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp`）：在 wrapper 函数体内 inline 生成 fast path 汇编

---

## 一、控制开关

| 开关 | 默认值 | 作用 |
|------|--------|------|
| `enable-gc-fast-path` | `true` | 是否将 NewObject 替换为 fast path wrapper |
| `enable-gc-phase` | `true` | 写屏障是否内联 GC phase 检查（CopyGC 用） |
| `enable-tagged-pointer` | `true` | 读屏障是否内联 tagged pointer 检查（CopyGC 用） |
| `enable-msgc-read-barrier-lowering` | `true` | 是否消除 MSGC 读屏障（SDK1/SDK2 均有此优化） |

**SDK 编译器差异**：

| SDK | `enable-gc-fast-path` | MSGC size-class 特化 | 读屏障消除 |
|-----|-----------------------|---------------------|-----------|
| SDK2（dev260505） | ✅ 开启 | ❌ 无 | ✅ 有 |
| SDK1（dev260507） | ✅ 开启 | ✅ 有（CJ_MCC_NewObjectFast_MSGC_cls{N}_sz{S}） | ✅ 有 |

---

## 二、CJBarrierLowering 中的 NewObject 替换逻辑

### 入口：`doNewFastPath(Function &F, ...)`

在 `runOnFunction` 中，如果 `EnableGCFastPath` 开启，会收集所有 `CJ_MCC_NewObject` 调用点，然后：

```
doNewFastPath(F, News)
  └─ 对每个 GCStatepointInst (CI):
       └─ getMSGCFastPathConfig(CI, SizeClass, SlotSize)
             ├─ 取 CI 的第二个参数（分配大小）是否是编译期常量？
             │    否 → HasMSGCSpecialization = false
             ├─ 大小是否 > 16384（大对象阈值）？
             │    是 → HasMSGCSpecialization = false
             └─ 查 MSGCSizeClasses[] 表，找最小的 >= 大小的 class
                  → SizeClass = class 索引, SlotSize = class 大小
                  → HasMSGCSpecialization = true
       └─ 替换 callee:
             HasMSGCSpecialization = true:
               → "CJ_MCC_NewObjectFast_MSGC_cls{N}_sz{S}"
               → addFnAttr("cj-msgc-sizeclass", N)
               → addFnAttr("cj-msgc-slotsize", S)
             HasMSGCSpecialization = false:
               → "CJ_MCC_NewObjectFast"（通用 wrapper，CopyGC 风格）
```

### Size Class 映射表（与 Runtime 完全一致）

```
8, 16, 24, 32, 48, 64, 80, 96, 112, 128,       // class 0-9
160, 192, 224, 256, 320, 384, 448, 512,          // class 10-17
640, 768, 896, 1024, 1280, 1536, 1792, 2048,     // class 18-25
2560, 3072, 3584, 4096, 5120, 6144, 8192,        // class 26-32
10240, 12288, 16384                               // class 33-35
```

**规则**：分配大小向上取整到最近的 size class。例如，分配 24B 对象（TreeNode）→ class 3（32B），SlotSize=32。

### replaceFastFunc：创建 wrapper 函数

首次遇到某个 wrapper 名字时，在模块中创建同名函数：
```llvm
; 函数体仅是对真正 callee 的透传（作为 statepoint 的 actualy callee）
define internal void @CJ_MCC_NewObjectFast_MSGC_cls3_sz32(typeinfo*, size)
  noinline "cj-msgc-sizeclass"="3" "cj-msgc-slotsize"="32" {
  call CJ_MCC_NewObject(typeinfo, size)
  ret
}
```

该函数本身体是透传，但 `AArch64AsmPrinter` 会识别 `"cj-msgc-sizeclass"` / `"cj-msgc-slotsize"` 属性，在汇编阶段**替换掉这个函数的 body**，内联生成 fast path 汇编。

---

## 三、AArch64AsmPrinter：内联 Fast Path 汇编生成

### 分派逻辑

```cpp
void AArch64AsmPrinter::emitMccNewObjectFastPath(const MachineInstr *MI, ...) {
    MSGCFastAllocConfig Config;
    if (getMSGCFastAllocConfig(MI->getMF()->getFunction(), Config)) {
        emitMccNewObjectForMSGC(Param, Config);  // MSGC size-class 特化路径
    } else {
        emitMccNewObjectForCopyGC(Param);         // CopyGC TLAB 路径（或无特化时的通用路径）
    }
}
```

### CopyGC 路径（`emitMccNewObjectForCopyGC`）

用于 CopyGC SDK，或 MSGC SDK 中**无法确定编译期分配大小**的调用点。

生成的汇编（约 7 条指令）：

```asm
; fast path
ldr  x2, [x28]          ; [TLS+0x00] = AllocBuffer* buffer
ldr  x2, [x2]           ; [buffer+0x00] = RegionInfo* tlRegion（第一个字段）
ldr  x3, [x2]           ; [tlRegion+0x00] = allocPtr (cur)
ldr  x4, [x2, #0x8]     ; [tlRegion+0x08] = regionEnd (end)
add  x5, x3, x1         ; new_ptr = allocPtr + size
cmp  x5, x4
b.gt slow_path           ; new_ptr > end → miss，走 slow path
str  x0, [x3]           ; *allocPtr = typeinfo（写 typeinfo）
str  x5, [x2]           ; tlRegion->allocPtr = new_ptr（更新 cur）
mov  x0, x3             ; 返回旧 allocPtr（对象地址）
b    finish

slow_path:
bl   _CJ_MCC_NewObject  ; 调 runtime slow path
finish:
```

### MSGC 路径（`emitMccNewObjectForMSGC`）

用于 MSGC SDK 中**已知分配大小且 <= 16KB** 的调用点（SDK1）。

生成的汇编（约 60 条指令）：

```asm
; === GC phase 检查 ===
ldrb w9, [x28, #0x4e]         ; [TLS+0x4e] = encodedGCPhase
subs wzr, w9, #MSGCGCPhaseIdle  ; phase == IDLE?
b.eq fast_path                  ; 是 IDLE → 进 fast path
subs wzr, w9, #MSGCGCPhaseSweep ; phase == SWEEP?
b.ne slow_path                  ; 不是 SWEEP → 走 slow path（TRACE/STW 阶段）

fast_path:
; === 获取 msAllocBuffer ===
ldr  x9, [x28, #0x58]          ; [TLS+0x58] = AllocBufferMS* msAllocBuffer
cbz  x9, slow_path              ; null → slow path

; === 获取 activeRegion[cls] ===
ldr  x10, [x9, #activeRegions_offset + cls*8]
cbz  x10, slow_path             ; 无活跃 Region → slow path

movz x15, #SlotSize             ; slot 大小（编译期常量）

; === 判断 Region 类型 ===
ldrb w11, [x10, #regionType_offset]
subs wzr, w11, #MS_REGION_BUMP
b.eq bump_path                  ; BUMP 模式
subs wzr, w11, #MS_REGION_PARTIAL
b.ne slow_path                  ; 不是 PARTIAL → slow path

; === Partial Region：bitmap CTZ 分配 ===
partial_retry:
ldr  x12, [x10, #reservedBitmap_offset]  ; 预载 reserved bitmap word
cbz  x12, slow_path
rbit x13, x12                   ; 反转，找最低位空闲 slot
clz  x13, x13                   ; slot index = CLZ(RBIT(word))
ldr  x14, [x10, #allocBitmap_offset]     ; allocBitmap base
add  x14, x14, x13, lsl #3     ; 目标 bitmap word 地址
ldr  x16, [x14]                 ; 读 bitmap word
cbz  x16, partial_repair        ; bitmap word 全 0 → 修复
rbit x17, x16
clz  x17, x17                   ; bit index
movz x11, #1
lsl  x11, x11, x17
bic  x16, x16, x11              ; 清对应 bit（标记为已分配）
str  x16, [x14]                 ; 写回 bitmap
ldrh w11, [x10, #freeSlotCount_offset]
sub  w11, w11, #1
strh w11, [x10, #freeSlotCount_offset]  ; freeSlotCount--
; 计算地址
...
str  x0, [obj_addr]             ; 写 typeinfo
mov  x0, obj_addr               ; 返回对象地址
b    finish

partial_repair:
; 修复 reservedBitmap 不一致，重试
bic  x12, x12, x11
str  x12, [x10, #reservedBitmap_offset]
b    partial_retry

bump_path:
; === BUMP 模式 ===
ldr  x12, [x10]                 ; allocPtr
ldr  x14, [x10, #0x8]           ; regionEnd
add  x13, x12, x15              ; new_ptr = allocPtr + slotSize
cmp  x13, x14
b.gt slow_path                  ; 超出 region → slow path
str  x0, [x12]                  ; *allocPtr = typeinfo
str  x13, [x10]                 ; region->allocPtr = new_ptr
ldrh w11, [x10, #freeSlotCount_offset]
sub  w11, w11, #1
strh w11, [x10, #freeSlotCount_offset]
mov  x0, x12                    ; 返回地址
b    finish

slow_path:
bl   _CJ_MCC_NewObject          ; runtime slow path

finish:
```

---

## 四、读屏障消除优化（`EnableMSGCReadBarrierLowering`）

MSGC 使用 **写屏障**（SATB + 转移屏障），读屏障相对少。`EnableMSGCReadBarrierLowering` 开启后，`CJBarrierLowering` 会消除不必要的 `cj_gcread_ref` / `cj_gcread_struct` 等读屏障 intrinsic，直接替换为普通 load。

SDK1 和 SDK2 均包含此优化。

---

## 五、编译器优化流程总结

```
前端（Cangjie → IR）
  └─ 所有 new 操作 → GCStatepointInst 调 CJ_MCC_NewObject

CJBarrierLowering Pass（CodeGen 前）
  └─ EnableGCFastPath=true:
       ├─ 分配大小为编译期常量 && <= 16KB（MSGC 特化）
       │    → 替换为 CJ_MCC_NewObjectFast_MSGC_cls{N}_sz{S}
       │      + addFnAttr("cj-msgc-sizeclass", N)
       └─ 其他（动态大小 / 大对象）
            → 替换为 CJ_MCC_NewObjectFast

AArch64AsmPrinter（汇编生成）
  └─ 遇到 CJ_MCC_NewObjectFast* 函数定义时:
       ├─ 有 "cj-msgc-sizeclass" 属性 → emitMccNewObjectForMSGC（~60条指令）
       └─ 无属性 → emitMccNewObjectForCopyGC（~7条指令）
```

---

## 六、Wrapper 函数命名规则

| 函数名 | 场景 |
|--------|------|
| `CJ_MCC_NewObjectFast` | 通用 CopyGC TLAB 路径 |
| `CJ_MCC_NewFinalizerFast` | 带 finalizer 对象的通用路径 |
| `CJ_MCC_NewObjectFast_MSGC_cls3_sz32` | MSGC 特化，class 3，32B slot |
| `CJ_MCC_NewFinalizerFast_MSGC_cls3_sz32` | 带 finalizer 的 MSGC 特化 |
| `CJ_MCC_NewObject` | 无 fast path，直接调 runtime |

---

## 七、相关源码文件

| 文件 | 作用 |
|------|------|
| `llvm/lib/CodeGen/CJBarrierLowering.cpp` | NewObject 替换逻辑、开关定义、size class 表 |
| `llvm/lib/Target/AArch64/AArch64AsmPrinter.cpp` | fast path 汇编 inline 生成 |
| `llvm/lib/Target/AArch64/AArch64InstrInfo.cpp` | `hasMSGCFastAllocConfig` 辅助判断 |
