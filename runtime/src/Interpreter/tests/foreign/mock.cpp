#include <vector>
#include <atomic>
#include <dlfcn.h>
#include <cstring>
#include <iostream>
#include "mock.h"
#include "log.h"

extern MOCK_IMPLEMENTATION mockInstance;
using Ref = void*;

#define FRAME_ROOTS_N  8
#define SAVED_ROOTS_N  100
#define ITERATIONS 20000000

// compaction test
Ref savedRoots[SAVED_ROOTS_N];
Ref adjustableRoots[SAVED_ROOTS_N];

Ref frameRoots[FRAME_ROOTS_N];


void scanRefArray(root_visitor_t visitor, Ref* roots, int size, std::function<void(root_visitor_t, Ref*)> visit) {
    Ref* ph = roots;
    for (int i = 0; i < size; i++) {
        visit(visitor, ph);
        ph++;
    }
}

ForeignTestMock::ForeignTestMock(int a1, int a2, interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf)
    : Mock(a1, a2, interpreter_interf, cjnative_interf) 
{
    interpreter_interf->visit_global_roots = &visitGlobalRoots;

    registerC2I(0, reinterpret_cast<uintptr_t>(testForeignCall));
    registerC2I(1, reinterpret_cast<uintptr_t>(testGCDuringForeignCall));
    registerC2I(2, reinterpret_cast<uintptr_t>(testCangjieCallback));
    registerC2I(3, reinterpret_cast<uintptr_t>(testCangjieCallbackThrowingException));
    registerC2I(9, reinterpret_cast<uintptr_t>(stop));
}

void ForeignTestMock::visitGlobalRoots(root_visitor_t visitor) {
    printf("[Mock] visitGlobalRoots called!\n");
    scanRefArray(visitor, static_cast<Ref*>(frameRoots), FRAME_ROOTS_N,
        [](root_visitor_t visitor, Ref* ph){
            mockInstance.cjNativeInterface.visit_frame_root_from_interpreter(visitor, ph);
        });
    scanRefArray(visitor, static_cast<Ref*>(adjustableRoots), SAVED_ROOTS_N,
        [](root_visitor_t visitor, Ref* ph){
            mockInstance.cjNativeInterface.visit_frame_root_from_interpreter(visitor, ph);
        });
}

void ForeignTestMock::testForeignCall() {
    printf("[Mock] #0 testForeignCall: do foreign call from interpreter\n");
    auto interface = mockInstance.cjNativeInterface;
    ThreadLocalDataMirror* thread_local_data = reinterpret_cast<ThreadLocalDataMirror*>(interface.get_thread_local_data());
    mockInstance.invokeI2N(0, 0, 0, 0, "foreignCanBeCalled", thread_local_data);
    printf("[Mock] #0 testForeignCall: finished\n");
}


