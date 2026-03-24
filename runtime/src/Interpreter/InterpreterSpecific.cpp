// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#include "dlfcn.h"
#include <stdint.h>
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
#include "InterpreterSpecific.h"

using namespace MRTExport;
namespace MapleRuntime {

extern "C" void CJ_MCC_StackGrowStub(...);
#if defined(__x86_64__) && defined(__linux__)
extern "C" void CJ_MCC_I2NStub(...);
#endif

struct {
    bool initialized = false;
    interpreter_interface_t interpreterInterface;

    void initialize(interpreter_interface_t& value)
    {
        interpreterInterface = value;
        initialized = true;
    }

    operator bool() { return initialized; }

    interpreter_interface_t* operator->()
    {
        if (initialized) {
            return &interpreterInterface;
        }
        LOG(RTLOG_FATAL, "Usage of uninitialized interpreter interface\n");
        return nullptr;
    }

} interpreterInterfaceInstance;

void ProvideInterpretedFrameInfo(uint64_t fp, uint64_t ip, interpreted_frame_info_t* fInfo)
{
    auto framePointer = reinterpret_cast<frame_pointer_t>(fp);
    auto instructionPointer = reinterpret_cast<instruction_pointer_t>(ip);

    DLOG(INTERPRETER, "ProvideInterpretedFrameInfo(fp=%p, ip=%p)", framePointer, instructionPointer);

    if (interpreterInterfaceInstance) {
        interpreterInterfaceInstance->frame_info_provider(framePointer, instructionPointer, fInfo);
    }
}

void IterateFramesWithState(fiber_specific_data_t fiberData, void (*callback)(visiting_state_t, void*), void* ctx) 
{
    if (interpreterInterfaceInstance) {
        interpreterInterfaceInstance->iterate_frames_with_state(fiberData, callback, ctx);
    }
}

frame_desc_t toFrameDesc(FrameInfo frameInfo)
{
    return {
        .fp = frameInfo.GetMachineFrame().GetFA(),
        .ip = frameInfo.GetMachineFrame().GetIP(),
    };
}

void VisitInterpreterFrameRootsExpansion(visiting_state_t state, FrameInfo frameDesc, root_visitor_t stackPtrVisitor, derived_ptr_visitor_t derivedPtrVisitor)
{
    if (interpreterInterfaceInstance) {
        frame_desc_t fd = toFrameDesc(frameDesc);
        interpreterInterfaceInstance->visit_frame_roots_expansion(state, fd, stackPtrVisitor, derivedPtrVisitor);
    }
}

void VisitInterpreterFrameRootsMarking(visiting_state_t state, FrameInfo frameDesc, root_visitor_t rootVisitor)
{
    if (interpreterInterfaceInstance) {
        frame_desc_t fd = toFrameDesc(frameDesc);
        interpreterInterfaceInstance->visit_frame_roots_marking(state, fd, rootVisitor);
    }
}

void VisitInterpreterFrameRootsAdjusting(visiting_state_t state, FrameInfo frameDesc, root_visitor_t rootVisitor, derived_ptr_visitor_t derivedPtrVisitor)
{
    if (interpreterInterfaceInstance) {
        frame_desc_t fd = toFrameDesc(frameDesc);
        interpreterInterfaceInstance->visit_frame_roots_adjusting(state, fd, rootVisitor, derivedPtrVisitor);
    }
}

void VisitInterpreterGlobalRoots(root_visitor_t visitor)
{
    if (interpreterInterfaceInstance) {
        interpreterInterfaceInstance->visit_global_roots(visitor);
    }
}

void InterpreterFiberStart(fiber_specific_data_t* dataField)
{
    if (interpreterInterfaceInstance) {
        interpreterInterfaceInstance->fiber_start(dataField);
    }
}

void InterpreterFiberDestroy(fiber_specific_data_t* dataField)
{
    if (interpreterInterfaceInstance) {
        interpreterInterfaceInstance->fiber_destroy(dataField);
    }
}

void VisitRootFromInterpreter(root_visitor_t _visitor, placeholder_t _placeholder)
{
    const RootVisitor* visitor = static_cast<const RootVisitor*>(_visitor);
    ObjectRef* placeholder = static_cast<ObjectRef*>(_placeholder);

    DLOG(INTERPRETER, "VisitRootFromInterpreter: visitor=[%p], placeholder=[%p]", (void*)visitor, (void*)placeholder);

    (*visitor)(*placeholder);
}

void VisitDerivedPtrFromInterpreter(derived_ptr_visitor_t _visitor, placeholder_t _basePtrHolder, placeholder_t _derivedPtrHolder)
{
    const DerivedPtrVisitor* visitor = static_cast<const DerivedPtrVisitor*>(_visitor);
    BasePtrType* basePtrLocation = static_cast<BasePtrType*>(_basePtrHolder);
    DerivedPtrType* derivedPtrLocation = static_cast<DerivedPtrType*>(_derivedPtrHolder);

    DLOG(INTERPRETER, "VisitDerivedPtrFromInterpreter: visitor=[%p], placeholder=[%p]", (void*)visitor, (void*) derivedPtrLocation);

    (*visitor)(*basePtrLocation, *derivedPtrLocation);
}

struct type_info_t* TypeInfoProvider(const char * sig)
{
    DLOG(INTERPRETER, "TypeInfoProvider: %s", sig);
    auto *result = reinterpret_cast<struct type_info_t*>(LoaderManager::GetInstance()->FindTypeInfoFromLoadedFiles(sig));
    DLOG(INTERPRETER, "FindTypeInfoFromLoadedFiles(%s) return %p", sig, result);

