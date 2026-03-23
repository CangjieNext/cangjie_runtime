// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.


#ifndef MRT_INTERPRETER_SPECIFIC_H
#define MRT_INTERPRETER_SPECIFIC_H

#ifdef INTERPRETER_ENABLED

#include <stdio.h>
#include <cstdlib>
#include "Interpreter/RTInterface.h"
#include "Options.h"
#include "Mutator/Mutator.h"
#include "Common/StackType.h"


#ifdef __cplusplus
using namespace MRTExport;
#endif

namespace MapleRuntime { // TODO introduce separate namespace for interpreter-related functions  

void IterateFramesWithState(fiber_specific_data_t fiberData, void (*callback)(visiting_state_t, void*), void* ctx);

void VisitInterpreterGlobalRoots(root_visitor_t visitor);

void VisitInterpreterFrameRootsExpansion(visiting_state_t state, FrameInfo frameDesc, root_visitor_t stackPtrVisitor, root_visitor_t derivedPtrVisitor);
void VisitInterpreterFrameRootsMarking(visiting_state_t state, FrameInfo frameDesc, root_visitor_t rootVisitor);
void VisitInterpreterFrameRootsAdjusting(visiting_state_t state, FrameInfo frameDesc, root_visitor_t rootVisitor, root_visitor_t derivedPtrVisitor);


void InterpreterFiberStart(fiber_specific_data_t* mutator);
void InterpreterFiberDestroy(fiber_specific_data_t* mutator);

void ProvideInterpretedFrameInfo(uint64_t fp, uint64_t ip, interpreted_frame_info_t* fInfo);

struct type_info_t* TypeInfoProvider(char * sig);

obj_ref_t ObjectAllocate(struct type_info_t* tpe);

void SafePoint();

bool IsPendingSafePoint(thread_local_data_t* tlData);

obj_ref_t GetPendingException();
obj_ref_t GetAndClearPendingException();
bool InstanceOf(obj_ref_t ref, struct type_info_t* ti);

void InitInterpreter(const RuntimeParam& runtimeParam);

bool IsC2IStubAddr(const uintptr_t addr);
bool Isi2iAdapterStartAddr(const uintptr_t addr);
bool IsI2NStubAddr(const uintptr_t addr);

uintptr_t GetCommonInterpreterLandingPad();

} // namespace MapleRuntime

#endif // INTERPRETER_ENABLED

#endif // MRT_INTERPRETER_SPECIFIC_H
