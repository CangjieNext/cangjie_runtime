#include <vector>
#include <atomic>
#include <cassert>
#include <cstring>
#include "mock.h"
#include "log.h"
#include "libinterpreter.h"
#include "utils.h"

extern MOCK_IMPLEMENTATION mockInstance;

using Ref = void*;

#define FRAME_ROOTS_N  8
#define GLOBAL_ROOTS_N 64
#define ITERATIONS CJ_INTERPRETER_TEST_ITER(20000000)


Ref frameRoots[FRAME_ROOTS_N];
Ref globalRoots[GLOBAL_ROOTS_N];

DYN_GCTib RecFieldGCTib()
{
    // RecField has 4 Object references.
    // GCTib = ShortGCTib (Tag Bit 63 set) | Bitmap 0xF (bits 0,1,2,3 set).
    DYN_GCTib tib = {};
    tib.raw = 0x800000000000000FULL;
    return tib;
}

class CJThreadMagicData {
public:
    uint64_t magicValue;
    uint64_t cjThreadId;
    CJThreadMagicData(uint64_t magicValue, uint64_t cjThreadId) {
        this->magicValue = magicValue;
        this->cjThreadId = cjThreadId;
    }
};

ThreadLocalDataTestMock::ThreadLocalDataTestMock(INT_InterpreterInterface* interpreter_interf, DYN_CJNativeInterface* cjnative_interf)
    : Mock(interpreter_interf, cjnative_interf)
{
    interpreter_interf->iterateFramesWithState = &IterateFramesWithState;
    interpreter_interf->visitFrameRootsMarking = &markFrameRoots;
    interpreter_interf->visitFrameRootsAdjusting = &adjustFrameRoots;
    interpreter_interf->visitGlobalRoots = &visitGlobalRoots;

    interpreter_interf->cjThreadOnStart = &doOnCJThreadStart;

    registerC2I(0, reinterpret_cast<uintptr_t>(testThreadLocalData));
    registerC2I(8, reinterpret_cast<uintptr_t>(init));
}

void scanRefArray(DYN_RootVisitor visitor, Ref* roots, int size, std::function<void(DYN_RootVisitor, Ref*)> visit) {
    Ref* ph = roots;
    for (int i = 0; i < size; i++) {
        visit(visitor, ph);
        ph++;
    }
}

void ThreadLocalDataTestMock::visitGlobalRoots(DYN_RootVisitor visitor) {
    scanRefArray(visitor, globalRoots, GLOBAL_ROOTS_N,
        [](DYN_RootVisitor visitor, Ref* ph){
            mockInstance.cjNativeInterface.visitRootFromInterpreter(visitor, ph);
        });
    log("[Mock] finished global roots scanning");
}

void ThreadLocalDataTestMock::IterateFramesWithState(DYN_CJThreadSpecificData cjThreadData, void (*callback)(DYN_VisitingState, void*), void* ctx) {
    log("[Mock] frames iterator called!");
    callback(cjThreadData, ctx);
    log("[Mock] frames iterator finished!");
}

void ThreadLocalDataTestMock::markFrameRoots(DYN_VisitingState state, INT_FrameDesc desc, DYN_RootVisitor visitor) {
    log("[Mock] frame: (ip=%p, fp=%p)", desc.ip, desc.fp);

    scanRefArray(visitor, static_cast<Ref*>(frameRoots), FRAME_ROOTS_N,
        [](DYN_RootVisitor visitor, Ref* ph){
            mockInstance.cjNativeInterface.visitRootFromInterpreter(visitor, ph);
        });

    auto magic = static_cast<CJThreadMagicData*>(state);
    check(magic->magicValue == 0xCAFEBABE,
        "[Interpreter] cjThread: magic value mismatch (expected: 0xCAFEBABE, got: 0x%016lX)\n",
        magic->magicValue);

    log("[Mock] finished frame roots marking");
}

void ThreadLocalDataTestMock::adjustFrameRoots(DYN_VisitingState state, INT_FrameDesc desc, DYN_RootVisitor visitor, DYN_DerivedPtrVisitor derivedPtrVisitor) {
    log("[Mock] frame: (ip=%p, fp=%p)", desc.ip, desc.fp);

    scanRefArray(visitor, static_cast<Ref*>(frameRoots), FRAME_ROOTS_N,
        [](DYN_RootVisitor visitor, Ref* ph){
            mockInstance.cjNativeInterface.visitRootFromInterpreter(visitor, ph);
        });

    log("[Mock] finished frame roots adjusting");
}

std::atomic<int64_t> cjThreads_count(0);