    return result;
}

type_template_t FindTypeTemplate(const char* mangledName)
{
    DLOG(INTERPRETER, "FindTypeTemplate: %s", mangledName);
    type_template_t result = LoaderManager::GetInstance()->FindTypeTemplateFromLoadedFiles(mangledName);
    DLOG(INTERPRETER, "FindTypeTemplateFromLoadedFiles(%s) return %p", mangledName, result);

    return result;
}

struct type_info_t* GetOrCreateTypeInfo(type_template_t typeTemplate, uint32_t typeArgsCount, struct type_info_t** typeArgs)
{
    DLOG(INTERPRETER, "GetOrCreateTypeInfo: template=%p size=%u", typeTemplate, typeArgsCount);

    return reinterpret_cast<struct type_info_t*>(TypeInfoManager::GetTypeInfoManager().GetOrCreateTypeInfo(
        static_cast<TypeTemplate*>(typeTemplate), 
        typeArgsCount, 
        reinterpret_cast<TypeInfo**>(typeArgs)
    ));
}

uint32_t GetInstanceSize(struct type_info_t* typeInfoPtr)
{
    TypeInfo* ti = reinterpret_cast<TypeInfo*>(typeInfoPtr);
    DLOG(INTERPRETER, "GetInstanceSize: %s", ti->GetName());
    return ti->GetInstanceSize();
}

obj_ref_t ObjectAllocate(struct type_info_t* tpe) 
{
    TypeInfo* typeInfo = reinterpret_cast<TypeInfo*>(tpe);
    MSize size = AlignUp<MSize>(typeInfo->GetInstanceSize(), 8) + sizeof(TypeInfo*);

    ObjRef obj = ObjectManager::NewObject(typeInfo, size);

    DLOG(INTERPRETER, "ObjectAllocate: type=%p (%s), addr=%p, size=%u", tpe, typeInfo->GetName(), obj, size);

    return (obj_ref_t)obj;
}

obj_ref_t ArrayAllocate(struct type_info_t* tpe, uint64_t size) 
{
    TypeInfo* typeInfo = reinterpret_cast<TypeInfo*>(tpe);

    ArrayRef arr = ObjectManager::NewArray(size, typeInfo);
    DLOG(INTERPRETER, "ArrayAllocate: type=%p, addr=%p, size=%u", tpe, arr, size);

    return (obj_ref_t)arr;
}

void SafePoint() 
{
    DLOG(INTERPRETER, "SafePoint");

    // TODO: reduce gc-point overhead
    ScopedEnterSaferegion gcpoint(true);
}

bool IsPendingSafePoint(thread_local_data_t tlData)
{
    if (tlData == nullptr) {
        LOG(RTLOG_ERROR, "[Interpreter] IsPendingSafePoint called with null thread_local_data_t");
        return false;
    }
    ThreadLocalData* threadLocalData = reinterpret_cast<ThreadLocalData*>(tlData);
    return threadLocalData->safepointState != 0;
}

void ThrowException(obj_ref_t exception) 
{
    DLOG(INTERPRETER, "ThrowException: exception=%p", exception);
    ExceptionManager::ThrowException(static_cast<ExceptionRef>(exception));
}

void ThrowOutOfMemoryError() 
{
    DLOG(INTERPRETER, "ThrowOutOfMemoryError");
    ExceptionManager::OutOfMemory();
    ExceptionManager::ThrowPendingException();
}

obj_ref_t GetPendingException()
{
    ExceptionRef ref = ExceptionManager::GetPendingException();
    
    DLOG(INTERPRETER, "GetPendingException: %p", ref);
    return static_cast<obj_ref_t>(ref);
}

obj_ref_t GetAndClearPendingException()
{
    auto ref = GetPendingException();

    ExceptionManager::ClearPendingException();
    DLOG(INTERPRETER, "GetAndClearPendingException: %p", ref);

    return ref;
}

bool InstanceOf(obj_ref_t ref, struct type_info_t* ti) 
{
    DLOG(INTERPRETER, "InstanceOf: ref=%p, typeInfo=%p", ref, ti);

    MObject* obj    = static_cast<MObject*>(ref);
    TypeInfo* tinfo = reinterpret_cast<TypeInfo*>(ti);

    DLOG(INTERPRETER, "  obj type is %s", obj->GetTypeInfo()->GetName());
    DLOG(INTERPRETER, "  type to check is %s", tinfo->GetName());

    return obj->IsSubType(*tinfo);
}

bool IsSubType(struct type_info_t* type_info, struct type_info_t* super_type_info)
{
    DLOG(INTERPRETER, "IsSubType: ti=%p, sti=%p", type_info, super_type_info);
    if (type_info == nullptr || super_type_info == nullptr) {
        return false;
    }
    if (type_info == super_type_info) {
        return true;
    }
    
    TypeInfo* tInfo = reinterpret_cast<TypeInfo*>(type_info);
    TypeInfo* sInfo = reinterpret_cast<TypeInfo*>(super_type_info);
    return tInfo->IsSubType(sInfo);
}

obj_ref_t ReadStaticField(field_ref_t source) 
{
    DLOG(INTERPRETER, "ReadStaticField %p", source);

    BaseObject* res = Heap::GetBarrier().ReadStaticRef(*static_cast<RefField<false>*>(source));
    return static_cast<obj_ref_t>(res);
}

void WriteStaticField(field_ref_t destination, obj_ref_t new_value) 
{
    DLOG(INTERPRETER, "WriteStaticField %p %p", destination, new_value);

    Heap::GetBarrier().WriteStaticRef(*static_cast<RefField<false>*>(destination), static_cast<BaseObject*>(new_value));
}

obj_ref_t ReadInstanceField(obj_ref_t source, field_ref_t field) 
{
    DLOG(INTERPRETER, "ReadInstanceField %p %p", source, field);

    RefField<false>& ref = *static_cast<RefField<false>*>(field); // TODO support RefField<true>
    BaseObject* res = Heap::GetBarrier().ReadReference(static_cast<BaseObject*>(source), ref);
    return static_cast<obj_ref_t>(res);
}

void WriteInstanceField(obj_ref_t destination, field_ref_t field, obj_ref_t new_value) 
{
    DLOG(INTERPRETER, "WriteInstanceField %p %p %p", destination, field, new_value);
    RefField<false>& ref = *static_cast<RefField<false>*>(field); // TODO support RefField<true>
    Heap::GetBarrier().WriteReference(static_cast<BaseObject*>(destination), ref, static_cast<BaseObject*>(new_value));
}

obj_ref_t GetArrayElement(obj_ref_t source, U64 index)
{
    DLOG(INTERPRETER, "GetArrayElement %p %llu", source, index);
    MArray* array = static_cast<MArray*>(source);
    return static_cast<obj_ref_t>(array->GetRefElement(index));
}


void SetArrayElement(obj_ref_t source, U64 index, obj_ref_t new_value)
{
    DLOG(INTERPRETER, "SetArrayElement %p %llu %p", source, index, new_value);
    MArray* array = static_cast<MArray*>(source);
    array->SetRefElement(index, static_cast<BaseObject*>(new_value));
}

void ReadStructFieldOfObject(MAddress dstPtr, obj_ref_t obj, MAddress srcField, size_t size) {
    DLOG(INTERPRETER, "ReadStructFieldOfObject %p %p %p %zu", dstPtr, obj, srcField, size);
    if (size == 0) {
        return;
    }
    Heap::GetHeap().GetBarrier().ReadStruct(dstPtr, static_cast<BaseObject*>(obj), srcField, size);
}

void WriteStructFieldOfObject(obj_ref_t obj, MAddress dst, MAddress src, size_t size)
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

inline GCTib CastToGCTib(size_t value) {
    GCTib tib;
    tib.tag = static_cast<U64>(value);
    return tib;
}

void ReadStaticStructField(uintptr_t dstPtr, size_t dstSize, uintptr_t srcPtr, size_t srcSize, size_t tib_value)
{
    DLOG(INTERPRETER, "ReadStaticStructField dst=%p size=%zu src=%p size=%zu tib_val=%zx", 
        dstPtr, dstSize, srcPtr, srcSize, tib_value);

    GCTib tib = CastToGCTib(tib_value);    
    Heap::GetHeap().GetBarrier().ReadStaticStruct(dstPtr, srcPtr, dstSize, tib);
}

void WriteStaticStructField(uintptr_t dst, size_t dstLen, uintptr_t src, size_t srcLen, size_t tib_value)
{
    DLOG(INTERPRETER, "WriteStaticStructField dst=%p size=%zu src=%p size=%zu tib_val=%zx", 
        dst, dstLen, src, srcLen, tib_value);

    CHECK_DETAIL((dst != 0u && src != 0u), "WriteStaticStructField wrong parameter, dst: %p src: %p", dst, src);
    
    GCTib tib = CastToGCTib(tib_value);
    Heap::GetBarrier().WriteStaticStruct(dst, dstLen, src, srcLen, tib);
}

void ReadGenericField(obj_ref_t dst, obj_ref_t srcObj, uintptr_t srcField, size_t size)
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

void WriteGenericField(obj_ref_t dstObj, uintptr_t dstField, obj_ref_t src, size_t size)
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

bool IsValidObject(obj_ref_t object) 
{
    DLOG(INTERPRETER, "IsValidObject %p", object);
    BaseObject* obj = reinterpret_cast<BaseObject*>(object);
    if (obj == nullptr) {
        return false; 
    }
    return obj->IsValidObject();
}

thread_local_data_t GetThreadLocalData()
{
    return reinterpret_cast<thread_local_data_t>(MRT_GetThreadLocalData());
}

bool IsActiveGCPhase(thread_local_data_t tld) {
    ThreadLocalData* threadLocalData = static_cast<ThreadLocalData*>(tld);
    Mutator* mutator = threadLocalData->mutator;
    return mutator->GetMutatorPhase() >= GCPhase::GC_PHASE_ENUM;
}

exception_wrapper_t GetExceptionWrapper()
{
    DLOG(INTERPRETER, "GetExceptionWrapper");
    return static_cast<exception_wrapper_t>(ExceptionManager::GetExceptionWrapper());
}

extern "C" uintptr_t GetCurrentCatchFunctionPC()
{
    ExceptionWrapper& eWrapper = Mutator::GetMutator()->GetExceptionWrapper();
    return eWrapper.GetCurrentCatchFunctionPC();
}

void* PostThrowException(exception_wrapper_t exception_wrapper) 
{
    DLOG(INTERPRETER, "PostThrowException %p", exception_wrapper);
    ExceptionWrapper* wrapper = static_cast<ExceptionWrapper*>(exception_wrapper);
    return ExceptionManager::BeginCatch(wrapper);
}

void NativeLogger(int logLevel, char* tag, char* message)
{
    VLOG(static_cast<LogType>(logLevel), "[%s] %s", tag, message);
}

size_t ComputeFiberDataOffset()
{
    union MutatorTmpContainer {
        MutatorTmpContainer() {}
        ~MutatorTmpContainer() {}
        Mutator mutator;
    } tmp;
    return ((size_t) &tmp.mutator.interpreterFiberData) - ((size_t) &tmp.mutator);
}

void InitInterpreter(const RuntimeParam& runtimeParam) 
{
    if (runtimeParam.interpreterParam.interpreterLibName.IsEmpty()) {
        DLOG(INTERPRETER, "interpreter library name is empty");
        return;
    }

    const char* libName = runtimeParam.interpreterParam.interpreterLibName.Str();
    DLOG(INTERPRETER, "initializing interpreter with library: %s", libName);

#if defined(__OHOS__)
    void* handle = LoaderManager::GetInstance()->LoadInterpreter(libName);
#else
    void* handle = dlopen(libName, RTLD_LAZY);
#endif
    if (handle == 0) {
        DLOG(INTERPRETER, "interpreter library could not be loaded: %s", dlerror());
        return;
    }

    StackManager::InitAddressScopeForInterpreter(libName);

    void *sym = dlsym(handle, "interpreter_bridge_init");
    init_rt init = (init_rt)sym;
    if (sym == 0) {
        DLOG(INTERPRETER, "dlsym failed: %s", dlerror());
        return;
    }

    struct cjnative_interface_t cjnativeInterface = {
        .carrier_specific_offset = offsetof(ThreadLocalData, mutator),
        .fiber_specific_offset   = ComputeFiberDataOffset(),
        .type_info = &TypeInfoProvider,
        .type_template = &FindTypeTemplate,
        .get_or_create_type_info = &GetOrCreateTypeInfo,
        .get_instance_size = &GetInstanceSize,
        .object_alloc = &ObjectAllocate,
        .array_alloc = &ArrayAllocate,
        .safe_point = &SafePoint,
        .is_pending_safe_point = &IsPendingSafePoint,
        .throw_exception = &ThrowException,
        .throw_OOM = &ThrowOutOfMemoryError,
        .get_pending_exception = &GetPendingException,
        .get_and_clear_pending_exception = &GetAndClearPendingException,
        .get_exception_wrapper = &GetExceptionWrapper,
        .post_throw_exception = &PostThrowException,
        .get_current_catch_function_PC = &GetCurrentCatchFunctionPC,
        .instance_of = &InstanceOf,
        .is_sub_type = &IsSubType,
        .visit_root_from_interpreter = &VisitRootFromInterpreter,
        .visit_derived_ptr_from_interpreter = &VisitDerivedPtrFromInterpreter,
        .read_static_field = &ReadStaticField,
        .write_static_field = &WriteStaticField,
        .read_instance_field = &ReadInstanceField,
        .write_instance_field = &WriteInstanceField,
        .read_struct_field = &ReadStructFieldOfObject,
        .write_struct_field = &WriteStructFieldOfObject,
        .read_static_struct_field = &ReadStaticStructField,
        .write_static_struct_field = &WriteStaticStructField,
        .read_generic_field = &ReadGenericField,
        .write_generic_field = &WriteGenericField,
        .array_body_offset = static_cast<size_t>(MArray::GetContentOffset()),
        .get_array_ref_element = &GetArrayElement,
        .set_array_ref_element = &SetArrayElement,
        .is_valid_object = &IsValidObject,
        .get_thread_local_data = &GetThreadLocalData,
        .is_active_gc_phase = &IsActiveGCPhase,
        .stack_grow_stub = reinterpret_cast<stack_grow_stub_f>(&CJ_MCC_StackGrowStub),
#if defined(__x86_64__) && defined(__linux__)
        .i2n_stub = reinterpret_cast<i2n_stub_f>(&CJ_MCC_I2NStub),
#else
        .i2n_stub = nullptr,
#endif
        .native_logger = &NativeLogger,
    };

    interpreter_interface_t interpreterInterface;
    interpreterInterface.version = 1;

    int code = init(&interpreterInterface, &cjnativeInterface);
    interpreterInterfaceInstance.initialize(interpreterInterface);
}

bool IsC2IStubAddr(const uintptr_t address)
{
    bool res = interpreterInterfaceInstance->c2iStubStartAddr <= address && address < interpreterInterfaceInstance->c2iStubEndAddr;
    DLOG(INTERPRETER, "Checking c2i virtual executor borders: is %p in [%p, %p)? -- %s", address,
        interpreterInterfaceInstance->c2iStubStartAddr,
        interpreterInterfaceInstance->c2iStubEndAddr, res ? "true" : "false");
    return res;
}

bool Isi2iAdapterStartAddr(const uintptr_t address)
{
    bool res = interpreterInterfaceInstance->i2iAdapterStartAddr <= address && address < interpreterInterfaceInstance->i2iAdapterEndAddr;
    DLOG(INTERPRETER, "Checking i2i function borders: is %p in [%p, %p)? -- %s", address,
        interpreterInterfaceInstance->i2iAdapterStartAddr,
        interpreterInterfaceInstance->i2iAdapterEndAddr, res ? "true" : "false");
    return res;
}

uintptr_t GetCommonInterpreterLandingPad() {
    DLOG(INTERPRETER, "GetCommonInterpreterLandingPad: %p", interpreterInterfaceInstance->landing_pad);
    return reinterpret_cast<uintptr_t>(interpreterInterfaceInstance->landing_pad);
}

}
