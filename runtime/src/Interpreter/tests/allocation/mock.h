#ifndef MOCK_H
#define MOCK_H

#include "libinterpreter.h"
#include "log.h"

class AllocationMock : Mock {
    public:
        AllocationMock() = default;
        AllocationMock(int a1, interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf);


    private:
        static int allocationTest();
};

// define once per test directory
#define MOCK_IMPLEMENTATION AllocationMock

#endif
