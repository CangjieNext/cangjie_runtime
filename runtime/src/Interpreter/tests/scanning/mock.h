#ifndef MOCK_H
#define MOCK_H

#include "libinterpreter.h"

class ScanningMock : public Mock {
    public:
        ScanningMock() = default;
        ScanningMock(INT_InterpreterInterface* interpreter_interf, DYN_CJNativeInterface* cjnative_interf);

    private:
        static void IterateFramesWithState(DYN_CJThreadSpecificData cjThreadData, void (*callback)(DYN_VisitingState, void*), void* ctx);

        static void visitGlobalRoots(DYN_RootVisitor visitor);
        static void markFrameRoots(DYN_VisitingState state, INT_FrameDesc frameDesc, DYN_RootVisitor rootVisitor);
        static void adjustFrameRoots(DYN_VisitingState state, INT_FrameDesc frameDesc, DYN_RootVisitor rootVisitor, DYN_DerivedPtrVisitor derivedPtrVisitor);
        static void init();
        static void doOnCJThreadStart(DYN_CJThreadSpecificData* data);
        static void intermidiateFrames();

        static void visitStackPtrs(DYN_VisitingState state, INT_FrameDesc frameDesc, DYN_RootVisitor stackPtrVisitor, DYN_DerivedPtrVisitor derivedPtrVisitor);
        static void frameWithStackPtrSlot();
        static void checkIsPointerAdjusted();
};

// define once per test directory
#define MOCK_IMPLEMENTATION ScanningMock

#endif
