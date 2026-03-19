#ifndef MOCK_H
#define MOCK_H

#include "libinterpreter.h"

class GCPointMock : public Mock {
    public:
        GCPointMock() = default;
        GCPointMock(interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf);

    private:
        static void visitFiberRoots(root_visitor_t visitor, fiber_specific_data_t data);
        static void doOnFiberStart(fiber_specific_data_t* data);
        static void pollSafepoints();
};

// define once per test directory
#define MOCK_IMPLEMENTATION GCPointMock

#endif
