// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifdef MRT_USE_MARK_SWEEP

#include "Heap/MAllocator/MSScanClass.h"

#include <cstdlib>

#include "ObjectModel/MClass.inline.h"

namespace MapleRuntime {

MSScanClass ClassifyMSScanClass(const TypeInfo* typeInfo)
{
    if (typeInfo == nullptr) {
        std::abort();
    }

    if (typeInfo->IsWeakRefType() || typeInfo->HasFinalizer()) {
        return MSScanClass::HAS_REF;
    }

    if (typeInfo->IsRawArray() || typeInfo->IsVArray()) {
        const TypeInfo* componentTypeInfo = typeInfo->GetComponentTypeInfo();
        if (componentTypeInfo == nullptr) {
            std::abort();
        }
        return (componentTypeInfo->IsRef() || componentTypeInfo->HasRefField())
            ? MSScanClass::HAS_REF
            : MSScanClass::NO_REF;
    }

    return typeInfo->HasRefField() ? MSScanClass::HAS_REF : MSScanClass::NO_REF;
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
