// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef RT_INTERFACE_H
#define RT_INTERFACE_H

#include <stdint.h>
#include <stddef.h>

#include "Options.h"

#ifdef __cplusplus
extern "C" {
namespace MRTExport {
#endif // __cplusplus

////////////////////////////////////////////////////////////////////////////////////
// region Types
////////////////////////////////////////////////////////////////////////////////////

// Pointer to traceable object reference. Binary layout of object is defined by CJNative runtime.
typedef void *obj_ref_t;

// Pointer to RefField
typedef void *field_ref_t;

// Pointer to the place that contains traceable object reference.
typedef void *placeholder_t;

// Pointer to TypeInfo. Binary layout of this structure is defined by CJNative runtime.
typedef void *type_info_t;

// Pointer to TypeTemplate. Binary layout of this structure is defined by CJNative runtime.
typedef void *type_template_t;

// Pointer to ThreadLocalData that is currently executing current fiber.
typedef void *thread_local_data_t;

// Pointer to fiber specific data that is used by interpreter during execution.
// This pointer is stored in fiber specific storage.
typedef void *fiber_specific_data_t;

// This is alias for frame pointer. CJNative runtime should pass this pointer to `frame_info_provider_f` function.
// Interpreter would return description of corresponding frame (e.g. file_name, line_number).
typedef const void *frame_pointer_t;

// This is alias for instruction pointer. CJNative runtime should pass this pointer to `frame_info_provider_f` function.
// Interpreter would return description of corresponding frame (e.g. file_name, line_number).
typedef const void *instruction_pointer_t; 

// Visitor of placeholders that contain object references.
// Interpreter cannot use this data directly but should pass it to the callbacks defined in `cjnative_interface_t`.
typedef const void *root_visitor_t;

// Visitor of placeholders that containt pointers to stack slots.
// Interpreter cannot use this data directly but should pass it to the callbacks defined in `cjnative_interface_t`.
typedef const void *stack_ptr_visitor_t;

// Pointer to ExceptionWrapper.
typedef void *exception_wrapper_t;

// Frame description used by interpreter frame visitors.
struct frame_desc_t {
    frame_pointer_t fp;
    instruction_pointer_t ip;
};

// This struct describes frame. Interpreter would fill this structure in `frame_info_provider_f` function.
struct interpreted_frame_info_t {
    size_t line_number;
    char*  method_name;
    char*  class_name;
    char*  file_name;
};

// Pointer on stack can point to stack itself or not.
enum stack_ptr_slot_type_t {
    OnStack,
    OutOfStack
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
struct interpreter_interface_t;

// Collection of callbacks implemented by CJNative. See detailed description below.
struct cjnative_interface_t;


////////////////////////////////////////////////////////////////////////////////////
// region interpreter interface
////////////////////////////////////////////////////////////////////////////////////

// Initalizes runtime and its interface.
//
// params:
// - Pointer to `interpreter_interface_t` which will be filled.
// - Pointer to `cjnative_interface_t`. The content will be copied.
// return: 0 on success.
//
typedef int (*init_rt)(struct interpreter_interface_t *, struct cjnative_interface_t *);


// Visit static roots (global variables) of interpreted code with some visitor.
// Can be used to add interpreted roots for GC root-set.
typedef void (*visit_global_roots_f)(root_visitor_t visitor);

// Visit local roots (local variables) of interpreted function with some visitor.
// Can be used to add interpreted roots for GC root-set.
// 
// params:
// - visitor: CJNative visitor of reference placeholders
// - frame_desc: description of interpreted frame to visit
//
typedef void (*visit_frame_roots_f)(root_visitor_t visitor, frame_desc_t frame_desc);

// Visits interpreter fiber specific storages and invokes given visitor for each placeholder that contains object reference.
// This method could be called by any thread (e.g. auxiliary GC thread) not necessarily from the thread that
// invoked `safe_point` or allocator function.
// This function should be invoked after fiber that owns given `fiber_specific_data_t` has reached safe point and is stopped.
//
// params:
// - CJNative visitor of reference placeholders
// - Pointer to fiber-specific data
//
typedef void (*visit_fiber_roots_f)(root_visitor_t visitor, fiber_specific_data_t interp_fiber_data);

// Visits interpreter fiber specific storages and invokes given visitor for each placeholder that contains pointer to CJ stack.
// This function should be invoked when fiber that owns given `fiber_specific_data_t` tries to extend its stack.
//
// params:
// - CJNative visitor of stack pointer placeholders
// - Pointer to fiber-specific data
// - Determines if on-stack or out-of-stack placeholders should be visited: there can be separate visitors for each
//   kind of placeholders because they (not only pointed value) can be moved or not depending on their place.
//
typedef void (*visit_stack_ptrs_f)(stack_ptr_visitor_t visitor, fiber_specific_data_t interp_fiber_data, stack_ptr_slot_type_t slot_type);


// Registers newly created fiber in interpreter. This method could be called by any thread (e.g. auxiliary scheduler thread) to notify interpreter about creation of new fiber.
//
// params:
// - Pointer to FiberSpecificData
//
// Notes:
//  Interpreter will save auxiliary metadata in [fiber_specific_data_t + offset_to_fiber_specific_memory_f] memory area to simplify support of interpretation and Garbage collection.
//
typedef void (*fiber_start_f)(fiber_specific_data_t*);

// Unregisters fiber in interpreter. This method could be called by any thread (e.g. auxiliary scheduler thread) to notify interpreter about termination of some completed fiber.
//
// params:
// - Pointer to FiberSpecificData
//
typedef void (*fiber_destroy_f)(fiber_specific_data_t*);


// Provides information about given frame which could be used for precise stack trace generation.
// This method could be called by any thread (e.g. auxiliary stack dumper thread) to query information about interpreter frames.
//
// Important: this function may invoke "long-running" tasks (fseek, pthread_mutex_lock) so CJNative runtime
//            should treat this method as "foreign" code.
//
// params:
// - Frame pointer. Must be provided by CJNative runtime.
// - Instruction pointer in frame (callsite position).
// - Pointer to interpreted_frame_info_t. Fields of this struct will be initialized by interpreter.
//
typedef void (*frame_info_provider_f)(frame_pointer_t, instruction_pointer_t ip, interpreted_frame_info_t *);


// Landing pad which handles pending exception.
// If there is suitable catch block in current last interpreted frame, interprets catch block.
// Otherwise, drops current frame and rethrows exception.
//
typedef void (*landing_pad_f)();

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
typedef obj_ref_t (*obj_alloc_f)(type_info_t);

// Allocate new array.
// params:
// - array_type - pointer to TypeInfo describing the array to be allocated
// - size - size of the array to be allocated
//
// return: allocated array object or NULL
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef obj_ref_t (*array_alloc_f)(type_info_t array_type, uint64_t size);

// Poll a safe point.
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*safe_point_f)();

// Enter safe region. This method can be called before executing long-running "foreign" code.
//
// params:
// - updateUnwindContext: true if CJNative runtime should update UnwindContext for current Mutator.
//
// return: true if Mutator wasn`t is safe region and sucessfully entered it, false - otherwise
//
typedef bool (*enter_safe_region_f)(bool updateUnwindContext);

// Leave safe region. This method can be called after executing long-running "foreign" code.
//
// return: true if Mutator was in safe region and successfully left it, false - otherwise
//
typedef bool (*leave_safe_region_f)();

// C2N_Stub function, can be reused as implementation of I2N call.
// TODO avoid using it
typedef void (*C2N_Stub_f)();

// Typedef for foreign function. See below for explanation.
typedef void *(*foreign_func)(void*, void*, void*, void*, void*);


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
typedef type_info_t (*type_info_provider_f)(const char *);

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
typedef type_template_t (*type_template_f)(const char *);

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
typedef type_info_t (*get_or_create_type_info_f)(type_template_t typeTemplate, uint32_t argSize, type_info_t* typeArgs);

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
typedef uint32_t (*get_instance_size_f)(type_info_t typeInfo);


// Throw OutOfMemoryError exception.
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*throw_OOM_f)();

// Throw given exception.
//
// params:
// - exception object to throw
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*throw_exception_f)(obj_ref_t exception_object);

