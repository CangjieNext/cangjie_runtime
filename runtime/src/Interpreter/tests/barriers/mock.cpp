#include <vector>
#include <atomic>
#include <cassert>
#include <cstring>
#include "mock.h"
#include "log.h"
#include "libinterpreter.h"

extern MOCK_IMPLEMENTATION mockInstance;

using Ref = void*;

#define FRAME_ROOTS_N  8
#define GLOBAL_ROOTS_N 64
#define SAVED_ROOTS_N  32
#define ITERATIONS 20000000

// compaction test
Ref savedRoots[SAVED_ROOTS_N];
Ref adjustableRoots[SAVED_ROOTS_N];

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

BarriersMock::BarriersMock(interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf)
    : Mock(interpreter_interf, cjnative_interf)
{
    interpreter_interf->iterate_frames_with_state = &IterateFramesWithState;
    interpreter_interf->visit_frame_roots_marking = &markFrameRoots;
    interpreter_interf->visit_frame_roots_adjusting = &adjustFrameRoots;
    interpreter_interf->visit_global_roots = &visitGlobalRoots;
    interpreter_interf->fiber_start = &doOnFiberStart;

    registerC2I(0, reinterpret_cast<uintptr_t>(testBarriers));
    registerC2I(1, reinterpret_cast<uintptr_t>(testBarriersFastPath));
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

void BarriersMock::visitGlobalRoots(root_visitor_t visitor) {
    scanRefArray(visitor, globalRoots, GLOBAL_ROOTS_N,
        [](root_visitor_t visitor, Ref* ph){
            mockInstance.cjNativeInterface.visit_root_from_interpreter(visitor, ph);
        });
    scanRefArray(visitor, adjustableRoots, SAVED_ROOTS_N,
        [](root_visitor_t visitor, Ref* ph){
            mockInstance.cjNativeInterface.visit_root_from_interpreter(visitor, ph);
        });
    log("[Mock] finished global roots scanning");
}

void BarriersMock::IterateFramesWithState(fiber_specific_data_t fiberData, void (*callback)(visiting_state_t, void*), void* ctx) {
    log("[Mock] frames iterator called!");
    callback(nullptr /* not used */, ctx);
    log("[Mock] frames iterator finished!");
}

void BarriersMock::markFrameRoots(visiting_state_t state, frame_desc_t desc, root_visitor_t visitor) {
    log("[Mock] frame: (ip=%p, fp=%p)", desc.ip, desc.fp);
    scanRefArray(visitor, static_cast<Ref*>(frameRoots), FRAME_ROOTS_N,
        [](root_visitor_t visitor, Ref* ph){
            mockInstance.cjNativeInterface.visit_root_from_interpreter(visitor, ph);
        });
    log("[Mock] finished frame roots scanning");
}

void BarriersMock::adjustFrameRoots(visiting_state_t state, frame_desc_t desc, root_visitor_t visitor, root_visitor_t derivedPtrVisitor) {
    log("[Mock] frame: (ip=%p, fp=%p)", desc.ip, desc.fp);
    scanRefArray(visitor, static_cast<Ref*>(frameRoots), FRAME_ROOTS_N,
        [](root_visitor_t visitor, Ref* ph){
            mockInstance.cjNativeInterface.visit_root_from_interpreter(visitor, ph);
        });
    log("[Mock] finished frame roots scanning");
}

std::atomic<int64_t> fibers_count(0);

void BarriersMock::doOnFiberStart(fiber_specific_data_t* data) {
    printf("[Mock] doOnFiberStart: data = %p\n", data);
    fibers_count.fetch_add(1);
    auto fiberId = fibers_count.load();
    FiberMagicData* magicData = new FiberMagicData(0xCAFEBABE, fiberId);
    *data = magicData;
    printf("[Mock] doOnFiberStart: fiber id = %ld\n", fiberId);
}

void BarriersMock::init() {
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

void writeAndCheckRecField() {
    auto interface = mockInstance.cjNativeInterface;
    auto tiObject = interface.type_info("std.core:Object");
    if (!tiObject) {
        printf("[Mock] error: type info std.core:Object not found (writeAndCheckRecField)\n");
        return;
    }

    void* static_object_placeholder = get_symbol_address("_CN7default24globalObjectWithRecFieldE");
    if (!static_object_placeholder) {
        printf("[Mock] error: symbol _CN7default24globalObjectWithRecFieldE not found (writeAndCheckRecField)\n");
        return;
    }

    RecFieldMirror record_to_write;
    record_to_write.obj0 = interface.object_alloc(tiObject);
    record_to_write.obj1 = interface.object_alloc(tiObject);
    record_to_write.obj2 = interface.object_alloc(tiObject);
    record_to_write.obj3 = interface.object_alloc(tiObject);

    if (!record_to_write.obj0 || !record_to_write.obj1 || !record_to_write.obj2 || !record_to_write.obj3) {
        printf("[Mock] error: failed to allocate objects for record_to_write\n");
        return;
    }

    auto static_obj = interface.read_static_field(static_object_placeholder);
    if (!static_obj) {
        printf("[Mock] error: read null from static field for record object\n");
        return;
    }

    uintptr_t fieldAddr = reinterpret_cast<uintptr_t>(static_obj) + sizeof(type_info_t*); // start of record inside object
    interface.write_struct_field(static_obj, fieldAddr, reinterpret_cast<uintptr_t>(&record_to_write), sizeof(RecFieldMirror));
}

// ---------------------------------------------------------
// static struct field tests
// ---------------------------------------------------------

void readAndCheckStaticRecField() {
    auto interface = mockInstance.cjNativeInterface;
    void* static_struct_addr = get_symbol_address("_CN7default14globalRecFieldE");
    if (!static_struct_addr) {
        printf("[Mock] error: symbol _CN7default14globalRecFieldE not found\n");
        return;
    }

    RecFieldMirror record;
    // RecField has 4 Object references.
    // GCTib = ShortGCTib (Tag Bit 63 set) | Bitmap 0xF (bits 0,1,2,3 set).
    size_t tib = 0x800000000000000F;

    interface.read_static_struct_field(reinterpret_cast<uintptr_t>(&record), sizeof(RecFieldMirror), 
                                       reinterpret_cast<uintptr_t>(static_struct_addr), sizeof(RecFieldMirror), tib);

    if (!record.obj0 || !record.obj1 || !record.obj2 || !record.obj3) {
        printf("[Mock] error: one or more objects in static record are null\n");
    }
    if (!interface.is_valid_object(record.obj0) || !interface.is_valid_object(record.obj1) ||
        !interface.is_valid_object(record.obj2) || !interface.is_valid_object(record.obj3)) {
        printf("[Mock] error: one or more objects in static record are invalid\n");
    }
}

void writeAndCheckStaticRecField() {
    auto interface = mockInstance.cjNativeInterface;
    auto tiObject = interface.type_info("std.core:Object");
    if (!tiObject) {
        printf("[Mock] error: type info std.core:Object not found (writeAndCheckStaticRecField)\n");
        return;
    }

    void* static_struct_addr = get_symbol_address("_CN7default14globalRecFieldE");
    if (!static_struct_addr) {
        printf("[Mock] error: symbol _CN7default14globalRecFieldE not found (writeAndCheckStaticRecField)\n");
        return;
    }

    RecFieldMirror record_to_write;
    record_to_write.obj0 = interface.object_alloc(tiObject);
    record_to_write.obj1 = interface.object_alloc(tiObject);
    record_to_write.obj2 = interface.object_alloc(tiObject);
    record_to_write.obj3 = interface.object_alloc(tiObject);

    if (!record_to_write.obj0 || !record_to_write.obj1 || !record_to_write.obj2 || !record_to_write.obj3) {
        printf("[Mock] error: failed to allocate objects for record_to_write\n");
        return;
    }

    // RecField has 4 Object references.
    // GCTib = ShortGCTib (Tag Bit 63 set) | Bitmap 0xF (bits 0,1,2,3 set).
    size_t tib = 0x800000000000000F;

    interface.write_static_struct_field(reinterpret_cast<uintptr_t>(static_struct_addr), sizeof(RecFieldMirror), 
                                        reinterpret_cast<uintptr_t>(&record_to_write), sizeof(RecFieldMirror), tib);
}

// ---------------------------------------------------------
// Generic Field Tests
// ---------------------------------------------------------

bool readAndCheckGenericField() {
    auto interface = mockInstance.cjNativeInterface;
    void* global_wrapper_addr = get_symbol_address("_CN7default20globalGenericWrapperE");
    if (!global_wrapper_addr) {
        printf("[Mock] error: symbol _CN7default20globalGenericWrapperE not found\n");
        return false;
    }

    frameRoots[0] = interface.read_static_field(global_wrapper_addr);
    if (!frameRoots[0] || !interface.is_valid_object(frameRoots[0])) {
        printf("[Mock] error: invalid GenericWrapper object read from static field\n");
        return false;
    }

    uintptr_t fieldAddr = reinterpret_cast<uintptr_t>(frameRoots[0]) + sizeof(type_info_t*);

    type_template_t wrapperTemplate = interface.type_template("default:GenericWrapper");
    if (!wrapperTemplate) {
        printf("[Mock] error: type template default:GenericWrapper not found\n");
        return false;
    }
    auto tiRecField = interface.type_info("default:RecField");
    if (!tiRecField) {        
        printf("[Mock] error: type info default:RecField not found\n");
        return false;
    }
    auto typeInfoFull = interface.get_or_create_type_info(wrapperTemplate, 1, &tiRecField);
    if (!typeInfoFull) {
        printf("[Mock] error: failed to get or create type info for GenericWrapper<RecField>\n");
        return false;
    }

    frameRoots[1] = interface.object_alloc(tiRecField); // destination box 
    if (!frameRoots[1] || !interface.is_valid_object(frameRoots[1])) {
        printf("[Mock] error: failed to allocate destination box for generic field read\n");
        return false;
    }
    interface.read_generic_field(frameRoots[1], frameRoots[0], fieldAddr, sizeof(RecFieldMirror));

    uintptr_t fieldAddr2 = reinterpret_cast<uintptr_t>(frameRoots[1]) + sizeof(type_info_t*); // start of record inside box
    RecFieldMirror record;
    interface.read_struct_field(reinterpret_cast<uintptr_t>(&record), frameRoots[1], fieldAddr2, sizeof(RecFieldMirror));

    if (!record.obj0 || !record.obj1 || !record.obj2 || !record.obj3) {
        printf("[Mock] error: one or more objects in generic field record are null\n");
        return false;
    }
    if (!interface.is_valid_object(record.obj0) || !interface.is_valid_object(record.obj1) ||
        !interface.is_valid_object(record.obj2) || !interface.is_valid_object(record.obj3)) {
        printf("[Mock] error: one or more objects in generic field record are invalid\n");
        return false;
    }

    return true;
}

bool writeAndCheckGenericField() {
    auto interface = mockInstance.cjNativeInterface;
    void* global_wrapper_addr = get_symbol_address("_CN7default20globalGenericWrapperE");
    if (!global_wrapper_addr) {
        printf("[Mock] error: symbol _CN7default20globalGenericWrapperE not found\n");
        return false;
    }

    frameRoots[0] = interface.read_static_field(global_wrapper_addr);
    if (!frameRoots[0] || !interface.is_valid_object(frameRoots[0])) {
        printf("[Mock] error: invalid GenericWrapper object read from static field\n");
        return false;
    }

    uintptr_t fieldAddr = reinterpret_cast<uintptr_t>(frameRoots[0]) + sizeof(type_info_t*);

    auto tiRecField = interface.type_info("default:RecField");
    if (!tiRecField) {        
        printf("[Mock] error: type info default:RecField not found\n");
        return false;
    }

    frameRoots[1] = interface.object_alloc(tiRecField); // source box 
    if (!frameRoots[1] || !interface.is_valid_object(frameRoots[1])) {
        printf("[Mock] error: failed to allocate source box for generic field write\n");
        return false;
    }

    auto tiObject = interface.type_info("std.core:Object");
    if (!tiObject) {
        printf("[Mock] error: type info std.core:Object not found\n");
        return false;
    }

    RecFieldMirror record_to_write;
    record_to_write.obj0 = interface.object_alloc(tiObject);
    record_to_write.obj1 = interface.object_alloc(tiObject);
    record_to_write.obj2 = interface.object_alloc(tiObject);
    record_to_write.obj3 = interface.object_alloc(tiObject);

    if (!record_to_write.obj0 || !record_to_write.obj1 || !record_to_write.obj2 || !record_to_write.obj3) {
        printf("[Mock] error: failed to allocate objects for generic field record\n");
        return false;
    }

    // Initialize the source box with the record data
    uintptr_t boxFieldAddr = reinterpret_cast<uintptr_t>(frameRoots[1]) + sizeof(void*);
    interface.write_struct_field(frameRoots[1], boxFieldAddr, reinterpret_cast<uintptr_t>(&record_to_write), sizeof(RecFieldMirror));

    // Perform the generic field write: copy from box (frameRoots[1]) to wrapper (frameRoots[0])
    interface.write_generic_field(frameRoots[0], fieldAddr, frameRoots[1], sizeof(RecFieldMirror));

    return true;
}

// ---------------------------------------------------------

void BarriersMock::testBarriers() {
    printf("[Mock] testBarriers\n");
    auto interface = mockInstance.cjNativeInterface;
    auto tiObject = interface.type_info("std.core:Object");
    if (!tiObject) {
        printf("[Mock] error: type info std.core:Object not found\n");
        return;
    }

    printf("[Mock] fill adjustable roots\n");
    int saveEveryNth = 100;
    for (int i = 0; i < SAVED_ROOTS_N * saveEveryNth; i++) {
        if (i % saveEveryNth == 0) {
            interface.write_static_field(&adjustableRoots[i / saveEveryNth], interface.object_alloc(tiObject));
        } else {
            interface.object_alloc(tiObject); // allocate junk so adjustable roots can be compacted
        }
    }
    memcpy(savedRoots, adjustableRoots, sizeof(Ref) * SAVED_ROOTS_N); // save the initial set of roots for later verification
    printf("[Mock]   adjustable roots filled\n");

    // Static reference field read/write
    void* static_object_placeholder = get_symbol_address("_CN7default17to_be_set_in_mockE");
    if (!static_object_placeholder) {
        printf("[Mock] error: symbol _CN7default17to_be_set_in_mockE not found\n");
        return;
    }
    for (int i = 0; i < ITERATIONS; i++) {
        if (i % 2 == 0) {
            obj_ref_t obj = interface.object_alloc(tiObject);
            if (!obj) {
                printf("[Mock] error: failed to allocate std.core:Object\n");
                return;
            }
            interface.write_static_field(static_object_placeholder, obj);
        } else {
            auto static_obj = interface.read_static_field(static_object_placeholder);
            if (!static_obj) {
                printf("[Mock] error: read null from static field\n");
                return;
            }
            if (!interface.is_valid_object(static_obj)) {
                printf("[Mock] error: invalid object read from static field\n");
                return;
            }
        }
    }
    printf("[Mock] finished static barriers testing\n");

    auto tiObjectContainer = interface.type_info("default:ObjectContainer");
    if (!tiObjectContainer) {
        printf("[Mock] error: type info default:ObjectContainer not found\n");
        return;
    }

    frameRoots[0] = interface.object_alloc(tiObjectContainer);
    
    for (int i = 0; i < ITERATIONS; i++) {
        if (i % 2 == 0) {
            obj_ref_t obj_in_container = interface.object_alloc(tiObject);
            field_ref_t field = getRefField(frameRoots[0], sizeof(void*));
            interface.write_instance_field(frameRoots[0], field, obj_in_container);
        } else {
            field_ref_t field = getRefField(frameRoots[0], sizeof(void*));
            obj_ref_t obj_in_container = interface.read_instance_field(frameRoots[0], field);
            if (!obj_in_container) {
                printf("[Mock] error: read null from instance field\n");
                return;
            }
            if (!interface.is_valid_object(obj_in_container)) {
                printf("[Mock] error: invalid object read from instance field\n");
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
        interface.write_static_field(&globalRoots[i], interface.object_alloc(tiObject));
    }

    printf("[Mock] finished barriers testing\n");

    bool isSomethingWasMoved = false;
    for (int i = 0; i < SAVED_ROOTS_N; i++) {
        auto old_real_address = getUntaggedAddress(savedRoots[i]);
        auto new_real_address = getUntaggedAddress(interface.read_static_field(&adjustableRoots[i]));
        if (new_real_address != old_real_address) {
            isSomethingWasMoved = true;
            log("[Mock] adjustable root %d was moved: old address = %p, new address = %p\n", 
                i, (void*)old_real_address, (void*)new_real_address);
        }
        if (!interface.is_valid_object(interface.read_static_field(&adjustableRoots[i]))) {
            printf("[Mock] error: adjustable root %d is invalid after compaction\n", i);
            return;
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
void staticWriteBarrierFast(obj_ref_t* static_object_placeholder) {
    auto interface = mockInstance.cjNativeInterface;
    if (interface.is_active_gc_phase(interface.get_thread_local_data())) {
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
void instanceWriteBarrierRecordFast(obj_ref_t object, uintptr_t dst, uintptr_t src, size_t size) {
    auto interface = mockInstance.cjNativeInterface;
    if (interface.is_active_gc_phase(interface.get_thread_local_data())) {
        // slow path
        interface.write_struct_field(object, dst, src, size);
    } else {
        // fast path
        memcpy(reinterpret_cast<void*>(dst), reinterpret_cast<void*>(src), size);
    }
}

void writeAndCheckRecFieldFast() {
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
    record_to_write.obj0 = interface.object_alloc(tiObject);
    record_to_write.obj1 = interface.object_alloc(tiObject);
    record_to_write.obj2 = interface.object_alloc(tiObject);
    record_to_write.obj3 = interface.object_alloc(tiObject);

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
    instanceWriteBarrierRecordFast(frameRoots[0], fieldAddr, reinterpret_cast<uintptr_t>(&record_to_write), sizeof(RecFieldMirror));
}

void BarriersMock::testBarriersFastPath() {
    printf("[Mock] testBarriersFastPath\n");
    auto interface = mockInstance.cjNativeInterface;
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
    for (int i = 0; i < ITERATIONS * 5; i++) {
        if (i % 2 == 0) {
            frameRoots[0] = interface.object_alloc(tiObject);
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
            if (!interface.is_valid_object(frameRoots[0])) {
                printf("[Mock] error: invalid object read from static field (fast path)\n");
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

void BarriersMock::stop() {
    bool* continue_allocation_placeholder = (bool*)get_symbol_address("_CN7default18continueAllocationE");
    if (!continue_allocation_placeholder) {
        printf("[Mock] error: symbol _CN7default18continueAllocationE not found\n");
        return;
    }
    *continue_allocation_placeholder = false;
}
