#include <vector>

#include "mock.h"
#include "utils.h"
#include "log.h"

extern MOCK_IMPLEMENTATION mockInstance;

using Ref = void*;

#define FRAME_ROOTS_N  8
#define GLOBAL_ROOTS_N 64

Ref frameRoots[FRAME_ROOTS_N];
Ref globalRoots[GLOBAL_ROOTS_N];

ScanningMock::ScanningMock(interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf)
    : Mock(interpreter_interf, cjnative_interf)
{
    interpreter_interf->iterate_frames_with_state = &IterateFramesWithState;
    interpreter_interf->visit_global_roots = &visitGlobalRoots;
    interpreter_interf->visit_frame_roots_marking = &markFrameRoots;
    interpreter_interf->visit_frame_roots_adjusting = &adjustFrameRoots;
    interpreter_interf->visit_frame_roots_expansion = &visitStackPtrs;
    interpreter_interf->fiber_start = &doOnFiberStart;

    registerC2I(0, reinterpret_cast<uintptr_t>(intermidiateFrames));
    registerC2I(1, reinterpret_cast<uintptr_t>(frameWithStackPtrSlot));
    registerC2I(8, reinterpret_cast<uintptr_t>(init));
}

////////////////// GC ROOTS SCANNING SUB-TEST LOGIC //////////////////

void scanRefArray(root_visitor_t visitor, Ref* roots, int size, std::function<void(root_visitor_t, Ref*)> visit) {
    Ref* ph = roots;
    for (int i = 0; i < size; i++) {
        visit(visitor, ph);
        ph++;
    }
}

volatile bool shouldLog = true;

void ScanningMock::IterateFramesWithState(fiber_specific_data_t fiberData, void (*callback)(visiting_state_t, void*), void* ctx) {
    if (shouldLog) log("[Mock] frames iterator called!");

    int counter = 0;

    visiting_state_t state = reinterpret_cast<visiting_state_t>(&counter);
    {
        callback(state, ctx);
    }
    if (shouldLog) log("[Mock] counted frames: %d", counter);

    if (shouldLog) log("[Mock] frames iterator finished!");
}

void ScanningMock::visitGlobalRoots(root_visitor_t visitor) {
    log("[Mock] started global roots scanning");
    scanRefArray(visitor, globalRoots, GLOBAL_ROOTS_N,
        [](root_visitor_t visitor, Ref* ph){
            mockInstance.cjNativeInterface.visit_root_from_interpreter(visitor, ph);
        });
    log("[Mock] finished global roots scanning");
}

void ScanningMock::markFrameRoots(visiting_state_t state, frame_desc_t frameDesc, root_visitor_t rootVisitor) {
    log("[Mock] frame: (ip=%p, fp=%p)", frameDesc.ip, frameDesc.fp);
    scanRefArray(rootVisitor, static_cast<Ref*>(frameRoots), FRAME_ROOTS_N,
        [](root_visitor_t visitor, Ref* ph){
            mockInstance.cjNativeInterface.visit_root_from_interpreter(visitor, ph);
        });

    (*static_cast<int*>(state))++;
    log("[Mock] finished frame roots scanning");
}

void ScanningMock::adjustFrameRoots(visiting_state_t state, frame_desc_t frameDesc, root_visitor_t rootVisitor, derived_ptr_visitor_t derivedPtrVisitor) {
    log("[Mock] frame: (ip=%p, fp=%p)", frameDesc.ip, frameDesc.fp);
    scanRefArray(rootVisitor, static_cast<Ref*>(frameRoots), FRAME_ROOTS_N,
        [](root_visitor_t visitor, Ref* ph){
            mockInstance.cjNativeInterface.visit_root_from_interpreter(visitor, ph);
        });

    (*static_cast<int*>(state))++;
    log("[Mock] finished frame roots scanning");
}

void ScanningMock::doOnFiberStart(fiber_specific_data_t* data) {
    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.type_info("std.core:Object");
    check(ti != nullptr, "typeinfo wasn't provided");
}

void ScanningMock::init() {
    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.type_info("std.core:Object");
    check(ti != nullptr, "typeinfo wasn't provided");

    for (int i = 0; i < GLOBAL_ROOTS_N; i++) {
        interface.write_static_field(&globalRoots[i], interface.object_alloc(ti));
    }

    for (int i = 0; i < FRAME_ROOTS_N; i++) {
        frameRoots[i] = interface.object_alloc(ti);
    }
}

void ScanningMock::intermidiateFrames() {
    static int count = 0;

    if (count == 5) {
        count = 0;
        mockInstance.invokeI2C("_CN7default5test1Hv");
        return;
    }

    count++;
    intermidiateFrames();
}

////////////////// STACK POINTERS SCANNING SUB-TEST LOGIC //////////////////

#define STACK_PTR_MAP_SIZE 8

volatile void* stackPtrMap[STACK_PTR_MAP_SIZE];


void ScanningMock::frameWithStackPtrSlot() {
    shouldLog = false; // disable loggging in IterateFramesWithState to avoid heavy stack usage during stack expansion

    volatile int localVar = 42;
    volatile int* stackPtr = &localVar;

    // Remember the address of the stack slot that contains a pointer to stack
    stackPtrMap[0] = &stackPtr;

    printf("[Mock] old slot = %p, ptr = %p\n", &stackPtr, stackPtr);
    {
        mockInstance.invokeI2C("_CN7default5test2Hv");
    }
    printf("[Mock] new slot = %p, ptr = %p\n", &stackPtr, stackPtr);

    check(&stackPtr != stackPtrMap[0], "stack was not expanded\n");
    check(stackPtr == &localVar,       "stack pointer was not adjusted correctly after expansion\n");
    check(*(stackPtr) == 42,           "local variable value was not preserved after stack expansion\n");
}

void ScanningMock:: visitStackPtrs(visiting_state_t state, frame_desc_t frameDesc, root_visitor_t stackPtrVisitor, derived_ptr_visitor_t derivedPtrVisitor) {
    for (int i = 0; i < STACK_PTR_MAP_SIZE; i++) {
        if (stackPtrMap[i] != nullptr) {
            mockInstance.cjNativeInterface.visit_root_from_interpreter(stackPtrVisitor, const_cast<void*>(stackPtrMap[i]));
        }
    }
}
