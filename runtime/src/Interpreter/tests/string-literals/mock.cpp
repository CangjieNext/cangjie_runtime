#include "mock.h"
#include "log.h"
#include "utils.h"
#include "cangjieUtils.h"
#include <cstring>

extern MOCK_IMPLEMENTATION mockInstance;

#define CONTENT "HelloWorld"
#define CONTENT_SIZE (sizeof(CONTENT) - 1) // do not include 0-terminator

uint8_t literalsStorage[sizeof(RawArray) + CONTENT_SIZE];

StrLiteralMock::StrLiteralMock(int a1, interpreter_interface_t* interpreter_interf, cjnative_interface_t* cjnative_interf)
    : Mock(a1, interpreter_interf, cjnative_interf) 
{
    registerC2I(0, reinterpret_cast<uintptr_t>(produceLiteralSlow1));
    registerC2I(1, reinterpret_cast<uintptr_t>(produceLiteralSlow2));
    registerC2I(2, reinterpret_cast<uintptr_t>(produceLiteralFast1));
    registerC2I(3, reinterpret_cast<uintptr_t>(produceLiteralFast2));
    registerC2I(8, reinterpret_cast<uintptr_t>(init));
}

void* getUtf8ArrayTI() {
    void* ti = mockInstance.cjNativeInterface.type_info("RawArray<UInt8>");
    check(ti != nullptr, "[Mock] couldn't find type info for RawArray<UInt8>");
    return ti;
}

void replaceStringField(RawArray* array, uint32_t start, uint32_t len) {
    log("[Mock] replace string field content {array = %p, start = %u, len = %u}", array, start, len);

    CJString tmp;
    tmp.rawArray = array;
    tmp.start    = start;
    tmp.len      = len;

    CJString* testFieldAddr = (CJString*) get_symbol_address("_CN7default9testFieldE");
    check(testFieldAddr != nullptr, "[Mock] couldn't replace field");

    mockInstance.cjNativeInterface.write_static_struct_field(
        reinterpret_cast<uintptr_t>(testFieldAddr), sizeof(CJString), 
        reinterpret_cast<uintptr_t>(&tmp),          sizeof(CJString),
        (size_t) 0x8000000000000001); // only first field of String is ref

    log("[Mock] string field content is modified");
}

void StrLiteralMock::init() {
    log("[Mock] start initialization");

    RawArray* array = (RawArray*) literalsStorage;
    {
        array->typeInfo = getUtf8ArrayTI();
        array->size = CONTENT_SIZE;
        std::memcpy(array->content, CONTENT, CONTENT_SIZE); // copy without 0-terminator
    }

    log("[Mock] initialization finished");
}

/**
 * Such method of literal initialization could be used if runtime wouldn't support out-of-heap objects. 
 */
void StrLiteralMock::produceLiteralSlow1() {
    obj_ref_t obj = mockInstance.cjNativeInterface.array_alloc(getUtf8ArrayTI(), CONTENT_SIZE);
    check(obj != nullptr, "allocation error");

    RawArray* array = static_cast<RawArray*>(obj);
    std::memcpy(array->content, CONTENT, CONTENT_SIZE); // copy without 0-terminator

    // string uses full array
    replaceStringField(array, 0, CONTENT_SIZE);
}

/**
 * Such method of literal initialization could be used if runtime wouldn't support out-of-heap objects. 
 */
void StrLiteralMock::produceLiteralSlow2() {
    obj_ref_t obj = mockInstance.cjNativeInterface.array_alloc(getUtf8ArrayTI(), CONTENT_SIZE);
    check(obj != nullptr, "allocation error");

    RawArray* array = static_cast<RawArray*>(obj);
    std::memcpy(array->content, CONTENT, CONTENT_SIZE); // copy without 0-terminator

    // string is half-size slice of array
    replaceStringField(array, CONTENT_SIZE / 2, CONTENT_SIZE / 2);
}

/**
 * Initialize string using out-of-heap object. 
 */
void StrLiteralMock::produceLiteralFast1() {
    replaceStringField((RawArray*) &literalsStorage, 0, CONTENT_SIZE / 2);
}

/**
 * Initialize string using out-of-heap object. 
 */
void StrLiteralMock::produceLiteralFast2() {
    replaceStringField((RawArray*) &literalsStorage, CONTENT_SIZE / 2, CONTENT_SIZE / 2);
}
