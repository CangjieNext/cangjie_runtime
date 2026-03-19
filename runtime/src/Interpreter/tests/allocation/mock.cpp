#include "mock.h"
#include "cangjieUtils.h"
#include <dlfcn.h>
#include <cstddef>
#include <cstdio>

extern MOCK_IMPLEMENTATION mockInstance;

AllocationMock::AllocationMock(interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf)
    : Mock(interpreter_interf, cjnative_interf)
{
    registerC2I(0, reinterpret_cast<uintptr_t>(allocationTest));
    printf("[Mock] AllocationMock created\n");
}

int AllocationMock::allocationTest()
{
    printf("[Mock] allocationTest\n");

    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.type_info("std.core:Object");
    printf("[Mock] type info = %p\n", static_cast<void*>(ti));
    if (!ti) {
        printf("[Mock] error: type info std.core:Object not found\n");
        return 1;
    }

    obj_ref_t obj = interface.object_alloc(ti);
    if (!obj) {
        printf("[Mock] error: failed to allocate std.core:Object\n");
        return 1;
    } else {
        printf("[Mock] allocated object: %p\n", static_cast<void*>(obj));
    }
    void* addr = get_symbol_address("_CN7default17to_be_set_in_mockE");
    if (!addr) {
        printf("[Mock] error: symbol _CN7default17to_be_set_in_mockE not found\n");
        return 1;
    }
    interface.write_static_field(addr, obj);
    printf("[Mock] wrote object to static field: %p\n", static_cast<void*>(addr));

    auto ti2 = interface.type_info("RawArray<default:TestData>");
    printf("[Mock] type info RawArray<default:TestData> = %p\n", static_cast<void*>(ti2));

    if (!ti2) {
        printf("[Mock] error: type info RawArray<default:TestData> not found\n");
        return 1;
    }
    obj_ref_t arr = interface.array_alloc(ti2, 10);
    if (!arr) {
        printf("[Mock] error: failed to allocate RawArray<default:TestData>\n");
        return 1;
    } else {
        printf("[Mock] allocated RawArray<default:TestData>: %p\n", static_cast<void*>(arr));
    }
    auto ti_elem = interface.type_info("default:TestData");
    if (!ti_elem) {
        printf("[Mock] error: type info default:TestData not found\n");
        return 1;
    }
    for (size_t i = 0; i < 10; i++) {
        obj_ref_t elem = interface.object_alloc(ti_elem);
        if (!elem) {
            printf("[Mock] error: failed to allocate element for RawArray<default:TestData>\n");
            return 1;
        }
        interface.set_array_ref_element(arr, i, elem);
    }

    CJArray* array_placeholder = (CJArray*)get_symbol_address("_CN7default17placeholder_arrayE");
    if (!array_placeholder) {
        printf("[Mock] error: symbol _CN7default17placeholder_arrayE not found\n");
        return 1;
    }
    interface.write_static_field((obj_ref_t)array_placeholder, arr); // array_placeholder->rawptr = arr
    array_placeholder->start = 0;
    array_placeholder->len = 10;
    printf("[Mock] wrote object to static field: %p\n", static_cast<void*>(array_placeholder));

    printf("[Mock] allocationTest done\n");

    return 0;
}

