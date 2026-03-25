#ifndef MOCK_H
#define MOCK_H

#include "libinterpreter.h"

class ThreadLocalDataTestMock : public Mock {
    public:
        ThreadLocalDataTestMock() = default;
        ThreadLocalDataTestMock(INT_InterpreterInterface* interpreter_interf, DYN_CJNativeInterface* cjnative_interf);

    private:
        static void IterateFramesWithState(DYN_CJThreadSpecificData cjThreadData, void (*callback)(DYN_VisitingState, void*), void* ctx);
        static void markFrameRoots(DYN_VisitingState state, INT_FrameDesc desc, DYN_RootVisitor visitor);
        static void adjustFrameRoots(DYN_VisitingState state, INT_FrameDesc desc, DYN_RootVisitor visitor, DYN_RootVisitor derivedPtrVisitor);
        static void visitGlobalRoots(DYN_RootVisitor visitor);

        static int  doOnEntrypoint(char* func_name);
        static void doOnCJThreadStart(DYN_CJThreadSpecificData* data);
        static void testThreadLocalData(DYN_ThreadLocalData thread_local_data_arg);
        static void init();
        static void stop();
};

struct RecFieldMirror {
    public:
        DYN_ObjRef obj0;
        DYN_ObjRef obj1;
        DYN_ObjRef obj2;
        DYN_ObjRef obj3;
};

// define once per test directory
#define MOCK_IMPLEMENTATION ThreadLocalDataTestMock

#endif
