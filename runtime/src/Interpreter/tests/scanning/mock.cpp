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

ScanningMock::ScanningMock(INT_InterpreterInterface* interpreter_interf, DYN_CJNativeInterface* cjnative_interf)
    : Mock(interpreter_interf, cjnative_interf)
{
    interpreter_interf->iterateFramesWithState = &IterateFramesWithState;
    interpreter_interf->visitGlobalRoots = &visitGlobalRoots;
    interpreter_interf->visitFrameRootsMarking = &markFrameRoots;
    interpreter_interf->visitFrameRootsAdjusting = &adjustFrameRoots;
    interpreter_interf->visitFrameRootsExpansion = &visitStackPtrs;
    interpreter_interf->cjThreadOnStart = &doOnCJThreadStart;

    registerC2I(0, reinterpret_cast<uintptr_t>(intermidiateFrames));
    registerC2I(1, reinterpret_cast<uintptr_t>(frameWithStackPtrSlot));
    registerC2I(8, reinterpret_cast<uintptr_t>(init));
}

////////////////// GC ROOTS SCANNING SUB-TEST LOGIC //////////////////

void scanRefArray(DYN_RootVisitor visitor, Ref* roots, int size, std::function<void(DYN_RootVisitor, Ref*)> visit) {
    Ref* ph = roots;
    for (int i = 0; i < size; i++) {
        visit(visitor, ph);
        ph++;
    }
}

volatile bool shouldLog = true;

void ScanningMock::IterateFramesWithState(DYN_CJThreadSpecificData cjThreadData, void (*callback)(DYN_VisitingState, void*), void* ctx) {
    if (shouldLog) log("[Mock] frames iterator called!");

    int counter = 0;

    DYN_VisitingState state = reinterpret_cast<DYN_VisitingState>(&counter);
    {
        callback(state, ctx);
    }
    if (shouldLog) log("[Mock] counted frames: %d", counter);

    if (shouldLog) log("[Mock] frames iterator finished!");
}

void ScanningMock::visitGlobalRoots(DYN_RootVisitor visitor) {
    log("[Mock] started global roots scanning");
    scanRefArray(visitor, globalRoots, GLOBAL_ROOTS_N,
        [](DYN_RootVisitor visitor, Ref* ph){
            mockInstance.cjNativeInterface.visitRootFromInterpreter(visitor, ph);
        });
    log("[Mock] finished global roots scanning");
}

void ScanningMock::markFrameRoots(DYN_VisitingState state, INT_FrameDesc frameDesc, DYN_RootVisitor rootVisitor) {
    log("[Mock] frame: (ip=%p, fp=%p)", frameDesc.ip, frameDesc.fp);
    scanRefArray(rootVisitor, static_cast<Ref*>(frameRoots), FRAME_ROOTS_N,
        [](DYN_RootVisitor visitor, Ref* ph){
            mockInstance.cjNativeInterface.visitRootFromInterpreter(visitor, ph);
        });

    (*static_cast<int*>(state))++;
    log("[Mock] finished frame roots scanning");
}

void ScanningMock::adjustFrameRoots(DYN_VisitingState state, INT_FrameDesc frameDesc, DYN_RootVisitor rootVisitor, DYN_DerivedPtrVisitor derivedPtrVisitor) {
    log("[Mock] frame: (ip=%p, fp=%p)", frameDesc.ip, frameDesc.fp);
    scanRefArray(rootVisitor, static_cast<Ref*>(frameRoots), FRAME_ROOTS_N,
        [](DYN_RootVisitor visitor, Ref* ph){
            mockInstance.cjNativeInterface.visitRootFromInterpreter(visitor, ph);
        });

    (*static_cast<int*>(state))++;
    log("[Mock] finished frame roots scanning");
}

void ScanningMock::doOnCJThreadStart(DYN_CJThreadSpecificData* data) {
    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.typeInfo("std.core:Object");
    check(ti != nullptr, "typeinfo wasn't provided");
}

void ScanningMock::init() {
    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.typeInfo("std.core:Object");
    check(ti != nullptr, "typeinfo wasn't provided");

    for (int i = 0; i < GLOBAL_ROOTS_N; i++) {
        interface.writeStaticField(&globalRoots[i], interface.objectAlloc(ti));
    }

    for (int i = 0; i < FRAME_ROOTS_N; i++) {
        frameRoots[i] = interface.objectAlloc(ti);
    }
}

void ScanningMock::intermidiateFrames() {
    static int count = 0;

    if (count == 5) {
        count = 0;
        mockInstance.invokeI2C("_CN4test5test1Hv");
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
        mockInstance.invokeI2C("_CN4test5test2Hv");
    }
    printf("[Mock] new slot = %p, ptr = %p\n", &stackPtr, stackPtr);

    check(&stackPtr != stackPtrMap[0], "stack was not expanded\n");
    check(stackPtr == &localVar,       "stack pointer was not adjusted correctly after expansion\n");
    check(*(stackPtr) == 42,           "local variable value was not preserved after stack expansion\n");
}

void ScanningMock:: visitStackPtrs(DYN_VisitingState state, INT_FrameDesc frameDesc, DYN_RootVisitor stackPtrVisitor, DYN_DerivedPtrVisitor derivedPtrVisitor) {
    for (int i = 0; i < STACK_PTR_MAP_SIZE; i++) {
        if (stackPtrMap[i] != nullptr) {
            mockInstance.cjNativeInterface.visitRootFromInterpreter(stackPtrVisitor, const_cast<void*>(stackPtrMap[i]));
        }
    }
}
