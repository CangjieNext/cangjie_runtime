#ifndef MOCK_H
#define MOCK_H

#include "libinterpreter.h"

class ExceptionsMock : public Mock {
    public:
        ExceptionsMock() = default;
        ExceptionsMock(INT_InterpreterInterface* interpreter_interf, DYN_CJNativeInterface* cjnative_interf);

    private:
        static void IterateFramesWithState(DYN_CJThreadSpecificData cjThreadData, void (*callback)(DYN_VisitingState, void*), void* ctx);
        static void markFrameRoots(DYN_VisitingState state, INT_FrameDesc desc, DYN_RootVisitor visitor);
        static void adjustFrameRoots(DYN_VisitingState state, INT_FrameDesc desc, DYN_RootVisitor visitor, DYN_RootVisitor derivedPtrVisitor);

        static int  doOnEntrypoint(char* func_name);
        static void doOnCJThreadStart(DYN_CJThreadSpecificData* data);
        static void getInterpretedFrameInfo(DYN_FramePointer fp, DYN_InstructionPointer ip, INT_InterpretedFrameInfo* frameInfo);
        static void throwOutException();
        static void throwAndCatchException();
        static void testCompiledThrower();
        static void testI2IExceptions();
        static void testArithmeticException();
        static void testNoneValueException();
        static void testOutOfMemoryError();
        static void testDumpAllStackTrace();
        static void dumpAllStackTrace();
        static void testStackOverflowPrepare();
        static void testStackOverflow();
    };
extern "C" void landingPad();

// define once per test directory
#define MOCK_IMPLEMENTATION ExceptionsMock

#endif
