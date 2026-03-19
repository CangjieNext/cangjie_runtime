#ifndef MOCK_H
#define MOCK_H

#include "libinterpreter.h"

class GCPointMock : public Mock {
    public:
        GCPointMock() = default;
        GCPointMock(interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf);

    private:
        static void IterateFramesWithState(fiber_specific_data_t fiberData, void (*callback)(visiting_state_t, void*), void* ctx);
        static void markFrameRoots(visiting_state_t state, frame_desc_t desc, root_visitor_t visitor);

        static void doOnFiberStart(fiber_specific_data_t* data);
        static void pollSafepoints();
};

// define once per test directory
#define MOCK_IMPLEMENTATION GCPointMock

#endif
