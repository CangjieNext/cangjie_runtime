// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_THREAD_CREATOR_INFO_H
#define MRT_THREAD_CREATOR_INFO_H

#include <cstdint>
#include <cstddef>

#include "schedule.h" // CJThreadKey / CJThreadKeyCreateInner / CJThreadGetspecificInner

namespace MapleRuntime {

// 追加到 fillInStackTrace pc 三元组末尾、标记“创建者帧”的哨兵值（放在 funcStart 位）。
constexpr uint64_t kThreadCreatorSentinel = UINT64_C(0xC9C9C9C9C9C9C9C9);

// 记录里的函数名 / 文件名定长缓冲。函数名存 demangle 后的 `pkg::method` 文本。
constexpr uint16_t THREAD_CREATOR_NAME_CAPACITY = 512;
constexpr uint16_t THREAD_CREATOR_FILE_CAPACITY = 128;

// 创建者记录。挂在子线程 cjthread 的 keyed localData 槽上（见 GetThreadCreatorKey）。
// 只含原生文本与数值，无 managed 引用、无跨线程共享（父写子读，发布前写）。
struct ThreadCreatorRecord {
    char name[THREAD_CREATOR_NAME_CAPACITY];      // demangle 后的 pkg::method，NUL 结尾
    uint16_t nameLength;
    char fileName[THREAD_CREATOR_FILE_CAPACITY];  // 仅文件名，NUL 结尾；空 = 无文件信息
    uint16_t fileNameLength;
    uint32_t lineNumber;                          // 0 = 无行号
};

// 返回（首次调用会注册）localData key；析构器释放 ThreadCreatorRecord。
CJThreadKey GetThreadCreatorKey();

// 取当前线程的创建者记录；无（主线程/CFFI/未采集成功）返回 nullptr。
ThreadCreatorRecord* GetCurrentThreadCreator();

// 父线程在 MCC 入口回栈采集创建者：跳过 runtime/stub 帧，取首个 MANAGED 帧，
// 解码 FuncDesc 函数名/文件名与 StackMap 行号。成功返回 calloc 的 record（调用方在
// 子线程 localData 释放时由 key 析构器 free），失败返回 nullptr（不改变线程创建）。
ThreadCreatorRecord* CaptureCreator();

} // namespace MapleRuntime

#ifdef __cplusplus
extern "C" {
#endif

// Runtime-internal creation helper used only by the two MCC entries.
// 不放进公共 schedule.h；把创建者记录在发布前挂到子线程 localData。
void *CJThreadNewWithCreator(void *schedule, const void *attrUser, void *func,
                                 const void *argStart, unsigned int argSize,
                                 unsigned int createSource,
                                 MapleRuntime::ThreadCreatorRecord *creatorRecord);

#ifdef __cplusplus
}
#endif

#endif // MRT_THREAD_CREATOR_INFO_H
