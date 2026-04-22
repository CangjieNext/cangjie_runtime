// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef RT_INTERFACE_H
#define RT_INTERFACE_H

#include <stdint.h>
#include <stddef.h>

#include "RuntimeTypes.h"

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

////////////////////////////////////////////////////////////////////////////////////
// region Types
////////////////////////////////////////////////////////////////////////////////////

// Pointer to traceable object reference. Binary layout of object is defined by CJNative runtime.
typedef void *DYN_ObjRefT;

// Pointer to RefField
typedef void *DYN_FieldRefT;

// Pointer to the place that contains traceable object reference.
typedef void *DYN_PlaceholderT;

// State that's passed during visiting stack frames.
typedef void *DYN_VisitingStateT;

// Pointer to TypeTemplate. Binary layout of this structure is defined by CJNative runtime.
typedef void *DYN_TypeTemplateT;

// Pointer to ThreadLocalData that is currently executing current cjThread.
typedef void *DYN_ThreadLocalDataT;

// Pointer to cjThread specific data that is used by interpreter during execution.
// This pointer is stored in cjThread specific storage.
typedef void *DYN_CJThreadSpecificDataT;

// This is alias for frame pointer. CJNative runtime should pass this pointer to `DYN_FrameInfoProviderFn` function.
// Interpreter would return description of corresponding frame (e.g. fileName, lineNumber).
typedef const void *DYN_FramePointerT;

// This is alias for instruction pointer. CJNative runtime should pass this pointer to `DYN_FrameInfoProviderFn` function.
// Interpreter would return description of corresponding frame (e.g. fileName, lineNumber).
typedef const void *DYN_InstructionPointerT; 

// Visitor of interpreter pointer placeholders.
// Interpreter cannot use this data directly but should pass it to the callbacks defined in `DYN_CJNativeInterfaceT`.
typedef const void *DYN_RootVisitorT;

// Visitor of placeholders that contain derived pointers (intrapointers).
// Interpreter cannot use this data directly but should pass it to the callbacks defined in `DYN_CJNativeInterfaceT`.
typedef const void *DYN_DerivedPtrVisitorT;

// Pointer to ExceptionWrapper.
typedef void *DYN_ExceptionWrapperT;

// Frame description used by interpreter frame visitors.
struct DYN_FrameDescT {
    DYN_FramePointerT fp;
    DYN_InstructionPointerT ip;
};

// This struct describes frame. Interpreter would fill this structure in `DYN_FrameInfoProviderFn` function.
struct DYN_InterpretedFrameInfoT {
    size_t lineNumber;
    char*  methodName;
    char*  className;
    char*  fileName;
};

////////////////////////////////////////////////////////////////////////////////////
// endregion Types
////////////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////////////
// region Calling Conventions
////////////////////////////////////////////////////////////////////////////////////

// C Calling convention (CCall)
//   This is default calling convention used by any C/C++ code.
//   Provided callbacks use this calling convention to simplify interoperability of CJNative and interpreter.
//

// Managed Cangjie Calling convention (ManagedCall)
//   This is default calling convention used by compiled Cangjie code. Parameter passing is similar to CCall but there are some
//   differences. For example, there is dedicated non-volatile register that holds pointer to thread local data (e.g. r15 on x86_64).
//   Take care of this calling convention when making C2I or I2C transitions.
//

////////////////////////////////////////////////////////////////////////////////////
// endregion Calling Conventions
////////////////////////////////////////////////////////////////////////////////////


// Collection of callbacks implemented by interpreter. See detailed description below.
struct DYN_InterpreterInterfaceT;

// Collection of callbacks implemented by CJNative. See detailed description below.
struct DYN_CJNativeInterfaceT;


////////////////////////////////////////////////////////////////////////////////////
// region interpreter interface
////////////////////////////////////////////////////////////////////////////////////

// Initalizes runtime and its interface.
//
// params:
// - interpreterInterface - Pointer to `DYN_InterpreterInterfaceT` which will be filled.
// - cjnativeInterface - Pointer to `DYN_CJNativeInterfaceT`. The content will be copied.
// - interpreterArgsCount - Number of interpreter arguments in `interpreterArgs`.
// - interpreterArgs - Interpreter arguments array (null-terminated). 
//                     Pointer is owned by runtime and will not be freed.
// return: 0 on success.
//
typedef int (*DYN_InitRt)(struct DYN_InterpreterInterfaceT *interpreterInterface,
                      struct DYN_CJNativeInterfaceT *cjnativeInterface,
                      int interpreterArgsCount,
                      const char* const* interpreterArgs);

