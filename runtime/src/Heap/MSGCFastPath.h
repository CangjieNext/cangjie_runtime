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
  uint8_t barrierPhase;          // +0
  uint8_t explicitPad0[3];       // +1
  uint32_t currentMarkEpoch;     // +4
  uint64_t heapReservedStart;    // +8
  uint64_t heapReservedEnd;      // +16
  uint64_t regionInfoArray;      // +24
  uint64_t allocPlaneBase;       // +32
  uint64_t markPlaneBase;        // +40
  uint64_t scanPlaneBase;        // +48
  uint64_t reservedAbi;          // +56
};

extern "C" MRT_EXPORT MSGCFastPathState CJ_MRT_MSGCFastPathState;

static_assert(sizeof(uintptr_t) == sizeof(uint64_t),
              "MSGC only supports 64-bit targets");
static_assert(__atomic_always_lock_free(sizeof(uint32_t), nullptr),
              "MSGC requires lock-free 32-bit atomics");
static_assert(__atomic_always_lock_free(sizeof(uint64_t), nullptr),
              "MSGC requires lock-free 64-bit atomics");
static_assert(sizeof(MSGCFastPathState) == 64,
              "compiler ABI: MSGCFastPathState size");
static_assert(offsetof(MSGCFastPathState, barrierPhase) == 0,
              "compiler ABI: MSGCFastPathState::barrierPhase");
static_assert(offsetof(MSGCFastPathState, currentMarkEpoch) == 4,
              "compiler ABI: MSGCFastPathState::currentMarkEpoch");
static_assert(offsetof(MSGCFastPathState, heapReservedStart) == 8,
              "compiler ABI: MSGCFastPathState::heapReservedStart");
static_assert(offsetof(MSGCFastPathState, heapReservedEnd) == 16,
              "compiler ABI: MSGCFastPathState::heapReservedEnd");
static_assert(offsetof(MSGCFastPathState, regionInfoArray) == 24,
              "compiler ABI: MSGCFastPathState::regionInfoArray");
static_assert(offsetof(MSGCFastPathState, allocPlaneBase) == 32,
              "compiler ABI: MSGCFastPathState::allocPlaneBase");
static_assert(offsetof(MSGCFastPathState, markPlaneBase) == 40,
              "compiler ABI: MSGCFastPathState::markPlaneBase");
static_assert(offsetof(MSGCFastPathState, scanPlaneBase) == 48,
              "compiler ABI: MSGCFastPathState::scanPlaneBase");
static_assert(offsetof(MSGCFastPathState, reservedAbi) == 56,
              "compiler ABI: MSGCFastPathState::reservedAbi");

inline void PublishMSGCFastPathBarrierPhase(uint8_t phase) {
  __atomic_store_n(&CJ_MRT_MSGCFastPathState.barrierPhase, phase,
                   __ATOMIC_RELEASE);
}

inline void PublishMSGCFastPathMarkEpoch(uint32_t epoch) {
  __atomic_store_n(&CJ_MRT_MSGCFastPathState.currentMarkEpoch, epoch,
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

inline void PublishMSGCFastPathPlaneBases(uintptr_t allocBase,
                                          uintptr_t markBase,
                                          uintptr_t scanBase) {
  __atomic_store_n(&CJ_MRT_MSGCFastPathState.allocPlaneBase, allocBase,
                   __ATOMIC_RELEASE);
  __atomic_store_n(&CJ_MRT_MSGCFastPathState.markPlaneBase, markBase,
                   __ATOMIC_RELEASE);
  __atomic_store_n(&CJ_MRT_MSGCFastPathState.scanPlaneBase, scanBase,
                   __ATOMIC_RELEASE);
}

#endif // MRT_USE_MARK_SWEEP

} // namespace MapleRuntime

#endif // MRT_MSGC_FAST_PATH_H
