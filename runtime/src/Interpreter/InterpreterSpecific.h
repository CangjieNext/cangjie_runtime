// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_INTERPRETER_SPECIFIC_H
#define MRT_INTERPRETER_SPECIFIC_H

#include "Options.h"
#ifdef INTERPRETER_ENABLED

#include <stdio.h>
#include <cstdlib>
#include "Interpreter/RTInterface.h"
#include "Mutator/Mutator.h"
#include "Common/StackType.h"


#ifdef __cplusplus
using namespace MRTExport;
#endif

namespace MapleRuntime { // TODO introduce separate namespace for interpreter-related functions  

void IterateFramesWithState(CJThreadSpecificDataT cjThreadData, void (*callback)(VisitingStateT, void*), void* ctx);

void VisitInterpreterGlobalRoots(RootVisitorT visitor);

void VisitInterpreterFrameRootsExpansion(VisitingStateT state, FrameInfo frameDesc, RootVisitorT stackPtrVisitor, RootVisitorT derivedPtrVisitor);
void VisitInterpreterFrameRootsMarking(VisitingStateT state, FrameInfo frameDesc, RootVisitorT rootVisitor);
void VisitInterpreterFrameRootsAdjusting(VisitingStateT state, FrameInfo frameDesc, RootVisitorT rootVisitor, RootVisitorT derivedPtrVisitor);


void InterpreterCJThreadStart(CJThreadSpecificDataT* mutator);
void InterpreterCJThreadDestroy(CJThreadSpecificDataT* mutator);

void FillInterpretedFrameInfo(uint64_t fp, uint64_t ip, InterpretedFrameInfoT* fInfo);

struct TypeInfoT* TypeInfoProvider(char * sig);

ObjRefT ObjectAllocate(struct TypeInfoT* tpe);

void SafePoint();

bool IsPendingSafePoint(ThreadLocalDataT* tlData);

ObjRefT GetPendingException();
ObjRefT GetAndClearPendingException();
bool InstanceOf(ObjRefT ref, struct TypeInfoT* ti);

void InitInterpreter(const RuntimeParam& runtimeParam);

bool IsC2IStubAddr(const uintptr_t addr);
bool IsI2IAdapterStartAddr(const uintptr_t addr);

uintptr_t GetCommonInterpreterLandingPad();

} // namespace MapleRuntime

#endif // INTERPRETER_ENABLED

#endif // MRT_INTERPRETER_SPECIFIC_H
