#ifndef MOCK_H
#define MOCK_H

#include "libinterpreter.h"
#include "log.h"

class AllocationMock : Mock {
    public:
        AllocationMock() = default;
        AllocationMock(INT_InterpreterInterface* interpreter_interf, DYN_CJNativeInterface* cjnative_interf);


    private:
        static void allocationTest();
};

// define once per test directory
#define MOCK_IMPLEMENTATION AllocationMock

#endif
