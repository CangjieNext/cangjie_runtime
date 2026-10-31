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

// localData 槽 key。首次 GetThreadCreatorKey 时由 CJThreadKeyCreateInner 注册，
// 析构器 FreeCreatorRecord 释放 record。未注册时保持无效值（getter 会返回 nullptr）。
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
    // 父线程当前 unwind 上下文必须可靠，否则放弃（不改变线程创建）。
    Mutator* mutator = Mutator::GetMutator();
    if (mutator == nullptr ||
        mutator->GetUnwindContext().GetUnwindContextStatus() != UnwindContextStatus::RELIABLE) {
        return nullptr;
    }

    // 复用现有回栈：PrintStackInfo::FillInStackTrace 只收集 MANAGED 帧，
    // stack[0] 即 MCC 入口之上最近的仓颉用户帧（spawn 创建者）。
    PrintStackInfo stackInfo;
    stackInfo.FillInStackTrace();
    std::vector<FrameInfo>& stack = stackInfo.GetStack();
    if (stack.empty()) {
        return nullptr;
    }

    // 复用 StackMetadataHelper 统一解析名称/文件/行号（与异常栈打印同一套）。
    StackMetadataHelper metadata(stack[0]);
    MangleNameHelper* helper = metadata.GetMangleNameHelper();
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