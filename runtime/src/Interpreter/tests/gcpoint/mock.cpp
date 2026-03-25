#include <vector>
#include <atomic>
#include "mock.h"
#include "log.h"
#include "utils.h"

extern MOCK_IMPLEMENTATION mockInstance;

#define ITERATIONS CJ_INTERPRETER_TEST_ITER(100000000)

class CJThreadMagicData {
public:
    uint64_t magicValue;
    uint64_t cjThreadId;
    CJThreadMagicData(uint64_t magicValue, uint64_t cjThreadId) {
        this->magicValue = magicValue;
        this->cjThreadId = cjThreadId;
    }
};

GCPointMock::GCPointMock(INT_InterpreterInterface* interpreter_interf, DYN_CJNativeInterface* cjnative_interf)
    : Mock(interpreter_interf, cjnative_interf)
{

    interpreter_interf->iterateFramesWithState = &IterateFramesWithState;
    interpreter_interf->visitFrameRootsMarking = &markFrameRoots;

    interpreter_interf->cjThreadOnStart = &doOnCJThreadStart;

    registerC2I(0, reinterpret_cast<uintptr_t>(pollSafepoints));
}

void GCPointMock::IterateFramesWithState(DYN_CJThreadSpecificData cjThreadData, void (*callback)(DYN_VisitingState, void*), void* ctx) {
    log("[Mock] frames iterator called!");
    callback(cjThreadData, ctx);
    log("[Mock] frames iterator finished!");
}

void GCPointMock::markFrameRoots(DYN_VisitingState state, INT_FrameDesc desc, DYN_RootVisitor visitor) {
    auto magic = static_cast<CJThreadMagicData*>(state);

    check(magic->magicValue == 0xCAFEBABE,
        "[Mock] cjThread: magic value mismatch (expected: 0xCAFEBABE, got: 0x%016lX)\n",
        magic->magicValue);

    printf("[Mock] visitCJThreadRoots: cjThread id = %ld\n", magic->cjThreadId);
}

std::atomic<int64_t> cjThreads_count(0);

void GCPointMock::doOnCJThreadStart(DYN_CJThreadSpecificData* data) {
    printf("[Mock] doOnCJThreadStart: data = %p\n", data);
    cjThreads_count.fetch_add(1);
    auto cjThreadId = cjThreads_count.load();
    CJThreadMagicData* magicData = new CJThreadMagicData(0xCAFEBABE, cjThreadId);
    *data = magicData;
    printf("[Mock] doOnCJThreadStart: cjThread id = %ld\n", cjThreadId);
}

void GCPointMock::pollSafepoints() {
    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.typeInfo("std.core:Object");

    printf("[Mock] pollSafepoints: allocating objects and call safePoint...\n");

    for (int i = 0; i < ITERATIONS; i++) {
        interface.safePoint();
    }

    printf("[Mock] pollSafepoints: check if safepoint is pending and call safePoint...\n");

    for (int i = 0; i < ITERATIONS; i++) {
        if (interface.isPendingSafePoint(interface.getThreadLocalData())) {
            interface.safePoint();
        }
    }

    printf("[Mock] pollSafepoints: finished\n");
}
