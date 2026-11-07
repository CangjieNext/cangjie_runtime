// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.

#ifndef MRT_THREAD_CREATOR_INFO_H
#define MRT_THREAD_CREATOR_INFO_H

#include <cstddef>
#include <cstdint>

// Runtime-internal linkage: none of the entities below is part of the public ABI.
// They are hidden so that no new dynamic symbol is exported from the runtime library.
#if defined(_WIN32) || defined(_WIN64)
#define THREAD_CREATOR_HIDDEN
#else
#define THREAD_CREATOR_HIDDEN __attribute__((visibility("hidden")))
#endif

namespace MapleRuntime {

// Creator-frame marker, set in the ip slot of the creator triple appended by
// MCC_FillInStackTraceImpl. Encoding invariants (see the v2 design doc §4.1):
// N1 - real user-mode instruction addresses have this bit clear (aarch64 user VA < 2^48,
//      x86_64 canonical user addresses have bits 48-63 clear), and the ips stored in the
//      pcArray are the same clean values used for stack-map lookup, so ordinary frames
//      never trigger the creator branch;
// N2 - encoding/decoding is lossless: (ip | TAG) & ~TAG == ip;
// N3 - tag detection and stripping happen before any address use in the decode branch.
constexpr uint64_t kThreadCreatorTagBit = UINT64_C(1) << 62;

// Identity of the creator frame: the same three values an ordinary frame's pcArray
// triple carries (ip / funcStart / funcDesc), captured verbatim at spawn time on the
// parent thread, copied into the child cjthread's tail fields before publish, and
// copied again into the exception's pcArray at fill time. Pure values: no runtime-owned
// memory, no lifetime coupling with any thread; the funcDesc points into module memory
// and therefore shares the module-lifetime semantics of every ordinary frame triple.
struct CreatorFrameInfo {
    uintptr_t ip;         // call-site return address (the spawn call instruction); 0 = not captured
    uintptr_t funcStart;  // creator function start (stack-map lookup base)
    uint64_t funcDesc;    // creator frame FuncDesc pointer value
};

// Captures the creator frame on the parent thread at the MCC entries. Bounded unwind
// to the first MANAGED frame, then a verbatim copy of the frame's three identity
// values - the same values the ordinary fill path stores for every frame
// (StackInfo.cpp ExtractLiteFrameInfoFromStack). No decoding, no lookup, no residency
// table, no startup hook (the inherited StackInfo base constructor still performs its
// usual vector reserve - shared unwind headers are left untouched by design).
// Returns ip == 0 whenever capture is given up (interpreter caller, unreliable unwind
// context, no valid frame, or unresolved FuncDesc); retry happens on the next spawn.
THREAD_CREATOR_HIDDEN CreatorFrameInfo CaptureCreator();

// Returns the current cjthread's creator frame; ip == 0 when absent
// (main/CFFI threads, capture given up, or non-spawn creation paths).
THREAD_CREATOR_HIDDEN CreatorFrameInfo CJThreadGetCreatorFrame();

} // namespace MapleRuntime

#ifdef __cplusplus
extern "C" {
#endif

// Runtime-internal creation helper used only by the two MCC entries. Kept out of the
// public schedule.h. creatorFrame points to a parent-stack CreatorFrameInfo (or nullptr):
// the wrapper synchronously copies the three values into the child's tail fields before
// publish and never stores the pointer itself (the parent stack object dies on return).
THREAD_CREATOR_HIDDEN void *CJThreadNewWithCreator(void *schedule, const void *attrUser, void *func,
                                                   const void *argStart, unsigned int argSize,
                                                   unsigned int createSource, const void *creatorFrame);

#ifdef __cplusplus
}
#endif

#endif // MRT_THREAD_CREATOR_INFO_H
