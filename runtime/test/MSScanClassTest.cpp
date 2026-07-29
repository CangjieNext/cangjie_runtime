#include <cassert>
#include <cstdint>
#include <cstring>

#include "src/Heap/MAllocator/MSScanClass.h"
#include "src/ObjectModel/MClass.inline.h"

namespace {

using MapleRuntime::ClassifyMSScanClass;
using MapleRuntime::MSScanClass;
using MapleRuntime::TypeInfo;

class TestTypeInfo {
public:
    TestTypeInfo(MapleRuntime::TypeKind kind, uint8_t flags = 0)
    {
        std::memset(storage_, 0, sizeof(storage_));
        Get()->SetType(static_cast<MapleRuntime::I8>(kind));
        Get()->SetFlag(static_cast<MapleRuntime::I8>(flags));
    }

    TypeInfo* Get()
    {
        return reinterpret_cast<TypeInfo*>(storage_);
    }

private:
    alignas(TypeInfo) uint8_t storage_[sizeof(TypeInfo)];
};

void TestOrdinaryTypes()
{
    TestTypeInfo noRef(MapleRuntime::TypeKind::TYPE_KIND_STRUCT);
    TestTypeInfo hasRef(
        MapleRuntime::TypeKind::TYPE_KIND_STRUCT, MapleRuntime::FLAG_HAS_REF_FIELD);
    assert(ClassifyMSScanClass(noRef.Get()) == MSScanClass::NO_REF);
    assert(ClassifyMSScanClass(hasRef.Get()) == MSScanClass::HAS_REF);
}

void TestArrayTypes()
{
    TestTypeInfo primitive(MapleRuntime::TypeKind::TYPE_KIND_INT64);
    TestTypeInfo ref(MapleRuntime::TypeKind::TYPE_KIND_CLASS);
    TestTypeInfo structWithRef(
        MapleRuntime::TypeKind::TYPE_KIND_STRUCT, MapleRuntime::FLAG_HAS_REF_FIELD);
    TestTypeInfo rawArray(MapleRuntime::TypeKind::TYPE_KIND_RAWARRAY);
    TestTypeInfo vArray(MapleRuntime::TypeKind::TYPE_KIND_VARRAY);

    rawArray.Get()->SetComponentTypeInfo(primitive.Get());
    assert(ClassifyMSScanClass(rawArray.Get()) == MSScanClass::NO_REF);

    rawArray.Get()->SetComponentTypeInfo(ref.Get());
    assert(ClassifyMSScanClass(rawArray.Get()) == MSScanClass::HAS_REF);

    vArray.Get()->SetComponentTypeInfo(structWithRef.Get());
    assert(ClassifyMSScanClass(vArray.Get()) == MSScanClass::HAS_REF);
}

void TestSpecialTypes()
{
    TestTypeInfo weakRef(MapleRuntime::TypeKind::TYPE_KIND_WEAKREF_CLASS);
    TestTypeInfo finalizer(
        MapleRuntime::TypeKind::TYPE_KIND_CLASS, MapleRuntime::FLAG_HAS_FINALIZER);
    assert(ClassifyMSScanClass(weakRef.Get()) == MSScanClass::HAS_REF);
    assert(ClassifyMSScanClass(finalizer.Get()) == MSScanClass::HAS_REF);
}

} // namespace

int main()
{
    TestOrdinaryTypes();
    TestArrayTypes();
    TestSpecialTypes();
    return 0;
}
