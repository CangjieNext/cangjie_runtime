// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_MSGC_FAST_PATH_H
#define MRT_MSGC_FAST_PATH_H

#include <cstddef>
#include <cstdint>

#include "Cangjie.h"

namespace MapleRuntime {

#ifdef MRT_USE_MARK_SWEEP

// Stable C data ABI consumed by the AArch64 and X86-64 MSGC emitters. Fields
// may only be appended; existing offsets and meanings are compiler ABI.
struct MSGCFastPathState {
  uint8_t barrierPhase;
  uint8_t reserved[7];
  uintptr_t heapReservedStart;
  uintptr_t heapReservedEnd;
  uintptr_t regionInfoArray;
};

extern "C" MRT_EXPORT MSGCFastPathState CJ_MRT_MSGCFastPathState;

static_assert(sizeof(MSGCFastPathState) == 32,
              "compiler ABI: MSGCFastPathState size");
static_assert(offsetof(MSGCFastPathState, barrierPhase) == 0,
              "compiler ABI: MSGCFastPathState::barrierPhase");
static_assert(offsetof(MSGCFastPathState, heapReservedStart) == 8,
              "compiler ABI: MSGCFastPathState::heapReservedStart");
static_assert(offsetof(MSGCFastPathState, heapReservedEnd) == 16,
              "compiler ABI: MSGCFastPathState::heapReservedEnd");
static_assert(offsetof(MSGCFastPathState, regionInfoArray) == 24,
              "compiler ABI: MSGCFastPathState::regionInfoArray");

inline void PublishMSGCFastPathBarrierPhase(uint8_t phase) {
  __atomic_store_n(&CJ_MRT_MSGCFastPathState.barrierPhase, phase,
                   __ATOMIC_RELEASE);
}

inline void PublishMSGCFastPathHeapRange(uintptr_t start, uintptr_t end) {
  __atomic_store_n(&CJ_MRT_MSGCFastPathState.heapReservedStart, start,
                   __ATOMIC_RELEASE);
  __atomic_store_n(&CJ_MRT_MSGCFastPathState.heapReservedEnd, end,
                   __ATOMIC_RELEASE);
}

inline void PublishMSGCFastPathRegionInfoArray(uintptr_t array) {
  __atomic_store_n(&CJ_MRT_MSGCFastPathState.regionInfoArray, array,
                   __ATOMIC_RELEASE);
}

#endif // MRT_USE_MARK_SWEEP

} // namespace MapleRuntime

#endif // MRT_MSGC_FAST_PATH_H