// Get pending exception instance.
// This method hasn't any side effects (doesn't affect pending status).
//
// return: pointer to pending object
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef obj_ref_t (*get_pending_exception_f)();

// Get and clear pending exception instance.
// Any call to `get_pending_exception_f` after execution of this method should return null.
//
// return: pointer to pending object
//
// Notes:
//  This method will be invoked by interpreter as a part of interpretation loop.
//
typedef obj_ref_t (*get_and_clear_pending_exception_f)();


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
typedef bool (*instance_of_f)(obj_ref_t obj, type_info_t ti);

// Check that typeInfo is a subtype of superTypeInfo.
//
// params:
// - type_info - type to check
// - super_type_info - base type
//
// return: true if type_info is a subtype of super_type_info.
//
// Notes:
//
typedef bool (*is_sub_type_f)(type_info_t type_info, type_info_t super_type_info);


// Apply global root visitor to reference placeholder.
//
// params:
// - visitor: CJNative visitor of reference placeholders
// - placeholder: pointer to the place that contains traceable object reference
//
// Notes:
// This method will be invoked by interpreter as a part of root set visiting started by CJNative runtime.
//
typedef void (*visit_global_root_from_interpreter_f)(root_visitor_t visitor, placeholder_t placeholder);

// Apply local root visitor to reference placeholder.
//
// Notes:
// This method will be invoked by interpreter as a part of root set visiting started by CJNative runtime.
//
typedef void (*visit_frame_root_from_interpreter_f)(root_visitor_t visitor, placeholder_t placeholder);

