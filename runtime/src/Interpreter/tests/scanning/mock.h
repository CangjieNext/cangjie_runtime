#ifndef MOCK_H
#define MOCK_H

#include "libinterpreter.h"

class ScanningMock : public Mock {
    public:
        ScanningMock() = default;
        ScanningMock(interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf);

    private:
        static void IterateFramesWithState(fiber_specific_data_t fiberData, void (*callback)(visiting_state_t, void*), void* ctx);

        static void visitGlobalRoots(root_visitor_t visitor);
        static void markFrameRoots(visiting_state_t state, frame_desc_t frameDesc, root_visitor_t rootVisitor);
        static void adjustFrameRoots(visiting_state_t state, frame_desc_t frameDesc, root_visitor_t rootVisitor, derived_ptr_visitor_t derivedPtrVisitor);
        static void init();
        static void doOnFiberStart(fiber_specific_data_t* data);
        static void intermidiateFrames();

        static void visitStackPtrs(visiting_state_t state, frame_desc_t frameDesc, root_visitor_t stackPtrVisitor, derived_ptr_visitor_t derivedPtrVisitor);
        static void frameWithStackPtrSlot();
        static void checkIsPointerAdjusted();
};

// define once per test directory
#define MOCK_IMPLEMENTATION ScanningMock

#endif
