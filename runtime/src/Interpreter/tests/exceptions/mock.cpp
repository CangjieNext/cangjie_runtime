#include <vector>
#include <atomic>
#include <cassert>
#include <dlfcn.h>
#include "mock.h"
#include "cangjieUtils.h"
#include "log.h"
#include "libinterpreter.h"
#include "utils.h"

extern MOCK_IMPLEMENTATION mockInstance;

#define STRING_SIZE 64
#define FRAME_ROOTS_N  8

using Ref = void*;
Ref frameRoots[FRAME_ROOTS_N];

static size_t originalInterpreterStackReserveSize = 0;
static bool hasOriginalInterpreterStackReserveSize = false;

extern "C" void restoreMockInterpreterStackReserveSize()
{
    if (!hasOriginalInterpreterStackReserveSize) {
        printf("[Mock] restoreMockInterpreterStackReserveSize: original reserve size was not saved\n");
        return;
    }

    g_InterpreterStackReserveSize = originalInterpreterStackReserveSize;
    hasOriginalInterpreterStackReserveSize = false;
    printf("[Mock] restored interpreter stack reserve size to %zu\n", g_InterpreterStackReserveSize);
}

ExceptionsMock::ExceptionsMock(INT_InterpreterInterface* interpreter_interf, DYN_CJNativeInterface* cjnative_interf)
    : Mock(interpreter_interf, cjnative_interf)
{
    interpreter_interf->iterateFramesWithState = &IterateFramesWithState;
    interpreter_interf->visitFrameRootsMarking = &markFrameRoots;
    interpreter_interf->visitFrameRootsAdjusting = &adjustFrameRoots;

    interpreter_interf->getFrameSize      = &getFrameSize; 
    interpreter_interf->frameDescProvider = &getInterpretedFrameDesc;
    interpreter_interf->frameInfoProvider = &getInterpretedFrameInfo;

    registerC2I(0, reinterpret_cast<uintptr_t>(throwOutException));
    registerC2I(1, reinterpret_cast<uintptr_t>(throwAndCatchException));
    registerC2I(2, reinterpret_cast<uintptr_t>(testCompiledThrower));
    registerC2I(3, reinterpret_cast<uintptr_t>(testI2IExceptions));
    registerC2I(4, reinterpret_cast<uintptr_t>(testArithmeticException));
    registerC2I(5, reinterpret_cast<uintptr_t>(testNoneValueException));
    registerC2I(6, reinterpret_cast<uintptr_t>(testOutOfMemoryError));
    registerC2I(7, reinterpret_cast<uintptr_t>(testStackOverflowPrepare));
    registerC2I(8, reinterpret_cast<uintptr_t>(testStackOverflow));
    registerC2I(9, reinterpret_cast<uintptr_t>(testDumpAllStackTrace));

    for (int i = 0; i < FRAME_ROOTS_N; i++) {
        frameRoots[i] = nullptr;
    }
}

void scanRefArray(DYN_RootVisitor visitor, Ref* roots, int size, std::function<void(DYN_RootVisitor, Ref*)> visit) {
    Ref* ph = roots;
    for (int i = 0; i < size; i++) {
        visit(visitor, ph);
        ph++;
    }
}

uint32_t ExceptionsMock::getFrameSize(DYN_FramePointer fp) {
    uint32_t frameSize = 1024 * 1024 * 1024;

    printf("[Mock] simulate allocation of very big frame: size = %u\n", frameSize);
    return frameSize;
}

void ExceptionsMock::IterateFramesWithState(DYN_CJThreadSpecificData cjThreadData, void (*callback)(DYN_VisitingState, void*), void* ctx) {
    log("[Mock] frames iterator called!");
    callback(nullptr /* not used */, ctx);
    log("[Mock] frames iterator finished!");
}

void ExceptionsMock::markFrameRoots(DYN_VisitingState state, INT_FrameDesc desc, DYN_RootVisitor visitor) {
    log("[Mock] frame: (ip=%p, fp=%p)", desc.ip, desc.fp);
    scanRefArray(visitor, static_cast<Ref*>(frameRoots), FRAME_ROOTS_N,
        [](DYN_RootVisitor visitor, Ref* ph){
            mockInstance.cjNativeInterface.visitRootFromInterpreter(visitor, ph);
        });
    log("[Mock] finished frame roots scanning");
}

void ExceptionsMock::adjustFrameRoots(DYN_VisitingState state, INT_FrameDesc desc, DYN_RootVisitor visitor, DYN_RootVisitor derivedPtrVisitor) {
    log("[Mock] frame: (ip=%p, fp=%p)", desc.ip, desc.fp);
    scanRefArray(visitor, static_cast<Ref*>(frameRoots), FRAME_ROOTS_N,
        [](DYN_RootVisitor visitor, Ref* ph){
            mockInstance.cjNativeInterface.visitRootFromInterpreter(visitor, ph);
        });
    log("[Mock] finished frame roots scanning");
}

