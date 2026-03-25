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
#define SAVED_ROOTS_N  32
#define ITERATIONS CJ_INTERPRETER_TEST_ITER(20000000)

// compaction test
Ref savedRoots[SAVED_ROOTS_N];
Ref adjustableRoots[SAVED_ROOTS_N];

Ref frameRoots[FRAME_ROOTS_N];
Ref globalRoots[GLOBAL_ROOTS_N];

class CJThreadMagicData {
public:
    uint64_t magicValue;
    uint64_t cjThreadId;
    CJThreadMagicData(uint64_t magicValue, uint64_t cjThreadId) {
        this->magicValue = magicValue;
        this->cjThreadId = cjThreadId;
    }
};

BarriersMock::BarriersMock(INT_InterpreterInterface* interpreter_interf, DYN_CJNativeInterface* cjnative_interf)
    : Mock(interpreter_interf, cjnative_interf)
{
    interpreter_interf->iterateFramesWithState = &IterateFramesWithState;
    interpreter_interf->visitFrameRootsMarking = &markFrameRoots;
    interpreter_interf->visitFrameRootsAdjusting = &adjustFrameRoots;
    interpreter_interf->visitGlobalRoots = &visitGlobalRoots;
    interpreter_interf->cjThreadOnStart = &doOnCJThreadStart;

    registerC2I(0, reinterpret_cast<uintptr_t>(testBarriers));
    registerC2I(1, reinterpret_cast<uintptr_t>(testBarriersFastPath));
    registerC2I(8, reinterpret_cast<uintptr_t>(init));
}

void scanRefArray(DYN_RootVisitor visitor, Ref* roots, int size, std::function<void(DYN_RootVisitor, Ref*)> visit) {
    Ref* ph = roots;
    for (int i = 0; i < size; i++) {
        visit(visitor, ph);
        ph++;
    }
}

void BarriersMock::visitGlobalRoots(DYN_RootVisitor visitor) {
    scanRefArray(visitor, globalRoots, GLOBAL_ROOTS_N,
        [](DYN_RootVisitor visitor, Ref* ph){
            mockInstance.cjNativeInterface.visitRootFromInterpreter(visitor, ph);
        });
    scanRefArray(visitor, adjustableRoots, SAVED_ROOTS_N,
        [](DYN_RootVisitor visitor, Ref* ph){
            mockInstance.cjNativeInterface.visitRootFromInterpreter(visitor, ph);
        });
    log("[Mock] finished global roots scanning");
}

void BarriersMock::IterateFramesWithState(DYN_CJThreadSpecificData cjThreadData, void (*callback)(DYN_VisitingState, void*), void* ctx) {
    log("[Mock] frames iterator called!");
    callback(nullptr /* not used */, ctx);
    log("[Mock] frames iterator finished!");
}

void BarriersMock::markFrameRoots(DYN_VisitingState state, INT_FrameDesc desc, DYN_RootVisitor visitor) {
    log("[Mock] frame: (ip=%p, fp=%p)", desc.ip, desc.fp);
    scanRefArray(visitor, static_cast<Ref*>(frameRoots), FRAME_ROOTS_N,
        [](DYN_RootVisitor visitor, Ref* ph){
            mockInstance.cjNativeInterface.visitRootFromInterpreter(visitor, ph);
        });
    log("[Mock] finished frame roots scanning");
}

void BarriersMock::adjustFrameRoots(DYN_VisitingState state, INT_FrameDesc desc, DYN_RootVisitor visitor, DYN_RootVisitor derivedPtrVisitor) {
    log("[Mock] frame: (ip=%p, fp=%p)", desc.ip, desc.fp);
    scanRefArray(visitor, static_cast<Ref*>(frameRoots), FRAME_ROOTS_N,
        [](DYN_RootVisitor visitor, Ref* ph){
            mockInstance.cjNativeInterface.visitRootFromInterpreter(visitor, ph);
        });
    log("[Mock] finished frame roots scanning");
}

std::atomic<int64_t> cjThreads_count(0);

