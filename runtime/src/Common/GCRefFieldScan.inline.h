// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GC_REF_FIELD_SCAN_INLINE_H
#define MRT_GC_REF_FIELD_SCAN_INLINE_H

#include <utility>

#include "ObjectModel/MArray.inline.h"

namespace MapleRuntime {

template<class Visitor>
inline void ForEachRefFieldInNonArrayObjectGCKnownType(BaseObject* obj, TypeInfo* typeInfo,
                                                       Visitor& visitor)
{
    MAddress objAddr = reinterpret_cast<MAddress>(obj) + TYPEINFO_PTR_SIZE;
    typeInfo->GetGCTib().ForEachBitmapWordFast(objAddr, visitor);
}

template<class Visitor>
inline void ForEachElementInArrayGCKnownType(BaseObject* obj, TypeInfo* typeInfo,
                                             Visitor& visitor)
{
    MArray* mArray = reinterpret_cast<MArray*>(obj);
    MIndex arrayLength = mArray->GetLength();
    TypeInfo* componentTypeInfo = typeInfo->GetComponentTypeInfo();
    if (componentTypeInfo->IsStructType()) {
        GCTib gcTib = componentTypeInfo->GetGCTib();
        MAddress contentAddr = reinterpret_cast<Uptr>(mArray) + MArray::GetContentOffset();
        MSize elementSize = componentTypeInfo->GetComponentSize();
        for (MIndex i = 0; i < arrayLength; ++i) {
            gcTib.ForEachBitmapWordFast(contentAddr, visitor);
            contentAddr += elementSize;
        }
    } else if (componentTypeInfo->IsObjectType() || componentTypeInfo->IsArrayType() ||
               componentTypeInfo->IsInterface()) {
        RefField<>* arrayContent = reinterpret_cast<RefField<>*>(mArray->ConvertToCArray());
        for (MIndex i = 0; i < arrayLength; ++i) {
            visitor(arrayContent[i]);
        }
    } else {
        LOG(RTLOG_FATAL, "array object %p has wrong component type", mArray);
    }
}

template<class Visitor>
inline void ForEachRefFieldGCKnownType(BaseObject* obj, TypeInfo* typeInfo,
                                       Visitor&& visitor)
{
    auto& scanVisitor = visitor;
    if (UNLIKELY(typeInfo->IsRawArray())) {
        ForEachElementInArrayGCKnownType(obj, typeInfo, scanVisitor);
        return;
    }
    ForEachRefFieldInNonArrayObjectGCKnownType(obj, typeInfo, scanVisitor);
}

template<class Visitor>
inline void ForEachRefFieldGCInline(BaseObject* obj, Visitor&& visitor)
{
    TypeInfo* typeInfo = obj->GetTypeInfo();
    if (!typeInfo->HasRefField()) {
        return;
    }
    ForEachRefFieldGCKnownType(obj, typeInfo, std::forward<Visitor>(visitor));
}

} // namespace MapleRuntime

#endif // MRT_GC_REF_FIELD_SCAN_INLINE_H