void ForeignTestMock::testGCDuringForeignCall() {
    printf("[Mock] #1 testGCDuringForeignCall: do long-running foreign call from interpreter and see if local objects were moved during call\n");
    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.type_info("std.core:Object");

    printf("[Mock] filling adjustable roots\n");
    int saveEveryNth = 100;
    for (int i = 0; i < SAVED_ROOTS_N * saveEveryNth; i++) {
        if (i % saveEveryNth == 0) {
            adjustableRoots[i / saveEveryNth] = interface.object_alloc(ti);
        } else {
            interface.object_alloc(ti); // allocate junk so adjustable roots can be compacted
        }
    }
    memcpy(savedRoots, adjustableRoots, sizeof(Ref) * SAVED_ROOTS_N); // save the initial set of roots for later verification
    printf("[Mock]   adjustable roots filled\n");


    printf("[Mock] making foreign call\n");

    ThreadLocalDataMirror* thread_local_data = reinterpret_cast<ThreadLocalDataMirror*>(interface.get_thread_local_data());
    mockInstance.invokeI2N(0, 0, 0, 0, "longRunningForeignCall", thread_local_data);

    printf("[Mock] returned from foreign call\n");
    
    bool isSomethingWasMoved = false;
    for (int i = 0; i < SAVED_ROOTS_N; i++) {
        auto old_real_address = getUntaggedAddress(savedRoots[i]);
        auto new_real_address = getUntaggedAddress(adjustableRoots[i]);
        if (new_real_address != old_real_address) {
            isSomethingWasMoved = true;
            log("[Mock] adjustable root %d was moved: old address = %p, new address = %p\n", 
                i, (void*)old_real_address, (void*)new_real_address);
            }
            if (!interface.is_valid_object(adjustableRoots[i])) {
                printf("[Mock] error: adjustable root %d is invalid after compaction\n", i);
                return;
            }
        }
        if (isSomethingWasMoved) {
            printf("[Mock] objects were compacted: OK\n");
            printf("[Mock] #1 testGCDuringForeignCall: OK\n");
        } else {
            printf("[Mock] objects were not compacted\n");
            printf("[Mock] #1 testGCDuringForeignCall: error?\n");
        }
    }
    
    
    void ForeignTestMock::testCangjieCallback() {
        printf("[Mock] #2 testCangjieCallback: do foreign call which calls compiled @C callback\n");
        auto interface = mockInstance.cjNativeInterface;
        ThreadLocalDataMirror* thread_local_data = reinterpret_cast<ThreadLocalDataMirror*>(interface.get_thread_local_data());
        mockInstance.invokeI2N(1234, 0, 0, 0, "call_callback", thread_local_data);
        printf("[Mock] #2 testCangjieCallback: finished\n");
    }
    
    
    void ForeignTestMock::testCangjieCallbackThrowingException() {
        printf("[Mock] #3 testCangjieCallbackThrowingException: do foreign call which calls compiled @C callback which throws an exception\n");
        auto interface = mockInstance.cjNativeInterface;
        ThreadLocalDataMirror* thread_local_data = reinterpret_cast<ThreadLocalDataMirror*>(interface.get_thread_local_data());
        mockInstance.invokeI2N(-1, 0, 0, 0, "call_callback", thread_local_data);
        printf("[Mock] #3 testCangjieCallbackThrowingException: error\n");
    }
    
    
    void ForeignTestMock::stop() {
        bool* continue_allocation_placeholder = (bool*)get_symbol_address("_CN7default18continueAllocationE");
    if (!continue_allocation_placeholder) {
        printf("[Mock] error: symbol _CN7default18continueAllocationE not found\n");
        return;
    }
    *continue_allocation_placeholder = false;
}


/**
 * Simulates exception handling in the interpreter.
 * 
 * returns 
 *     - true if exception is handled
 *     - if exception cannot be handled, unwinds frames including interpreted function frame and rethrows exception.
 */
bool interpretExceptionHandling(uint64_t pc, obj_ref_t exception) {
    frameRoots[2] = exception;
    auto interface = mockInstance.cjNativeInterface;
    printf("[Mock]     interpretExceptionHandling: pc=%p, exception=%p\n", (void*)pc, frameRoots[2]);
    
    if (interface.instance_of(frameRoots[2], interface.type_info("std.core:IllegalStateException"))) {
        printf("[Mock]     simulate handling IllegalStateException ...\n");
        return true;
    }
    printf("[Mock]     cannot handle exception, unwinding and rethrowing ...\n");
    unwindInterpretedFrameAndRethrowException(frameRoots[2]);
    assert(false); // should not return here
}

uint64_t getTopInterpretedFramePC() {
    return 0; // mocked implementation
}

extern "C" void landingPad() {
    // now state before calling "throw" is restored except that we are in landing pad function instead of original one, 
    // and return address on stack points to the next instruction after call that threw exception.
    printf("[Mock] landingPad called\n");
    auto interface = mockInstance.cjNativeInterface;
    auto exceptionWrapper = interface.get_exception_wrapper();
    printf("[Mock]   got exception wrapper %p\n", exceptionWrapper);
    frameRoots[1] = interface.post_throw_exception(exceptionWrapper); // exception object
    printf("[Mock]   got exception object %lx\n", (uint64_t)frameRoots[1]);

    if (interpretExceptionHandling(getTopInterpretedFramePC(), frameRoots[1])) {
        printf("[Mock]   exception was handled, returning to normal execution\n");
        return;
    }

    // should not reach here
    printf("[Mock] error: landingPad, should not reach here\n"); 
}
