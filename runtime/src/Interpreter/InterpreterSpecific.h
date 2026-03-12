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
#include "RTInterface.h"
#include "Options.h"
#include "Mutator/Mutator.h"
#include "Common/StackType.h"

#ifdef TEST
#include "TestSupport.h"
#endif

namespace MapleRuntime { // TODO introduce separate namespace for interpreter-related functions  

void VisitInterpreterStackPtrs(root_visitor_t visitor, fiber_specific_data_t interpreterFiberData, stack_ptr_slot_type_t slotType);
void VisitInterpreterFiberRoots(root_visitor_t visitor, fiber_specific_data_t interpreterFiberData);
void VisitInterpreterGlobalRoots(root_visitor_t visitor);
void VisitInterpreterFrameRoots(root_visitor_t visitor, FrameInfo frameInfo);

void InterpreterFiberStart(fiber_specific_data_t* mutator);
void InterpreterFiberDestroy(fiber_specific_data_t* mutator);

bool IsManagedEntrypointAddr(const uint32_t* addr);

void ProvideInterpretedFrameInfo(uint64_t fp, uint64_t ip, interpreted_frame_info_t* fInfo);

type_info_t TypeInfoProvider(char * sig);

obj_ref_t ObjectAllocate(type_info_t tpe);

void SafePoint();

void StackCheck();

void ThrowImplicitException(implicit_exception_type_t type);
obj_ref_t GetPendingException();
obj_ref_t GetAndClearPendingException();
void SetPendingException(obj_ref_t ref);
bool InstanceOf(obj_ref_t ref, type_info_t ti);

void Dummy();

void* ForeignWrapper(foreign_func f, void* p1, void* p2, void* p3, void* p4, void* p5);

void InitInterpreter(const RuntimeParam& runtimeParam);

#ifdef __cplusplus
extern "C" {
#endif

void MRT_InterpretCBCNoArgs(); // TODO: delete

#ifdef __cplusplus
};
#endif

bool IsC2IVirtualExecutorAddr(const uintptr_t addr);
bool IsI2IFunctionAddr(const uintptr_t addr);

uintptr_t GetCommonInterpreterLandingPad();

} // namespace MapleRuntime

#endif // INTERPRETER_ENABLED

#endif // MRT_INTERPRETER_SPECIFIC_H
