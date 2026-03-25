#include "mock.h"
#include "log.h"
#include "utils.h"
#include "cangjieUtils.h"
#include <cstring>

extern MOCK_IMPLEMENTATION mockInstance;

#define CONTENT "HelloWorld"
#define CONTENT_SIZE (sizeof(CONTENT) - 1) // do not include 0-terminator

uint8_t literalsStorage[sizeof(RawArray) + CONTENT_SIZE];

StrLiteralMock::StrLiteralMock(INT_InterpreterInterface* interpreter_interf, DYN_CJNativeInterface* cjnative_interf)
    : Mock(interpreter_interf, cjnative_interf)
{
    registerC2I(0, reinterpret_cast<uintptr_t>(init));
    registerC2I(1, reinterpret_cast<uintptr_t>(produceLiteralSlow1));
    registerC2I(2, reinterpret_cast<uintptr_t>(produceLiteralSlow2));
    registerC2I(3, reinterpret_cast<uintptr_t>(produceLiteralFast1));
    registerC2I(4, reinterpret_cast<uintptr_t>(produceLiteralFast2));
}

struct DYN_TypeInfo* getUtf8ArrayTI() {
    struct DYN_TypeInfo* ti = mockInstance.cjNativeInterface.typeInfo("RawArray<UInt8>");
    check(ti != nullptr, "[Mock] couldn't find type info for RawArray<UInt8>");
    return ti;
}

void replaceStringField(RawArray* array, uint32_t start, uint32_t len) {
    log("[Mock] replace string field content {array = %p, start = %u, len = %u}", array, start, len);

    CJString tmp;
    tmp.rawArray = array;
    tmp.start    = start;
    tmp.len      = len;

    CJString* testFieldAddr = (CJString*) get_symbol_address("_CN4test9testFieldE");
    check(testFieldAddr != nullptr, "[Mock] couldn't replace field");

    DYN_GCTib tib = {};
    tib.raw = 0x8000000000000001ULL; // only first field of String is ref

    mockInstance.cjNativeInterface.writeStaticStructField(
        reinterpret_cast<uintptr_t>(testFieldAddr), sizeof(CJString),
        reinterpret_cast<uintptr_t>(&tmp),          sizeof(CJString),
        tib);

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
    DYN_ObjRef obj = mockInstance.cjNativeInterface.arrayAlloc(getUtf8ArrayTI(), CONTENT_SIZE);
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
    DYN_ObjRef obj = mockInstance.cjNativeInterface.arrayAlloc(getUtf8ArrayTI(), CONTENT_SIZE);
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