// Applies given visitor to given placeholder.
// This method will be used by interpreter to visit local roots stored in interpreter fiber-specific storage.
//
// params:
// - CJNative visitor of reference placeholders
// - placeholder from interpreter fiber-specific storage
//
// Notes:
// This method will be invoked by interpreter as a part of root set visiting started by CJNative runtime.
//
typedef void (*visit_fiber_root_from_interpreter_f)(root_visitor_t visitor, placeholder_t placeholder);

// Applies given visitor to given placeholder that contains pointer to stack.
// This method will be used by interpreter to visit local roots stored in interpreter fiber-specific storage.
//
// params:
// - CJNative visitor of stack pointer placeholders
// - placeholder from interpreter fiber-specific storage
//
// Notes:
// This method will be invoked by interpreter as a part of stack pointers adjusting started by CJNative runtime.
//
typedef void (*visit_stack_ptr_from_interpreter_f)(stack_ptr_visitor_t visitor, placeholder_t placeholder);

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
typedef obj_ref_t (*read_static_field_f)(field_ref_t source);

// Write reference to static field.
//
// params:
// - destination - pointer to static reference field
// - new_value - object reference to store in the field
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*write_static_field_f)(field_ref_t destination, obj_ref_t new_value);

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
typedef obj_ref_t (*read_instance_field_f)(obj_ref_t source, field_ref_t field);

// Write reference to instance field.
//
// params:
// - destination - object that contains instance field
// - field - pointer to instance reference field
// - new_value - object reference to store in the field
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*write_instance_field_f)(obj_ref_t destination, field_ref_t field, obj_ref_t new_value);

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
typedef void (*read_struct_field_f)(uintptr_t dstPtr, obj_ref_t obj, uintptr_t srcField, size_t size);

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
typedef void (*write_struct_field_f)(obj_ref_t obj, uintptr_t dst, uintptr_t src, size_t size);

// Read a static struct field.
//
// params:
// - dstPtr - pointer to the destination memory
// - dstSize - size of destination memory
// - srcPtr - pointer to the source static struct
// - srcSize - size of source struct
// - tib_value - The GCTib value (can be a pointer to TypeInfo or a raw ShortGCTib bitmap)
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*read_static_struct_field_f)(uintptr_t dstPtr, size_t dstSize, uintptr_t srcPtr, size_t srcSize, size_t tib_value);

// Write a static value type field (struct).
//
// params:
// - dst - pointer to the destination static struct
// - dstLen - size of destination struct
// - src - pointer to the source memory
// - srcLen - size of source memory
// - tib_value - The GCTib value (can be a pointer to TypeInfo or a raw ShortGCTib bitmap)
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*write_static_struct_field_f)(uintptr_t dst, size_t dstLen, uintptr_t src, size_t srcLen, size_t tib_value);

// Read a generic field from an object.
// Should be used if generic type resolves to struct/value at runtime, otherwise use read_instance_field_f.
//
// params:
// - dst - pointer to the destination box-object of generic struct/value
// - src_obj - the source object on the heap (can be box of generic struct/value)
// - src_field - address of the field inside src_obj
// - size - size of the data to copy
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*read_generic_field_f)(obj_ref_t dst, obj_ref_t src_obj, uintptr_t src_field, size_t size);

// Write a generic field to an object.
// Should be used if generic type resolves to struct/value at runtime, otherwise use write_instance_field_f.
//
// params:
// - dst_obj - destination object (can be box of generic struct/value)
// - dst_field - address of the field inside dst_obj
// - src_obj - source box-object of generic struct/value
// - size - size of the generic value
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*write_generic_field_f)(obj_ref_t dst_obj, uintptr_t dst_field, obj_ref_t src_obj, size_t size);

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
typedef obj_ref_t (*get_array_ref_element_f)(obj_ref_t source, uint64_t index);

