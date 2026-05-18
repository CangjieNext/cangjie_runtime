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

struct TYPE_INFO_ATTRS DYN_TypeInfo;
struct TYPE_INFO_ATTRS DYN_ExtensionData;

typedef void* DYN_MTableDesc;
typedef void* DYN_FuncPtr;

union DYN_GCTib {
    uintptr_t raw; // higher bit - 1: raw, 0: ptr
    void *ptr;
};

struct TYPE_INFO_ATTRS DYN_TypeInfo {
    const char* typeInfoName;
    int8_t type;
    uint8_t flag;
    uint16_t fieldNum;
    union {
        uint32_t instanceSize;
        uint32_t componentSize;
    };
    union DYN_GCTib gctib;
    uint32_t uuid;
    uint8_t align;
    int8_t typeArgsNum;
    uint16_t validInheritNum;
    uint32_t* fieldOffsets;
    DYN_FuncPtr finalizerMethod;
    struct DYN_TypeInfo** typeArgs;
    struct DYN_TypeInfo** fields;
    union {
        struct DYN_TypeInfo* superTypeInfo;
        struct DYN_TypeInfo* componentTypeInfo;
    };
    struct DYN_ExtensionData** vExtensionDataStart;
    DYN_MTableDesc* mTableDesc;
    void* reflectOrDebugInfo;
};

struct EXTENSION_DATA_ATTRS DYN_ExtensionData {
    uint32_t argNum;
    uint8_t isInterfaceTypeInfo;
    uint8_t flag;
    uint16_t funcTableSize;
    union {
        void* tt;
        struct DYN_TypeInfo* ti;
    };
    union {
        DYN_FuncPtr interfaceFn;
        struct DYN_TypeInfo* interfaceTypeInfo;
    };
    DYN_FuncPtr whereCondFn;
    DYN_FuncPtr* funcTable;
};

#ifdef __cplusplus
} // extern "C"
#endif // __cplusplus

#endif // RT_TYPES_H
