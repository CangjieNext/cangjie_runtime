#ifndef MOCK_H
#define MOCK_H

#include "libinterpreter.h"

class ForeignTestMock : public Mock {
    public:
        ForeignTestMock() = default;
        ForeignTestMock(int a1, interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf);

    private:
        static void visitGlobalRoots(root_visitor_t visitor);
        static void testForeignCall();
        static void testGCDuringForeignCall();
        static void testCangjieCallback();
        static void testCangjieCallbackThrowingException();
        static void stop();
};
extern "C" void landingPad();

// define once per test directory
#define MOCK_IMPLEMENTATION ForeignTestMock

#endif