void BarriersMock::doOnCJThreadStart(DYN_CJThreadSpecificData* data) {
    printf("[Mock] doOnCJThreadStart: data = %p\n", data);
    cjThreads_count.fetch_add(1);
    auto cjThreadId = cjThreads_count.load();
    CJThreadMagicData* magicData = new CJThreadMagicData(0xCAFEBABE, cjThreadId);
    *data = magicData;
    printf("[Mock] doOnCJThreadStart: cjThread id = %ld\n", cjThreadId);
}

void BarriersMock::init() {
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
    interface.readStructField(reinterpret_cast<uintptr_t>(&record), static_obj, fieldAddr, sizeof(RecFieldMirror));

    if (!record.obj0 || !record.obj1 || !record.obj2 || !record.obj3) {
        printf("[Mock] error: one or more objects in record are null\n");
    }
}

void writeAndCheckRecField() {
    auto interface = mockInstance.cjNativeInterface;
    auto tiObject = interface.typeInfo("std.core:Object");
    if (!tiObject) {
        printf("[Mock] error: type info std.core:Object not found (writeAndCheckRecField)\n");
        return;
    }

    void* static_object_placeholder = get_symbol_address("_CN4test24globalObjectWithRecFieldE");
    if (!static_object_placeholder) {
        printf("[Mock] error: symbol _CN4test24globalObjectWithRecFieldE not found (writeAndCheckRecField)\n");
        return;
    }

    RecFieldMirror record_to_write;
    record_to_write.obj0 = interface.objectAlloc(tiObject);
    record_to_write.obj1 = interface.objectAlloc(tiObject);
    record_to_write.obj2 = interface.objectAlloc(tiObject);
    record_to_write.obj3 = interface.objectAlloc(tiObject);

    if (!record_to_write.obj0 || !record_to_write.obj1 || !record_to_write.obj2 || !record_to_write.obj3) {
        printf("[Mock] error: failed to allocate objects for record_to_write\n");
        return;
    }

    auto static_obj = interface.readStaticField(static_object_placeholder);
    if (!static_obj) {
        printf("[Mock] error: read null from static field for record object\n");
        return;
    }

    uintptr_t fieldAddr = reinterpret_cast<uintptr_t>(static_obj) + sizeof(DYN_TypeInfo*); // start of record inside object
    interface.writeStructField(static_obj, fieldAddr, reinterpret_cast<uintptr_t>(&record_to_write), sizeof(RecFieldMirror));
}

// ---------------------------------------------------------
// static struct field tests
// ---------------------------------------------------------

void readAndCheckStaticRecField() {
    auto interface = mockInstance.cjNativeInterface;
    void* static_struct_addr = get_symbol_address("_CN4test14globalRecFieldE");
    if (!static_struct_addr) {
        printf("[Mock] error: symbol _CN4test14globalRecFieldE not found\n");
        return;
    }

    RecFieldMirror record;
    // RecField has 4 Object references.
    // GCTib = ShortGCTib (Tag Bit 63 set) | Bitmap 0xF (bits 0,1,2,3 set).
    DYN_GCTib tib = {};
    tib.raw = 0x800000000000000FULL;

    interface.readStaticStructField(reinterpret_cast<uintptr_t>(&record), sizeof(RecFieldMirror),
                                       reinterpret_cast<uintptr_t>(static_struct_addr), sizeof(RecFieldMirror), tib);

    if (!record.obj0 || !record.obj1 || !record.obj2 || !record.obj3) {
        printf("[Mock] error: one or more objects in static record are null\n");
    }
}

