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
namespace MRTExport {
#endif // __cplusplus

////////////////////////////////////////////////////////////////////////////////////
// region Types
////////////////////////////////////////////////////////////////////////////////////

// Pointer to traceable object reference. Binary layout of object is defined by CJNative runtime.
typedef void *ObjRefT;

// Pointer to RefField
typedef void *FieldRefT;

// Pointer to the place that contains traceable object reference.
typedef void *PlaceholderT;

// State that's passed during visiting stack frames.
typedef void *VisitingStateT;

// Pointer to TypeTemplate. Binary layout of this structure is defined by CJNative runtime.
typedef void *TypeTemplateT;

// Pointer to ThreadLocalData that is currently executing current fiber.
typedef void *ThreadLocalDataT;

// Pointer to fiber specific data that is used by interpreter during execution.
// This pointer is stored in fiber specific storage.
typedef void *FiberSpecificDataT;

// This is alias for frame pointer. CJNative runtime should pass this pointer to `FrameInfoProviderFn` function.
// Interpreter would return description of corresponding frame (e.g. fileName, lineNumber).
typedef const void *FramePointerT;

// This is alias for instruction pointer. CJNative runtime should pass this pointer to `FrameInfoProviderFn` function.
// Interpreter would return description of corresponding frame (e.g. fileName, lineNumber).
typedef const void *InstructionPointerT; 

// Visitor of interpreter pointer placeholders.
// Interpreter cannot use this data directly but should pass it to the callbacks defined in `CJNativeInterfaceT`.
typedef const void *RootVisitorT;

// Visitor of placeholders that contain derived pointers (intrapointers).
// Interpreter cannot use this data directly but should pass it to the callbacks defined in `CJNativeInterfaceT`.
typedef const void *DerivedPtrVisitorT;

// Pointer to ExceptionWrapper.
typedef void *ExceptionWrapperT;

// Frame description used by interpreter frame visitors.
struct FrameDescT {
    FramePointerT fp;
    InstructionPointerT ip;
};

// This struct describes frame. Interpreter would fill this structure in `FrameInfoProviderFn` function.
struct InterpretedFrameInfoT {
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
struct InterpreterInterfaceT;

// Collection of callbacks implemented by CJNative. See detailed description below.
struct CJNativeInterfaceT;


////////////////////////////////////////////////////////////////////////////////////
// region interpreter interface
////////////////////////////////////////////////////////////////////////////////////

// Initalizes runtime and its interface.
//
// params:
// - interpreterInterface - Pointer to `InterpreterInterfaceT` which will be filled.
// - cjnativeInterface - Pointer to `CJNativeInterfaceT`. The content will be copied.
// - interpreterArgsCount - Number of interpreter arguments in `interpreterArgs`.
// - interpreterArgs - Interpreter arguments array (null-terminated).
// return: 0 on success.
//
typedef int (*InitRt)(struct InterpreterInterfaceT *interpreterInterface,
                      struct CJNativeInterfaceT *cjnativeInterface,
                      int interpreterArgsCount,
                      char **interpreterArgs);

// Prepares state and calls callback with initialized state and provided context. After callback returns, performs necessary cleanup of the state.
//
// params:
// - fiberData - pointer to interpreter data for current fiber.
// - callback - continuation that expects initialized state and context.
// - ctx - context that should be passed to callback.
//
typedef void (*IterateFramesWithStateFn)(FiberSpecificDataT fiberData, void (*callback)(VisitingStateT, void*), void* ctx);


// Visit all frame slots that contain pointers to stack using provided visitor.
//
// params:
// - state - visiting state that was initialized in iterateFramesWithState callback. This state is passed between consecutive calls to this function for different frames.
// - frameDesc - description of the frame which slots are being visited.
// - stackPtrVisitor - visitor callback provided by CJNative runtime for processing slots with stack pointers.
// - derivedPtrVisitor - derived pointers visitor callback provided by CJNative runtime for processing slots with intrapointers (which point to stack).
//
// Notes:
// Provided visitors cannot be called directly by interpreter. Interpreter should pass visitor to the callbacks defined in `CJNativeInterfaceT` to process slots.
//
typedef void (*VisitFrameRootsExpansionFn)(VisitingStateT state, FrameDescT frameDesc, RootVisitorT stackPtrVisitor, DerivedPtrVisitorT derivedPtrVisitor);


// Visit frame roots (local variables) of interpreted code with marking visitor.
//
// params:
// - state - visiting state that was initialized in iterateFramesWithState callback. This state is passed between consecutive calls to this function for different frames.
// - frameDesc - description of the frame which roots are being visited.
// - rootVisitor - marking root visitor callback provided by CJNative runtime. Interpreter should call this callback for each root it needs to process.
//
// Notes:
// Root visitor cannot be called directly by interpreter. Interpreter should pass root visitor to the callbacks defined in `CJNativeInterfaceT` to process roots.
//
typedef void (*VisitFrameRootsMarkingFn)(VisitingStateT state, FrameDescT frameDesc, RootVisitorT rootVisitor);


// Visit frame roots (local variables) of interpreted code with adjusting visitor.
//
// params:
// - state - visiting state that was initialized in iterateFramesWithState callback. This state is passed between consecutive calls to this function for different frames.
// - frameDesc - description of the frame which roots are being visited.
// - rootVisitor - adjusting root visitor callback provided by CJNative runtime. Interpreter should call this callback for each root it needs to process.
// - derivedPtrVisitor - derived pointers visitor callback provided by CJNative runtime.
//
// Notes:
// Root visitors cannot be called directly by interpreter. Interpreter should pass root visitor to the callbacks defined in `CJNativeInterfaceT` to process roots.
//
typedef void (*VisitFrameRootsAdjustingFn)(VisitingStateT state, FrameDescT frameDesc, RootVisitorT rootVisitor, DerivedPtrVisitorT derivedPtrVisitor);


// Visit static roots (global variables) of interpreted code with some visitor.
// Can be used to add interpreted roots for GC root-set or for references adjusting.
//
// params:
// - visitor - root visitor callback provided by CJNative runtime. Interpreter should call this callback for each root it needs to process.
//
// Notes:
// Root visitor cannot be called directly by interpreter. Interpreter should pass root visitor to the callbacks defined in `CJNativeInterfaceT` to process roots.
//
typedef void (*VisitGlobalRootsFn)(RootVisitorT visitor);


// Registers newly created fiber in interpreter. This method could be called by any thread (e.g. auxiliary scheduler thread) to notify interpreter about creation of new fiber.
//
// params:
// - Pointer to FiberSpecificData
//
// Notes:
//  Interpreter will save auxiliary metadata in [FiberSpecificDataT + offsetToFiberSpecificMemory] memory area to simplify support of interpretation and Garbage collection.
//
typedef void (*FiberStartFn)(FiberSpecificDataT*);


// Unregisters fiber in interpreter. This method could be called by any thread (e.g. auxiliary scheduler thread) to notify interpreter about termination of some completed fiber.
//
// params:
// - Pointer to FiberSpecificData
//
typedef void (*FiberDestroyFn)(FiberSpecificDataT*);


// Provides information about given frame which could be used for precise stack trace generation.
// This method could be called by any thread (e.g. auxiliary stack dumper thread) to query information about interpreter frames.
//
// Important: this function may invoke "long-running" tasks (fseek, pthread_mutex_lock) so CJNative runtime
//            should treat this method as "foreign" code.
//
// params:
// - Frame pointer. Must be provided by CJNative runtime.
// - Instruction pointer in frame (callsite position).
// - Pointer to InterpretedFrameInfoT. Fields of this struct will be initialized by interpreter.
//
typedef void (*FrameInfoProviderFn)(FramePointerT, InstructionPointerT ip, InterpretedFrameInfoT *);


// Landing pad which handles pending exception.
// If there is suitable catch block in current last interpreted frame, interprets catch block.
// Otherwise, drops current frame and rethrows exception.
//
typedef void (*LandingPadFn)();

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
typedef ObjRefT (*ObjAllocFn)(struct TypeInfoT*);

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
typedef ObjRefT (*ArrayAllocFn)(struct TypeInfoT* arrayType, uint64_t size);

// Poll a safe point.
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*SafePointFn)();

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
typedef bool (*IsPendingSafePointFn)(ThreadLocalDataT);

// C2N_Stub function, can be reused as implementation of I2N call.
// TODO avoid using it
typedef void (*C2NStubFn)();

// Typedef for foreign function. See below for explanation.
typedef void *(*ForeignFunc)(void*, void*, void*, void*, void*);


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
typedef struct TypeInfoT* (*TypeInfoProviderFn)(const char *);

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
typedef TypeTemplateT (*TypeTemplateFn)(const char *);

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
typedef struct TypeInfoT* (*GetOrCreateTypeInfoFn)(TypeTemplateT typeTemplate, uint32_t argSize, struct TypeInfoT** typeArgs);

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
typedef uint32_t (*GetInstanceSizeFn)(struct TypeInfoT* typeInfo);


// Throw OutOfMemoryError exception.
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*ThrowOOMFn)();

