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


namespace MapleRuntime { // TODO introduce separate namespace for interpreter-related functions  

void IterateFramesWithState(DYN_CJThreadSpecificDataT cjThreadData, void (*callback)(DYN_VisitingStateT, void*), void* ctx);

void VisitInterpreterGlobalRoots(DYN_RootVisitorT visitor);

void VisitInterpreterFrameRootsExpansion(DYN_VisitingStateT state, FrameInfo frameDesc, DYN_RootVisitorT stackPtrVisitor, DYN_RootVisitorT derivedPtrVisitor);
void VisitInterpreterFrameRootsMarking(DYN_VisitingStateT state, FrameInfo frameDesc, DYN_RootVisitorT rootVisitor);
void VisitInterpreterFrameRootsAdjusting(DYN_VisitingStateT state, FrameInfo frameDesc, DYN_RootVisitorT rootVisitor, DYN_RootVisitorT derivedPtrVisitor);


void InterpreterCJThreadStart(DYN_CJThreadSpecificDataT* mutator);
void InterpreterCJThreadDestroy(DYN_CJThreadSpecificDataT* mutator);

void FillInterpretedFrameInfo(uint64_t fp, uint64_t ip, DYN_InterpretedFrameInfoT* fInfo);

struct DYN_TypeInfoT* TypeInfoProvider(char * sig);

DYN_ObjRefT ObjectAllocate(struct DYN_TypeInfoT* tpe);

void SafePoint();

bool IsPendingSafePoint(DYN_ThreadLocalDataT* tlData);

DYN_ObjRefT GetPendingException();
DYN_ObjRefT GetAndClearPendingException();
bool InstanceOf(DYN_ObjRefT ref, struct DYN_TypeInfoT* ti);

void InitInterpreter(const InterpreterParam& interpreterParam);

bool IsC2IStubAddr(const uintptr_t addr);
bool IsI2IAdapterStartAddr(const uintptr_t addr);
bool IsInterpreterPrologueAddr(const uintptr_t addr);

uintptr_t GetCommonInterpreterLandingPad();

} // namespace MapleRuntime

#endif // INTERPRETER_ENABLED

#endif // MRT_INTERPRETER_SPECIFIC_H
