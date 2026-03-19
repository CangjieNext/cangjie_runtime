#include "RTInterface.h"
#include "libinterpreter.h"
#include "mock.h"
#include "log.h"
#include <dlfcn.h>

#ifdef __cplusplus
using namespace MRTExport;
#endif

#ifndef MOCK_IMPLEMENTATION
#define MOCK_IMPLEMENTATION Mock
#endif

MOCK_IMPLEMENTATION mockInstance;

#define C2I_STUB_FRAME_SIZE 64
#define INTERPRETER_FRAMES_SIZE 1024

extern "C" {
struct VirtFuncContainer virtualFuncs;
}

extern "C" {
    get_current_catch_function_PC_f g_GetCurrentCatchFunctionPCPtr = nullptr;
    throw_exception_f g_ThrowExceptionPtr = nullptr;
    get_thread_local_data_f g_GetThreadLocalDataPtr = nullptr;
    stack_grow_stub_f g_CJ_MCC_StackGrowStubPtr = nullptr;
    C2N_Stub_f g_C2NStubPtr = nullptr;
    size_t g_InterpreterStackReserveSize = 0;
}

Mock::Mock(interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf): cjNativeInterface(*cjnative_interf) {
    interpreter_interf->version = 1;
    interpreter_interf->fiber_specific_data_size = 16;
    interpreter_interf->iterator_size = 0;
    g_InterpreterStackReserveSize = INTERPRETER_FRAMES_SIZE;

    interpreter_interf->c2iStubStartAddr = (uintptr_t) C2I_Stub;
    interpreter_interf->c2iStubEndAddr = (uintptr_t) C2I_Stub_End;

    interpreter_interf->i2iAdapterStartAddr = (uintptr_t) I2I_Stub;
    interpreter_interf->i2iAdapterEndAddr = (uintptr_t) I2I_Stub_End;

    interpreter_interf->landing_pad = &LandingPadWrapper;

    interpreter_interf->visit_global_roots = (visit_global_roots_f)+[]() {
        log("[Mock] dummy visit_global_roots called!\n"); return 0;
    };
    interpreter_interf->visit_fiber_roots = (visit_fiber_roots_f)+[]() {
        log("[Mock] dummy visit_fiber_roots called!\n"); return 0;
    };
    interpreter_interf->visit_frame_roots = (visit_frame_roots_f)+[]() {
        log("[Mock] dummy visit_frame_roots called!\n"); return 0;
    };
    interpreter_interf->visit_stack_ptrs = (visit_stack_ptrs_f)+[]() {
        log("[Mock] dummy visit_stack_ptrs called!\n"); return 0;
    };
    interpreter_interf->fiber_start = (fiber_start_f)+[]() {
        log("[Mock] dummy fiber_start called!\n"); return 0;
    };
    interpreter_interf->fiber_destroy = (fiber_destroy_f)+[]() {
        log("[Mock] dummy fiber_destroy called!\n"); return 0;
    };
    interpreter_interf->frame_info_provider = (frame_info_provider_f)+[]() {
        log("[Mock] dummy frame_info_provider called!\n"); return 0;
    };

    g_GetCurrentCatchFunctionPCPtr = cjnative_interf->get_current_catch_function_PC;
    g_ThrowExceptionPtr = cjnative_interf->throw_exception;
    g_GetThreadLocalDataPtr = cjnative_interf->get_thread_local_data;

    g_CJ_MCC_StackGrowStubPtr = reinterpret_cast<stack_grow_stub_f>(get_symbol_address("CJ_MCC_StackGrowStub"));
    g_C2NStubPtr = reinterpret_cast<C2N_Stub_f>(get_symbol_address("CJ_MCC_C2NStub"));
    assert(g_CJ_MCC_StackGrowStubPtr != nullptr);
}

// Public library API

PUBLIC int interpreter_bridge_init(interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf) {
        initLogs();
        log("[Interpreter] Start interfaces exchange");

        mockInstance = MOCK_IMPLEMENTATION(interpreter_interf, cjnative_interf);

        log("[Interpreter] End of interfaces exchange");
        return 0;
}

PUBLIC void* getC2IFunc(int idx) {
        uint8_t* tStartAddr = &trampolinesStart;
        uint8_t* trampolineAddr = tStartAddr + (idx * TRAMPOLINE_SIZE);
        return trampolineAddr;
}

// Public library API end

/**
 * Tries to find a symbol in the global scope (RTLD_DEFAULT).
 * Returns the address if found, or NULL if not.
 */
void* get_symbol_address(const char* symbol_name) {
    // Clear any existing errors
    dlerror();

    // RTLD_DEFAULT searches the global scope (executable + loaded libraries)
    void* addr = dlsym(RTLD_DEFAULT, symbol_name);

    if (!addr) {
        log("[Mock] error: symbol %s not found\n", symbol_name);
        log("[Mock] linker error: %s\n", dlerror());
        return NULL;
    }

    return addr;
}

