#ifndef MOCK_H
#define MOCK_H

#include "libinterpreter.h"

class ScanningMock : public Mock {
    public:
        ScanningMock() = default;
        ScanningMock(int a1, int a2, interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf);

    private:
        static void visitGlobalRoots(root_visitor_t visitor);
        static void visitFiberRoots(root_visitor_t visitor, fiber_specific_data_t data);
        static void visitFrameRoots(root_visitor_t visitor, frame_desc_t desc);
        static int  doOnEntrypoint(char* func_name);
        static void doOnFiberStart(fiber_specific_data_t* data);
        static void intermidiateFrames();

        static void visitStackPtrs(stack_ptr_visitor_t visitor, fiber_specific_data_t interp_fiber_data, stack_ptr_slot_type_t slot_type);
        static void saveSomePointerToStack();
        static void checkIsPointerAdjusted();
};

// define once per test directory
#define MOCK_IMPLEMENTATION ScanningMock

#endif