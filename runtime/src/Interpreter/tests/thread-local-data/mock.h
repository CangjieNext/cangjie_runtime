#ifndef MOCK_H
#define MOCK_H

#include "libinterpreter.h"

class ThreadLocalDataTestMock : public Mock {
    public:
        ThreadLocalDataTestMock() = default;
        ThreadLocalDataTestMock(interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf);

    private:
        static void IterateFramesWithState(fiber_specific_data_t fiberData, void (*callback)(visiting_state_t, void*), void* ctx);
        static void markFrameRoots(visiting_state_t state, frame_desc_t desc, root_visitor_t visitor);
        static void adjustFrameRoots(visiting_state_t state, frame_desc_t desc, root_visitor_t visitor, root_visitor_t derivedPtrVisitor);
        static void visitGlobalRoots(root_visitor_t visitor);

        static int  doOnEntrypoint(char* func_name);
        static void doOnFiberStart(fiber_specific_data_t* data);
        static void testThreadLocalData(thread_local_data_t thread_local_data_arg);
        static void init();
        static void stop();
};

struct RecFieldMirror {
    public:
        obj_ref_t obj0;
        obj_ref_t obj1;
        obj_ref_t obj2;
        obj_ref_t obj3;
};

// define once per test directory
#define MOCK_IMPLEMENTATION ThreadLocalDataTestMock

#endif