// Prepares state and calls callback with initialized state and provided context. After callback returns, performs necessary cleanup of the state.
//
// params:
// - cjThreadData - pointer to interpreter data for current cjThread.
// - callback - continuation that expects initialized state and context.
// - ctx - context that should be passed to callback.
//
typedef void (*DYN_IterateFramesWithStateFn)(DYN_CJThreadSpecificDataT cjThreadData, void (*callback)(DYN_VisitingStateT, void*), void* ctx);


// Visit all frame slots that contain pointers to stack using provided visitor.
//
// params:
// - state - visiting state that was initialized in iterateFramesWithState callback. This state is passed between consecutive calls to this function for different frames.
// - frameDesc - description of the frame which slots are being visited.
// - stackPtrVisitor - visitor callback provided by CJNative runtime for processing slots with stack pointers.
// - derivedPtrVisitor - derived pointers visitor callback provided by CJNative runtime for processing slots with intrapointers (which point to stack).
//
// Notes:
// Provided visitors cannot be called directly by interpreter. Interpreter should pass visitor to the callbacks defined in `DYN_CJNativeInterfaceT` to process slots.
//
typedef void (*DYN_VisitFrameRootsExpansionFn)(DYN_VisitingStateT state, DYN_FrameDescT frameDesc, DYN_RootVisitorT stackPtrVisitor, DYN_DerivedPtrVisitorT derivedPtrVisitor);


// Visit frame roots (local variables) of interpreted code with marking visitor.
//
// params:
// - state - visiting state that was initialized in iterateFramesWithState callback. This state is passed between consecutive calls to this function for different frames.
// - frameDesc - description of the frame which roots are being visited.
// - rootVisitor - marking root visitor callback provided by CJNative runtime. Interpreter should call this callback for each root it needs to process.
//
// Notes:
// Root visitor cannot be called directly by interpreter. Interpreter should pass root visitor to the callbacks defined in `DYN_CJNativeInterfaceT` to process roots.
//
typedef void (*DYN_VisitFrameRootsMarkingFn)(DYN_VisitingStateT state, DYN_FrameDescT frameDesc, DYN_RootVisitorT rootVisitor);


// Visit frame roots (local variables) of interpreted code with adjusting visitor.
//
// params:
// - state - visiting state that was initialized in iterateFramesWithState callback. This state is passed between consecutive calls to this function for different frames.
// - frameDesc - description of the frame which roots are being visited.
// - rootVisitor - adjusting root visitor callback provided by CJNative runtime. Interpreter should call this callback for each root it needs to process.
// - derivedPtrVisitor - derived pointers visitor callback provided by CJNative runtime.
//
// Notes:
// Root visitors cannot be called directly by interpreter. Interpreter should pass root visitor to the callbacks defined in `DYN_CJNativeInterfaceT` to process roots.
//
typedef void (*DYN_VisitFrameRootsAdjustingFn)(DYN_VisitingStateT state, DYN_FrameDescT frameDesc, DYN_RootVisitorT rootVisitor, DYN_DerivedPtrVisitorT derivedPtrVisitor);


// Visit static roots (global variables) of interpreted code with some visitor.
// Can be used to add interpreted roots for GC root-set or for references adjusting.
//
// params:
// - visitor - root visitor callback provided by CJNative runtime. Interpreter should call this callback for each root it needs to process.
//
// Notes:
// Root visitor cannot be called directly by interpreter. Interpreter should pass root visitor to the callbacks defined in `DYN_CJNativeInterfaceT` to process roots.
//
typedef void (*DYN_VisitGlobalRootsFn)(DYN_RootVisitorT visitor);


