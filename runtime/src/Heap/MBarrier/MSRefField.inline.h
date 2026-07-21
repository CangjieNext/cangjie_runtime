// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MS_REF_FIELD_INLINE_H
#define MRT_MS_REF_FIELD_INLINE_H

#ifdef MRT_USE_MARK_SWEEP

#include "ObjectModel/RefField.inline.h"

namespace MapleRuntime {

// MSGC never encodes CopyGC forwarding tags in a RefField. Keep the complete
// pointer bit pattern so runtime barriers and compiler-emitted fast paths have
// the same raw 64-bit reference representation.
template<bool isAtomic>
inline BaseObject* MSLoadRawReference(const RefField<isAtomic>& field,
                                      std::memory_order order = std::memory_order_relaxed)
{
    return reinterpret_cast<BaseObject*>(field.GetFieldValue(order));
}

template<bool isAtomic>
inline void MSStoreRawReference(RefField<isAtomic>& field, const BaseObject* ref,
                                std::memory_order order = std::memory_order_relaxed)
{
    field.SetFieldValue(reinterpret_cast<MAddress>(ref), order);
}

inline BaseObject* MSExchangeRawReference(RefField<true>& field, const BaseObject* ref,
                                          std::memory_order order = std::memory_order_relaxed)
{
    return reinterpret_cast<BaseObject*>(field.Exchange(reinterpret_cast<MAddress>(ref), order));
}

inline bool MSCompareExchangeRawReference(RefField<true>& field, const BaseObject* expectedRef,
                                          const BaseObject* newRef,
                                          std::memory_order succOrder = std::memory_order_relaxed,
                                          std::memory_order failOrder = std::memory_order_relaxed)
{
    return field.CompareExchange(reinterpret_cast<MAddress>(expectedRef),
                                 reinterpret_cast<MAddress>(newRef), succOrder, failOrder);
}

} // namespace MapleRuntime

#endif // MRT_USE_MARK_SWEEP
#endif // MRT_MS_REF_FIELD_INLINE_H
