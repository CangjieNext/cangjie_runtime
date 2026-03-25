#include "mock.h"
#include "cangjieUtils.h"
#include <dlfcn.h>
#include <cstddef>
#include <cstdio>

extern MOCK_IMPLEMENTATION mockInstance;

AllocationMock::AllocationMock(INT_InterpreterInterface* interpreter_interf, DYN_CJNativeInterface* cjnative_interf)
    : Mock(interpreter_interf, cjnative_interf)
{
    registerC2I(0, reinterpret_cast<uintptr_t>(allocationTest));
    printf("[Mock] AllocationMock created\n");
}

void AllocationMock::allocationTest()
{
    printf("[Mock] allocationTest\n");

    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.typeInfo("std.core:Object");
    printf("[Mock] type info = %p\n", static_cast<void*>(ti));
    if (!ti) {
        printf("[Mock] error: type info std.core:Object not found\n");
        return;
    }

    DYN_ObjRef obj = interface.objectAlloc(ti);
    if (!obj) {
        printf("[Mock] error: failed to allocate std.core:Object\n");
        return;
    } else {
        printf("[Mock] allocated object: %p\n", static_cast<void*>(obj));
    }
    void* addr = get_symbol_address("_CN4test17to_be_set_in_mockE");
    if (!addr) {
        printf("[Mock] error: symbol _CN4test17to_be_set_in_mockE not found\n");
        return;
    }
    interface.writeStaticField(addr, obj);
    printf("[Mock] wrote object to static field: %p\n", static_cast<void*>(addr));

    auto ti2 = interface.typeInfo("RawArray<test:TestData>");
    printf("[Mock] type info RawArray<test:TestData> = %p\n", static_cast<void*>(ti2));

    if (!ti2) {
        printf("[Mock] error: type info RawArray<test:TestData> not found\n");
        return;
    }
    DYN_ObjRef arr = interface.arrayAlloc(ti2, 10);
    if (!arr) {
        printf("[Mock] error: failed to allocate RawArray<test:TestData>\n");
        return;
    } else {
        printf("[Mock] allocated RawArray<test:TestData>: %p\n", static_cast<void*>(arr));
    }
    auto ti_elem = interface.typeInfo("test:TestData");
    if (!ti_elem) {
        printf("[Mock] error: type info test:TestData not found\n");
        return;
    }
    for (size_t i = 0; i < 10; i++) {
        DYN_ObjRef elem = interface.objectAlloc(ti_elem);
        if (!elem) {
            printf("[Mock] error: failed to allocate element for RawArray<test:TestData>\n");
            return;
        }
        interface.setArrayRefElement(arr, i, elem);
    }

    CJArray* array_placeholder = (CJArray*)get_symbol_address("_CN4test17placeholder_arrayE");
    if (!array_placeholder) {
        printf("[Mock] error: symbol _CN4test17placeholder_arrayE not found\n");
        return;
    }
    interface.writeStaticField((DYN_ObjRef)array_placeholder, arr); // array_placeholder->rawptr = arr
    array_placeholder->start = 0;
    array_placeholder->len = 10;
    printf("[Mock] wrote object to static field: %p\n", static_cast<void*>(array_placeholder));

    printf("[Mock] allocationTest done\n");

    return;
}

