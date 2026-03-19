#include <vector>

#include "mock.h"
#include "utils.h"
#include "log.h"

extern MOCK_IMPLEMENTATION mockInstance;

using Ref = void*;

#define FIBER_ROOTS_N  8
#define FRAME_ROOTS_N  8
#define GLOBAL_ROOTS_N 64

Ref frameRoots[FRAME_ROOTS_N];
Ref globalRoots[GLOBAL_ROOTS_N];

ScanningMock::ScanningMock(interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf)
    : Mock(interpreter_interf, cjnative_interf)
{
    interpreter_interf->visit_global_roots = &visitGlobalRoots;
    interpreter_interf->visit_fiber_roots = &visitFiberRoots;
    interpreter_interf->visit_frame_roots = &visitFrameRoots;
    interpreter_interf->visit_stack_ptrs = &visitStackPtrs;
    interpreter_interf->fiber_start = &doOnFiberStart;

    registerC2I(0, reinterpret_cast<uintptr_t>(intermidiateFrames));
    registerC2I(1, reinterpret_cast<uintptr_t>(saveSomePointerToStack));
    registerC2I(2, reinterpret_cast<uintptr_t>(checkIsPointerAdjusted));
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

void ScanningMock::visitGlobalRoots(root_visitor_t visitor) {
    log("[Mock] started global roots scanning");
    scanRefArray(visitor, globalRoots, GLOBAL_ROOTS_N,
        [](root_visitor_t visitor, Ref* ph){
            mockInstance.cjNativeInterface.visit_global_root_from_interpreter(visitor, ph);
        });
    log("[Mock] finished global roots scanning");
}

void ScanningMock::visitFiberRoots(root_visitor_t visitor, fiber_specific_data_t data) {
    log("[Mock] started fiber roots scanning");
    scanRefArray(visitor, static_cast<Ref*>(data), FIBER_ROOTS_N,
        [](root_visitor_t visitor, Ref* ph){
            mockInstance.cjNativeInterface.visit_global_root_from_interpreter(visitor, ph);
        });
    log("[Mock] finished fiber roots scanning");
}

void ScanningMock::visitFrameRoots(root_visitor_t visitor, frame_desc_t desc) {
    log("[Mock] frame: (ip=%p, fp=%p)", desc.ip, desc.fp);
    scanRefArray(visitor, static_cast<Ref*>(frameRoots), FRAME_ROOTS_N,
        [](root_visitor_t visitor, Ref* ph){
            mockInstance.cjNativeInterface.visit_frame_root_from_interpreter(visitor, ph);
        });
    log("[Mock] finished frame roots scanning");
}

void ScanningMock::doOnFiberStart(fiber_specific_data_t* data) {
    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.type_info("std.core:Object");
    check(ti != nullptr, "typeinfo wasn't provided");

    Ref* roots = new Ref[8];
    for (int i = 0; i < FIBER_ROOTS_N; i++) {
        roots[i] = interface.object_alloc(ti);
    }

    *data = static_cast<fiber_specific_data_t>(roots);
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
        mockInstance.invokeI2C("_CN7default4testHv");
        return;
    }

    count++;
    intermidiateFrames();
}

////////////////// STACK POINTERS SCANNING SUB-TEST LOGIC //////////////////

/**
 * This is the placeholder that contains pointer to Cangjie thread stack slot.
 * The containing value should be adjusted when stack is expanding. 
 */
volatile uintptr_t stackPtrContainer;

volatile bool changed = false;

void ScanningMock::saveSomePointerToStack() {
    uintptr_t rsp_val;
    asm("mov %%rbp, %0" : "=r"(rsp_val));

    stackPtrContainer = rsp_val;
    log("[Mock] old stack ptr = %p", (void*) stackPtrContainer);
}

void ScanningMock:: visitStackPtrs(stack_ptr_visitor_t visitor, fiber_specific_data_t interp_fiber_data, stack_ptr_slot_type_t slot_type) {
    log("[Mock] started stack pointers scanning");

    if (slot_type == OutOfStack) {
        auto before = stackPtrContainer;
        mockInstance.cjNativeInterface.visit_stack_ptr_from_interpreter(visitor, (uintptr_t*)(&stackPtrContainer));
        auto after = stackPtrContainer;

        printf("Before: %p\n", (void*) before);
        printf("After: %p\n",  (void*) after);

        if (before != after) {
            changed = true;
        }
    }

    if (slot_type == OnStack) {
        log("[Mock] no on-stack slots in mock");
    }

    log("[Mock] finished stack pointers scanning");
}

void ScanningMock::checkIsPointerAdjusted() {
    check(changed, "Stack pointer value isn't changed");
}
