
#ifndef RT_MIRRORS_H
#define RT_MIRRORS_H

#include <stdint.h>
#include <stddef.h>
#include <atomic>
#include <mutex>
#include <memory>
#include <vector>

enum class ThreadTypeMirror { CJ_PROCESSOR = 0, GC_THREAD, FP_THREAD, HOT_UPDATE_THREAD };

enum GCPhaseMirror : uint8_t {
    GC_PHASE_UNDEF = 0,
    GC_PHASE_IDLE = 1,
    GC_PHASE_FINISH = 2,
    GC_PHASE_RECLAIM_SATB_NODE = 3,
    GC_PHASE_INIT = 8,

    // only gc phase after GC_PHASE_INIT ( enum value > GC_PHASE_INIT) needs barrier.
    GC_PHASE_ENUM = 9,
    GC_PHASE_TRACE = 10,
    GC_PHASE_CLEAR_SATB_BUFFER = 11,
    GC_PHASE_POST_TRACE = 12,
    GC_PHASE_PREFORWARD = 13,
    GC_PHASE_FORWARD = 14,
};


enum class UnwindContextStatusMirror {
    UNKNOWN = 0,
    RELIABLE = 1,
    RISKY = 2,
    SIGNAL_STATUS = 3,
};

enum class FrameTypeMirror {
    UNKNOWN = 0,
    MANAGED = 1,
    N2C_STUB = 2,
    C2N_STUB = 3,
    RUNTIME = 4,
    SAFEPOINT = 5,
    C2R_STUB = 6,
    NATIVE = 7,
    STACKGROW = 8,
    EXSLUSIVE = 9,
#ifdef INTERPRETER_ENABLED
    INTERPRETER = 10,
    INTERPRETER_C2I = 11,
    INTERPRETER_I2I = 12
#endif
};

class MachineFrameMirror {
        void* fa;
        const uint32_t* ip;
    #if defined(ENABLE_BACKWARD_PTRAUTH_CFI)
        const uint64_t* ptrAuthRAMod;
    #endif
};

class FrameInfoMirror {
    MachineFrameMirror mFrame;
    const uint32_t* startProc;
    const uint8_t* lsdaStart;
    FrameTypeMirror fType;
};

class UnwindContextMirror {
    enum class UnwindContextStatusMirror {
        UNKNOWN = 0,
        RELIABLE = 1,
        RISKY = 2,
        SIGNAL_STATUS = 3,
    };

public:
    UnwindContextMirror() : status(UnwindContextStatusMirror::UNKNOWN) {}


    virtual ~UnwindContextMirror() { anchorFA = nullptr; }

    FrameInfoMirror frameInfo;

    // Use to save anchorFA stored in tls.
    uint32_t* anchorFA = nullptr;

protected:
    UnwindContextStatusMirror status;
};

class IEHFrameInfoMirror {
public:
    virtual ~IEHFrameInfoMirror() = default;

    virtual bool IsCatchException() const = 0;
};

class ExceptionWrapperMirror {
    enum class ExceptionType {
        UNKNOWN = 0,
        OUT_OF_MEMORY = 1,
        STACK_OVERFLOW = 2,
        ARITHMETIC = 3,
        INCOMPATIBILE_PACKAGE = 4
    };
private:
    void* exceptionRef;
    bool isCaught;
    uint32_t typeIndex;
    uintptr_t landingPad;

    bool throwingOOME;
    void* throwingSOFFramePc;
    uint32_t adjustedSize;
    bool fatalException;

    ExceptionType exceptionType = ExceptionType::UNKNOWN;

    std::vector<std::unique_ptr<IEHFrameInfoMirror>> ehFrameInfos;
    std::vector<uint64_t> liteFrameInfos;
    char* message;
    size_t messageLength;
    uintptr_t topManagedPC;
#ifdef INTERPRETER_ENABLED
    uintptr_t currentCatchFunctionPC;
#endif
};

class MutatorMirror {
public:
    // flag which indicates the reason why mutator should suspend. flag is set by some external thread.
    enum SuspensionType : uint32_t {
        SUSPENSION_FOR_GC_PHASE = 1,
        SUSPENSION_FOR_SYNC = 2,
        SUSPENSION_FOR_EXIT = 4,
        SUSPENSION_FOR_CPU_PROFILE = 8,
    };

    enum GCPhaseTransitionState : uint32_t {
        NO_TRANSITION,
        NEED_TRANSITION,
        IN_TRANSITION,
        FINISH_TRANSITION,
    };

    enum CpuProfileState : uint32_t {
        NO_CPUPROFILE,
        NEED_CPUPROFILE,
        IN_CPUPROFILING,
        FINISH_CPUPROFILE,
    };

    // Indicate whether mutator is in saferegion
    enum SaferegionState : uint32_t {
        SAFE_REGION_TRUE = 0x17161514,
        SAFE_REGION_FALSE = 0x03020100,
    };

    GCPhaseMirror GetMutatorPhase() const {
        return mutatorPhase.load(std::memory_order_acquire);
    }

private:
    // Indicate the current mutator phase and use which barrier in concurrent gc
    // ATTENTION: THE LAYOUT FOR GCPHASE MUST NOT BE CHANGED!
    std::atomic<GCPhaseMirror> mutatorPhase;
    // thread id
    uint32_t tid = 0;
    // cjthread ptr
    void* cjthread;
    // in saferegion, it will not access any managed objects and can be visitted by observer
    std::atomic<uint32_t> inSaferegion;
    // Protect observerCnt
    std::mutex observeCntMutex;
    // Increase when this mutator is observed by some observer
    std::atomic<size_t> observerCnt;
    // context for unwinding stack
    UnwindContextMirror uwContext;
    // exception object wrapper
    ExceptionWrapperMirror exceptionWrapper;
#ifndef __WIN64
    void* unuse = nullptr; // reusable placeholder
#endif // __WIN64

    uint64_t* safepointStatePtr = nullptr; // state: active or not
#ifdef _WIN64
    uint32_t stackGrowFrameSize = 0;
#endif

    // If set implies this mutator should process suspension requests
    std::atomic<uint32_t> suspensionFlag = { 0 };
    // Indicate the state of mutator's phase transition
    std::atomic<GCPhaseTransitionState> transitionState = { NO_TRANSITION };
    void* rawObject{ nullptr };

    void* satbNode = nullptr;
#if defined(GCINFO_DEBUG) && GCINFO_DEBUG
    GCInfos gcInfos;
#endif
    // this flag is used for gc unwind stack, when runtime-thread stack doesn't include managed frame,
    // we don't need to scan it.
    std::atomic<bool> inManagedContext = { true };
    void* stackBoundAddr = { nullptr };
    std::mutex mutatorLock;

    uintptr_t stackBaseAddr;
    uintptr_t stackTopAddr;
    uintptr_t stackSize;

    std::atomic<CpuProfileState> cpuProfileState;
    struct ForeignThreadInfo {
        bool isForeignThread = { false };
        bool isExit = { false };
        void* allocBuffer = { nullptr };
        void* schedule = { nullptr };
    } foreignThreadInfo;

};

struct ThreadLocalDataMirror {
    // External thread local var.
    void* buffer;
    MutatorMirror* mutator;
    uint8_t* cjthread;
    uint8_t* schedule;
    uint8_t* preemptFlag;
    uint8_t* protectAddr;
    uint64_t safepointState;
    uint64_t tid;
    void* foreignCJThread;
    // Internal thread local var.
    ThreadTypeMirror threadType;
    bool isCJProcessor;
    void* threadCache;
};

#endif // RT_MIRRORS_H