// Throw given exception.
//
// params:
// - exception object to throw
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*ThrowExceptionFn)(ObjRefT exceptionObject);

// Get pending exception instance.
// This method hasn't any side effects (doesn't affect pending status).
//
// return: pointer to pending object
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef ObjRefT (*GetPendingExceptionFn)();

// Get and clear pending exception instance.
// Any call to `GetPendingExceptionFn` after execution of this method should return null.
//
// return: pointer to pending object
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef ObjRefT (*GetAndClearPendingExceptionFn)();


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
typedef bool (*InstanceOfFn)(ObjRefT obj, struct TypeInfoT* ti);

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
typedef bool (*IsSubTypeFn)(struct TypeInfoT* typeInfo, struct TypeInfoT* superTypeInfo);

// Applies visitor to the given placeholder.
//
// params:
// - visitor - root visitor callback provided by CJNative runtime. Interpreter should call this callback for each root it needs to process.
// - placeholder - pointer to the place that contains reference which should be visited.
//
typedef void (*VisitRootFromInterpreterFn)(RootVisitorT visitor, PlaceholderT placeholder);

// Applies visitor to the given derived and base pointers.
//
// params:
// - visitor - root visitor callback provided by CJNative runtime.
// - basePtrHolder - pointer to the place that contains base pointer.
// - derivedPtrHolder - pointer to the place that contains derived pointer (intrapointer).
//
typedef void (*VisitDerivedPtrFromInterpreterFn)(DerivedPtrVisitorT visitor, PlaceholderT basePtrHolder, PlaceholderT derivedPtrHolder);

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
typedef ObjRefT (*ReadStaticFieldFn)(FieldRefT source);

