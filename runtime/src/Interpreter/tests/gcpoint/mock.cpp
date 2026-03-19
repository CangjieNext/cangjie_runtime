#include <vector>
#include <atomic>
#include "mock.h"
#include "log.h"

extern MOCK_IMPLEMENTATION mockInstance;

class FiberMagicData {
public:
    uint64_t magicValue;
    uint64_t fiberId;
    FiberMagicData(uint64_t magicValue, uint64_t fiberId) {
        this->magicValue = magicValue;
        this->fiberId = fiberId;
    }
};

GCPointMock::GCPointMock(interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf)
    : Mock(interpreter_interf, cjnative_interf)
{
    interpreter_interf->visit_fiber_roots = &visitFiberRoots;
    interpreter_interf->fiber_start = &doOnFiberStart;

    registerC2I(0, reinterpret_cast<uintptr_t>(pollSafepoints));
}

void GCPointMock::visitFiberRoots(root_visitor_t visitor, fiber_specific_data_t data) {
    auto magic = static_cast<FiberMagicData*>(data);
    if (magic->magicValue != 0xCAFEBABE) {
        printf("[Interpreter] fiber: magic value mismatch (expected: 0xCAFEBABE, got: 0x%016lX)\n", 
            magic->magicValue);
        return;
    }
    auto fiberId = magic->fiberId;
    printf("[Interpreter] visitFiberRoots: fiber id = %ld\n", fiberId);
}


std::atomic<int64_t> fibers_count(0);

void GCPointMock::doOnFiberStart(fiber_specific_data_t* data) {
    printf("[Interpreter] doOnFiberStart: data = %p\n", data);
    fibers_count.fetch_add(1);
    auto fiberId = fibers_count.load();
    FiberMagicData* magicData = new FiberMagicData(0xCAFEBABE, fiberId);
    *data = magicData;
    printf("[Interpreter] doOnFiberStart: fiber id = %ld\n", fiberId);
}

void GCPointMock::pollSafepoints() {
    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.type_info("std.core:Object");

    printf("[Interpreter] pollSafepoints: allocating objects and poll safepoint...\n");

    for (int i = 0; i < 100000000; i++) {
        interface.safe_point();
    }

    printf("[Interpreter] pollSafepoints: finished\n");
}