void ThreadLocalDataTestMock::doOnCJThreadStart(DYN_CJThreadSpecificData* data) {
    printf("[Mock] doOnCJThreadStart: data = %p\n", data);
    cjThreads_count.fetch_add(1);
    auto cjThreadId = cjThreads_count.load();
    CJThreadMagicData* magicData = new CJThreadMagicData(0xCAFEBABE, cjThreadId);
    *data = magicData;
    printf("[Mock] doOnCJThreadStart: cjThread id = %ld\n", cjThreadId);
}

void ThreadLocalDataTestMock::init() {
    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.typeInfo("std.core:Object");

    for (int i = 0; i < GLOBAL_ROOTS_N; i++) {
        interface.writeStaticField(&globalRoots[i], interface.objectAlloc(ti));
    }

    for (int i = 0; i < FRAME_ROOTS_N; i++) {
        frameRoots[i] = interface.objectAlloc(ti);
    }
}

void readAndCheckRecField() {
    auto interface = mockInstance.cjNativeInterface;
    void* static_object_placeholder = get_symbol_address("_CN4test24globalObjectWithRecFieldE");
    if (!static_object_placeholder) {
        printf("[Mock] error: symbol _CN4test24globalObjectWithRecFieldE not found\n");
        return;
    }
    auto static_obj = interface.readStaticField(static_object_placeholder);
    uintptr_t fieldAddr = reinterpret_cast<uintptr_t>(static_obj) + sizeof(DYN_TypeInfo*); // start of record inside object
    RecFieldMirror record;
    interface.readStructField(reinterpret_cast<uintptr_t>(&record), static_obj, fieldAddr, sizeof(RecFieldMirror),
        RecFieldGCTib());

    if (!record.obj0 || !record.obj1 || !record.obj2 || !record.obj3) {
        printf("[Mock] error: one or more objects in record are null\n");
    }
}


/**
 * Reads a referense from provided static field and places it to frameRoots[0].
 * Simulates a read barrier with fast path.
 */
void staticReadBarrierFast(DYN_ObjRef* static_object_placeholder) {
    auto interface = mockInstance.cjNativeInterface;

    DYN_ObjRef raw_ref = *static_object_placeholder;
    if ((reinterpret_cast<uintptr_t>(raw_ref) >> 48) == 0) { // fast path: no tags
        frameRoots[0] = raw_ref; // place result in a root slot to keep it alive during GC
    } else {
        frameRoots[0] = interface.readStaticField(static_object_placeholder);
    }
}

/**
 * Writes a referense from frameRoots[0] to provided static field.
 * Simulates a write barrier with fast path.
 */
void staticWriteBarrierFast(DYN_ObjRef* static_object_placeholder, DYN_ThreadLocalData thread_local_data) {
    auto interface = mockInstance.cjNativeInterface;
    if (interface.isActiveGcPhase(thread_local_data)) {
        // slow path
        interface.writeStaticField(static_object_placeholder, frameRoots[0]);
    } else {
        // fast path
        *static_object_placeholder = frameRoots[0];
    }
}

/**
 * Writes a record from src to dst field of object.
 * Simulates a write barrier with fast path.
 */
void instanceWriteBarrierRecordFast(
    DYN_ObjRef object, uintptr_t dst, uintptr_t src, size_t size, DYN_GCTib tib, DYN_ThreadLocalData thread_local_data) {
    auto interface = mockInstance.cjNativeInterface;
    if (interface.isActiveGcPhase(thread_local_data)) {
        // slow path
        interface.writeStructField(object, dst, src, size, tib);
    } else {
        // fast path
        memcpy(reinterpret_cast<void*>(dst), reinterpret_cast<void*>(src), size);
    }
}

/**
 * Allocates an object of given type and updates thread local data, which is possibly changed due to GC-points in allocators.
 * Allocated object is placed in frameRoots[0] to be traceble by GC.
 */
void allocateObjectAndUpdateTLD(const char* tiName, DYN_ThreadLocalData& thread_local_data_ref) {
    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.typeInfo(tiName);
    if (!ti) {
        printf("[Mock] error: type info %s not found (allocateObjectAndUpdateTLD)\n", tiName);
        return;
    }
    frameRoots[0] = interface.objectAlloc(ti);
    if (!frameRoots[0]) {
        printf("[Mock] error: failed to allocate %s in allocateObjectAndUpdateTLD\n", tiName);
        return;
    }
    thread_local_data_ref = interface.getThreadLocalData();
}

