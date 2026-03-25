#include <vector>
#include <atomic>
#include "mock.h"
#include "log.h"
#include "utils.h"

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

    interpreter_interf->iterate_frames_with_state = &IterateFramesWithState;
    interpreter_interf->visit_frame_roots_marking = &markFrameRoots;

    interpreter_interf->fiber_start = &doOnFiberStart;

    registerC2I(0, reinterpret_cast<uintptr_t>(pollSafepoints));
}

void GCPointMock::IterateFramesWithState(fiber_specific_data_t fiberData, void (*callback)(visiting_state_t, void*), void* ctx) {
    log("[Mock] frames iterator called!");
    callback(fiberData, ctx);
    log("[Mock] frames iterator finished!");
}

void GCPointMock::markFrameRoots(visiting_state_t state, frame_desc_t desc, root_visitor_t visitor) {
    auto magic = static_cast<FiberMagicData*>(state);

    check(magic->magicValue == 0xCAFEBABE,
        "[Interpreter] fiber: magic value mismatch (expected: 0xCAFEBABE, got: 0x%016lX)\n",
        magic->magicValue);

    printf("[Interpreter] visitFiberRoots: fiber id = %ld\n", magic->fiberId);
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

    printf("[Interpreter] pollSafepoints: allocating objects and call safe_point...\n");

    for (int i = 0; i < 70000000; i++) {
        interface.safe_point();
    }

    printf("[Interpreter] pollSafepoints: check if safepoint is pending and call safe_point...\n");

    for (int i = 0; i < 70000000; i++) {
        if (interface.is_pending_safe_point(interface.get_thread_local_data())) {
            interface.safe_point();
        }
    }

    printf("[Interpreter] pollSafepoints: finished\n");
}
