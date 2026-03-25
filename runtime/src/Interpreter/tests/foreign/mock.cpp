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

// compaction test
Ref savedRoots[SAVED_ROOTS_N];
Ref adjustableRoots[SAVED_ROOTS_N];

Ref frameRoots[FRAME_ROOTS_N];


void scanRefArray(DYN_RootVisitor visitor, Ref* roots, int size, std::function<void(DYN_RootVisitor, Ref*)> visit) {
    Ref* ph = roots;
    for (int i = 0; i < size; i++) {
        visit(visitor, ph);
        ph++;
    }
}

ForeignTestMock::ForeignTestMock(INT_InterpreterInterface* interpreter_interf, DYN_CJNativeInterface* cjnative_interf)
    : Mock(interpreter_interf, cjnative_interf)
{
    interpreter_interf->visitGlobalRoots = &visitGlobalRoots;

    registerC2I(0, reinterpret_cast<uintptr_t>(testForeignCall));
    registerC2I(1, reinterpret_cast<uintptr_t>(testGCDuringForeignCall));
    registerC2I(2, reinterpret_cast<uintptr_t>(testCangjieCallback));
    registerC2I(3, reinterpret_cast<uintptr_t>(testCangjieCallbackThrowingException));
    registerC2I(4, reinterpret_cast<uintptr_t>(testI2NTransitionStackOverflow));
    registerC2I(9, reinterpret_cast<uintptr_t>(stop));
}

void ForeignTestMock::visitGlobalRoots(DYN_RootVisitor visitor) {
    printf("[Mock] visitGlobalRoots called!\n");
    scanRefArray(visitor, static_cast<Ref*>(frameRoots), FRAME_ROOTS_N,
        [](DYN_RootVisitor visitor, Ref* ph){
            mockInstance.cjNativeInterface.visitRootFromInterpreter(visitor, ph);
        });
    scanRefArray(visitor, static_cast<Ref*>(adjustableRoots), SAVED_ROOTS_N,
        [](DYN_RootVisitor visitor, Ref* ph){
            mockInstance.cjNativeInterface.visitRootFromInterpreter(visitor, ph);
        });
}

void ForeignTestMock::testForeignCall() {
    printf("[Mock] #0 testForeignCall: do foreign call from interpreter\n");
    auto interface = mockInstance.cjNativeInterface;
    DYN_ThreadLocalData thread_local_data = interface.getThreadLocalData();
    mockInstance.invokeI2N(0, 0, 0, 0, "foreignCanBeCalled", thread_local_data);
    printf("[Mock] #0 testForeignCall: finished\n");
}


void ForeignTestMock::testGCDuringForeignCall() {
    printf("[Mock] #1 testGCDuringForeignCall: do long-running foreign call from interpreter and see if local objects were moved during call\n");
    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.typeInfo("std.core:Object");

    printf("[Mock] filling adjustable roots\n");
    int saveEveryNth = 100;
    for (int i = 0; i < SAVED_ROOTS_N * saveEveryNth; i++) {
        if (i % saveEveryNth == 0) {
            interface.writeStaticField(&adjustableRoots[i / saveEveryNth], interface.objectAlloc(ti));
        } else {
            interface.objectAlloc(ti); // allocate junk so adjustable roots can be compacted
        }
    }
    memcpy(savedRoots, adjustableRoots, sizeof(Ref) * SAVED_ROOTS_N); // save the initial set of roots for later verification
    printf("[Mock]   adjustable roots filled\n");


    printf("[Mock] making foreign call\n");

    DYN_ThreadLocalData thread_local_data = interface.getThreadLocalData();
    mockInstance.invokeI2N(0, 0, 0, 0, "longRunningForeignCall", thread_local_data);

    printf("[Mock] returned from foreign call\n");

    bool isSomethingWasMoved = false;
    for (int i = 0; i < SAVED_ROOTS_N; i++) {
        auto old_real_address = getUntaggedAddress(savedRoots[i]);
        auto new_real_address = getUntaggedAddress(interface.readStaticField(&adjustableRoots[i]));
        if (new_real_address != old_real_address) {
            isSomethingWasMoved = true;
            log("[Mock] adjustable root %d was moved: old address = %p, new address = %p\n",
                i, (void*)old_real_address, (void*)new_real_address);
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
        DYN_ThreadLocalData thread_local_data = interface.getThreadLocalData();
        mockInstance.invokeI2N(1234, 0, 0, 0, "call_callback", thread_local_data);
        printf("[Mock] #2 testCangjieCallback: finished\n");
    }


    void ForeignTestMock::testCangjieCallbackThrowingException() {
        printf("[Mock] #3 testCangjieCallbackThrowingException: do foreign call which calls compiled @C callback which throws an exception\n");
        auto interface = mockInstance.cjNativeInterface;
        DYN_ThreadLocalData thread_local_data = interface.getThreadLocalData();
        mockInstance.invokeI2N(-1, 0, 0, 0, "call_callback", thread_local_data);
        printf("[Mock] #3 testCangjieCallbackThrowingException: error\n");
    }

    void ForeignTestMock::testI2NTransitionStackOverflow() {
        printf("[Mock] #4 testI2NTransitionStackOverflow: trigger SOE in I2N transition\n");
        auto interface = mockInstance.cjNativeInterface;
        DYN_ThreadLocalData thread_local_data = interface.getThreadLocalData();
        size_t oldReserveSize = g_InterpreterStackReserveSize;
        g_InterpreterStackReserveSize = 1024 * 1024 * 1024; // 1GB reserve to force stack-overflow check in I2N path.
        mockInstance.invokeI2N(0, 0, 0, 0, "foreignCanBeCalled", thread_local_data);
        g_InterpreterStackReserveSize = oldReserveSize;
        printf("[Mock] #4 testI2NTransitionStackOverflow: error, should not return\n");
    }


    void ForeignTestMock::stop() {
        bool* continue_allocation_placeholder = (bool*)get_symbol_address("_CN4test18continueAllocationE");
    if (!continue_allocation_placeholder) {
        printf("[Mock] error: symbol _CN4test18continueAllocationE not found\n");
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
bool interpretExceptionHandling(uint64_t pc, DYN_ObjRef exception) {
    frameRoots[2] = exception;
    auto interface = mockInstance.cjNativeInterface;
    printf("[Mock]     interpretExceptionHandling: pc=%p, exception=%p\n", (void*)pc, frameRoots[2]);

    if (interface.instanceOf(frameRoots[2], interface.typeInfo("std.core:IllegalStateException"))) {
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
    auto exceptionWrapper = interface.getExceptionWrapper();
    printf("[Mock]   got exception wrapper %p\n", exceptionWrapper);
    frameRoots[1] = interface.postThrowException(exceptionWrapper); // exception object
    printf("[Mock]   got exception object %lx\n", (uint64_t)frameRoots[1]);

    if (interpretExceptionHandling(getTopInterpretedFramePC(), frameRoots[1])) {
        printf("[Mock]   exception was handled, returning to normal execution\n");
        return;
    }

    // should not reach here
    printf("[Mock] error: landingPad, should not reach here\n");
}
