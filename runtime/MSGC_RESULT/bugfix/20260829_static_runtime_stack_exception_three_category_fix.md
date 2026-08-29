# Linux x86 静态 MSGC 栈与异常路径三类问题修复记录

## 1. 修复范围

本次处理 MSGC Linux x86 静态 runtime 的编译模式、静态地址范围和 OOM 栈根三类问题。

## 2. 基线与证据

记录原始失败用例、复现命令、错误日志和源码证据。

## 3. 分类一：MSGC 编译模式一致性

记录 CopyGC/MSGC ABI 不一致的原因、修改和验证结果。

## 4. 分类二：静态 runtime 地址范围

记录 runtime section、静态 linker 范围和栈/异常误分类问题的修复。

## 5. 分类三：OOM 栈根与 RBX

记录 `exception_0006` 的 StackMap、寄存器保存和 emergency GC 修复。

## 6. 构建与验证

记录 SDK 来源、构建产物、重新编译规则和六个静态用例的逐项结果。

## 7. 修复结论与剩余风险

总结已解决问题、未通过项和后续风险。
