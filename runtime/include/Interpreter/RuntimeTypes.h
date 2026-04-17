#ifndef RT_TYPES_H
#define RT_TYPES_H

#include <stdint.h>
#include <stddef.h>

// This file exposes the internal structure of TypeInfo, so it can be constructed externally.

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#define ATTR_PACKED(x) __attribute__((__aligned__(x), __packed__))

#ifdef __arm__
#define TYPE_INFO_ATTRS
#else
#define TYPE_INFO_ATTRS ATTR_PACKED(4)
#endif

#define EXTENSION_DATA_ATTRS ATTR_PACKED(4)

struct TYPE_INFO_ATTRS DYN_TypeInfoT;
struct TYPE_INFO_ATTRS DYN_ExtensionDataT;

typedef void* DYN_MTableDescT;
typedef void* DYN_FuncPtrT;

union DYN_GCTibT {
    uintptr_t raw; // higher bit - 1: raw, 0: ptr
    void *ptr;
};

struct TYPE_INFO_ATTRS DYN_TypeInfoT {
    const char* typeInfoName;
    int8_t type;
    uint8_t flag;
    uint16_t fieldNum;
    union {
        uint32_t instanceSize;
        uint32_t componentSize;
    };
    union DYN_GCTibT gctib;
    uint32_t uuid;
    uint8_t align;
    int8_t typeArgsNum;
    uint16_t validInheritNum;
    uint32_t* fieldOffsets;
    DYN_FuncPtrT finalizerMethod;
    struct DYN_TypeInfoT** typeArgs;
    struct DYN_TypeInfoT** fields;
    union {
        struct DYN_TypeInfoT* superTypeInfo;
        struct DYN_TypeInfoT* componentTypeInfo;
    };
    struct DYN_ExtensionDataT** vExtensionDataStart;
    DYN_MTableDescT* mTableDesc;
    void* reflectOrDebugInfo;
};

struct EXTENSION_DATA_ATTRS DYN_ExtensionDataT {
    uint32_t argNum;
    uint8_t isInterfaceTypeInfo;
    uint8_t flag;
    uint16_t funcTableSize;
    union {
        void* tt;
        struct DYN_TypeInfoT* ti;
    };
    union {
        DYN_FuncPtrT interfaceFn;
        struct DYN_TypeInfoT* interfaceTypeInfo;
    };
    DYN_FuncPtrT whereCondFn;
    DYN_FuncPtrT* funcTable;
};

#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus

#endif // RT_TYPES_H
