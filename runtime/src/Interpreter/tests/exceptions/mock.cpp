#include <vector>
#include <atomic>
#include <cassert>
#include "mock.h"
#include "cangjieUtils.h"
#include "log.h"
#include "libinterpreter.h"

extern MOCK_IMPLEMENTATION mockInstance;


#define FRAME_ROOTS_N  8

using Ref = void*;
Ref frameRoots[FRAME_ROOTS_N];

ExceptionsMock::ExceptionsMock(int a1, int a2, interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf)
    : Mock(a1, a2, interpreter_interf, cjnative_interf) 
{
    interpreter_interf->visit_frame_roots = &visitFrameRoots;
    interpreter_interf->interpretNoParams = &doOnEntrypoint;

    interpreter_interf->frame_info_provider = &getInterpretedFrameInfo;

    registerC2I(0, reinterpret_cast<uintptr_t>(throwOutException));
    registerC2I(1, reinterpret_cast<uintptr_t>(throwAndCatchException));
    registerC2I(2, reinterpret_cast<uintptr_t>(testCompiledThrower));
    registerC2I(3, reinterpret_cast<uintptr_t>(testI2IExceptions));
    registerC2I(4, reinterpret_cast<uintptr_t>(testArithmeticException));
    registerC2I(5, reinterpret_cast<uintptr_t>(testNoneValueException));
    registerC2I(6, reinterpret_cast<uintptr_t>(testOutOfMemoryError));
    registerC2I(7, reinterpret_cast<uintptr_t>(testStackOverflowPrepare));
    registerC2I(8, reinterpret_cast<uintptr_t>(testStackOverflow));
}

void scanRefArray(root_visitor_t visitor, Ref* roots, int size, std::function<void(root_visitor_t, Ref*)> visit) {
    Ref* ph = roots;
    for (int i = 0; i < size; i++) {
        visit(visitor, ph);
        ph++;
    }
}

void ExceptionsMock::visitFrameRoots(root_visitor_t visitor, frame_desc_t desc) {
    log("[Mock] frame: (ip=%p, fp=%p)", desc.ip, desc.fp);
    scanRefArray(visitor, static_cast<Ref*>(frameRoots), FRAME_ROOTS_N,
        [](root_visitor_t visitor, Ref* ph){
            mockInstance.cjNativeInterface.visit_frame_root_from_interpreter(visitor, ph);
        });
    log("[Mock] finished frame roots scanning");
}

int ExceptionsMock::doOnEntrypoint(char* func_name) {
    for (int i = 0; i < FRAME_ROOTS_N; i++) {
        frameRoots[i] = nullptr;
    }

    return 0;
}

void throwCatchableException() {
    printf("[Mock] throwCatchableException called\n");
    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.type_info("std.core:IllegalStateException");
    frameRoots[0] = interface.object_alloc(ti);
    mockInstance.invokeI2C(reinterpret_cast<uintptr_t>(frameRoots[0]), 0, 0, 0, "_CNat21IllegalStateException6<init>Hv");
    interface.throw_exception(frameRoots[0]);
    printf("[Mock] throwCatchableException: OK\n");
}

void throwUncatchedException() {
    printf("[Mock] throwUncatchedException called\n");
    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.type_info("std.core:Exception");
    frameRoots[0] = interface.object_alloc(ti);
    mockInstance.invokeI2C(reinterpret_cast<uintptr_t>(frameRoots[0]), 0, 0, 0, "_CNat9Exception6<init>Hv");
    interface.throw_exception(frameRoots[0]);
    printf("[Mock] throwUncatchedException: error, should not reach here\n");
}

void callCompiledThrower() {
    printf("[Mock] callCompiledThrower called\n");
    mockInstance.invokeI2C("_CN7default27i2cThrowOutFromCompiledCodeHv");
    printf("[Mock] callCompiledThrower: error, should not reach here\n");
}

void callCompiledThrowCatch() {
    printf("[Mock] callCompiledThrowCatch called\n");
    mockInstance.invokeI2C("_CN7default25i2cThrowAndCatchExceptionHv");
    printf("[Mock] callCompiledThrowCatch: OK\n");
}

void callCompiledThrowIllegalStateException() {
    printf("[Mock] callCompiledThrowIllegalStateException: start\n");
    mockInstance.invokeI2C("_CN7default29i2cThrowIllegalStateExceptionHv");
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
    mockInstance.cjNativeInterface.throw_exception(frameRoots[0]);
    printf("[Mock] doThrowArithmeticException: error, should not reach here\n");
}

// std.core:IndexOutOfBoundsException and NegativeArraySize are also thrown like so
void doThrowNoneValueException() {
    printf("[Mock] doThrowNoneValueException called\n");
    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.type_info("std.core:NoneValueException");
    frameRoots[0] = interface.object_alloc(ti);
    mockInstance.invokeI2C(reinterpret_cast<uintptr_t>(frameRoots[0]), 0, 0, 0, "_CNat18NoneValueException6<init>Hv");
    interface.throw_exception(frameRoots[0]);
    printf("[Mock] doThrowNoneValueException: error, should not reach here\n");
}

void doThrowOutOfMemoryError() {
    printf("[Mock] doThrowOutOfMemoryError called\n");
    auto interface = mockInstance.cjNativeInterface;
    interface.throw_OOM();
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

// ---------------------------------------------------------

void ExceptionsMock::getInterpretedFrameInfo(frame_pointer_t fp, instruction_pointer_t ip, interpreted_frame_info_t* frameInfo) {
    printf("[Mock] getInterpretedFrameInfo called for (fp=%p, ip=%p)\n", fp, ip);
    frameInfo->line_number = 42;
    frameInfo->class_name = const_cast<char*>("UnknownClass");
    frameInfo->method_name = const_cast<char*>("unknownMethod");
    frameInfo->file_name = const_cast<char*>("unknown.cj");
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

void ExceptionsMock::testStackOverflowPrepare() {
    printf("[Mock] testStackOverflowPrepare called\n");
    g_InterpreterStackReserveSize = 1024 * 1024 * 1024; // set reserve size to 1GB to trigger stack overflow on next c2i transition
}

void ExceptionsMock::testStackOverflow() {
    printf("[Mock] testStackOverflow: error, should not reach here\n");
}