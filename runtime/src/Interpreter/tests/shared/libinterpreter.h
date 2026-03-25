#ifndef INTERPRETER_MOCK_H
#define INTERPRETER_MOCK_H

#include <stdint.h>
#include <dlfcn.h>
#include <cassert>
#include <functional>

#include "Interpreter/RTInterface.h"
#include "log.h"

#define PUBLIC  extern "C"
#define TRAMPOLINE_SIZE 16 // in bytes
#define VIRTUAL_FUNCS_N 10

void* get_symbol_address(const char* symbol_name);

struct VirtFuncContainer {
        uintptr_t padding; // we want all trampolines be equal by size
        uintptr_t data[VIRTUAL_FUNCS_N];
};

extern "C" {
    extern DYN_GetCurrentCatchFunctionPcFn g_GetCurrentCatchFunctionPCPtr;
    extern DYN_ThrowExceptionFn g_ThrowExceptionPtr;
    extern DYN_GetThreadLocalDataFn g_GetThreadLocalDataPtr;
    extern DYN_StackGrowStubFn g_CJ_MCC_StackGrowStubPtr;
    extern DYN_GetPendingExceptionFn g_getPendingExceptionPtr;
    extern size_t g_InterpreterStackReserveSize;
}

extern "C" {
    extern uint8_t trampolinesStart;      // defined in C2I_Stub.s
    extern void C2I_Stub(void* funcAddr); // defined in C2I_Stub.s
    extern void C2I_Stub_End();           // defined in C2I_Stub.s
    extern struct VirtFuncContainer virtualFuncs;

    extern uintptr_t I2C_Stub(uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4, void* funcAddr, DYN_ThreadLocalData threadLocalData); // defined in I2C_Stub.s

    extern void I2I_Stub(uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4, void* funcAddr); // defined in I2I_Stub.s
    extern void I2I_Stub_End();           // defined in I2I_Stub.s

    extern void LandingPadWrapper();

    __attribute__((noreturn))
    extern void unwindInterpretedFrameAndRethrowException(void* exception_obj);
}

// Mock concrete signature for invoking opaque DYN_CJNativeInterface::i2nStub.
typedef uintptr_t (*mock_i2n_stub_f)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, void*, DYN_ThreadLocalData);

class Mock {
    public:
        Mock() = default;
        Mock(INT_InterpreterInterface* interpreter_interf, DYN_CJNativeInterface* cjnative_interf);

        DYN_CJNativeInterface cjNativeInterface;

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
            return I2C_Stub(arg1, arg2, arg3, arg4, symAddr, cjNativeInterface.getThreadLocalData());
        }

        void invokeI2C(const char* funcName, DYN_ThreadLocalData& thread_local_data) {
            invokeI2C(0, 0, 0, 0, funcName, thread_local_data);
        }

        uintptr_t invokeI2C(uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4, const char* funcName, DYN_ThreadLocalData& thread_local_data) {
            void* symAddr = get_symbol_address(funcName);
            assert(symAddr != nullptr);
            uintptr_t res = I2C_Stub(arg1, arg2, arg3, arg4, symAddr, thread_local_data);

            // update thread local data after returning from compiled code
            thread_local_data = cjNativeInterface.getThreadLocalData();

            return res;
        }

        uintptr_t invokeI2N(uintptr_t arg1, uintptr_t arg2, uintptr_t arg3, uintptr_t arg4, const char* funcName, DYN_ThreadLocalData& thread_local_data) {
            void* symAddr = get_symbol_address(funcName);
            assert(symAddr != nullptr);
            auto i2nStub = reinterpret_cast<mock_i2n_stub_f>(cjNativeInterface.i2nStub);
            assert(i2nStub != nullptr);
            uintptr_t res = i2nStub(arg1, arg2, arg3, arg4, symAddr, thread_local_data);

            // update thread local data
            thread_local_data = cjNativeInterface.getThreadLocalData();

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

inline DYN_FieldRef getRefField(DYN_ObjRef object, uint32_t offset) {
    return reinterpret_cast<DYN_FieldRef>(reinterpret_cast<uintptr_t>(object) + offset);
}

#endif
