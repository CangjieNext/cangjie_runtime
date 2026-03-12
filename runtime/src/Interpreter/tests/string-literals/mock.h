#ifndef MOCK_H
#define MOCK_H

#include "libinterpreter.h"

class StrLiteralMock : public Mock {
    public:
        StrLiteralMock() = default;
        StrLiteralMock(int a1, int a2, interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf);

    private:
        static int  doOnEntrypoint(char* func_name);

        static void produceLiteralSlow1();
        static void produceLiteralSlow2();

        static void produceLiteralFast1();
        static void produceLiteralFast2();
};

// define once per test directory
#define MOCK_IMPLEMENTATION StrLiteralMock

#endif