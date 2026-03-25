#ifndef RT_TYPES_H
#define RT_TYPES_H

#include <stdint.h>
#include <stddef.h>

// This file exposes the internal structure of TypeInfo, so it can be constructed externally.

#ifdef __cplusplus
extern "C" {
namespace MRTExport {
#endif // __cplusplus

#define ATTR_PACKED(x) __attribute__((__aligned__(x), __packed__))

#ifdef __arm__
#define TYPE_INFO_ATTRS
#else
#define TYPE_INFO_ATTRS ATTR_PACKED(4)
#endif

#define EXTENSION_DATA_ATTRS ATTR_PACKED(4)

struct TYPE_INFO_ATTRS type_info_t;
struct TYPE_INFO_ATTRS extension_data_t;

typedef void* mtable_desc_t;
typedef void* func_ptr_t;

union gc_tib_t {
    uintptr_t raw;
    void *ptr;
};

struct TYPE_INFO_ATTRS type_info_t {
    const char* type_info_name;
    int8_t type;
    uint8_t flag;
    uint16_t field_num;
    union {
        uint32_t instance_size;
        uint32_t component_size;
    };
    union gc_tib_t gctib;
    uint32_t uuid;
    uint8_t align;
    int8_t type_args_num;
    uint16_t valid_inherit_num;
    uint32_t* field_offsets;
    func_ptr_t finalizer_method;
    struct type_info_t** type_args;
    struct type_info_t** fields;
    union {
        struct type_info_t* super_type_info;
        struct type_info_t* component_type_info;
    };
    struct extension_data_t** v_extension_data_start;
    mtable_desc_t* mtable_desc;
    void* reflect_or_debug_info;
};

struct EXTENSION_DATA_ATTRS extension_data_t {
    uint32_t arg_num;
    uint8_t is_interface_type_info;
    uint8_t flag;
    uint16_t func_table_size;
    union {
        void* tt;
        struct type_info_t* ti;
    };
    union {
        func_ptr_t interface_fn;
        struct type_info_t* interface_type_info;
    };
    func_ptr_t where_cond_fn;
    func_ptr_t* func_table;
};

#ifdef __cplusplus
} // namespace MRTExport
} // extern "C"
#endif // __cplusplus

#endif // RT_TYPES_H