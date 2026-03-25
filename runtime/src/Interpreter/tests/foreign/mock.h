#ifndef MOCK_H
#define MOCK_H

#include "libinterpreter.h"

class ForeignTestMock : public Mock {
    public:
        ForeignTestMock() = default;
        ForeignTestMock(INT_InterpreterInterface* interpreter_interf, DYN_CJNativeInterface* cjnative_interf);

    private:
        static void visitGlobalRoots(DYN_RootVisitor visitor);
        static void testForeignCall();
        static void testGCDuringForeignCall();
        static void testCangjieCallback();
        static void testCangjieCallbackThrowingException();
        static void testI2NTransitionStackOverflow();
        static void stop();
};
extern "C" void landingPad();

// define once per test directory
#define MOCK_IMPLEMENTATION ForeignTestMock

#endif
