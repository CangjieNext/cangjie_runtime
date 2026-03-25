#ifndef MOCK_H
#define MOCK_H

#include "libinterpreter.h"

class GCPointMock : public Mock {
    public:
        GCPointMock() = default;
        GCPointMock(INT_InterpreterInterface* interpreter_interf, DYN_CJNativeInterface* cjnative_interf);

    private:
        static void IterateFramesWithState(DYN_CJThreadSpecificData cjThreadData, void (*callback)(DYN_VisitingState, void*), void* ctx);
        static void markFrameRoots(DYN_VisitingState state, INT_FrameDesc desc, DYN_RootVisitor visitor);

        static void doOnCJThreadStart(DYN_CJThreadSpecificData* data);
        static void pollSafepoints();
};

// define once per test directory
#define MOCK_IMPLEMENTATION GCPointMock

#endif