// Write reference to static field.
//
// params:
// - destination - pointer to static reference field
// - newValue - object reference to store in the field
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*WriteStaticFieldFn)(FieldRefT destination, ObjRefT newValue);

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
typedef ObjRefT (*ReadInstanceFieldFn)(ObjRefT source, FieldRefT field);

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
typedef void (*WriteInstanceFieldFn)(ObjRefT destination, FieldRefT field, ObjRefT newValue);

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
typedef void (*ReadStructFieldFn)(uintptr_t dstPtr, ObjRefT obj, uintptr_t srcField, size_t size);

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
typedef void (*WriteStructFieldFn)(ObjRefT obj, uintptr_t dst, uintptr_t src, size_t size);

// Read a static struct field.
//
// params:
// - dstPtr - pointer to the destination memory
// - dstSize - size of destination memory
// - srcPtr - pointer to the source static struct
// - srcSize - size of source struct
// - tibValue - The GCTib value (can be a pointer to TypeInfo or a raw ShortGCTib bitmap)
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*ReadStaticStructFieldFn)(uintptr_t dstPtr, size_t dstSize, uintptr_t srcPtr, size_t srcSize, size_t tibValue);

// Write a static value type field (struct).
//
// params:
// - dst - pointer to the destination static struct
// - dstLen - size of destination struct
// - src - pointer to the source memory
// - srcLen - size of source memory
// - tibValue - The GCTib value (can be a pointer to TypeInfo or a raw ShortGCTib bitmap)
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*WriteStaticStructFieldFn)(uintptr_t dst, size_t dstLen, uintptr_t src, size_t srcLen, size_t tibValue);

// Read a generic field from an object.
// Should be used if generic type resolves to struct/value at runtime, otherwise use ReadInstanceFieldFn.
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
typedef void (*ReadGenericFieldFn)(ObjRefT dst, ObjRefT srcObj, uintptr_t srcField, size_t size);

// Write a generic field to an object.
// Should be used if generic type resolves to struct/value at runtime, otherwise use WriteInstanceFieldFn.
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
typedef void (*WriteGenericFieldFn)(ObjRefT dstObj, uintptr_t dstField, ObjRefT srcObj, size_t size);

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
typedef ObjRefT (*GetArrayRefElementFn)(ObjRefT source, uint64_t index);

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
typedef void (*SetArrayRefElementFn)(ObjRefT destination, uint64_t index, ObjRefT newValue);

//
// Check state flags of the object.
//
typedef bool (*IsValidObjectFn)(ObjRefT object);

//
// Get ThreadLocalData.
//
typedef ThreadLocalDataT (*GetThreadLocalDataFn)();

