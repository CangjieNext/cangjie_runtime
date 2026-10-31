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

// Sentinel value appended to the end of the fillInStackTrace pc triples to tag the "creator frame" (placed in the funcStart slot).
constexpr uint64_t kThreadCreatorSentinel = UINT64_C(0xC9C9C9C9C9C9C9C9);

// Fixed-size buffers for the function / file name in the record. The function name holds the demangled `pkg::method` text.
constexpr uint16_t THREAD_CREATOR_NAME_CAPACITY = 512;
constexpr uint16_t THREAD_CREATOR_FILE_CAPACITY = 128;

// Creator record, attached to the child cjthread's keyed localData slot (see GetThreadCreatorKey).
// Contains only native text and numbers: no managed references, no cross-thread sharing (parent writes, child reads; written before publish).
struct ThreadCreatorRecord {
    char name[THREAD_CREATOR_NAME_CAPACITY];      // demangled pkg::method, NUL-terminated
    uint16_t nameLength;
    char fileName[THREAD_CREATOR_FILE_CAPACITY];  // file name only, NUL-terminated; empty = no file info
    uint16_t fileNameLength;
    uint32_t lineNumber;                          // 0 = no line number
};

// Returns (registering on first call) the localData key; its destructor frees the ThreadCreatorRecord.
CJThreadKey GetThreadCreatorKey();

// Returns the current thread's creator record; nullptr when absent (main/CFFI threads or failed capture).
ThreadCreatorRecord* GetCurrentThreadCreator();

// Captures the creator on the parent thread at the MCC entries: skips runtime/stub frames
// and takes the first MANAGED frame, decoding the FuncDesc function/file name and the
// StackMap line number. On success returns a calloc'd record (freed by the key destructor
// when the child's localData is released); returns nullptr on failure (thread creation unchanged).
ThreadCreatorRecord* CaptureCreator();

} // namespace MapleRuntime

#ifdef __cplusplus
extern "C" {
#endif

// Runtime-internal creation helper used only by the two MCC entries.
// Not in the public schedule.h; attaches the creator record to the child's localData before publish.
void *CJThreadNewWithCreator(void *schedule, const void *attrUser, void *func,
                                 const void *argStart, unsigned int argSize,
                                 unsigned int createSource,
                                 MapleRuntime::ThreadCreatorRecord *creatorRecord);

#ifdef __cplusplus
}
#endif

#endif // MRT_THREAD_CREATOR_INFO_H