// Write reference to array.
//
// params:
// - source - RawArray object containing references
// - index - index of the element to write
// - new_value - object reference to store
//
// Notes:
// This method will be invoked by interpreter as a part of interpretation loop.
//
typedef void (*set_array_ref_element_f)(obj_ref_t destination, uint64_t index, obj_ref_t new_value);

//
// Check state flags of the object.
//
typedef bool (*is_valid_object_f)(obj_ref_t object);

//
// Get ThreadLocalData.
//
typedef thread_local_data_t (*get_thread_local_data_f)();

//
// Returns true if the GC is in an "active" phase.
// In active phase fast-path write barriers can`t be used.
//
typedef bool (*is_active_gc_phase_f)(thread_local_data_t);

// Stub for stack growth.
//
typedef void* (*stack_grow_stub_f)();

// Retrieves the current ExceptionWrapper.
//
// return: pointer to ExceptionWrapper
//
typedef exception_wrapper_t (*get_exception_wrapper_f)();

// Get PC of function which caught exception.
// Can be used as return address for interpreter landing pad.
//
typedef uintptr_t (*get_current_catch_function_PC_f)();

// Begins exception catch block using the provided ExceptionWrapper.
//
// params:
// - exception_wrapper - current exception wrapper
//
// return: exception object
//
typedef obj_ref_t (*post_throw_exception_f)(exception_wrapper_t exception_wrapper);

//
// Native logger function. E.g. hilog can be used to log events in interpreter on OHOS devices.
//
// params:
// - tag - log tag
// - message - log message
//
typedef void (*native_logger_f)(int log_level, char* tag, char* message);


////////////////////////////////////////////////////////////////////////////////////
// endregion CJNative interface
////////////////////////////////////////////////////////////////////////////////////


// region Interfaces

struct interpreter_interface_t {
    // current supported version is 1
    int64_t version;

    size_t fiber_specific_data_size;
    size_t iterator_size;

    uintptr_t c2iStubStartAddr;
    uintptr_t c2iStubEndAddr;

    uintptr_t i2iAdapterStartAddr;
    uintptr_t i2iAdapterEndAddr;

    visit_global_roots_f visit_global_roots;
    visit_fiber_roots_f visit_fiber_roots;
    visit_frame_roots_f visit_frame_roots;
    visit_stack_ptrs_f visit_stack_ptrs;

    fiber_start_f fiber_start;
    fiber_destroy_f fiber_destroy;

    frame_info_provider_f frame_info_provider;
    landing_pad_f landing_pad;
};


struct cjnative_interface_t {
    size_t carrier_specific_offset;
    size_t fiber_specific_offset;

    type_info_provider_f type_info;
    type_template_f type_template;
    get_or_create_type_info_f get_or_create_type_info;
    get_instance_size_f get_instance_size;

    obj_alloc_f object_alloc;
    array_alloc_f array_alloc;
    safe_point_f safe_point;
    enter_safe_region_f enter_safe_region;
    leave_safe_region_f leave_safe_region;

    throw_exception_f throw_exception;
    throw_OOM_f throw_OOM;
    get_pending_exception_f get_pending_exception;
    get_and_clear_pending_exception_f get_and_clear_pending_exception;
    get_exception_wrapper_f get_exception_wrapper;
    post_throw_exception_f post_throw_exception;
    get_current_catch_function_PC_f get_current_catch_function_PC;

    instance_of_f instance_of;
    is_sub_type_f is_sub_type;

    visit_fiber_root_from_interpreter_f visit_fiber_root_from_interpreter;
    visit_global_root_from_interpreter_f visit_global_root_from_interpreter;
    visit_frame_root_from_interpreter_f visit_frame_root_from_interpreter; 
    visit_stack_ptr_from_interpreter_f visit_stack_ptr_from_interpreter;

    read_static_field_f read_static_field;
    write_static_field_f write_static_field;
    read_instance_field_f read_instance_field;
    write_instance_field_f write_instance_field;

    read_struct_field_f read_struct_field;
    write_struct_field_f write_struct_field;
    read_static_struct_field_f read_static_struct_field;
    write_static_struct_field_f write_static_struct_field;

    read_generic_field_f read_generic_field;
    write_generic_field_f write_generic_field;

    size_t array_body_offset;
    get_array_ref_element_f get_array_ref_element;
    set_array_ref_element_f set_array_ref_element;

    is_valid_object_f is_valid_object;
    get_thread_local_data_f get_thread_local_data;
    is_active_gc_phase_f is_active_gc_phase;

    native_logger_f native_logger;
};

#ifdef __cplusplus
} // namespace MRTExport
} // extern "C"
#endif // __cplusplus

// endregion Interfaces
#endif