void throwCatchableException() {
    printf("[Mock] throwCatchableException called\n");
    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.typeInfo("std.core:IllegalStateException");
    frameRoots[0] = interface.objectAlloc(ti);
    mockInstance.invokeI2C(reinterpret_cast<uintptr_t>(frameRoots[0]), 0, 0, 0, "_CNat21IllegalStateException6<init>Hv");
    interface.throwException(frameRoots[0]);
    printf("[Mock] throwCatchableException: OK\n");
}

void throwUncatchedException() {
    printf("[Mock] throwUncatchedException called\n");
    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.typeInfo("std.core:Exception");
    frameRoots[0] = interface.objectAlloc(ti);
    mockInstance.invokeI2C(reinterpret_cast<uintptr_t>(frameRoots[0]), 0, 0, 0, "_CNat9Exception6<init>Hv");
    interface.throwException(frameRoots[0]);
    printf("[Mock] throwUncatchedException: error, should not reach here\n");
}

void callCompiledThrower() {
    printf("[Mock] callCompiledThrower called\n");
    mockInstance.invokeI2C("_CN4test27i2cThrowOutFromCompiledCodeHv");
    printf("[Mock] callCompiledThrower: error, should not reach here\n");
}

void callCompiledThrowCatch() {
    printf("[Mock] callCompiledThrowCatch called\n");
    mockInstance.invokeI2C("_CN4test25i2cThrowAndCatchExceptionHv");
    printf("[Mock] callCompiledThrowCatch: OK\n");
}

void callCompiledThrowIllegalStateException() {
    printf("[Mock] callCompiledThrowIllegalStateException: start\n");
    mockInstance.invokeI2C("_CN4test29i2cThrowIllegalStateExceptionHv");
    printf("[Mock] callCompiledThrowIllegalStateException: OK\n");
}

void doI2IAndThrowCatchableException() {
    printf("[Mock] doI2IAndThrowCatchableException called\n");
    mockInstance.invokeI2I(reinterpret_cast<void*>(throwCatchableException));
    printf("[Mock] doI2IAndThrowCatchableException: OK\n");
}

void doI2IAndCallCompiledThrower() {
    printf("[Mock] doI2IAndCallCompiledThrower called\n");
    mockInstance.invokeI2I(reinterpret_cast<void*>(callCompiledThrower));
    printf("[Mock] doI2IAndCallCompiledThrower: error, should not reach here\n");
}

// OverflowException are also thrown like so
void doThrowArithmeticException() {
    printf("[Mock] doThrowArithmeticException called\n");
    CJString* emptyString = getEmptyString();
    frameRoots[0] = reinterpret_cast<Ref>(mockInstance.invokeI2C(reinterpret_cast<uintptr_t>(emptyString), 0, 0, 0, "rt$CreateArithmeticException_msg"));
    mockInstance.cjNativeInterface.throwException(frameRoots[0]);
    printf("[Mock] doThrowArithmeticException: error, should not reach here\n");
}

// std.core:IndexOutOfBoundsException and NegativeArraySize are also thrown like so
void doThrowNoneValueException() {
    printf("[Mock] doThrowNoneValueException called\n");
    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.typeInfo("std.core:NoneValueException");
    frameRoots[0] = interface.objectAlloc(ti);
    mockInstance.invokeI2C(reinterpret_cast<uintptr_t>(frameRoots[0]), 0, 0, 0, "_CNat18NoneValueException6<init>Hv");
    interface.throwException(frameRoots[0]);
    printf("[Mock] doThrowNoneValueException: error, should not reach here\n");
}

void doThrowOutOfMemoryError() {
    printf("[Mock] doThrowOutOfMemoryError called\n");
    auto interface = mockInstance.cjNativeInterface;
    interface.throwOom();
    printf("[Mock] doThrowOutOfMemoryError: error, should not reach here\n");
}

