#ifndef INTERPRETER_MOCK_H
#define INTERPRETER_MOCK_H

#include <stdint.h>
#include <dlfcn.h>
#include <cassert>
#include <functional>

#include "RTInterface.h"
#include "log.h"

#ifdef __cplusplus
using namespace MRTExport;
#endif

#define PUBLIC  extern "C"
#define TRAMPOLINE_SIZE 16 // in bytes
#define VIRTUAL_FUNCS_N 10

void* get_symbol_address(const char* symbol_name);

struct VirtFuncContainer {
        uintptr_t padding; // we want all trampolines be equal by size
        uintptr_t data[VIRTUAL_FUNCS_N];
};

extern "C" {
    extern get_current_catch_function_PC_f g_GetCurrentCatchFunctionPCPtr;
    extern throw_exception_f g_ThrowExceptionPtr;
    extern get_thread_local_data_f g_GetThreadLocalDataPtr;
    extern stack_grow_stub_f g_CJ_MCC_StackGrowStubPtr;
    extern C2N_Stub_f g_C2NStubPtr;
    extern size_t g_InterpreterStackReserveSize;
}

extern "C" {
    extern uint8_t trampolinesStart;      // defined in C2I_Stub.s
    extern void C2I_Stub(void* funcAddr); // defined in C2I_Stub.s
    extern void C2I_Stub_End();           // defined in C2I_Stub.s
    extern struct VirtFuncContainer virtualFuncs;
    
    extern uintptr_t I2C_Stub(uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4, void* funcAddr, thread_local_data_t threadLocalData); // defined in I2C_Stub.s
    extern uintptr_t I2N_Stub(uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4, void* funcAddr, thread_local_data_t threadLocalData); // defined in I2N_Stub.s

    extern void I2I_Stub(uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4, void* funcAddr); // defined in I2I_Stub.s
    extern void I2I_Stub_End();           // defined in I2I_Stub.s

    extern void LandingPadWrapper();

    __attribute__((noreturn))
    extern void unwindInterpretedFrameAndRethrowException(void* exception_obj);
}

class Mock {
    public:
        Mock() = default;
        Mock(int a1, interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf);

        cjnative_interface_t cjNativeInterface;

        void registerC2I(int idx, uintptr_t funcAddr) {
            assert(idx >= 0 && idx < VIRTUAL_FUNCS_N);
            log("[Mock] registered virtual func #%d, addr=%p\n", idx, (void*)funcAddr);
            virtualFuncs.data[idx] = funcAddr;
        }

        void invokeI2C(const char* funcName) {
            invokeI2C(0, 0, 0, 0, funcName);
        }

        uintptr_t invokeI2C(uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4, const char* funcName) {
            void* symAddr = get_symbol_address(funcName);
            assert(symAddr != nullptr);
            return I2C_Stub(arg1, arg2, arg3, arg4, symAddr, cjNativeInterface.get_thread_local_data());
        }

        void invokeI2C(const char* funcName, thread_local_data_t& thread_local_data) {
            invokeI2C(0, 0, 0, 0, funcName, thread_local_data);
        }

        uintptr_t invokeI2C(uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4, const char* funcName, thread_local_data_t& thread_local_data) {
            void* symAddr = get_symbol_address(funcName);
            assert(symAddr != nullptr);
            uintptr_t res = I2C_Stub(arg1, arg2, arg3, arg4, symAddr, thread_local_data);

            // update thread local data after returning from compiled code
            thread_local_data = cjNativeInterface.get_thread_local_data();

            return res;
        }

        uintptr_t invokeI2N(uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4, const char* funcName, thread_local_data_t& thread_local_data) {
            void* symAddr = get_symbol_address(funcName);
            assert(symAddr != nullptr);
            uintptr_t res = I2N_Stub(arg1, arg2, arg3, arg4, symAddr, thread_local_data);

            // update thread local data
            thread_local_data = cjNativeInterface.get_thread_local_data();

            return res;
        }

        void invokeI2I(void* funcAddr) {
            invokeI2I(0, 0, 0, 0, funcAddr);
        }

        void invokeI2I(uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4, void* funcAddr) {
            I2I_Stub(arg1, arg2, arg3, arg4, funcAddr);
        }

    private:
        static int dummy() {
            printf("[Mock] someone called dummy func!");
            return 0;
        }
};


// Strips upper 16-bit tags from a 64-bit pointer and returns the raw address
template <typename T>
inline uintptr_t getUntaggedAddress(T* ptr) {
    constexpr uintptr_t LOWER_48_BIT_MASK = 0x0000FFFFFFFFFFFFULL;
    return reinterpret_cast<uintptr_t>(ptr) & LOWER_48_BIT_MASK;
}

inline field_ref_t getRefField(obj_ref_t object, uint32_t offset) {
    return reinterpret_cast<field_ref_t>(reinterpret_cast<uintptr_t>(object) + offset);
}

#endif
