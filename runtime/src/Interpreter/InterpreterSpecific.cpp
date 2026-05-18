// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
#include "InterpreterSpecific.h"
#ifdef INTERPRETER_ENABLED

#include "dlfcn.h"
#include <stdint.h>
#include <cstring>
#include "Interpreter/RTInterface.h"
#include "ObjectManager.inline.h"
#include "ExceptionManager.inline.h"
#include "Mutator/Mutator.h"
#include "Mutator/ThreadLocal.h"
#include "Base/LogFile.h"
#include "Common/ScopedObjectAccess.h"
#include "Common/BaseObject.h"
#include "Common/TypeDef.h"
#include "Heap/Barrier/Barrier.h"
#include "TypeInfoManager.h"
#include "ObjectModel/MArray.h"
#include "ObjectModel/MClass.h"
#include "Heap/Collector/Collector.h"
#include "StackManager.h"
#include "LoaderManager.h"

namespace MapleRuntime {

extern "C" void CJ_MCC_StackGrowStub(...);
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
extern "C" void CJ_MCC_I2NStub(...);
#endif

struct {
    bool initialized = false;
    INT_InterpreterInterface interpreterInterface;

    void initialize(INT_InterpreterInterface& value)
    {
        interpreterInterface = value;
        initialized = true;
    }

    operator bool() { return initialized; }

    INT_InterpreterInterface* operator->()
    {
        if (initialized) {
            return &interpreterInterface;
        }
        LOG(RTLOG_FATAL, "Usage of uninitialized interpreter interface\n");
        return nullptr;
    }

} interpreterInterfaceInstance;

void FillInterpretedFrameInfo(uint64_t fp, uint64_t ip, DYN_InterpretedFrameInfo* fInfo)
{
    auto framePointer = reinterpret_cast<DYN_FramePointer>(fp);
    auto instructionPointer = reinterpret_cast<DYN_InstructionPointer>(ip);

    DLOG(INTERPRETER, "FillInterpretedFrameInfo(fp=%p, ip=%p)", framePointer, instructionPointer);

    if (interpreterInterfaceInstance) {
        interpreterInterfaceInstance->frameInfoProvider(framePointer, instructionPointer, fInfo);
    }
}

void IterateFramesWithState(DYN_CJThreadSpecificData cjThreadData, void (*callback)(DYN_VisitingState, void*), void* ctx)
{
    if (interpreterInterfaceInstance) {
        interpreterInterfaceInstance->iterateFramesWithState(cjThreadData, callback, ctx);
    }
}

DYN_FrameDesc toFrameDesc(FrameInfo frameInfo)
{
    return {
        .fp = frameInfo.GetMachineFrame().GetFA(),
        .ip = frameInfo.GetMachineFrame().GetIP(),
    };
}

void VisitInterpreterFrameRootsExpansion(DYN_VisitingState state, FrameInfo frameDesc, DYN_RootVisitor stackPtrVisitor, DYN_DerivedPtrVisitor derivedPtrVisitor)
{
    if (interpreterInterfaceInstance) {
        DYN_FrameDesc fd = toFrameDesc(frameDesc);
        interpreterInterfaceInstance->visitFrameRootsExpansion(state, fd, stackPtrVisitor, derivedPtrVisitor);
    }
}

void VisitInterpreterFrameRootsMarking(DYN_VisitingState state, FrameInfo frameDesc, DYN_RootVisitor rootVisitor)
{
    if (interpreterInterfaceInstance) {
        DYN_FrameDesc fd = toFrameDesc(frameDesc);
        interpreterInterfaceInstance->visitFrameRootsMarking(state, fd, rootVisitor);
    }
}

void VisitInterpreterFrameRootsAdjusting(DYN_VisitingState state, FrameInfo frameDesc, DYN_RootVisitor rootVisitor, DYN_DerivedPtrVisitor derivedPtrVisitor)
{
    if (interpreterInterfaceInstance) {
        DYN_FrameDesc fd = toFrameDesc(frameDesc);
        interpreterInterfaceInstance->visitFrameRootsAdjusting(state, fd, rootVisitor, derivedPtrVisitor);
    }
}

void VisitInterpreterGlobalRoots(DYN_RootVisitor visitor)
{
    if (interpreterInterfaceInstance) {
        interpreterInterfaceInstance->visitGlobalRoots(visitor);
    }
}

void InterpreterCJThreadStart(DYN_CJThreadSpecificData* dataField)
{
    if (interpreterInterfaceInstance) {
        interpreterInterfaceInstance->cjThreadStart(dataField);
    }
}

void InterpreterCJThreadDestroy(DYN_CJThreadSpecificData* dataField)
{
    if (interpreterInterfaceInstance) {
        interpreterInterfaceInstance->cjThreadDestroy(dataField);
    }
}

void VisitRootFromInterpreter(DYN_RootVisitor _visitor, DYN_Placeholder _placeholder)
{
    const RootVisitor* visitor = static_cast<const RootVisitor*>(_visitor);
    ObjectRef* placeholder = static_cast<ObjectRef*>(_placeholder);

    DLOG(INTERPRETER, "VisitRootFromInterpreter: visitor=[%p], placeholder=[%p]", (void*)visitor, (void*)placeholder);

    (*visitor)(*placeholder);
}

void VisitDerivedPtrFromInterpreter(DYN_DerivedPtrVisitor _visitor, DYN_Placeholder _basePtrHolder, DYN_Placeholder _derivedPtrHolder)
{
    const DerivedPtrVisitor* visitor = static_cast<const DerivedPtrVisitor*>(_visitor);
    BasePtrType* basePtrLocation = static_cast<BasePtrType*>(_basePtrHolder);
    DerivedPtrType* derivedPtrLocation = static_cast<DerivedPtrType*>(_derivedPtrHolder);

    DLOG(INTERPRETER, "VisitDerivedPtrFromInterpreter: visitor=[%p], placeholder=[%p]", (void*)visitor, (void*) derivedPtrLocation);

    (*visitor)(*basePtrLocation, *derivedPtrLocation);
}

struct DYN_TypeInfo* TypeInfoProvider(const char * sig)
{
    DLOG(INTERPRETER, "TypeInfoProvider: %s", sig);
    auto *result = reinterpret_cast<struct DYN_TypeInfo*>(LoaderManager::GetInstance()->FindTypeInfoFromLoadedFiles(sig));
    DLOG(INTERPRETER, "FindTypeInfoFromLoadedFiles(%s) return %p", sig, result);