// Registers newly created cjThread in interpreter. This method could be called by any thread (e.g. auxiliary scheduler thread) to notify interpreter about creation of new cjThread.
//
// params:
// - Pointer to CJThreadSpecificData
//
// Notes:
//  Interpreter will save auxiliary metadata in [DYN_CJThreadSpecificDataT + offsetToCJThreadSpecificMemory] memory area to simplify support of interpretation and Garbage collection.
//
typedef void (*DYN_CJThreadStartFn)(DYN_CJThreadSpecificDataT*);


// Unregisters cjThread in interpreter. This method could be called by any thread (e.g. auxiliary scheduler thread) to notify interpreter about termination of some completed cjThread.
//
// params:
// - Pointer to CJThreadSpecificData
//
typedef void (*DYN_CJThreadDestroyFn)(DYN_CJThreadSpecificDataT*);


// Provides information about given frame which could be used for precise stack trace generation.
// This method could be called by any thread (e.g. auxiliary stack dumper thread) to query information about interpreter frames.
//
// Important: this function may invoke "long-running" tasks (fseek, pthread_mutex_lock) so CJNative runtime
//            should treat this method as "foreign" code.
//
// params:
// - Frame pointer. Must be provided by CJNative runtime.
// - Instruction pointer in frame (callsite position).
// - Pointer to DYN_InterpretedFrameInfoT. Fields of this struct will be initialized by interpreter.
//
typedef void (*DYN_FrameInfoProviderFn)(DYN_FramePointerT, DYN_InstructionPointerT ip, DYN_InterpretedFrameInfoT *);


// Landing pad which handles pending exception.
// If there is suitable catch block in current last interpreted frame, interprets catch block.
// Otherwise, drops current frame and rethrows exception.
//
typedef void (*DYN_LandingPadFn)();

////////////////////////////////////////////////////////////////////////////////////
// endregion interpreter interface
////////////////////////////////////////////////////////////////////////////////////


////////////////////////////////////////////////////////////////////////////////////
// region CJNative interface
////////////////////////////////////////////////////////////////////////////////////

// Allocate new object.
// params:
// - Pointer to TypeInfo describing the object to be allocated
//
// return: allocated object or NULL
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef DYN_ObjRefT (*DYN_ObjAllocFn)(struct DYN_TypeInfoT*);

// Allocate new array.
// params:
// - arrayType - pointer to TypeInfo describing the array to be allocated
// - size - size of the array to be allocated
//
// return: allocated array object or NULL
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef DYN_ObjRefT (*DYN_ArrayAllocFn)(struct DYN_TypeInfoT* arrayType, uint64_t size);

// Poll a safe point.
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*DYN_SafePointFn)();

// Check if safepoint is pending.
//
// params:
// - threadLocalData - pointer to current `ThreadLocalData`.
//
// return: true if safepoint is pending, false otherwise.
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef bool (*DYN_IsPendingSafePointFn)(DYN_ThreadLocalDataT);

// C2N_Stub function, can be reused as implementation of I2N call.
// TODO avoid using it
typedef void (*DYN_C2NStubFn)();

// Typedef for foreign function. See below for explanation.
typedef void *(*DYN_ForeignFunc)(void*, void*, void*, void*, void*);


// Provide a TypeInfo for type with given signature.
//
// params:
// - type description as char string.
//
// return: pointer to TypeInfo
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef struct DYN_TypeInfoT* (*DYN_TypeInfoProviderFn)(const char *);

// Provide a TypeTemplate for type with given signature.
//
// params:
// - type description as char string.
//
// return: pointer to TypeTemplate
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef DYN_TypeTemplateT (*DYN_TypeTemplateFn)(const char *);

// Get or create specialized TypeInfo from a TypeTemplate and type arguments.
//
// params:
// - typeTemplate - pointer to the TypeTemplate
// - argSize - number of type arguments
// - typeArgs - array of pointers to TypeInfo (type arguments)
//
// return: pointer to the specialized TypeInfo
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef struct DYN_TypeInfoT* (*DYN_GetOrCreateTypeInfoFn)(DYN_TypeTemplateT typeTemplate, uint32_t argSize, struct DYN_TypeInfoT** typeArgs);

// Get the instance size of a type from its TypeInfo.
//
// params:
// - typeInfo - pointer to the TypeInfo
//
// return: size of the instance in bytes
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef uint32_t (*DYN_GetInstanceSizeFn)(struct DYN_TypeInfoT* typeInfo);


