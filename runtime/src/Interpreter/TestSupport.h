#ifndef TEST_SUPPORT_H
#define TEST_SUPPORT_H
#include "Options.h"
#ifdef TEST

#define VIRTUAL_FUNCS_N 10

namespace MapleRuntime {

void initTestVirtualFuncs(const char* interpreterLibName);

}

#endif // TEST
#endif // TEST_SUPPORT_H