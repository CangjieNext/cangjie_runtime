#ifndef CANGJIE_UTILS_H
#define CANGJIE_UTILS_H

// Mirror of Cangjie RawArray<T>
struct RawArray {
    void*    typeInfo;
    uint64_t size;
    uint8_t  content[];
};

// Mirror of Cangjie struct Array<T>
struct CJArray {
    DYN_ObjRef rawptr;
    int64_t   start;
    int64_t   len;
};

struct CJString {
    RawArray* rawArray;
    uint32_t  start;
    uint32_t  len;
};

CJString* getEmptyString() {
    CJString* emptyString = static_cast<CJString*>(get_symbol_address("_CNat6String5emptyE"));
    assert(emptyString != nullptr);
    return emptyString;
}

#endif // CANGJIE_UTILS_H