void writeAndCheckStaticRecField() {
    auto interface = mockInstance.cjNativeInterface;
    auto tiObject = interface.typeInfo("std.core:Object");
    if (!tiObject) {
        printf("[Mock] error: type info std.core:Object not found (writeAndCheckStaticRecField)\n");
        return;
    }

    void* static_struct_addr = get_symbol_address("_CN4test14globalRecFieldE");
    if (!static_struct_addr) {
        printf("[Mock] error: symbol _CN4test14globalRecFieldE not found (writeAndCheckStaticRecField)\n");
        return;
    }

    RecFieldMirror record_to_write;
    record_to_write.obj0 = interface.objectAlloc(tiObject);
    record_to_write.obj1 = interface.objectAlloc(tiObject);
    record_to_write.obj2 = interface.objectAlloc(tiObject);
    record_to_write.obj3 = interface.objectAlloc(tiObject);

    if (!record_to_write.obj0 || !record_to_write.obj1 || !record_to_write.obj2 || !record_to_write.obj3) {
        printf("[Mock] error: failed to allocate objects for record_to_write\n");
        return;
    }

    // RecField has 4 Object references.
    // GCTib = ShortGCTib (Tag Bit 63 set) | Bitmap 0xF (bits 0,1,2,3 set).
    DYN_GCTib tib = {};
    tib.raw = 0x800000000000000FULL;

    interface.writeStaticStructField(reinterpret_cast<uintptr_t>(static_struct_addr), sizeof(RecFieldMirror),
                                        reinterpret_cast<uintptr_t>(&record_to_write), sizeof(RecFieldMirror), tib);
}

// ---------------------------------------------------------
// Generic Field Tests
// ---------------------------------------------------------

bool readAndCheckGenericField() {
    auto interface = mockInstance.cjNativeInterface;
    void* global_wrapper_addr = get_symbol_address("_CN4test20globalGenericWrapperE");
    if (!global_wrapper_addr) {
        printf("[Mock] error: symbol _CN4test20globalGenericWrapperE not found\n");
        return false;
    }

    frameRoots[0] = interface.readStaticField(global_wrapper_addr);
    if (!frameRoots[0]) {
        printf("[Mock] error: read null GenericWrapper object from static field\n");
        return false;
    }

    uintptr_t fieldAddr = reinterpret_cast<uintptr_t>(frameRoots[0]) + sizeof(DYN_TypeInfo*);

    DYN_TypeTemplate wrapperTemplate = interface.typeTemplate("test:GenericWrapper");
    if (!wrapperTemplate) {
        printf("[Mock] error: type template test:GenericWrapper not found\n");
        return false;
    }
    auto tiRecField = interface.typeInfo("test:RecField");
    if (!tiRecField) {
        printf("[Mock] error: type info test:RecField not found\n");
        return false;
    }
    auto typeInfoFull = interface.getOrCreateTypeInfo(wrapperTemplate, 1, &tiRecField);
    if (!typeInfoFull) {
        printf("[Mock] error: failed to get or create type info for GenericWrapper<RecField>\n");
        return false;
    }

    frameRoots[1] = interface.objectAlloc(tiRecField); // destination box
    if (!frameRoots[1]) {
        printf("[Mock] error: failed to allocate destination box for generic field read\n");
        return false;
    }
    interface.readGenericField(frameRoots[1], frameRoots[0], fieldAddr, sizeof(RecFieldMirror));

    uintptr_t fieldAddr2 = reinterpret_cast<uintptr_t>(frameRoots[1]) + sizeof(DYN_TypeInfo*); // start of record inside box
    RecFieldMirror record;
    interface.readStructField(reinterpret_cast<uintptr_t>(&record), frameRoots[1], fieldAddr2, sizeof(RecFieldMirror));

    if (!record.obj0 || !record.obj1 || !record.obj2 || !record.obj3) {
        printf("[Mock] error: one or more objects in generic field record are null\n");
        return false;
    }

    return true;
}

