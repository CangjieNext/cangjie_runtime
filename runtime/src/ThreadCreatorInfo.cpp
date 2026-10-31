// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "ThreadCreatorInfo.h"

#include <cstdlib>
#include <cstring>
#include <mutex>

#include "Common/StackType.h"
#include "Mutator/Mutator.h"
#include "UnwindStack/PrintStackInfo.h"
#include "UnwindStack/StackMetadataHelper.h"

namespace MapleRuntime {

namespace {

// localData slot key. Registered by CJThreadKeyCreateInner on the first GetThreadCreatorKey call;
// the FreeCreatorRecord destructor frees the record. Stays invalid until registered (getters return nullptr).
CJThreadKey gCreatorKey = static_cast<CJThreadKey>(-1);
std::once_flag gCreatorKeyOnce;

void FreeCreatorRecord(void* value)
{
    std::free(value);
}

void EnsureCreatorKey()
{
    std::call_once(gCreatorKeyOnce, [] {
        (void)CJThreadKeyCreateInner(&gCreatorKey, FreeCreatorRecord);
    });
}

} // namespace

CJThreadKey GetThreadCreatorKey()
{
    EnsureCreatorKey();
    return gCreatorKey;
}

ThreadCreatorRecord* GetCurrentThreadCreator()
{
    EnsureCreatorKey();
    return static_cast<ThreadCreatorRecord*>(CJThreadGetspecificInner(gCreatorKey));
}

ThreadCreatorRecord* CaptureCreator()
{
    // The parent thread's unwind context must be RELIABLE, otherwise give up (thread creation unchanged).
    Mutator* mutator = Mutator::GetMutator();
    if (mutator == nullptr ||
        mutator->GetUnwindContext().GetUnwindContextStatus() != UnwindContextStatus::RELIABLE) {
        return nullptr;
    }

    // Reuse the existing unwind: FillInStackTrace collects only MANAGED frames,
    // so stack[0] is the nearest Cangjie user frame above the MCC entry (the spawn creator).
    PrintStackInfo stackInfo;
    stackInfo.FillInStackTrace();
    std::vector<FrameInfo>& stack = stackInfo.GetStack();
    if (stack.empty()) {
        return nullptr;
    }
#ifdef INTERPRETER_ENABLED
    // Interpreter frames are not laid out like MANAGED frames; StackMetadataHelper would
    // dereference a bogus FuncDesc. Skip the capture in interpreter builds for now.
    if (stack[0].GetFrameType() != FrameType::MANAGED) {
        return nullptr;
    }
#endif

    // Reuse StackMetadataHelper to decode name/file/line (same path as the exception stack).
    StackMetadataHelper metadata(stack[0]);
    MangleNameHelper* helper = metadata.GetMangleNameHelper();
    if (helper == nullptr) {
        return nullptr;
    }
    helper->Demangle();
    CString pkgCls = helper->GetPackClassName();
    CString meth = helper->GetMethodName();
    CString funcName = pkgCls.IsEmpty() ? meth : (pkgCls + "::" + meth);
    if (funcName.IsEmpty()) {
        funcName = helper->GetMangleName();
    }
    if (funcName.IsEmpty()) {
        return nullptr;
    }

    ThreadCreatorRecord* record =
        static_cast<ThreadCreatorRecord*>(std::calloc(1, sizeof(ThreadCreatorRecord)));
    if (record == nullptr) {
        return nullptr;
    }

    size_t nameLen = static_cast<size_t>(funcName.Length());
    if (nameLen >= THREAD_CREATOR_NAME_CAPACITY) {
        nameLen = THREAD_CREATOR_NAME_CAPACITY - 1;
    }
    std::memcpy(record->name, funcName.Str(), nameLen);
    record->name[nameLen] = '\0';
    record->nameLength = static_cast<uint16_t>(nameLen);

    CString funcFile = metadata.GetFileName();
    if (!funcFile.IsEmpty()) {
        size_t fileLen = static_cast<size_t>(funcFile.Length());
        if (fileLen >= THREAD_CREATOR_FILE_CAPACITY) {
            fileLen = THREAD_CREATOR_FILE_CAPACITY - 1;
        }
        std::memcpy(record->fileName, funcFile.Str(), fileLen);
        record->fileName[fileLen] = '\0';
        record->fileNameLength = static_cast<uint16_t>(fileLen);
    }

    record->lineNumber = metadata.GetLineNumber();
    return record;
}

} // namespace MapleRuntime