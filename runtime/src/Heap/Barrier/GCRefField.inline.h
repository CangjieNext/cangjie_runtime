// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_GC_REF_FIELD_INLINE_H
#define MRT_GC_REF_FIELD_INLINE_H

#include "ObjectModel/RefField.inline.h"
#ifdef MRT_USE_MARK_SWEEP
#include "Heap/MBarrier/MSRefField.inline.h"
#endif

namespace MapleRuntime {

// Common runtime code is built for both collectors. CopyGC keeps the tagged
// RefField interpretation, while MSGC stores and loads the complete pointer
// bit pattern because it has no forwarding tag in reference fields.
template<bool isAtomic>
inline BaseObject* LoadReferenceForGCMode(const RefField<isAtomic>& field,
                                          std::memory_order order = std::memory_order_relaxed)
{
#ifdef MRT_USE_MARK_SWEEP
    return MSLoadRawReference(field, order);
#else
    return field.GetTargetObject(order);
#endif
}

template<bool isAtomic>
inline void StoreReferenceForGCMode(RefField<isAtomic>& field, const BaseObject* ref,
                                    std::memory_order order = std::memory_order_relaxed)
{
#ifdef MRT_USE_MARK_SWEEP
    MSStoreRawReference(field, ref, order);
#else
    field.SetTargetObject(ref, order);
#endif
}

} // namespace MapleRuntime

#endif // MRT_GC_REF_FIELD_INLINE_H