bool writeAndCheckGenericField() {
    auto interface = mockInstance.cjNativeInterface;
    void* global_wrapper_addr = get_symbol_address("_CN4test20globalGenericWrapperE");
    if (!global_wrapper_addr) {
        printf("[Mock] error: symbol _CN4test20globalGenericWrapperE not found\n");
        return false;
    }

    frameRoots[0] = interface.readStaticField(global_wrapper_addr);
    if (!frameRoots[0]) {
        printf("[Mock] error: read null GenericWrapper object from static field\n");
        return false;
    }

    uintptr_t fieldAddr = reinterpret_cast<uintptr_t>(frameRoots[0]) + sizeof(DYN_TypeInfo*);

    auto tiRecField = interface.typeInfo("test:RecField");
    if (!tiRecField) {
        printf("[Mock] error: type info test:RecField not found\n");
        return false;
    }

    frameRoots[1] = interface.objectAlloc(tiRecField); // source box
    if (!frameRoots[1]) {
        printf("[Mock] error: failed to allocate source box for generic field write\n");
        return false;
    }

    auto tiObject = interface.typeInfo("std.core:Object");
    if (!tiObject) {
        printf("[Mock] error: type info std.core:Object not found\n");
        return false;
    }

    RecFieldMirror record_to_write;
    record_to_write.obj0 = interface.objectAlloc(tiObject);
    record_to_write.obj1 = interface.objectAlloc(tiObject);
    record_to_write.obj2 = interface.objectAlloc(tiObject);
    record_to_write.obj3 = interface.objectAlloc(tiObject);

    if (!record_to_write.obj0 || !record_to_write.obj1 || !record_to_write.obj2 || !record_to_write.obj3) {
        printf("[Mock] error: failed to allocate objects for generic field record\n");
        return false;
    }

    // Initialize the source box with the record data
    uintptr_t boxFieldAddr = reinterpret_cast<uintptr_t>(frameRoots[1]) + sizeof(void*);
    interface.writeStructField(frameRoots[1], boxFieldAddr, reinterpret_cast<uintptr_t>(&record_to_write), sizeof(RecFieldMirror));

    // Perform the generic field write: copy from box (frameRoots[1]) to wrapper (frameRoots[0])
    interface.writeGenericField(frameRoots[0], fieldAddr, frameRoots[1], sizeof(RecFieldMirror));

    return true;
}

// ---------------------------------------------------------

