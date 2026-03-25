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
#define ITERATIONS 20000000


Ref frameRoots[FRAME_ROOTS_N];
Ref globalRoots[GLOBAL_ROOTS_N];

class FiberMagicData {
public:
    uint64_t magicValue;
    uint64_t fiberId;
    FiberMagicData(uint64_t magicValue, uint64_t fiberId) {
        this->magicValue = magicValue;
        this->fiberId = fiberId;
    }
};

ThreadLocalDataTestMock::ThreadLocalDataTestMock(interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf)
    : Mock(interpreter_interf, cjnative_interf)
{
    interpreter_interf->iterate_frames_with_state = &IterateFramesWithState;
    interpreter_interf->visit_frame_roots_marking = &markFrameRoots;
    interpreter_interf->visit_frame_roots_adjusting = &adjustFrameRoots;
    interpreter_interf->visit_global_roots = &visitGlobalRoots;

    interpreter_interf->fiber_start = &doOnFiberStart;

    registerC2I(0, reinterpret_cast<uintptr_t>(testThreadLocalData));
    registerC2I(8, reinterpret_cast<uintptr_t>(init));
    registerC2I(9, reinterpret_cast<uintptr_t>(stop));
}

void scanRefArray(root_visitor_t visitor, Ref* roots, int size, std::function<void(root_visitor_t, Ref*)> visit) {
    Ref* ph = roots;
    for (int i = 0; i < size; i++) {
        visit(visitor, ph);
        ph++;
    }
}

void ThreadLocalDataTestMock::visitGlobalRoots(root_visitor_t visitor) {
    scanRefArray(visitor, globalRoots, GLOBAL_ROOTS_N,
        [](root_visitor_t visitor, Ref* ph){
            mockInstance.cjNativeInterface.visit_root_from_interpreter(visitor, ph);
        });
    log("[Mock] finished global roots scanning");
}

void ThreadLocalDataTestMock::IterateFramesWithState(fiber_specific_data_t fiberData, void (*callback)(visiting_state_t, void*), void* ctx) {
    log("[Mock] frames iterator called!");
    callback(fiberData, ctx);
    log("[Mock] frames iterator finished!");
}

void ThreadLocalDataTestMock::markFrameRoots(visiting_state_t state, frame_desc_t desc, root_visitor_t visitor) {
    log("[Mock] frame: (ip=%p, fp=%p)", desc.ip, desc.fp);

    scanRefArray(visitor, static_cast<Ref*>(frameRoots), FRAME_ROOTS_N,
        [](root_visitor_t visitor, Ref* ph){
            mockInstance.cjNativeInterface.visit_root_from_interpreter(visitor, ph);
        });

    auto magic = static_cast<FiberMagicData*>(state);
    check(magic->magicValue == 0xCAFEBABE,
        "[Interpreter] fiber: magic value mismatch (expected: 0xCAFEBABE, got: 0x%016lX)\n",
        magic->magicValue);

    log("[Mock] finished frame roots marking");
}

void ThreadLocalDataTestMock::adjustFrameRoots(visiting_state_t state, frame_desc_t desc, root_visitor_t visitor, derived_ptr_visitor_t derivedPtrVisitor) {
    log("[Mock] frame: (ip=%p, fp=%p)", desc.ip, desc.fp);

    scanRefArray(visitor, static_cast<Ref*>(frameRoots), FRAME_ROOTS_N,
        [](root_visitor_t visitor, Ref* ph){
            mockInstance.cjNativeInterface.visit_root_from_interpreter(visitor, ph);
        });

    log("[Mock] finished frame roots adjusting");
}

std::atomic<int64_t> fibers_count(0);

void ThreadLocalDataTestMock::doOnFiberStart(fiber_specific_data_t* data) {
    printf("[Mock] doOnFiberStart: data = %p\n", data);
    fibers_count.fetch_add(1);
    auto fiberId = fibers_count.load();
    FiberMagicData* magicData = new FiberMagicData(0xCAFEBABE, fiberId);
    *data = magicData;
    printf("[Mock] doOnFiberStart: fiber id = %ld\n", fiberId);
}

void ThreadLocalDataTestMock::init() {
    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.type_info("std.core:Object");

    for (int i = 0; i < GLOBAL_ROOTS_N; i++) {
        interface.write_static_field(&globalRoots[i], interface.object_alloc(ti));
    }

    for (int i = 0; i < FRAME_ROOTS_N; i++) {
        frameRoots[i] = interface.object_alloc(ti);
    }
}

void readAndCheckRecField() {
    auto interface = mockInstance.cjNativeInterface;
    void* static_object_placeholder = get_symbol_address("_CN7default24globalObjectWithRecFieldE");
    if (!static_object_placeholder) {
        printf("[Mock] error: symbol _CN7default24globalObjectWithRecFieldE not found\n");
        return;
    }
    auto static_obj = interface.read_static_field(static_object_placeholder);
    uintptr_t fieldAddr = reinterpret_cast<uintptr_t>(static_obj) + sizeof(type_info_t*); // start of record inside object
    RecFieldMirror record;
    interface.read_struct_field(reinterpret_cast<uintptr_t>(&record), static_obj, fieldAddr, sizeof(RecFieldMirror));

    if (!record.obj0 || !record.obj1 || !record.obj2 || !record.obj3) {
        printf("[Mock] error: one or more objects in record are null\n");
    }
    if (!interface.is_valid_object(record.obj0) || !interface.is_valid_object(record.obj1) ||
        !interface.is_valid_object(record.obj2) || !interface.is_valid_object(record.obj3)) {
        printf("[Mock] error: one or more objects in record are invalid\n");
    }
}