// Throw OutOfMemoryError exception.
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*DYN_ThrowOOMFn)();

// Throw given exception.
//
// params:
// - exception object to throw
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*DYN_ThrowExceptionFn)(DYN_ObjRefT exceptionObject);

// Get pending exception instance.
// This method hasn't any side effects (doesn't affect pending status).
//
// return: pointer to pending object
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef DYN_ObjRefT (*DYN_GetPendingExceptionFn)();

// Get and clear pending exception instance.
// Any call to `DYN_GetPendingExceptionFn` after execution of this method should return null.
//
// return: pointer to pending object
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef DYN_ObjRefT (*DYN_GetAndClearPendingExceptionFn)();


// Check that given object is an instance of given type.
//
// params:
// - object which type will be checked
// - pointer to TypeInfo
//
// return: true if object's type is a subtype of `ti`, false otherwise.
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef bool (*DYN_InstanceOfFn)(DYN_ObjRefT obj, struct DYN_TypeInfoT* ti);

// Check that typeInfo is a subtype of superTypeInfo.
//
// params:
// - typeInfo - type to check
// - superTypeInfo - base type
//
// return: true if typeInfo is a subtype of superTypeInfo.
//
// Notes:
//
typedef bool (*DYN_IsSubTypeFn)(struct DYN_TypeInfoT* typeInfo, struct DYN_TypeInfoT* superTypeInfo);

// Applies visitor to the given placeholder.
//
// params:
// - visitor - root visitor callback provided by CJNative runtime. Interpreter should call this callback for each root it needs to process.
// - placeholder - pointer to the place that contains reference which should be visited.
//
typedef void (*DYN_VisitRootFromInterpreterFn)(DYN_RootVisitorT visitor, DYN_PlaceholderT placeholder);

// Applies visitor to the given derived and base pointers.
//
// params:
// - visitor - root visitor callback provided by CJNative runtime.
// - basePtrHolder - pointer to the place that contains base pointer.
// - derivedPtrHolder - pointer to the place that contains derived pointer (intrapointer).
//
typedef void (*DYN_VisitDerivedPtrFromInterpreterFn)(DYN_DerivedPtrVisitorT visitor, DYN_PlaceholderT basePtrHolder, DYN_PlaceholderT derivedPtrHolder);

// Read reference from static field.
//
// params:
// - source - pointer to static reference field
//
// return: object reference stored in the field
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef DYN_ObjRefT (*DYN_ReadStaticFieldFn)(DYN_FieldRefT source);

// Write reference to static field.
//
// params:
// - destination - pointer to static reference field
// - newValue - object reference to store in the field
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*DYN_WriteStaticFieldFn)(DYN_FieldRefT destination, DYN_ObjRefT newValue);

// Read reference from instance field.
//
// params:
// - source - object that contains instance field
// - field - pointer to instance reference field
// 
// return: object reference stored in the field
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef DYN_ObjRefT (*DYN_ReadInstanceFieldFn)(DYN_ObjRefT source, DYN_FieldRefT field);

// Write reference to instance field.
//
// params:
// - destination - object that contains instance field
// - field - pointer to instance reference field
// - newValue - object reference to store in the field
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*DYN_WriteInstanceFieldFn)(DYN_ObjRefT destination, DYN_FieldRefT field, DYN_ObjRefT newValue);

// Read a struct field from an object.
//
// params:
// - dstPtr - pointer to the destination memory
// - obj - object containing the struct field
// - srcField - pointer to the start of the struct
// - size - size of the struct in bytes
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*DYN_ReadStructFieldFn)(uintptr_t dstPtr, DYN_ObjRefT obj, uintptr_t srcField, size_t size);

// Write a struct field to an object.
//
// params:
// - obj - object containing the struct field
// - dst - pointer to the destination memory (start of struct field inside object)
// - src - pointer to the source memory
// - size - size of the struct in bytes
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*DYN_WriteStructFieldFn)(DYN_ObjRefT obj, uintptr_t dst, uintptr_t src, size_t size);