void BarriersMock::testBarriers() {
    printf("[Mock] testBarriers\n");
    auto interface = mockInstance.cjNativeInterface;
    auto tiObject = interface.typeInfo("std.core:Object");
    if (!tiObject) {
        printf("[Mock] error: type info std.core:Object not found\n");
        return;
    }

    printf("[Mock] fill adjustable roots\n");
    int saveEveryNth = 100;
    for (int i = 0; i < SAVED_ROOTS_N * saveEveryNth; i++) {
        if (i % saveEveryNth == 0) {
            interface.writeStaticField(&adjustableRoots[i / saveEveryNth], interface.objectAlloc(tiObject));
        } else {
            interface.objectAlloc(tiObject); // allocate junk so adjustable roots can be compacted
        }
    }
    memcpy(savedRoots, adjustableRoots, sizeof(Ref) * SAVED_ROOTS_N); // save the initial set of roots for later verification
    printf("[Mock]   adjustable roots filled\n");

    // Static reference field read/write
    void* static_object_placeholder = get_symbol_address("_CN4test17to_be_set_in_mockE");
    if (!static_object_placeholder) {
        printf("[Mock] error: symbol _CN4test17to_be_set_in_mockE not found\n");
        return;
    }
    for (int i = 0; i < ITERATIONS; i++) {
        if (i % 2 == 0) {
            DYN_ObjRef obj = interface.objectAlloc(tiObject);
            if (!obj) {
                printf("[Mock] error: failed to allocate std.core:Object\n");
                return;
            }
            interface.writeStaticField(static_object_placeholder, obj);
        } else {
            auto static_obj = interface.readStaticField(static_object_placeholder);
            if (!static_obj) {
                printf("[Mock] error: read null from static field\n");
                return;
            }
        }
    }
    printf("[Mock] finished static barriers testing\n");

    auto tiObjectContainer = interface.typeInfo("test:ObjectContainer");
    if (!tiObjectContainer) {
        printf("[Mock] error: type info test:ObjectContainer not found\n");
        return;
    }

    frameRoots[0] = interface.objectAlloc(tiObjectContainer);

    for (int i = 0; i < ITERATIONS; i++) {
        if (i % 2 == 0) {
            DYN_ObjRef obj_in_container = interface.objectAlloc(tiObject);
            DYN_FieldRef field = getRefField(frameRoots[0], sizeof(void*));
            interface.writeInstanceField(frameRoots[0], field, obj_in_container);
        } else {
            DYN_FieldRef field = getRefField(frameRoots[0], sizeof(void*));
            DYN_ObjRef obj_in_container = interface.readInstanceField(frameRoots[0], field);
            if (!obj_in_container) {
                printf("[Mock] error: read null from instance field\n");
                return;
            }
        }
    }

    printf("[Mock] finished instance barriers testing\n");

    for (int i = 0; i < ITERATIONS/2; i++) {
        if (i % 2 == 0) {
            writeAndCheckRecField();
        } else {
            readAndCheckRecField();
        }
    }

    printf("[Mock] finished record-in-object barriers testing\n");

    for (int i = 0; i < ITERATIONS/2; i++) {
        if (i % 2 == 0) {
            writeAndCheckStaticRecField();
        } else {
            readAndCheckStaticRecField();
        }
    }
    printf("[Mock] finished static struct barriers testing\n");

    for (int i = 0; i < ITERATIONS/2; i++) {
        if (i % 2 == 0) {
            if (!writeAndCheckGenericField()) {
                return;
            }
        } else {
            if (!readAndCheckGenericField()) {
                return;
            }
        }
    }
    printf("[Mock] finished generic field barriers testing\n");

    for (int i = 0; i < GLOBAL_ROOTS_N; i++) {
        interface.writeStaticField(&globalRoots[i], interface.objectAlloc(tiObject));
    }

    printf("[Mock] finished barriers testing\n");

    bool isSomethingWasMoved = false;
    for (int i = 0; i < SAVED_ROOTS_N; i++) {
        auto old_real_address = getUntaggedAddress(savedRoots[i]);
        auto new_real_address = getUntaggedAddress(interface.readStaticField(&adjustableRoots[i]));
        if (new_real_address != old_real_address) {
            isSomethingWasMoved = true;
            log("[Mock] adjustable root %d was moved: old address = %p, new address = %p\n",
                i, (void*)old_real_address, (void*)new_real_address);
        }
    }
    if (isSomethingWasMoved) {
        printf("[Mock] compaction: OK\n");
    } else {
        printf("[Mock] compaction: FAILED?\n");
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
void staticWriteBarrierFast(DYN_ObjRef* static_object_placeholder) {
    auto interface = mockInstance.cjNativeInterface;
    if (interface.isActiveGcPhase(interface.getThreadLocalData())) {
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
void instanceWriteBarrierRecordFast(DYN_ObjRef object, uintptr_t dst, uintptr_t src, size_t size) {
    auto interface = mockInstance.cjNativeInterface;
    if (interface.isActiveGcPhase(interface.getThreadLocalData())) {
        // slow path
        interface.writeStructField(object, dst, src, size);
    } else {
        // fast path
        memcpy(reinterpret_cast<void*>(dst), reinterpret_cast<void*>(src), size);
    }
}

void writeAndCheckRecFieldFast() {
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
    record_to_write.obj0 = interface.objectAlloc(tiObject);
    record_to_write.obj1 = interface.objectAlloc(tiObject);
    record_to_write.obj2 = interface.objectAlloc(tiObject);
    record_to_write.obj3 = interface.objectAlloc(tiObject);

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
    instanceWriteBarrierRecordFast(frameRoots[0], fieldAddr, reinterpret_cast<uintptr_t>(&record_to_write), sizeof(RecFieldMirror));
}

void BarriersMock::testBarriersFastPath() {
    printf("[Mock] testBarriersFastPath\n");
    auto interface = mockInstance.cjNativeInterface;
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
    for (int i = 0; i < ITERATIONS * 2; i++) {
        if (i % 2 == 0) {
            frameRoots[0] = interface.objectAlloc(tiObject);
            if (!frameRoots[0]) {
                printf("[Mock] error: failed to allocate std.core:Object\n");
                return;
            }
            staticWriteBarrierFast(static_object_placeholder);
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
        if (i % 2 == 0) {
            writeAndCheckRecFieldFast();
        } else {
            readAndCheckRecField();
        }
    }
    printf("[Mock] finished record-in-object fast barriers testing\n");
}