void writeAndCheckRecFieldFast(DYN_ThreadLocalData& thread_local_data) {
    auto interface = mockInstance.cjNativeInterface;
    auto tiObject = interface.typeInfo("std.core:Object");
    if (!tiObject) {
        printf("[Mock] error: type info std.core:Object not found (writeAndCheckRecField)\n");
        return;
    }

    DYN_ObjRef* static_object_placeholder = reinterpret_cast<DYN_ObjRef*>(get_symbol_address("_CN4test24globalObjectWithRecFieldE"));
    if (!static_object_placeholder) {
        printf("[Mock] error: symbol _CN4test24globalObjectWithRecFieldE not found (writeAndCheckRecField)\n");
        return;
    }

    RecFieldMirror record_to_write;
    allocateObjectAndUpdateTLD("std.core:Object", thread_local_data);
    record_to_write.obj0 = frameRoots[0];
    allocateObjectAndUpdateTLD("std.core:Object", thread_local_data);
    record_to_write.obj1 = frameRoots[0];
    allocateObjectAndUpdateTLD("std.core:Object", thread_local_data);
    record_to_write.obj2 = frameRoots[0];
    allocateObjectAndUpdateTLD("std.core:Object", thread_local_data);
    record_to_write.obj3 = frameRoots[0];

    if (!record_to_write.obj0 || !record_to_write.obj1 || !record_to_write.obj2 || !record_to_write.obj3) {
        printf("[Mock] error: failed to allocate objects for record_to_write\n");
        return;
    }

    staticReadBarrierFast(static_object_placeholder);
    if (!frameRoots[0]) {
        printf("[Mock] error: read null from static field for record object\n");
        return;
    }

    uintptr_t fieldAddr = reinterpret_cast<uintptr_t>(frameRoots[0]) + sizeof(DYN_TypeInfo*); // start of record inside object
    instanceWriteBarrierRecordFast(frameRoots[0], fieldAddr, reinterpret_cast<uintptr_t>(&record_to_write),
        sizeof(RecFieldMirror), RecFieldGCTib(), thread_local_data);
}

void pollSafepointAndUpdateTLD(DYN_ThreadLocalData& thread_local_data_ref) {
    auto interface = mockInstance.cjNativeInterface;
    interface.safePoint();
    thread_local_data_ref = interface.getThreadLocalData();
}

void ThreadLocalDataTestMock::testThreadLocalData(DYN_ThreadLocalData thread_local_data_arg) {
    printf("[Mock] testThreadLocalData\n");
    auto interface = mockInstance.cjNativeInterface;
    DYN_ThreadLocalData thread_local_data = thread_local_data_arg;
    auto tiObject = interface.typeInfo("std.core:Object");

    if (!tiObject) {
        printf("[Mock] error: type info std.core:Object not found\n");
        return;
    }


    // Static reference field read/write
    DYN_ObjRef* static_object_placeholder = reinterpret_cast<DYN_ObjRef*>(get_symbol_address("_CN4test34static_field_for_fastpath_barriersE"));
    if (!static_object_placeholder) {
        printf("[Mock] error: symbol _CN4test34static_field_for_fastpath_barriersE not found\n");
        return;
    }
    int allocateEveryNth = 1000;
    int pollSafepointEveryNth = 3333;
    int callI2CEveryNth = 100000;
    for (int i = 0; i < ITERATIONS * 10; i++) {
        if (i % allocateEveryNth == 0) {
            allocateObjectAndUpdateTLD("std.core:Object", thread_local_data);
            frameRoots[1] = frameRoots[0];
        }
        if (i % pollSafepointEveryNth == 0) {
            pollSafepointAndUpdateTLD(thread_local_data);
        }
        if (i % callI2CEveryNth == 0) {
            mockInstance.invokeI2C("_CN4test17i2cAllocationLoopHv", thread_local_data);
        }

        if (i % 2 == 0) {
            frameRoots[0] = frameRoots[1];
            staticWriteBarrierFast(static_object_placeholder, thread_local_data);
        } else {
            staticReadBarrierFast(static_object_placeholder);
            if (!frameRoots[0]) {
                printf("[Mock] error: read null from static field (fast path)\n");
                return;
            }
        }
    }
    printf("[Mock] finished static fast barriers testing\n");


    for (int i = 0; i < ITERATIONS; i++) {
        if (i % callI2CEveryNth == 0) {
            mockInstance.invokeI2C("_CN4test17i2cAllocationLoopHv", thread_local_data);
        }
        if (i % 2 == 0) {
            writeAndCheckRecFieldFast(thread_local_data);
        } else {
            readAndCheckRecField();
        }
    }
    printf("[Mock] finished record-in-object fast barriers testing\n");
    printf("[Mock] testThreadLocalData: OK\n");
}