// Read a static struct field.
//
// params:
// - dstPtr - pointer to the destination memory
// - dstSize - size of destination memory
// - srcPtr - pointer to the source static struct
// - srcSize - size of source struct
// - tib - The GCTib value (can be a pointer to StdGCTib or a ShortGCTib bitmap)
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*DYN_ReadStaticStructFieldFn)(uintptr_t dstPtr, size_t dstSize, uintptr_t srcPtr, size_t srcSize, DYN_GCTibT tib);

// Write a static value type field (struct).
//
// params:
// - dst - pointer to the destination static struct
// - dstLen - size of destination struct
// - src - pointer to the source memory
// - srcLen - size of source memory
// - tib - The GCTib value (can be a pointer to StdGCTib or a ShortGCTib bitmap)
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*DYN_WriteStaticStructFieldFn)(uintptr_t dst, size_t dstLen, uintptr_t src, size_t srcLen, DYN_GCTibT tib);

// Read a generic field from an object.
// Should be used if generic type resolves to struct/value at runtime, otherwise use DYN_ReadInstanceFieldFn.
//
// params:
// - dst - pointer to the destination box-object of generic struct/value
// - srcObj - the source object on the heap (can be box of generic struct/value)
// - srcField - address of the field inside srcObj
// - size - size of the data to copy
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*DYN_ReadGenericFieldFn)(DYN_ObjRefT dst, DYN_ObjRefT srcObj, uintptr_t srcField, size_t size);

// Write a generic field to an object.
// Should be used if generic type resolves to struct/value at runtime, otherwise use DYN_WriteInstanceFieldFn.
//
// params:
// - dstObj - destination object (can be box of generic struct/value)
// - dstField - address of the field inside dstObj
// - srcObj - source box-object of generic struct/value
// - size - size of the generic value
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*DYN_WriteGenericFieldFn)(DYN_ObjRefT dstObj, uintptr_t dstField, DYN_ObjRefT srcObj, size_t size);

// Read reference from array.
//
// params:
// - source - RawArray object containing references
// - index - index of the element to read
// return: object reference stored in the array at the specified index
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef DYN_ObjRefT (*DYN_GetArrayRefElementFn)(DYN_ObjRefT source, uint64_t index);

// Write reference to array.
//
// params:
// - source - RawArray object containing references
// - index - index of the element to write
// - newValue - object reference to store
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*DYN_SetArrayRefElementFn)(DYN_ObjRefT destination, uint64_t index, DYN_ObjRefT newValue);

//
// Check state flags of the object.
//
typedef bool (*DYN_IsValidObjectFn)(DYN_ObjRefT object);

//
// Get ThreadLocalData.
//
typedef DYN_ThreadLocalDataT (*DYN_GetThreadLocalDataFn)();

//
// Returns true if the GC is in an "active" phase.
// In active phase fast-path write barriers can`t be used.
//
typedef bool (*DYN_IsActiveGcPhaseFn)(DYN_ThreadLocalDataT);

//
// Stub for stack growth.
//
typedef void* (*DYN_StackGrowStubFn)();

// I2N (interpreter-to-native) entry stub.
//
// This function points to an architecture-specific assembly stub used by the
// interpreter to invoke native code while preserving managed thread and stack in safe state.
//
// Assembly stub expects following arguments:
// params:
// - 5-th architecture-specific ABI integer argument - address of the native function to call.
// - 6-th architecture-specific ABI integer argument - current thread local data of the calling interpreter thread.
//
// return:
// - return value produced by the native callee using architecture-specific ABI.
//
// Notes:
// - The function can be null on platforms that do not provide an I2N stub.
// - The concrete register/stack mapping is architecture-dependent and defined by the corresponding assembly implementation.
typedef void (*DYN_I2NStubFn)();

// Retrieves the current ExceptionWrapper.
//
// return: pointer to ExceptionWrapper
//
typedef DYN_ExceptionWrapperT (*DYN_GetExceptionWrapperFn)();

// Get PC of function which caught exception.
// Can be used as return address for interpreter landing pad.
//
typedef uintptr_t (*DYN_GetCurrentCatchFunctionPcFn)();

// Begins exception catch block using the provided ExceptionWrapper.
//
// params:
// - exceptionWrapper - current exception wrapper
//
// return: exception object
//
typedef DYN_ObjRefT (*DYN_PostThrowExceptionFn)(DYN_ExceptionWrapperT exceptionWrapper);