//
// Returns true if the GC is in an "active" phase.
// In active phase fast-path write barriers can`t be used.
//
typedef bool (*IsActiveGcPhaseFn)(ThreadLocalDataT);

//
// Stub for stack growth.
//
typedef void* (*StackGrowStubFn)();

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
typedef void (*I2NStubFn)();

// Retrieves the current ExceptionWrapper.
//
// return: pointer to ExceptionWrapper
//
typedef ExceptionWrapperT (*GetExceptionWrapperFn)();

// Get PC of function which caught exception.
// Can be used as return address for interpreter landing pad.
//
typedef uintptr_t (*GetCurrentCatchFunctionPcFn)();

// Begins exception catch block using the provided ExceptionWrapper.
//
// params:
// - exceptionWrapper - current exception wrapper
//
// return: exception object
//
typedef ObjRefT (*PostThrowExceptionFn)(ExceptionWrapperT exceptionWrapper);

//
// Native logger function. E.g. hilog can be used to log events in interpreter on OHOS devices.
//
// params:
// - tag - log tag
// - message - log message
//
typedef void (*NativeLoggerFn)(int logLevel, char* tag, char* message);


////////////////////////////////////////////////////////////////////////////////////
// endregion CJNative interface
////////////////////////////////////////////////////////////////////////////////////


// region Interfaces

struct InterpreterInterfaceT {
    // current supported version is 1
    int64_t version;

    size_t fiberSpecificDataSize;
    size_t iteratorSize;

    uintptr_t c2iStubStartAddr;
    uintptr_t c2iStubEndAddr;

    uintptr_t i2iAdapterStartAddr;
    uintptr_t i2iAdapterEndAddr;

    uintptr_t i2nStubStartAddr;
    uintptr_t i2nStubEndAddr;

    IterateFramesWithStateFn iterateFramesWithState;
    VisitFrameRootsExpansionFn visitFrameRootsExpansion;
    VisitFrameRootsMarkingFn visitFrameRootsMarking;
    VisitFrameRootsAdjustingFn visitFrameRootsAdjusting;
    VisitGlobalRootsFn visitGlobalRoots;

    FiberStartFn fiberStart;
    FiberDestroyFn fiberDestroy;

    FrameInfoProviderFn frameInfoProvider;
    LandingPadFn landingPad;
};


struct CJNativeInterfaceT {
    size_t carrierSpecificOffset;
    size_t fiberSpecificOffset;

    TypeInfoProviderFn typeInfo;
    TypeTemplateFn typeTemplate;
    GetOrCreateTypeInfoFn getOrCreateTypeInfo;
    GetInstanceSizeFn getInstanceSize;

    ObjAllocFn objectAlloc;
    ArrayAllocFn arrayAlloc;
    SafePointFn safePoint;
    IsPendingSafePointFn isPendingSafePoint;

    ThrowExceptionFn throwException;
    ThrowOOMFn throwOom;
    GetPendingExceptionFn getPendingException;
    GetAndClearPendingExceptionFn getAndClearPendingException;
    GetExceptionWrapperFn getExceptionWrapper;
    PostThrowExceptionFn postThrowException;
    GetCurrentCatchFunctionPcFn getCurrentCatchFunctionPc;

    InstanceOfFn instanceOf;
    IsSubTypeFn isSubType;

    VisitRootFromInterpreterFn visitRootFromInterpreter;
    VisitDerivedPtrFromInterpreterFn visitDerivedPtrFromInterpreter;

    ReadStaticFieldFn readStaticField;
    WriteStaticFieldFn writeStaticField;
    ReadInstanceFieldFn readInstanceField;
    WriteInstanceFieldFn writeInstanceField;

    ReadStructFieldFn readStructField;
    WriteStructFieldFn writeStructField;
    ReadStaticStructFieldFn readStaticStructField;
    WriteStaticStructFieldFn writeStaticStructField;

    ReadGenericFieldFn readGenericField;
    WriteGenericFieldFn writeGenericField;

    size_t arrayBodyOffset;
    GetArrayRefElementFn getArrayRefElement;
    SetArrayRefElementFn setArrayRefElement;

    IsValidObjectFn isValidObject;
    GetThreadLocalDataFn getThreadLocalData;
    IsActiveGcPhaseFn isActiveGcPhase;
    StackGrowStubFn stackGrowStub;
    I2NStubFn i2nStub;

    NativeLoggerFn nativeLogger;
};

#ifdef __cplusplus
} // namespace MRTExport
} // extern "C"
#endif // __cplusplus

// endregion Interfaces
#endif

