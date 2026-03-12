#include "TestSupport.h"
#ifdef TEST


namespace MapleRuntime {

void initTestVirtualFuncs(const char* interpreterLibName) {
    void* interpreterLibHandle = dlopen(interpreterLibName, RTLD_LAZY);
    if (!interpreterLibHandle) {
        LOG(RTLOG_ERROR, "Interpreter library handle is null\n");
        return;
    }
    auto getC2IFunc = reinterpret_cast<FuncPtr(*)(int)>(dlsym(interpreterLibHandle, "getC2IFunc"));
    if (!getC2IFunc) {
        LOG(RTLOG_ERROR, "Failed to get symbol getC2IFunc: %s\n", dlerror());
        return;
    }

    for (int i = 0; i < VIRTUAL_FUNCS_N; i++) {
        FuncPtr trampoline = getC2IFunc(i);
        DLOG_INTERPRETER(DEBUG, "[Interpreter] trampoline addr(%d): %p", i, trampoline);
    }

    TypeInfo* ti = LoaderManager::GetInstance()->FindTypeInfoFromLoadedFiles("common:C2I");
    if (ti == nullptr) {
        LOG(RTLOG_ERROR, "[Interpreter] C2I type info is not patched");
        return;
    }
    
    ExtensionData* data = ti->GetvExtensionDataStart()[1];
    
    for (int i = 0; i < VIRTUAL_FUNCS_N; i++) {
        data->GetFuncTable()[i] = getC2IFunc(i);
        DLOG_INTERPRETER(DEBUG, "[Interpreter] patched C2I virtual func #%d, addr=%p", i, (void*)data->GetFuncTable()[i]);
    }
}

}
#endif // TEST