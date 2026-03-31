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

struct TYPE_INFO_ATTRS TypeInfoT;
struct TYPE_INFO_ATTRS ExtensionDataT;

typedef void* MTableDescT;
typedef void* FuncPtrT;

union GCTibT {
    uintptr_t raw;
    void *ptr;
};

struct TYPE_INFO_ATTRS TypeInfoT {
    const char* typeInfoName;
    int8_t type;
    uint8_t flag;
    uint16_t fieldNum;
    union {
        uint32_t instanceSize;
        uint32_t componentSize;
    };
    union GCTibT gctib;
    uint32_t uuid;
    uint8_t align;
    int8_t typeArgsNum;
    uint16_t validInheritNum;
    uint32_t* fieldOffsets;
    FuncPtrT finalizerMethod;
    struct TypeInfoT** typeArgs;
    struct TypeInfoT** fields;
    union {
        struct TypeInfoT* superTypeInfo;
        struct TypeInfoT* componentTypeInfo;
    };
    struct ExtensionDataT** vExtensionDataStart;
    MTableDescT* mTableDesc;
    void* reflectOrDebugInfo;
};

struct EXTENSION_DATA_ATTRS ExtensionDataT {
    uint32_t argNum;
    uint8_t isInterfaceTypeInfo;
    uint8_t flag;
    uint16_t funcTableSize;
    union {
        void* tt;
        struct TypeInfoT* ti;
    };
    union {
        FuncPtrT interfaceFn;
        struct TypeInfoT* interfaceTypeInfo;
    };
    FuncPtrT whereCondFn;
    FuncPtrT* funcTable;
};

#ifdef __cplusplus
} // namespace MRTExport
} // extern "C"
#endif // __cplusplus

#endif // RT_TYPES_H