    return result;
}

DYN_TypeTemplate FindTypeTemplate(const char* mangledName)
{
    DLOG(INTERPRETER, "FindTypeTemplate: %s", mangledName);
    DYN_TypeTemplate result = LoaderManager::GetInstance()->FindTypeTemplateFromLoadedFiles(mangledName);
    DLOG(INTERPRETER, "FindTypeTemplateFromLoadedFiles(%s) return %p", mangledName, result);

    return result;
}

struct DYN_TypeInfo* GetOrCreateTypeInfo(DYN_TypeTemplate typeTemplate, uint32_t typeArgsCount, struct DYN_TypeInfo** typeArgs)
{
    DLOG(INTERPRETER, "GetOrCreateTypeInfo: template=%p size=%u", typeTemplate, typeArgsCount);

    return reinterpret_cast<struct DYN_TypeInfo*>(TypeInfoManager::GetTypeInfoManager().GetOrCreateTypeInfo(
        static_cast<TypeTemplate*>(typeTemplate),
        typeArgsCount,
        reinterpret_cast<TypeInfo**>(typeArgs)
    ));
}

uint32_t GetInstanceSize(struct DYN_TypeInfo* typeInfoPtr)
{
    TypeInfo* ti = reinterpret_cast<TypeInfo*>(typeInfoPtr);
    DLOG(INTERPRETER, "GetInstanceSize: %s", ti->GetName());
    return ti->GetInstanceSize();
}

DYN_ObjRef ObjectAllocate(struct DYN_TypeInfo* tpe)
{
    TypeInfo* typeInfo = reinterpret_cast<TypeInfo*>(tpe);
    MSize size = AlignUp<MSize>(typeInfo->GetInstanceSize(), 8) + sizeof(TypeInfo*);

    ObjRef obj = ObjectManager::NewObject(typeInfo, size);

    DLOG(INTERPRETER, "ObjectAllocate: type=%p (%s), addr=%p, size=%u", tpe, typeInfo->GetName(), obj, size);

    return (DYN_ObjRef)obj;
}

DYN_ObjRef ArrayAllocate(struct DYN_TypeInfo* tpe, uint64_t size)
{
    TypeInfo* typeInfo = reinterpret_cast<TypeInfo*>(tpe);

    ArrayRef arr = ObjectManager::NewArray(size, typeInfo);
    DLOG(INTERPRETER, "ArrayAllocate: type=%p, addr=%p, size=%u", tpe, arr, size);

    return (DYN_ObjRef)arr;
}

void SafePoint()
{
    DLOG(INTERPRETER, "SafePoint");

    // TODO: reduce gc-point overhead
    ScopedEnterSaferegion gcpoint(true);
}

bool IsPendingSafePoint(DYN_ThreadLocalData tlData)
{
    if (tlData == nullptr) {
        LOG(RTLOG_ERROR, "[Interpreter] IsPendingSafePoint called with null DYN_ThreadLocalData");
        return false;
    }
    ThreadLocalData* threadLocalData = reinterpret_cast<ThreadLocalData*>(tlData);
    return threadLocalData->safepointState != 0;
}

void ThrowException(DYN_ObjRef exception)
{
    ExceptionRef exceptionRef = static_cast<ExceptionRef>(exception);
    const char* exceptionTypeName =
        (exceptionRef != nullptr && exceptionRef->GetTypeInfo() != nullptr) ? exceptionRef->GetTypeInfo()->GetName()
                                                                             : "<null>";
    DLOG(INTERPRETER, "ThrowException: exception=%p, type=%s", exceptionRef, exceptionTypeName);
    ExceptionManager::ThrowException(exceptionRef);
}

void ThrowOutOfMemoryError()
{
    DLOG(INTERPRETER, "ThrowOutOfMemoryError");
    ExceptionManager::OutOfMemory();
    ExceptionManager::ThrowPendingException();
}

DYN_ObjRef GetPendingException()
{
    ExceptionRef ref = ExceptionManager::GetPendingException();

    DLOG(INTERPRETER, "GetPendingException: %p", ref);
    return static_cast<DYN_ObjRef>(ref);
}

DYN_ObjRef GetAndClearPendingException()
{
    auto ref = GetPendingException();

    ExceptionManager::ClearPendingException();
    DLOG(INTERPRETER, "GetAndClearPendingException: %p", ref);

    return ref;
}

bool InstanceOf(DYN_ObjRef ref, struct DYN_TypeInfo* ti)
{
    DLOG(INTERPRETER, "InstanceOf: ref=%p, typeInfo=%p", ref, ti);

    MObject* obj    = static_cast<MObject*>(ref);
    TypeInfo* tinfo = reinterpret_cast<TypeInfo*>(ti);

    DLOG(INTERPRETER, "  obj type is %s", obj->GetTypeInfo()->GetName());
    DLOG(INTERPRETER, "  type to check is %s", tinfo->GetName());

    return obj->IsSubType(*tinfo);
}

bool IsSubType(struct DYN_TypeInfo* typeInfo, struct DYN_TypeInfo* super_type_info)
{
    DLOG(INTERPRETER, "IsSubType: ti=%p, sti=%p", typeInfo, super_type_info);
    if (typeInfo == nullptr || super_type_info == nullptr) {
        return false;
    }
    if (typeInfo == super_type_info) {
        return true;
    }

    TypeInfo* tInfo = reinterpret_cast<TypeInfo*>(typeInfo);
    TypeInfo* sInfo = reinterpret_cast<TypeInfo*>(super_type_info);
    return tInfo->IsSubType(sInfo);
}

DYN_ObjRef ReadStaticField(DYN_FieldRef source)
{
    DLOG(INTERPRETER, "ReadStaticField %p", source);

    BaseObject* res = Heap::GetBarrier().ReadStaticRef(*static_cast<RefField<false>*>(source));
    return static_cast<DYN_ObjRef>(res);
}

void WriteStaticField(DYN_FieldRef destination, DYN_ObjRef new_value)
{
    DLOG(INTERPRETER, "WriteStaticField %p %p", destination, new_value);

    Heap::GetBarrier().WriteStaticRef(*static_cast<RefField<false>*>(destination), static_cast<BaseObject*>(new_value));
}

DYN_ObjRef ReadInstanceField(DYN_ObjRef source, DYN_FieldRef field)
{
    DLOG(INTERPRETER, "ReadInstanceField %p %p", source, field);

    RefField<false>& ref = *static_cast<RefField<false>*>(field); // TODO support RefField<true>
    BaseObject* res = Heap::GetBarrier().ReadReference(static_cast<BaseObject*>(source), ref);
    return static_cast<DYN_ObjRef>(res);
}

void WriteInstanceField(DYN_ObjRef destination, DYN_FieldRef field, DYN_ObjRef new_value)
{
    DLOG(INTERPRETER, "WriteInstanceField %p %p %p", destination, field, new_value);
    RefField<false>& ref = *static_cast<RefField<false>*>(field); // TODO support RefField<true>
    Heap::GetBarrier().WriteReference(static_cast<BaseObject*>(destination), ref, static_cast<BaseObject*>(new_value));
}

DYN_ObjRef GetArrayElement(DYN_ObjRef source, U64 index)
{
    DLOG(INTERPRETER, "GetArrayElement %p %llu", source, index);
    MArray* array = static_cast<MArray*>(source);
    return static_cast<DYN_ObjRef>(array->GetRefElement(index));
}


void SetArrayElement(DYN_ObjRef source, U64 index, DYN_ObjRef new_value)
{
    DLOG(INTERPRETER, "SetArrayElement %p %llu %p", source, index, new_value);
    MArray* array = static_cast<MArray*>(source);
    array->SetRefElement(index, static_cast<BaseObject*>(new_value));
}

void ReadStructFieldOfObject(MAddress dstPtr, DYN_ObjRef obj, MAddress srcField, size_t size) {
    DLOG(INTERPRETER, "ReadStructFieldOfObject %p %p %p %zu", dstPtr, obj, srcField, size);
    if (size == 0) {
        return;
    }
    Heap::GetHeap().GetBarrier().ReadStruct(dstPtr, static_cast<BaseObject*>(obj), srcField, size);
}

void WriteStructFieldOfObject(DYN_ObjRef obj, MAddress dst, MAddress src, size_t size)
{
    DLOG(INTERPRETER, "WriteStructFieldOfObject %p %p %p %zu", obj, dst, src, size);
    CHECK_DETAIL((dst != 0u && src != 0u), "MCC_WriteStructField wrong parameter, dst: %p src: %p", dst, src);
    if (UNLIKELY(!Heap::IsHeapAddress(obj))) {
        CHECK_DETAIL(memcpy_s(reinterpret_cast<void*>(dst), size, reinterpret_cast<void*>(src), size) == EOK,
                     "memcpy_s failed");
        return;
    }
    Heap::GetBarrier().WriteStruct(static_cast<BaseObject*>(obj), dst, size, src, size);
}

void ReadStaticStructField(uintptr_t dstPtr, size_t dstSize, uintptr_t srcPtr, size_t srcSize, DYN_GCTib tib)
{
    DLOG(INTERPRETER, "ReadStaticStructField dst=%p size=%zu src=%p size=%zu tib_val=%zx",
        dstPtr, dstSize, srcPtr, srcSize, static_cast<size_t>(tib.raw));

    GCTib gcTib = *reinterpret_cast<GCTib*>(&tib);
    Heap::GetHeap().GetBarrier().ReadStaticStruct(dstPtr, srcPtr, dstSize, gcTib);
}

void WriteStaticStructField(uintptr_t dst, size_t dstLen, uintptr_t src, size_t srcLen, DYN_GCTib tib)
{
    DLOG(INTERPRETER, "WriteStaticStructField dst=%p size=%zu src=%p size=%zu tib_val=%zx",
        dst, dstLen, src, srcLen, static_cast<size_t>(tib.raw));

    CHECK_DETAIL((dst != 0u && src != 0u), "WriteStaticStructField wrong parameter, dst: %p src: %p", dst, src);

    GCTib gcTib = *reinterpret_cast<GCTib*>(&tib);
    Heap::GetBarrier().WriteStaticStruct(dst, dstLen, src, srcLen, gcTib);
}

void ReadGenericField(DYN_ObjRef dst, DYN_ObjRef srcObj, uintptr_t srcField, size_t size)
{
    DLOG(INTERPRETER, "ReadGenericField dst=%p srcObj=%p srcField=%p size=%zu", dst, srcObj, srcField, size);

    if (size == 0) {
        return;
    }

    Heap::GetBarrier().ReadGeneric(
        static_cast<BaseObject*>(dst),
        static_cast<BaseObject*>(srcObj),
        reinterpret_cast<void*>(srcField),
        size
    );
}

void WriteGenericField(DYN_ObjRef dstObj, uintptr_t dstField, DYN_ObjRef src, size_t size)
{
    DLOG(INTERPRETER, "WriteGenericField dstObj=%p dstField=%p src=%p size=%zu", dstObj, dstField, src, size);

    if (src == nullptr || size == 0) {
        return;
    }

    Heap::GetBarrier().WriteGeneric(
        static_cast<BaseObject*>(dstObj),
        reinterpret_cast<void*>(dstField),
        static_cast<BaseObject*>(src),
        size
    );
}

bool IsValidObject(DYN_ObjRef object)
{
    DLOG(INTERPRETER, "IsValidObject %p", object);
    BaseObject* obj = reinterpret_cast<BaseObject*>(object);
    if (obj == nullptr) {
        return false;
    }
    return obj->IsValidObject();
}

DYN_ThreadLocalData GetThreadLocalData()
{
    return reinterpret_cast<DYN_ThreadLocalData>(MRT_GetThreadLocalData());
}

bool IsActiveGCPhase(DYN_ThreadLocalData tld) {
    ThreadLocalData* threadLocalData = static_cast<ThreadLocalData*>(tld);
    Mutator* mutator = threadLocalData->mutator;
    return mutator->GetMutatorPhase() >= GCPhase::GC_PHASE_ENUM;
}

DYN_ExceptionWrapper GetExceptionWrapper()
{
    DLOG(INTERPRETER, "GetExceptionWrapper");
    return static_cast<DYN_ExceptionWrapper>(ExceptionManager::GetExceptionWrapper());
}

extern "C" uintptr_t GetCurrentCatchFunctionPC()
{
    ExceptionWrapper& eWrapper = Mutator::GetMutator()->GetExceptionWrapper();
    return eWrapper.GetCurrentCatchFunctionPC();
}

void* PostThrowException(DYN_ExceptionWrapper exception_wrapper)
{
    DLOG(INTERPRETER, "PostThrowException %p", exception_wrapper);
    ExceptionWrapper* wrapper = static_cast<ExceptionWrapper*>(exception_wrapper);
    return ExceptionManager::BeginCatch(wrapper);
}

void NativeLogger(int logLevel, char* tag, char* message)
{
    VLOG(static_cast<LogType>(logLevel), "[%s] %s", tag, message);
}

size_t ComputeCJThreadDataOffset()
{
    union MutatorTmpContainer {
        MutatorTmpContainer() {}
        ~MutatorTmpContainer() {}
        Mutator mutator;
    } tmp;
    return ((size_t) &tmp.mutator.interpreterCJThreadData) - ((size_t) &tmp.mutator);
}

RTErrorCode InitInterpreter(const InterpreterParam& interpreterParam)
{
    const char* libName = interpreterParam.interpreterLibName;
    const int argsCount = interpreterParam.interpreterArgsCount;
    const char* const* args = interpreterParam.interpreterArgs;
    void* symbolHandle = interpreterParam.appLibHandle;

    VLOG(INTERPRETER, "Interpreter parameter:\n\tInterpreter library: %s\n\tInterpreter arguments count: %d",
        libName != nullptr ? libName : "(null)", argsCount);
    if (args != nullptr && argsCount > 0) {
        for (int i = 0; i < argsCount; ++i) {
            VLOG(INTERPRETER, "\tInterpreter argument #%d: %s", i, args[i] != nullptr ? args[i] : "(null)");
        }
    }

    if (libName == nullptr || std::strlen(libName) == 0) {
        DLOG(INTERPRETER, "interpreter library name is empty, interpreter could not be initialized");
        return E_ARGS;
    }

#if defined(__OHOS__)
    void* interpreterLibHandle = LoaderManager::GetInstance()->LoadInterpreter(libName);
#else
    void* interpreterLibHandle = dlopen(libName, RTLD_LAZY);
#endif
    if (interpreterLibHandle == 0) {
        DLOG(INTERPRETER, "interpreter library could not be loaded: %s", dlerror());
        return E_FAILED;
    }

    StackManager::InitAddressScopeForInterpreter(libName);

    void *sym = dlsym(interpreterLibHandle, "interpreter_bridge_init");
    DYN_InitRt init = (DYN_InitRt)sym;
    if (sym == 0) {
        DLOG(INTERPRETER, "dlsym failed: %s", dlerror());
        return E_FAILED;
    }

    struct DYN_CJNativeInterface cjnativeInterface = {
        .version = DYN_CJNATIVE_INTERFACE_VERSION,
        .appLibHandle = symbolHandle,
        .carrierSpecificOffset = offsetof(ThreadLocalData, mutator),
        .cjThreadSpecificOffset   = ComputeCJThreadDataOffset(),
        .typeInfo = &TypeInfoProvider,
        .typeTemplate = &FindTypeTemplate,
        .getOrCreateTypeInfo = &GetOrCreateTypeInfo,
        .getInstanceSize = &GetInstanceSize,
        .objectAlloc = &ObjectAllocate,
        .arrayAlloc = &ArrayAllocate,
        .safePoint = &SafePoint,
        .isPendingSafePoint = &IsPendingSafePoint,
        .throwException = &ThrowException,
        .throwOom = &ThrowOutOfMemoryError,
        .getPendingException = &GetPendingException,
        .getAndClearPendingException = &GetAndClearPendingException,
        .getExceptionWrapper = &GetExceptionWrapper,
        .postThrowException = &PostThrowException,
        .getCurrentCatchFunctionPc = &GetCurrentCatchFunctionPC,
        .instanceOf = &InstanceOf,
        .isSubType = &IsSubType,
        .visitRootFromInterpreter = &VisitRootFromInterpreter,
        .visitDerivedPtrFromInterpreter = &VisitDerivedPtrFromInterpreter,
        .readStaticField = &ReadStaticField,
        .writeStaticField = &WriteStaticField,
        .readInstanceField = &ReadInstanceField,
        .writeInstanceField = &WriteInstanceField,
        .readStructField = &ReadStructFieldOfObject,
        .writeStructField = &WriteStructFieldOfObject,
        .readStaticStructField = &ReadStaticStructField,
        .writeStaticStructField = &WriteStaticStructField,
        .readGenericField = &ReadGenericField,
        .writeGenericField = &WriteGenericField,
        .arrayBodyOffset = static_cast<size_t>(MArray::GetContentOffset()),
        .getArrayRefElement = &GetArrayElement,
        .setArrayRefElement = &SetArrayElement,
        .isValidObject = &IsValidObject,
        .getThreadLocalData = &GetThreadLocalData,
        .isActiveGcPhase = &IsActiveGCPhase,
        .stackGrowStub = reinterpret_cast<DYN_StackGrowStubFn>(&CJ_MCC_StackGrowStub),
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
        .i2nStub = reinterpret_cast<DYN_I2NStubFn>(&CJ_MCC_I2NStub),
#else
        .i2nStub = nullptr,
#endif
        .nativeLogger = &NativeLogger,
    };

    INT_InterpreterInterface interpreterInterface {};
    interpreterInterface.version = INT_INTERPRETER_INTERFACE_VERSION;

    int code = init(&interpreterInterface, &cjnativeInterface,
        interpreterParam.interpreterArgsCount,
        interpreterParam.interpreterArgs);
    if (code != 0) {
        DLOG(INTERPRETER, "interpreter_bridge_init failed, return code: %d", code);
        return E_FAILED;
    }
    if (interpreterInterface.version != INT_INTERPRETER_INTERFACE_VERSION) {
        DLOG(INTERPRETER, "unsupported interpreter interface version: %lld, expected: %d",
            static_cast<long long>(interpreterInterface.version), INT_INTERPRETER_INTERFACE_VERSION);
        return E_FAILED;
    }
    interpreterInterfaceInstance.initialize(interpreterInterface);
    return E_OK;
}

bool IsC2IStubAddr(const uintptr_t address)
{
    if (!interpreterInterfaceInstance) {
        return false;
    }
    bool res = interpreterInterfaceInstance->c2iStubStartAddr <= address && address < interpreterInterfaceInstance->c2iStubEndAddr;
    DLOG(INTERPRETER, "Checking c2i virtual executor borders: is %p in [%p, %p)? -- %s", address,
        interpreterInterfaceInstance->c2iStubStartAddr,
        interpreterInterfaceInstance->c2iStubEndAddr, res ? "true" : "false");
    return res;
}

bool IsI2IAdapterStartAddr(const uintptr_t address)
{
    if (!interpreterInterfaceInstance) {
        return false;
    }
    bool res = interpreterInterfaceInstance->i2iAdapterStartAddr <= address && address < interpreterInterfaceInstance->i2iAdapterEndAddr;
    DLOG(INTERPRETER, "Checking i2i function borders: is %p in [%p, %p)? -- %s", address,
        interpreterInterfaceInstance->i2iAdapterStartAddr,
        interpreterInterfaceInstance->i2iAdapterEndAddr, res ? "true" : "false");
    return res;
}

bool IsInterpreterPrologueAddr(const uintptr_t address)
{
    if (!interpreterInterfaceInstance) {
        return false;
    }
    uintptr_t startAddr = interpreterInterfaceInstance->interpreterPrologueStartAddr;
    uintptr_t endAddr = interpreterInterfaceInstance->interpreterPrologueEndAddr;
    bool hasRange = startAddr != 0 && startAddr < endAddr;
    bool res = hasRange && startAddr <= address && address < endAddr;
    DLOG(INTERPRETER, "Checking interpreter prologue borders: is %p in [%p, %p)? -- %s", address,
        startAddr, endAddr, res ? "true" : "false");
    return res;
}

uintptr_t GetCommonInterpreterLandingPad() {
    if (!interpreterInterfaceInstance) {
        return 0;
    }
    DLOG(INTERPRETER, "GetCommonInterpreterLandingPad: %p", interpreterInterfaceInstance->landingPad);
    return reinterpret_cast<uintptr_t>(interpreterInterfaceInstance->landingPad);
}

}

#endif // INTERPRETER_ENABLED