//
// Native logger function. E.g. hilog can be used to log events in interpreter on OHOS devices.
//
// params:
// - tag - log tag
// - message - log message
//
typedef void (*DYN_NativeLoggerFn)(int logLevel, char* tag, char* message);


////////////////////////////////////////////////////////////////////////////////////
// endregion CJNative interface
////////////////////////////////////////////////////////////////////////////////////


// region Interfaces

struct DYN_InterpreterInterfaceT {
    // current supported version is 1
    int64_t version;

    size_t cjThreadSpecificDataSize;
    size_t iteratorSize;

    uintptr_t c2iStubStartAddr;
    uintptr_t c2iStubEndAddr;

    uintptr_t i2iAdapterStartAddr;
    uintptr_t i2iAdapterEndAddr;

    uintptr_t interpreterPrologueStartAddr;
    uintptr_t interpreterPrologueEndAddr;

    DYN_IterateFramesWithStateFn iterateFramesWithState;
    DYN_VisitFrameRootsExpansionFn visitFrameRootsExpansion;
    DYN_VisitFrameRootsMarkingFn visitFrameRootsMarking;
    DYN_VisitFrameRootsAdjustingFn visitFrameRootsAdjusting;
    DYN_VisitGlobalRootsFn visitGlobalRoots;

    DYN_CJThreadStartFn cjThreadStart;
    DYN_CJThreadDestroyFn cjThreadDestroy;

    DYN_FrameInfoProviderFn frameInfoProvider;
    DYN_LandingPadFn landingPad;
};


struct DYN_CJNativeInterfaceT {
    void* appLibHandle;
    size_t carrierSpecificOffset;
    size_t cjThreadSpecificOffset;

    DYN_TypeInfoProviderFn typeInfo;
    DYN_TypeTemplateFn typeTemplate;
    DYN_GetOrCreateTypeInfoFn getOrCreateTypeInfo;
    DYN_GetInstanceSizeFn getInstanceSize;

    DYN_ObjAllocFn objectAlloc;
    DYN_ArrayAllocFn arrayAlloc;
    DYN_SafePointFn safePoint;
    DYN_IsPendingSafePointFn isPendingSafePoint;

    DYN_ThrowExceptionFn throwException;
    DYN_ThrowOOMFn throwOom;
    DYN_GetPendingExceptionFn getPendingException;
    DYN_GetAndClearPendingExceptionFn getAndClearPendingException;
    DYN_GetExceptionWrapperFn getExceptionWrapper;
    DYN_PostThrowExceptionFn postThrowException;
    DYN_GetCurrentCatchFunctionPcFn getCurrentCatchFunctionPc;

    DYN_InstanceOfFn instanceOf;
    DYN_IsSubTypeFn isSubType;

    DYN_VisitRootFromInterpreterFn visitRootFromInterpreter;
    DYN_VisitDerivedPtrFromInterpreterFn visitDerivedPtrFromInterpreter;

    DYN_ReadStaticFieldFn readStaticField;
    DYN_WriteStaticFieldFn writeStaticField;
    DYN_ReadInstanceFieldFn readInstanceField;
    DYN_WriteInstanceFieldFn writeInstanceField;

    DYN_ReadStructFieldFn readStructField;
    DYN_WriteStructFieldFn writeStructField;
    DYN_ReadStaticStructFieldFn readStaticStructField;
    DYN_WriteStaticStructFieldFn writeStaticStructField;

    DYN_ReadGenericFieldFn readGenericField;
    DYN_WriteGenericFieldFn writeGenericField;

    size_t arrayBodyOffset;
    DYN_GetArrayRefElementFn getArrayRefElement;
    DYN_SetArrayRefElementFn setArrayRefElement;

    DYN_IsValidObjectFn isValidObject;
    DYN_GetThreadLocalDataFn getThreadLocalData;
    DYN_IsActiveGcPhaseFn isActiveGcPhase;
    DYN_StackGrowStubFn stackGrowStub;
    DYN_I2NStubFn i2nStub;

    DYN_NativeLoggerFn nativeLogger;
};

#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus

// endregion Interfaces
#endif