uint64_t getTopInterpretedFramePC() {
    return 0; // mocked implementation
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

// ---------------------------------------------------------

void ExceptionsMock::getInterpretedFrameInfo(DYN_FramePointer fp, DYN_InstructionPointer ip, INT_InterpretedFrameInfo* frameInfo) {
    printf("[Mock] getInterpretedFrameInfo called for (fp=%p, ip=%p)\n", fp, ip);
    frameInfo->fuh = reinterpret_cast<INT_FunctionHandle>(0xDEAD);
    frameInfo->bcPos = reinterpret_cast<INT_BytecodePos>(0xBEEF);
}

void freeResources(char* className, char* methodName, char* fileName) {
    delete[] className;
    delete[] methodName;
    delete[] fileName;
}

void ExceptionsMock::getInterpretedFrameDesc(INT_FunctionHandle fuh, INT_BytecodePos pos, INT_InterpretedFrameDesc* frameDesc) {
    printf("[Mock] getInterpretedFrameDesc called for (fuh=%p, pos=%p)\n", fuh, pos);
    check(reinterpret_cast<uint64_t>(fuh) == 0xDEAD, "incorrect fuh");
    check(reinterpret_cast<uint64_t>(pos) == 0xBEEF, "incorrect pos");

    char* className = new char[STRING_SIZE];
    snprintf(className, STRING_SIZE, "UnknownClass");

    char* methodName = new char[STRING_SIZE];
    snprintf(methodName, STRING_SIZE, "unknownMethod");

    char* fileName = new char[STRING_SIZE];
    snprintf(fileName, STRING_SIZE, "unknown.cj");

    frameDesc->lineNumber = 42;
    frameDesc->className = className;
    frameDesc->methodName = methodName;
    frameDesc->fileName = fileName;
    frameDesc->freeResources = freeResources;
}

void ExceptionsMock::throwOutException() {
    printf("[Mock] throwOutException called\n");
    throwUncatchedException();
    printf("[Mock] throwOutException: error\n");
}

void ExceptionsMock::throwAndCatchException() {
    printf("[Mock] throwAndCatchException called\n");
    throwCatchableException();
    printf("[Mock] throwAndCatchException -- OK\n");
}

void ExceptionsMock::testCompiledThrower() {
    printf("[Mock] callCompiledThrower called\n");
    callCompiledThrowCatch();
    callCompiledThrowIllegalStateException();
    callCompiledThrower();
    printf("[Mock] callCompiledThrower: error\n");
}

void ExceptionsMock::testI2IExceptions() {
    printf("[Mock] testI2IExceptions called\n");
    doI2IAndThrowCatchableException();
    doI2IAndCallCompiledThrower();
    printf("[Mock] testI2IExceptions: error\n");
}

void ExceptionsMock::testArithmeticException() {
    printf("[Mock] testArithmeticException called\n");
    doThrowArithmeticException();
    printf("[Mock] testArithmeticException: error\n");
}

void ExceptionsMock::testNoneValueException() {
    printf("[Mock] testNoneValueException called\n");
    doThrowNoneValueException();
    printf("[Mock] testNoneValueException: error\n");
}

void ExceptionsMock::testOutOfMemoryError() {
    printf("[Mock] testOutOfMemoryError called\n");
    doThrowOutOfMemoryError();
    printf("[Mock] testOutOfMemoryError: error\n");
}

void ExceptionsMock::dumpAllStackTrace()
{
    printf("[Mock] dumpAllStackTrace called through I2I\n");
    using DumpAllStackTraceFn = void (*)();
    auto dumpAllStackTrace = reinterpret_cast<DumpAllStackTraceFn>(dlsym(RTLD_DEFAULT, "CJ_MRT_DumpAllStackTrace"));
    if (dumpAllStackTrace == nullptr) {
        printf("[Mock] CJ_MRT_DumpAllStackTrace is unavailable\n");
        return;
    }
    dumpAllStackTrace();
    printf("[Mock] dumpAllStackTrace: OK\n");
}

void ExceptionsMock::testDumpAllStackTrace()
{
    printf("[Mock] testDumpAllStackTrace called through C2I\n");
    mockInstance.invokeI2I(reinterpret_cast<void*>(dumpAllStackTrace));
    printf("[Mock] testDumpAllStackTrace: OK\n");
}

void ExceptionsMock::testStackOverflowPrepare() {
    printf("[Mock] testStackOverflowPrepare called\n");
    if (!hasOriginalInterpreterStackReserveSize) {
        originalInterpreterStackReserveSize = g_InterpreterStackReserveSize;
        hasOriginalInterpreterStackReserveSize = true;
    }
    g_InterpreterStackReserveSize = 1024 * 1024 * 1024; // set reserve size to 1GB to trigger stack overflow on next c2i transition
}

void ExceptionsMock::testStackOverflow() {
    // Stack checks are done in prologue of all interpreted functions
    // Emulate it here manually
    StackCheck(mockInstance.cjNativeInterface.getThreadLocalData());

    // Exception will be thrown in stack check, frame will be unwound
    // by exception handing code in common landing pad

    printf("[Mock] testStackOverflow: error, should not reach here\n");
}
