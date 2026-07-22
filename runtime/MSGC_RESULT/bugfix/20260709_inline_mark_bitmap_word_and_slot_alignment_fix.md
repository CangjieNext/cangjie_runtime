# 2026-07-09 inline mark bitmap word addressing and region-neighbor fix

## Symptom

- 开启 MSGC inline write barrier 后，`ref/static-ref/struct/static-struct` 共用的 inline mark 路径存在两处风险：
  - mark bitmap 的 word 地址按 `slotIdx >> 3` 计算，和 runtime `markBitmap[wordIdx]` 的 `wordIdx = slotIdx >> 6` 不一致。
  - inline region 解析只看 `(addr - heapStart) >> 15` 的单个 entry，没有对齐 runtime `GetRegionInfoValidated()` 的相邻 entry 修正逻辑。

## Root Cause

### 1. bitmap word 地址计算错误

- runtime `MSRegionInfo::TestAndSetMarkBit()`：
  - `wordIdx = slotIdx / 64`
  - 地址 = `markBitmap + (wordIdx * 8)`
- 旧 inline asm：
  - bit mask 用的是 `1 << (slotIdx & 63)`，这是对的
  - 但 word 地址用了 `markBitmap + (slotIdx >> 3)`，把 `uint64_t[]` bitmap 当成按字节索引了

结果：

- 多数 `slotIdx` 会命中错误 word；
- 很多地址不是 8B 对齐，`LDSETALX` 可能直接触发 `SIGBUS`；
- 即使不崩，也会把 mark bit 写到 runtime/sweep 不读取的位置，导致漏标。

### 2. inline region 解析缺少相邻 entry 修正

- runtime `MSRegionInfoArray::GetRegionInfoValidated()` 在主 entry 不匹配时，还会继续看前后相邻 entry：
  - `prev`
  - `next`
- 旧 inline 路径只看当前 `(addr - heapStart) >> 15` 算出的单个 entry。

结果是：未按 32KB 对齐切分的小对象 region，在边界附近可能被 inline 路径错误 slow-fall，命中率低于 runtime 实际可接受范围。

## Fix

编译器侧 `AArch64AsmPrinter.cpp` 的共享 helper `EmitInlineMarkOrTo()` 修了两件事：

1. bitmap word 地址改为和 runtime 一致：
   - `slotIdx >> 6`
   - `markBitmap + (wordIdx << 3)`
2. 补相邻 entry 修正：
   - 当前 entry 不匹配时，先试 `prev`
   - 再试 `next`
   - 相邻修正只接受非 `MS_REGION_LARGE` entry，并继续复用原有 small/medium mark 路径

## Follow-up

- 早先尝试过一版“由 `divMagic` 反推 `slotSize` 再做 slot 对齐校验”的实现，但它会额外占用 `X16/X17`，而 `struct/static-struct` 循环路径把这两个寄存器作为 heap range 常驻寄存器复用，结果会把后续迭代的 heap 检查改坏。
- 最终版本已移除这段 slotSize 反解/对齐校验逻辑，避免在循环内破坏 `X16/X17`。

## Verification

使用重编后的 `llc` 验证：

```bash
ninja -C /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/build/build/third_party/cjnative-prefix/src/cjnative-build llc

/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/build/build/third_party/cjnative-prefix/src/cjnative-build/bin/llc \
  -mtriple=arm64-apple-macosx12.0.0 -o - \
  /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/third_party/llvm-project/llvm/test/CodeGen/AArch64/cangjie-writebarrier-msgc-fastpath.ll \
  | /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/build/build/third_party/cjnative-prefix/src/cjnative-build/bin/FileCheck \
    /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/third_party/llvm-project/llvm/test/CodeGen/AArch64/cangjie-writebarrier-msgc-fastpath.ll

/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/build/build/third_party/cjnative-prefix/src/cjnative-build/bin/llc \
  -mtriple=arm64-apple-macosx12.0.0 -enable-msgc-inline-write-barrier=false -o - \
  /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/third_party/llvm-project/llvm/test/CodeGen/AArch64/cangjie-writebarrier-msgc-ref-no-inline.ll \
  | /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/build/build/third_party/cjnative-prefix/src/cjnative-build/bin/FileCheck \
    /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/third_party/llvm-project/llvm/test/CodeGen/AArch64/cangjie-writebarrier-msgc-ref-no-inline.ll

/Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/build/build/third_party/cjnative-prefix/src/cjnative-build/bin/llc \
  -mtriple=arm64-apple-macosx12.0.0 -enable-msgc-inline-write-barrier=false -o - \
  /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/third_party/llvm-project/llvm/test/CodeGen/AArch64/cangjie-writebarrier-msgc-struct-no-inline.ll \
  | /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/build/build/third_party/cjnative-prefix/src/cjnative-build/bin/FileCheck \
    /Users/lipeisheng/code/cangjie/cangjie/cangjie_compiler/third_party/llvm-project/llvm/test/CodeGen/AArch64/cangjie-writebarrier-msgc-struct-no-inline.ll
```

关键反汇编片段已变为：

```asm
sub   x9, x14, #64
add   x9, x9, #128
lsr   x15, x15, #6
add   x15, x9, x15, lsl #3
ldsetal x14, x9, [x15]
```