/**
 * Reads a referense from provided static field and places it to frameRoots[0].
 * Simulates a read barrier with fast path.
 */
void staticReadBarrierFast(obj_ref_t* static_object_placeholder) {
    auto interface = mockInstance.cjNativeInterface;
    
    obj_ref_t raw_ref = *static_object_placeholder;
    if ((reinterpret_cast<uintptr_t>(raw_ref) >> 48) == 0) { // fast path: no tags
        frameRoots[0] = raw_ref; // place result in a root slot to keep it alive during GC
    } else {
        frameRoots[0] = interface.read_static_field(static_object_placeholder);
    }
}

/**
 * Writes a referense from frameRoots[0] to provided static field.
 * Simulates a write barrier with fast path.
 */
void staticWriteBarrierFast(obj_ref_t* static_object_placeholder, thread_local_data_t thread_local_data) {
    auto interface = mockInstance.cjNativeInterface;
    if (interface.is_active_gc_phase(thread_local_data)) {
        // slow path
        interface.write_static_field(static_object_placeholder, frameRoots[0]);
    } else {
        // fast path
        *static_object_placeholder = frameRoots[0];
    }
}

/**
 * Writes a record from src to dst field of object.
 * Simulates a write barrier with fast path.
 */
void instanceWriteBarrierRecordFast(obj_ref_t object, uintptr_t dst, uintptr_t src, size_t size, thread_local_data_t thread_local_data) {
    auto interface = mockInstance.cjNativeInterface;
    if (interface.is_active_gc_phase(thread_local_data)) {
        // slow path
        interface.write_struct_field(object, dst, src, size);
    } else {
        // fast path
        memcpy(reinterpret_cast<void*>(dst), reinterpret_cast<void*>(src), size);
    }
}

/**
 * Allocates an object of given type and updates thread local data, which is possibly changed due to GC-points in allocators.
 * Allocated object is placed in frameRoots[0] to be traceble by GC.
 */
void allocateObjectAndUpdateTLD(const char* tiName, thread_local_data_t& thread_local_data_ref) {
    auto interface = mockInstance.cjNativeInterface;
    auto ti = interface.type_info(tiName);
    if (!ti) {
        printf("[Mock] error: type info %s not found (allocateObjectAndUpdateTLD)\n", tiName);
        return;
    }
    frameRoots[0] = interface.object_alloc(ti);
    if (!frameRoots[0]) {
        printf("[Mock] error: failed to allocate %s in allocateObjectAndUpdateTLD\n", tiName);
        return;
    }
    thread_local_data_ref = interface.get_thread_local_data();
}

void writeAndCheckRecFieldFast(thread_local_data_t& thread_local_data) {
    auto interface = mockInstance.cjNativeInterface;
    auto tiObject = interface.type_info("std.core:Object");
    if (!tiObject) {
        printf("[Mock] error: type info std.core:Object not found (writeAndCheckRecField)\n");
        return;
    }

    obj_ref_t* static_object_placeholder = reinterpret_cast<obj_ref_t*>(get_symbol_address("_CN7default24globalObjectWithRecFieldE"));
    if (!static_object_placeholder) {
        printf("[Mock] error: symbol _CN7default24globalObjectWithRecFieldE not found (writeAndCheckRecField)\n");
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

    uintptr_t fieldAddr = reinterpret_cast<uintptr_t>(frameRoots[0]) + sizeof(type_info_t*); // start of record inside object
    instanceWriteBarrierRecordFast(frameRoots[0], fieldAddr, reinterpret_cast<uintptr_t>(&record_to_write), sizeof(RecFieldMirror), thread_local_data);
}

void pollSafepointAndUpdateTLD(thread_local_data_t& thread_local_data_ref) {
    auto interface = mockInstance.cjNativeInterface;
    interface.safe_point();
    thread_local_data_ref = interface.get_thread_local_data();
}

void ThreadLocalDataTestMock::testThreadLocalData(thread_local_data_t thread_local_data_arg) {
    printf("[Mock] testThreadLocalData\n");
    auto interface = mockInstance.cjNativeInterface;
    thread_local_data_t thread_local_data = thread_local_data_arg;
    auto tiObject = interface.type_info("std.core:Object");
    
    if (!tiObject) {
        printf("[Mock] error: type info std.core:Object not found\n");
        return;
    }


    // Static reference field read/write
    obj_ref_t* static_object_placeholder = reinterpret_cast<obj_ref_t*>(get_symbol_address("_CN7default34static_field_for_fastpath_barriersE"));
    if (!static_object_placeholder) {
        printf("[Mock] error: symbol _CN7default34static_field_for_fastpath_barriersE not found\n");
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
            mockInstance.invokeI2C("_CN7default17i2cAllocationLoopHv", thread_local_data);
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
            if (!interface.is_valid_object(frameRoots[0])) {
                printf("[Mock] error: invalid object read from static field (fast path)\n");
                return;
            }
        }
    }
    printf("[Mock] finished static fast barriers testing\n");

    
    for (int i = 0; i < ITERATIONS; i++) {
        if (i % callI2CEveryNth == 0) {
            mockInstance.invokeI2C("_CN7default17i2cAllocationLoopHv", thread_local_data);
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

void ThreadLocalDataTestMock::stop() {
    bool* continue_allocation_placeholder = (bool*)get_symbol_address("_CN7default18continueAllocationE");
    if (!continue_allocation_placeholder) {
        printf("[Mock] error: symbol _CN7default18continueAllocationE not found\n");
        return;
    }
    *continue_allocation_placeholder = false;
}
