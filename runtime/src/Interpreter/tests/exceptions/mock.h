#ifndef MOCK_H
#define MOCK_H

#include "libinterpreter.h"

class ExceptionsMock : public Mock {
    public:
        ExceptionsMock() = default;
        ExceptionsMock(interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf);

    private:
        static void visitGlobalRoots(root_visitor_t visitor);
        static void visitFrameRoots(root_visitor_t visitor, frame_desc_t desc);
        static int  doOnEntrypoint(char* func_name);
        static void doOnFiberStart(fiber_specific_data_t* data);
        static void getInterpretedFrameInfo(frame_pointer_t fp, instruction_pointer_t ip, interpreted_frame_info_t* frameInfo);
        static void throwOutException();
        static void throwAndCatchException();
        static void testCompiledThrower();
        static void testI2IExceptions();
        static void testArithmeticException();
        static void testNoneValueException();
        static void testOutOfMemoryError();
        static void testStackOverflowPrepare();
        static void testStackOverflow();
    };
extern "C" void landingPad();

// define once per test directory
#define MOCK_IMPLEMENTATION ExceptionsMock

#endif
