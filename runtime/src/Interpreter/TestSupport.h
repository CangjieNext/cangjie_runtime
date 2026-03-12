#ifndef TEST_SUPPORT_H
#define TEST_SUPPORT_H
#include "Options.h"
#ifdef TEST
#include "RTInterface.h"
#include "InterpreterSpecific.h"
#include "ObjectModel/ExtensionData.h"
#include "ObjectModel/MClass.h"
#include "TypeInfoManager.h"
#include "Base/LogFile.h"
#include "LoaderManager.h"

#include <stdio.h>
#include <dlfcn.h>

#define VIRTUAL_FUNCS_N 10

namespace MapleRuntime {

void initTestVirtualFuncs(const char* interpreterLibName);

}

#endif
#endif