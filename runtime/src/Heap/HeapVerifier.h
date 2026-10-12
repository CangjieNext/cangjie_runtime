// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.

#ifndef MRT_HEAP_VERIFIER_H
#define MRT_HEAP_VERIFIER_H

#include <cstddef>

namespace MapleRuntime {

class __attribute__((visibility("hidden"))) HeapVerifier {
public:
    // cjEnableHeapVerify: 0 disables, 1 checks before GC, 2 checks before
    // and after GC. The default path does not scan the heap.
    static void Init();
    static bool IsEnabled();

    // Internal GC checkpoints, called while the collector owns ScopedSTWLock.
    // Verify allocated layouts and references reachable from roots, including
    // stack objects. Reuse an active STW or stop the world before scanning.
    // gcCycle is the completed GC count + 1, stable through both checkpoints.
    static bool VerifyAt(const char* checkpoint, size_t gcCycle);

private:
    static unsigned int ReadModeFromEnv();
    // Initialized during HeapImpl::Init before GC and Finalizer threads start;
    // the value is read-only for the remainder of the process lifetime.
    static unsigned int heapVerifyMode;
};

} // namespace MapleRuntime

#endif // MRT_HEAP_VERIFIER_H
